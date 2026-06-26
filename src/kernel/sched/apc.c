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
#include "kernel/sched/irql.h"
#include "kernel/sched/dpc_config.h"
#include "kernel/smp.h"
#include "kernel/klog.h"

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
    apc_state->kernel_apc_depth              = 0;
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
    int warn_starvation = 0;

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
    if (mode == ApcUserMode) {
        t->apc_state.user_apc_pending = 1;
    } else {
        t->apc_state.kernel_apc_pending = 1;
        /* APC starvation watchdog: warn ONCE, only when THIS kernel-APC insert
         * crosses the threshold (depth becomes exactly the warn level). A user
         * APC insert -- which does not touch kernel_apc_depth -- must never
         * re-warn a thread already sitting at the threshold. */
        if (++t->apc_state.kernel_apc_depth == APC_STARVATION_WARN_DEPTH)
            warn_starvation = 1;
    }
    spin_unlock_irqrestore(&t->apc_lock, flags);
    if (warn_starvation)
        klog(LOG_WARN, "apc",
             "starvation watchdog: kernel APC queue depth reached %u on a thread",
             (uint64_t)APC_STARVATION_WARN_DEPTH);
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
            if (mode == ApcKernelMode && t->apc_state.kernel_apc_depth > 0)
                t->apc_state.kernel_apc_depth--;   /* APC starvation watchdog */
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
    /* Saturate rather than wrap: a wrapped (negative) counter would fail OPEN
     * (KeAreApcsDisabled's >0 check would read FALSE inside a still-nested
     * region). INT32_MAX nesting is physically unreachable; the cap is defense. */
    if (t && t->kernel_apc_disable < 0x7FFFFFFF)
        t->kernel_apc_disable++;
}

void KeLeaveCriticalRegion(void)
{
    struct thread *t = thread_current();
    /* Underflow-guarded. On the transition OUT of the last critical region,
     * deliver any kernel APCs that were blocked while inside it (NT semantics:
     * KeLeaveCriticalRegion drains deferred APCs). KiDeliverApc re-checks each
     * APC's region gating, so gate here only on: fully out of critical region,
     * not in a guarded region, an APC actually pending, and below APC_LEVEL. */
    if (t && t->kernel_apc_disable > 0) {
        t->kernel_apc_disable--;
        if (t->kernel_apc_disable == 0 && t->special_apc_disable == 0 &&
            t->apc_state.kernel_apc_pending &&
            KeGetCurrentIrql() < APC_LEVEL)
            KiDeliverApc((uint8_t)ApcKernelMode, (void *)0, (void *)0);
    }
}

void KeEnterGuardedRegion(void)
{
    struct thread *t = thread_current();
    if (t && t->special_apc_disable < 0x7FFFFFFF)   /* saturate (see above) */
        t->special_apc_disable++;
}

void KeLeaveGuardedRegion(void)
{
    struct thread *t = thread_current();
    /* On the transition OUT of the last guarded region, deliver kernel APCs
     * blocked by it (special APCs become deliverable even if still inside a
     * critical region; KiDeliverApc applies the per-APC critical-region check
     * to normal APCs). Gate on: fully out of guarded region, an APC pending,
     * and below APC_LEVEL. */
    if (t && t->special_apc_disable > 0) {
        t->special_apc_disable--;
        if (t->special_apc_disable == 0 &&
            t->apc_state.kernel_apc_pending &&
            KeGetCurrentIrql() < APC_LEVEL)
            KiDeliverApc((uint8_t)ApcKernelMode, (void *)0, (void *)0);
    }
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

/* ---- Delivery engine (KiDeliverApc) ------------------------------------- */

/* Delivery counters (SMP: any CPU may deliver; relaxed atomics -- diagnostics
 * only, no ordering dependency). User stays 0 until user-APC delivery ships. */
static uint64_t s_apc_kernel_delivered;
static uint64_t s_apc_user_delivered;

void apc_delivery_stats(uint64_t *kernel_delivered, uint64_t *user_delivered)
{
    if (kernel_delivered)
        *kernel_delivered = __atomic_load_n(&s_apc_kernel_delivered, __ATOMIC_RELAXED);
    if (user_delivered)
        *user_delivered = __atomic_load_n(&s_apc_user_delivered, __ATOMIC_RELAXED);
}

void KiDeliverApc(uint8_t previous_mode, void *exception_frame, void *trap_frame)
{
    struct thread      *t    = thread_current();
    struct per_cpu_data *pcpu = smp_this_cpu();
    KIRQL                entry_irql;
    uint64_t             flags;

    /* USER-mode APC delivery (redirect ring-3 to KiUserApcDispatcher via a
     * patched trap frame) is deferred: it needs the user-mode dispatcher and
     * trap-frame editing that do not exist yet. This engine delivers KERNEL
     * APCs only, so the NT user-delivery parameters are currently unused. */
    (void)previous_mode;
    (void)exception_frame;
    (void)trap_frame;

    if (!t || !pcpu)
        return;
    entry_irql = pcpu->current_irql;

    /* Drain the kernel APC queue one at a time. Each APC is dequeued UNDER the
     * lock; its routines run AFTER unlock (a KernelRoutine may free the KAPC or
     * call back into APC APIs and must never run holding apc_lock). */
    for (;;) {
        KAPC             *apc;
        PKKERNEL_ROUTINE  krout;
        PKNORMAL_ROUTINE  nrout;
        void             *nctx, *sa1, *sa2;
        int               is_special;

        spin_lock_irqsave(&t->apc_lock, &flags);
        apc = t->apc_state.apc_list_head[ApcKernelMode];
        if (!apc) {
            t->apc_state.kernel_apc_pending = 0;
            spin_unlock_irqrestore(&t->apc_lock, flags);
            break;
        }
        is_special = (apc->normal_routine == (PKNORMAL_ROUTINE)0);

        /* A guarded region (special_apc_disable) blocks ALL kernel APCs incl
         * special; stop entirely until it is left. A critical region
         * (kernel_apc_disable) or an in-progress normal APC blocks only normal
         * APCs -- a special APC at the head still delivers, but a normal one
         * cannot, so stop (the head is FIFO and the next item may be normal).
         * NOTE: the region counters are read here under apc_lock but WRITTEN
         * lock-free by KeEnter/LeaveCriticalRegion on thread_current(); this is
         * race-free only because delivery + the region writers are the SAME
         * thread on the same CPU (the lock guards the queues, not these). */
        if (t->special_apc_disable > 0) {
            spin_unlock_irqrestore(&t->apc_lock, flags);
            break;
        }
        if (!is_special &&
            (t->kernel_apc_disable > 0 || t->apc_state.kernel_apc_in_progress)) {
            spin_unlock_irqrestore(&t->apc_lock, flags);
            break;
        }

        /* Dequeue from the head. */
        t->apc_state.apc_list_head[ApcKernelMode] = apc->next;
        apc->next     = (KAPC *)0;
        apc->inserted = 0;
        if (t->apc_state.kernel_apc_depth > 0)
            t->apc_state.kernel_apc_depth--;       /* APC starvation watchdog */
        if (!t->apc_state.apc_list_head[ApcKernelMode])
            t->apc_state.kernel_apc_pending = 0;

        krout = apc->kernel_routine;
        nrout = apc->normal_routine;
        nctx  = apc->normal_context;
        sa1   = apc->system_arg1;
        sa2   = apc->system_arg2;
        if (!is_special)
            t->apc_state.kernel_apc_in_progress = 1;
        spin_unlock_irqrestore(&t->apc_lock, flags);

        /* Per the NT contract, KernelRoutine runs at APC_LEVEL and NormalRoutine
         * at PASSIVE_LEVEL. APC_LEVEL and PASSIVE_LEVEL both map to LAPIC TPR
         * 0x00, so these are software-only IRQL moves (no TPR write); set
         * current_irql directly rather than via KeRaiseIrql/KeLowerIrql, which
         * would re-enter the delivery path. Re-resolve smp_this_cpu() at each
         * write rather than reusing the entry `pcpu`: a NormalRoutine may yield
         * and (under a future per-CPU-run-queue scheduler) the thread could
         * resume on another CPU, so the IRQL bookkeeping must land on whatever
         * CPU is current now. entry_irql is PASSIVE on the real lower-path entry,
         * so restoring it on a migrated-to CPU is still correct. */
        smp_this_cpu()->current_irql = APC_LEVEL;
        /* KernelRoutine: the cleanup hook. It may free the KAPC and may rewrite
         * the NormalRoutine / context / args. After it runs, `apc` may be
         * dangling -- do not touch it again. */
        if (krout)
            krout(apc, (void **)&nrout, &nctx, &sa1, &sa2);

        /* NormalRoutine is the deferred work (kernel APC at PASSIVE_LEVEL). */
        smp_this_cpu()->current_irql = PASSIVE_LEVEL;
        if (!is_special && nrout)
            nrout(nctx, sa1, sa2);

        /* Restore the IRQL we were entered at before the next iteration. */
        smp_this_cpu()->current_irql = entry_irql;

        if (!is_special) {
            spin_lock_irqsave(&t->apc_lock, &flags);
            t->apc_state.kernel_apc_in_progress = 0;
            spin_unlock_irqrestore(&t->apc_lock, flags);
        }
        __atomic_fetch_add(&s_apc_kernel_delivered, 1, __ATOMIC_RELAXED);
    }
}

/* ---- Thread-exit / reap rundown ----------------------------------------- */

void apc_rundown_thread(struct thread *t)
{
    uint64_t flags;
    KAPC    *lists[2];
    int      m;

    if (!t)
        return;

    /* The caller already published THREAD_DEAD/THREAD_FREE under apc_lock, so
     * KeInsertQueueApc rejects new inserts. Detach both queues + clear pending
     * UNDER the lock; run rundown routines AFTER unlock (a rundown_routine may
     * free the KAPC and must not re-enter apc_lock). */
    spin_lock_irqsave(&t->apc_lock, &flags);
    lists[ApcKernelMode] = t->apc_state.apc_list_head[ApcKernelMode];
    lists[ApcUserMode]   = t->apc_state.apc_list_head[ApcUserMode];
    t->apc_state.apc_list_head[ApcKernelMode] = (KAPC *)0;
    t->apc_state.apc_list_head[ApcUserMode]   = (KAPC *)0;
    t->apc_state.kernel_apc_pending       = 0;
    t->apc_state.user_apc_pending         = 0;
    t->apc_state.special_user_apc_pending = 0;
    t->apc_state.kernel_apc_depth         = 0;   /* queues emptied below */
    for (m = 0; m < 2; m++) {
        KAPC *a;
        for (a = lists[m]; a; a = a->next)
            a->inserted = 0;
    }
    spin_unlock_irqrestore(&t->apc_lock, flags);

    for (m = 0; m < 2; m++) {
        KAPC *a = lists[m];
        while (a) {
            KAPC *next = a->next;     /* read before rundown frees the KAPC */
            a->next = (KAPC *)0;
            if (a->rundown_routine)
                a->rundown_routine(a);
            a = next;
        }
    }
}
