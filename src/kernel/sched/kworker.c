/* ============================================================================
 * kworker.c -- system worker thread pool for long-period periodic callbacks
 *
 * One shared kernel thread (kworker_main) runs every registered callback at its
 * period. Cancellation is race-free via a per-slot generation + running flag
 * (the "rundown" protocol): the worker marks a slot running=1 UNDER the table
 * lock in the same critical section it checks active+deadline, so unregister --
 * which clears active under the same lock and then waits for running==0 -- can
 * never let a just-unregistered callback fire or use a freed ctx.
 *
 * A SINGLE serial worker runs all callbacks, so a slow/hung callback delays
 * every other entry (a duration watchdog warns; callbacks must be bounded). A
 * callback that unregisters on the worker thread skips the drain-wait so it
 * cannot deadlock waiting for itself.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/sched/kworker.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/task.h"
#include "kernel/timer.h"
#include "kernel/klog.h"

#define NS_PER_MS  1000000ULL

/* Bound on kworker_init's wait for the worker to publish STARTED (yield count).
 * The worker runs within a few yields once scheduled; the cap only fail-closes a
 * worker that never gets scheduled. */
#define KWORKER_START_YIELD_CAP  100000u

struct kworker_entry {
    kworker_callback_t callback;
    void              *ctx;
    uint32_t           period_ms;
    uint64_t           last_fire_ns;
    uint64_t           deadline_ns;
    uint32_t           generation;  /* bumped on each register; pins the token   */
    uint8_t            active;       /* 1 = registered (mutated only under lock)  */
    uint8_t            running;      /* 1 = callback in flight (set under lock)   */
};

/* startup state: a racing kworker_init caller must never observe "started" while
 * the worker thread is still being created or has just failed. */
#define KWORKER_STOPPED   0
#define KWORKER_STARTING  1
#define KWORKER_STARTED   2

static struct kworker_entry s_entries[KWORKER_MAX_ENTRIES];
static spinlock_t           s_kworker_lock;
static struct thread       *s_kworker_thread;  /* dispatch THREAD identity for the */
                                               /* self-unregister guard -- a true   */
                                               /* per-thread id, NOT the task, so a */
                                               /* sibling kthread in the same task  */
                                               /* is not mistaken for the worker    */
static volatile int         s_kworker_state;

/* token = (generation << 8) | slot. slot is < KWORKER_MAX_ENTRIES (<= 8 bits);
 * generation is masked to 23 bits (KWORKER_GEN_MASK) so the 31-bit token always
 * stays NON-NEGATIVE as an int -- a token can never alias the negative failure
 * return, so a successful register never reports failure with the slot active.
 *
 * Bounded aliasing (accepted): the 23-bit generation wraps after 2^23 (~8.4M)
 * register/unregister cycles ON THE SAME SLOT, at which point a stale token held
 * across all of those cycles could alias the live token. A monitor registers
 * once and (rarely) unregisters, so this is not reachable in practice;
 * eliminating it entirely would need a token wider than the public `int` return
 * (a uint64 token API change). */
#define KWORKER_GEN_MASK  0x7FFFFFu   /* 23 bits -> token <= 0x7FFFFFFF (positive) */

static int kworker_make_token(uint32_t slot, uint32_t gen)
{
    return (int)(((gen & KWORKER_GEN_MASK) << 8) | (slot & 0xFFu));
}
static uint32_t kworker_token_slot(int token) { return (uint32_t)token & 0xFFu; }
static uint32_t kworker_token_gen(int token)  { return (uint32_t)token >> 8; }

int kworker_register(kworker_callback_t fn, void *ctx, uint32_t period_ms)
{
    uint64_t f;
    uint32_t i;
    int token = -1;

    if (!fn || period_ms == 0)
        return -1;

    spin_lock_irqsave(&s_kworker_lock, &f);
    for (i = 0; i < KWORKER_MAX_ENTRIES; i++) {
        if (!s_entries[i].active && !s_entries[i].running) {
            s_entries[i].generation =
                (s_entries[i].generation + 1) & KWORKER_GEN_MASK; /* fresh token  */
            s_entries[i].callback     = fn;
            s_entries[i].ctx          = ctx;
            s_entries[i].period_ms    = period_ms;
            s_entries[i].last_fire_ns = 0;
            s_entries[i].deadline_ns  = uptime_ns() + (uint64_t)period_ms * NS_PER_MS;
            s_entries[i].active       = 1;
            token = kworker_make_token(i, s_entries[i].generation);
            break;
        }
    }
    spin_unlock_irqrestore(&s_kworker_lock, f);

    if (token < 0)
        klog(LOG_WARN, "kworker", "register failed: no free slot (max %u)",
             (uint64_t)KWORKER_MAX_ENTRIES);
    return token;
}

int kworker_unregister(int token)
{
    uint32_t slot = kworker_token_slot(token);
    uint32_t gen  = kworker_token_gen(token);
    uint64_t f;
    int      on_worker;

    if (token < 0 || slot >= KWORKER_MAX_ENTRIES)
        return -1;

    /* Are we being called from inside a kworker callback (i.e. ON the worker
     * dispatch thread)? If so we must NOT wait for running==0 -- that is us, and
     * we would deadlock waiting for ourselves. Compare the THREAD identity (not
     * the task): a sibling kthread spawned into the same task must take the
     * normal drain path. ACQUIRE pairs the worker's RELEASE publish. */
    {
        struct thread *w = __atomic_load_n(&s_kworker_thread, __ATOMIC_ACQUIRE);
        on_worker = (w && thread_current() == w);
    }

    spin_lock_irqsave(&s_kworker_lock, &f);
    if (!s_entries[slot].active || s_entries[slot].generation != gen) {
        spin_unlock_irqrestore(&s_kworker_lock, f);
        return -1;   /* stale or already-unregistered token */
    }
    s_entries[slot].active = 0;   /* no further fire: the worker checks active    */
                                  /* under this same lock before running=1        */
    spin_unlock_irqrestore(&s_kworker_lock, f);

    if (on_worker)
        return 0;   /* the current callback finishes normally; no drain-wait     */

    /* Drain: wait until any in-flight call on this slot returns. The worker sets
     * running=0 under the lock after the callback. Yield between polls so the
     * worker can make progress (this caller is a PASSIVE thread, not the worker). */
    for (;;) {
        uint8_t busy;
        spin_lock_irqsave(&s_kworker_lock, &f);
        busy = s_entries[slot].running;
        spin_unlock_irqrestore(&s_kworker_lock, f);
        if (!busy)
            break;
        yield();
    }
    return 0;
}

uint64_t kworker_last_fire_ns(int token)
{
    uint32_t slot = kworker_token_slot(token);
    uint32_t gen  = kworker_token_gen(token);
    uint64_t f, ret = 0;

    if (token < 0 || slot >= KWORKER_MAX_ENTRIES)
        return 0;
    spin_lock_irqsave(&s_kworker_lock, &f);
    if (s_entries[slot].active && s_entries[slot].generation == gen)
        ret = s_entries[slot].last_fire_ns;
    spin_unlock_irqrestore(&s_kworker_lock, f);
    return ret;
}

static void kworker_main(void)
{
    /* Publish our thread identity (RELEASE) so kworker_unregister can detect a
     * self-unregister, then mark fully started so a racing init returns success
     * only once we actually exist. */
    __atomic_store_n(&s_kworker_thread, thread_current(), __ATOMIC_RELEASE);
    __atomic_store_n(&s_kworker_state, KWORKER_STARTED, __ATOMIC_RELEASE);

    klog(LOG_INFO, "kworker", "system worker thread started (%u slots)",
         (uint64_t)KWORKER_MAX_ENTRIES);

    for (;;) {
        uint64_t now  = uptime_ns();
        uint64_t next = now + (uint64_t)KWORKER_MAX_SLEEP_MS * NS_PER_MS;
        uint32_t i;
        uint32_t sleep_ms_val;

        for (i = 0; i < KWORKER_MAX_ENTRIES; i++) {
            kworker_callback_t cb = (kworker_callback_t)0;
            void              *ctx = (void *)0;
            uint64_t           f;
            int                fire = 0;

            spin_lock_irqsave(&s_kworker_lock, &f);
            if (s_entries[i].active) {
                if (now >= s_entries[i].deadline_ns) {
                    /* Due: mark running + snapshot UNDER the lock (the rundown
                     * barrier), so unregister cannot slip between the active
                     * check and the call. */
                    s_entries[i].running = 1;
                    cb   = s_entries[i].callback;
                    ctx  = s_entries[i].ctx;
                    fire = 1;
                } else if (s_entries[i].deadline_ns < next) {
                    next = s_entries[i].deadline_ns;
                }
            }
            spin_unlock_irqrestore(&s_kworker_lock, f);

            if (!fire)
                continue;

            {
                uint64_t t0 = uptime_ns();
                if (cb)
                    cb(ctx);   /* OUTSIDE the lock: callbacks may block/page/sleep */
                uint64_t dt_ms = (uptime_ns() - t0) / NS_PER_MS;
                if (dt_ms > KWORKER_CALLBACK_WARN_MS)
                    klog(LOG_WARN, "kworker",
                         "callback in slot %u ran %lu ms -- callbacks must be "
                         "bounded (a slow one delays every other entry)",
                         (uint64_t)i, (uint64_t)dt_ms);
            }

            spin_lock_irqsave(&s_kworker_lock, &f);
            s_entries[i].running = 0;
            if (s_entries[i].active) {
                /* Coalesce missed periods: next deadline from NOW, not
                 * deadline += period, so an overrun does not burst-fire to
                 * catch up (matches NT/Linux periodic-work semantics). */
                s_entries[i].last_fire_ns = now;
                s_entries[i].deadline_ns  =
                    uptime_ns() + (uint64_t)s_entries[i].period_ms * NS_PER_MS;
                if (s_entries[i].deadline_ns < next)
                    next = s_entries[i].deadline_ns;
            }
            spin_unlock_irqrestore(&s_kworker_lock, f);
        }

        /* Cooperative wait until the next deadline (capped). Use a yield-poll,
         * NOT sleep_ms(): the timer HAL's sleep_ms busy-HLTs without yielding,
         * which starves every other thread on a single CPU. yield() hands the
         * CPU to the scheduler each pass (same cooperative idle pattern the
         * threaded-DPC worker uses for event_wait_timeout). */
        now = uptime_ns();
        sleep_ms_val = (next > now) ? (uint32_t)((next - now) / NS_PER_MS) : 0;
        if (sleep_ms_val > KWORKER_MAX_SLEEP_MS)
            sleep_ms_val = KWORKER_MAX_SLEEP_MS;
        {
            uint64_t wake = uptime_ns() + (uint64_t)sleep_ms_val * NS_PER_MS;
            do {
                yield();
            } while (uptime_ns() < wake);
        }
    }
}

int kworker_is_started(void)
{
    /* Acquire pairs with the RELEASE store in kworker_main that publishes
     * STARTED, so a caller that observes started also observes the worker's
     * initialized state. No CAS, no task_create, no spin -- a caller on a
     * latency-sensitive thread can ask this question for free. */
    return (__atomic_load_n(&s_kworker_state, __ATOMIC_ACQUIRE) == KWORKER_STARTED)
           ? 1 : 0;
}

int kworker_init(void)
{
    extern int task_create(void (*entry)(void), const char *name);
    int expected = KWORKER_STOPPED;
    int tid;

    /* CAS stopped -> starting: exactly one caller creates the worker task. A
     * concurrent caller that loses the CAS does NOT return success while state
     * is "starting" -- it waits for the terminal state (started or, on failure,
     * stopped) so it can never falsely report an armed-but-workerless pool. */
    if (!__atomic_compare_exchange_n(&s_kworker_state, &expected, KWORKER_STARTING,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        /* Another caller owns startup. Wait for a terminal state, BUT bounded --
         * if the winner's worker is wedged at startup (state stuck at STARTING),
         * a loser must fail closed, not spin forever. State stays STARTING so a
         * latent worker can still self-heal to STARTED for later callers. */
        int st;
        uint32_t spin = 0;
        while ((st = __atomic_load_n(&s_kworker_state, __ATOMIC_ACQUIRE))
               == KWORKER_STARTING) {
            if (++spin >= KWORKER_START_YIELD_CAP)
                return -1;
            yield();
        }
        return (st == KWORKER_STARTED) ? 0 : -1;
    }

    /* Own task (NOT kthread_create into the shared kernel task) so the worker's
     * THREAD pointer is an unambiguous self-identity for the self-unregister
     * guard -- matches the threaded-DPC worker. kworker_main publishes STARTED. */
    tid = task_create(kworker_main, "kworker");
    if (tid < 0) {
        __atomic_store_n(&s_kworker_state, KWORKER_STOPPED, __ATOMIC_RELEASE);
        klog(LOG_ERROR, "kworker",
             "worker task creation FAILED -- periodic monitors will NOT run");
        return -1;
    }

    /* Wait until the worker actually runs and publishes STARTED, so "success"
     * means worker-READY (the boot caller arms monitors on this return, and
     * losing callers already wait for the same terminal state). Bounded so a
     * wedged-at-startup worker fails closed instead of hanging boot. */
    {
        uint32_t spin = 0;
        while (__atomic_load_n(&s_kworker_state, __ATOMIC_ACQUIRE) != KWORKER_STARTED) {
            if (++spin >= KWORKER_START_YIELD_CAP) {
                klog(LOG_ERROR, "kworker",
                     "worker did not reach STARTED -- periodic monitors disabled");
                return -1;   /* leave state STARTING: if the worker runs later it */
                             /* self-heals to STARTED; we just do not arm now      */
            }
            yield();
        }
    }
    return 0;
}
