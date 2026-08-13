/* ============================================================================
 * boot_payload.c -- validator for the typed payload descriptor array.
 *
 * Called from boot_hw.c after the header copy + pre-copy validation are
 * complete and g_boot_info is populated. Runs every policy documented
 * in the struct boot_payload_desc contract:
 *
 *   1. payload_count must be in [0, BOOT_PAYLOAD_MAX].
 *   2. Every occupied slot at index >= payload_count is rejected
 *      (packed-prefix invariant) so consumers scanning the full array
 *      cannot be tricked into processing a payload the validator did
 *      not see.
 *   3. For every occupied slot at index < payload_count:
 *        - phys_start + length must not overflow uint64_t.
 *        - alignment (if non-zero) must be a power of 2.
 *        - The descriptor's range (phys_start, length bytes) must
 *          not overlap: the struct boot_info handoff region, any UEFI
 *          runtime memory map entry, any populated entry in
 *          usb_controller.dma_pages[], or the linear framebuffer.
 *        - Unknown flag bits are rejected only when
 *          BOOT_PAYLOAD_FLAG_REQUIRED is set -- otherwise older
 *          kernels can boot against newer bootloaders that introduce
 *          new bits, per the forward-compatibility contract.
 *        - Unknown type enums are ACCEPTED (skipped for type-specific
 *          validation) unless BOOT_PAYLOAD_FLAG_REQUIRED is set, in
 *          which case boot aborts.
 *   4. payload_total_bytes must equal the recomputed sum of length for
 *      every occupied slot in the packed-prefix range.
 *
 * On BOOT_FATAL a LOG_ERROR entry names the offending descriptor index
 * and failure class; *out_error (when non-NULL) is populated with the
 * specific enum boot_payload_error so tests can assert failure classes
 * without parsing log text.
 *
 * This is a PURE function: no side effects beyond klog output. It is
 * safe to call from test fixtures with synthetic struct boot_info
 * buffers (BOOT_PRODUCER_KERNEL_TEST).
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "kernel/klog.h"

/* BOOT_INFO_PHYS_ADDR is the physical base of the kernel-side struct
 * boot_info copy, single-sourced via kernel/boot_info.h (which re-exports
 * it from the UEFI-safe kernel/boot_version_constants.h the bootloader
 * also compiles against). Both that region and the bootloader's own are
 * retained until Phase 3 completes and must never be overlapped by any
 * typed payload. */

/* Linker-provided kernel image bounds -- the kernel ELF ends up loaded
 * at __kernel_start through __kernel_end (end exclusive), and no typed
 * payload may land anywhere inside that range. Symbols come from
 * src/boot/linker.ld. */
extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

static int is_power_of_two_u64(uint64_t x)
{
    if (x == 0u)
        return 0;
    return (x & (x - 1u)) == 0u;
}

/* Returns 1 on overlap, 0 on disjoint. Both ranges must be pre-checked
 * for end-address overflow (start + len <= UINT64_MAX); the helper asserts
 * that in debug builds but trusts its callers to use the
 * range_end_overflows() guard below before calling. */
static int ranges_overlap(uint64_t a_start, uint64_t a_len,
                          uint64_t b_start, uint64_t b_len)
{
    uint64_t a_end;
    uint64_t b_end;

    if (a_len == 0u || b_len == 0u)
        return 0;

    /* a_end / b_end are exclusive (phys_start + length). The caller
     * guarantees a_len / b_len are already overflow-checked before
     * reaching this helper. */
    a_end = a_start + a_len;
    b_end = b_start + b_len;

    return a_start < b_end && b_start < a_end;
}

/* Returns 1 when (start + len) would wrap past UINT64_MAX. Used to
 * refuse any retained-region description that cannot be expressed as a
 * well-formed half-open interval -- otherwise the ranges_overlap()
 * below can produce a false "disjoint" result for payload bytes that
 * really do land inside the wrapped region. */
static int range_end_overflows(uint64_t start, uint64_t len)
{
    if (len == 0u)
        return 0;
    return len > (uint64_t)-1 - start;
}

/* A valid empty slot is type=NONE with every other field zero. Any
 * deviation is ABI drift: a bootloader bug setting random bytes in an
 * unused slot, or a future field being populated without a matching
 * type enum assignment. Either way the validator must reject it so
 * the invariant "NONE means empty" stays intact for type-keyed
 * consumers that scan the full array. */
static int descriptor_is_empty_slot(const struct boot_payload_desc *d)
{
    return d->type == (uint32_t)BOOT_PAYLOAD_NONE &&
           d->flags == 0u &&
           d->phys_start == 0u &&
           d->length == 0u &&
           d->alignment == 0u &&
           d->checksum == 0u &&
           d->producer_id == 0u &&
           d->_reserved == 0u;
}

static int type_is_known(uint32_t type)
{
    switch (type) {
    case (uint32_t)BOOT_PAYLOAD_NONE:
    case (uint32_t)BOOT_PAYLOAD_MODULE:
    case (uint32_t)BOOT_PAYLOAD_INITRD:
    case (uint32_t)BOOT_PAYLOAD_RECOVERY_IMAGE:
    case (uint32_t)BOOT_PAYLOAD_HIBERNATION_META:
    case (uint32_t)BOOT_PAYLOAD_TPM_EVENT_LOG:
    case (uint32_t)BOOT_PAYLOAD_NETWORK_CONFIG:
    case (uint32_t)BOOT_PAYLOAD_RANDOM_SEED:
    case (uint32_t)BOOT_PAYLOAD_USB_HANDOVER:
    case (uint32_t)BOOT_PAYLOAD_WARM_UPDATE_STATE:
        return 1;
    default:
        return 0;
    }
}

/* Overlap-check one payload against every retained boot region.
 * Returns BOOT_PAYLOAD_ERR_OK when the payload is disjoint, otherwise
 * the specific overlap-class error code. info is known-non-NULL and
 * (start, len) are known-non-zero + already overflow-checked.
 *
 * Every retained-region end address is itself overflow-checked BEFORE
 * ranges_overlap() is called. A malformed bootloader describing a
 * region near UINT64_MAX whose end wraps past zero is rejected as an
 * overlap in its own class -- otherwise the wrapped interval could
 * falsely report a payload as disjoint. */
static enum boot_payload_error payload_overlap_check(
    const struct boot_info *info,
    uint64_t start,
    uint64_t len)
{
    uint32_t i;
    uintptr_t kernel_start_addr = (uintptr_t)&__kernel_start;
    uintptr_t kernel_end_addr   = (uintptr_t)&__kernel_end;

    /* 1a. The kernel's copied struct boot_info at BOOT_INFO_PHYS_ADDR.
     *     Fixed range at a known address -- no wrap possible, pin stays
     *     a lookup of compile-time constants. */
    if (ranges_overlap(start, len,
                       BOOT_INFO_PHYS_ADDR,
                       (uint64_t)sizeof(struct boot_info))) {
        return BOOT_PAYLOAD_ERR_OVERLAP_BOOT_INFO;
    }

    /* 1b. Kernel image range from __kernel_start to __kernel_end (end
     *     exclusive; matched by the kernel_end_addr > kernel_start_addr
     *     gate below so the computed length is always positive). Linker
     *     guarantees end > start so no overflow check needed, but the
     *     end-overflow gate below protects against a toolchain bug
     *     placing the image at a wrapping physical address. */
    if (kernel_end_addr > kernel_start_addr) {
        uint64_t k_start = (uint64_t)kernel_start_addr;
        uint64_t k_len   = (uint64_t)(kernel_end_addr - kernel_start_addr);
        if (range_end_overflows(k_start, k_len))
            return BOOT_PAYLOAD_ERR_OVERLAP_BOOT_INFO;
        if (ranges_overlap(start, len, k_start, k_len))
            return BOOT_PAYLOAD_ERR_OVERLAP_BOOT_INFO;
    }

    /* 2. UEFI runtime memory regions. rt_mmap_count slots are
     *    guaranteed populated; remaining slots are zeroed. */
    if (info->rt_mmap_count > BOOT_RT_MMAP_MAX)
        return BOOT_PAYLOAD_ERR_OVERLAP_RT_MMAP;
    for (i = 0u; i < info->rt_mmap_count; i++) {
        const struct boot_rt_mem_entry *rt = &info->rt_mmap[i];
        uint64_t rt_start = rt->phys_addr;
        /* num_pages is in 4 KiB units; cap the multiplication so a
         * malformed bootloader count cannot wrap rt_len. */
        uint64_t rt_len;
        if (rt->num_pages > ((uint64_t)-1 / 4096ull))
            return BOOT_PAYLOAD_ERR_OVERLAP_RT_MMAP;
        rt_len = rt->num_pages * 4096ull;
        if (rt_len == 0u)
            continue;
        /* Reject any retained-region description whose half-open end
         * would wrap past UINT64_MAX -- otherwise ranges_overlap could
         * report a payload inside the wrapped interval as disjoint. */
        if (range_end_overflows(rt_start, rt_len))
            return BOOT_PAYLOAD_ERR_OVERLAP_RT_MMAP;
        if (ranges_overlap(start, len, rt_start, rt_len))
            return BOOT_PAYLOAD_ERR_OVERLAP_RT_MMAP;
    }

    /* 3. USB xHCI DMA pages. dma_page_count caps the populated prefix;
     *    dma_pages[] are 4 KiB pages keyed by physical base. */
    if (info->usb_controller.dma_page_count > BOOT_USB_MAX_DMA_PAGES)
        return BOOT_PAYLOAD_ERR_OVERLAP_USB_DMA;
    for (i = 0u; i < info->usb_controller.dma_page_count; i++) {
        uint64_t dma_start = info->usb_controller.dma_pages[i];
        if (dma_start == 0u)
            continue;
        if (range_end_overflows(dma_start, 4096ull))
            return BOOT_PAYLOAD_ERR_OVERLAP_USB_DMA;
        if (ranges_overlap(start, len, dma_start, 4096ull))
            return BOOT_PAYLOAD_ERR_OVERLAP_USB_DMA;
    }

    /* 4. Linear framebuffer. Range is pitch * height bytes starting at
     *    fb.addr. Overlap only applies when the framebuffer is live. */
    if (info->fb_available && info->fb.addr != 0u &&
        info->fb.pitch != 0u && info->fb.height != 0u) {
        uint64_t fb_start = (uint64_t)info->fb.addr;
        uint64_t fb_len;
        uint64_t pitch = (uint64_t)info->fb.pitch;
        uint64_t height = (uint64_t)info->fb.height;
        if (height != 0u && pitch > ((uint64_t)-1 / height))
            return BOOT_PAYLOAD_ERR_OVERLAP_FB;
        fb_len = pitch * height;
        if (range_end_overflows(fb_start, fb_len))
            return BOOT_PAYLOAD_ERR_OVERLAP_FB;
        if (ranges_overlap(start, len, fb_start, fb_len))
            return BOOT_PAYLOAD_ERR_OVERLAP_FB;
    }

    return BOOT_PAYLOAD_ERR_OK;
}

/* Kept separate so tests can hook a version-neutral klog mock later
 * without touching the validator's control flow. */
static void payload_log_failure(uint32_t index,
                                enum boot_payload_error code,
                                const struct boot_payload_desc *d)
{
    /* Keep the message short enough to survive any klog buffer limit.
     * phys_start / length are printed in hex so the operator can match
     * against boot-path memory-map logs. */
    klog(LOG_ERROR, "boot",
         "boot_payload: descriptor[%u] rejected (err=%u) type=%u flags=0x%08X "
         "start=0x%lx length=0x%lx",
         (uint64_t)index,
         (uint64_t)code,
         d ? (uint64_t)d->type : (uint64_t)0,
         d ? (uint64_t)d->flags : (uint64_t)0,
         d ? (uint64_t)d->phys_start : (uint64_t)0,
         d ? (uint64_t)d->length : (uint64_t)0);
}

boot_result_t boot_payload_validate(const struct boot_info *info,
                                    enum boot_payload_error *out_error)
{
    uint32_t i;
    uint64_t total = 0u;

    if (out_error != (enum boot_payload_error *)0)
        *out_error = BOOT_PAYLOAD_ERR_OK;

    if (info == (const struct boot_info *)0) {
        /* Caller bug; nothing safe to log against. */
        if (out_error != (enum boot_payload_error *)0)
            *out_error = BOOT_PAYLOAD_ERR_COUNT_OOR;
        return BOOT_FATAL;
    }

    /* count bound. Rejected before any slot iteration so we do not
     * leak per-slot error classes on a malformed count. */
    if (info->payload_count > BOOT_PAYLOAD_MAX) {
        klog(LOG_ERROR, "boot",
             "boot_payload: payload_count=%u exceeds BOOT_PAYLOAD_MAX=%u",
             (uint64_t)info->payload_count,
             (uint64_t)BOOT_PAYLOAD_MAX);
        if (out_error != (enum boot_payload_error *)0)
            *out_error = BOOT_PAYLOAD_ERR_COUNT_OOR;
        return BOOT_FATAL;
    }

    /* payload_overflow is the producer's explicit "I had more payloads
     * than I could fit in BOOT_PAYLOAD_MAX slots" signal. Accepting a
     * truncated handoff lets a future security-sensitive payload
     * (random seed, TPM log, hibernation metadata, recovery image) get
     * silently dropped while consumers still think they processed the
     * full set. refuses the handoff outright; a degraded policy
     * ("warn but continue for optional payloads only") is
     * capability-negotiation territory, not this ABI's contract. */
    if (info->payload_overflow != 0u) {
        klog(LOG_ERROR, "boot",
             "boot_payload: payload_overflow=%u -- producer truncated the "
             "payload array; refusing to boot with incomplete payload set",
             (uint64_t)info->payload_overflow);
        if (out_error != (enum boot_payload_error *)0)
            *out_error = BOOT_PAYLOAD_ERR_OVERFLOW_TRUNCATED;
        return BOOT_FATAL;
    }

    /* packed-prefix + NONE-emptiness: scan the FULL array.
     *
     * The ABI contract is now two-part:
     *   (a) Every type=BOOT_PAYLOAD_NONE slot MUST be fully zero --
     *       anywhere in the array, including past payload_count. A
     *       NONE slot with residual flags / phys_start / length /
     *       alignment is ABI drift and gets rejected so type-keyed
     *       consumers cannot be tricked into skipping a slot that
     *       actually carries metadata.
     *   (b) Every type != NONE slot MUST be inside the packed prefix
     *       (i < payload_count). An occupied slot past payload_count
     *       is a rejected invariant violation -- this closes the
     *       footgun where consumers scan the full array while the
     *       validator only walked payload_count. */
    for (i = 0u; i < BOOT_PAYLOAD_MAX; i++) {
        const struct boot_payload_desc *d = &info->payload_descriptors[i];

        if (d->type == (uint32_t)BOOT_PAYLOAD_NONE) {
            /* NONE slot: must be FULLY zero regardless of position. */
            if (!descriptor_is_empty_slot(d)) {
                payload_log_failure(i, BOOT_PAYLOAD_ERR_NONE_NOT_EMPTY, d);
                if (out_error != (enum boot_payload_error *)0)
                    *out_error = BOOT_PAYLOAD_ERR_NONE_NOT_EMPTY;
                return BOOT_FATAL;
            }
            continue;
        }

        /* type != NONE: must be inside the packed prefix. */
        if (i >= info->payload_count) {
            payload_log_failure(i, BOOT_PAYLOAD_ERR_PREFIX_VIOLATED, d);
            if (out_error != (enum boot_payload_error *)0)
                *out_error = BOOT_PAYLOAD_ERR_PREFIX_VIOLATED;
            return BOOT_FATAL;
        }

        /* a0 missing FLAG_VALID. The ABI documents
         * BOOT_PAYLOAD_FLAG_VALID as the explicit producer/consumer gate
         * meaning "this slot carries a real payload to process". type !=
         * NONE alone is not enough: an alternate or future producer that
         * forgets the flag would still have its descriptor consumed by
         * boot_payload_find, the PMM reservation pass, and type-keyed
         * downstream paths, defeating the flag's purpose and creating
         * producer-consumer drift. Reject occupied descriptors that
         * omit it so the gate is load-bearing rather than informational.
         *
         * Warm-update exemption: the warm-kernel-update contract is
         * intentionally fail-soft -- a warm-update descriptor that omits
         * BOTH FLAG_VALID and FLAG_RESERVED falls back to cold init via
         * boot_warm_update_consume's COLD_FALLBACK path rather than
         * halting boot. The exemption is narrow: a descriptor with
         * FLAG_RESERVED but missing FLAG_VALID is MALFORMED, not a
         * cold-fallback case -- the producer would be telling PMM to
         * reserve a range while telling consumers to ignore it, which
         * would leak physical memory on a cold boot. Reject that shape
         * with the strict path so PMM never reserves an "ignored"
         * range. Only the truly empty (no validity, no reservation)
         * shape gets the cold-fallback exemption. */
        {
            /* Cold-fallback exemption is the EXACT no-flags shape:
             * d->flags == 0. Any other bit (CHECKSUMMED, REQUIRED, an
             * unknown future bit) without FLAG_VALID is producer drift,
             * not a documented fail-soft case, and gets the strict
             * reject. */
            int is_warm_update_cold_fallback =
                (d->type == (uint32_t)BOOT_PAYLOAD_WARM_UPDATE_STATE) &&
                (d->flags == 0u);
            if (!is_warm_update_cold_fallback &&
                (d->flags & BOOT_PAYLOAD_FLAG_VALID) == 0u) {
                payload_log_failure(i, BOOT_PAYLOAD_ERR_MISSING_VALID_FLAG, d);
                if (out_error != (enum boot_payload_error *)0)
                    *out_error = BOOT_PAYLOAD_ERR_MISSING_VALID_FLAG;
                return BOOT_FATAL;
            }
        }

        /* a range overflow. phys_start + length must fit uint64_t. */
        if (d->length > (uint64_t)-1 - d->phys_start) {
            payload_log_failure(i, BOOT_PAYLOAD_ERR_RANGE_WRAP, d);
            if (out_error != (enum boot_payload_error *)0)
                *out_error = BOOT_PAYLOAD_ERR_RANGE_WRAP;
            return BOOT_FATAL;
        }

        /* b alignment. 0 = "no requirement"; any other value must be
         * both a power of 2 AND honored by phys_start. Without the
         * phys_start check the ABI promise "alignment describes this
         * payload's natural alignment" becomes un-enforceable -- a
         * bootloader could certify alignment=4096 for an unaligned
         * address and downstream consumers (module loader, initrd
         * mount, DMA-setup helpers) would hit the fault instead. */
        if (d->alignment != 0u) {
            if (!is_power_of_two_u64(d->alignment)) {
                payload_log_failure(i, BOOT_PAYLOAD_ERR_ALIGNMENT, d);
                if (out_error != (enum boot_payload_error *)0)
                    *out_error = BOOT_PAYLOAD_ERR_ALIGNMENT;
                return BOOT_FATAL;
            }
            if ((d->phys_start & (d->alignment - 1u)) != 0u) {
                payload_log_failure(i, BOOT_PAYLOAD_ERR_ALIGNMENT, d);
                if (out_error != (enum boot_payload_error *)0)
                    *out_error = BOOT_PAYLOAD_ERR_ALIGNMENT;
                return BOOT_FATAL;
            }
        }

        /* c generic overlap. Every occupied descriptor is checked
         * against every retained region regardless of type -- unknown
         * or future types still cannot stomp boot_info, runtime
         * memory, USB DMA, or the framebuffer. */
        {
            enum boot_payload_error ov =
                payload_overlap_check(info, d->phys_start, d->length);
            if (ov != BOOT_PAYLOAD_ERR_OK) {
                payload_log_failure(i, ov, d);
                if (out_error != (enum boot_payload_error *)0)
                    *out_error = ov;
                return BOOT_FATAL;
            }
        }

        /* c2 pairwise descriptor overlap. payload_overlap_check above
         * compares each descriptor against retained boot regions only;
         * it does NOT compare two payload descriptors against each
         * other. Without this loop, a malformed handoff that publishes
         * MODULE@X and INITRD@Y with overlapping ranges would pass
         * validation as long as neither hits boot_info / rt_mmap / USB
         * DMA / fb, payload_total_bytes would double-count the overlap,
         * and downstream consumers would be handed aliased payloads.
         * O(N) per outer iter; total O(N^2/2) = 496 compares max over
         * BOOT_PAYLOAD_MAX=32. Zero-length descriptors cannot overlap
         * by construction (ranges_overlap rejects len==0); skip
         * empty-slot peers defensively. */
        {
            uint32_t j;
            for (j = 0u; j < i; j++) {
                const struct boot_payload_desc *e =
                    &info->payload_descriptors[j];
                if (descriptor_is_empty_slot(e))
                    continue;
                if (ranges_overlap(d->phys_start, d->length,
                                   e->phys_start, e->length)) {
                    payload_log_failure(
                        i, BOOT_PAYLOAD_ERR_DESCRIPTOR_OVERLAP, d);
                    klog(LOG_ERROR, "boot",
                         "boot_payload: descriptor #%u "
                         "[%lx+%lx) overlaps descriptor #%u "
                         "[%lx+%lx)",
                         (uint64_t)i,
                         (uint64_t)d->phys_start,
                         (uint64_t)d->length,
                         (uint64_t)j,
                         (uint64_t)e->phys_start,
                         (uint64_t)e->length);
                    if (out_error != (enum boot_payload_error *)0)
                        *out_error = BOOT_PAYLOAD_ERR_DESCRIPTOR_OVERLAP;
                    return BOOT_FATAL;
                }
            }
        }

        /* d unknown-type / unknown-flags policy. The REQUIRED flag
         * forces strict checking for forward-compatibility control:
         * a newer bootloader marking a payload REQUIRED tells this
         * kernel "fail boot rather than silently ignoring me".
         *
         * Type-aware unknown-flags mask: warm-update descriptors carry
         * continuation bits in positions 8..13 above the standard
         * BOOT_PAYLOAD_FLAG_* family at positions 0..3. For a REQUIRED
         * BOOT_PAYLOAD_WARM_UPDATE_STATE descriptor the legal mask is
         * BOOT_PAYLOAD_FLAG_MASK_KNOWN | BOOT_WARM_UPDATE_CONT_MASK_KNOWN.
         * Without this, a producer that published a REQUIRED warm-update
         * descriptor with a known continuation bit fataled here before
         * the warm-update fail-closed COLD_FALLBACK path could run
         * (Codex 2026-04-30 H2 finding). */
        if (d->flags & BOOT_PAYLOAD_FLAG_REQUIRED) {
            if (!type_is_known(d->type)) {
                payload_log_failure(i, BOOT_PAYLOAD_ERR_UNKNOWN_REQUIRED, d);
                if (out_error != (enum boot_payload_error *)0)
                    *out_error = BOOT_PAYLOAD_ERR_UNKNOWN_REQUIRED;
                return BOOT_FATAL;
            }
            uint32_t legal_flag_mask = (uint32_t)BOOT_PAYLOAD_FLAG_MASK_KNOWN;
            if (d->type == (uint32_t)BOOT_PAYLOAD_WARM_UPDATE_STATE) {
                legal_flag_mask |= (uint32_t)BOOT_WARM_UPDATE_CONT_MASK_KNOWN;
            }
            if ((d->flags & ~legal_flag_mask) != 0u) {
                payload_log_failure(i, BOOT_PAYLOAD_ERR_UNKNOWN_FLAGS, d);
                if (out_error != (enum boot_payload_error *)0)
                    *out_error = BOOT_PAYLOAD_ERR_UNKNOWN_FLAGS;
                return BOOT_FATAL;
            }
        }
        /* Optional unknown types / flag bits silently skip
         * type-specific validation for forward compatibility. The
         * consumer (the future boot_payload_find query helper) sees the raw
         * type and chooses to ignore it. */

        /* Aggregate-size wrap guard. Each individual descriptor is
         * already range-checked (phys_start + length fits u64), but the
         * SUM across descriptors can still wrap. Without this check, a
         * malformed producer could set a matching wrapped
         * payload_total_bytes and pass the comparison below. */
        if (d->length > ((uint64_t)-1) - total) {
            payload_log_failure(i, BOOT_PAYLOAD_ERR_TOTAL_MISMATCH, d);
            if (out_error != (enum boot_payload_error *)0)
                *out_error = BOOT_PAYLOAD_ERR_TOTAL_MISMATCH;
            return BOOT_FATAL;
        }
        total += d->length;
    }

    /* recompute total. Reject a producer that reports a bogus sum. */
    if (info->payload_total_bytes != total) {
        klog(LOG_ERROR, "boot",
             "boot_payload: payload_total_bytes=%lu disagrees with "
             "recomputed sum %lu across %u slot(s)",
             (uint64_t)info->payload_total_bytes,
             (uint64_t)total,
             (uint64_t)info->payload_count);
        if (out_error != (enum boot_payload_error *)0)
            *out_error = BOOT_PAYLOAD_ERR_TOTAL_MISMATCH;
        return BOOT_FATAL;
    }

    return BOOT_OK;
}

const struct boot_payload_desc *
boot_payload_find(const struct boot_info *info,
                  uint32_t type,
                  uint32_t index)
{
    if (info == (const struct boot_info *)0)
        return (const struct boot_payload_desc *)0;
    /* NONE is the empty-slot sentinel; boot_payload_validate rejects any
     * NONE descriptor carrying data, so looking it up would always return
     * NULL. Refusing the type at the API level makes intent explicit and
     * shortens the hot path. */
    if (type == (uint32_t)BOOT_PAYLOAD_NONE)
        return (const struct boot_payload_desc *)0;
    /* The validator must have cleared this before any consumer runs, but
     * guard so a caller that skipped validation does not walk past the
     * fixed array bound. */
    if (info->payload_count > BOOT_PAYLOAD_MAX)
        return (const struct boot_payload_desc *)0;

    uint32_t seen = 0u;
    uint32_t i;
    for (i = 0u; i < info->payload_count; i++) {
        const struct boot_payload_desc *d = &info->payload_descriptors[i];
        if (d->type != type)
            continue;
        /* FLAG_VALID is load-bearing consume state: the validator rejects
         * non-VALID descriptors at Phase 0, so the only way the bit is
         * clear here is a consumer (e.g. the seed payload path) retiring
         * the descriptor after wiping/freeing the range. A retired
         * descriptor must never be rediscovered. */
        if ((d->flags & BOOT_PAYLOAD_FLAG_VALID) == 0u)
            continue;
        if (seen == index)
            return d;
        seen++;
    }
    return (const struct boot_payload_desc *)0;
}
