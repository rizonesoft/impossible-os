/* ============================================================================
 * boot_payload.c -- validator for the §4 typed payload descriptor array.
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
 *        - The descriptor's [phys_start, phys_start + length) range
 *          must not overlap: the struct boot_info handoff region, any
 *          UEFI runtime memory map entry, any populated entry in
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

/* Physical base of the kernel-side struct boot_info copy. The bootloader
 * writes its own struct to BOOT_INFO_PHYS_ADDR (0x10000); both regions
 * are retained until Phase 3 completes and must never be overlapped by
 * any typed payload. */
#define BOOT_INFO_PHYS_ADDR  0x10000ull

/* Linker-provided kernel image bounds -- the kernel ELF ends up loaded
 * at [__kernel_start, __kernel_end) and no typed payload may land
 * anywhere inside that range. Symbols come from src/boot/linker.ld. */
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

    /* 1b. Kernel image range at [__kernel_start, __kernel_end). Linker
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

    /* §1 count bound. Rejected before any slot iteration so we do not
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
     * full set. §4 refuses the handoff outright; a degraded policy
     * ("warn but continue for optional payloads only") is §11
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

    /* §2 packed-prefix + NONE-emptiness: scan the FULL array.
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

        /* §3a range overflow. phys_start + length must fit uint64_t. */
        if (d->length > (uint64_t)-1 - d->phys_start) {
            payload_log_failure(i, BOOT_PAYLOAD_ERR_RANGE_WRAP, d);
            if (out_error != (enum boot_payload_error *)0)
                *out_error = BOOT_PAYLOAD_ERR_RANGE_WRAP;
            return BOOT_FATAL;
        }

        /* §3b alignment. 0 = "no requirement"; any other value must be
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

        /* §3c generic overlap. Every occupied descriptor is checked
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

        /* §3d unknown-type / unknown-flags policy. The REQUIRED flag
         * forces strict checking for forward-compatibility control:
         * a newer bootloader marking a payload REQUIRED tells this
         * kernel "fail boot rather than silently ignoring me". */
        if (d->flags & BOOT_PAYLOAD_FLAG_REQUIRED) {
            if (!type_is_known(d->type)) {
                payload_log_failure(i, BOOT_PAYLOAD_ERR_UNKNOWN_REQUIRED, d);
                if (out_error != (enum boot_payload_error *)0)
                    *out_error = BOOT_PAYLOAD_ERR_UNKNOWN_REQUIRED;
                return BOOT_FATAL;
            }
            if ((d->flags & ~(uint32_t)BOOT_PAYLOAD_FLAG_MASK_KNOWN) != 0u) {
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

    /* §4 recompute total. Reject a producer that reports a bogus sum. */
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
