/* ============================================================================
 * boot_warm_update.c -- warm-kernel-update handoff validator.
 *
 * Consumes BOOT_PAYLOAD_WARM_UPDATE_STATE descriptors produced by the
 * outgoing kernel during a live update. Validates shape + continuation
 * flags; returns a decision (accept vs cold-fallback) without halting.
 *
 * Fail-closed policy: ANY rejection -> COLD_FALLBACK. A partial reattach
 * would leave the incoming kernel with unreconciled subsystem state,
 * which is the exact failure mode this ABI prevents.
 *
 * This file ships the ABI-surface portion of the warm-kernel-update
 * handoff contract. The actual "reattach preserved memory into PMM +
 * restore subsystem state" work lives in the runtime live-update TODO
 * (todo/03-memory-concurrency/TODO-11-warm-kernel-update-runtime.md).
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"

const char *boot_warm_update_cont_name(uint32_t flag_bit)
{
    switch (flag_bit) {
    case BOOT_WARM_UPDATE_CONT_PAGE_TABLES:       return "page_tables";
    case BOOT_WARM_UPDATE_CONT_SCHEDULER_QUIESCED: return "scheduler_quiesced";
    case BOOT_WARM_UPDATE_CONT_VFS_WRITEBACK:     return "vfs_writeback";
    case BOOT_WARM_UPDATE_CONT_FD_TABLE:          return "fd_table";
    case BOOT_WARM_UPDATE_CONT_OBJECT_HANDLES:    return "object_handles";
    case BOOT_WARM_UPDATE_CONT_HW_QUEUES:         return "hw_queues";
    default:                                       return "reserved";
    }
}

/* Every condition below is DESCRIPTOR-LOCAL -- type, length, required flags,
 * page alignment, continuation bits -- so both boot phases can evaluate it
 * over the same bytes and reach the same answer. That is deliberate, and it
 * is why no table-wide rule (cardinality, aggregate budget) belongs here: a
 * rule depending on the OTHER descriptors has to be decided ONCE and shared,
 * which is what boot_warm_update_selection_get() below does. This function
 * validates a descriptor; it does not authorize a warm update.
 *
 * `out_error` may be NULL for a caller that wants only the verdict. */
enum boot_warm_update_decision
boot_warm_update_consume(const struct boot_payload_desc *desc,
                         enum boot_warm_update_error *out_error)
{
    if (out_error != (enum boot_warm_update_error *)0)
        *out_error = BOOT_WARM_UPDATE_ERR_OK;

    if (desc == (const struct boot_payload_desc *)0) {
        if (out_error != (enum boot_warm_update_error *)0)
            *out_error = BOOT_WARM_UPDATE_ERR_NULL_DESC;
        return BOOT_WARM_UPDATE_COLD_FALLBACK;
    }

    /* Rule 1: descriptor type must be WARM_UPDATE_STATE. A consumer
     * accidentally given the wrong descriptor is producer bug. */
    if (desc->type != (uint32_t)BOOT_PAYLOAD_WARM_UPDATE_STATE) {
        klog(LOG_WARN, "boot",
             "boot_warm_update: wrong descriptor type %u (expected %u); cold fallback",
             (uint64_t)desc->type,
             (uint64_t)BOOT_PAYLOAD_WARM_UPDATE_STATE);
        if (out_error != (enum boot_warm_update_error *)0)
            *out_error = BOOT_WARM_UPDATE_ERR_WRONG_TYPE;
        return BOOT_WARM_UPDATE_COLD_FALLBACK;
    }

    /* Rule 2: length must be nonzero. An empty descriptor carries no
     * preserved state; treat as absent. */
    if (desc->length == 0u) {
        klog(LOG_WARN, "boot",
             "boot_warm_update: descriptor length is 0; cold fallback");
        if (out_error != (enum boot_warm_update_error *)0)
            *out_error = BOOT_WARM_UPDATE_ERR_EMPTY;
        return BOOT_WARM_UPDATE_COLD_FALLBACK;
    }

    /* Rule 2b: length must be inside the type's own contract.
     *
     * Rules 2 and 4 together accepted ANY nonzero page-multiple length,
     * with no maximum anywhere -- so once BOOT_FLAG_WARM_UPDATE was set,
     * an outgoing kernel (or anything able to forge its handoff) could
     * declare an arbitrary span and the Phase-0 reservation pass would pin
     * it permanently. Preserved subsystem state is metadata, not bulk
     * data; the bound and the reasoning behind its value live in
     * include/boot/boot_payload_limits.h.
     *
     * This is the SAME predicate the reservation pass applies, called
     * rather than reimplemented, so the pass and this consumer cannot
     * disagree about what was pinned. (Design-review finding, TODO-01
     * the per-type payload length contract.) */
    if (!boot_payload_length_reservable(desc->type, desc->length)) {
        klog(LOG_WARN, "boot",
             "boot_warm_update: length %lu outside the type contract; cold fallback",
             (uint64_t)desc->length);
        if (out_error != (enum boot_warm_update_error *)0)
            *out_error = BOOT_WARM_UPDATE_ERR_LENGTH_CONTRACT;
        return BOOT_WARM_UPDATE_COLD_FALLBACK;
    }

    /* Rule 3: required generic payload flags must be set. The
     * warm-update region MUST survive PMM reclaim (RESERVED bit),
     * and a VALID descriptor is table-stakes. Without RESERVED, PMM
     * would reclaim the preserved pages as free memory at
     * pmm_init() -- the incoming kernel would corrupt or
     * use-after-free the outgoing kernel's subsystem state. */
    const uint32_t REQUIRED_PAYLOAD_FLAGS =
        BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_RESERVED;
    if ((desc->flags & REQUIRED_PAYLOAD_FLAGS) != REQUIRED_PAYLOAD_FLAGS) {
        klog(LOG_WARN, "boot",
             "boot_warm_update: missing required payload flags (have 0x%x, need 0x%x); cold fallback",
             (uint64_t)(desc->flags & REQUIRED_PAYLOAD_FLAGS),
             (uint64_t)REQUIRED_PAYLOAD_FLAGS);
        if (out_error != (enum boot_warm_update_error *)0)
            *out_error = BOOT_WARM_UPDATE_ERR_MISSING_FLAGS;
        return BOOT_WARM_UPDATE_COLD_FALLBACK;
    }

    /* Rule 4: phys_start AND length must be 4K-page-multiple. The
     * reattach path feeds the range to pmm_mark_region_used(), which
     * is page-granular; a non-page-multiple length would leave the
     * tail page owned by PMM but described as preserved, creating
     * silent leak/corruption at the boundary. */
    if ((desc->phys_start & 0xFFFull) != 0u ||
        (desc->length     & 0xFFFull) != 0u) {
        klog(LOG_WARN, "boot",
             "boot_warm_update: phys_start 0x%lx / length %lu not page-multiple; cold fallback",
             (uint64_t)desc->phys_start, (uint64_t)desc->length);
        if (out_error != (enum boot_warm_update_error *)0)
            *out_error = BOOT_WARM_UPDATE_ERR_UNALIGNED;
        return BOOT_WARM_UPDATE_COLD_FALLBACK;
    }

    /* Rule 5: continuation flags. Any bit in the continuation range
     * (bits 8..31) that is NOT in BOOT_WARM_UPDATE_CONT_MASK_KNOWN
     * means the outgoing kernel signaled a subsystem state this
     * incoming kernel does not understand. Fail-closed per spec.
     * Bits 0..7 are the standard BOOT_PAYLOAD_FLAG_* family handled
     * by the typed-payload-descriptor validator. */
    uint32_t cont_range = desc->flags & 0xFFFFFF00u;  /* bits 8..31 */
    uint32_t unknown_cont = cont_range & ~BOOT_WARM_UPDATE_CONT_MASK_KNOWN;
    if (unknown_cont != 0u) {
        klog(LOG_WARN, "boot",
             "boot_warm_update: unknown continuation bits 0x%x (known mask 0x%x); cold fallback",
             (uint64_t)unknown_cont,
             (uint64_t)BOOT_WARM_UPDATE_CONT_MASK_KNOWN);
        if (out_error != (enum boot_warm_update_error *)0)
            *out_error = BOOT_WARM_UPDATE_ERR_UNKNOWN_CONT_FLAG;
        return BOOT_WARM_UPDATE_COLD_FALLBACK;
    }

    /* All gates passed. Log a summary + return ACCEPTED. The actual
     * reattach-into-PMM + subsystem-state-restore work happens in
     * the runtime live-update TODO; this ABI-surface section only
     * decides whether it is safe to proceed. */
    klog(LOG_INFO, "boot",
         "boot_warm_update: descriptor accepted (phys=0x%lx length=%lu cont_flags=0x%x)",
         (uint64_t)desc->phys_start,
         (uint64_t)desc->length,
         (uint64_t)(desc->flags & BOOT_WARM_UPDATE_CONT_MASK_KNOWN));
    return BOOT_WARM_UPDATE_ACCEPTED;
}

/* The sealed cross-phase selection.
 *
 * SMP: written exactly once, on the BSP, from Phase 0 (boot_hw.c) long before
 * any AP is started, and read-only from then on. No lock: there is no writer
 * to race with by the time a second CPU exists, and the seal is what removes
 * the only ordering that mattered -- Phase 0 versus pmm_init on the same CPU.
 * s_sel_sealed is set LAST so a reader can never observe a half-filled
 * record. */
static struct boot_warm_update_sel s_sel;
static int s_sel_sealed;

/* Compute the verdict from `info` alone. Pure apart from the klog line the
 * single candidate's verbose validation emits. */
static void warm_update_compute(const struct boot_info *info,
                                struct boot_warm_update_sel *out)
{
    uint32_t i;
    uint32_t count = 0u;
    uint32_t chosen = 0u;

    out->verdict         = (uint32_t)BOOT_WARM_UPDATE_SELECT_NONE;
    out->index           = 0u;
    out->candidate_count = 0u;
    out->error           = (uint32_t)BOOT_WARM_UPDATE_ERR_NULL_DESC;
    out->mismatch        = 0u;
    out->pinned          = 0u;
    out->flags           = 0u;
    out->checksum        = 0u;
    out->phys_start      = 0u;
    out->length          = 0u;

    if (info == (const struct boot_info *)0)
        return;
    /* Both preconditions the two phases already applied independently, now
     * applied once. Without BOOT_FLAG_WARM_UPDATE the handoff never announced
     * a warm update, and without BOOT_CAP_PAYLOAD_DESCRIPTORS the producer
     * asked that the descriptor array not be consumed at all. */
    if ((info->flags & BOOT_FLAG_WARM_UPDATE) == 0u)
        return;
    if ((info->caps_present & BOOT_CAP_PAYLOAD_DESCRIPTORS) == 0u)
        return;
    /* A count past the array bound is a malformed handoff: refuse rather than
     * walk off the end. boot_payload_validate() and the reservation pass both
     * reject it too, this is the local guard that makes the scan safe on its
     * own terms. */
    if (info->payload_count > (uint32_t)BOOT_PAYLOAD_MAX)
        return;

    for (i = 0u; i < info->payload_count; i++) {
        if (info->payload_descriptors[i].type
            != (uint32_t)BOOT_PAYLOAD_WARM_UPDATE_STATE)
            continue;
        if (count == 0u)
            chosen = i;
        count++;
    }
    out->candidate_count = count;

    if (count == 0u)
        return;
    if (count > 1u) {
        out->verdict = (uint32_t)BOOT_WARM_UPDATE_SELECT_AMBIGUOUS;
        out->error   = (uint32_t)BOOT_WARM_UPDATE_ERR_OK;
        return;
    }

    {
        enum boot_warm_update_error werr = BOOT_WARM_UPDATE_ERR_OK;
        enum boot_warm_update_decision wd =
            boot_warm_update_consume(&info->payload_descriptors[chosen], &werr);
        out->error = (uint32_t)werr;
        if (wd != BOOT_WARM_UPDATE_ACCEPTED)
            return;
        out->verdict    = (uint32_t)BOOT_WARM_UPDATE_SELECT_ONE;
        out->index      = chosen;
        out->phys_start = info->payload_descriptors[chosen].phys_start;
        out->length     = info->payload_descriptors[chosen].length;
        /* Seal every field a consumer acts on, not just where the region is.
         * The continuation bits pick which restore callbacks run, and
         * BOOT_PAYLOAD_FLAG_CHECKSUMMED plus `checksum` decide whether the
         * preserved bytes are verified at all -- so leaving any of them in the
         * live descriptor lets a mutation change what a consumer DOES without
         * changing a single field the mismatch check compares. (Round-2 and
         * round-3 adversarial findings: the first version sealed only the
         * continuation bits and the checksum walked through the same hole.) */
        out->flags    = info->payload_descriptors[chosen].flags;
        out->checksum = info->payload_descriptors[chosen].checksum;
    }
}

const struct boot_warm_update_sel *
boot_warm_update_selection_get(const struct boot_info *info)
{
    struct boot_warm_update_sel now;

    if (!s_sel_sealed) {
        warm_update_compute(info, &s_sel);
        s_sel_sealed = 1;
        return &s_sel;
    }

    /* Already sealed. Re-deriving the verdict here is a DETECTOR, not a
     * second decision: a boot_info that moved between the two phases becomes
     * visible instead of silently producing two answers from one struct.
     *
     * AN AUTHORIZATION IS NEVER REVOKED. An earlier round forced the verdict
     * to AMBIGUOUS on disagreement, which is unsafe in the one direction that
     * matters: boot_hw.c has already acted on a SELECT_ONE by the time
     * pmm_init() looks (boot_hw.c calls pmm_init() further down its own
     * function), so withdrawing the verdict leaves preserved state reattached
     * and its pages handed back to PMM. Keeping the verdict and pinning the
     * SEALED range is the conservative outcome -- the worst case is memory
     * held that nobody reads -- while `mismatch` tells a reattach consumer to
     * refuse and puts a LOG_ERROR in the boot log, because a handoff that
     * moves after Phase 0 read it is corruption, not a policy choice. */
    if (s_sel.mismatch != 0u)
        return &s_sel;
    warm_update_compute(info, &now);
    if (now.verdict         != s_sel.verdict
        || now.index        != s_sel.index
        || now.candidate_count != s_sel.candidate_count
        || now.flags        != s_sel.flags
        || now.checksum     != s_sel.checksum
        || now.phys_start   != s_sel.phys_start
        || now.length       != s_sel.length) {
        klog(LOG_ERROR, "boot",
             "boot_warm_update: handoff changed after the selection was "
             "sealed (verdict %u->%u index %u->%u phys 0x%lx->0x%lx "
             "flags 0x%x->0x%x); the sealed range stays pinned and reattach "
             "must be refused",
             (uint64_t)s_sel.verdict, (uint64_t)now.verdict,
             (uint64_t)s_sel.index, (uint64_t)now.index,
             (uint64_t)s_sel.phys_start, (uint64_t)now.phys_start,
             (uint64_t)s_sel.flags, (uint64_t)now.flags);
        s_sel.mismatch = 1u;
    }
    return &s_sel;
}

void boot_warm_update_selection_mark_pinned(void)
{
    /* The commit half. A SELECT_ONE verdict says the handoff is well-formed
     * and unambiguous; it does NOT say the range was admitted -- the
     * reservation can still refuse it for reasons that have nothing to do
     * with warm update, such as overlapping the kernel boot stack. A consumer
     * that restores state requires this flag as well, so it cannot reattach
     * over memory PMM was never told to keep. */
    if (s_sel_sealed
        && s_sel.verdict == (uint32_t)BOOT_WARM_UPDATE_SELECT_ONE)
        s_sel.pinned = 1u;
}

#ifdef KERNEL_TESTS
void boot_warm_update_selection_reset_for_test(void)
{
    s_sel_sealed = 0;
    s_sel.verdict         = (uint32_t)BOOT_WARM_UPDATE_SELECT_NONE;
    s_sel.index           = 0u;
    s_sel.candidate_count = 0u;
    s_sel.error           = (uint32_t)BOOT_WARM_UPDATE_ERR_OK;
    s_sel.mismatch        = 0u;
    s_sel.pinned          = 0u;
    s_sel.flags           = 0u;
    s_sel.checksum        = 0u;
    s_sel.phys_start      = 0u;
    s_sel.length          = 0u;
}
#endif
