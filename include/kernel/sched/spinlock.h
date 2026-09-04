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
 * spin_tryunlock(s) -- the counterpart to spin_trylock: release the flag and
 * NOTHING else.
 *
 * spin_unlock() is NOT the right pairing. It ends with an unconditional
 * irq_enable(), and spin_trylock never disabled interrupts in the first place,
 * so pairing them turns "acquire a lock" into "enable interrupts" -- fatal in
 * a panic path that acquired the lock precisely because interrupts must stay
 * masked. spin_unlock_irqrestore() is equally wrong: it also lowers the
 * per-CPU IRQL that spin_trylock never raised.
 *
 * The release fence matches spin_trylock's __ATOMIC_ACQ_REL acquire, so the
 * critical section's stores are visible before the flag clears. IRQ state is
 * entirely the caller's business, exactly as it is on the acquire side.
 * ------------------------------------------------------------------------- */
static inline void spin_tryunlock(spinlock_t *s)
{
    __atomic_store_n(&s->flag, 0, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------------------
 * spin_is_locked(s) -- returns 1 if currently locked (debug / assertions).
 * ------------------------------------------------------------------------- */
static inline int spin_is_locked(const spinlock_t *s)
{
    return __atomic_load_n(&s->flag, __ATOMIC_ACQUIRE) != 0;
}

/* ---------------------------------------------------------------------------
 * local_irq_save / local_irq_restore -- local interrupt masking WITHOUT
 * touching software IRQL (unlike spin_lock_irqsave, which also raises IRQL to
 * DISPATCH_LEVEL).
 *
 * Use when code must exclude same-CPU interrupt reentry around a short critical
 * region but must NOT raise IRQL -- e.g. guarding a non-reentrant heap call
 * that is itself illegal at DISPATCH_LEVEL. Does NOT serialize across CPUs.
 * ARCH: x86-64 -- will move to arch/ with the rest of the CPU primitives.
 * ------------------------------------------------------------------------- */
static inline uint64_t local_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq\n\t popq %0\n\t cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void local_irq_restore(uint64_t flags)
{
    __asm__ volatile("pushq %0\n\t popfq" :: "r"(flags) : "memory", "cc");
}

/* ---------------------------------------------------------------------------
 * irqs_enabled() -- non-zero when maskable interrupts are currently deliverable
 * on this CPU (RFLAGS.IF set).
 *
 * A predicate, not a mask: it changes nothing. It exists so a caller whose
 * contract requires interrupts to be ON can REFUSE up front rather than
 * misbehave. The concrete case is an elapsed-time measurement taken against
 * mono_ns(): when the active clock source is tick-derived it stops advancing
 * while interrupts are masked, so the measurement silently becomes fiction
 * rather than failing loudly.
 *
 * Only the IF=0 answer is durable -- nothing can turn interrupts ON underneath
 * a caller that has them off, whereas an IF=1 answer can be stale the moment it
 * is read. Do not build mutual exclusion on it.
 * ARCH: x86-64 -- will move to arch/ with the rest of the CPU primitives.
 * ------------------------------------------------------------------------- */
#define RFLAGS_IF_BIT 0x200u   /* RFLAGS.IF (Intel SDM Vol. 3A, 2.3) */

static inline int irqs_enabled(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq\n\t popq %0" : "=r"(flags) :: "memory");
    return (flags & RFLAGS_IF_BIT) != 0;
}
