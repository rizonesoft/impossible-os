/*
 * power_callback.c -- Driver power callbacks and sleep/resume ordering
 *
 * Owner: 02-kernel-core/TODO-26-power-management.md section 9.
 * See include/kernel/pm/power_callback.h
 * for the interface contracts: the two-layer split, the uncancellable-dispatch
 * warning, the PASSIVE_LEVEL-with-interrupts-enabled requirement, and the
 * built-in-callbacks-only lifetime rule.
 *
 * LOCKING
 *
 * One spinlock guards the table's mutable header: count, txn_state, and the
 * quiesced/wake masks. It is taken to open and to close a transaction (and to
 * snapshot count under the same acquire), and to register. It is NEVER held
 * across a callback -- callbacks block for seconds and spin_lock_irqsave
 * raises IRQL to DISPATCH_LEVEL.
 *
 * count and txn_state are written with RELEASE stores even though the lock is
 * held for every write: pm_cb_table_count() and pm_cb_table_txn_state() read
 * them with ACQUIRE loads and take no lock, so pairing the stores keeps those
 * two observers from being a mixed atomic/plain access on the same object.
 *
 * What makes the unlocked walk safe is not the lock, it is the transaction:
 * while txn_state is not PM_TXN_IDLE, registration is refused, so the slot
 * prefix [0, count) the walk snapshotted cannot grow underneath it. Slots are
 * never removed, so it cannot shrink either. Drop the transaction check and
 * this walk becomes a use-after-publish race.
 */

#include "kernel/types.h"
#include "kernel/pm/power_callback.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/irql.h"
#include "kernel/time/mono_clock.h"
#include "kernel/klog.h"

/* The production registry. Deliberately zero-initialised in .bss and never
 * explicitly constructed: PM_TXN_IDLE is 0, an unlocked spinlock_t is 0, a
 * count of 0 is an empty table, and a NULL now_ns selects mono_ns(). Adding a
 * pm_power_callback_init() would create a boot-ordering dependency for every
 * driver registration to trip over, and buy nothing: the only registrant today
 * is acpi_ec_init() in Phase 2, and the four blocked driver registrations would
 * land in Phase 1 and Phase 2 alike. Leaving PM_TXN_DEGRADED needs the LOCKED
 * pm_cb_table_recover(), not this zero state. */
static pm_cb_table_t s_pm_table;

#define NS_PER_MS 1000000ull

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static uint64_t pm_cb_now(const pm_cb_table_t *t)
{
    return t->now_ns ? t->now_ns() : mono_ns();
}

/* Elapsed nanoseconds across a callback.
 *
 * Elapsed-subtraction rather than an absolute deadline, matching
 * pci_pm_delay_us(): it cannot be defeated by a counter that wraps. A clock
 * that stepped BACKWARD reports 0 rather than an enormous unsigned difference
 * -- under-reporting costs a diagnostic warning, whereas the wrapped value
 * would fabricate a multi-year overrun and log a lie. */
static uint64_t pm_cb_elapsed_ns(uint64_t start, uint64_t end)
{
    return (end >= start) ? (end - start) : 0ull;
}

/* Set bits in a slot mask. Written out rather than using __builtin_popcountll:
 * that lowers to a POPCNT instruction the compiler may emit without a CPUID
 * check, and this kernel does not gate on the POPCNT feature bit. */
static uint32_t pm_cb_popcount(uint64_t mask)
{
    uint32_t n = 0;

    while (mask) {
        mask &= (mask - 1ull);
        n++;
    }
    return n;
}

/* Invoke one callback and account for it. Returns the callback's own status.
 *
 * The overrun check happens AFTER the call returns, and cannot do otherwise:
 * see the uncancellable-dispatch note in the header. A callback that never
 * returns is never measured and never warned about. */
static int pm_cb_invoke(pm_cb_table_t *t, const pm_cb_slot_t *slot,
                        pm_power_callback_t fn, uint32_t state,
                        uint32_t budget_ms, const char *phase,
                        pm_cb_report_t *report)
{
    uint64_t start = pm_cb_now(t);
    int      rc    = fn(state, slot->ctx);
    uint64_t ns    = pm_cb_elapsed_ns(start, pm_cb_now(t));

    report->invoked++;

    if (ns > (uint64_t)budget_ms * NS_PER_MS) {
        report->overruns++;
        klog(LOG_WARN, "pm",
             "%s callback '%s' overran budget: %ums used, %ums budget",
             phase, slot->name ? slot->name : "(unnamed)",
             (uint64_t)(ns / NS_PER_MS), (uint64_t)budget_ms);
    }

    if (rc != 0) {
        if (report->failed == 0) {
            report->first_status           = rc;
            report->first_failed_priority  = slot->priority;
        }
        report->failed++;
        klog(LOG_ERROR, "pm", "%s callback '%s' failed with status %d",
             phase, slot->name ? slot->name : "(unnamed)", (int64_t)rc);
    }

    return rc;
}

/* Wake every slot recorded in 'mask', in resume order (low priority first).
 * Used both by the sleep unwind and by the resume walk, which is the whole
 * reason the two agree on ordering by construction rather than by review. */
static uint64_t pm_cb_wake_mask(pm_cb_table_t *t, uint64_t mask, uint32_t state,
                                uint32_t count, pm_cb_report_t *report,
                                int is_unwind)
{
    uint64_t unrecovered = 0;
    uint32_t pri, i;

    for (pri = 0; pri < (uint32_t)PM_PRI_COUNT; pri++) {
        for (i = 0; i < count; i++) {
            const pm_cb_slot_t *slot = &t->slots[i];
            uint32_t budget;
            int      rc;

            if (slot->priority != pri)
                continue;
            if ((mask & (1ull << i)) == 0ull)
                continue;
            if (!slot->on_wake)
                continue;

            budget = (pri == (uint32_t)PM_PRI_STORAGE)
                         ? PM_CB_RESUME_STORAGE_OVERRUN_MS
                         : PM_CB_RESUME_OVERRUN_MS;

            rc = pm_cb_invoke(t, slot, slot->on_wake, state, budget,
                              is_unwind ? "unwind" : "resume", report);

            if (is_unwind)
                report->unwound++;

            /* A storage device that did not come back is the one resume
             * failure the caller cannot treat as advisory: unfreezing the
             * scheduler would release filesystem threads against a controller
             * that cannot serve them. */
            if (rc != 0) {
                /* Retained so an unwind can say WHICH slots it could not
                 * bring back, rather than reporting a count nobody can act
                 * on. */
                unrecovered |= (1ull << i);

                if (pri == (uint32_t)PM_PRI_STORAGE)
                    report->storage_failed = 1;
            }
        }
    }

    return unrecovered;
}

static void pm_cb_report_reset(pm_cb_report_t *r)
{
    r->invoked               = 0;
    r->failed                = 0;
    r->overruns              = 0;
    r->unwound               = 0;
    r->first_status          = 0;
    r->first_failed_priority = 0;
    r->storage_failed        = 0;
}

/* ---------------------------------------------------------------------------
 * Table mechanics
 * ------------------------------------------------------------------------- */

void pm_cb_table_init(pm_cb_table_t *t)
{
    uint32_t i;

    if (!t)
        return;

    for (i = 0; i < PM_CB_MAX_SLOTS; i++) {
        t->slots[i].on_sleep = 0;
        t->slots[i].on_wake  = 0;
        t->slots[i].ctx      = 0;
        t->slots[i].name     = 0;
        t->slots[i].priority = 0;
        t->slots[i]._pad     = 0;
    }

    t->count         = 0;
    t->txn_state     = PM_TXN_IDLE;
    t->quiesced_mask = 0;
    t->wake_mask     = 0;
    t->now_ns        = 0;
    t->lock.flag     = 0;
}

int pm_cb_table_register(pm_cb_table_t *t, pm_priority_t priority,
                         pm_power_callback_t on_sleep,
                         pm_power_callback_t on_wake,
                         void *ctx, const char *name)
{
    uint64_t flags;
    uint32_t i;
    int      slot_idx;

    if (!t)
        return PM_CB_INVALID;
    if ((uint32_t)priority >= (uint32_t)PM_PRI_COUNT)
        return PM_CB_INVALID;
    /* A slot with neither callback would occupy capacity and do nothing on
     * both walks. Refuse it rather than silently consuming a slot. */
    if (!on_sleep && !on_wake)
        return PM_CB_INVALID;
    /* A slot that can be QUIESCED must have a way back. Without this, a
     * sleep-only slot takes a quiesced_mask bit no unwind can ever clear
     * (pm_cb_wake_mask skips a slot with no on_wake), so an aborted sleep
     * would leave that device down and still report a clean recovery.
     *
     * A driver that genuinely needs nothing on resume passes a no-op that
     * returns 0. That is a deliberate claim about the device rather than an
     * omission, and it is the difference between a contract and a silence. */
    if (on_sleep && !on_wake)
        return PM_CB_INVALID;

    spin_lock_irqsave(&t->lock, &flags);

    /* Registration is refused for the whole sleep-to-resume transaction, not
     * merely during a walk. A driver admitted between the two walks would be
     * resumed having never been quiesced. */
    if (t->txn_state != PM_TXN_IDLE) {
        int rc = (t->txn_state == PM_TXN_DEGRADED) ? PM_CB_DEGRADED
                                                   : PM_CB_BUSY;
        spin_unlock_irqrestore(&t->lock, flags);
        return rc;
    }

    for (i = 0; i < t->count; i++) {
        if (t->slots[i].on_sleep == on_sleep &&
            t->slots[i].on_wake  == on_wake  &&
            t->slots[i].ctx      == ctx) {
            spin_unlock_irqrestore(&t->lock, flags);
            return PM_CB_DUPLICATE;
        }
    }

    if (t->count >= PM_CB_MAX_SLOTS) {
        spin_unlock_irqrestore(&t->lock, flags);
        return PM_CB_FULL;
    }

    slot_idx                       = (int)t->count;
    t->slots[slot_idx].on_sleep    = on_sleep;
    t->slots[slot_idx].on_wake     = on_wake;
    t->slots[slot_idx].ctx         = ctx;
    t->slots[slot_idx].name        = name;
    t->slots[slot_idx].priority    = (uint32_t)priority;
    t->slots[slot_idx]._pad        = 0;
    __atomic_store_n(&t->count, t->count + 1u, __ATOMIC_RELEASE);

    spin_unlock_irqrestore(&t->lock, flags);
    return slot_idx;
}

int pm_cb_table_notify_sleep(pm_cb_table_t *t, uint32_t state,
                             pm_cb_report_t *report)
{
    pm_cb_report_t  local;
    pm_cb_report_t *rep = report ? report : &local;
    uint64_t        flags;
    uint64_t        quiesced = 0;   /* on_sleep actually ran and succeeded */
    uint64_t        wake     = 0;   /* quiesced PLUS wake-only slots       */
    uint64_t        unrecovered;
    uint32_t        count;
    uint32_t        pri, i;
    int             failed = 0;

    if (!t)
        return PM_CB_INVALID;

    pm_cb_report_reset(rep);

    /* Open the transaction and snapshot the slot count under the same lock:
     * from here registration is refused, so the prefix [0, count) is frozen
     * for the whole walk without the walk holding anything. */
    spin_lock_irqsave(&t->lock, &flags);
    if (t->txn_state != PM_TXN_IDLE) {
        int rc = (t->txn_state == PM_TXN_DEGRADED) ? PM_CB_DEGRADED
                                                   : PM_CB_BUSY;
        spin_unlock_irqrestore(&t->lock, flags);
        return rc;
    }
    __atomic_store_n(&t->txn_state, PM_TXN_SLEEPING, __ATOMIC_RELEASE);
    t->quiesced_mask = 0;
    t->wake_mask     = 0;
    count            = t->count;
    spin_unlock_irqrestore(&t->lock, flags);

    /* Reverse priority: user-space quiesces first, storage last. */
    for (pri = (uint32_t)PM_PRI_COUNT; pri-- > 0 && !failed; ) {
        for (i = 0; i < count; i++) {
            const pm_cb_slot_t *slot = &t->slots[i];

            if (slot->priority != pri)
                continue;
            if (!slot->on_sleep) {
                /* Nothing to quiesce, but the slot still owns a resume: a
                 * wake-only driver must be woken by the matching walk. It does
                 * NOT enter quiesced_mask -- it never slept, so a sleep abort
                 * must not "restore" it by running a re-initialisation against
                 * hardware that was never taken down. */
                wake |= (1ull << i);
                continue;
            }

            if (pm_cb_invoke(t, slot, slot->on_sleep, state,
                             PM_CB_SLEEP_OVERRUN_MS, "sleep", rep) != 0) {
                failed = 1;
                break;
            }

            quiesced |= (1ull << i);
            wake     |= (1ull << i);
        }
    }

    if (failed) {
        /* Unwind: wake everything already quiesced, in resume order, and put
         * the transaction back. The caller must NOT enter the platform sleep
         * state -- half the machine is live and its drivers believe they are
         * suspended.
         *
         * The count is the mask population, not rep->invoked: invoked includes
         * the callback that just FAILED (which is not being unwound) and
         * excludes wake-only slots (which are). */
        /* UNWIND FIRST, THEN REPORT. klog reaches serial_write, whose normal
         * path spins unbounded on UART THRE (src/kernel/drivers/serial.c
         * documents this as what hangs a panic on a wedged UART). A diagnostic
         * ahead of the unwind would let a wedged UART prevent the recovery it
         * is only describing. Nothing here needs to be said before the devices
         * are back. */
        unrecovered = pm_cb_wake_mask(t, quiesced, state, count, rep, 1);

        klog(LOG_ERROR, "pm",
             "sleep aborted at priority %u: unwound %u quiesced callback(s)",
             (uint64_t)rep->first_failed_priority,
             (uint64_t)pm_cb_popcount(quiesced));

        spin_lock_irqsave(&t->lock, &flags);
        if (unrecovered) {
            /* The abort could not put the machine back. Returning to IDLE here
             * would advertise a clean table while a device is still down, and
             * would let the next transaction stack on top of it. Keep the
             * unrecovered bits and refuse everything until someone explicitly
             * re-initialises. */
            t->quiesced_mask = unrecovered;
            t->wake_mask     = unrecovered;
            __atomic_store_n(&t->txn_state, PM_TXN_DEGRADED, __ATOMIC_RELEASE);
        } else {
            t->quiesced_mask = 0;
            t->wake_mask     = 0;
            __atomic_store_n(&t->txn_state, PM_TXN_IDLE, __ATOMIC_RELEASE);
        }
        spin_unlock_irqrestore(&t->lock, flags);

        if (unrecovered) {
            klog(LOG_ERROR, "pm",
                 "sleep unwind FAILED for %u slot(s): power state is unknown",
                 (uint64_t)pm_cb_popcount(unrecovered));
            return PM_CB_UNWIND_FAILED;
        }

        return PM_CB_CALLBACK_FAILED;
    }

    spin_lock_irqsave(&t->lock, &flags);
    t->quiesced_mask = quiesced;
    t->wake_mask     = wake;
    __atomic_store_n(&t->txn_state, PM_TXN_ASLEEP, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&t->lock, flags);

    return PM_CB_OK;
}

int pm_cb_table_notify_resume(pm_cb_table_t *t, uint32_t state,
                              pm_cb_report_t *report)
{
    pm_cb_report_t  local;
    pm_cb_report_t *rep = report ? report : &local;
    uint64_t        flags;
    uint64_t        mask;
    uint64_t        quiesced;
    uint64_t        unrecovered;
    uint32_t        count;

    if (!t)
        return PM_CB_INVALID;

    pm_cb_report_reset(rep);

    spin_lock_irqsave(&t->lock, &flags);
    /* A resume with no preceding sleep is a caller bug. Reporting it beats
     * silently waking devices that were never quiesced. */
    if (t->txn_state != PM_TXN_ASLEEP) {
        spin_unlock_irqrestore(&t->lock, flags);
        return PM_CB_NO_TRANSACTION;
    }
    __atomic_store_n(&t->txn_state, PM_TXN_RESUMING, __ATOMIC_RELEASE);
    mask         = t->wake_mask;
    quiesced     = t->quiesced_mask;
    count        = t->count;
    spin_unlock_irqrestore(&t->lock, flags);

    /* Forward priority: storage first, user-space last. Does not stop at the
     * first failure -- abandoning the walk would strand every remaining device
     * powered down, and there is no "do not proceed" left to protect. */
    unrecovered = pm_cb_wake_mask(t, mask, state, count, rep, 0);

    spin_lock_irqsave(&t->lock, &flags);
    if (unrecovered) {
        /* A callback that reported failure is contractually still in its
         * pre-wake state, so those devices are STILL DOWN. Clearing the masks
         * and reopening the table would erase which controller never came
         * back -- the caller has just been told not to unfreeze the scheduler
         * and would have nothing left to name. Every wake was still attempted
         * before landing here. */
        /* INTERSECT, never assign. A wake-only slot holds a wake_mask bit and
         * no quiesced_mask bit; copying `unrecovered` into both would invent a
         * quiesce that never happened and contradict the mask contract in the
         * header. quiesced_mask stays a subset of wake_mask on every path. */
        t->quiesced_mask = quiesced & unrecovered;
        t->wake_mask     = unrecovered;
        __atomic_store_n(&t->txn_state, PM_TXN_DEGRADED, __ATOMIC_RELEASE);
    } else {
        t->quiesced_mask = 0;
        t->wake_mask     = 0;
        __atomic_store_n(&t->txn_state, PM_TXN_IDLE, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&t->lock, flags);

    if (unrecovered)
        klog(LOG_ERROR, "pm",
             "resume FAILED for %u slot(s): power state is unknown",
             (uint64_t)pm_cb_popcount(unrecovered));

    /* Storage dominates: it is the one failure the caller may not treat as
     * advisory, so it gets its own code rather than being averaged into the
     * generic one. */
    if (rep->storage_failed)
        return PM_CB_STORAGE_FAILED;

    return (rep->failed > 0) ? PM_CB_CALLBACK_FAILED : PM_CB_OK;
}

/* Both observers take an acquire load rather than the lock: they are
 * diagnostics, a lock would raise IRQL for a single word, and a caller that
 * needs count and txn_state to agree with each other must open a transaction
 * instead -- no pair of separate reads can promise that. */
int pm_cb_table_recover(pm_cb_table_t *t)
{
    uint64_t flags;

    if (!t)
        return PM_CB_INVALID;

    /* Takes the lock, unlike pm_cb_table_init(): this runs on a live table
     * that other CPUs may be trying to register against, and it must not
     * clear the lock word out from under one of them. It also preserves the
     * registered callbacks -- the devices are still registered, only the
     * failed transaction is being written off. */
    spin_lock_irqsave(&t->lock, &flags);
    if (t->txn_state != PM_TXN_DEGRADED) {
        spin_unlock_irqrestore(&t->lock, flags);
        return PM_CB_NO_TRANSACTION;
    }
    t->quiesced_mask = 0;
    t->wake_mask     = 0;
    __atomic_store_n(&t->txn_state, PM_TXN_IDLE, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&t->lock, flags);

    klog(LOG_WARN, "pm", "power callback registry recovered from degraded");
    return PM_CB_OK;
}

uint64_t pm_cb_table_unrecovered(pm_cb_table_t *t)
{
    uint64_t flags;
    uint64_t mask;

    if (!t)
        return 0ull;

    /* Under the LOCK, because the answer is a pair: the mask means
     * "unrecovered" only while the table is DEGRADED, and in every other state
     * wake_mask holds the slots PENDING a wake. Reading the two separately
     * would report a healthy mid-transaction table's pending slots as failed
     * devices, which is the opposite of what a recovery consumer needs. */
    spin_lock_irqsave(&t->lock, &flags);
    mask = (t->txn_state == PM_TXN_DEGRADED) ? t->wake_mask : 0ull;
    spin_unlock_irqrestore(&t->lock, flags);

    return mask;
}

uint32_t pm_cb_table_count(const pm_cb_table_t *t)
{
    return t ? __atomic_load_n((const volatile uint32_t *)&t->count,
                               __ATOMIC_ACQUIRE)
             : 0u;
}

uint32_t pm_cb_table_txn_state(const pm_cb_table_t *t)
{
    return t ? __atomic_load_n((const volatile uint32_t *)&t->txn_state,
                               __ATOMIC_ACQUIRE)
             : PM_TXN_IDLE;
}

/* ---------------------------------------------------------------------------
 * Production singleton
 * ------------------------------------------------------------------------- */

/* Non-zero when this context may run a notification walk.
 *
 * Both conditions are refusals, not compensations. Above PASSIVE_LEVEL a
 * seconds-long walk holds off DPCs; with interrupts masked a tick-derived
 * mono_ns() stops advancing and every overrun measurement becomes fiction. */
static int pm_notify_context_ok(void)
{
    return KeGetCurrentIrql() == PASSIVE_LEVEL && irqs_enabled();
}

int pm_register_power_callback(pm_priority_t priority,
                               pm_power_callback_t on_sleep,
                               pm_power_callback_t on_wake,
                               void *ctx, const char *name)
{
    int rc = pm_cb_table_register(&s_pm_table, priority, on_sleep, on_wake,
                                  ctx, name);

    if (rc < 0) {
        klog(LOG_ERROR, "pm",
             "power callback '%s' registration refused (status %d)",
             name ? name : "(unnamed)", (uint64_t)rc);
        return rc;
    }

    klog(LOG_INFO, "pm", "registered power callback '%s' at priority %u",
         name ? name : "(unnamed)", (uint64_t)priority);
    return rc;
}

int pm_notify_sleep(uint32_t state)
{
    pm_cb_report_t rep;
    int            rc;

    if (!pm_notify_context_ok()) {
        klog(LOG_ERROR, "pm",
             "pm_notify_sleep refused: needs PASSIVE_LEVEL with interrupts on");
        return PM_CB_IRQL;
    }

    rc = pm_cb_table_notify_sleep(&s_pm_table, state, &rep);

    klog(LOG_INFO, "pm",
         "sleep notify state %u: %u invoked, %u failed, %u overran, %u unwound",
         (uint64_t)state, (uint64_t)rep.invoked, (uint64_t)rep.failed,
         (uint64_t)rep.overruns, (uint64_t)rep.unwound);

    return rc;
}

int pm_notify_resume(uint32_t state)
{
    pm_cb_report_t rep;
    int            rc;

    if (!pm_notify_context_ok()) {
        klog(LOG_ERROR, "pm",
             "pm_notify_resume refused: needs PASSIVE_LEVEL with interrupts on");
        return PM_CB_IRQL;
    }

    rc = pm_cb_table_notify_resume(&s_pm_table, state, &rep);

    klog(LOG_INFO, "pm",
         "resume notify state %u: %u invoked, %u failed, %u overran",
         (uint64_t)state, (uint64_t)rep.invoked, (uint64_t)rep.failed,
         (uint64_t)rep.overruns);

    /* Loud and separate: a caller that unfreezes the scheduler after this has
     * released filesystem threads against a controller that did not return.
     *
     * LOG_ERROR, not LOG_FATAL -- LOG_FATAL halts (include/kernel/klog.h), and
     * whether an unresumable disk is fatal is the caller's policy call, not
     * this dispatcher's. The report field is how that decision is delivered;
     * this line is how a human finds it in the log. */
    if (rep.storage_failed)
        klog(LOG_ERROR, "pm",
             "storage failed to resume: the scheduler must NOT be unfrozen");

    return rc;
}

uint32_t pm_power_callback_count(void)
{
    return pm_cb_table_count(&s_pm_table);
}

uint32_t pm_power_callback_txn_state(void)
{
    return pm_cb_table_txn_state(&s_pm_table);
}

int pm_power_callback_recover(void)
{
    return pm_cb_table_recover(&s_pm_table);
}
