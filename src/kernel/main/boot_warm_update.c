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
 * is why no table-wide rule (cardinality, aggregate budget) belongs here: the
 * Phase-0 consumer runs before the reservation table exists, so a rule
 * depending on the OTHER descriptors could not be evaluated by both.
 *
 * `out_error` may be NULL for a caller that wants only the verdict.
 * boot_warm_update_desc_admissible() below is that caller. */
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

int boot_warm_update_desc_admissible(const struct boot_payload_desc *desc)
{
    /* The reservation pass's view of these rules, obtained by CALLING them
     * rather than restating them.
     *
     * The pass used to state its own subset and it was a strict subset: it
     * omitted rule 4 (page alignment) and rule 5 (continuation bits), so a
     * descriptor Phase 0 cold-fell-back was still pinned for the life of the
     * machine, up to the type's 64 MiB. Nothing but a shared implementation
     * prevents that -- a second list of the same rules is exactly how it
     * happened. */
    return boot_warm_update_consume(desc, (enum boot_warm_update_error *)0)
           == BOOT_WARM_UPDATE_ACCEPTED;
}
