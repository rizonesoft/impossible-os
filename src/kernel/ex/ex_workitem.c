/* ============================================================================
 * ex_workitem.c -- EX_WORK_ITEM: immediate worker items (TODO-06 S12).
 *
 * Queues a caller-owned routine to run on the system worker thread (sys_wq) at
 * PASSIVE_LEVEL. A lock-free state machine layered over the bare workqueue (which
 * has NO dequeue) gives the cancel-vs-fire contract: ExCancelWorkItem wins only
 * while the item is QUEUED; once the worker flips it to RUNNING the cancel fails.
 *
 * State is touched ONLY through __atomic acquire/release/ACQ_REL so a terminal
 * observation on another CPU has an acquire edge to the routine's side effects.
 *
 * Delayed work + the EX_TIMER object set are deferred -- no multi-deadline timer
 * queue exists yet (the timer layer is a singleton one-shot). See ex.h S12.
 * ============================================================================ */

#include "kernel/ex.h"
#include "kernel/sched/workqueue.h"

static inline int32_t wi_load(const EX_WORK_ITEM *wi)
{
    return __atomic_load_n(&wi->State, __ATOMIC_ACQUIRE);
}

/* The workqueue trampoline: only the worker thread runs this, exactly once per
 * enqueued node. The node is the LAST reference to `wi` while the item is
 * QUEUED/CANCELLED (the workqueue has no dequeue), so this is also where a
 * cancelled item DRAINS: CANCELLED -> IDLE marks the stale node consumed and
 * makes the item free/re-queue-safe (see the lifetime contract in ex.h). */
static void ex_work_trampoline(void *arg)
{
    EX_WORK_ITEM *wi = (EX_WORK_ITEM *)arg;
    int32_t expected = EX_WI_QUEUED;

    if (__atomic_compare_exchange_n(&wi->State, &expected, EX_WI_RUNNING, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        wi->Routine(wi->Context);
        __atomic_store_n(&wi->State, EX_WI_DONE, __ATOMIC_RELEASE);
        return;
    }
    /* Not QUEUED -> this node was cancelled before dispatch. Drain it: the item
     * stays CANCELLED (and thus NOT re-queueable) until this stale node runs, so
     * `expected` is CANCELLED here; flip it to the drained IDLE state. */
    if (expected == EX_WI_CANCELLED)
        __atomic_compare_exchange_n(&wi->State, &expected, EX_WI_IDLE, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

void ExInitializeWorkItem(EX_WORK_ITEM *wi, EX_WORKER_ROUTINE routine, void *context)
{
    if (!wi)
        return;
    wi->Routine = routine;
    wi->Context = context;
    __atomic_store_n(&wi->State, EX_WI_IDLE, __ATOMIC_RELEASE);
}

int ExQueueWorkItem(EX_WORK_ITEM *wi)
{
    int32_t cur;

    if (!wi || !wi->Routine || !sys_wq)
        return -1;

    /* Only IDLE or DONE are re-queueable. A CANCELLED item is NOT (a stale,
     * undequeueable node may still be pending -- re-queueing would create a
     * second node for one item); QUEUED/RUNNING are already active. */
    cur = wi_load(wi);
    for (;;) {
        if (cur != EX_WI_IDLE && cur != EX_WI_DONE)
            return -1;   /* active or cancel-pending */
        if (__atomic_compare_exchange_n(&wi->State, &cur, EX_WI_QUEUED, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
        /* CAS failed -- cur reloaded with the live state; retry. */
    }

    /* The workqueue has a fixed free-node pool; enqueue can fail. On failure NO
     * node was published, so the trampoline will never drain this item -- we must
     * return it to IDLE ourselves. It is QUEUED (ours) unless a concurrent cancel
     * raced it to CANCELLED; in BOTH cases there is no pending node, so drain
     * straight to IDLE so the caller can retry / free. */
    if (workqueue_enqueue(sys_wq, ex_work_trampoline, wi) == 0) {
        int32_t e = EX_WI_QUEUED;
        if (!__atomic_compare_exchange_n(&wi->State, &e, EX_WI_IDLE, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            int32_t c = EX_WI_CANCELLED;   /* concurrent cancel; no node exists */
            __atomic_compare_exchange_n(&wi->State, &c, EX_WI_IDLE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
        return -1;   /* worker queue full */
    }
    return 0;
}

bool ExCancelWorkItem(EX_WORK_ITEM *wi)
{
    int32_t expected = EX_WI_QUEUED;
    if (!wi)
        return false;
    /* Cancel wins only from QUEUED (the worker has not yet claimed it). */
    return __atomic_compare_exchange_n(&wi->State, &expected, EX_WI_CANCELLED, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

EX_WORK_ITEM_STATE ExpWorkItemState(const EX_WORK_ITEM *wi)
{
    return wi ? (EX_WORK_ITEM_STATE)wi_load(wi) : EX_WI_IDLE;
}
