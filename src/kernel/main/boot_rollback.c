/* ============================================================================
 * boot_rollback.c -- anti-rollback + security-version helpers.
 *
 * Two functions:
 *   - boot_rollback_validate() -- sanity check on the boot_info fields
 *     the bootloader wrote (unknown flag bits, out-of-range versions).
 *     Runs in Phase 0 after boot_decision_validate. The downgrade
 *     refusal itself is enforced PRE-JUMP by the bootloader; if we
 *     see the kernel running, shipped >= required by construction.
 *   - boot_rollback_should_raise() -- decision helper called from
 *     the Phase 3 completion site. Returns 1 when the caller should
 *     write IPOSRequiredSecVersion with the shipped value (opt-in
 *     policy AND shipped > required).
 *
 * Logs at LOG_ERROR on validator reject; caller (boot_hw.c) converts
 * BOOT_FATAL to boot_halt(). Same stance as boot_caps.c and
 * boot_decision.c.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"

boot_result_t boot_rollback_validate(const struct boot_info *info,
                                     enum boot_rollback_error *out_error)
{
    if (out_error != (enum boot_rollback_error *)0)
        *out_error = BOOT_ROLLBACK_ERR_OK;

    if (info == (const struct boot_info *)0) {
        if (out_error != (enum boot_rollback_error *)0)
            *out_error = BOOT_ROLLBACK_ERR_NULL_INFO;
        return BOOT_FATAL;
    }

    uint32_t flags    = info->flags;
    uint32_t shipped  = info->os_loader_security_version;
    uint32_t required = info->required_security_version;

    /* Rule 1: flags outside BOOT_FLAG_MASK_KNOWN is a producer bug
     * (or a forged record). Unlike capability negotiation this is a
     * CLOSED mask -- no forward-compat tolerance. */
    uint32_t unknown = flags & ~BOOT_FLAG_MASK_KNOWN;
    if (unknown != 0u) {
        klog(LOG_ERROR, "boot",
             "boot_rollback: boot_info.flags 0x%x has unknown bits 0x%x (known mask 0x%x)",
             (uint64_t)flags, (uint64_t)unknown, (uint64_t)BOOT_FLAG_MASK_KNOWN);
        if (out_error != (enum boot_rollback_error *)0)
            *out_error = BOOT_ROLLBACK_ERR_UNKNOWN_FLAG;
        return BOOT_FATAL;
    }

    /* Rule 2: either version beyond MAX is a corrupted-NVRAM or
     * build-time signed value. Cap prevents a monotonic counter
     * runaway when the NVRAM variable holds garbage. */
    if (shipped > BOOT_SECURITY_VERSION_MAX || required > BOOT_SECURITY_VERSION_MAX) {
        klog(LOG_ERROR, "boot",
             "boot_rollback: version out of range (shipped=%u required=%u max=%u)",
             (uint64_t)shipped, (uint64_t)required,
             (uint64_t)BOOT_SECURITY_VERSION_MAX);
        if (out_error != (enum boot_rollback_error *)0)
            *out_error = BOOT_ROLLBACK_ERR_VERSION_OOR;
        return BOOT_FATAL;
    }

    /* Telemetry: if either refusal flag is set when the kernel is
     * running, something is wrong. The bootloader should have halted
     * before we got here. Log + keep booting -- we do NOT halt
     * because the kernel running is itself proof that the bootloader
     * reached the kernel_jump; the flag is stale state from an
     * earlier image in low memory. */
    if ((flags & BOOT_FLAG_ROLLBACK_REFUSAL) != 0u) {
        klog(LOG_WARN, "boot",
             "boot_rollback: REFUSAL flag set but kernel running; "
             "stale pre-jump state in low memory (shipped=%u required=%u)",
             (uint64_t)shipped, (uint64_t)required);
    }
    if ((flags & BOOT_FLAG_ROLLBACK_READ_FAILED) != 0u) {
        klog(LOG_WARN, "boot",
             "boot_rollback: READ_FAILED flag set but kernel running; "
             "stale pre-jump state in low memory");
    }

    klog(LOG_INFO, "boot",
         "boot_rollback: security version shipped=%u required=%u (no downgrade)",
         (uint64_t)shipped, (uint64_t)required);

    return BOOT_OK;
}

int boot_rollback_should_raise(const struct boot_info *info,
                               int opt_in,
                               uint32_t *new_value)
{
    if (info == (const struct boot_info *)0)
        return 0;
    if (!opt_in)
        return 0;

    uint32_t shipped  = info->os_loader_security_version;
    uint32_t required = info->required_security_version;

    /* Never decrease: shipped must strictly exceed required. */
    if (shipped <= required)
        return 0;

    /* Out-of-range guard: never propagate a value the validator would
     * reject on the next boot. */
    if (shipped > BOOT_SECURITY_VERSION_MAX)
        return 0;

    if (new_value != (uint32_t *)0)
        *new_value = shipped;
    return 1;
}
