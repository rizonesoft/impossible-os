/* ============================================================================
 * spinlock.c — IRQ-safe spinlock primitives
 *
 * All four functions manipulate CPU interrupt state:
 *   spin_lock / spin_unlock      — unconditional cli/sti
 *   spin_lock_irqsave / spin_unlock_irqrestore — save/restore RFLAGS.IF
 *
 * Memory ordering (see barrier.h):
 *   - spin body:   barrier() prevents GCC from caching the flag read (CSE)
 *   - on acquire:  barrier() prevents critical-section loads from being
 *                  hoisted before the successful CAS
 *   - on release:  barrier() prevents critical-section stores from being
 *                  sunk after the flag is cleared
 *   On x86, LOCK XCHG/CMPXCHG carry an implicit full hardware barrier, so
 *   we only need compiler barriers here for the acquire/release fence.
 * ============================================================================ */

#include "kernel/sched/spinlock.h"

/* ---------------------------------------------------------------------------
 * irq_disable / irq_restore — thin wrappers around CLI and STI / POPFQ
 * ------------------------------------------------------------------------- */

static inline void irq_disable(void)
{
    __asm__ volatile("cli" ::: "memory");
}

static inline void irq_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

/* Save current RFLAGS and disable interrupts atomically. */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

/* Restore RFLAGS from saved value (re-enables IRQs only if they were on). */
static inline void irq_restore(uint64_t flags)
{
    __asm__ volatile(
        "pushq %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

/* ---------------------------------------------------------------------------
 * cas_acquire — atomic compare-and-swap on a uint32_t.
 * Returns 1 on success (lock was 0, now 1), 0 on failure.
 *
 * Uses GCC __atomic_compare_exchange_n with ACQ_REL ordering so GCC knows
 * the operation has both acquire and release semantics.  On x86 the LOCK
 * CMPXCHG instruction already serialises the pipeline.
 * ------------------------------------------------------------------------- */
static inline int cas_acquire(volatile uint32_t *ptr,
                              uint32_t expected,
                              uint32_t desired)
{
    return __atomic_compare_exchange_n(ptr, &expected, desired,
                                       /*weak=*/0,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

/* ---------------------------------------------------------------------------
 * spin_lock(s) — disable interrupts then spin until lock acquired
 *
 * IRQ safety: CLI before the spin means we cannot be preempted by an IRQ
 * that might try to acquire the same lock (which would deadlock).
 *
 * HOLD-TIME RULE: Release the lock before doing ANY work that could take
 * more than ~100 ns.  Never call printk, kmalloc, or yield while holding.
 * ------------------------------------------------------------------------- */
void spin_lock(spinlock_t *s)
{
    irq_disable();
    while (!cas_acquire(&s->flag, 0, 1))
        barrier();  /* spin: prevent GCC from CSE-ing the flag read */
    barrier();      /* acquire fence: prevent hoisting of critical section */
}

/* ---------------------------------------------------------------------------
 * spin_unlock(s) — release flag then restore interrupts
 * ------------------------------------------------------------------------- */
void spin_unlock(spinlock_t *s)
{
    barrier();              /* release fence: all CS stores visible before clear */
    s->flag = 0;
    irq_enable();
}

/* ---------------------------------------------------------------------------
 * spin_lock_irqsave(s, flags) — save RFLAGS, disable IRQs, acquire lock
 *
 * Saves the full RFLAGS register (including IF bit) before disabling IRQs.
 * Essential for nested callers: if a function is called from both thread
 * context (IRQs on) and IRQ context (IRQs already off), blindly calling
 * sti in the paired unlock is wrong.  irqsave/irqrestore preserves whatever
 * state was active before this call.
 * ------------------------------------------------------------------------- */
void spin_lock_irqsave(spinlock_t *s, uint64_t *flags)
{
    *flags = irq_save();   /* save RFLAGS + cli atomically */
    while (!cas_acquire(&s->flag, 0, 1))
        barrier();
    barrier();
}

/* ---------------------------------------------------------------------------
 * spin_unlock_irqrestore(s, flags) — release flag then restore RFLAGS
 * ------------------------------------------------------------------------- */
void spin_unlock_irqrestore(spinlock_t *s, uint64_t flags)
{
    barrier();
    s->flag = 0;
    irq_restore(flags);    /* re-enables IRQs only if they were on before */
}

/* ---------------------------------------------------------------------------
 * spin_trylock(s) — non-blocking attempt, returns 1 on success
 *
 * Does NOT disable interrupts — caller is responsible for IRQ safety if
 * called from interrupt context.  Typically used in trylock patterns from
 * thread context where IRQs can legally fire.
 * ------------------------------------------------------------------------- */
int spin_trylock(spinlock_t *s)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&s->flag, &expected, 1,
                                       /*weak=*/0,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}
