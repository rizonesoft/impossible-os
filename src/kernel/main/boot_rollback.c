/* ============================================================================
 * boot_rollback.c -- anti-rollback + security-version helpers.
 *
 * Functions:
 *   - boot_rollback_validate() -- sanity check on the boot_info fields
 *     the bootloader wrote (unknown flag bits, out-of-range versions).
 *     Runs in Phase 0 after boot_decision_validate. The downgrade
 *     refusal itself is enforced PRE-JUMP by the bootloader; if we
 *     see the kernel running, shipped >= required by construction.
 *   - boot_rollback_should_raise() -- decision helper. Returns 1 when
 *     the opt-in policy is set AND shipped > required.
 *   - boot_rollback_mark_steady() -- compositor-side setter called once
 *     the first stable frame has rendered (or a timer fallback fires).
 *     Records that the boot reached user-visible steady state.
 *   - boot_rollback_raise_if_steady() -- one-shot NVRAM write of
 *     IPOSRequiredSecVersion. No-op until mark_steady has fired;
 *     subsequent calls after the first successful raise are no-ops.
 *     This is the "truly-steady" gate: if the compositor crashes
 *     before its first frame, the rollback floor never advances and
 *     the next boot can still fall back to the same image.
 *
 * Logs at LOG_ERROR on validator reject; caller (boot_hw.c) converts
 * BOOT_FATAL to boot_halt(). Same stance as boot_caps.c and
 * boot_decision.c.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/uefi_runtime.h"

extern struct boot_info g_boot_info;

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

/* Compositor-steady gate: defense against advancing the rollback floor
 * on a boot that never reached user-visible steady state. Without this,
 * a crash between POST16_BOOT_OK and the first composited frame would
 * strand the machine on a broken image (next boot would refuse to fall
 * back to the previously-working image).
 *
 * Latch split:
 *   s_steady    -- set once by compositor mark_steady.
 *   s_attempted -- set once the policy has been evaluated. On opt-out
 *                  (no raise needed) this latches immediately because
 *                  the decision is stable for the remainder of the
 *                  boot. Early-out guard; prevents repeated policy
 *                  re-evaluation on every compositor frame.
 *   s_raised    -- set ONLY on confirmed NVRAM write success. Keeps
 *                  the getter honest: is_raised() means the counter
 *                  actually advanced.
 *
 * Why split: a transient uefi_set_variable() failure used to latch
 * s_raised alongside the attempt, which over-stated success and
 * silently suppressed any future retry a caller might wire up. The
 * compositor invokes raise_if_steady() exactly once per boot today
 * (first-frame path), so a SetVariable miss simply drops this boot's
 * advance; the next boot tries again. If a future caller wraps the
 * helper in a retry loop, this split lets it work -- s_attempted
 * stays 1 (do not reconsider policy) while s_raised stays 0 until
 * NVRAM actually changes.
 *
 * Race note: all three are written from the compositor thread and
 * read from the same thread. Plain volatile is fine today; if a
 * cross-CPU caller ever appears the stores become __atomic_store_n
 * with __ATOMIC_RELEASE + matching ACQUIRE load. */
static volatile int s_steady    = 0;
static volatile int s_attempted = 0;
static volatile int s_raised    = 0;

void boot_rollback_mark_steady(void)
{
    s_steady = 1;
}

int boot_rollback_is_steady(void)
{
    return s_steady ? 1 : 0;
}

int boot_rollback_was_raised(void)
{
    return s_raised ? 1 : 0;
}

int boot_rollback_raise_if_steady(void)
{
    if (!s_steady)
        return 0;
    if (s_attempted)
        return 0;

    uint32_t new_value = 0;
    int opt_in = (int)g_boot_info.config.anti_rollback_raise;
    if (!boot_rollback_should_raise(&g_boot_info, opt_in, &new_value)) {
        /* Stable policy: opt-out OR shipped <= required. Latch so
         * the helper does not re-evaluate on every compositor wake. */
        s_attempted = 1;
        return 0;
    }

    /* IPOSRequiredSecVersion GUID; name is UCS-2 NUL-terminated.
     * Attributes: NV | BS | RT = 0x7. */
    static struct boot_uefi_guid ipos_guid = {
        0x6f35d3a4, 0xc0e6, 0x4a82,
        { 0xb5, 0xd8, 0x7c, 0x9d, 0x2e, 0x4f, 0x8a, 0x13 }
    };
    static const uint16_t req_name[] = {
        'I','P','O','S','R','e','q','u','i','r','e','d',
        'S','e','c','V','e','r','s','i','o','n', 0
    };

    uint64_t status = uefi_set_variable(
        &ipos_guid, req_name,
        0x7u,
        sizeof(new_value), &new_value);

    if (status == 0) {
        /* Confirmed NVRAM write. Latch both: attempted guards the
         * early-out, raised keeps the semantic honest for any
         * future probe. */
        s_attempted = 1;
        s_raised    = 1;
        klog(LOG_INFO, "boot",
             "anti-rollback: raised IPOSRequiredSecVersion to %u (steady)",
             (uint64_t)new_value);
        return 1;
    }

    /* Transient UEFI Runtime Services failure. Do NOT latch
     * s_attempted -- leave the door open for a retry if a caller
     * ever adds one. s_raised stays clear: is_raised() must only
     * report confirmed advance. */
    klog(LOG_WARN, "boot",
         "anti-rollback: SetVariable failed (0x%lx); counter not advanced",
         (uint64_t)status);
    return 0;
}

/* Test-only reset. Tests that drive the steady gate need a way to
 * rewind state between cases without rebooting. NOT for production use. */
void boot_rollback_reset_for_test(void)
{
    s_steady    = 0;
    s_attempted = 0;
    s_raised    = 0;
}
