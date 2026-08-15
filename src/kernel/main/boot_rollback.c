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
 *     the first stable frame has rendered. There is no timer fallback:
 *     first_frame forces a full composite on iteration 1, so either the
 *     first-frame path fires or the compositor hung before it, and a
 *     hung compositor withholding the raise IS the safety goal. The
 *     sole production caller is compositor.c's first_frame branch.
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
#include "kernel/uefi_vars.h"
#include "kernel/sched/workqueue.h"
#include "kernel/sched/spinlock.h"

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

    /* Rule 3: _rollback_pad is reserved-zero. The field matrix says
     * "reserved for future anti-rollback policy word". A nonzero
     * producer is forging policy data the kernel does not interpret,
     * which is exactly the silent ABI drift the validator must catch
     * at the anti-rollback trust boundary. (Codex 2026-04-30 finding.) */
    if (info->_rollback_pad != 0u) {
        klog(LOG_ERROR, "boot",
             "boot_rollback: _rollback_pad must be zero, got 0x%x (forged policy word?)",
             (uint64_t)info->_rollback_pad);
        if (out_error != (enum boot_rollback_error *)0)
            *out_error = BOOT_ROLLBACK_ERR_RESERVED_NONZERO;
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
 * silently suppressed any future retry. The compositor first-frame
 * path requests one raise per steady boot via
 * boot_rollback_request_raise(); the worker (or sync fallback) runs
 * raise_if_steady() and rolls back s_attempted on transient failure,
 * letting a future request retry. s_attempted stays 1 (do not
 * reconsider policy) on success or opt-out; stays 0 on transient
 * failure. s_raised stays 0 until NVRAM actually changes.
 *
 * SMP note: as of the deferred-raise refactor, s_steady / s_attempted
 * / s_raised / s_enqueued are accessed by both the compositor caller
 * and the sys_wq worker (different kernel tasks, possibly different
 * CPUs). All four are read/written via __atomic_* with explicit
 * memory order: mark_steady stores RELEASE; the worker loads
 * ACQUIRE. s_attempted and s_enqueued use CAS so the worker-vs-
 * fallback race resolves to a single SetVariable attempt. */
/* SMP synchronization: the worker thread (boot_rollback_request_raise
 * dispatches the raise to sys_wq) reads/writes these flags from a
 * different kernel task than the compositor first-frame caller. Use
 * __atomic_load_n / __atomic_store_n with explicit memory order so
 * the compositor s mark_steady write happens-before the worker s
 * raise_if_steady read. The previous "single-thread by construction"
 * justification no longer holds once the slow path is deferred. */
static volatile int s_steady    = 0;  /* compositor publishes; worker reads */
static volatile int s_attempted = 0;  /* claimed under s_state_lock */
static volatile int s_raised    = 0;  /* worker writes; readers see latched advance */
static volatile int s_enqueued  = 0;  /* request_raise dedup; updated under s_state_lock */
/* s_state_lock makes the {s_attempted, s_enqueued} pair atomic with
 * respect to concurrent callers. Codex 2026-04-30 step-13 M2 finding:
 * a transient SetVariable failure used to clear the two flags via two
 * independent atomic stores, exposing a window where a concurrent
 * request_raise saw s_enqueued=1 + s_attempted=0 and dropped the
 * retry. With the lock, the failure path clears both inside the same
 * critical region; request_raise reads s_enqueued under the same
 * lock. s_steady and s_raised stay outside the lock (acquire/release
 * atomics) because their state machines do not interlock with the
 * request/attempt pair. */
static DEFINE_SPINLOCK(s_state_lock);

void boot_rollback_mark_steady(void)
{
    /* Release: the compositor s "first frame painted" evidence must be
     * visible to the worker thread that consumes s_steady via
     * raise_if_steady. */
    __atomic_store_n(&s_steady, 1, __ATOMIC_RELEASE);
    /* Publish the latch on serial. The raise runs asynchronously on
     * sys_wq, so its success log says WHETHER the floor moved but never
     * WHEN the gate opened, and those are different questions. Without
     * this line the only evidence of ordering is wall-clock timing,
     * which cannot separate "raised after the first frame" from "raised
     * before it and logged late". The rollback fixture harness asserts
     * steady -> enqueued -> raised as an ORDER; on bare metal this is
     * also the marker that says the compositor reached a real frame. */
    klog_unrated(LOG_INFO, "boot", "anti-rollback: compositor steady latched");
}

int boot_rollback_is_steady(void)
{
    return __atomic_load_n(&s_steady, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int boot_rollback_was_raised(void)
{
    return __atomic_load_n(&s_raised, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int boot_rollback_raise_if_steady(void)
{
    if (!__atomic_load_n(&s_steady, __ATOMIC_ACQUIRE))
        return 0;

    /* Claim the single raise attempt under the state lock so the
     * load + set is atomic with respect to concurrent callers and
     * with respect to the failure-path cleanup below. */
    {
        uint64_t flags;
        spin_lock_irqsave(&s_state_lock, &flags);
        if (s_attempted) {
            spin_unlock_irqrestore(&s_state_lock, flags);
            return 0;
        }
        s_attempted = 1;
        spin_unlock_irqrestore(&s_state_lock, flags);
    }

    uint32_t new_value = 0;
    int opt_in = (int)g_boot_info.config.anti_rollback_raise;
    if (!boot_rollback_should_raise(&g_boot_info, opt_in, &new_value)) {
        /* Stable policy: opt-out OR shipped <= required. s_attempted
         * stays latched; future calls early-out at the lock check. */
        return 0;
    }

    /* IPOSRequiredSecVersion namespace + attributes come from the
     * canonical definitions in kernel/uefi_vars.h, NOT a local copy.
     * The bootloader reads this variable back pre-jump through its own
     * mirror of the same GUID (bootx64.c g_impossible_os_guid) and
     * fail-closes when the attribute set does not carry NV|BS|RT, so a
     * private duplicate here could drift out of the pair silently.
     * Name is UCS-2 NUL-terminated. */
    static const struct boot_uefi_guid ipos_guid =
        IMPOSSIBLE_OS_VENDOR_GUID_INIT;
    static const uint16_t req_name[] = {
        'I','P','O','S','R','e','q','u','i','r','e','d',
        'S','e','c','V','e','r','s','i','o','n', 0
    };

    uint64_t status = uefi_set_variable(
        &ipos_guid, req_name,
        UEFI_VAR_NV_BOOT_RUNTIME,
        sizeof(new_value), &new_value);

    if (status == 0) {
        /* Confirmed NVRAM write. s_attempted is already latched
         * inside the state lock above; publish s_raised with release
         * so was_raised() observers see a coherent advance. */
        __atomic_store_n(&s_raised, 1, __ATOMIC_RELEASE);
        klog_unrated(LOG_INFO, "boot",
                     "anti-rollback: raised IPOSRequiredSecVersion to %u (steady)",
                     (uint64_t)new_value);
        return 1;
    }

    /* Transient UEFI Runtime Services failure. Roll back s_attempted
     * AND s_enqueued atomically under the state lock so a concurrent
     * request_raise caller cannot observe a half-cleared state. */
    {
        uint64_t flags;
        spin_lock_irqsave(&s_state_lock, &flags);
        s_attempted = 0;
        s_enqueued  = 0;
        spin_unlock_irqrestore(&s_state_lock, flags);
    }
    klog(LOG_WARN, "boot",
         "anti-rollback: SetVariable failed (0x%lx); counter not advanced",
         (uint64_t)status);
    return 0;
}

/* Worker callback for sys_wq -- runs the slow-path raise on the
 * deferred-work thread instead of the compositor first-frame
 * thread. The actual NVRAM write blocks for 10-100 ms on real
 * firmware; offloading it removes that hitch from presentation. */
static void boot_rollback_raise_worker(void *arg)
{
    (void)arg;
    /* raise_if_steady handles both the success latch (leaves
     * s_enqueued=1) and the transient-failure rollback (clears
     * s_enqueued=0 atomically with s_attempted) inside its own
     * critical region. Nothing to do in the worker epilogue.
     * (Codex 2026-04-30 step-13 M1 follow-up: collapsing the race
     * window required moving the s_enqueued cleanup into the same
     * place that rolls back s_attempted.) */
    (void)boot_rollback_raise_if_steady();
}

int boot_rollback_request_raise(void)
{
    /* Precondition: caller must have signalled steady first. Without
     * this guard a pre-steady request would latch s_enqueued, the
     * worker/sync-fallback would early-out at !s_steady inside
     * raise_if_steady, and a later post-steady caller would observe
     * s_enqueued=1 and never advance the counter -- permanently
     * stranding the anti-rollback floor for that boot. The compositor
     * caller orders mark_steady before request_raise, but this is a
     * public helper exposed in boot_info.h so we enforce the
     * precondition here. */
    if (!__atomic_load_n(&s_steady, __ATOMIC_ACQUIRE))
        return 0;

    /* Idempotent: claim the enqueue slot under the state lock so a
     * concurrent failure-path cleanup cannot expose a half-cleared
     * {s_attempted, s_enqueued} pair. */
    {
        uint64_t flags;
        spin_lock_irqsave(&s_state_lock, &flags);
        if (s_enqueued) {
            spin_unlock_irqrestore(&s_state_lock, flags);
            return 0;
        }
        s_enqueued = 1;
        spin_unlock_irqrestore(&s_state_lock, flags);
    }

    /* Second half of the ordering evidence described in mark_steady:
     * this fires exactly once per boot, on the caller's thread, at the
     * moment the raise is claimed -- before either the workqueue or the
     * synchronous fallback runs. A regression that requests the raise
     * from anywhere other than the compositor first-frame path shows up
     * here immediately, even when the write itself lands later or never. */
    klog_unrated(LOG_INFO, "boot", "anti-rollback: raise request enqueued");

    /* Try the deferred path first. sys_wq might be NULL during early
     * boot or if creation failed; the workqueue pool can also fill
     * under load. */
    if (sys_wq != (workqueue_t *)0
        && workqueue_enqueue(sys_wq, boot_rollback_raise_worker,
                             (void *)0) != 0) {
        return 1;
    }

    /* Fallback: run synchronously on the caller s thread. The
     * compositor first-frame is one-shot; silently dropping the only
     * raise attempt would violate the section test checkpoint
     * "a boot that reaches first stable compositor frame MUST advance
     * the counter exactly once when the opt-in policy is set". The
     * stall is unfortunate but bounded (single SetVariable call, no
     * loop). */
    klog(LOG_WARN, "boot",
         "anti-rollback: workqueue unavailable (sys_wq=%s); running "
         "raise synchronously on caller",
         (uint64_t)(uintptr_t)(sys_wq ? "full" : "null"));
    /* raise_if_steady manages s_enqueued cleanup atomically with
     * s_attempted on transient failure; nothing to do here. */
    (void)boot_rollback_raise_if_steady();
    return 1;
}

/* Test-only reset. Tests that drive the steady gate need a way to
 * rewind state between cases without rebooting. NOT for production use. */
#ifdef KERNEL_TESTS
/* KERNEL_TESTS-gated: every caller is a src/kernel/test/ TU, which the
 * release flavor (`make KERNEL_TESTS=off`) prunes. Without the guard this
 * non-static definition survives into a release kernel.map as gadget
 * surface with no legitimate production caller. */
void boot_rollback_reset_for_test(void)
{
    /* Atomic stores to mirror the production paths -- tests that
     * observe these from a different thread must see a coherent
     * reset. */
    __atomic_store_n(&s_steady,    0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_attempted, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_raised,    0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_enqueued,  0, __ATOMIC_RELEASE);
}
#endif /* KERNEL_TESTS */
