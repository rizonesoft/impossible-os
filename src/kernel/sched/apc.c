/* ============================================================================
 * apc.c -- KAPC object type + per-thread APC queues + critical/guarded regions
 *
 * Data structures + queue insert/remove + region gating only. The DELIVERY
 * engine (KiDeliverApc) is a separate section. Mirrors dpc.c: caller-owned
 * intrusive objects, a per-thread irqsave spinlock, no allocation on insert.
 *
 * LIFECYCLE LOCK: the per-thread apc_lock guards BOTH the APC queues AND the
 * THREAD_DEAD/FREE transition that KeInsertQueueApc checks, so a cross-thread
 * insert cannot enqueue onto an exiting or reused thread. The same lock is
 * taken by KeInsertQueueApc, KeRemoveQueueApc, thread_exit's APC close, and
 * thread-slot reuse (see task.c).
 *
 * REGION COUNTERS are per-thread and OUTSIDE KAPC_STATE (which is swappable for
 * a future attach): a critical/guarded region must not be moved by attach.
 * Region APIs operate on thread_current() only (single-writer, no lock).
 * ============================================================================ */

#include "kernel/sched/apc.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"

/* ---- Per-thread APC-state init (thread create / slot reuse) -------------- */

void apc_thread_init(KAPC_STATE *apc_state, void *process)
{
    if (!apc_state)
        return;
    apc_state->apc_list_head[ApcKernelMode]  = (KAPC *)0;
    apc_state->apc_list_head[ApcUserMode]    = (KAPC *)0;
    apc_state->process                       = process;
    apc_state->kernel_apc_pending            = 0;
    apc_state->user_apc_pending              = 0;
    apc_state->special_user_apc_pending      = 0;
    apc_state->kernel_apc_in_progress        = 0;
}

/* ---- KeInitializeApc ----------------------------------------------------- */

void KeInitializeApc(KAPC *apc, void *thread, KAPC_ENVIRONMENT environment,
                     PKKERNEL_ROUTINE kernel_routine,
                     PKRUNDOWN_ROUTINE rundown_routine,
                     PKNORMAL_ROUTINE normal_routine, uint8_t apc_mode,
                     void *normal_context)
{
    if (!apc)
        return;
    apc->type            = APC_OBJECT_TYPE;
    apc->size            = (uint8_t)sizeof(KAPC);
    apc->apc_state_index = (uint8_t)environment;
    /* A special kernel APC (no NormalRoutine) is always KernelMode. */
    apc->apc_mode        = normal_routine ? apc_mode : (uint8_t)ApcKernelMode;
    apc->inserted        = 0;
    apc->thread          = thread;
    apc->next            = (KAPC *)0;
    apc->kernel_routine  = kernel_routine;
    apc->rundown_routine = rundown_routine;
    apc->normal_routine  = normal_routine;
    apc->normal_context  = normal_context;
    apc->system_arg1     = (void *)0;
    apc->system_arg2     = (void *)0;
}

/* ---- KeInsertQueueApc ---------------------------------------------------- */

int KeInsertQueueApc(KAPC *apc, void *system_arg1, void *system_arg2,
                     uint8_t increment)
{
    struct thread *t;
    uint64_t flags;
    int mode;

    /* increment = the priority boost applied when the target thread wakes to
     * run the APC; consumed by the delivery/wake path (separate section). */
    (void)increment;

    if (!apc || !apc->thread)
        return 0;
    t = (struct thread *)apc->thread;

    spin_lock_irqsave(&t->apc_lock, &flags);
    /* Under the lock: reject an exiting/reaped target or a double-insert. The
     * lock makes this check atomic vs thread_exit / slot reuse. */
    if (t->state == THREAD_DEAD || t->state == THREAD_FREE || apc->inserted) {
        spin_unlock_irqrestore(&t->apc_lock, flags);
        return 0;
    }
    apc->system_arg1 = system_arg1;
    apc->system_arg2 = system_arg2;
    mode = (apc->apc_mode == (uint8_t)ApcUserMode) ? ApcUserMode : ApcKernelMode;

    if (apc->normal_routine == (PKNORMAL_ROUTINE)0 && mode == ApcKernelMode) {
        /* Special kernel APC -> head of the kernel queue. */
        apc->next = t->apc_state.apc_list_head[ApcKernelMode];
        t->apc_state.apc_list_head[ApcKernelMode] = apc;
    } else {
        /* Normal kernel / user APC -> tail of the matching queue. */
        KAPC **pp = &t->apc_state.apc_list_head[mode];
        while (*pp)
            pp = &(*pp)->next;
        apc->next = (KAPC *)0;
        *pp = apc;
    }
    apc->inserted = 1;
    if (mode == ApcUserMode)
        t->apc_state.user_apc_pending = 1;
    else
        t->apc_state.kernel_apc_pending = 1;
    spin_unlock_irqrestore(&t->apc_lock, flags);
    return 1;
}

/* ---- KeRemoveQueueApc ---------------------------------------------------- */

int KeRemoveQueueApc(KAPC *apc)
{
    struct thread *t;
    uint64_t flags;
    int mode, found = 0;

    if (!apc || !apc->thread)
        return 0;
    t = (struct thread *)apc->thread;

    spin_lock_irqsave(&t->apc_lock, &flags);
    if (apc->inserted) {
        KAPC **pp;
        mode = (apc->apc_mode == (uint8_t)ApcUserMode) ? ApcUserMode : ApcKernelMode;
        pp = &t->apc_state.apc_list_head[mode];
        while (*pp && *pp != apc)
            pp = &(*pp)->next;
        if (*pp == apc) {
            *pp = apc->next;
            found = 1;
        }
        apc->next     = (KAPC *)0;
        apc->inserted = 0;
        /* Clear the pending flag if its queue is now empty. */
        if (!t->apc_state.apc_list_head[mode]) {
            if (mode == ApcUserMode)
                t->apc_state.user_apc_pending = 0;
            else
                t->apc_state.kernel_apc_pending = 0;
        }
    }
    spin_unlock_irqrestore(&t->apc_lock, flags);
    return found;
}

/* ---- Critical / guarded regions (current thread; single-writer) ---------- */

void KeEnterCriticalRegion(void)
{
    struct thread *t = thread_current();
    if (t)
        t->kernel_apc_disable++;
}

void KeLeaveCriticalRegion(void)
{
    struct thread *t = thread_current();
    /* Underflow-guarded. Deferred-APC delivery on leave is a delivery-engine
     * concern (separate section); here we only manage the nesting counter. */
    if (t && t->kernel_apc_disable > 0)
        t->kernel_apc_disable--;
}

void KeEnterGuardedRegion(void)
{
    struct thread *t = thread_current();
    if (t)
        t->special_apc_disable++;
}

void KeLeaveGuardedRegion(void)
{
    struct thread *t = thread_current();
    if (t && t->special_apc_disable > 0)
        t->special_apc_disable--;
}

int KeAreApcsDisabled(void)
{
    struct thread *t = thread_current();
    return t ? (t->kernel_apc_disable > 0 || t->special_apc_disable > 0) : 0;
}

int KeAreAllApcsDisabled(void)
{
    struct thread *t = thread_current();
    return t ? (t->special_apc_disable > 0) : 0;
}
