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
#include "kernel/smp.h"
#include "kernel/drivers/lapic.h"
#include "kernel/klog.h"
#include "kernel/sched/dpc.h"
#include "kernel/sched/apc.h"
#include "kernel/sched/task.h"

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

/* Write a precomputed TPR value.  Safe no-op if LAPIC is not available
 * (single-CPU legacy fallback uses cli/sti only).  Transition callers pass
 * the already-mapped TPR value so the hot path computes irql_to_tpr() once
 * per IRQL rather than recomputing the target inside the writer. */
static void irql_write_tpr_value(uint32_t tpr)
{
    if (lapic_available())
        lapic_write(LAPIC_REG_TPR, tpr);
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

    /* Map both levels to their TPR class once (cheap branch cascade), so the
     * write path below does not recompute the target. */
    uint32_t new_tpr = irql_to_tpr(new_irql);
    uint32_t prev_tpr = irql_to_tpr(prev);

    /* Update per-CPU state */
    pcpu->current_irql = new_irql;

    /* For HIGH_LEVEL, also disable interrupts via CLI as the ultimate barrier
     * (TPR alone cannot mask NMI/SMI). The CLI is independent of the TPR-skip
     * below. */
    if (new_irql >= HIGH_LEVEL) {
        __asm__ volatile("cli" ::: "memory");
    }

    /* Program the LAPIC TPR only when the priority class actually changes
     * (e.g. PASSIVE<->APC both map to 0x00, CLOCK/IPI/POWER/HIGH all map to
     * 0xFF, nested same-level raises map equal). The hardware TPR mirrors
     * irql_to_tpr(current_irql): the only TPR writers are this transition path
     * and the two PASSIVE-equivalent (TPR=0) LAPIC init writes, so a same-value
     * store would be a pure UC-MMIO tax on the spinlock hot path. */
    if (new_tpr != prev_tpr)
        irql_write_tpr_value(new_tpr);
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

    /* DPC drain-on-lower (NT software-interrupt dispatch): crossing from
     * >= DISPATCH_LEVEL to below it, drain pending DPCs while the CPU runs at
     * DISPATCH_LEVEL with the CORRECT hardware priority -- program the LAPIC TPR
     * to the DISPATCH class and (if we were cli'd at HIGH) enable interrupts, so
     * DPC callbacks run at DISPATCH with device IRQs at/below DISPATCH masked
     * but higher ones serviced (the DPC contract). The per-CPU dpc_draining
     * guard, held ONLY across dpc_drain_current_cpu() (never across the APC
     * delivery that follows), stops a DPC that itself lowers IRQL from
     * re-entering the drain. dpc_drain_current_cpu() does no IRQL management and
     * early-outs on an empty queue. Spinlock release lowers current_irql
     * directly (not via KeLowerIrql), so this is NOT on the spinlock hot path. */
    if (cur >= DISPATCH_LEVEL && old_irql < DISPATCH_LEVEL && !pcpu->dpc_draining) {
        uint32_t disp_tpr = irql_to_tpr(DISPATCH_LEVEL);
        pcpu->dpc_draining = 1;
        pcpu->current_irql = DISPATCH_LEVEL;
        if (irql_to_tpr(cur) != disp_tpr)
            irql_write_tpr_value(disp_tpr);
        if (cur >= HIGH_LEVEL)
            __asm__ volatile("sti" ::: "memory");
        dpc_drain_current_cpu();
        pcpu->dpc_draining = 0;
        /* current_irql is now DISPATCH_LEVEL; lower the rest below. */
    }

    /* Lower from the current level to old_irql and deliver pending kernel APCs
     * (no DPC drain -- the guarded block above already handled that). */
    irql_lower_deliver(old_irql);
}

/* Lower current_irql to old_irql with TPR + interrupt-flag handling and kernel
 * APC delivery-on-lower, but NO DPC drain. Shared by KeLowerIrql (after its
 * guarded drain phase) and KiDispatchDpc (which must NOT trigger a second
 * batch nor hold a drain guard across the yieldable APC NormalRoutine). */
void irql_lower_deliver(KIRQL old_irql)
{
    struct per_cpu_data *pcpu = smp_this_cpu();
    KIRQL cur;
    uint32_t old_tpr, cur_tpr;

    if (!pcpu)
        return;
    cur = pcpu->current_irql;
    if (old_irql > cur)             /* never accidentally raise */
        old_irql = cur;

    old_tpr = irql_to_tpr(old_irql);
    cur_tpr = irql_to_tpr(cur);
    pcpu->current_irql = old_irql;

    /* Reprogram the LAPIC TPR only when the priority class actually changes
     * (e.g. APC->PASSIVE both TPR 0x00). See KeRaiseIrql for the invariant
     * that makes the skip safe. */
    if (old_tpr != cur_tpr)
        irql_write_tpr_value(old_tpr);

    /* If we were at HIGH_LEVEL (cli), re-enable interrupts now that we have
     * lowered below it. Independent of the TPR-skip above. */
    if (cur >= HIGH_LEVEL && old_irql < HIGH_LEVEL)
        __asm__ volatile("sti" ::: "memory");

    /* Kernel APC delivery-on-lower: crossing from >= APC_LEVEL to below it,
     * deliver pending kernel APCs to the current thread. Gate ONLY on a guarded
     * region (KeAreAllApcsDisabled blocks ALL kernel APC classes); a critical
     * region blocks only NORMAL APCs, so a special kernel APC must still be
     * delivered -- KiDeliverApc applies the precise per-APC region check itself.
     * No per-CPU reentrancy guard: a NormalRoutine may yield/block/exit, so a
     * CPU-scoped guard could leak across a context switch and wedge delivery;
     * the per-thread kernel_apc_in_progress flag bounds normal-APC reentrancy
     * and special APCs are self-limiting (each is dequeued once). */
    if (cur >= APC_LEVEL && old_irql < APC_LEVEL) {
        struct thread *th = thread_current();
        if (th && th->apc_state.kernel_apc_pending && !KeAreAllApcsDisabled())
            KiDeliverApc((uint8_t)ApcKernelMode, (void *)0, (void *)0);
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
