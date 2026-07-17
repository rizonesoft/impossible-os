/* ============================================================================
 * boot_health_check.h -- per-entry health-gated mark-good
 *
 * Greenboot-style gate. The bootloader has already decremented the
 * selected entry's tries counter pre-EBS; this module is the userspace
 * positive-confirmation half: only when configurable health checks pass
 * is the entry marked good (counter removed by the next bootloader run).
 *
 * Distinct from boot_health.h:
 *   - boot_health.h     == snapshot publisher (X:\Diag\boot-health.json)
 *                          composing "what's wrong this boot" signals.
 *   - boot_health_check.h (this file) == the GATE that decides whether
 *                          to ask the bootloader to delete the counter.
 *
 * Cross-boot ABI lives in include/boot/boot_health_handoff.h; this
 * header layers kernel-side semantics (registry, run, report) on top.
 *
 * Lifecycle:
 *   Phase 3 init    -> boot_health_check_register_defaults()
 *   Phase 3 publish -> boot_health_check_run() from boot_desktop.c
 *                      after boot_audit_publish()
 *   Health pass     -> mark_entry_successful(selected_entry_id) writes
 *                      MarkGood UEFI var with state-bound triple
 *
 * Single-shot guard: boot_health_check_run() is idempotent. A second
 * call within the same boot is a no-op (returns the cached aggregate
 * from the first run). Prevents accidental double-write of the JSONL
 * report or double-mark.
 *
 * Failure model: every uefi_var_set / VFS write that fails inside this
 * module degrades to LOG_WARN + "no opinion" -- never blocks userland
 * entry. The counter stays decremented; next boot retries.
 * ============================================================================ */

#ifndef KERNEL_BOOT_HEALTH_CHECK_H
#define KERNEL_BOOT_HEALTH_CHECK_H

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

/* Result of a single check. OK passes; SOFT_FAIL is logged-only
 * (non-blocking); HARD_FAIL is blocking iff the check is REQUIRED.
 * SKIPPED is the "infrastructure not present yet" outcome -- used by
 * the network/service-crash checks today. */
enum boot_health_check_result {
    BOOT_HEALTH_OK        = 0,
    BOOT_HEALTH_SOFT_FAIL = 1,
    BOOT_HEALTH_HARD_FAIL = 2,
    BOOT_HEALTH_SKIPPED   = 3
};

/* Required vs Wanted (greenboot semantic). A required HARD_FAIL blocks
 * mark-good. A wanted result is recorded but never blocks. */
enum boot_health_check_kind {
    BOOT_HEALTH_KIND_REQUIRED = 1,
    BOOT_HEALTH_KIND_WANTED   = 2
};

/* Aggregate outcome surfaced by boot_health_check_run(). */
enum boot_health_aggregate {
    BOOT_HEALTH_AGG_PENDING       = 0,
    BOOT_HEALTH_AGG_PASS          = 1,
    BOOT_HEALTH_AGG_INDETERMINATE = 2
};

/* Check function. Returns one of BOOT_HEALTH_OK / SOFT_FAIL / HARD_FAIL
 * / SKIPPED. May log via klog; must not panic. Must not depend on
 * runtime state beyond what is already initialized at Phase 3 (after
 * boot_audit_publish has run). */
typedef enum boot_health_check_result (*boot_health_check_fn)(void);

/* Cap on registered checks. 6 defaults + 6 slack for future
 * registrations (subsystem-specific checks added by TODO-N later). */
#define BOOT_HEALTH_CHECK_MAX 12u
#define BOOT_HEALTH_CHECK_NAME_LEN 24u

/* Register a check. Returns 1 on success, 0 if name is invalid /
 * duplicate / registry is full. Names match BOOT_HEALTH_SUBSET_NAME_LEN
 * geometry from boot_health_handoff.h. Called once per check at Phase 3
 * init, before scheduler_enable -- single-CPU context, no locking
 * required. */
int boot_health_check_register(const char *name,
                                enum boot_health_check_kind kind,
                                boot_health_check_fn fn);

/* Register the default check set (desktop_ready, no_boot_err, no_panic
 * required; x_mountable, network_reachable, no_service_crash_60s
 * wanted). Idempotent: second call is a no-op. */
void boot_health_check_register_defaults(void);

/* Run the gate. Reads ImpossibleOS-CurBootCtr + (optional) ImpossibleOS-
 * HealthSubset UEFI vars, executes every relevant registered check, and
 * writes a one-line JSONL report to X:\Boot\health.jsonl. On
 * BOOT_HEALTH_AGG_PASS it records the verdict via boot_status_note_health_pass()
 * but does NOT write the ImpossibleOS-MarkGood handoff itself -- the boot-status
 * acceptance ledger owns the single bless: its accepted transition performs the
 * per-entry MarkGood (only when health passed) alongside the A/B mark-good and
 * durable record. A health PASS alone is therefore necessary but not sufficient
 * to retire boot-entry tries; the boot must also reach its acceptance stage.
 *
 * Single-shot: a second call within the same boot returns the cached aggregate
 * without re-running (SMP-safe atomic latch). */
enum boot_health_aggregate boot_health_check_run(void);

/* Query the cached aggregate after boot_health_check_run() has been
 * called. Returns BOOT_HEALTH_AGG_PENDING if the gate has not run yet. */
enum boot_health_aggregate boot_health_check_last_aggregate(void);

/* Mark the named entry as successful by composing the MarkGood UEFI
 * variable record from the CurBootCtr handoff. Public so a future
 * user-mode tool (the deferred live-boot bootcfg binary) can call it via a
 * syscall thin wrapper without going through the full gate.
 *
 * Returns STATUS_SUCCESS on successful write, an NTSTATUS error code
 * otherwise. STATUS_NOT_FOUND if CurBootCtr is absent or invalid (no
 * entry_id binding available). STATUS_INVALID_PARAMETER if entry_id
 * does not match the CurBootCtr-recorded id (caller mismatch). */
NTSTATUS mark_entry_successful(const char *entry_id);

/* ----- Test surface ---------------------------------------------------
 *
 * Pure helpers exposed for unit tests so the registry mechanics,
 * subset-filter logic, and aggregate combinator are testable without
 * calling any live boot infrastructure (per the test policy in
 * docs/infrastructure/test-policy.md).
 * --------------------------------------------------------------------- */

/* Pure aggregator. Takes counts of (required_ok, required_softfail,
 * required_hardfail, required_skipped) and returns the aggregate. A
 * single hardfail on a required check is enough to indeterminate the
 * boot. A required check that returned SKIPPED is conservatively
 * treated as a hard-fail (we cannot prove the boot is good if a
 * required check could not run). */
enum boot_health_aggregate
boot_health_check_aggregate(unsigned int req_ok, unsigned int req_soft,
                             unsigned int req_hard, unsigned int req_skipped);

/* Reset the registry. Test-only; production code never calls this.
 * Clears registered checks and the cached aggregate. Guarded out of release
 * builds (release test-surface exclusion). */
#ifdef KERNEL_TESTS
void boot_health_check_test_reset(void);
#endif

/* Filter check: returns 1 if a check with the given name should run
 * under the supplied subset list, 0 otherwise. Empty subset (count==0)
 * runs every check (default greenboot). Non-empty subset runs only
 * checks whose name matches one of subset_names[0..count). */
int boot_health_check_in_subset(const char *name,
                                 const char (*subset_names)[BOOT_HEALTH_CHECK_NAME_LEN],
                                 unsigned int subset_count);

/* Count of registered checks. Test-only oracle; guarded out of release builds
 * (release test-surface exclusion). */
#ifdef KERNEL_TESTS
unsigned int boot_health_check_registered_count(void);
#endif

#endif /* KERNEL_BOOT_HEALTH_CHECK_H */
