/* ============================================================================
 * irql.c -- Per-CPU IRQL tracking and transition primitives
 *
 * Implements the Windows NT-style IRQL model: each CPU tracks its current
 * interrupt request level.  KeRaiseIrql/KeLowerIrql enforce monotonic
 * transitions and program the LAPIC Task Priority Register (TPR) to mask
 * hardware interrupts below the active IRQL.
 *
 * IRQL-to-TPR mapping:
 *   PASSIVE/APC (0-1): TPR = 0x00  -- accept all hardware interrupts
 *   DISPATCH    (2):   TPR = 0x20  -- block ISA legacy vectors (group 0-1)
 *   DIRQL N   (3-14):  TPR = (N-1) << 4  -- block vectors below group N
 *   CLOCK+     (28+):  TPR = 0xFF  -- block all hardware interrupts
 *   HIGH       (31):   TPR = 0xFF  -- block all hardware interrupts
 * ============================================================================ */

#include "kernel/sched/irql.h"
#include "kernel/sched/dpc.h"
#include "kernel/smp.h"
#include "kernel/drivers/lapic.h"
#include "kernel/klog.h"

/* ---- LAPIC TPR programming ----------------------------------------------- */

/* Convert an IRQL to the LAPIC Task Priority Register value.
 *
 * The LAPIC TPR[7:4] (task priority class) masks all interrupt vectors
 * whose priority group (vector >> 4) is <= the TPR class.  This lets us
 * selectively block lower-priority device interrupts while allowing
 * higher-priority ones through. */
uint32_t irql_to_tpr(KIRQL irql)
{
    /* Software levels: accept all hardware interrupts */
    if (irql <= APC_LEVEL)
        return 0x00;

    /* DISPATCH_LEVEL: block ISA legacy range (priority groups 0-1) */
    if (irql == DISPATCH_LEVEL)
        return 0x20;

    /* Device IRQLs: block all vectors below this priority group.
     * TPR class = irql - 1, so vectors in group (irql-1) and below are masked,
     * and vectors in group irql and above can still deliver. */
    if (irql >= DIRQL_MIN && irql <= DIRQL_MAX)
        return (uint32_t)(irql - 1) << 4;

    /* System levels (CLOCK, IPI, POWER, HIGH): mask everything */
    return 0xFF;
}

/* Write the TPR for the given IRQL.  Safe no-op if LAPIC is not available
 * (single-CPU legacy fallback uses cli/sti only). */
static void irql_set_tpr(KIRQL irql)
{
    if (lapic_available())
        lapic_write(LAPIC_REG_TPR, irql_to_tpr(irql));
}

/* ---- Per-CPU IRQL access ------------------------------------------------- */

KIRQL KeGetCurrentIrql(void)
{
    return smp_this_cpu()->current_irql;
}

/* ---- KeRaiseIrql --------------------------------------------------------- */

void KeRaiseIrql(KIRQL new_irql, KIRQL *old_irql)
{
    struct per_cpu_data *pcpu = smp_this_cpu();
    KIRQL prev = pcpu->current_irql;

    /* Debug: monotonic raise validation */
    if (new_irql < prev) {
        klog(LOG_ERROR, "irql",
             "KeRaiseIrql violation: CPU %u attempted lower %u -> %u",
             (uint64_t)pcpu->cpu_id, (uint64_t)prev, (uint64_t)new_irql);
        /* Continue with clamped value rather than crashing --
         * the telemetry in section 8 will add hard traps */
        new_irql = prev;
    }

    /* Store previous for caller's restore */
    *old_irql = prev;

    /* Update per-CPU state */
    pcpu->current_irql = new_irql;

    /* Program LAPIC TPR to mask interrupts below the new level.
     * For HIGH_LEVEL, also disable interrupts via CLI as the
     * ultimate barrier (TPR alone cannot mask NMI/SMI). */
    if (new_irql >= HIGH_LEVEL) {
        __asm__ volatile("cli" ::: "memory");
    }
    irql_set_tpr(new_irql);
}

/* ---- KeLowerIrql --------------------------------------------------------- */

void KeLowerIrql(KIRQL old_irql)
{
    struct per_cpu_data *pcpu = smp_this_cpu();
    KIRQL cur = pcpu->current_irql;

    /* Debug: symmetric lower validation */
    if (old_irql > cur) {
        klog(LOG_ERROR, "irql",
             "KeLowerIrql violation: CPU %u attempted raise %u -> %u",
             (uint64_t)pcpu->cpu_id, (uint64_t)cur, (uint64_t)old_irql);
        /* Clamp: don't accidentally raise */
        old_irql = cur;
    }

    /* Update per-CPU state */
    pcpu->current_irql = old_irql;

    /* Reprogram LAPIC TPR for the restored level */
    irql_set_tpr(old_irql);

    /* If we were at HIGH_LEVEL (cli), re-enable interrupts now that
     * we've lowered below it. */
    if (cur >= HIGH_LEVEL && old_irql < HIGH_LEVEL) {
        __asm__ volatile("sti" ::: "memory");
    }

    /* NT DPC dispatch point: when lowering below DISPATCH_LEVEL, drain
     * any pending DPCs before returning to thread-level code.  This is
     * the primary DPC execution trigger -- KiDispatchDpc raises back to
     * DISPATCH_LEVEL internally and lowers when done. */
    if (cur >= DISPATCH_LEVEL && old_irql < DISPATCH_LEVEL) {
        struct dpc_queue *q = dpc_this_cpu_queue();
        if (q && q->head)
            KiDispatchDpc();
    }
}

/* ---- Debug assertion: IRQL contract check -------------------------------- */

void _irql_check_max(KIRQL max_irql, const char *caller)
{
    struct per_cpu_data *pcpu = smp_this_cpu();
    KIRQL cur = pcpu->current_irql;

    if (cur > max_irql) {
        klog(LOG_ERROR, "irql",
             "IRQL violation in %s: CPU %u at IRQL %u, max allowed %u",
             caller ? caller : "?",
             (uint64_t)pcpu->cpu_id, (uint64_t)cur, (uint64_t)max_irql);
    }
}
