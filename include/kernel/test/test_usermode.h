/* ============================================================================
 * test_usermode.h -- Kernel-side launcher for user-mode test binaries
 *
 * Scans C:\ for `test_*.exe` files (deployed by the user-mode test
 * framework's Makefile rule), runs each in its own task, collects exit
 * codes, and emits a `[UTEST] <name>: PASS|FAIL (exit=N)` line per
 * binary plus a summary `[UTEST] N passed, N failed of N total`.
 *
 * Triggered from boot_tests_run() when boot.conf has `test=1`. The
 * launcher is a no-op when test=0 (boot_tests_run skips the call) or
 * when no `test_*.exe` files exist on the disk.
 *
 * Each binary runs as a separate kernel task via the canonical
 * task_create + utest_loader_func + task_exec pattern (mirrors how
 * boot_desktop spawns cmd.exe). The launcher waits for each binary
 * with task_waitpid before scanning the next, so binaries run
 * sequentially and isolated from one another.
 *
 * The `utest_filter=<name|glob>` boot.conf parameter is honoured by
 * test_usermode_set_filter() via boot_tests_run's wire-up; the
 * launcher runs every match when unset, or only globs matching the
 * filter string when set.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Run every test_*.exe found at C:\ root sequentially and collect
 * results. Logs per-binary `[UTEST]` lines + a final summary.
 * No-op (cleanly returns) if C:\ is not mounted or no matching
 * binaries exist. Safe to call repeatedly; the function is
 * stateless other than the per-call summary counters. */
void test_usermode_run(void);

/* Set a filename filter to restrict which binaries the next
 * test_usermode_run() invocation will execute. NULL (or "") means
 * "run all". Glob via the literal `*` wildcard at any position
 * (e.g. "test_smoke_*.exe" matches `test_smoke_boot.exe`). The
 * pointed-to string must outlive test_usermode_run(); the launcher
 * does not copy it. */
void test_usermode_set_filter(const char *filter);

/* Set per-binary wall-clock timeout in milliseconds. 0 = use the
 * built-in default (10 000 ms). A binary that does not reach
 * TASK_DEAD by the deadline is killed (SIGKILL then force-DEAD) and
 * reported as FAIL (timeout). */
void test_usermode_set_timeout_ms(uint32_t ms);

/* Enable TAP (Test Anything Protocol) emission around each binary.
 * When enabled, the launcher emits a `1..N` plan, `ok N - name` /
 * `not ok N - name` / `ok N - name # SKIP <reason>` lines parsable by
 * kselftest-style tooling. Disabled by default. */
void test_usermode_set_tap(int enable);

/* Enable or disable per-test isolation. When enabled (default), the
 * launcher creates C:\Temp\utest\<name>\ as a scratch root, deletes
 * HKLM\SOFTWARE\ImpossibleOS\Test\<name> before and after each run,
 * snapshots the task's handle-table count, and logs a WARN on leak.
 * Disable only for debugging broken cleanup hooks; boot.conf
 * `utest_isolation=0` toggles this via test_usermode_set_isolation(). */
void test_usermode_set_isolation(int enable);

/* Enable JUnit XML output: the launcher emits one
 * `[UTEST-XML] <testsuite ...>` opener, one `[UTEST-XML] <testcase
 * ...>` per binary with `<failure>` / `<skipped/>` children where
 * appropriate, and one `[UTEST-XML] </testsuite>` closer. scripts/
 * test.sh extracts these lines into `build/test-results.xml` for CI
 * tools (GitLab junit reports, GitHub Actions upload-artifact, Jenkins
 * junit plugin). Orthogonal to TAP and JSON -- all three can be on
 * simultaneously. Disabled by default. */
void test_usermode_set_xml(int enable);

/* Enable JSON-line output: one `[UTEST-JSON] {"name":...}` record per
 * binary + one `[UTEST-JSON] {"summary":{...}}` at the end. Typed
 * schema for downstream trend analysis. Disabled by default. */
void test_usermode_set_json(int enable);

/* Contract for SKIP: a test binary reports "this test is not
 * applicable here" by exiting with status 77 (kselftest convention).
 * The launcher counts it as SKIPPED, not FAIL. Any other non-zero
 * exit is FAIL. */
#define UTEST_EXIT_SKIP    77
/* Synthetic exit the launcher assigns when a binary is killed for
 * exceeding its wall-clock timeout. */
#define UTEST_EXIT_TIMEOUT (-6)
