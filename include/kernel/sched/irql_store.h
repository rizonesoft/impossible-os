/* ============================================================================
 * irql_store.h -- the one writer of per_cpu_data.current_irql
 *
 * Every IRQL change on every path goes through irql_store(), including the
 * paths that deliberately bypass KeRaiseIrql/KeLowerIrql: spinlock irqsave and
 * restore, IDT entry and exit, serial emergency locking, APC level moves, and
 * per-CPU slot init. Anything that must see every transition, such as
 * time-at-DISPATCH accounting or a LIFO validator, hooks in here and nowhere
 * else. scripts/lint.sh Check 34 refuses a write of the field anywhere else.
 *
 * Software state only: this never touches the LAPIC TPR. It runs in the ISR
 * path, where a TPR write is forbidden, and before GS_BASE is set, so it takes
 * the per-CPU block from its caller instead of calling smp_this_cpu().
 *
 * The release store pairs with an acquire load by another CPU watching this
 * one's level. x86 gives it for free, so it costs a compiler barrier only.
 * ============================================================================ */

#pragma once

#include "kernel/sched/irql.h"
#include "kernel/smp.h"

_Static_assert(sizeof(((struct per_cpu_data *)0)->current_irql) == sizeof(KIRQL),
               "irql_store writes one KIRQL");

static inline __attribute__((always_inline))
void irql_store(struct per_cpu_data *pcpu, KIRQL level)
{
    __atomic_store_n(&pcpu->current_irql, level, __ATOMIC_RELEASE);
}
