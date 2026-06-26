/* ============================================================================
 * kinterrupt.c -- KINTERRUPT sync object + KeSynchronizeExecution
 *
 * See include/kernel/sched/kinterrupt.h for the contract. The per-interrupt
 * lock + active_cpu live in the KINTERRUPT object; the irq.c dispatcher runs
 * the bound vector's ISR under that lock (irq_bind_kinterrupt), so a thread/DPC
 * calling KeSynchronizeExecution is mutually exclusive with the ISR.
 * ============================================================================ */

#include "kernel/sched/kinterrupt.h"
#include "kernel/irq.h"
#include "kernel/smp.h"

/* Lock-free diagnostic counter for self-ISR misuse (KeSynchronizeExecution
 * called from inside the interrupt's own ISR). We must NOT klog on that path:
 * the dispatcher may hold ki->lock at DIRQL, so serial I/O there would violate
 * the spinlock hold-time rule. Read it via KeGetSelfIsrRejectCount(). */
static volatile uint32_t s_self_isr_rejects;

uint32_t KeGetSelfIsrRejectCount(void)
{
    return __atomic_load_n(&s_self_isr_rejects, __ATOMIC_RELAXED);
}

void KeInitializeInterrupt(KINTERRUPT *interrupt, uint8_t vector)
{
    if (!interrupt)
        return;
    interrupt->vector     = vector;
    interrupt->sync_irql  = vector_to_irql(vector);
    interrupt->lock.flag  = 0;
    __atomic_store_n(&interrupt->active_cpu, MAX_CPUS, __ATOMIC_RELEASE); /* no ISR */
    interrupt->connected  = 0;
}

int KeConnectInterrupt(KINTERRUPT *interrupt)
{
    if (!interrupt)
        return 0;
    if (irq_bind_kinterrupt(interrupt->vector, interrupt) != IRQ_OK)
        return 0;
    interrupt->connected = 1;
    return 1;
}

void KeDisconnectInterrupt(KINTERRUPT *interrupt)
{
    uint64_t flags;
    if (!interrupt)
        return;
    /* Unpublish first: no NEW dispatch will acquire-load the pointer. */
    irq_unbind_kinterrupt(interrupt->vector, interrupt);
    /* Best-effort teardown barrier: acquire+release the lock to drain any
     * dispatcher currently holding it (about to clear active_cpu / unlock).
     * The residual window -- a dispatcher that already acquire-loaded the
     * pointer but has NOT yet taken the lock -- is closed by the caller
     * PRECONDITION (the interrupt source must already be masked/quiesced so no
     * ISR is in flight or pending; see kinterrupt.h). A full mask+drain barrier
     * for hot-unplug of a LIVE interrupt is tracked as a deferred item. */
    spin_lock_irqsave(&interrupt->lock, &flags);
    spin_unlock_irqrestore(&interrupt->lock, flags);
    interrupt->connected = 0;
}

int KeSynchronizeExecution(KINTERRUPT *interrupt,
                           PKSYNCHRONIZE_ROUTINE routine, void *context)
{
    KIRQL    old;
    uint64_t flags;
    int      result;
    uint32_t me;
    struct per_cpu_data *cpu;

    if (!interrupt || !routine)
        return 0;

    cpu = smp_this_cpu();
    if (!cpu)
        return 0;                /* GS_BASE not yet live -- no valid CPU context */
    me = cpu->cpu_id;

    /* Self-ISR guard: the dispatcher holds interrupt->lock while this vector's
     * ISR runs on this CPU, so re-acquiring it from inside the ISR would
     * deadlock. Detect (active_cpu == me) and reject rather than deadlock --
     * documented caller misuse, like NT. The read is lock-free; in the self
     * case it is the same CPU's own prior store, so program order suffices.
     * Bump a lock-free counter instead of klog'ing: on the real self-ISR path
     * the dispatcher holds ki->lock at DIRQL, where serial I/O is forbidden. */
    if (__atomic_load_n(&interrupt->active_cpu, __ATOMIC_ACQUIRE) == me) {
        __atomic_fetch_add(&s_self_isr_rejects, 1u, __ATOMIC_RELAXED);
        return 0;
    }

    /* Run at the interrupt's SynchronizeIrql under its lock: mutually exclusive
     * with the ISR (dispatcher holds the same lock). KeRaiseIrql sets the
     * software IRQL to DIRQL; spin_lock_irqsave then cli + acquires (and will
     * not lower the already-higher IRQL). */
    KeRaiseIrql(interrupt->sync_irql, &old);
    spin_lock_irqsave(&interrupt->lock, &flags);
    result = routine(context);
    spin_unlock_irqrestore(&interrupt->lock, flags);
    KeLowerIrql(old);
    return result;
}
