/* SPDX-License-Identifier: MIT */
/* Per-phase boot perf budgets and threshold alarms.
 *
 * Companion to boot_timing.c -- defines target durations for each named
 * boot step + a soft/hard cap classifier. Budget breaches emit WARN/ERR
 * lines from boot_perf_dump; halt-on-breach is owned by TODO-23 watchdog.
 *
 * String-keyed against boot_timing_step_t.step so a renamed step in
 * boot_progress() shows up as "no budget" (NULL lookup) rather than as
 * a silently mismatched enum.
 */

#ifndef KERNEL_BOOT_PERF_BUDGET_H
#define KERNEL_BOOT_PERF_BUDGET_H

#include "kernel/types.h"

struct boot_phase_budget {
    const char *step;
    uint32_t    target_ms;
    const char *reason;
};

enum boot_perf_budget_class {
    BUDGET_OK   = 0,
    BUDGET_SOFT = 1,
    BUDGET_HARD = 2,
};

/* Classify observed_ms against target_ms.
 *   <= 1.5x   -> BUDGET_OK (silent)
 *   (1.5x, 4x] -> BUDGET_SOFT (WARN)
 *   > 4x      -> BUDGET_HARD (ERR)
 * target_ms == 0 always returns BUDGET_OK (caller has no budget). */
enum boot_perf_budget_class
boot_perf_budget_classify(uint32_t observed_ms, uint32_t target_ms);

/* Lookup a budget entry by step name. NULL on unknown step or NULL input. */
const struct boot_phase_budget *boot_perf_budget_lookup(const char *step);

/* Walk every recorded boot step, look up its budget by name, and emit
 * WARN/ERR for soft/hard cap breaches. Last step is skipped (delta-to-
 * next is always 0). Steps without a budget stay silent. Called from
 * boot_perf_dump after the existing PERF table emits. Data-only -- no
 * side effects, no halts. */
void boot_perf_budget_check(void);

/* Compare total boot time (steps[last].tsc - steps[0].tsc) against a
 * 4-second total target. Same soft/hard cap thresholds as per-step.
 * Called from boot_perf_dump after boot_perf_budget_check. */
void boot_perf_total_check(void);

/* Test-only accessors -- pure data reads, no live boot infrastructure. */
uint32_t boot_perf_budget_count(void);
const struct boot_phase_budget *boot_perf_budget_get(uint32_t i);

#endif /* KERNEL_BOOT_PERF_BUDGET_H */
