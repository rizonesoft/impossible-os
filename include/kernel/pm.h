/* ---------------------------------------------------------------------------
 * pm.h -- CPU idle entry (power management, todo/02-kernel-core/TODO-26-power-management.md section 2).
 *
 * This header owns the C1 idle PRIMITIVE, not system sleep. ACPI 6.5 section
 * 8.1 puts C1-Cn inside G0/S0 as the per-processor idle states and gives them
 * no meaning under S1-S4, so a PM1 SLP_EN write is a whole-machine firmware
 * transition and never a deeper HLT. acpi_enter_sleep_state() in
 * include/kernel/acpi.h is that other thing, and deliberately unreachable from
 * here: it refuses every caller off the BSP, it confirms its wake through
 * WAK_STS (which an ordinary timer tick does not set), and it logs on every
 * entry and exit -- all three of which are wrong at timer rate on an idle
 * machine.
 *
 * todo/02-kernel-core/TODO-26-power-management.md section 16 replaces the fixed C1 choice below with a governor that
 * selects among _CST-advertised C-states; pm_deep_idle_allowed() is the
 * readiness predicate it will consume.
 * ------------------------------------------------------------------------- */
#ifndef KERNEL_PM_H
#define KERNEL_PM_H

#include "kernel/types.h"

/* Read the PowerIdleEnable policy value once and cache it. Call after the
 * registry is populated. Idempotent.
 *
 * It does NOT run before the first idle site, and cannot: smp_init() parks the
 * APs inside pm_idle_c1() at src/kernel/main/boot_storage.c:271, while this
 * runs at :1090 of the same boot_phase2(). That is the designed contract, not
 * a gap -- an unread cache reads as ENABLED, which is exactly the
 * unconditional HLT every idle site performed before this section, and the
 * cache is published with release/acquire precisely because live readers are
 * already running when it is written. */
void pm_idle_init(void);

/* Deep-idle readiness for the CALLING CPU. Zero when this CPU has a DPC
 * queued, or when the cached PowerIdleEnable policy value is zero.
 *
 * Advisory on its own: the answer can be invalidated by an interrupt the
 * instant it is returned, which is why pm_idle_c1() re-asks it with interrupts
 * already disabled rather than trusting a caller's earlier call. */
int pm_deep_idle_allowed(void);

/* One-shot, race-safe C1 idle entry. Masks interrupts, samples
 * pm_deep_idle_allowed() in that window, and halts with the `sti; hlt` pair --
 * STI's interrupt shadow defers delivery until the HLT has begun, so a wake
 * arriving in that instant halts-then-wakes instead of being lost.
 *
 * IT ALWAYS HALTS when the caller has interrupts enabled, and the predicate
 * NEVER turns the halt into a spin. That is not a softening of the readiness
 * check, it is the only correct behaviour for the two sites that exist today:
 * neither the compositor loop nor the AP park can SERVICE a pending DPC, and
 * an idle AP has no drain trigger at all -- its LAPIC timer is masked and
 * there is no DPC IPI (`src/kernel/sched/ktimer.c:248-251`), so a stranded DPC
 * would hold the predicate false forever and a refusing loop would burn a
 * whole logical CPU. Returning to a scheduler that can run the work is what a
 * refusal is FOR, and no such caller exists until per-CPU run queues do.
 *
 * Returns the sampled predicate: 1 if the CPU was idle by policy and had no
 * DPC queued, 0 if it halted anyway with work pending or policy disabled, and
 * 0 without halting only when the caller arrived with interrupts already
 * masked (halting there would need `sti`, running an ISR inside a region the
 * caller believes is interrupt-free). The caller's interrupt flag is restored
 * on every exit.
 *
 * Allocation-free and log-free by contract: this runs at timer rate. */
int pm_idle_c1(void);

/* Accumulated halt cycles for a CPU, or 0 for a slot that is not online.
 * Written only by its owning CPU inside pm_idle_c1().
 *
 * READ THE TWO QUALIFICATIONS BEFORE EXPOSING THIS ANYWHERE. It is an UPPER
 * BOUND over SELECTED sites, not accumulated processor idle time:
 *
 *   - Upper bound: the instruction after HLT retires only once the waking ISR
 *     has RETURNED, and on the BSP the scheduler may switch away first, so a
 *     contribution can include that ISR and potentially another thread's whole
 *     quantum. Fixing it needs a timestamp taken at interrupt ENTRY, which is
 *     parked as a `[/]` item in todo/02-kernel-core/TODO-26-power-management.md section 2.
 *   - Selected sites: only TERMINAL idle halts route through pm_idle_c1().
 *     The bounded input-batching halt in src/kernel/main/compositor.c
 *     deliberately does not, because it is a wait to accumulate input rather
 *     than time the CPU has nothing to do.
 *
 * todo/02-kernel-core/TODO-26-power-management.md section 25 is the intended telemetry consumer and is BLOCKED on the
 * first of these: exporting this as a Win32 idle time today would report a
 * value wrong by up to a scheduling quantum. */
uint64_t pm_idle_cycles(uint32_t cpu);

/* The accepted contribution of one halt interval: t1 - t0, or 0 when the TSC
 * did not advance. A backwards sample (migration, firmware write) would
 * otherwise wrap to ~2^64 and destroy the accumulator permanently, where a
 * rejected sample costs one idle episode. Exposed so that guard is testable. */
uint64_t pm_idle_delta(uint64_t t0, uint64_t t1);

#ifdef KERNEL_TESTS
/* Test-only: override the cached PowerIdleEnable policy value, returning the
 * previous one, so the disabled path is testable without a registry write.
 * Guarded so a production caller cannot reach in and disable idle policy. */
int pm_idle_set_enable_test(int enable);

/* Test-only: the predicate AND the exact queue depth it was computed from, in
 * ONE sample. Two separate observations cannot be compared -- a remote enqueue
 * or a cross-CPU KeRemoveQueueDpc between them yields a contradiction that is
 * nobody's bug and would false-fail a ship gate. */
int pm_deep_idle_probe_test(uint32_t *out_depth);

/* Test-only: how many times control has arrived at the instruction after the
 * halt. Proves the halt site was REACHED, which no other assertion can. */
uint64_t pm_idle_halt_hits_test(void);
#endif

#endif /* KERNEL_PM_H */
