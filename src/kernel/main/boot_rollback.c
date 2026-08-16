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
 *   - boot_rollback_raise_if_steady() -- one NVRAM write attempt of
 *     IPOSRequiredSecVersion. No-op until mark_steady has fired;
 *     subsequent calls after the first successful raise are no-ops.
 *     This is the "truly-steady" gate: if the compositor crashes
 *     before its first frame, the rollback floor never advances and
 *     the next boot can still fall back to the same image. A
 *     transient firmware failure hands the remaining attempts to a
 *     bounded retry chain carried by a kworker delayed-work entry; a
 *     terminal firmware status, or the attempt cap, ends the boot's
 *     attempts with a single LOG_ERROR naming the status.
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
#include "kernel/sched/kworker.h"
#include "kernel/timer.h"

extern struct boot_info g_boot_info;

/* ---- Bounded-retry policy for a transient rollback-floor write ----
 *
 * The steady gate fires exactly once per boot (the compositor first
 * frame), so a single failed SetVariable used to end the story: the
 * floor stayed at its old value and the only evidence was one
 * LOG_WARN. An operator who set anti_rollback_raise to retire a
 * vulnerable image got a machine that silently did not retire it.
 *
 * ATTEMPT CAP. Four attempts total (one initial plus three retries),
 * so a firmware that fails every write cannot keep attempting for the
 * life of the boot. Note what this does NOT bound: the registration
 * is released by the tick that observes the terminal outcome, so a
 * stopped clock or a hung peer callback -- neither of which this code
 * can recover from, see BACKOFF -- prevents that tick and therefore
 * holds the slot. The cap bounds WRITES, not registration lifetime.
 *
 * BACKOFF. 25 ms, then 50 ms, then 100 ms between attempts. These are
 * MINIMUM spacings with NO upper bound on delivery. kworker has no
 * wakeup on register, so its loop notices a freshly armed entry only
 * on its next pass; KWORKER_MAX_SLEEP_MS bounds that IDLE interval at
 * ~1 s, but only while the clock advances and no peer callback is
 * occupying the single serial worker. A retry that lands late still
 * lands inside the same boot, and advancing the floor before the
 * machine next restarts is the whole user-visible contract here. Do
 * not read the ladder as a timing guarantee. The chain also inherits
 * the carrier's contract that callbacks are bounded and non-hanging
 * (kernel/sched/kworker.h): a peer callback that hangs forever stalls
 * every registered monitor, this one included, and nothing here can
 * recover from that.
 *
 * CARRIER. The retries ride the kworker delayed-work primitive, not a
 * spawned task and not sys_wq. A task would work exactly once: task
 * slots are handed out monotonically (src/kernel/sched/task.c takes
 * pid = num_tasks and refuses at TASK_MAX) and are reclaimed only
 * through task_waitpid, so a fire-and-forget retry task would hold
 * one of 32 slots plus its guarded stack for the life of the boot.
 * sys_wq is worse: it has a single worker, so a multi-attempt loop
 * there would block every other system work item -- including the A/B
 * mark-good the compositor enqueues immediately after this raise --
 * across the whole backoff. kworker registrations are reclaimed on
 * unregister, its callbacks run at PASSIVE_LEVEL and may call
 * firmware, and a typical SetVariable sits inside its
 * KWORKER_CALLBACK_WARN_MS budget -- typical, not guaranteed: nothing
 * here bounds how long firmware may take.
 *
 * LOCK ORDER. kworker_register / kworker_unregister are NEVER called
 * while holding s_state_lock: the token is read or published inside
 * the critical region and the kworker call is made outside it. */
/* BOOT_ROLLBACK_MAX_ATTEMPTS is declared in kernel/boot_info.h: the
 * cap is part of the documented behavior of the raise path, and the
 * fault-injected tests assert the exact write count against it. */
#define BOOT_ROLLBACK_BACKOFF_BASE_MS   25u
#define BOOT_ROLLBACK_BACKOFF_MAX_MS   100u
#define BOOT_ROLLBACK_NS_PER_MS         1000000ULL

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
 * ONE attempt through raise_if_steady() and, on a transient failure,
 * releases s_attempted while KEEPING s_enqueued so the bounded retry
 * chain owns the single remaining claim. s_attempted stays 1 (do not
 * reconsider policy) on success, opt-out, or once the chain has given
 * up. s_raised stays 0 until NVRAM actually changes.
 *
 * SMP note. Three kernel tasks touch this state -- the compositor
 * first-frame caller, the single sys_wq worker, and the kworker
 * thread running the retry chain -- so the split matters:
 *   s_steady, s_raised      PUBLICATION flags, __atomic_* with
 *                           explicit order (mark_steady stores
 *                           RELEASE, readers load ACQUIRE). They do
 *                           not interlock with anything else.
 *   everything else         guarded by s_state_lock, NOT atomics and
 *                           NOT CAS: s_attempted, s_enqueued,
 *                           s_attempts, s_terminal, s_last_status,
 *                           s_retry_spawned, s_retry_token,
 *                           s_retry_period_ms, s_next_attempt_ns,
 *                           s_arm_epoch. They are read and mutated
 *                           together, so every decision that reads
 *                           one takes the lock.
 * A transient failure releases ONLY s_attempted and KEEPS s_enqueued
 * (see the failure region below); do not "simplify" that to clearing
 * both, which is what the pre-retry code did and what would let a
 * second requester start a competing write. */
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
/* Retry bookkeeping. Both live under s_state_lock with the
 * {s_attempted, s_enqueued} pair because every decision they drive is
 * taken in the same critical region that claims or releases an
 * attempt: s_attempts is the cap counter (how many SetVariable calls
 * this boot has FAILED -- a successful write ends the chain and never
 * bumps it, so a chain whose last attempt lands reads one lower than
 * the number of writes issued; the cap arithmetic wants the failure
 * count, which is why it is counted this way) and s_terminal latches the moment the
 * chain gives up, so the one LOG_ERROR can never be emitted twice and
 * no later caller can start a fresh chain. */
static volatile uint32_t s_attempts = 0;
static volatile int      s_terminal = 0;
/* Status of the most recent failed write, so a give-up that happens
 * AWAY from the write site (no carrier could be armed) still names
 * the firmware error the operator needs rather than a bare zero. */
static volatile uint64_t s_last_status = 0;
/* Set under s_state_lock once the retry chain has been scheduled.
 * Belt-and-braces against a second chain: the claim model already
 * prevents one (s_enqueued stays latched for the life of the chain),
 * so a second schedule would be a logic error, not a race. */
static volatile int      s_retry_spawned = 0;
/* s_retry_token is the ONE registration that carries the whole chain,
 * armed at the fixed BOOT_ROLLBACK_BACKOFF_BASE_MS cadence and never
 * re-registered; -1 means none is published, and a tick that observes
 * -1 does nothing and waits for the next period rather than acting on
 * an unpublished token. s_retry_period_ms is NOT that cadence: it is
 * the current logical backoff STEP, moved 25 -> 50 -> 100 solely to
 * compute s_next_attempt_ns. Do not reintroduce unregister-then-
 * register to change the cadence -- that needs a second free slot
 * while the running callback still holds the first, and fails the
 * chain when none exists. Both under s_state_lock. */
static volatile int      s_retry_token     = -1;
static volatile uint32_t s_retry_period_ms = 0;
/* Absolute deadline of the NEXT attempt. The registration is armed
 * once for the whole chain at a fixed polling cadence and the backoff
 * is enforced here, so a tick that arrives early simply returns. That
 * is what keeps the chain to ONE slot: kworker_register only accepts
 * a slot that is neither active nor running, and the running bit of
 * the slot executing the callback is not cleared until the callback
 * returns, so an unregister-then-register re-arm from inside the tick
 * would transiently need a SECOND free slot -- and fail the whole
 * chain when none exists. */
static volatile uint64_t s_next_attempt_ns = 0;
/* Arming epoch, bumped whenever the chain state is rewound. The
 * registration is made OUTSIDE s_state_lock (the carrier must not be
 * called under it) and published afterwards, so there is a window in
 * which a live kworker entry exists that s_retry_token does not name.
 * A rewind landing in that window would leave the entry orphaned --
 * unreachable to cancel and free to tick against freshly reset state.
 * The arming path therefore captures the epoch before registering and
 * publishes only if it still matches; otherwise it unregisters the
 * entry it just created. Guarded by s_state_lock. */
static volatile uint32_t s_arm_epoch = 0;
/* s_state_lock makes the {s_attempted, s_enqueued} pair atomic with
 * respect to concurrent callers. Codex 2026-04-30 step-13 M2 finding:
 * a transient SetVariable failure used to clear the two flags via two
 * independent atomic stores, exposing a window where a concurrent
 * request_raise saw s_enqueued=1 + s_attempted=0 and dropped the
 * retry. The lock is what closed that window; what the failure path
 * does INSIDE it changed with the bounded retry, which now releases
 * s_attempted and deliberately RETAINS s_enqueued so the chain keeps
 * the single claim (the older "clears both" behavior would hand the
 * raise to whoever asked next). request_raise reads s_enqueued under
 * the same lock. s_steady and s_raised stay outside it
 * (acquire/release atomics) because their state machines do not
 * interlock with the request/attempt pair. */
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

/* The NVRAM write itself. IPOSRequiredSecVersion namespace +
 * attributes come from the canonical definitions in
 * kernel/uefi_vars.h, NOT a local copy. The bootloader reads this
 * variable back pre-jump through its own mirror of the same GUID
 * (bootx64.c g_impossible_os_guid) and fail-closes when the attribute
 * set does not carry NV|BS|RT, so a private duplicate here could
 * drift out of the pair silently. Name is UCS-2 NUL-terminated. */
static uint64_t boot_rollback_write_nvram(uint32_t value)
{
    static const struct boot_uefi_guid ipos_guid =
        IMPOSSIBLE_OS_VENDOR_GUID_INIT;
    static const uint16_t req_name[] = {
        'I','P','O','S','R','e','q','u','i','r','e','d',
        'S','e','c','V','e','r','s','i','o','n', 0
    };

    return uefi_set_variable(&ipos_guid, req_name,
                             UEFI_VAR_NV_BOOT_RUNTIME,
                             sizeof(value), &value);
}

#ifdef KERNEL_TESTS
/* Fault-injection seam. The retry state machine is otherwise
 * unreachable from a unit test: no harness can make real firmware
 * fail one SetVariable and pass the next, which is exactly the case
 * this section exists to handle. KERNEL_TESTS-gated, so the PRUNED flavor
 * (`KERNEL_TESTS=off`) calls the writer directly with no indirect call
 * and no writable code pointer behind the anti-rollback write. Note
 * the default build is KERNEL_TESTS=on (Makefile), so an image built
 * without that override DOES carry the pointer and its setter; the
 * hardening applies to the pruned flavor, not to every build.
 * s_inline_retry runs the chain on the caller instead of registering
 * a kworker entry, so a test observes the whole state machine
 * deterministically rather than racing a background thread;
 * s_sched_fail forces the registration to fail so the fail-closed
 * latch is exercised rather than assumed. s_terminal_reports and
 * s_terminal_status record the single give-up report, so a test can
 * assert the exactly-once telemetry the section requires without
 * scraping the log. */
static boot_rollback_writer_fn s_writer          = boot_rollback_write_nvram;
static int                     s_inline_retry    = 0;
static int                     s_sched_fail      = 0;
static uint32_t                s_terminal_reports = 0;
static uint64_t                s_terminal_status  = 0;
#define BOOT_ROLLBACK_WRITE(v)      (s_writer((v)))
#else
#define BOOT_ROLLBACK_WRITE(v)      boot_rollback_write_nvram((v))
#endif

/* Backoff progression. Pure: a function of the current period only,
 * never of the injected test state, so the 25 -> 50 -> 100 -> 100
 * ladder is directly assertable. The clamp also absorbs a doubling
 * that would wrap. */
static uint32_t boot_rollback_next_backoff_ms(uint32_t current_ms)
{
    uint32_t next;

    if (current_ms == 0u)
        return BOOT_ROLLBACK_BACKOFF_BASE_MS;

    next = current_ms * 2u;
    if (next < current_ms || next > BOOT_ROLLBACK_BACKOFF_MAX_MS)
        next = BOOT_ROLLBACK_BACKOFF_MAX_MS;
    return next;
}

/* The single give-up report. Called exactly once per boot because
 * every path into it first latches s_terminal inside the state lock.
 * Naming the raw status is what lets an operator separate "policy
 * declined" (opt-out or shipped <= required, which reports nothing
 * here) from "policy failed", and separate a firmware refusal from a
 * write that kept failing. */
static void boot_rollback_report_terminal(uint64_t status, uint32_t attempts,
                                          const char *reason)
{
#ifdef KERNEL_TESTS
    /* Under the same lock as the rest of the retry state. Exactly-once
     * is already enforced by the s_terminal latch, so there is no live
     * race to close -- but this is the file's only shared mutable
     * state, and leaving one field outside the discipline invites the
     * next reader to conclude the discipline is optional. The klog
     * below stays outside the region. */
    {
        uint64_t rflags;
        spin_lock_irqsave(&s_state_lock, &rflags);
        s_terminal_reports++;
        s_terminal_status = status;
        spin_unlock_irqrestore(&s_state_lock, rflags);
    }
#endif
    klog_unrated(LOG_ERROR, "boot",
                 "anti-rollback: floor NOT advanced -- SetVariable status 0x%lx "
                 "after %u attempt(s) (%s)",
                 status, (uint64_t)attempts, (uint64_t)(uintptr_t)reason);
}

/* Fail-closed retry allowlist. Of the SetVariable error returns, only
 * these two can plausibly clear on a later attempt: a firmware NVRAM
 * controller hiccup, or a transient shortage of variable-store
 * working memory. UEFI_INVALID_PARAMETER, UEFI_UNSUPPORTED,
 * UEFI_WRITE_PROTECTED and UEFI_SECURITY_VIOLATION are decisions the
 * firmware will make identically every time, and an UNKNOWN status is
 * not demonstrably transient, so both are terminal. Retrying a
 * permanent refusal would spend three more blocking firmware calls
 * and then report a platform refusal as retry exhaustion, which is
 * precisely the distinction the operator needs to keep. */
static int boot_rollback_status_is_transient(uint64_t status)
{
    if (status == UEFI_DEVICE_ERROR)
        return 1;
    if (status == UEFI_OUT_OF_RESOURCES)
        return 1;
    return 0;
}

enum boot_rollback_attempt {
    BOOT_ROLLBACK_ATTEMPT_RAISED = 0,  /* NVRAM confirmed advanced */
    BOOT_ROLLBACK_ATTEMPT_SKIPPED,     /* not steady / claimed / policy / done */
    BOOT_ROLLBACK_ATTEMPT_RETRYABLE,   /* transient failure, attempts remain */
    BOOT_ROLLBACK_ATTEMPT_GAVE_UP      /* terminal status or cap reached */
};

/* One attempt. Owns all of the claim bookkeeping and none of the
 * scheduling: whoever wants a retry asks for one after seeing
 * RETRYABLE, which keeps the state machine testable in isolation and
 * keeps a retry from ever nesting inside an attempt. */
static enum boot_rollback_attempt boot_rollback_attempt_raise(void)
{
    if (!__atomic_load_n(&s_steady, __ATOMIC_ACQUIRE))
        return BOOT_ROLLBACK_ATTEMPT_SKIPPED;

    /* Claim the attempt under the state lock so the load + set is
     * atomic with respect to concurrent callers and with respect to
     * the failure-path release below. s_terminal is checked in the
     * same region: once the chain has given up, nothing re-arms it
     * for the remainder of the boot. */
    {
        uint64_t flags;
        spin_lock_irqsave(&s_state_lock, &flags);
        if (s_attempted || s_terminal) {
            spin_unlock_irqrestore(&s_state_lock, flags);
            return BOOT_ROLLBACK_ATTEMPT_SKIPPED;
        }
        s_attempted = 1;
        spin_unlock_irqrestore(&s_state_lock, flags);
    }

    uint32_t new_value = 0;
    int opt_in = (int)g_boot_info.config.anti_rollback_raise;
    if (!boot_rollback_should_raise(&g_boot_info, opt_in, &new_value)) {
        /* Stable policy: opt-out OR shipped <= required. s_attempted
         * stays latched; future calls early-out at the lock check. */
        return BOOT_ROLLBACK_ATTEMPT_SKIPPED;
    }

    uint64_t status = BOOT_ROLLBACK_WRITE(new_value);

    if (status == 0) {
        /* Confirmed NVRAM write. s_attempted is already latched
         * inside the state lock above; publish s_raised with release
         * so was_raised() observers see a coherent advance. */
        __atomic_store_n(&s_raised, 1, __ATOMIC_RELEASE);
        klog_unrated(LOG_INFO, "boot",
                     "anti-rollback: raised IPOSRequiredSecVersion to %u (steady)",
                     (uint64_t)new_value);
        return BOOT_ROLLBACK_ATTEMPT_RAISED;
    }

    /* Firmware refused the write. Decide inside one critical region
     * whether another attempt is owed, so a concurrent caller can
     * never observe a half-updated {s_attempted, s_enqueued,
     * s_attempts, s_terminal} set.
     *
     * Releasing s_attempted while KEEPING s_enqueued is the whole
     * retry contract: the chain retains exactly one in-flight claim,
     * so the next attempt comes from the retry chain and never from a
     * second requester, and no two writers can be live at once. */
    int      transient = boot_rollback_status_is_transient(status);
    uint32_t attempts;
    int      give_up;
    {
        uint64_t flags;
        spin_lock_irqsave(&s_state_lock, &flags);
        s_attempts++;
        s_last_status = status;
        attempts = s_attempts;
        give_up  = (!transient || attempts >= BOOT_ROLLBACK_MAX_ATTEMPTS);
        if (give_up)
            s_terminal = 1;
        else
            s_attempted = 0;
        spin_unlock_irqrestore(&s_state_lock, flags);
    }

    if (!give_up) {
        klog(LOG_WARN, "boot",
             "anti-rollback: SetVariable failed (0x%lx) on attempt %u of %u; retrying",
             (uint64_t)status, (uint64_t)attempts,
             (uint64_t)BOOT_ROLLBACK_MAX_ATTEMPTS);
        return BOOT_ROLLBACK_ATTEMPT_RETRYABLE;
    }

    boot_rollback_report_terminal(status, attempts,
                                  transient ? "retries exhausted"
                                            : "terminal firmware status");
    return BOOT_ROLLBACK_ATTEMPT_GAVE_UP;
}

static void boot_rollback_retry_tick(void *ctx);

/* Register the retry callback at the fixed base cadence, once for the
 * life of the chain (the backoff is a deadline, not a period; see
 * s_retry_period_ms). Returns the
 * kworker token, or negative when no carrier could be armed.
 * kworker is called with s_state_lock NOT held (see LOCK ORDER). */
static int boot_rollback_arm_retry(uint32_t period_ms)
{
#ifdef KERNEL_TESTS
    if (s_sched_fail)
        return -1;
#endif
    /* A registration only fills a slot: with no worker thread running,
     * the entry would never fire and the claim would sit latched
     * behind a chain that can never advance -- the silent failure this
     * section exists to remove.
     *
     * The probe is kworker_is_started(), a pure query, NOT kworker_init().
     * init is idempotent but it is only CHEAP when the worker already
     * reached STARTED: on a system whose boot-time kworker_init failed,
     * it re-runs task_create or yield-spins to KWORKER_START_YIELD_CAP
     * before returning -1. This path can run on the compositor
     * first-frame thread (the sys_wq-unavailable fallback), where
     * spawning a task and stalling presentation on a startup handshake
     * is exactly the deferred-init hazard the bare-metal rules forbid. */
    if (!kworker_is_started())
        return -1;

    return kworker_register(boot_rollback_retry_tick, (void *)0, period_ms);
}

/* Nothing left to carry the retry. Latch terminal rather than leave
 * the claim held by nobody, so the state stays honest and the
 * operator gets the one error line this failure is owed. */
static void boot_rollback_abandon_retry(void)
{
    uint32_t attempts;
    uint64_t status;
    uint64_t flags;

    spin_lock_irqsave(&s_state_lock, &flags);
    s_attempted   = 1;
    s_terminal    = 1;
    s_retry_token = -1;
    attempts      = s_attempts;
    status        = s_last_status;
    spin_unlock_irqrestore(&s_state_lock, flags);

    boot_rollback_report_terminal(status, attempts,
                                  "no carrier available for the retry");
}

/* Chain finished (raised, policy-skipped, or given up): drop the
 * registration so the slot returns to the pool. Safe from inside the
 * callback -- kworker_unregister marks the slot inactive and does not
 * wait for the in-flight call to drain when called on the worker. */
static void boot_rollback_cancel_retry(void)
{
    int      token;
    uint64_t flags;

    spin_lock_irqsave(&s_state_lock, &flags);
    token         = s_retry_token;
    s_retry_token = -1;
    spin_unlock_irqrestore(&s_state_lock, flags);

    if (token >= 0)
        (void)kworker_unregister(token);
}

/* Push the next attempt out by the next backoff step. The
 * registration is left alone: only the deadline moves. */
static void boot_rollback_defer_next_attempt(void)
{
    uint32_t period;
    uint64_t flags;

    spin_lock_irqsave(&s_state_lock, &flags);
    period            = boot_rollback_next_backoff_ms(s_retry_period_ms);
    s_retry_period_ms = period;
    s_next_attempt_ns = uptime_ns() + (uint64_t)period * BOOT_ROLLBACK_NS_PER_MS;
    spin_unlock_irqrestore(&s_state_lock, flags);
}

/* A poll of the retry deadline, at the carrier's cadence. */
static void boot_rollback_retry_tick(void *ctx)
{
    (void)ctx;

    /* The token is published after kworker_register returns, so a
     * tick can in principle beat the store. Do nothing and take the
     * next period rather than act on a registration this code cannot
     * yet cancel. */
    {
        int      token;
        uint64_t due;
        uint64_t flags;
        spin_lock_irqsave(&s_state_lock, &flags);
        token = s_retry_token;
        due   = s_next_attempt_ns;
        spin_unlock_irqrestore(&s_state_lock, flags);
        if (token < 0)
            return;
        if (uptime_ns() < due)
            return;   /* backoff still running */
    }

    if (boot_rollback_attempt_raise() == BOOT_ROLLBACK_ATTEMPT_RETRYABLE)
        boot_rollback_defer_next_attempt();
    else
        boot_rollback_cancel_retry();
}

#ifdef KERNEL_TESTS
/* Test carrier: run the same attempt loop on the caller with no
 * waiting at all. The production timing belongs to kworker, so a test
 * that drove real periods would be asserting kworker's scheduler
 * rather than this state machine. */
static void boot_rollback_run_chain_inline(void)
{
    while (boot_rollback_attempt_raise() == BOOT_ROLLBACK_ATTEMPT_RETRYABLE)
        ;
}
#endif

static void boot_rollback_schedule_retry(void)
{
    int      arm = 0;
    int      keep;
    int      token;
    uint32_t epoch;
    uint32_t period = BOOT_ROLLBACK_BACKOFF_BASE_MS;
    uint64_t flags;

    spin_lock_irqsave(&s_state_lock, &flags);
    if (!s_retry_spawned) {
        s_retry_spawned = 1;
        arm = 1;
    }
    epoch = s_arm_epoch;
    spin_unlock_irqrestore(&s_state_lock, flags);
    if (!arm)
        return;

#ifdef KERNEL_TESTS
    if (s_inline_retry) {
        boot_rollback_run_chain_inline();
        return;
    }
#endif

    /* Armed ONCE for the life of the chain, at the base cadence. The
     * backoff lives in s_next_attempt_ns, so later steps move the
     * deadline instead of taking a second slot. */
    token = boot_rollback_arm_retry(period);
    if (token < 0) {
        boot_rollback_abandon_retry();
        return;
    }

    /* Publish only if this arming is still the current one. A rewind
     * that landed while kworker_register was running has moved the
     * epoch on, and the entry we just created belongs to nobody -- so
     * we unregister it here rather than leave it ticking. */
    spin_lock_irqsave(&s_state_lock, &flags);
    keep = (epoch == s_arm_epoch) && !s_terminal;
    if (keep) {
        s_retry_token     = token;
        s_retry_period_ms = period;
        s_next_attempt_ns = uptime_ns()
                          + (uint64_t)period * BOOT_ROLLBACK_NS_PER_MS;
    }
    spin_unlock_irqrestore(&s_state_lock, flags);

    if (!keep)
        (void)kworker_unregister(token);
}

int boot_rollback_raise_if_steady(void)
{
    enum boot_rollback_attempt r = boot_rollback_attempt_raise();

    if (r == BOOT_ROLLBACK_ATTEMPT_RETRYABLE)
        boot_rollback_schedule_retry();

    return (r == BOOT_ROLLBACK_ATTEMPT_RAISED) ? 1 : 0;
}

/* Worker callback for sys_wq -- runs the slow-path raise on the
 * deferred-work thread instead of the compositor first-frame
 * thread. The NVRAM write is typically 10-100 ms on real firmware
 * and has no enforced ceiling here (no timeout, no watchdog), which
 * is exactly why it is kept off the presentation path.
 * Exactly ONE attempt runs here: on a transient failure
 * raise_if_steady hands the retry chain to the kworker carrier and
 * returns, so the single sys_wq worker is never held across a
 * backoff. */
static void boot_rollback_raise_worker(void *arg)
{
    (void)arg;

    /* NO carrier-startup handshake here, deliberately. A review round
     * proposed calling kworker_init() on this thread so a worker still
     * in STARTING could be waited for rather than reported
     * unavailable; implementing it produced two worse defects and it
     * was withdrawn.
     *
     * First, kworker_init() can reach task_create(), and this callback
     * runs after userland and preemptive scheduling are live.
     * task_create picks its slot with pid = num_tasks and publishes it
     * by incrementing num_tasks much later (src/kernel/sched/task.c),
     * with no lock spanning the two, so a concurrent NtCreateProcess
     * can select the same slot. Trading a rare unraised security floor
     * for task-table corruption is not a trade worth making, and
     * fixing the admission race belongs to the scheduler, not here.
     *
     * Second, the handshake would run BEFORE the policy check, so on a
     * degraded system every steady boot would pay it even when the
     * policy is off and no write can happen -- stalling the single
     * sys_wq worker, and with it the A/B mark-good queued immediately
     * behind this item.
     *
     * The accepted limitation: arm_retry probes the side-effect-free
     * kworker_is_started(), so a worker still in STARTING reads as
     * unavailable and the chain ends terminal. It ends LOUDLY -- one
     * LOG_ERROR naming the failure -- which is the contract this
     * section actually owes: the floor is raised, or the operator is
     * told it was not. */
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

    /* Idempotent: test-and-latch s_enqueued under the state lock, so
     * exactly one caller per boot takes ownership of the raise. Note
     * the failure path never CLEANS UP this flag -- a transient
     * failure mutates only s_attempted and deliberately retains
     * s_enqueued, so the pair it establishes is {0,1} and the chain
     * keeps the claim. Reading it under the same lock is what makes
     * that hand-off indivisible from a concurrent requester's view. */
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
     * stall is ONE SetVariable call with no loop and no retry inline.
     * That bounds the NUMBER of firmware calls, not their duration:
     * BOOT_ROLLBACK_WRITE enters firmware with no timeout and no
     * watchdog, so degraded firmware can stall this thread for as
     * long as it likes. The 10-100 ms figure quoted elsewhere is a
     * typical observation, not a bound this code enforces. */
    klog(LOG_WARN, "boot",
         "anti-rollback: workqueue unavailable (sys_wq=%s); running "
         "raise synchronously on caller",
         (uint64_t)(uintptr_t)(sys_wq ? "full" : "null"));
    /* On a transient failure raise_if_steady releases s_attempted
     * under s_state_lock and RETAINS s_enqueued, handing the chain its
     * single claim, so the fallback has no state transition of its own
     * to perform. It does NOT clear both flags -- that was the
     * pre-retry behavior. */
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
    /* Latch terminal FIRST, then drop any live registration, and only
     * then rewind.
     *
     * What this actually guarantees, precisely: no tick can PUBLISH a
     * registration, because the tick has no path that registers -- the
     * chain arms exactly once and later steps only move a deadline. So
     * cancel_retry clears the one token that can exist, and nothing can
     * create another behind it.
     *
     * What the s_terminal pre-latch does NOT do, despite an earlier
     * comment here saying so: it does not stop a callback that has
     * ALREADY claimed its attempt. attempt_raise reads s_terminal once,
     * at claim time, and the give-up decision afterwards is computed
     * from the status and the attempt count alone. Such a callback can
     * still return RETRYABLE and push the deadline out. That is
     * harmless -- the deadline it writes belongs to a chain whose token
     * is already -1, and the next tick returns at the token guard --
     * but it is not the mechanism, and claiming it was would be
     * asserting a property nothing enforces. What the pre-latch buys
     * is that no attempt claimed AFTER it can start a fresh write. */
    {
        uint64_t flags;
        spin_lock_irqsave(&s_state_lock, &flags);
        s_terminal = 1;
        spin_unlock_irqrestore(&s_state_lock, flags);
    }
    boot_rollback_cancel_retry();

    /* The lock-owned fields are reset under the lock they live under,
     * so a reset can never interleave with a live claim and leave the
     * retry set half-rewound. s_steady and s_raised stay outside it,
     * matching the production split. */
    {
        uint64_t flags;
        spin_lock_irqsave(&s_state_lock, &flags);
        s_attempted       = 0;
        s_enqueued        = 0;
        s_attempts        = 0;
        s_terminal        = 0;
        s_last_status     = 0;
        s_retry_spawned   = 0;
        s_arm_epoch++;
        s_retry_token     = -1;
        s_retry_period_ms = 0;
        s_next_attempt_ns = 0;
        spin_unlock_irqrestore(&s_state_lock, flags);
    }
    /* Atomic stores to mirror the production paths -- tests that
     * observe these from a different thread must see a coherent
     * reset. */
    __atomic_store_n(&s_steady, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_raised, 0, __ATOMIC_RELEASE);

    /* Fault injection is per-test opt-in: a reset restores the
     * production writer and carrier so a test that forgets to clean
     * up cannot leak an injected failure into the next one. */
    s_writer           = boot_rollback_write_nvram;
    s_inline_retry     = 0;
    s_sched_fail       = 0;
    s_terminal_reports = 0;
    s_terminal_status  = 0;
}

void boot_rollback_set_writer_for_test(boot_rollback_writer_fn fn)
{
    s_writer = (fn != (boot_rollback_writer_fn)0) ? fn
                                                  : boot_rollback_write_nvram;
}

void boot_rollback_set_inline_retry_for_test(int inline_retry)
{
    s_inline_retry = inline_retry ? 1 : 0;
}

void boot_rollback_set_sched_fail_for_test(int fail)
{
    s_sched_fail = fail ? 1 : 0;
}

uint32_t boot_rollback_attempts_for_test(void)
{
    uint32_t n;
    uint64_t flags;

    spin_lock_irqsave(&s_state_lock, &flags);
    n = s_attempts;
    spin_unlock_irqrestore(&s_state_lock, flags);
    return n;
}

uint32_t boot_rollback_terminal_reports_for_test(void)
{
    uint32_t n;
    uint64_t flags;

    spin_lock_irqsave(&s_state_lock, &flags);
    n = s_terminal_reports;
    spin_unlock_irqrestore(&s_state_lock, flags);
    return n;
}

uint64_t boot_rollback_terminal_status_for_test(void)
{
    uint64_t st;
    uint64_t flags;

    spin_lock_irqsave(&s_state_lock, &flags);
    st = s_terminal_status;
    spin_unlock_irqrestore(&s_state_lock, flags);
    return st;
}

uint32_t boot_rollback_next_backoff_for_test(uint32_t current_ms)
{
    return boot_rollback_next_backoff_ms(current_ms);
}
#endif /* KERNEL_TESTS */
