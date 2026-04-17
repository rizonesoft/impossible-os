/* ============================================================================
 * msr.c -- #GP-safe MSR probe implementation
 *
 * msr_try_read() temporarily installs a #GP handler that sets a flag
 * instead of panicking, allowing safe detection of unsupported MSRs.
 * Protected by a spinlock + interrupt disable to prevent SMP races
 * and unrelated #GP swallowing.
 *
 * XREF: 02-kernel-core/TODO-09-x86-64-architecture.md §4
 * ============================================================================ */

#include "kernel/msr.h"
#include "kernel/idt.h"
#include "kernel/panic.h"
#include "kernel/sched/spinlock.h"

/* Per-probe state -- protected by s_probe_lock + CLI */
static volatile int s_gp_fired;
static volatile uint64_t s_probe_rip;  /* expected fault address */
static interrupt_handler_t s_saved_gp_handler;
static DEFINE_SPINLOCK(s_probe_lock);

/* Temporary #GP handler: only consume the fault if it came from our
 * probe instruction at s_probe_rip. Otherwise chain to the saved handler. */
static uint64_t gp_probe_handler(struct interrupt_frame *frame)
{
    if (frame->rip == s_probe_rip) {
        s_gp_fired = 1;
        /* rdmsr is a 2-byte instruction (0F 32); skip it */
        frame->rip += 2;
        return (uint64_t)frame;
    }
    /* Not our probe -- chain to original handler */
    if (s_saved_gp_handler)
        return s_saved_gp_handler(frame);
    /* No saved handler -- trigger the same panic the default ISR path
     * would produce. Returning the unchanged frame would infinite-loop
     * because the faulting instruction would retry and fault again. */
    panic_screen(frame, frame->err_code,
                 "General Protection Fault (#GP)", "msr.c", 0);
    __builtin_unreachable();
}

/* Address of the rdmsr instruction inside msr_read().
 * msr_read is static inline, so the compiler inlines it into
 * msr_try_read. We compute the probe RIP at runtime by taking
 * the address after setup and before the call. For robustness,
 * we use a dedicated inline asm rdmsr so we know the exact RIP. */

int msr_try_read(uint32_t index, uint64_t *out)
{
    uint32_t lo = 0, hi = 0;
    uint64_t irq_flags;

    /* Serialize: only one CPU can probe at a time. Interrupts disabled
     * to prevent unrelated #GPs from being swallowed by our handler. */
    spin_lock_irqsave(&s_probe_lock, &irq_flags);

    /* Save current #GP handler and install our probe */
    s_saved_gp_handler = idt_get_handler(13);
    s_gp_fired = 0;

    /* Compute the exact RIP of the rdmsr we're about to execute.
     * Use a label to get the address. */
    uint64_t probe_addr;
    __asm__ volatile (
        "lea 1f(%%rip), %0\n\t"
        : "=r"(probe_addr)
    );
    s_probe_rip = probe_addr;

    idt_register_handler(13, gp_probe_handler);

    /* The rdmsr instruction -- if the MSR doesn't exist, #GP fires
     * at exactly this RIP and gp_probe_handler skips it. */
    __asm__ volatile (
        "1: rdmsr\n\t"
        : "=a"(lo), "=d"(hi)
        : "c"(index)
    );

    /* Restore original handler */
    idt_register_handler(13, s_saved_gp_handler);

    /* Capture result while still holding the lock to prevent the next
     * CPU's probe from resetting s_gp_fired before we read it. */
    {
        int faulted = s_gp_fired;
        spin_unlock_irqrestore(&s_probe_lock, irq_flags);

        if (faulted) {
            if (out) *out = 0;
            return -1;
        }

        if (out) *out = ((uint64_t)hi << 32) | lo;
        return 0;
    }
}
