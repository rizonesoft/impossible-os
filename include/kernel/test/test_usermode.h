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
 *
 * KERNEL_TESTS gating: the launcher lives in src/kernel/test/, which the
 * release flavor (`make KERNEL_TESTS=off`) prunes from the build entirely.
 * Its callers -- boot_tests.c and klog.c -- call it unguarded, so every
 * declaration below is `#ifdef KERNEL_TESTS` with a no-op `static inline`
 * `#else` arm, mirroring test.h. The type taxonomy and the exit-code
 * contract carry no linkage and stay visible in both flavors.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Test-type taxonomy (user-mode test framework). The launcher derives the type
 * from the binary filename prefix:
 *   `test_smoke_*.exe`   -> UTEST_TYPE_SMOKE    (phase 0: FAIL aborts run)
 *   `test_stress_*.exe`  -> UTEST_TYPE_STRESS   (classname=stress; binary loops internally)
 *   `test_perf_*.exe`    -> UTEST_TYPE_PERF     (classname=perf; test owns baseline)
 *   anything else        -> UTEST_TYPE_CORRECTNESS (default)
 * An optional `type=<name>` token in the manifest entry overrides
 * the filename-derived default (future-proofing for binaries that
 * want to declare a different policy).
 *
 * Stress note: the launcher spawns each binary exactly once. Stress
 * iteration is the BINARY's responsibility (see user/test/
 * test_stress_libc.c). Kernel task slots are monotonic so a
 * launcher-side loop would exhaust TASK_MAX (32) after ~20 binaries. */
typedef enum {
    UTEST_TYPE_CORRECTNESS = 0,
    UTEST_TYPE_SMOKE       = 1,
    UTEST_TYPE_STRESS      = 2,
    UTEST_TYPE_PERF        = 3
} utest_type_t;

/* Contract for SKIP: a test binary reports "this test is not
 * applicable here" by exiting with status 77 (kselftest convention).
 * The launcher counts it as SKIPPED, not FAIL. Any other non-zero
 * exit is FAIL. */
#define UTEST_EXIT_SKIP    77
/* Synthetic exit the launcher assigns when a binary is killed for
 * exceeding its wall-clock timeout. */
#define UTEST_EXIT_TIMEOUT (-6)

/* Layer 1 of the constant's defence, at COMPILE time. 77 is an external
 * contract -- the GNU automake / kselftest "skipped" status -- so drift
 * away from it silently turns our SKIPs into FAILs in any mixed CI
 * consumer expecting the Linux convention. A runtime unit test could only
 * compare the define against its own literal, which verifies nothing but
 * that someone typed it twice (scripts/lint.sh Check: tautological-test);
 * a static assert refuses to BUILD instead, which is the check that was
 * actually wanted. */
_Static_assert(UTEST_EXIT_SKIP == 77,
               "UTEST_EXIT_SKIP must stay the kselftest/automake skip status");
_Static_assert(UTEST_EXIT_TIMEOUT < 0 && UTEST_EXIT_TIMEOUT != UTEST_EXIT_SKIP,
               "the timeout marker must not collide with a real exit status");

/* Per-write source-level stdout capture context. Callers (sys_write in
 * syscall.c, the stdout branch of NtWriteFile in nt_syscall.c) declare
 * ONE of these as a LOCAL (stack) variable before their write() loop, in
 * BOTH build flavors (hence this struct is defined unconditionally, not
 * inside the KERNEL_TESTS block below -- a release build still needs the
 * COMPLETE type to declare the local, even though every operation on it
 * is then a no-op). Deliberately NOT stored on struct task: it belongs
 * to exactly one call on exactly one thread's own stack, so accumulating
 * bytes into it needs no lock even when a multi-threaded test binary's
 * OTHER thread is concurrently inside its own write() -- there is no
 * shared buffer left to race on. `_owner` is an opaque `struct task *`
 * (kept untyped here so this header does not need to pull in task.h);
 * test_usermode.c casts it back. */
struct utest_capture_ctx {
    void    *_owner;
    uint32_t _len;
    uint8_t  _active;
    char     _buf[192];
};

#ifdef KERNEL_TESTS

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

/* RESERVED -- boot.conf `stress_iters=<N>` is plumbed through this
 * setter but currently has no consumer (stress binaries hardcode
 * their iteration count). When a future env-passing syscall lands,
 * the launcher will forward this value to the child so a stress
 * binary can query its desired iteration count at runtime. 0 =
 * default. */
void test_usermode_set_stress_iters(uint32_t n);

/* Non-zero while a user-mode test binary owns the current task, so klog
 * renders every kernel line emitted on that binary's behalf in the UTEST
 * color. Read by klog's color-scope block; the launcher owns the flag. */
int test_usermode_color_active(void);

/* Resolves task_current() and its capture ownership ONCE per write(),
 * not per byte. Safe to call even when not capture-owned -- ctx->_active
 * ends up 0 and every later call on this ctx is then a cheap no-op. */
void test_usermode_capture_start(struct utest_capture_ctx *ctx);

/* One byte of a ring-3 write(). Called from the caller's single read of
 * the raw user buffer (never a second dereference of the same byte) so it
 * cannot observe a value the caller's own NUL-check or terminal echo did
 * not also observe. Returns 1 if this byte was consumed into the framed
 * capture pipeline (the caller must NOT also serial_putchar() it -- the
 * payload crosses serial exactly once, either raw or framed, never
 * both); returns 0 if this ctx is not capture-owned, in which case the
 * caller falls back to its existing raw serial_putchar() path unchanged.
 * Internally escapes and chunks into one or more "[UTEST-CAPTURE] ..."
 * records emitted through the same frame-nonce mechanism the non-
 * forgeable launcher record framing uses, so the new record type is
 * non-forgeable for free. */
int test_usermode_capture_byte(struct utest_capture_ctx *ctx, char c);

/* Flushes any partially-filled capture chunk as a final=1 record. Callers
 * call this once after their write() loop ends, unconditionally -- a no-op
 * when nothing was staged (or when this ctx was never capture-owned). */
void test_usermode_capture_end(struct utest_capture_ctx *ctx);

/* Emits the one-time "[UTEST-CAPTURE-BEGIN] owner=<pid> name=<name>"
 * announcement binding a capture owner pid to its binary name. The ONLY
 * caller is task_create_internal() (task.c), which calls this BEFORE the
 * new task is published (num_tasks++) -- emitting it any later (e.g. from
 * the launcher after task_create_captured() returns) would let an
 * immediately-scheduled task on another CPU emit capture chunk records
 * before their owner binding exists on the wire. */
void test_usermode_capture_begin(uint32_t owner_pid, const char *name);

#else /* !KERNEL_TESTS */

/* Release flavor: src/kernel/test/ is pruned from the build, so the launcher
 * has no definition. The unguarded callers in boot_tests.c and klog.c compile
 * against these no-ops instead, and the calls fold away. */

static inline void test_usermode_run(void) {}
static inline void test_usermode_set_filter(const char *filter __attribute__((unused))) {}
static inline void test_usermode_set_timeout_ms(uint32_t ms __attribute__((unused))) {}
static inline void test_usermode_set_tap(int enable __attribute__((unused))) {}
static inline void test_usermode_set_isolation(int enable __attribute__((unused))) {}
static inline void test_usermode_set_xml(int enable __attribute__((unused))) {}
static inline void test_usermode_set_json(int enable __attribute__((unused))) {}
static inline void test_usermode_set_stress_iters(uint32_t n __attribute__((unused))) {}
static inline int  test_usermode_color_active(void) { return 0; }
static inline void test_usermode_capture_start(struct utest_capture_ctx *ctx __attribute__((unused))) {}
static inline int  test_usermode_capture_byte(struct utest_capture_ctx *ctx __attribute__((unused)),
                                              char c __attribute__((unused))) { return 0; }
static inline void test_usermode_capture_end(struct utest_capture_ctx *ctx __attribute__((unused))) {}
static inline void test_usermode_capture_begin(uint32_t owner_pid __attribute__((unused)),
                                               const char *name __attribute__((unused))) {}

#endif /* KERNEL_TESTS */
