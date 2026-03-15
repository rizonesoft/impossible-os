/* ============================================================================
 * spinlock.h — IRQ-safe spinlock primitives
 *
 * Spinlocks are the ONLY correct synchronization primitive inside interrupt
 * handlers — they busy-wait using atomic CAS without calling yield() or the
 * scheduler.  They must be held for very short durations (< ~100 ns).
 *
 * IRQ safety
 * ----------
 *   spin_lock(s)            — disable IRQs, spin until acquired
 *   spin_unlock(s)          — release flag, restore IRQs
 *   spin_lock_irqsave(s,f)  — save RFLAGS, disable IRQs, spin until acquired
 *   spin_unlock_irqrestore(s,f) — release flag, restore saved RFLAGS
 *
 * Memory barriers
 * ---------------
 *   barrier.h is included by this header.  barrier() is called:
 *     - In the spin loop body to prevent the compiler from caching the lock
 *       flag read in a register (CSE elimination).
 *     - After acquiring the lock (acquire semantics) so that critical-section
 *       loads are not hoisted before the lock is observed as held.
 *     - Before releasing the lock (release semantics) so that critical-section
 *       stores are not sunk after the lock flag is cleared.
 *
 *   On x86 TSO, MFENCE is not required on the acquire / release path because
 *   x86 guarantees that all prior stores are ordered before later loads.
 *   barrier() (compiler fence only) is sufficient for single-core.  When
 *   CONFIG_SMP is added, the CAS instruction (LOCK XCHG / LOCK CMPXCHG)
 *   already carries an implicit full hardware barrier on x86, so smp_mb()
 *   maps to barrier() on the release path and the LOCK prefix handles
 *   acquire ordering.
 *
 * NOTE: This header is a forward declaration / API stub.
 *       Implementation will be added in §6 Spinlocks (TODO-020 §6) once the
 *       IRQ save/restore helpers are available.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/barrier.h"   /* barrier(), mb(), smp_mb() */

/* Spinlock type — a single volatile flag. */
typedef struct {
    volatile uint32_t flag;  /* 0 = unlocked, 1 = locked */
} spinlock_t;

#define SPINLOCK_INIT { .flag = 0 }

/* ---------------------------------------------------------------------------
 * spin_lock(s)
 *
 * Disable IRQs on the current CPU, then spin (busy-wait) until the lock is
 * acquired via atomic CAS.  Uses barrier() in the spin body to prevent the
 * compiler from hoisting the flag read out of the loop.
 *
 * Acquire barrier: barrier() after successful CAS ensures that loads inside
 * the critical section are not reordered before the lock acquisition.
 * Implementation lives in src/kernel/sched/spinlock.c (TODO-020 §6).
 * ------------------------------------------------------------------------- */
void spin_lock(spinlock_t *s);

/* spin_unlock(s) — release flag, restore IRQs.
 * Release barrier: barrier() before clearing flag ensures critical-section
 * stores are globally visible before the lock is released. */
void spin_unlock(spinlock_t *s);

/* spin_lock_irqsave(s, flags) — save RFLAGS (reading IF bit), disable IRQs,
 * acquire lock.  Pass a uint64_t variable for flags. */
void spin_lock_irqsave(spinlock_t *s, uint64_t *flags);

/* spin_unlock_irqrestore(s, flags) — release lock then restore RFLAGS. */
void spin_unlock_irqrestore(spinlock_t *s, uint64_t flags);

/* spin_trylock(s) — attempt to acquire without blocking.
 * Returns 1 on success (lock acquired), 0 on failure. */
int spin_trylock(spinlock_t *s);

/* spin_is_locked(s) — returns 1 if locked (for debug / assertions). */
static inline int spin_is_locked(const spinlock_t *s) {
    return s->flag != 0;
}
