/* ============================================================================
 * dpc_config.h -- DPC/APC fairness + watchdog tuning constants
 *
 * Single home for the platform-calibratable knobs the DPC fairness budget +
 * watchdog consume, so a board bring-up can retune them in one place without
 * hunting through dpc.c. All values are conservative diagnostics
 * thresholds, NOT hard scheduling limits (DPC_BATCH_LIMIT in dpc.h is the hard
 * per-drain bound).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Single-DPC runtime watchdog (Bug Check 0x133 param 0x0) -------------- */

/* A single DPC routine that runs longer than this trips the per-DPC watchdog
 * (warn by default; bugcheck if dpc_watchdog_set_strict(1)). Matches the Win11
 * DPC_WATCHDOG_VIOLATION single-DPC threshold. Microseconds. */
#define DPC_WATCHDOG_SINGLE_DPC_US      100u

/* ---- Per-tick DPC fairness budget (count, with carry-over) ---------------- */

/* Soft per-CPU budget of DPCs dispatched per timer tick before the watchdog
 * flags monopolization. Generous so it only fires on genuine floods; the hard
 * per-drain bound stays DPC_BATCH_LIMIT. */
#define DPC_BUDGET_PER_TICK             256u

/* Cap on accumulated carry-over budget so an idle CPU cannot bank an unbounded
 * burst allowance. */
#define DPC_BUDGET_CARRYOVER_MAX        512u

/* ---- Sustained queue-depth watchdog -------------------------------------- */

/* Number of CONSECUTIVE ticks the per-CPU DPC queue depth must stay above
 * DPC_QUEUE_WARN_DEPTH (dpc.h) before the sustained-depth warning fires. A
 * single transient spike does not warn. */
#define DPC_DEPTH_WARN_TICKS            5u

/* ---- Kernel APC starvation watchdog -------------------------------------- */

/* Kernel APC queue depth on a single thread above this suggests the thread is
 * stuck in a critical/guarded region or at elevated IRQL too long (APCs queue
 * but never deliver). Warns; does not bugcheck. */
#define APC_STARVATION_WARN_DEPTH       32u

/* ---- Threaded-DPC worker (section 15) ------------------------------------ */

/* Max threaded DPCs the worker pops from ONE CPU's list per pass before moving
 * on, so one busy CPU (or a self-rearming threaded DPC) cannot starve others.
 * The worker blocks on event_wait() between passes (no idle CPU burn); a lost
 * wakeup self-heals via dpc_watchdog_tick's per-tick re-signal while pending. */
#define DPC_THREADED_BATCH_LIMIT        32u
