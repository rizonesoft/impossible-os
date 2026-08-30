/* ============================================================================
 * Single authoritative table of boot_info-derived physical regions
 * that PMM must NOT hand back to the free-page pool after boot.
 * Populated from boot_info in pmm_init, applied to the PMM bitmap via
 * pmm_mark_region_used(), logged at LOG_INFO for post-boot audit, and
 * mirrored to X:\Diag\boot-reserved.json via BlackBox once the disk
 * writer is alive.
 *
 * Scope boundary: this module covers struct boot_info, USB DMA pages
 * + scratchpad, TPM event log, framebuffer, every UEFI runtime memory
 * region, and every payload descriptor with BOOT_PAYLOAD_FLAG_RESERVED.
 * PMM-internal reservations (first 1 MiB, kernel image, bitmap, user
 * ELF) stay as direct `pmm_mark_region_used()` calls inside pmm_init --
 * they are not boot_info-driven and the handoff audit does not need
 * to re-describe the kernel's own layout.
 *
 * Overlap between any two entries is a build-time boot_fatal: a real
 * overlap means the bootloader and kernel disagree on who owns the
 * range, and that ambiguity historically caused USB DMA pages to be
 * reclaimed by unrelated allocators. Overlap detection runs O(N)
 * against the existing entries on every add; N <= 128 and the cost is
 * negligible at boot.
 * ============================================================================ */

#include "kernel/mm/boot_reserved.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/heap.h"
#include "kernel/boot_info.h"
#include "boot/boot_payload_limits.h" /* per-type contract + aggregate budget + repeatability */
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "libc/string.h"

/* Bootloader writes struct boot_info at BOOT_INFO_PHYS_ADDR, single-sourced
 * via kernel/boot_info.h (which re-exports it from the UEFI-safe
 * kernel/boot_version_constants.h the bootloader also compiles against), so
 * this reservation cannot name a different address than the producer used. */

static struct boot_reserved_region s_table[BOOT_RESERVED_MAX];
static uint32_t s_count;

static const char *kind_name(uint32_t kind)
{
    switch (kind) {
    case BOOT_RESERVED_BOOT_INFO:      return "boot_info";
    case BOOT_RESERVED_USB_DMA_PAGE:   return "usb_dma_page";
    case BOOT_RESERVED_USB_SCRATCHPAD: return "usb_scratchpad";
    case BOOT_RESERVED_TPM_EVENT_LOG:  return "tpm_event_log";
    case BOOT_RESERVED_FRAMEBUFFER:    return "framebuffer";
    case BOOT_RESERVED_RT_MMAP:        return "rt_mmap";
    case BOOT_RESERVED_PAYLOAD:        return "payload";
    case BOOT_RESERVED_BOOT_STACK:     return "boot_stack";
    default:                           return "unknown";
    }
}

static int range_end_overflows(uint64_t start, uint64_t len)
{
    if (len == 0u)
        return 0;
    return len > (uint64_t)-1 - start;
}

static int ranges_overlap(uint64_t a_start, uint64_t a_len,
                          uint64_t b_start, uint64_t b_len)
{
    if (a_len == 0u || b_len == 0u)
        return 0;
    uint64_t a_end = a_start + a_len;
    uint64_t b_end = b_start + b_len;
    return a_start < b_end && b_start < a_end;
}

/* Append one reservation. Rejects zero-length, range wrap, table
 * overflow, and any overlap with an existing entry. On overlap the
 * caller gets enough context via klog to identify both offenders. */
static enum boot_reserved_error add_region(uint64_t phys_start,
                                           uint64_t length,
                                           uint32_t kind,
                                           uint32_t source_index)
{
    if (length == 0u)
        return BOOT_RESERVED_ERR_ZERO_LENGTH;
    if (range_end_overflows(phys_start, length))
        return BOOT_RESERVED_ERR_RANGE_WRAP;
    if (s_count >= BOOT_RESERVED_MAX)
        return BOOT_RESERVED_ERR_COUNT_OOR;

    uint32_t i;
    for (i = 0u; i < s_count; i++) {
        const struct boot_reserved_region *e = &s_table[i];
        if (ranges_overlap(phys_start, length, e->phys_start, e->length)) {
            klog(LOG_ERROR, "mm",
                 "boot_reserved: overlap -- new kind=%s phys=0x%x+%u "
                 "vs existing kind=%s phys=0x%x+%u",
                 (uint64_t)(uintptr_t)kind_name(kind),
                 phys_start, length,
                 (uint64_t)(uintptr_t)kind_name(e->kind),
                 e->phys_start, e->length);
            return BOOT_RESERVED_ERR_OVERLAP;
        }
    }

    s_table[s_count].phys_start   = phys_start;
    s_table[s_count].length       = length;
    s_table[s_count].kind         = kind;
    s_table[s_count].source_index = source_index;
    s_count++;
    return BOOT_RESERVED_ERR_OK;
}

static boot_result_t add_or_fatal(uint64_t phys_start, uint64_t length,
                                  uint32_t kind, uint32_t source_index,
                                  enum boot_reserved_error *out_err)
{
    enum boot_reserved_error e = add_region(phys_start, length, kind, source_index);
    if (e != BOOT_RESERVED_ERR_OK) {
        if (out_err != (enum boot_reserved_error *)0)
            *out_err = e;
        return BOOT_FATAL;
    }
    return BOOT_OK;
}

/* Did the Phase-0 reservation pass actually PIN the payload descriptor at
 * `payload_index`?
 *
 * THIS IS THE ONLY HONEST ANSWER, and a consumer re-deriving it from the
 * descriptor is the bug this function exists to remove. Until the per-type
 * length contract landed, every reason the pass could decline was
 * re-derivable by a consumer from the handoff alone (capability absent,
 * warm-update flag clear, seed length out of contract), so checking
 * FLAG_RESERVED plus those conditions happened to be equivalent to asking
 * what was pinned. The aggregate budget and the singleton rule broke that
 * equivalence: BOTH depend on the OTHER descriptors in the table and on the
 * order the pass walked them, which no consumer can reconstruct. A
 * budget-skipped seed would otherwise be read pre-IDT by
 * canary_seed_desc_ok() with FLAG_RESERVED still set and its length
 * perfectly in contract.
 *
 * The handoff bytes are deliberately NOT rewritten to encode this. Clearing
 * FLAG_RESERVED on a declined descriptor would be a smaller change and was
 * considered; it was rejected because `boot_info` is a measured, dumped and
 * attested record, and silently editing a producer's assertion inside it
 * makes the record disagree with what the producer actually sent.
 *
 * Answers from the reservation TABLE, which stores the source index of every
 * payload entry it admitted, so it cannot drift from what was pinned: the
 * table IS the record of the decision. */
int boot_reserved_payload_is_pinned(uint32_t payload_index,
                                    uint64_t phys_start, uint64_t length)
{
    uint32_t n = boot_reserved_count();
    uint32_t k;

    for (k = 0u; k < n; k++) {
        const struct boot_reserved_region *r = boot_reserved_get(k);
        /* IDENTITY, not slot number. Matching source_index alone made the
         * answer depend only on WHICH SLOT was asked about, so any payload
         * reservation at that slot -- from a different handoff entirely --
         * read as "pinned". boot_headless_authz_take_from() accepts a
         * caller-supplied boot_info, so that is reachable rather than
         * theoretical, and it also let a test pass for the wrong reason
         * against a reservation an earlier test had left behind. The range
         * is what was actually pinned, so the range is what has to match.
         * (Re-adversarial finding on the fix for the previous round.) */
        if (r != (const struct boot_reserved_region *)0
            && r->kind == BOOT_RESERVED_PAYLOAD
            && r->source_index == payload_index
            && r->phys_start == phys_start
            && r->length == length)
            return 1;
    }
    return 0;
}

/* Is a sealed warm-update extent one PMM will actually act on?
 *
 * pmm_mark_region_used() clamps only the END of a range against total_frames
 * (src/kernel/mm/pmm.c), so a range that STARTS past the last tracked frame
 * marks nothing and reports nothing. The reservation table would still hold
 * the entry, and the commit flag would certify an extent no consumer can map
 * -- a positive claim about memory that does not exist. Phase 0 cannot check
 * this for itself: the memory map has not been walked when the selection is
 * sealed, which is exactly why the commit is a separate step.
 * (Round-3 adversarial finding.)
 *
 * The caller has already established length != 0 and that start + length does
 * not wrap, so the last-frame arithmetic below cannot underflow or overflow. */
static int warm_range_is_applicable(uint64_t phys_start, uint64_t length)
{
    uint64_t total = pmm_get_total_frames();
    uint64_t last_frame;

    if (total == 0u)
        return 0;
    last_frame = (phys_start + length - 1u) / (uint64_t)PMM_FRAME_SIZE;
    return last_frame < total;
}

boot_result_t boot_reserved_populate_from_info(const struct boot_info *info,
                                               enum boot_reserved_error *out_err)
{
    if (out_err != (enum boot_reserved_error *)0)
        *out_err = BOOT_RESERVED_ERR_OK;
    if (info == (const struct boot_info *)0) {
        if (out_err != (enum boot_reserved_error *)0)
            *out_err = BOOT_RESERVED_ERR_NULL_INFO;
        return BOOT_FATAL;
    }

    /* 1. struct boot_info at BOOT_INFO_PHYS_ADDR. */
    if (add_or_fatal(BOOT_INFO_PHYS_ADDR, (uint64_t)sizeof(struct boot_info),
                     BOOT_RESERVED_BOOT_INFO, 0u, out_err) != BOOT_OK)
        return BOOT_FATAL;

    /* 2. xHCI DMA pages (DCBAA, command ring, event ring, ERST, etc.).
     *    Each page is its own entry so overlap detection can flag a
     *    producer bug that points two slots at the same page.
     *
     *    Bootloader caveat (src/boot/uefi/bootx64.c around line 4491):
     *    the producer records `scratchpad_base_phys` in BOTH
     *    `scratchpad_base_phys` and `dma_pages[]`. If we registered
     *    both as-is, the 4 KiB DMA-page entry for sp_base_addr would
     *    overlap the contiguous scratchpad entry that also covers
     *    sp_base_addr+4096, failing populate on every xHCI system
     *    with scratchpads. Skip the scratchpad base address from the
     *    DMA-page loop; the scratchpad entry below covers it and all
     *    remaining scratchpad pages. */
    if (info->usb_controller.active && info->usb_controller.dma_page_count > 0u) {
        uint32_t n = info->usb_controller.dma_page_count;
        if (n > BOOT_USB_MAX_DMA_PAGES)
            n = BOOT_USB_MAX_DMA_PAGES;
        uint64_t sp_base = info->usb_controller.scratchpad_base_phys;
        uint32_t sp_count = info->usb_controller.scratchpad_page_count;
        uint64_t sp_array = info->usb_controller.scratchpad_array_phys;
        int have_scratchpad = (sp_base != 0u && sp_count > 0u);
        /* Scratchpad pointer array length in bytes = sp_count * 8 (each
         * pointer is uint64_t in xHCI). Bootloader allocates
         * ((sp_count * 8 + 4095) / 4096) pages; only the BASE page lands
         * in dma_pages[]. For sp_count > 512 the array spans multiple
         * pages and the tail pages would be reclaimed by PMM as free
         * memory while the controller still references them via the
         * loaded ERST/Slot context -- silent DMA corruption on real
         * hardware advertising a large MaxScratchpadBufs in HCSPARAMS2.
         * Compute the real array byte length and reserve that span when
         * we encounter the array's base page below. */
        uint64_t sp_array_len = 0u;
        if (sp_array != 0u && sp_count > 0u) {
            uint64_t array_bytes = (uint64_t)sp_count * 8ull;
            sp_array_len = ((array_bytes + 4095ull) & ~4095ull);
        }
        uint32_t u;
        for (u = 0u; u < n; u++) {
            uint64_t page = info->usb_controller.dma_pages[u];
            if (page == 0u)
                continue;  /* zero = unused slot */
            if (have_scratchpad && page == sp_base)
                continue;  /* scratchpad buffer pages covered below */
            if (sp_array_len != 0u && page == sp_array) {
                /* Scratchpad pointer array: reserve the full multi-page
                 * extent rather than the base 4 KiB page. */
                if (add_or_fatal(page, sp_array_len,
                                 BOOT_RESERVED_USB_DMA_PAGE, u, out_err)
                        != BOOT_OK)
                    return BOOT_FATAL;
                continue;
            }
            if (add_or_fatal(page, 4096ull,
                             BOOT_RESERVED_USB_DMA_PAGE, u, out_err) != BOOT_OK)
                return BOOT_FATAL;
        }
        /* 3. xHCI scratchpad contiguous pages. */
        if (have_scratchpad) {
            uint64_t slen = (uint64_t)sp_count * 4096ull;
            if (add_or_fatal(sp_base, slen,
                             BOOT_RESERVED_USB_SCRATCHPAD, 0u, out_err) != BOOT_OK)
                return BOOT_FATAL;
        }
    }

    /* 4. TPM event log copy. Gated on caps: a loader that advertised
     * BOOT_CAP_TPM_EVENT_LOG as degraded (log integrity failure,
     * etc.) MUST NOT force the kernel to pin the region even when
     * the companion fields are populated. A consumer that skipped
     * parsing the log (tpm_init with degraded caps) has no need of
     * a preserved region. Reads info->caps_present directly (not the
     * g_boot_info global) so test fixtures that call
     * boot_reserved_populate_from_info with a synthetic boot_info
     * can exercise both gate branches. */
    if (info->tpm_event_log != 0u && info->tpm_event_log_size > 0u &&
        (info->caps_present & BOOT_CAP_TPM_EVENT_LOG) != 0u) {
        if (add_or_fatal((uint64_t)info->tpm_event_log,
                         (uint64_t)info->tpm_event_log_size,
                         BOOT_RESERVED_TPM_EVENT_LOG, 0u, out_err) != BOOT_OK)
            return BOOT_FATAL;
    }

    /* 5. Linear framebuffer. pitch * height bounds the consumed bytes
     *    (pitch already includes bytes-per-pixel). UINT32 * UINT32
     *    fits UINT64 so no overflow guard needed beyond add_region's. */
    if (info->fb_available && info->fb.addr != 0u &&
        info->fb.pitch > 0u && info->fb.height > 0u) {
        uint64_t fb_len = (uint64_t)info->fb.pitch * (uint64_t)info->fb.height;
        if (add_or_fatal((uint64_t)info->fb.addr, fb_len,
                         BOOT_RESERVED_FRAMEBUFFER, 0u, out_err) != BOOT_OK)
            return BOOT_FATAL;
    }

    /* 6. UEFI runtime memory map regions. Each length is
     *    num_pages * 4096; add_region's wrap check guards the product. */
    if (info->rt_mmap_count > BOOT_RT_MMAP_MAX) {
        if (out_err != (enum boot_reserved_error *)0)
            *out_err = BOOT_RESERVED_ERR_COUNT_OOR;
        klog(LOG_ERROR, "mm",
             "boot_reserved: rt_mmap_count=%u > BOOT_RT_MMAP_MAX=%u",
             (uint64_t)info->rt_mmap_count, (uint64_t)BOOT_RT_MMAP_MAX);
        return BOOT_FATAL;
    }
    /* Gated on BOOT_CAP_RUNTIME_SERVICES: when the capability is
     * degraded (RT init failed and `rt_advertise_unavailable()` cleared
     * the bit, OR the bootloader populated the field but never asserted
     * caps_present), the kernel has explicitly decided runtime services
     * are unavailable. Pinning rt_mmap regions in that state would
     * waste physical memory on regions the kernel will never call into.
     * Codex 2026-04-30 finding: previously unconditional. */
    if ((info->caps_present & BOOT_CAP_RUNTIME_SERVICES) != 0u) {
        uint32_t i;
        for (i = 0u; i < info->rt_mmap_count; i++) {
            const struct boot_rt_mem_entry *rt = &info->rt_mmap[i];
            if (rt->phys_addr == 0u || rt->num_pages == 0u)
                continue;
            /* num_pages is uint64_t; multiplying by 4096 without a
             * pre-guard can wrap. A producer bug or corrupt handoff
             * with num_pages above UINT64_MAX / 4096 would land a tiny
             * reservation and leave the rest of the runtime region
             * free for reclaim -- silent corruption when
             * SetVirtualAddressMap later touches it. */
            if (rt->num_pages > ((uint64_t)-1 / 4096ull)) {
                if (out_err != (enum boot_reserved_error *)0)
                    *out_err = BOOT_RESERVED_ERR_RANGE_WRAP;
                klog(LOG_ERROR, "mm",
                     "boot_reserved: rt_mmap[%u] num_pages=%u wraps on *4096",
                     (uint64_t)i, rt->num_pages);
                return BOOT_FATAL;
            }
            uint64_t rt_len = rt->num_pages * 4096ull;
            if (add_or_fatal(rt->phys_addr, rt_len,
                             BOOT_RESERVED_RT_MMAP, i, out_err) != BOOT_OK)
                return BOOT_FATAL;
        }
    }

    /* 7. typed payloads with BOOT_PAYLOAD_FLAG_RESERVED set.
     *
     * Structural validation runs UNCONDITIONALLY: payload_count
     * bounds + slot consistency are malformed-handoff defenses that
     * must fire regardless of capability classification. Otherwise a
     * producer that forgot to set BOOT_CAP_PAYLOAD_DESCRIPTORS but
     * still emitted a corrupt payload_count would slip past the
     * gate.
     *
     * The RESERVATION LOOP is gated on BOOT_CAP_PAYLOAD_DESCRIPTORS:
     * when the capability is degraded, PMM reservation is skipped
     * even if the producer populated the descriptors. A producer
     * that reports the capability as degraded is saying "do not
     * consume the descriptors"; the gate enforces that contract. */
    if (info->payload_count > BOOT_PAYLOAD_MAX) {
        if (out_err != (enum boot_reserved_error *)0)
            *out_err = BOOT_RESERVED_ERR_COUNT_OOR;
        klog(LOG_ERROR, "mm",
             "boot_reserved: payload_count=%u > BOOT_PAYLOAD_MAX=%u",
             (uint64_t)info->payload_count, (uint64_t)BOOT_PAYLOAD_MAX);
        return BOOT_FATAL;
    }
    /* The cross-phase warm-update selection, read ONCE and OUTSIDE the
     * capability gate below.
     *
     * Phase 0 sealed this from the same boot_info; asking again returns that
     * same record rather than recomputing an opinion, so "the consumer
     * accepted it" and "the pass pinned it" are one decision about one
     * descriptor. The read must sit ABOVE the caps gate: BOOT_CAP_PAYLOAD
     * _DESCRIPTORS is one of the conditions the SELECTION already evaluated,
     * so re-testing it here would let a bit cleared after Phase 0 skip the
     * reservation of a region Phase 0 was authorized to reattach -- the exact
     * cross-phase split this section removes, reintroduced by the gate that
     * was supposed to be safe. (Codex test-coverage finding on this
     * section.) */
    const struct boot_warm_update_sel *warm =
        boot_warm_update_selection_get(info);

    if ((info->caps_present & BOOT_CAP_PAYLOAD_DESCRIPTORS) != 0u) {
        uint64_t reserved_total = 0u;
        uint32_t type_seen[BOOT_PAYLOAD_MAX];
        uint32_t seen_count = 0u;
        uint32_t pass;
        uint32_t i;

        /* TWO PASSES, REQUIRED FIRST, and the ORDER is the point.
         *
         * The aggregate budget below is consumed in table order, so with a
         * single pass a run of optional payloads sitting earlier in the
         * table could exhaust it and starve a REQUIRED payload that happens
         * to sit later -- a boot outcome decided by descriptor ordering
         * rather than by policy. Claiming the required ones first makes the
         * budget deterministic: an optional payload is what gets dropped
         * when the handoff asks for too much, which is what "optional"
         * means. (Design-review finding on the per-type payload length
         * contract.) */
        for (pass = 0u; pass < 2u; pass++) {
            for (i = 0u; i < info->payload_count; i++) {
                const struct boot_payload_desc *d =
                    &info->payload_descriptors[i];
                int is_required =
                    (int)((d->flags & BOOT_PAYLOAD_FLAG_REQUIRED) != 0u);
                uint32_t j;
                int repeated = 0;

                if ((pass == 0u) != (is_required != 0))
                    continue;
                if ((d->flags & BOOT_PAYLOAD_FLAG_RESERVED) == 0u)
                    continue;
                if (d->length == 0u)
                    continue;

                /* Warm-update takes NOTHING from this loop -- not the
                 * FLAG_RESERVED test, not the length contract, not the
                 * singleton rule, not the budget. Every one of those is
                 * either already inside the sealed selection or is a
                 * table-order rule Phase 0 cannot reproduce, and the sealed
                 * range is pinned by its own step after this block. Skipping
                 * here is what makes "one descriptor, chosen once" true
                 * rather than nearly true. */
                if (d->type == (uint32_t)BOOT_PAYLOAD_WARM_UPDATE_STATE) {
                    if (warm->verdict
                            != (uint32_t)BOOT_WARM_UPDATE_SELECT_ONE
                        || i != warm->index) {
                        klog(LOG_WARN, "mm",
                             "boot_reserved: skip warm-update payload[%u] "
                             "(selection verdict=%u candidates=%u err=%u "
                             "mismatch=%u; cold init proceeds)",
                             (uint64_t)i, (uint64_t)warm->verdict,
                             (uint64_t)warm->candidate_count,
                             (uint64_t)warm->error, (uint64_t)warm->mismatch);
                    }
                    continue;
                }

                /* Every type is bounded by its OWN contract before it may
                 * pin anything. A malformed descriptor carrying
                 * FLAG_RESERVED and a gigabyte-scale length would otherwise
                 * reserve that whole span here, permanently, and starve the
                 * PMM -- the boot would die in heap_init rather than
                 * degrade. Consumers apply the identical predicate and
                 * refuse to touch what this pass declined to pin, so the
                 * two cannot disagree about who owns the frames.
                 *
                 * This generalizes the RANDOM_SEED-only check that shipped
                 * with the early-entropy seed payload capability gate; the
                 * other types
                 * were pinning whatever they declared. */
                if (!boot_payload_length_reservable(d->type, d->length)) {
                    klog(LOG_WARN, "mm",
                         "boot_reserved: skip %s payload[%u] (length %lu "
                         "outside the type contract; not pinned)",
                         boot_payload_type_label(d->type),
                         (uint64_t)i, (uint64_t)d->length);
                    continue;
                }

                /* WARM-UPDATE TAKES NO TABLE-WIDE RULE FROM THIS PASS, and
                 * the reason is ORDERING, not privilege.
                 *
                 * boot_hw.c consumes the type-9 descriptor at Phase 0 BEFORE
                 * pmm_init() runs this pass. Any rule applied HERE that the
                 * consumer cannot itself evaluate splits the decision across
                 * two phases: the consumer accepts a descriptor and reattaches
                 * its preserved state, and this pass then declines to pin the
                 * range PMM will reclaim. That is why the singleton rule below
                 * cannot be the answer -- with an optional descriptor first
                 * and a REQUIRED duplicate second, required-first pinning
                 * takes the second while a table-order consumer takes the
                 * first.
                 *
                 * Cardinality is answered instead by the SEALED SELECTION
                 * above, which both phases read rather than recompute, so
                 * "exactly one" is enforced without either phase deciding it
                 * alone. The aggregate budget stays here and warm-update stays
                 * outside it: the budget is consumed in table order, which
                 * Phase 0 cannot reproduce. The exposure is bounded by
                 * construction -- at most ONE descriptor, capped by the type's
                 * own 64 MiB length contract, gated on BOOT_FLAG_WARM_UPDATE.
                 * A type-9 descriptor has already `continue`d above, so the
                 * rules from here down apply to normal payloads only. */
                /* Cardinality. Every type except MODULE is a singleton by
                 * meaning, so a second occurrence is a malformed or hostile
                 * handoff -- and pinning it multiplies the memory one type
                 * may claim, which per-descriptor bounds cannot see. The
                 * FIRST occurrence wins, matching how the payload consumers
                 * already resolve duplicates. */
                if (!BOOT_PAYLOAD_TYPE_IS_REPEATABLE(d->type)) {
                    for (j = 0u; j < seen_count; j++) {
                        if (type_seen[j] == d->type) {
                            repeated = 1;
                            break;
                        }
                    }
                    if (repeated) {
                        klog(LOG_WARN, "mm",
                             "boot_reserved: skip duplicate %s payload[%u] "
                             "(type is a singleton; an earlier occurrence "
                             "was pinned)",
                             boot_payload_type_label(d->type), (uint64_t)i);
                        continue;
                    }
                    /* NOT recorded as seen here. A type is "seen" only once
                     * an occurrence is actually PINNED, below. Recording it
                     * at this point would let a first occurrence that the
                     * BUDGET then rejects block a later, smaller occurrence
                     * that would have fit -- and the refusal line would
                     * claim an earlier one was kept when nothing was.
                     * (Adversarial finding on this section.) */
                }

                /* Aggregate budget. Bounding each term does not bound the
                 * sum: 32 individually-legal 256 MiB descriptors would pin
                 * 8 GiB while passing every per-type rule, which is the
                 * starvation this section exists to close. Compared before
                 * adding, and written so the addition itself cannot
                 * overflow. */
                /* Warm-update never reaches this check, and its bytes are
                 * never charged to reserved_total: charging a payload the
                 * budget did not gate would make normal payload admission
                 * depend on where the warm descriptor sits in the table. The
                 * 2 GiB worst case an earlier round left open (32 descriptors
                 * at 64 MiB each) is closed by the sealed selection rather
                 * than by this budget -- at most one type-9 descriptor is
                 * ever pinned, and not from this loop. */
                if (!boot_payload_budget_admits(reserved_total, d->length)) {
                    klog(LOG_WARN, "mm",
                         "boot_reserved: skip %s payload[%u] (length %lu "
                         "exceeds the remaining %lu-byte payload reservation "
                         "budget; not pinned)",
                         boot_payload_type_label(d->type), (uint64_t)i,
                         (uint64_t)d->length,
                         (uint64_t)(BOOT_PAYLOAD_RESERVE_TOTAL_MAX
                                    - reserved_total));
                    continue;
                }

                if (add_or_fatal(d->phys_start, d->length,
                                 BOOT_RESERVED_PAYLOAD, i, out_err) != BOOT_OK)
                    return BOOT_FATAL;
                reserved_total += d->length;
                /* Seen only now, when an occurrence of this type has really
                 * been pinned -- see the cardinality block above. */
                if (!BOOT_PAYLOAD_TYPE_IS_REPEATABLE(d->type)
                    && seen_count < (uint32_t)BOOT_PAYLOAD_MAX)
                    type_seen[seen_count++] = d->type;
            }
        }
    }

    /* Warm-update reservation, driven by the SEAL and by nothing else.
     *
     * It sits outside the capability-gated loop above on purpose. Every
     * condition that decides whether this range may be pinned was evaluated
     * once, by whichever phase read the selection first, and sealed: the
     * capability bit, the BOOT_FLAG_WARM_UPDATE signal, descriptor
     * admissibility, and cardinality. Re-testing any of them here is what
     * would let a bit cleared after Phase 0 leave preserved state reattached
     * and unpinned.
     *
     * The range comes from the SEALED RECORD rather than from
     * info->payload_descriptors[warm->index], so a handoff that moved after
     * Phase 0 read it cannot redirect the pin either. */
    if (warm->verdict == (uint32_t)BOOT_WARM_UPDATE_SELECT_ONE
        && warm_range_is_applicable(warm->phys_start, warm->length)) {
        if (warm->mismatch != 0u) {
            /* Pin it anyway. The pages may already be reattached, so handing
             * them back to PMM is the one outcome that cannot be undone; a
             * reattach consumer reads `mismatch` and refuses. */
            klog(LOG_ERROR, "mm",
                 "boot_reserved: warm-update payload[%u] pinned from the "
                 "SEALED range 0x%lx+%lu -- the handoff changed after Phase 0 "
                 "read it, so reattach must be refused",
                 (uint64_t)warm->index, (uint64_t)warm->phys_start,
                 (uint64_t)warm->length);
        }
        if (add_or_fatal(warm->phys_start, warm->length,
                         BOOT_RESERVED_PAYLOAD, warm->index,
                         out_err) != BOOT_OK)
            return BOOT_FATAL;
    }

    /* Kernel boot stack (TODO-10 sec32). The run the kernel is executing on
     * RIGHT NOW: the bootloader allocated it as EfiLoaderData, so the memory
     * map may still describe those frames as reclaimable and pmm_init's map
     * walk frees them. This entry, not the map type, is what keeps the
     * allocator off them.
     *
     * It belongs in this table rather than in a direct pmm_mark_region_used()
     * call beside the kernel-image and bitmap reservations, because unlike
     * those its address comes from a bootloader handoff field -- so it must
     * take part in the same overlap detection, log line and BlackBox record
     * as every other retained region. A stack that collided with the
     * framebuffer or a reserved payload would otherwise become two successful
     * bitmap reservations and no complaint.
     *
     * Validation lives in boot_stack_init(), which ran in Phase 0 and halted
     * on a bad handoff; the fields are re-checked here only for the shapes
     * that would corrupt this table itself (zero base, zero length), because
     * this function is also reachable from tests with a synthetic boot_info
     * that never went through Phase 0. */
    if (info->kstack_base != 0u && info->kstack_size != 0u) {
        if (add_or_fatal(info->kstack_base, (uint64_t)info->kstack_size,
                         BOOT_RESERVED_BOOT_STACK, 0u, out_err) != BOOT_OK)
            return BOOT_FATAL;
    }

    /* COMMIT, and only here -- after the WHOLE table was admitted.
     *
     * SELECT_ONE says the handoff is well-formed and unambiguous. It does not
     * say the range survived reservation, and reservation can refuse it for a
     * reason that has nothing to do with warm update: overlapping the kernel
     * boot stack or another retained region is a fatal handoff error. Marking
     * the commit beside the warm add was not enough for exactly that case --
     * the stack entry is added AFTER it, so an overlapping warm range read as
     * committed for the instant before the boot died. A restore consumer that
     * acted on the verdict alone would reattach over memory PMM was never
     * told to keep. (Round-2 adversarial finding, and its own regression
     * test.)
     *
     * Everything that can still fail after this point -- the PMM-internal
     * disjointness checks in pmm_init(), which run before
     * boot_reserved_apply() -- halts the boot, so no consumer ever observes a
     * commit the machine did not honor. */
    if (warm->verdict == (uint32_t)BOOT_WARM_UPDATE_SELECT_ONE) {
        if (warm_range_is_applicable(warm->phys_start, warm->length)) {
            boot_warm_update_selection_mark_pinned();
        } else {
            /* Deliberately NOT fatal, and deliberately not pinned. The rest of
             * the handoff may be perfectly good; what this says is that the
             * preserved region is not memory this machine has, so no consumer
             * may reattach it. `pinned` staying clear is the whole mechanism
             * -- the restore path is gated on it -- so cold init proceeds. */
            klog(LOG_ERROR, "mm",
                 "boot_reserved: warm-update payload[%u] range 0x%lx+%lu lies "
                 "outside the %lu frames PMM tracks; not pinned, not "
                 "committed, cold init proceeds",
                 (uint64_t)warm->index, (uint64_t)warm->phys_start,
                 (uint64_t)warm->length, (uint64_t)pmm_get_total_frames());
        }
    }

    return BOOT_OK;
}

void boot_reserved_apply(void)
{
    uint32_t i;
    for (i = 0u; i < s_count; i++) {
        const struct boot_reserved_region *e = &s_table[i];
        pmm_mark_region_used((uintptr_t)e->phys_start, e->length);
    }
}

uint32_t boot_reserved_check_payloads_disjoint(uint64_t start,
                                               uint64_t len,
                                               const char *label)
{
    uint32_t i;
    if (len == 0u)
        return 0u;
    for (i = 0u; i < s_count; i++) {
        const struct boot_reserved_region *e = &s_table[i];
        if (e->kind != (uint32_t)BOOT_RESERVED_PAYLOAD)
            continue;
        if (ranges_overlap(e->phys_start, e->length, start, len)) {
            klog(LOG_ERROR, "mm",
                 "boot_reserved: payload[%u] phys=0x%x+%u overlaps "
                 "PMM-internal region %s 0x%x+%u",
                 (uint64_t)i, e->phys_start, e->length,
                 (uint64_t)(uintptr_t)(label ? label : "?"),
                 start, len);
            return i + 1u;
        }
    }
    return 0u;
}

void boot_reserved_log(void)
{
    uint32_t i;
    klog(LOG_INFO, "mm",
         "boot_reserved: %u retained region(s) populated from boot_info",
         (uint64_t)s_count);
    for (i = 0u; i < s_count; i++) {
        const struct boot_reserved_region *e = &s_table[i];
        klog(LOG_INFO, "mm",
             "  [%u] kind=%s src=%u phys=0x%x len=%u",
             (uint64_t)i,
             (uint64_t)(uintptr_t)kind_name(e->kind),
             (uint64_t)e->source_index,
             e->phys_start, e->length);
    }
}

/* X:\Diag\boot-reserved.json writer. Called late in Phase 3 (next to
 * hw_dump_write_file) where IXFS + X:\ are mounted and vfs_write is
 * safe. Early-boot callers can invoke this function; it no-ops when
 * klog_using_blackbox is 0 (e.g. during a test that never brings up
 * storage). Idempotent: overwrites the file on every call. */
void boot_reserved_blackbox_dump(void)
{
    extern int klog_using_blackbox;
    extern const char *klog_dir;
    if (!klog_using_blackbox)
        return;  /* No BlackBox partition; log-only path remains. */

    const char *diag_dir = "X:\\Diag\\";
    /* Max size: each entry costs ~110 chars of JSON; with 128 entries
     * + 100 bytes of header/footer/framing we need ~14 KiB. Round up
     * to 16 KiB -- this is a one-shot heap buffer released after the
     * write. */
    const uint32_t max_sz = 16384u;
    char *buf = (char *)kmalloc(max_sz);
    if (!buf) {
        klog(LOG_WARN, "mm",
             "boot_reserved_blackbox_dump: kmalloc(%u) failed",
             (uint64_t)max_sz);
        return;
    }

    uint32_t pos = 0u;
    uint32_t i;
    int n;
    char line[192];
    const char *open_hdr = "{\n  \"retained_regions\": [\n";
    for (i = 0u; open_hdr[i] && pos < max_sz - 1u; i++)
        buf[pos++] = open_hdr[i];

    for (i = 0u; i < s_count; i++) {
        const struct boot_reserved_region *e = &s_table[i];
        /* Use %llx for phys_start + length so 64-bit values above 4 GiB
         * are not truncated. Hex strings keep the JSON readable and
         * let parsers distinguish overlong tokens from real numbers. */
        n = snprintf(line, sizeof(line),
                     "    { \"kind\": \"%s\", \"phys\": \"0x%llx\", "
                     "\"length\": \"0x%llx\", \"src\": %u }%s\n",
                     kind_name(e->kind),
                     (unsigned long long)e->phys_start,
                     (unsigned long long)e->length,
                     e->source_index,
                     (i + 1u < s_count) ? "," : "");
        if (n < 0 || (uint32_t)n >= sizeof(line))
            continue;  /* truncation: skip the malformed line */
        int j;
        for (j = 0; j < n && pos < max_sz - 1u; j++)
            buf[pos++] = line[j];
    }

    const char *close_hdr = "  ]\n}\n";
    for (i = 0u; close_hdr[i] && pos < max_sz - 1u; i++)
        buf[pos++] = close_hdr[i];

    char path[64];
    int pi = 0;
    int j;
    for (j = 0; diag_dir[j] && pi < (int)sizeof(path) - 1; j++)
        path[pi++] = diag_dir[j];
    const char *fn = "boot-reserved.json";
    for (j = 0; fn[j] && pi < (int)sizeof(path) - 1; j++)
        path[pi++] = fn[j];
    path[pi] = '\0';

    /* Single-open VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC -- creates
     * the file if missing, truncates to zero if it exists. No more
     * dir-handle pre-create dance and no more close+truncate+reopen
     * for prior-size handling. */
    struct vfs_node *f = vfs_open(path,
                                  VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) {
        klog(LOG_WARN, "mm",
             "boot_reserved_blackbox_dump: vfs_open(%s) returned NULL; "
             "audit artifact NOT written this boot",
             (uint64_t)(uintptr_t)path);
    } else {
        int wrote = vfs_write(f, 0, pos, (const uint8_t *)buf);
        vfs_close(f);
        if (wrote < 0 || (uint32_t)wrote != pos) {
            klog(LOG_ERROR, "mm",
                 "boot_reserved_blackbox_dump: short write to %s "
                 "(wanted=%u got=%d); audit artifact may be torn",
                 (uint64_t)(uintptr_t)path, (uint64_t)pos, (uint64_t)wrote);
        } else {
            klog(LOG_INFO, "mm",
                 "boot_reserved: dumped %u region(s) to %s",
                 (uint64_t)s_count, (uint64_t)(uintptr_t)path);
        }
    }
    kfree(buf);
    (void)klog_dir;  /* reserved for the headless-klog fallback path */
}

uint32_t boot_reserved_count(void)
{
    return s_count;
}

const struct boot_reserved_region *boot_reserved_get(uint32_t index)
{
    if (index >= s_count)
        return (const struct boot_reserved_region *)0;
    return &s_table[index];
}

#ifdef KERNEL_TESTS
void boot_reserved_reset_for_test(void)
{
    uint32_t i;
    for (i = 0u; i < BOOT_RESERVED_MAX; i++) {
        s_table[i].phys_start   = 0u;
        s_table[i].length       = 0u;
        s_table[i].kind         = 0u;
        s_table[i].source_index = 0u;
    }
    s_count = 0u;
}
#endif
