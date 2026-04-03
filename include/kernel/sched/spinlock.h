/* ============================================================================
 * spinlock.h -- IRQ-safe spinlock primitives
 *
 * Spinlocks are the ONLY correct synchronization primitive inside interrupt
 * handlers -- they busy-wait using atomic CAS without calling yield() or the
 * scheduler.  Must only be held for very short durations (< ~100 ns).
 *
 * IRQ safety rules
 * ----------------
 *   - ALWAYS use spin_lock_irqsave / spin_unlock_irqrestore in any code
 *     that can be called from BOTH thread context and IRQ context.
 *   - spin_lock / spin_unlock may be used when the caller is certain it
 *     runs in IRQ context only (IRQs already disabled by the CPU).
 *   - NEVER call yield(), kmalloc(), printk(), or any blocking function
 *     while holding a spinlock.
 *   - NEVER sleep inside a spinlock critical section.
 *   - NEVER hold a spinlock across a mutex_lock() or sem_wait() call.
 *
 * Memory barriers (from barrier.h)
 * ----------------------------------
 *   barrier() is inserted:
 *     1. In the CAS spin loop body -- prevents GCC from CSE-ing the flag read
 *        into a register and spinning forever on a cached value.
 *     2. After acquiring the lock (acquire fence) -- prevents critical-section
 *        loads from being hoisted before the CAS.
 *     3. Before releasing the lock (release fence) -- prevents critical-section
 *        stores from being sunk after the flag is cleared.
 *   On x86, LOCK CMPXCHG carries an implicit full hardware barrier, so these
 *   are compiler-only fences.  When CONFIG_SMP is defined, smp_mb() will emit
 *   the hardware fence need on non-x86 ISAs.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/barrier.h"

/* ---------------------------------------------------------------------------
 * spinlock_t -- a single volatile flag word.
 * 0 = unlocked, 1 = locked.
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t flag;
} spinlock_t;

#define SPINLOCK_INIT { .flag = 0 }

/* Static initializer helper */
#define DEFINE_SPINLOCK(name)  spinlock_t name = SPINLOCK_INIT

/* ---------------------------------------------------------------------------
 * spin_lock(s)   -- disable IRQs then spin until acquired (unconditional cli)
 * spin_unlock(s) -- release flag then re-enable IRQs (unconditional sti)
 *
 * Use these only when the caller is always in IRQ context (IRQs already
 * off) OR when the caller guarantees no nesting.  In general, prefer the
 * irqsave variants below.
 * ------------------------------------------------------------------------- */
void spin_lock(spinlock_t *s);
void spin_unlock(spinlock_t *s);

/* ---------------------------------------------------------------------------
 * spin_lock_irqsave(s, flags)          save RFLAGS → disable IRQs → acquire
 * spin_unlock_irqrestore(s, flags)     release → restore saved RFLAGS
 *
 * flags must be a uint64_t allocated by the caller:
 *   uint64_t irq_flags;
 *   spin_lock_irqsave(&lock, &irq_flags);
 *   ...
 *   spin_unlock_irqrestore(&lock, irq_flags);
 *
 * This preserves the pre-lock interrupt state.  Correct even when called
 * from inside another IRQ handler where IRQs are already disabled -- the
 * restore will NOT re-enable them because IF was 0 in the saved RFLAGS.
 * ------------------------------------------------------------------------- */
void spin_lock_irqsave(spinlock_t *s, uint64_t *flags);
void spin_unlock_irqrestore(spinlock_t *s, uint64_t flags);

/* ---------------------------------------------------------------------------
 * spin_trylock(s) -- non-blocking attempt, returns 1 on success, 0 on fail.
 * Does NOT disable IRQs -- caller is responsible for IRQ safety.
 * ------------------------------------------------------------------------- */
int spin_trylock(spinlock_t *s);

/* ---------------------------------------------------------------------------
 * spin_is_locked(s) -- returns 1 if currently locked (debug / assertions).
 * ------------------------------------------------------------------------- */
static inline int spin_is_locked(const spinlock_t *s)
{
    return __atomic_load_n(&s->flag, __ATOMIC_ACQUIRE) != 0;
}
