/*
 * power_callback.h -- Driver power callbacks and sleep/resume ordering
 *
 * The registry a system sleep transition walks to tell drivers to quiesce, and
 * walks again in the opposite order to tell them to come back.
 * Owner: 02-kernel-core/TODO-26-power-management.md section 9.
 *
 * TWO LAYERS, DELIBERATELY SEPARATE
 *
 *   pm_cb_table_*()   Pure table mechanics over a caller-supplied table. No
 *                     globals, no context policy, injectable clock. This is
 *                     what the unit tests drive.
 *   pm_*()            The production singleton. Enforces the context contract
 *                     below and logs. This is what drivers and the sleep path
 *                     call.
 *
 * The split is not stylistic. Driver registration happens during Phase 1/2
 * boot, and the in-kernel test runner does not run until Phase 3
 * (boot_tests_run() in src/kernel/main/boot_desktop.c), so by the time a test
 * executes, the production registry already holds real driver callbacks. A
 * test that notified the singleton would drive live storage and USB hardware
 * into a low-power state during an ordinary test boot, and a test that filled
 * the singleton to capacity could never give the slots back. Tests therefore
 * own their own table and never touch the singleton.
 *
 * DISPATCH IS SYNCHRONOUS AND UNCANCELLABLE -- READ THIS BEFORE RELYING ON THE
 * OVERRUN BUDGET
 *
 * A callback is a plain indirect call. This kernel cannot preempt, abandon or
 * time out one: nothing observes a call that has not returned. The overrun
 * budget below is therefore a POST-RETURN DIAGNOSTIC, not a bound on progress.
 * A callback that genuinely hangs hangs the whole transition, and no warning is
 * emitted at all, because the code that would emit it runs after the call
 * returns.
 *
 * Do not build a liveness assumption on this interface. Bounded liveness would
 * need an explicit watchdog policy (Windows bugchecks DRIVER_POWER_STATE_FAILURE
 * 0x9F for exactly this case); it cannot be retrofitted by tightening the
 * numbers below.
 *
 * CONTEXT CONTRACT (production entry points only)
 *
 * pm_notify_sleep() and pm_notify_resume() refuse unless called at
 * PASSIVE_LEVEL with interrupts ENABLED, mirroring pci_set_d_state()'s refusal
 * above PASSIVE_LEVEL (include/kernel/drivers/pci_pm.h). Two reasons, and the
 * second is the load-bearing one:
 *
 *   - Callbacks block. At raised IRQL that holds off DPCs and lower-priority
 *     interrupts for the whole walk.
 *   - mono_ns() stops advancing with interrupts masked when the active clock
 *     source is tick-derived, so an overrun measured with IF=0 is fiction.
 *     Refusing up front is one condition; compensating afterwards is three
 *     (the same argument as pci_pm_timebase_ready() in
 *     src/kernel/drivers/pci_pm.c).
 *
 * The check runs ONCE at entry, not between callbacks: a driver callback that
 * returns with interrupts masked or IRQL raised makes every later overrun
 * measurement in that walk untrustworthy, and the dispatcher cannot tell.
 * Callbacks are required to return in the context they were called in.
 *
 * The designed caller satisfies this: the pm_enter_s3() worker described in
 * 02-kernel-core/TODO-26-power-management.md section 3 runs on
 * a PASSIVE_LEVEL worker and broadcasts to these callbacks BEFORE the
 * smp_rendezvous_begin() barrier masks interrupts.
 *
 * REFERENT LIFETIME -- BUILT-IN CALLBACKS ONLY
 *
 * Publication safety covers the slot bytes, not the lifetime of the code and
 * data the slot points at. There is no unregister, so on_sleep, on_wake, ctx
 * and name must all stay valid for the rest of the boot. That restricts
 * registration to built-in drivers.
 *
 * A loadable module may NOT register: module_unload() unmaps and frees the
 * module's pages, which would leave a callable function pointer and a dangling
 * context in the table. The module loader already carries the matching
 * obligation for the object-manager callback registry.
 *   -> XREF: 04-drivers-hardware/TODO-05-kernel-module-system.md section 4
 *      (item: "Before pmm_free(base), unregister + drain any Ob handle-op
 *      callbacks the module registered")
 *
 * Supporting removal later is not a matter of adding a clear() call. It needs
 * generation-tagged handles, per-slot active/running state, and rundown
 * draining so an in-flight walk that already snapshotted a slot completes
 * before the referent dies. Published slots must never be compacted.
 */

#ifndef KERNEL_PM_POWER_CALLBACK_H
#define KERNEL_PM_POWER_CALLBACK_H

#include "kernel/types.h"
#include "kernel/sched/spinlock.h"

/* Slot capacity. A uint64_t bitmap records which slots a sleep walk quiesced,
 * so raising this past 64 requires widening the masks it indexes. The static
 * asserts below are the enforcement, not this comment. */
#define PM_CB_MAX_SLOTS 64

/* Walk order. Sleep runs high priority to low (user-space first, storage
 * last); resume runs low to high (storage first, user-space last), so a
 * filesystem is never released against a controller that has not come back. */
typedef enum {
    PM_PRI_STORAGE  = 0,
    PM_PRI_NETWORK  = 1,
    PM_PRI_USB      = 2,
    PM_PRI_INPUT    = 3,
    PM_PRI_GRAPHICS = 4,
    PM_PRI_USER     = 5,
    PM_PRI_COUNT    = 6
} pm_priority_t;

/* Returns 0 on success, non-zero on failure.
 *
 * A callback that returns non-zero MUST have left its device in the state it
 * found it. The dispatcher treats a failed slot as never having transitioned:
 * it is not woken during the sleep unwind, and it is not walked on resume. A
 * callback that mutates and then reports failure breaks that contract, and the
 * dispatcher has no way to detect it. */
typedef int (*pm_power_callback_t)(uint32_t state, void *ctx);

/* Status codes. 0 is success; every failure is negative and distinct, so a
 * caller can tell a refusal apart from a driver fault. */
#define PM_CB_OK               0
#define PM_CB_INVALID         (-1)  /* NULL table, bad priority, no callbacks  */
#define PM_CB_FULL            (-2)  /* PM_CB_MAX_SLOTS reached                 */
#define PM_CB_DUPLICATE       (-3)  /* same (on_sleep, on_wake, ctx) triple    */
#define PM_CB_BUSY            (-4)  /* a transition is in flight               */
#define PM_CB_IRQL            (-5)  /* wrong IRQL or interrupts masked         */
#define PM_CB_CALLBACK_FAILED (-6)  /* at least one callback returned non-zero */
#define PM_CB_NO_TRANSACTION  (-7)  /* resume without a preceding sleep        */
#define PM_CB_UNWIND_FAILED   (-8)  /* a sleep abort could not restore a slot  */
#define PM_CB_STORAGE_FAILED  (-9)  /* resume: storage did not come back       */
#define PM_CB_DEGRADED       (-10)  /* terminally degraded; retrying cannot    */
                                    /* clear it -- see pm_cb_table_recover()   */

/* Post-return overrun thresholds. Diagnostic only -- see the header comment.
 * Storage gets the larger resume budget because a controller re-initialising
 * its command engine legitimately takes longer than a NIC re-arming an
 * interrupt. */
#define PM_CB_SLEEP_OVERRUN_MS          2000u
#define PM_CB_RESUME_OVERRUN_MS         2000u
#define PM_CB_RESUME_STORAGE_OVERRUN_MS 5000u

/* Transaction state. A sleep/resume pair is ONE transaction: registration is
 * refused for its whole duration, so the set of callbacks resumed is exactly
 * the set that was slept. Without that span, a driver registering between the
 * two walks would be woken having never been quiesced. */
#define PM_TXN_IDLE     0u
#define PM_TXN_SLEEPING 1u
#define PM_TXN_ASLEEP   2u
#define PM_TXN_RESUMING 3u
/* A sleep abort whose OWN unwind failed. Terminal: the machine is in an
 * unknown power state (a device was quiesced and could not be brought back),
 * so registration and both walks are refused rather than layering another
 * transaction on top of hardware nobody can account for. Only an explicit
 * pm_cb_table_init() clears it, which is a deliberate act of recovery policy
 * rather than something a caller does by accident. */
#define PM_TXN_DEGRADED 4u

typedef struct {
    pm_power_callback_t on_sleep;
    pm_power_callback_t on_wake;
    void               *ctx;
    const char         *name;
    uint32_t            priority;
    uint32_t            _pad;
} pm_cb_slot_t;

/* Per-walk outcome. Purely informational: the return code is the verdict. */
typedef struct {
    uint32_t invoked;         /* callbacks actually called                   */
    uint32_t failed;          /* callbacks that returned non-zero            */
    uint32_t overruns;        /* callbacks that exceeded their budget        */
    uint32_t unwound;         /* on_wake calls made by a sleep unwind        */
    int      first_status;    /* first non-zero callback return value        */
    uint32_t first_failed_priority;
    uint32_t storage_failed;  /* resume: a PM_PRI_STORAGE callback failed.
                               * The caller MUST NOT unfreeze the scheduler
                               * or release filesystem threads when this is
                               * set -- the disk controller did not come
                               * back, and the filesystem would be issuing
                               * I/O to a controller that cannot serve it. */
} pm_cb_report_t;

typedef struct {
    pm_cb_slot_t slots[PM_CB_MAX_SLOTS];
    uint32_t     count;
    uint32_t     txn_state;
    /* TWO masks, because "was quiesced" and "must be woken" are different sets
     * and conflating them wakes hardware that never slept.
     *   quiesced_mask  slots whose on_sleep actually RAN and succeeded. This
     *                  is what a sleep abort unwinds, and only this.
     *   wake_mask      quiesced_mask plus wake-only slots (no on_sleep at all,
     *                  e.g. the EC). This is what a completed sleep's matching
     *                  resume walks. */
    uint64_t     quiesced_mask;
    uint64_t     wake_mask;
    uint64_t   (*now_ns)(void); /* NULL means mono_ns()                       */
    spinlock_t   lock;          /* guards count + txn_state. NEVER held across
                                 * a callback: callbacks block for seconds,
                                 * and a spinlock raises IRQL to
                                 * DISPATCH_LEVEL. */
} pm_cb_table_t;

/* Pin the bitmaps against the FIELD, not against a literal. Comparing with a
 * bare 64 would still pass if someone narrowed the mask to uint32_t, and the
 * walk would then silently drop every slot above 31. */
_Static_assert(PM_CB_MAX_SLOTS <= sizeof(((pm_cb_table_t *)0)->quiesced_mask) * 8,
               "PM_CB_MAX_SLOTS exceeds the width of quiesced_mask");
_Static_assert(sizeof(((pm_cb_table_t *)0)->wake_mask) ==
               sizeof(((pm_cb_table_t *)0)->quiesced_mask),
               "wake_mask and quiesced_mask must index slots identically");

/* Pin every enumerator VALUE, not just the count. The walk uses priority as a
 * dense 0..PM_PRI_COUNT-1 loop index, so a non-contiguous or reordered
 * enumerator silently skips a whole class of callbacks while PM_PRI_COUNT == 6
 * still holds. Reordering these also reverses the meaning of the sleep and
 * resume walks, which is the bug this file exists to prevent. */
_Static_assert(PM_PRI_STORAGE  == 0, "storage must be the first slot resumed");
_Static_assert(PM_PRI_NETWORK  == 1, "network priority moved");
_Static_assert(PM_PRI_USB      == 2, "usb priority moved");
_Static_assert(PM_PRI_INPUT    == 3, "input priority moved");
_Static_assert(PM_PRI_GRAPHICS == 4, "graphics priority moved");
_Static_assert(PM_PRI_USER     == 5, "user-space must be the first slot slept");
_Static_assert(PM_PRI_COUNT    == 6, "pm_priority_t gained a level: check every "
                                     "walk bound and the OS-comparison table");

/* ---- Table mechanics: no globals, no context policy, injectable clock ---- */

/* Zero a table and seal it in PM_TXN_IDLE. Safe on a stack or static table;
 * NOT safe to call while a walk is in flight.
 *
 * A zeroed table is already valid (PM_TXN_IDLE is 0, an unlocked spinlock is 0,
 * and a NULL now_ns means mono_ns()), which is why the production singleton
 * needs no boot-phase init call. */
void pm_cb_table_init(pm_cb_table_t *t);

/* Append a slot. Refuses a duplicate (on_sleep, on_wake, ctx) triple so a
 * re-run init cannot double-notify, and refuses entirely while a transaction
 * is in flight (PM_CB_BUSY).
 *
 * At least one of on_sleep/on_wake must be non-NULL, and on_sleep REQUIRES an
 * on_wake: a slot that can be quiesced must have a way back, or an aborted
 * sleep leaves its device down while reporting a clean recovery. A driver that
 * genuinely needs nothing on resume passes a no-op returning 0 -- an authored
 * claim about the device rather than an omission. Wake-only (no on_sleep) IS
 * allowed and is the EC's shape.
 *
 * Returns the slot index (>= 0) or a negative PM_CB_* code. */
int pm_cb_table_register(pm_cb_table_t *t, pm_priority_t priority,
                         pm_power_callback_t on_sleep,
                         pm_power_callback_t on_wake,
                         void *ctx, const char *name);

/* Walk high priority to low, calling on_sleep.
 *
 * Stops at the FIRST failure and UNWINDS: every slot already quiesced is woken
 * in resume order and the caller must not enter the platform sleep state.
 * Continuing past a failed quiesce and sleeping anyway would leave part of the
 * machine live with its driver believing it is suspended.
 *
 * The unwind wakes quiesced_mask ONLY. A wake-only slot never slept, so waking
 * it during an abort would run a re-initialisation against hardware that was
 * never taken down.
 *
 * If the unwind ITSELF fails, the table does NOT return to idle: it enters
 * PM_TXN_DEGRADED, keeps the unrecovered bits, and returns PM_CB_UNWIND_FAILED
 * rather than the generic failure. Returning to idle there would advertise a
 * clean machine while a device is still down, and would let the next
 * transaction stack on top of it.
 *
 * On success the transaction moves to PM_TXN_ASLEEP, and only
 * pm_cb_table_notify_resume() can clear it. 'report' may be NULL. */
int pm_cb_table_notify_sleep(pm_cb_table_t *t, uint32_t state,
                             pm_cb_report_t *report);

/* Walk low priority to high, calling on_wake for exactly the slots the
 * matching sleep walk quiesced.
 *
 * Unlike sleep this does NOT stop at the first failure: there is no "do not
 * proceed" left to protect, and abandoning the walk would strand every
 * remaining device powered down. Failures are counted and reported, and a
 * storage failure additionally sets report->storage_failed.
 *
 * Returns PM_CB_STORAGE_FAILED (not the generic PM_CB_CALLBACK_FAILED) when a
 * PM_PRI_STORAGE callback did not come back, so the two layers agree and the
 * distinction cannot be lost on the way out.
 *
 * A resume with ANY failed wake also enters PM_TXN_DEGRADED and retains the
 * failed slots, for the same reason a failed unwind does: a callback that
 * reported failure is still in its pre-wake state, so returning to idle would
 * erase which controller is down at exactly the moment the caller was told not
 * to unfreeze the scheduler.
 *
 * Refuses with PM_CB_NO_TRANSACTION unless the table is PM_TXN_ASLEEP -- a
 * resume with no preceding sleep is a caller bug, not a no-op. */
int pm_cb_table_notify_resume(pm_cb_table_t *t, uint32_t state,
                              pm_cb_report_t *report);

/* Leave PM_TXN_DEGRADED. Takes the lock, so unlike pm_cb_table_init() it is
 * safe against a concurrent registration, and it clears ONLY the transaction
 * state and the retained masks -- registered callbacks survive.
 *
 * Calling this asserts that the caller has accounted for the hardware the
 * failed walk left down; the registry cannot know that on its own. Returns
 * PM_CB_OK when it cleared a degraded table, PM_CB_NO_TRANSACTION when the
 * table was not degraded, PM_CB_INVALID on NULL. */
int pm_cb_table_recover(pm_cb_table_t *t);

/* Observers, for tests and diagnostics. */
uint32_t pm_cb_table_count(const pm_cb_table_t *t);
uint32_t pm_cb_table_txn_state(const pm_cb_table_t *t);

/* Slots a degraded walk could not bring back, so a recovery path can name the
 * devices rather than only counting them. Zero unless PM_TXN_DEGRADED. */
/* Takes the lock (so NOT const): the answer is a pair, and reading txn_state
 * and wake_mask separately would report a healthy mid-transaction table's
 * pending slots as failed devices. */
uint64_t pm_cb_table_unrecovered(pm_cb_table_t *t);

/* ---- Production singleton ------------------------------------------------ */

/* Register against the kernel's power callback table. Called by built-in
 * drivers from their init(); see the referent-lifetime note above. */
int pm_register_power_callback(pm_priority_t priority,
                               pm_power_callback_t on_sleep,
                               pm_power_callback_t on_wake,
                               void *ctx, const char *name);

/* Broadcast to registered drivers. Both refuse unless at PASSIVE_LEVEL with
 * interrupts enabled (PM_CB_IRQL). 'state' is the target system power state,
 * passed through to the callbacks uninterpreted. */
/* pm_notify_resume() returns PM_CB_STORAGE_FAILED, distinct from the generic
 * PM_CB_CALLBACK_FAILED, when a PM_PRI_STORAGE callback did not come back. The
 * caller MUST NOT unfreeze the scheduler or release filesystem threads on that
 * code. It is a return VALUE rather than a report field precisely because it is
 * the one resume outcome a caller cannot treat as advisory, and a field in a
 * struct the production entry point does not hand back would be unreachable to
 * the very caller that has to act on it. */
int pm_notify_sleep(uint32_t state);
int pm_notify_resume(uint32_t state);

/* Registered callback count and transaction state of the kernel's table. */
uint32_t pm_power_callback_count(void);
uint32_t pm_power_callback_txn_state(void);

/* Leave PM_TXN_DEGRADED on the kernel's table. Without this the production
 * singleton has no way out of a degraded state at all -- it is never
 * pm_cb_table_init()'d, so one failed wake would pin power management for the
 * rest of the boot. */
int pm_power_callback_recover(void);

#endif /* KERNEL_PM_POWER_CALLBACK_H */
