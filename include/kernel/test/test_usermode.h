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

/* Neither kernel-originated refusal status is defined here. Both live in
 * include/kernel/sched/task.h, allocated from the reserved exit-reason block
 * below -SIG_MAX, because a status in the small-negative range is
 * indistinguishable from -(signum):
 *
 *   TASK_EXIT_UTEST_IDENTITY  the bytes read did not match the identity the
 *                             plan froze. Its first version used -7 and
 *                             collided with a signal death exactly as that
 *                             block's own comment warns.
 *   TASK_EXIT_UTEST_TIMEOUT   the binary outran its wall clock. Was -6 here
 *                             until 2026-08-01, so a timed-out binary and a
 *                             SIGABRT-range death reported identically.
 *
 * The move was deferred once on the belief that host artifact parsers keyed
 * on the -6 value. They do not: scripts/test.sh reads the named TIMEOUT
 * verdict token, and no shell or Python consumer in the tree compares the
 * number. Only UTEST_EXIT_SKIP stays here, because 77 is an EXTERNAL contract
 * a ring-3 binary produces, not a kernel-assigned cause. */

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
    /* Discard mode: this owner has already spent its emission budget, so
     * bytes are swallowed without being staged, escaped, or charged. It is
     * SEPARATE from _active because the two say different things: _active=0
     * means "not captured, caller must fall back to raw serial", while
     * discard means "captured and deliberately dropped" -- taking the
     * fallback here would put the very payload the budget stopped back on
     * the wire unframed. Without it, a binary that keeps writing after its
     * stop still pays a full escape pass and a global-lock acquisition per
     * write forever, so the budget would bound serial traffic while leaving
     * producer CPU and lock contention unbounded. */
    uint8_t  _discard;
    /* Identity of the write() this ctx is staging, published as `wr=` on
     * every record the write emits so the host can reconcile each write
     * independently instead of only the owner's highest sequence number.
     * It is the sequence number of this write's FIRST emitted chunk, so
     * _has_wid stays 0 for a write that never reached the wire -- see the
     * WRITE IDENTITY block above u_capture_emit_chunk (test_usermode.c). */
    uint8_t  _has_wid;
    uint32_t _wid;
    /* The thread that owns this write, resolved ONCE at capture_start.
     * Cached because the arm, the identity mirror on the first emitted
     * chunk, and the settle at capture_end each need it, and resolving it
     * three times per write is three lookups on the hottest path the
     * capture machinery has. Opaque for the same reason `_owner` is: this
     * header must not pull in task.h. */
    void    *_thr;
    /* Why this write stopped, recorded AT the moment it stopped rather
     * than re-derived at settle time. The owner's fence can latch between
     * the stop and capture_end, and re-reading it then would label a write
     * dropped for an unrelated reason as a deliberate teardown -- the same
     * error the death path avoids by snapshotting at the transition. */
    uint8_t  _cut_fenced;
    char     _buf[192];
};

/* What one claim on the capture wire resolved to -- the producer-side
 * emission budget's verdict. Declared in the header rather than privately
 * in test_usermode.c because the decision function is exported for unit
 * test: a test comparing against bare 0/1/2/3 would pass just as happily
 * after someone reordered the enum, which is the kind of assertion that
 * verifies nothing. */
enum utest_capture_verdict {
    UTEST_CAP_EMIT = 0,       /* charge accepted: emit this chunk        */
    UTEST_CAP_OVER_OWNER = 1, /* this owner just exhausted its budget    */
    UTEST_CAP_OVER_RUN = 2,   /* the run-wide budget went first          */
    UTEST_CAP_DROP = 3,       /* already terminated: emit nothing        */
    /* The RUN closed admission before this claim arrived. Distinct from
     * DROP even though both emit nothing: DROP is a per-owner policy stop
     * that the owner's own latch remembers, while SEALED is a run-boundary
     * refusal that says nothing about the owner and is lifted wholesale by
     * the next frame. Conflating the two would let a boundary refusal latch
     * an owner permanently, and would leave the run-boundary fence with no
     * verdict of its own for a test to assert against. */
    UTEST_CAP_SEALED = 4
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
 * not also observe. Returns 1 if this byte was consumed by the capture
 * pipeline (the caller must NOT also serial_putchar() it); returns 0 if
 * this ctx is not capture-owned, in which case the caller falls back to
 * its existing raw serial_putchar() path unchanged.
 *
 * "Consumed" means AT MOST once on serial, not exactly once. A captured
 * byte normally crosses framed, and an uncaptured one crosses raw -- never
 * both. But once the owner has spent its producer emission budget the byte
 * is consumed and DROPPED, crossing zero times, and the return value is
 * still 1. That is the point: returning 0 there would send the caller to
 * its raw fallback and put the very payload the budget stopped straight
 * back on the wire unframed, so the budget would bound nothing. Both
 * callers (sys_write, NtWriteFile) need only the never-both guarantee,
 * which still holds.
 * Internally escapes and chunks into one or more "[UTEST-CAPTURE] ..."
 * records emitted through the same frame-nonce mechanism the non-
 * forgeable launcher record framing uses, so the new record type is
 * non-forgeable for free. */
int test_usermode_capture_byte(struct utest_capture_ctx *ctx, char c);

/* Flushes any partially-filled capture chunk as a final=1 record. Callers
 * call this once after their write() loop ends, unconditionally -- a no-op
 * when nothing was staged (or when this ctx was never capture-owned). */
void test_usermode_capture_end(struct utest_capture_ctx *ctx);

/* Settle a DEAD thread's open capture write and emit its evidence, called
 * from the reap barrier. Declared here rather than kept
 * private because the reap points that know a thread has stopped existing
 * live in the scheduler, while the capture wire lives here. */
struct task;
struct thread;
void test_usermode_cap_settle_dead_thread(struct task *t, struct thread *thr);

/* Test shims over the two predicates the per-write narrowing rests on. */
int test_usermode_capture_was_budget_cut(const struct task *owner,
                                         const struct thread *thr);
int test_usermode_capture_claim_settlement(struct thread *thr);

/* Emits the one-time "[UTEST-CAPTURE-BEGIN] owner=<pid> name=<name>"
 * announcement binding a capture owner pid to its binary name. The ONLY
 * caller is task_create_internal() (task.c), which calls this BEFORE the
 * new task is published (num_tasks++) -- emitting it any later (e.g. from
 * the launcher after task_create_captured() returns) would let an
 * immediately-scheduled task on another CPU emit capture chunk records
 * before their owner binding exists on the wire. */
void test_usermode_capture_begin(uint32_t owner_pid, const char *name);

/* --- The launcher's per-run infrastructure latches --------------------------
 *
 * A condition that is sticky for the rest of a run and aborts it, where "the
 * run" means ONE test_usermode_run() invocation -- documented above as safe to
 * call repeatedly. Left as a bare file static such a latch would outlive its
 * run and abort the NEXT one, which would then serialize aborted artifacts
 * carrying no record of the fault they name, because that record belongs to the
 * previous run's frame.
 *
 * SCOPE IS THE MEMBERSHIP TEST, and it is not obvious from severity. The
 * launcher's other sticky latch, s_reap_degraded, is deliberately NOT here: it
 * says an unfinished fork constructor still owns a task slot, which starting a
 * new run does not settle, so resetting it would trade a corruption guard for
 * tidiness. A latch belongs here only when the next run genuinely re-measures
 * the thing it describes.
 *
 * A struct rather than a bare flag so the boundary reset is one act over the
 * whole set: a per-run latch added later is cleared by construction rather than
 * by whoever remembers to extend a list of assignments. */
struct utest_run_latches {
    uint8_t wait_stalled;   /* a bounded wait ended on the clock watchdog */
};

/* Clear every latch. Pure over the caller's struct so the run-boundary
 * behaviour is assertable without driving a live run, which a kernel test may
 * not do (it needs scheduling, signals, a clock and a mounted C:). */
void test_usermode_run_latches_reset(struct utest_run_latches *l);

/* --- The launcher's one bounded wait ---------------------------------------
 *
 * Every wait in the launcher yields until a condition holds, and every one of
 * them used to terminate ONLY on u_uptime_ms(). That reads like a bound and is
 * not one: uptime_ns() resolves through the driver's read_ns to
 * mono_ns_coarse(), which for PMTMR hands back an epoch the timer ISR banks
 * every PMTMR_ADVANCE_TICKS ticks and for the LAPIC/PIT source is derived from
 * system_get_ticks(). Both stop dead if the tick stops -- and a deadline
 * compared against a stopped clock is never reached, so each `bounded` wait
 * yielded forever and the boot hung with no diagnostic at all.
 *
 * The escape is a WATCHDOG on an independent counter (mono_tsc_raw), NOT an
 * inference from how fast the loop is spinning. Rate inference cannot be made
 * safe here: a HEALTHY coarse clock plateaus for a full ISR advance interval,
 * nothing in this tree bounds how many yields fit inside one plateau, and a
 * wait cut short on a healthy machine reports capture records as lost when they
 * were merely slow -- a false refusal is strictly worse than the hang, because
 * it fires on runs that had nothing wrong with them. */

/* Why the wait ended. Distinguishing the last two is the point: a producer that
 * hung and a clock that stopped need opposite investigations, and before this
 * enum existed neither was distinguishable from the other or from a clean
 * exit. */
enum utest_wait_end {
    UTEST_WAIT_READY   = 0,  /* the condition became true */
    UTEST_WAIT_TIMEOUT = 1,  /* the deadline passed on an ADVANCING clock */
    UTEST_WAIT_STALLED = 2   /* the clock stopped; the watchdog ended the wait */
};

/* The wait's world, injectable for the same reason the reap's is: the shapes
 * that matter (a frozen ms clock, a frozen watchdog, a coarse clock that
 * plateaus and then advances) cannot be produced on demand by a live run.
 *
 * `now_ticks` is a free-running counter that must NOT be derived from `now_ms`
 * -- deriving it would reproduce the very coupling the watchdog exists to
 * break. The live binding is mono_tsc_raw(). */
struct utest_wait_ops {
    uint64_t (*now_ms)(void);
    uint64_t (*now_ticks)(void);
    /* Ticks the counter may accumulate in one millisecond, used to turn the
     * watchdog's budget into a tick count. NULL means "no measured rate" and
     * selects UTEST_WAIT_TICKS_PER_MS_MAX below. */
    uint64_t (*ticks_per_ms)(void);
    void     (*wait)(void);
};

/* The FALLBACK rate, in ticks per millisecond, used only when no measured one
 * is available -- and it is the CEILING mono_source_qualify() will accept for a
 * TSC (100 GHz, src/kernel/time/mono_clock.c), not a guess about hardware.
 *
 * The pessimism is the safety property, and the direction is easy to get
 * backwards: real elapsed time to accumulate N ticks is N / real_rate, so only
 * a rate at or ABOVE the real one makes the watchdog fire LATE. A constant
 * chosen from "the fastest shipping part" is not an upper bound at all -- this
 * kernel accepts a scaled or virtual TSC an order of magnitude past that, and
 * on such a part a too-low constant burns the budget proportionally faster and
 * declares a HEALTHY advancing wait stalled. Anchoring to the qualification
 * ceiling makes the bound true for every clock this kernel will run on.
 *
 * THE FALLBACK'S LATENCY IS A KNOWN LIMITATION, stated rather than papered
 * over: assuming 100 GHz on a 3 GHz part stretches the escape ~33x, past the
 * harness's own 60 s boot bound, so on a platform whose TSC never qualified a
 * stall surfaces as the runner's generic timeout instead of as a named record.
 * Narrowing it would require a rate, which is exactly what that platform does
 * not have -- and firing early on a healthy fast TSC is the worse error, so the
 * bound stays true rather than convenient. mono_tsc_hz() supplies the measured
 * rate in every other case, which is every platform this kernel is validated
 * on. */
#define UTEST_WAIT_TICKS_PER_MS_MAX  100000000ULL

/* Overflow hygiene, and NOTHING ELSE -- in particular NOT a harness-fit bound.
 *
 * The invariant that outranks every other consideration here is that the
 * watchdog must expire LATER than the caller's own deadline. Break it and the
 * escape stops being an escape and becomes a killer of healthy tests: a cap
 * below the configured timeout ends a wait whose millisecond clock is advancing
 * perfectly, kills its child, and aborts the suite. `utest_timeout_ms` is a
 * UINT16, so a caller may legitimately configure up to 65535 ms, and
 * docs/testing/usermode-env-matrix.md recommends 30000 for storage-heavy
 * binaries on WHPX -- a cap chosen to fit the harness would fire in the middle
 * of exactly the runs that setting exists for.
 *
 * So this sits ABOVE anything the boot contract can express (65535 x the
 * multiplier), and the relation to the harness is an operator contract stated
 * in that doc instead: the escape lands at roughly twice the configured
 * per-binary timeout, so a run raising the timeout raises scripts/test.sh's
 * TIMEOUT with it. */
#define UTEST_WAIT_WATCHDOG_CAP_MS   300000u

/* The watchdog's budget, as a multiple of the caller's own timeout plus a
 * floor. A wait whose deadline is honoured on a live clock must reach TIMEOUT
 * first, so the watchdog has to sit clear of it; the floor keeps a very short
 * timeout (the 200 ms drains) from producing a budget so tight that ordinary
 * scheduling jitter could reach it. */
#define UTEST_WAIT_WATCHDOG_MULT     2u
#define UTEST_WAIT_WATCHDOG_FLOOR_MS 1000u

/* Last-resort TERMINATION PROOF, not the mechanism. It is reachable only when
 * BOTH the millisecond clock and the watchdog counter are frozen, which on real
 * hardware means the CPU is retiring instructions while its TSC does not
 * advance. It exists so "every wait terminates" is a property of the code
 * rather than of the platform. Sized far above any count a healthy wait can
 * reach, because on a healthy clock the deadline or the watchdog always ends
 * the wait long before this does. */
#define UTEST_WAIT_ITER_CEIL         (1u << 24)

/* Drive the shared bounded wait over a caller-supplied world. Returns one of
 * enum utest_wait_end. `ready` is polled BEFORE every termination check, so a
 * condition that became true in the same instant the deadline passed reports
 * READY rather than a timeout it did not suffer. */
int test_usermode_bounded_wait(int (*ready)(void *ctx), void *ctx,
                               uint32_t timeout_ms,
                               const struct utest_wait_ops *ops);

/* The watchdog's budget in ticks, exposed so the CONVERSION is assertable
 * without a wait around it. The floor, the cap and the measured-versus-fallback
 * rate are four decisions whose product is one number, and a wrong one is
 * invisible from the outside: too large and the escape lands after the harness
 * has killed the machine, too small and it ends healthy waits. */
uint64_t test_usermode_wait_watchdog_ticks(uint32_t timeout_ms,
                                           const struct utest_wait_ops *ops);

/* --- The run-boundary reap's injectable world ------------------------------
 *
 * These two types are declared in the HEADER rather than kept private to
 * test_usermode.c for one reason: the reap loop is otherwise undrivable. Its
 * pure helpers were exported by section 49 and are all asserted, but the loop
 * around them -- round cap, grace shape, publication drain, kill/terminate
 * ordering, final census, descending cleanup -- was reachable only from a
 * live run, and a live run cannot be made to produce the shapes that matter
 * on demand (a descendant published inside the grace window, a fork still in
 * flight when the tree is latched). Section 49's own post-commit audit found
 * two real defects in that loop while every pure-helper assertion stayed
 * green, which is what settled it: the loop takes its world as a parameter.
 *
 * Every member is a seam a synthetic tree substitutes. `kill` and
 * `terminate` deliberately carry no signal number or exit code -- both are
 * fixed by the reap and are not the loop's decision, so routing them through
 * the seam would only let a test drive a combination the reap cannot
 * produce. */
struct utest_reap_ops {
    struct task *(*get_by_pid)(uint32_t pid);
    void         (*fence)(struct task *owner);
    void         (*latch)(uint32_t owner_pid);
    uint32_t     (*fork_pending)(uint32_t owner_pid, uint32_t *stranded);
    void         (*kill)(uint32_t pid);
    void         (*terminate)(struct task *t);
    void         (*cleanup)(uint32_t pid);
    uint64_t     (*now_ms)(void);
    /* The reap's two waits run through the shared bounded wait, so its world
     * has to carry the watchdog counter too. It is a member of THIS struct,
     * and the wait-ops view handed to the helper is projected from these two
     * members rather than supplied separately -- a synthetic tree then sets
     * one clock pair and cannot accidentally leave the reap and its waits
     * reading different time. */
    uint64_t     (*now_ticks)(void);
    /* Paired with now_ticks for the same reason: the reap's waits convert its
     * counter to a duration, and a synthetic tree that supplied a counter but
     * not its rate would be measured against the production fallback instead
     * of against its own clock. */
    uint64_t     (*ticks_per_ms)(void);
    void         (*wait)(void);
};

/* What the reap learned, rather than only how many survived.
 *
 * `live` alone stopped being able to carry the answer once fork publication
 * was interlocked with the reap: the count is normally EXACT -- a statement
 * about the tree rather than about the walk -- but only while the drain
 * reaches zero, and an exact zero is indistinguishable from a best-effort
 * zero at every reporting site. Separating them is the whole point of the
 * interlock, so the flag travels with the number. */
struct utest_reap_result {
    uint32_t live;           /* capture-owning descendants alive at the census */
    uint32_t rounds;         /* kill rounds consumed (<= the reap's round cap) */
    uint32_t drain_pending;  /* forks still in flight when the drain gave up */
    uint32_t drain_stranded; /* registrations on tasks that died mid-constructor */
    uint8_t  exact;          /* 1 iff nothing outstanding: `live` is a statement */
    /* 1 iff a wait inside the reap ended on the clock watchdog rather than on
     * its deadline. Carried OUT rather than reported from inside the loop: the
     * loop runs over a synthetic world in the assertions, and a record emitted
     * from there would put a fabricated stall on a healthy run's wire and mark
     * the suite failed. The live entry point is the only thing that reports. */
    uint8_t  stalled;
};

/* Drive the reap loop over a caller-supplied world. The live binding is
 * internal; this shim exists for assertions only. */
void test_usermode_capture_reap_tree_ops(uint32_t owner_pid,
                                         const struct utest_reap_ops *ops,
                                         struct utest_reap_result *out);

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
static inline int  test_usermode_color_active(void)
{ /* INTENTIONAL-STUB: no launcher in this flavor, so color is never on */ return 0; }
static inline void test_usermode_capture_start(struct utest_capture_ctx *ctx __attribute__((unused))) {}
static inline int  test_usermode_capture_byte(struct utest_capture_ctx *ctx __attribute__((unused)),
                                              char c __attribute__((unused)))
{ /* INTENTIONAL-STUB: this flavor captures nothing */ return 0; }
static inline void test_usermode_capture_end(struct utest_capture_ctx *ctx __attribute__((unused))) {}
static inline void test_usermode_capture_begin(uint32_t owner_pid __attribute__((unused)),
                                               const char *name __attribute__((unused))) {}

#endif /* KERNEL_TESTS */
