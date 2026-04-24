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

    /* Rule 3: phys_start must be 4K-aligned. The reattach path
     * requires page-granular memory to splice back into PMM. */
    if ((desc->phys_start & 0xFFFull) != 0u) {
        klog(LOG_WARN, "boot",
             "boot_warm_update: phys_start 0x%lx not page-aligned; cold fallback",
             (uint64_t)desc->phys_start);
        if (out_error != (enum boot_warm_update_error *)0)
            *out_error = BOOT_WARM_UPDATE_ERR_UNALIGNED;
        return BOOT_WARM_UPDATE_COLD_FALLBACK;
    }

    /* Rule 4: continuation flags. Any bit in the continuation range
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
