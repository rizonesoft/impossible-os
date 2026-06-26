/* ============================================================================
 * kinterrupt.h -- KINTERRUPT sync object + KeSynchronizeExecution
 *
 * NT-style per-interrupt synchronization. A driver binds a KINTERRUPT to its
 * IDT vector; DPC/thread code that touches ISR-shared device state then runs
 * that access through KeSynchronizeExecution, which raises to the interrupt's
 * SynchronizeIrql (its DIRQL) and takes the interrupt's spinlock -- mutually
 * exclusive with the ISR, which the irq.c dispatcher runs under the SAME lock.
 *
 *   driver init:   KeInitializeInterrupt(&ki, vector); KeConnectInterrupt(&ki);
 *   ISR (DIRQL):   runs under ki->lock automatically (dispatcher-acquired)
 *   thread/DPC:    KeSynchronizeExecution(&ki, routine, ctx)  // safe touch
 *
 * SCOPE: this owns the per-interrupt SynchronizeIrql + lock contract only.
 * GSI/vector routing + affinity are owned by the interrupt-arch work
 * (irq_request_gsi, the interrupt-routing work). The KINTERRUPT lock is for
 * SHORT ISR-shared register/state windows; it is NOT a general driver
 * serialization primitive (do not run allocation / paging / logging under it).
 *
 * SELF-ISR PRECONDITION: KeSynchronizeExecution must NOT be called from inside
 * the interrupt's own ISR -- the dispatcher already holds the lock, so a
 * re-acquire would deadlock. The dispatcher records the CPU running the ISR
 * (active_cpu); a self-call is detected and rejected (returns FALSE + bumps a
 * lock-free counter, not a klog -- serial I/O is forbidden under the lock at
 * DIRQL) rather than deadlocking. Like NT, this is documented caller misuse.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/irql.h"
#include "kernel/sched/spinlock.h"

/* SynchronizeRoutine: runs at SynchronizeIrql under the interrupt lock.
 * Returns a BOOLEAN that KeSynchronizeExecution propagates to its caller. */
typedef int (*PKSYNCHRONIZE_ROUTINE)(void *synchronize_context);

typedef struct _KINTERRUPT {
    uint8_t           vector;       /* IDT vector this object guards */
    KIRQL             sync_irql;    /* SynchronizeIrql = vector_to_irql(vector) */
    spinlock_t        lock;         /* per-interrupt spinlock (ISR + KeSync) */
    volatile uint32_t active_cpu;   /* CPU running the ISR under lock; MAX_CPUS = none */
    uint8_t           connected;    /* 1 = bound into the irq.c dispatch path */
} KINTERRUPT;

/* Initialize a KINTERRUPT for an IDT vector. Caller-owned (static/pool). */
void KeInitializeInterrupt(KINTERRUPT *interrupt, uint8_t vector);

/* Bind the KINTERRUPT into the irq.c dispatch path so the dispatcher runs the
 * vector's ISR under interrupt->lock (enabling KeSynchronizeExecution to
 * synchronize with it). Returns 1 on success, 0 if the vector is out of range.
 * Call after the driver has registered its ISR on that vector. */
int KeConnectInterrupt(KINTERRUPT *interrupt);

/* Unbind (driver teardown). Safe to call at PASSIVE_LEVEL.
 * PRECONDITION: the interrupt source must already be masked/quiesced (the
 * device stopped) so no ISR is in flight or pending -- exactly like NT's
 * IoDisconnectInterrupt, which is called after the device is stopped. This
 * call unpublishes the binding and drains any dispatcher currently holding the
 * lock, but it canNOT recall a dispatcher that loaded the pointer before the
 * device was quiesced; honoring the precondition is what makes it safe to free
 * the caller-owned KINTERRUPT afterward. (A full mask+drain barrier for
 * hot-unplug of a LIVE interrupt is a tracked follow-up.) */
void KeDisconnectInterrupt(KINTERRUPT *interrupt);

/* Diagnostic: count of KeSynchronizeExecution self-ISR rejections (caller
 * misuse). Lock-free; the reject path bumps this instead of logging because the
 * dispatcher may hold the interrupt lock at DIRQL on that path. */
uint32_t KeGetSelfIsrRejectCount(void);

/* Run routine(ctx) at the interrupt's SynchronizeIrql under its lock, mutually
 * exclusive with the ISR. Returns the routine's BOOLEAN. If called from inside
 * the interrupt's own ISR (active_cpu == this CPU) it returns FALSE and bumps a
 * lock-free counter (KeGetSelfIsrRejectCount) -- see the SELF-ISR PRECONDITION
 * above. Callable at IRQL <= DISPATCH_LEVEL. */
int KeSynchronizeExecution(KINTERRUPT *interrupt,
                           PKSYNCHRONIZE_ROUTINE routine, void *context);
