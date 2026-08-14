/* ============================================================================
 * test_usermode.c -- Kernel-side launcher for user-mode test binaries
 *
 * Ships (baseline spawn-and-wait) and (manifest, timeouts, TAP,
 * SKIP, filter) of TODO-04. See include/kernel/test/test_usermode.h
 * for the public API contract.
 *
 * Sequencing: the launcher is single-threaded by design. Binaries run
 * one at a time so a leaked file handle, dirty Registry key, or stuck
 * process from binary N cannot perturb binary N+1's run. Per-test
 * isolation hardening (scratch dir + handle-leak detection) is owned
 * by; this file gets the basic sequence + watchdog right.
 *
 * Preemptive scheduling: test_usermode_run() enables the scheduler
 * for the duration of the launcher run and disables it on return.
 * Without this, a test_*.exe that spins in user mode without making
 * any syscall would block CPU 0 forever -- cooperative yield() cannot
 * wrest control back from a spinning user task, and the timeout
 * watchdog below depends on the launcher periodically regaining the
 * CPU to check uptime_ms(). Wrapped in enable/disable so the rest of
 * boot_phase3 (which assumes single-threaded init order) is
 * unaffected. Mirrors the sys_wq creation wrapper at
 * boot_desktop.c:86.
 *
 * The path-passing trick:
 *   task_create_captured(loader_func, name, path, digest) launches a
 *   kernel task that runs loader_func once. The constructor arms the path
 *   and the frozen identity into the new task's OWN slot before publishing
 *   it to the scheduler; the loader reads them back out of that slot,
 *   opens the file, stages a buffer, and calls task_exec(buf, size) to
 *   morph the kernel task into a user task running the binary. Same
 *   pattern as exec_loader_func in src/kernel/main/test_threads.c.
 *
 *   The inputs AND the loader's verdict live in the child's TCB rather
 *   than in file-scope statics, so neither can outlive the invocation that
 *   produced them. The statics they replaced were cleared per spawn and
 *   read back after the wait, a pairing that held only because dispatch
 *   goes through one global current_task -- so a late store from a
 *   force-killed loader could be read as the NEXT binary's evidence.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/fs/vfs.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/ipc/signal.h"
#include "kernel/ob/ob_process.h"
#include "kernel/timer.h"
#include "kernel/test/test_usermode.h"
#include "kernel/csprng.h"
#include "kernel/crypto/sha256.h"
#include "kernel/exec.h"
#include "kernel/time/mono_clock.h"
#include "registry.h"

/* ---- Internal state -------------------------------------------------- */

/* One child's loader evidence, copied out of its TCB while the slot is
 * still valid (after the child is dead, before task_cleanup -- the same
 * window u_report_snapshot and u_isolation_snapshot_leaks already use).
 *
 * Copied rather than read through a retained pointer so the verdict
 * computation downstream cannot be reading a slot the reap has begun to
 * tear down, and so every field of one invocation's verdict is taken at a
 * single point rather than field-by-field across the classification.
 *
 * WHAT THE FIVE FLAGS MEAN, and why no two of them are interchangeable:
 *
 *   stage_fault       The loader EXITED before ring 3 -- exact, recorded at
 *                     each exit. Needed because the exit status alone cannot
 *                     say so: the loader's own -1..-5 codes sit inside the
 *                     -(signum) range a signalled ring-3 binary reports.
 *   reached_exec      The loader got as far as ATTEMPTING task_exec. Set
 *                     BEFORE the call, and that ordering is the whole
 *                     correctness argument: task_exec publishes the frame
 *                     and re-enables interrupts before returning, so a tick
 *                     can carry the task into the new image and nothing
 *                     after the call is guaranteed to run. Marking
 *                     beforehand can only OVER-report, which pushes
 *                     ambiguous cases toward FAIL; marking afterwards would
 *                     label a genuine ring-3 hang as never-ran.
 *   identity_mismatch The bytes on disk stopped matching the identity the
 *                     plan froze. Names WHY a refusal happened, so the run
 *                     can publish a counted, NAMED infrastructure failure
 *                     rather than a bare exit code.
 *   frame_adopted     The SCHEDULER took the published exec frame. This is
 *                     what closes the half a timeout could not: a frame
 *                     published but never adopted is provably a binary that
 *                     never ran. exec_pending cannot stand in for it: a
 *                     ZERO there means "adopted" AND "no frame was ever
 *                     published", which are opposite verdicts. (It is NOT
 *                     swept on a timeout -- an earlier comment here said so
 *                     and was wrong; EXEC_PENDING_STUCK_TICKS only gates a
 *                     WARN.)
 *   entered_user      A syscall arrived from CPL 3. Unlike frame_adopted
 *                     this is PROOF the image executed, not a prediction
 *                     that it would: adoption still leaves TSS, CR3, GS,
 *                     swapgs and the iretq ahead of the first user
 *                     instruction. */
struct u_loader_evidence {
    uint32_t stage_fault;
    uint32_t reached_exec;
    uint32_t identity_mismatch;
    uint32_t frame_adopted;
    uint32_t entered_user;
};

/* Copy one task's loader evidence out of its slot.
 *
 * Split from the pid resolution below so the FIELD COPY is assertable on a
 * plain struct task fixture. A snapshot that swapped two fields or dropped
 * one would reconcile cleanly in the artifacts and still publish the wrong
 * ERROR-versus-FAIL verdict, and nothing about that failure is visible
 * from the outside -- so it needs an assertion of its own rather than only
 * being covered incidentally by the binaries the launcher spawns.
 *
 * A NULL task yields an all-zero record, which reads as "nothing observed"
 * -- the same conservative never-ran-leaning shape a fresh slot has, and
 * never a fabricated success. */
static void u_loader_evidence_from_task(const struct task *t,
                                        struct u_loader_evidence *out)
{
    if (!t) {
        out->stage_fault = out->reached_exec = out->identity_mismatch = 0;
        out->frame_adopted = out->entered_user = 0;
        return;
    }
    out->stage_fault =
        __atomic_load_n(&t->utest_loader.stage_fault, __ATOMIC_ACQUIRE);
    out->reached_exec =
        __atomic_load_n(&t->utest_loader.reached_exec, __ATOMIC_ACQUIRE);
    out->identity_mismatch =
        __atomic_load_n(&t->utest_loader.identity_mismatch, __ATOMIC_ACQUIRE);
    out->frame_adopted =
        __atomic_load_n(&t->utest_loader.frame_adopted, __ATOMIC_ACQUIRE);
    out->entered_user =
        __atomic_load_n(&t->utest_loader.entered_user, __ATOMIC_ACQUIRE);
}

/* Resolve the reaped child and take that copy. */
static void u_loader_snapshot(uint32_t pid, struct u_loader_evidence *out)
{
    u_loader_evidence_from_task(task_get_by_pid(pid), out);
}


/* Filter set by -- NULL means "run every test_*.exe". */
static const char *s_filter;

/* Per-binary wall-clock timeout in ms. 0 = default. */
static uint32_t s_timeout_ms;

/* TAP mode: 1 = emit `ok N - name` / `not ok N - name` / `1..N` plan. */
/* A reap ended without being able to account for an in-flight fork
 * constructor, so no further task may be CREATED in this boot's suite.
 *
 * Withholding cleanup contains the memory, but containment is only half the
 * problem. An unfinished task_fork has already chosen its child slot --
 * `child_pid = num_tasks` -- WITHOUT having published it, and the very next
 * task_create_internal chooses `pid = num_tasks` too. Launching the next
 * binary would hand a second constructor the same slot the first one is
 * still writing, so a degraded reap has to stop the suite rather than merely
 * decline to free things: the host-side refusal happens after the guest has
 * already run, and cannot contain anything.
 *
 * Sticky for the rest of the BOOT, and deliberately NOT reset at the framed-run
 * boundary the way struct utest_run_latches is. The distinction is what the two
 * latches MEAN. A stalled clock is a statement about a run's measurements: the
 * next run re-measures, so carrying it forward would abort a healthy run over
 * someone else's fault. This one is a statement about the TASK TABLE -- an
 * unfinished fork constructor still owns the slot the next task_create_internal
 * would claim -- and nothing about starting a new run settles that constructor.
 * Resetting it would hand back a corruption guard in exchange for tidiness. */
static int s_reap_degraded;

/* Per-RUN infrastructure latches. See struct utest_run_latches for why these
 * are separated from the boot-sticky poison above. */
static struct utest_run_latches s_run_latches;

/* Clear the whole set, by ASSIGNING a zeroed struct rather than by clearing
 * each member. A latch added later is then cleared by construction; a list of
 * per-member assignments is a list someone has to remember to extend, and the
 * cost of forgetting is a run aborted over another run's fault. */
static void u_run_latches_reset(struct utest_run_latches *l)
{
    struct utest_run_latches cleared;

    cleared.wait_stalled = 0;
    *l = cleared;
}

static int s_tap_mode;

/* per-test isolation: 1 = scratch dir + Registry wipe + handle-leak
 * detection around each binary. Default 1 (ON); boot.conf
 * utest_isolation=0 flips it off for debugging broken cleanup hooks. */
static int s_isolation_enabled = 1;

/* CI-friendly output formats. Orthogonal to TAP and to each
 * other: any subset can be enabled and all enabled formats emit
 * interleaved on serial, tagged with distinct `[UTEST-XML]` /
 * `[UTEST-JSON]` prefixes so scripts/test.sh can split them by grep. */
static int s_xml_mode;
static int s_json_mode;

/* test-type taxonomy: stress iteration count (0 = built-in default).
 * Set from boot.conf `stress_iters=<N>` via test_usermode_set_stress_iters. */
static uint32_t s_stress_iters;
#define UTEST_STRESS_DEFAULT_ITERS 100u

/* UTEST color-scope flag. Set by the launcher around each spawn
 * (task_create -> task_cleanup); read by klog's color picker so every
 * kernel subsystem line emitted WHILE a user-mode test binary is the
 * live task (sched/exec/elf/signal/etc.) renders in the UTEST color
 * instead of the default per-level color. The launcher is
 * single-threaded on a single CPU at boot_tests_run time, so one
 * global suffices -- no per-CPU ABI churn. Exposed to klog via
 * test_usermode_color_active() below. */
static volatile int s_utest_color_active;

int test_usermode_color_active(void)
{
    return s_utest_color_active;
}

/* Defaults matching the test checkpoint: 10s wall clock is long
 * enough for a trivial test_*.exe on WHPX TCG (launch overhead plus
 * ELF load plus a few syscalls is <2s), short enough that a genuine
 * hang is caught in one boot cycle. */
#define UTEST_DEFAULT_TIMEOUT_MS 10000u
/* Grace period after SIGKILL before we force state=DEAD. */
#define UTEST_KILL_GRACE_MS       500u
/* There is deliberately no separate cap on manifest runnables. The
 * enumeration plan (UTEST_PLAN_MAX) is the single capacity that bounds
 * what a run can enumerate, from either source; a second, smaller cap
 * here is what used to drop manifest entries past 128 onto a glob
 * fallback that could not reconstruct their metadata. */

void test_usermode_set_filter(const char *filter)
{
    s_filter = (filter && filter[0]) ? filter : (const char *)0;
}

void test_usermode_set_timeout_ms(uint32_t ms)
{
    s_timeout_ms = ms;
}

void test_usermode_set_tap(int enable)
{
    s_tap_mode = enable ? 1 : 0;
}

void test_usermode_set_isolation(int enable)
{
    s_isolation_enabled = enable ? 1 : 0;
}

void test_usermode_set_xml(int enable)
{
    s_xml_mode = enable ? 1 : 0;
}

void test_usermode_set_json(int enable)
{
    s_json_mode = enable ? 1 : 0;
}

void test_usermode_set_stress_iters(uint32_t n)
{
    s_stress_iters = n;
}

/* ---- Tiny string helpers (no libc deps in kernel) -------------------- */

static int u_strncmp(const char *a, const char *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb)
            return (int)ca - (int)cb;
        if (ca == 0)
            return 0;
    }
    return 0;
}

static int u_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int u_ends_with(const char *s, const char *suffix)
{
    uint32_t sl = 0, fl = 0;
    while (s[sl]) sl++;
    while (suffix[fl]) fl++;
    if (fl > sl)
        return 0;
    return u_strncmp(s + (sl - fl), suffix, fl + 1) == 0;
}

/* Fold one ASCII letter, matching what IXFS does to a filename.
 *
 * Declared up here because the filter below is the FIRST consumer in file
 * order; u_name_equal_fs further down folds identically. Only ASCII needs
 * it: the accepted charset is [A-Za-z0-9._-]. */
static char u_fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive fixed-length compare, same fold as the filesystem. */
static int u_strncmp_fold(const char *a, const char *b, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        char ca = u_fold(a[i]), cb = u_fold(b[i]);
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (ca == '\0')
            return 0;
    }
    return 0;
}

/* fnmatch-style `*` glob (single wildcard supported, anywhere). The
 * launcher-filter spec calls for "literal name or `*`-glob" and that's exactly
 * what tests need (`test_smoke_*.exe` etc.). NULL pattern matches
 * everything. Exposed for unit tests (u_glob_match_public).
 *
 * Matching is CASE-INSENSITIVE, because the filesystem it selects over is.
 * C: is IXFS, whose ixfs_strcmp folds ASCII case (ixfs_core.c), so
 * `test_Foo.exe` and `test_foo.exe` name one file -- and a filter that
 * compared literally could reject a dirent under one spelling while the
 * dedup that runs beside it treated the two as the same identity. That
 * combination could suppress a requested binary entirely: the launcher
 * would plan and run it zero times while total_planned and total_ran
 * still agreed, which is a false green rather than a visible miss. Two
 * spellings of one filename now mean one thing to the filter, the dedup
 * and the filesystem alike.
 *
 * User-visible: a `utest_filter=` value that previously missed on case
 * now matches. That is the intended direction -- the old behaviour could
 * only ever run FEWER binaries than the operator asked for. */
int test_usermode_glob_match(const char *pattern, const char *name);
int test_usermode_glob_match(const char *pattern, const char *name)
{
    const char *star;
    uint32_t prefix_len, suffix_len, name_len;
    const char *suffix;

    if (!pattern)
        return 1;
    star = pattern;
    while (*star && *star != '*') star++;
    if (!*star) {
        const char *p = pattern, *n = name;
        while (*p && *n && u_fold(*p) == u_fold(*n)) { p++; n++; }
        return *p == 0 && *n == 0;
    }
    prefix_len = (uint32_t)(star - pattern);
    suffix = star + 1;
    name_len = 0;
    while (name[name_len]) name_len++;
    if (name_len < prefix_len)
        return 0;
    if (u_strncmp_fold(pattern, name, prefix_len) != 0)
        return 0;
    suffix_len = 0;
    while (suffix[suffix_len]) suffix_len++;
    if (suffix_len > name_len - prefix_len)
        return 0;
    return u_strncmp_fold(name + (name_len - suffix_len), suffix,
                          suffix_len + 1) == 0;
}

static int u_is_test_binary(const char *name)
{
    if (!u_starts_with(name, "test_"))
        return 0;
    if (!u_ends_with(name, ".exe"))
        return 0;
    return 1;
}

/* Why a name gets refused, kept SEPARATE from "this is not a test file".
 *
 * A readdir entry that is not `test_*.exe` is not addressed to this
 * framework at all and stays silently ignored, exactly as before. A
 * manifest entry, or a discovered entry that IS test-shaped, is a binary
 * somebody intended to run -- so refusing it is a result, not a filter,
 * and it has to reach the artifacts as a counted infrastructure failure.
 * Dropping one silently is the false-green this taxonomy exists to close:
 * before it, a rejected entry never reached total_planned, so it vanished
 * from every artifact while the suite still reported success. */
typedef enum {
    UTEST_NAME_ACCEPT = 0,
    UTEST_NAME_NOT_TEST_SHAPED, /* not test_*.exe -- ignore, do not count */
    UTEST_NAME_REFUSE_LENGTH,   /* longer than the derived record bound   */
    UTEST_NAME_REFUSE_CHARSET,  /* byte outside [A-Za-z0-9._-]            */
    UTEST_NAME_REFUSE_PATH,     /* `..` traversal component               */
    UTEST_NAME_REFUSE_NUL,      /* NUL inside the manifest line's span    */
    UTEST_NAME_REFUSE_ATTR      /* recognised attribute, unusable value   */
} utest_name_verdict_t;

/* Defined after the record-bound derivation, which is expressed in terms
 * of UTEST_RECORD_LINE_MAX and so cannot precede the parser that calls
 * these. `span_len` is the length of the raw bytes the caller holds: the
 * manifest passes its line span so an embedded NUL is caught rather than
 * silently truncating the name, and the glob path passes the dirent's own
 * string length. */
static utest_name_verdict_t u_classify_name_span(const char *name,
                                                 uint32_t span_len);
static utest_name_verdict_t u_classify_name(const char *name);
static uint32_t u_name_digest(const char *p, uint32_t len);
/* Defined beside the plan's identity freeze, used by the loader far above
 * it -- forward-declared here for the same reason u_name_digest is. */
static int u_identity_matches(const uint8_t *expect, const uint8_t *actual);

/* Stricter gate used for manifest entries: the file is user-provided
 * text and u_run_one concatenates `C:\<name>` before vfs_open+task_exec,
 * so entries must stay in the C:\ root and must not contain path
 * separators, drive-letter colons, or upwards traversal components.
 * The glob-discovery path is already implicitly safe because vfs_readdir
 * returns one directory entry at a time, but the manifest path has no
 * such guard.
 *
 * Now a thin boolean view of the taxonomy above, and TEST-ONLY: every
 * enumeration call site takes the verdict instead, because "why" is what
 * decides between ignoring an entry and counting a failure. It survives
 * because the yes/no answer is what the validator's own regressions
 * assert; the release build never compiles this file at all. */
static int u_is_valid_manifest_name(const char *name)
{
    return u_classify_name(name) == UTEST_NAME_ACCEPT;
}

static uint64_t u_uptime_ms(void)
{
    return uptime_ns() / 1000000ULL;
}

/* ---- The one bounded wait ------------------------------------------------ *
 *
 * struct utest_wait_ops / enum utest_wait_end (include/kernel/test/
 * test_usermode.h) carry the full rationale for why a millisecond deadline is
 * not a bound and why the escape is a watchdog on an independent counter rather
 * than an iteration count. What follows is the loop and the LIVE binding.
 * ------------------------------------------------------------------------- */

/* The fallback rate must cover every TSC this kernel will accept, and the two
 * constants live in different subsystems, so the relation is pinned rather than
 * documented. A raised qualification ceiling with an unchanged launcher bound
 * would leave the watchdog burning its budget faster than real time on exactly
 * the fast virtual TSCs the ceiling was raised to admit -- and the symptom
 * would be healthy waits reported as stalled, on the platforms hardest to
 * reproduce. */
_Static_assert(UTEST_WAIT_TICKS_PER_MS_MAX * 1000ULL >= MONO_TSC_HZ_MAX,
               "the launcher's fallback watchdog rate is below the fastest TSC "
               "mono_source_qualify() accepts, so the watchdog could fire on a "
               "healthy wait");

/* The live rate for the watchdog's conversion: the MEASURED TSC frequency when
 * the kernel has one, with a safety factor, and the qualification ceiling when
 * it does not. Never a guess -- see UTEST_WAIT_TICKS_PER_MS_MAX for why a
 * plausible-sounding constant is the failure mode here. */
static uint64_t u_wait_ticks_per_ms_live(void)
{
    uint64_t hz = mono_tsc_hz();

    /* Used AS MEASURED, with no safety factor on top. The budget is already a
     * multiple of the caller's own timeout, so a healthy wait reaches its
     * deadline at half the watchdog's mark and calibration error of a fraction
     * of a percent cannot close that gap. A second multiplier here would only
     * push the escape past the harness's boot bound, which is where an escape
     * stops being one. */
    if (hz == 0)
        return UTEST_WAIT_TICKS_PER_MS_MAX;
    /* Integer division floors, which biases the rate DOWN and the escape
     * EARLY -- the unsafe direction. It cannot matter here: the qualification
     * floor is 100 MHz, so the quotient is at least 100000 and the discarded
     * remainder is under one part in 100000 of it. */
    return hz / 1000ULL;
}

static const struct utest_wait_ops u_wait_ops_live = {
    .now_ms       = u_uptime_ms,
    .now_ticks    = mono_tsc_raw,
    .ticks_per_ms = u_wait_ticks_per_ms_live,
    .wait         = yield,
};

/* Ticks the watchdog will let pass before it ends the wait.
 *
 * Kept separate from the loop so the arithmetic is checkable on its own, and
 * computed in 64-bit throughout. The widest product it can form is the capped
 * budget against the qualification ceiling -- 300000 ms x 1e8 ticks/ms = 3e13
 * -- well inside a uint64, and the cap is what keeps it that way rather than a
 * claim about what callers pass.
 *
 * The cap is deliberately ABOVE anything utest_timeout_ms can express, so it
 * can never pull the budget below the caller's own deadline. That ordering is
 * the invariant the whole escape rests on: a watchdog that expires first ends
 * healthy waits. */
static uint64_t u_wait_watchdog_ticks(uint32_t timeout_ms,
                                      const struct utest_wait_ops *ops)
{
    uint64_t budget_ms = (uint64_t)timeout_ms * UTEST_WAIT_WATCHDOG_MULT;
    uint64_t rate = ops->ticks_per_ms ? ops->ticks_per_ms()
                                      : UTEST_WAIT_TICKS_PER_MS_MAX;

    if (budget_ms < (uint64_t)UTEST_WAIT_WATCHDOG_FLOOR_MS)
        budget_ms = (uint64_t)UTEST_WAIT_WATCHDOG_FLOOR_MS;
    if (budget_ms > (uint64_t)UTEST_WAIT_WATCHDOG_CAP_MS)
        budget_ms = (uint64_t)UTEST_WAIT_WATCHDOG_CAP_MS;
    /* A world that reports a zero rate would otherwise produce a zero budget,
     * which is an immediately-expired watchdog: the one failure this whole
     * design exists to avoid. Fall back rather than trust it. */
    if (rate == 0)
        rate = UTEST_WAIT_TICKS_PER_MS_MAX;
    return budget_ms * rate;
}

/* THREE termination conditions, and the order between them is the design.
 *
 *   1. `ready` is polled FIRST, every iteration. A condition that became true
 *      in the same instant the deadline passed is a satisfied wait, not a
 *      timeout, and reporting it as a timeout would manufacture failures out of
 *      waits that succeeded.
 *   2. The millisecond DEADLINE is the ordinary bound and stays the ordinary
 *      answer. On a live clock it is always reached before the watchdog,
 *      because the watchdog's budget is a multiple of the same timeout.
 *   3. The WATCHDOG is the escape, and only the escape. It measures elapsed
 *      counter ticks against a budget converted at a deliberately overstated
 *      rate, so on any real part it fires later than nominal -- never earlier.
 *
 * The counter delta is taken as an unsigned subtraction from the value sampled
 * at entry, which is correct across the counter's own wrap. A counter that goes
 * BACKWARD (no per-CPU offset correction is applied to a raw read) yields a
 * huge unsigned delta and would end the wait early, so the sample is clamped to
 * a high-water mark: the launcher is not migrated between CPUs while it waits
 * (task dispatch uses a single global current_task), and the clamp makes that
 * an enforced property of this loop rather than an assumption about the caller.
 *
 * The iteration ceiling below all three is the termination PROOF: reachable
 * only when the deadline clock and the watchdog counter are BOTH frozen, and
 * reported as a stall because that is what it is. */
static int u_bounded_wait(int (*ready)(void *ctx), void *ctx,
                          uint32_t timeout_ms,
                          const struct utest_wait_ops *ops)
{
    uint64_t deadline;
    uint64_t wd_budget;
    uint64_t wd_start;
    uint64_t wd_high;
    uint32_t iters;
    int have_watchdog;

    if (!ready || !ops || !ops->now_ms || !ops->wait)
        return UTEST_WAIT_STALLED;

    deadline  = ops->now_ms() + (uint64_t)timeout_ms;
    wd_budget = u_wait_watchdog_ticks(timeout_ms, ops);
    /* A world without a counter still terminates, on the ceiling alone. Stated
     * as an explicit branch rather than left to a NULL call, because the whole
     * promise of this helper is that no binding of it can spin forever. */
    have_watchdog = (ops->now_ticks != (uint64_t (*)(void))0);
    wd_start = have_watchdog ? ops->now_ticks() : 0;
    wd_high  = wd_start;

    for (iters = 0; iters < UTEST_WAIT_ITER_CEIL; iters++) {
        if (ready(ctx))
            return UTEST_WAIT_READY;
        if (ops->now_ms() >= deadline)
            return UTEST_WAIT_TIMEOUT;
        if (have_watchdog) {
            uint64_t now = ops->now_ticks();
            /* RE-DERIVED EVERY ITERATION, and only ever allowed to GROW.
             *
             * A budget snapshotted at entry goes stale: the rate behind it can
             * change under a live wait -- a drift demotion moves timekeeping
             * off the TSC, and the rate the wait is still holding was measured
             * for a counter the kernel has since stopped trusting. Recomputing
             * closes that, but recomputing alone would let the budget SHRINK
             * mid-wait, which retroactively expires a watchdog that had not
             * expired. Keeping the maximum makes every transition safe in one
             * direction: a rate that rises (or falls back to the ceiling)
             * extends the escape, and a rate that falls never shortens it. */
            uint64_t budget_now = u_wait_watchdog_ticks(timeout_ms, ops);

            if (budget_now > wd_budget)
                wd_budget = budget_now;
            if (now > wd_high)
                wd_high = now;
            if (wd_high - wd_start >= wd_budget)
                return UTEST_WAIT_STALLED;
        }
        ops->wait();
    }
    return UTEST_WAIT_STALLED;
}

int test_usermode_bounded_wait(int (*ready)(void *ctx), void *ctx,
                               uint32_t timeout_ms,
                               const struct utest_wait_ops *ops)
{
    return u_bounded_wait(ready, ctx, timeout_ms, ops);
}

uint64_t test_usermode_wait_watchdog_ticks(uint32_t timeout_ms,
                                           const struct utest_wait_ops *ops)
{
    if (!ops)
        return 0;
    return u_wait_watchdog_ticks(timeout_ms, ops);
}

/* Name the stall on the wire, once per site that suffers one. Defined after the
 * record-framing macro it uses; declared here because the first caller is the
 * per-binary wait, which sits above that macro. */
static void u_wait_report_stall(const char *site, uint64_t detail);

/* ---- test-type taxonomy ----------------------------------------- *
 *
 * The launcher classifies each binary by filename prefix so it can
 * apply per-type policy: smoke runs FIRST with abort-on-FAIL, stress
 * loops N times, perf gets `classname="perf"` in the XML/JSON output.
 * An optional manifest `type=<value>` attribute overrides the
 * filename-derived default (for binaries that want to declare a
 * different policy or migrate without renaming).
 *
 * Prefix matching: `test_smoke_` beats `test_stress_` beats
 * `test_perf_` beats bare `test_`. All four prefixes are
 * case-sensitive (test_*.exe is lowercase by convention). Any
 * name that matches u_is_test_binary but none of the typed
 * prefixes defaults to UTEST_TYPE_CORRECTNESS.
 * --------------------------------------------------------------------- */

static utest_type_t u_type_for_name(const char *name)
{
    if (u_starts_with(name, "test_smoke_"))  return UTEST_TYPE_SMOKE;
    if (u_starts_with(name, "test_stress_")) return UTEST_TYPE_STRESS;
    if (u_starts_with(name, "test_perf_"))   return UTEST_TYPE_PERF;
    return UTEST_TYPE_CORRECTNESS;
}

/* Human-readable type label for the `classname` attribute in XML and
 * the `"type"` field in JSON. Kept short and hyphen-free so CI tools
 * can group by it without escaping. */
static const char *u_type_label(utest_type_t t)
{
    switch (t) {
    case UTEST_TYPE_SMOKE:       return "smoke";
    case UTEST_TYPE_STRESS:      return "stress";
    case UTEST_TYPE_PERF:        return "perf";
    case UTEST_TYPE_CORRECTNESS:
    default:                     return "correctness";
    }
}

/* Exact case-sensitive string equality. */
static int u_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Parse a manifest `type=<value>` attribute. Returns the enum for
 * known names, or UTEST_TYPE_CORRECTNESS for unrecognized inputs
 * (warn-and-continue). */
static utest_type_t u_type_from_attr(const char *val)
{
    if (u_streq(val, "smoke"))       return UTEST_TYPE_SMOKE;
    if (u_streq(val, "stress"))      return UTEST_TYPE_STRESS;
    if (u_streq(val, "perf"))        return UTEST_TYPE_PERF;
    if (u_streq(val, "correctness")) return UTEST_TYPE_CORRECTNESS;
    return UTEST_TYPE_CORRECTNESS;
}

/* Whether `type=<val>` names a policy this launcher understands.
 *
 * Split from the mapping above because the two questions have different
 * answers on failure. An UNKNOWN KEY stays silently ignored -- that is
 * the documented forward-compatibility contract, so an older kernel does
 * not choke on a newer manifest. A RECOGNISED key with an unparseable
 * VALUE is the opposite: defaulting it to correctness silently DEMOTES a
 * binary out of the smoke fast-fail phase, so `type=smoke#note` would
 * quietly stop gating the suite. That fails closed as a refusal. */
static int u_type_attr_known(const char *val)
{
    return u_streq(val, "smoke") || u_streq(val, "stress") ||
           u_streq(val, "perf")  || u_streq(val, "correctness");
}

/* ---- per-test isolation helpers --------------------------------- *
 *
 * For each binary the launcher creates a fresh scratch directory and
 * wipes a Registry subkey so leftover state cannot cross-pollute the
 * next binary. After the binary exits (PASS, FAIL, or timeout) the
 * launcher tears both down and records the task's final handle-table
 * count as a handle-leak signal. Opt out via boot.conf
 * utest_isolation=0 (consumed via test_usermode_set_isolation).
 * --------------------------------------------------------------------- */

/* Scratch root paths. `<name>` is the test binary filename minus the
 * ".exe" suffix (derived in u_derive_test_name below). Both roots use
 * backslash separators to match the Win32-native path convention. */
#define UTEST_SCRATCH_ROOT_L1 "C:\\Temp"
#define UTEST_SCRATCH_ROOT_L2 "C:\\Temp\\utest"
#define UTEST_REG_ROOT_L1     "SOFTWARE"
#define UTEST_REG_ROOT_L2     "SOFTWARE\\ImpossibleOS"
#define UTEST_REG_ROOT_L3     "SOFTWARE\\ImpossibleOS\\Test"

/* Hard caps on the iterative recursive-delete loop. The scratch dir is
 * launcher-owned and tests should not create deep hierarchies in it;
 * these caps exist as a paranoia floor against pathological FS state
 * that would otherwise infinite-loop the cleanup. */
#define UTEST_RMTREE_MAX_ENTRIES 512u
#define UTEST_RMTREE_MAX_DEPTH     8u

/* Strip a trailing ".exe" (case-insensitive) from `name_in` into
 * `out[out_cap]`. Returns 1 on success, 0 if the suffix was absent or
 * the resulting name would be empty / won't fit. `out` is always
 * NUL-terminated on success. */
static int u_derive_test_name(const char *name_in, char *out, uint32_t out_cap)
{
    uint32_t n = 0;
    uint32_t i;

    if (!name_in || !out || out_cap < 2)
        return 0;
    while (name_in[n]) n++;
    if (n < 5)   /* minimum "a.exe" is 5 chars; below that no stem remains */
        return 0;
    /* Match ".exe" / ".EXE" / mixed case at the tail. */
    if (name_in[n - 4] != '.' ||
        (name_in[n - 3] != 'e' && name_in[n - 3] != 'E') ||
        (name_in[n - 2] != 'x' && name_in[n - 2] != 'X') ||
        (name_in[n - 1] != 'e' && name_in[n - 1] != 'E'))
        return 0;
    if (n - 4 >= out_cap)
        return 0;
    for (i = 0; i < n - 4; i++)
        out[i] = name_in[i];
    out[i] = '\0';
    return 1;
}

/* Concat `parent\name` into `out[out_cap]`. Returns 1 on success, 0 on
 * overflow or empty input. */
static int u_path_join(const char *parent, const char *name,
                       char *out, uint32_t out_cap)
{
    uint32_t pi = 0;
    uint32_t ni = 0;

    if (!parent || !name || !out || out_cap < 3)
        return 0;
    while (parent[pi] && pi < out_cap - 2) {
        out[pi] = parent[pi];
        pi++;
    }
    if (parent[pi] != '\0' || pi == 0)
        return 0;  /* parent truncated or empty */
    if (out[pi - 1] != '\\' && out[pi - 1] != '/') {
        if (pi >= out_cap - 2)
            return 0;
        out[pi++] = '\\';
    }
    while (name[ni] && pi < out_cap - 1)
        out[pi++] = name[ni++];
    if (name[ni] != '\0')
        return 0;  /* name truncated */
    out[pi] = '\0';
    return 1;
}

/* Iterative recursive delete of a VFS path (file or directory). Safe
 * to call on a path that doesn't exist. Returns 0 on full cleanup, -1
 * if anything failed. Callers MUST check the return value and WARN on
 * failure so partial deletion does not silently let the next binary
 * reuse stale scratch state (Codex quality M 2026-04-20). */
static int u_rmtree(const char *path, uint32_t depth)
{
    struct vfs_node *n;
    uint8_t is_dir;

    if (depth > UTEST_RMTREE_MAX_DEPTH) {
        klog(LOG_WARN, "UTEST", "rmtree: depth > %u at '%s' -- aborting",
             (uint64_t)UTEST_RMTREE_MAX_DEPTH, path);
        return -1;
    }

    n = vfs_open(path, VFS_O_READ);
    if (!n)
        return 0;  /* absent -- nothing to do */
    is_dir = (uint8_t)(n->type & VFS_DIRECTORY);
    vfs_close(n);

    if (is_dir) {
        uint32_t pass;
        /* Loop: readdir[0] -> skip "."/".." -> recurse + unlink the
         * first real child -> restart readdir. Bounded by
         * UTEST_RMTREE_MAX_ENTRIES so a pathological FS state cannot
         * infinite-loop cleanup. */
        for (pass = 0; pass <= UTEST_RMTREE_MAX_ENTRIES; pass++) {
            struct vfs_node *dir;
            struct vfs_dirent *de;
            uint32_t idx;
            int found_real;
            char child_path[VFS_MAX_NAME + 64];
            char child_name[VFS_MAX_NAME];
            uint32_t ci;

            if (pass == UTEST_RMTREE_MAX_ENTRIES) {
                /* Cap exhausted -- directory still populated. Do NOT
                 * fall through to vfs_unlink: an attempt to delete a
                 * non-empty directory will fail, and more importantly
                 * we'd hide the cap-exhaustion in an ambiguous -1.
                 * Surface the cap hit explicitly so the caller can
                 * mark the run FAIL instead of letting stale state
                 * bleed through. */
                klog(LOG_WARN, "UTEST",
                     "rmtree: cap %u entries reached at '%s' -- partial cleanup",
                     (uint64_t)UTEST_RMTREE_MAX_ENTRIES, path);
                return -1;
            }

            dir = vfs_open(path, VFS_O_READ);
            if (!dir || !dir->ops || !dir->ops->readdir) {
                if (dir) vfs_close(dir);
                break;
            }

            found_real = 0;
            for (idx = 0; (de = dir->ops->readdir(dir, idx)) != (struct vfs_dirent *)0;
                 idx++) {
                /* Skip "." and ".." which some VFS backends include. */
                if (de->name[0] == '.' &&
                    (de->name[1] == '\0' ||
                     (de->name[1] == '.' && de->name[2] == '\0')))
                    continue;
                /* Snapshot the name (shared dirent storage). */
                for (ci = 0; de->name[ci] && ci < sizeof(child_name) - 1; ci++)
                    child_name[ci] = de->name[ci];
                child_name[ci] = '\0';
                found_real = 1;
                break;
            }
            vfs_close(dir);
            if (!found_real)
                break;

            if (!u_path_join(path, child_name, child_path, sizeof(child_path))) {
                klog(LOG_WARN, "UTEST",
                     "rmtree: path join overflowed for '%s\\%s' -- stopping",
                     path, child_name);
                return -1;
            }
            if (u_rmtree(child_path, depth + 1) != 0)
                return -1;  /* propagate cap / depth failures */
        }
    }

    return vfs_unlink(path);
}

/* Ensure a VFS directory exists. `parent` is the enclosing path, `full`
 * is the target. Tries to create; ignores errors (the target may
 * already exist). Returns 1 if the target is a directory after the
 * call, 0 otherwise. */
static int u_ensure_dir(const char *full)
{
    struct vfs_node *n = vfs_open(full, VFS_O_READ);
    if (n) {
        int is_dir = (n->type & VFS_DIRECTORY) != 0;
        vfs_close(n);
        return is_dir;
    }
    if (vfs_create(full, VFS_DIRECTORY) != 0)
        return 0;
    n = vfs_open(full, VFS_O_READ);
    if (!n)
        return 0;
    {
        int is_dir = (n->type & VFS_DIRECTORY) != 0;
        vfs_close(n);
        return is_dir;
    }
}

/* Pre-exec isolation: wipe stale state, create fresh scratch + Registry
 * subkey for `<name>`. Returns 0 on clean setup, -1 if anything failed
 * in a way that leaves stale state behind (cap-exhausted rmtree,
 * failed vfs_create). Caller treats -1 as a signal that the binary's
 * verdict should escalate to FAIL because the isolation contract
 * was violated. */
static int u_isolation_setup(const char *test_name)
{
    char path[VFS_MAX_NAME + 64];
    char reg_key[VFS_MAX_NAME + 64];
    int rc = 0;

    if (!vfs_is_mounted('C'))
        return 0;  /* no C:\ mount = nothing to isolate = not an error */

    /* L1 + L2 roots are shared across all binaries; create once, ignore
     * if they already exist. */
    (void)u_ensure_dir(UTEST_SCRATCH_ROOT_L1);
    (void)u_ensure_dir(UTEST_SCRATCH_ROOT_L2);

    /* Per-test scratch: wipe any leftover from a prior run, recreate
     * fresh. u_rmtree on a missing path is a no-op. If the pre-clear
     * rmtree fails (cap exhaustion, vfs error), the next vfs_create
     * may succeed at the existing-dir level but the contents are
     * still stale -- flag that so the caller can fail the run. */
    if (!u_path_join(UTEST_SCRATCH_ROOT_L2, test_name, path, sizeof(path))) {
        klog(LOG_WARN, "UTEST",
             "isolation: path join overflow for scratch '%s'", test_name);
        return -1;
    }
    if (u_rmtree(path, 0) != 0) {
        klog(LOG_WARN, "UTEST",
             "isolation: pre-clear of '%s' failed -- stale state may remain",
             path);
        rc = -1;
    }
    if (vfs_create(path, VFS_DIRECTORY) != 0) {
        klog(LOG_WARN, "UTEST",
             "isolation: failed to create scratch dir '%s'", path);
        rc = -1;
    }

    /* Registry subkey: HKLM\SOFTWARE\ImpossibleOS\Test\<name>.
     * RegCreateKeyEx is idempotent (open-or-create); RegDeleteTree
     * scrubs any prior content first so a previous run's dirty state
     * cannot bleed through. */
    {
        uint32_t pi = 0;
        uint32_t ni = 0;
        while (UTEST_REG_ROOT_L3[pi] && pi < sizeof(reg_key) - 2)
            reg_key[pi] = UTEST_REG_ROOT_L3[pi], pi++;
        if (pi < sizeof(reg_key) - 2)
            reg_key[pi++] = '\\';
        while (test_name[ni] && pi < sizeof(reg_key) - 1)
            reg_key[pi++] = test_name[ni++];
        reg_key[pi] = '\0';
        if (ni != 0 && test_name[ni] == '\0') {
            /* Clear any stale state, then ensure a fresh key exists. */
            (void)RegDeleteTree(HKEY_LOCAL_MACHINE, reg_key);
            {
                HKEY scratch = (HKEY)(uintptr_t)0;
                uint32_t disp = 0;
                if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, reg_key, 0,
                                   (char *)0, 0, 0, (void *)0,
                                   &scratch, &disp) == 0 && scratch)
                    (void)RegCloseKey(scratch);
            }
        }
    }

    return rc;
}

/* Post-exec isolation: read handle leak count (MUST be called BEFORE
 * task_cleanup -- once cleanup runs the handle table is gone). The
 * physical scratch + Registry tear-down has to wait until AFTER
 * task_cleanup closes the child's handles, because vfs_unlink rejects
 * targets with ref_count > 0: a leaked open handle on a file in the
 * scratch dir would otherwise block its deletion and leave stale state
 * for the next run (Codex quality H1, 2026-04-20). Split into two
 * phases accordingly: u_isolation_snapshot_leaks for the count-before-
 * teardown, u_isolation_reap for the delete-after-cleanup. */
static uint32_t u_isolation_snapshot_leaks(uint32_t child_pid)
{
    struct task *child = task_get_by_pid(child_pid);
    return child ? child->handle_table.count : 0u;
}

static int u_isolation_reap(const char *test_name)
{
    char path[VFS_MAX_NAME + 64];
    char reg_key[VFS_MAX_NAME + 64];
    int rc = 0;

    if (!vfs_is_mounted('C'))
        return 0;

    if (u_path_join(UTEST_SCRATCH_ROOT_L2, test_name, path, sizeof(path))) {
        if (u_rmtree(path, 0) != 0) {
            klog(LOG_WARN, "UTEST",
                 "isolation: post-run rmtree of '%s' failed -- stale scratch remains",
                 path);
            rc = -1;
        }
    }

    {
        uint32_t pi = 0;
        uint32_t ni = 0;
        while (UTEST_REG_ROOT_L3[pi] && pi < sizeof(reg_key) - 2)
            reg_key[pi] = UTEST_REG_ROOT_L3[pi], pi++;
        if (pi < sizeof(reg_key) - 2)
            reg_key[pi++] = '\\';
        while (test_name[ni] && pi < sizeof(reg_key) - 1)
            reg_key[pi++] = test_name[ni++];
        reg_key[pi] = '\0';
        if (ni != 0 && test_name[ni] == '\0')
            (void)RegDeleteTree(HKEY_LOCAL_MACHINE, reg_key);
    }

    return rc;
}

/* ---- Optional `tests/usermode-cleanup.manifest` ------------------- *
 *
 * Format: one path or Registry key per line, `#` starts a comment.
 * A line starting with `C:\` is deleted via vfs-rmtree (file or
 * directory). A line starting with `HKLM\` is deleted via
 * RegDeleteTree. Any other prefix is logged and skipped. Called once
 * per binary AFTER the per-test scratch + Registry teardown.
 *
 * Scope: tests that legitimately touch global state (e.g. DLL cache,
 * network sockets) can enumerate the paths/keys to scrub here.
 * --------------------------------------------------------------------- */

#define UTEST_CLEANUP_ARENA_BYTES 4096u
#define UTEST_CLEANUP_ARENA_PAGES 1u

/* Reject path-traversal components in a cleanup-manifest entry.
 * Returns 1 if the path contains any `..` component (after a `\`, `/`,
 * or at the start of the string), 0 if it is clean. Called for both
 * the `C:\...` and `HKLM\...` lines -- a `..` there is either an
 * attacker or a typo; either way reject.
 *
 * Without this guard the raw prefix check authorizes
 * `C:\Impossible\..\hello.txt` because the VFS walker resolves real
 * `..` entries in IXFS directories, so the delete escapes outside the
 * allowed subtree. The prefix match alone is not a containment
 * primitive; we have to ban the characters that let the walker leave
 * the subtree. */
static int u_path_has_traversal(const char *p)
{
    int at_component_start = 1;
    while (*p) {
        if (at_component_start && p[0] == '.' && p[1] == '.' &&
            (p[2] == '\0' || p[2] == '\\' || p[2] == '/'))
            return 1;
        at_component_start = (*p == '\\' || *p == '/');
        p++;
    }
    return 0;
}

static void u_cleanup_manifest_apply(void)
{
    struct vfs_node *f;
    uintptr_t arena_phys;
    char *arena;
    uint32_t size;
    int n;
    char *p;
    char *end;

    if (!vfs_is_mounted('C'))
        return;

    f = vfs_open("C:\\tests\\usermode-cleanup.manifest", VFS_O_READ);
    if (!f)
        return;  /* absent = normal */

    size = f->size;
    if (size == 0 || size >= UTEST_CLEANUP_ARENA_BYTES) {
        vfs_close(f);
        if (size >= UTEST_CLEANUP_ARENA_BYTES)
            klog(LOG_WARN, "UTEST",
                 "cleanup manifest %u bytes >= %u -- skipping",
                 (uint64_t)size, (uint64_t)UTEST_CLEANUP_ARENA_BYTES);
        return;
    }

    arena_phys = pmm_alloc_contiguous(UTEST_CLEANUP_ARENA_PAGES);
    if (!arena_phys) {
        vfs_close(f);
        klog(LOG_WARN, "UTEST", "cleanup manifest: pmm alloc failed");
        return;
    }
    arena = (char *)arena_phys;

    n = vfs_read(f, 0, size, (uint8_t *)arena);
    vfs_close(f);
    if (n <= 0 || (uint32_t)n != size) {
        pmm_free_frame(arena_phys);
        return;
    }

    /* In-place tokenize by newline and process each entry. */
    p = arena;
    end = arena + n;
    while (p < end) {
        char *line_start;
        char *line_end;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
            p++;
        if (p >= end) break;
        if (*p == '\n') { p++; continue; }
        if (*p == '#') {
            while (p < end && *p != '\n') p++;
            continue;
        }
        line_start = p;
        while (p < end && *p != '\n' && *p != '\r' && *p != '#')
            p++;
        line_end = p;
        while (line_end > line_start &&
               (line_end[-1] == ' ' || line_end[-1] == '\t'))
            line_end--;
        while (p < end && *p != '\n') p++;
        if (p < end) { *p = '\0'; p++; }
        if (line_end == line_start) continue;
        *line_end = '\0';

        if (line_start[0] == 'C' && line_start[1] == ':' &&
            line_start[2] == '\\') {
            /* Guard: cleanup manifest C:\ entries must live under
             * `C:\Impossible\` or the per-test scratch root so a typo
             * / malicious manifest cannot wipe hello.txt or cmd.exe
             * during a test boot. The per-test scratch at
             * `C:\Temp\utest\<stem>` is already scrubbed automatically
             * -- the cleanup manifest is for state OUTSIDE that root
             * (docs say DLL cache, etc.), which lives in
             * `C:\Impossible\` on this OS. */
            if (!(line_start[3] == 'I' && line_start[4] == 'm' &&
                  line_start[5] == 'p' && line_start[6] == 'o' &&
                  line_start[7] == 's' && line_start[8] == 's' &&
                  line_start[9] == 'i' && line_start[10] == 'b' &&
                  line_start[11] == 'l' && line_start[12] == 'e' &&
                  line_start[13] == '\\')) {
                klog(LOG_WARN, "UTEST",
                     "cleanup manifest: rejecting C:\\ entry outside C:\\Impossible\\ -- '%s'",
                     line_start);
                continue;
            }
            /* Reject `..` components -- a path like
             * `C:\Impossible\..\hello.txt` passes the prefix gate but
             * the VFS walker would escape outside the subtree. Codex
             * H3, 2026-04-20. */
            if (u_path_has_traversal(line_start)) {
                klog(LOG_WARN, "UTEST",
                     "cleanup manifest: rejecting C:\\ entry with '..' traversal -- '%s'",
                     line_start);
                continue;
            }
            (void)u_rmtree(line_start, 0);
        } else if (line_start[0] == 'H' && line_start[1] == 'K' &&
                   line_start[2] == 'L' && line_start[3] == 'M' &&
                   line_start[4] == '\\') {
            const char *sub = line_start + 5;
            /* Guard: reject empty suffix (`HKLM\\` alone).  RegDeleteTree
             * with an empty lpSubKey wipes every child of
             * HKEY_LOCAL_MACHINE, i.e. the whole registry, which is
             * catastrophic even under test=1.  Also restrict to the
             * ImpossibleOS test subtree to bound the blast radius. */
            if (sub[0] == '\0') {
                klog(LOG_WARN, "UTEST",
                     "cleanup manifest: rejecting bare 'HKLM\\' (would wipe root) -- '%s'",
                     line_start);
                continue;
            }
            if (!(sub[0] == 'S' && sub[1] == 'O' && sub[2] == 'F' &&
                  sub[3] == 'T' && sub[4] == 'W' && sub[5] == 'A' &&
                  sub[6] == 'R' && sub[7] == 'E' && sub[8] == '\\' &&
                  sub[9] == 'I' && sub[10] == 'm' && sub[11] == 'p' &&
                  sub[12] == 'o' && sub[13] == 's' && sub[14] == 's' &&
                  sub[15] == 'i' && sub[16] == 'b' && sub[17] == 'l' &&
                  sub[18] == 'e')) {
                klog(LOG_WARN, "UTEST",
                     "cleanup manifest: rejecting HKLM entry outside SOFTWARE\\Impossible -- '%s'",
                     line_start);
                continue;
            }
            /* Same `..` defense applies to Registry paths even though
             * reg_walk_path is stricter -- defense in depth. */
            if (u_path_has_traversal(sub)) {
                klog(LOG_WARN, "UTEST",
                     "cleanup manifest: rejecting HKLM entry with '..' traversal -- '%s'",
                     line_start);
                continue;
            }
            (void)RegDeleteTree(HKEY_LOCAL_MACHINE, sub);
        } else {
            klog(LOG_WARN, "UTEST",
                 "cleanup manifest: unknown prefix '%s' -- skipped",
                 line_start);
        }
    }

    pmm_free_frame(arena_phys);
}

/* ---- Manifest parser ------------------------------------------------ *
 *
 * Manifest format: one `test_*.exe` filename per line; `#` starts a
 * line comment; leading/trailing whitespace ignored; blank lines
 * skipped. First line that is not blank/comment is the first binary
 * to run, in file order. Runnable entries go straight into the
 * enumeration plan, so UTEST_PLAN_MAX is the only thing bounding how
 * many a manifest may list.
 *
 * Location: `C:\tests\usermode.manifest`. Absent = launcher falls
 * back to scanning C:\ root (legacy behavior). Makefile userland
 * target deploys the manifest file if `tests/usermode.manifest`
 * exists in the source tree.
 *
 * Parsed names are pointers into a single arena buffer holding the
 * file's text. The arena is allocated once per test_usermode_run()
 * invocation from PMM (8 KiB exceeds kmalloc's 4 KiB ceiling per
 * CLAUDE.md Freestanding Kernel rules) and freed after execution, so no
 * permanent allocation survives and no plan entry outlives its name.
 * ------------------------------------------------------------------ */

/* Refused manifest entries a run can publish individually. Held apart
 * from the runnable set because a refusal costs no task slot and must
 * never be crowded out by runnable entries; small because a manifest with
 * dozens of malformed lines is already a broken manifest, and exhausting
 * this array is itself published as a fail-closed marker rather than
 * silently truncated. */
#define UTEST_MANIFEST_REFUSAL_MAX 64u
/* Glob-discovered refusals a run can publish individually. Captured
 * DURING the planning walk rather than by a second traversal: C: is IXFS,
 * whose readdir restarts at entry zero for every index and kmallocs a
 * block buffer per call, so each full traversal costs O(N^2) block reads
 * plus O(N) allocations -- and the no-manifest path is the live one, so an
 * extra pass is paid on every boot. Smaller than the manifest cap because
 * a directory holding more than this many malformed test-shaped files is
 * already broken, and exhausting it is published as a fail-closed record
 * rather than truncated. */
#define UTEST_GLOB_REFUSAL_MAX 16u
/* Bytes of sanitized prefix retained per captured refusal. The identity's
 * prefix budget is at most the derived bound minus its fixed shape, so
 * this is sized to cover the widest budget a small ordinal can leave. */
#define UTEST_REFUSAL_PREFIX_STORE 20u
#define UTEST_MANIFEST_ARENA_BYTES 8192u  /* 128 entries * avg 64 bytes */
#define UTEST_MANIFEST_ARENA_PAGES 2u     /* 2 x 4 KiB */

/* ---- The immutable enumeration plan -------------------------------- *
 *
 * The launcher used to walk C:\ TWICE -- once to count `total_planned`,
 * once to execute -- with live children running in between. Any binary
 * that appeared, disappeared or was renamed across that window made the
 * two walks disagree, and the launcher could only report the disagreement
 * as an aggregate count: it never knew WHICH planned binary went missing.
 *
 * So enumeration happens ONCE and produces this plan; execution consumes
 * the plan and never calls readdir again. `total_planned` is the plan's
 * length by construction and `total_ran` counts entries consumed from the
 * SAME array, so the two can no longer describe different sets -- and a
 * binary deleted between planning and execution is a NAMED entry that
 * failed to open rather than an anonymous decrement.
 *
 * Name lifetimes, which are the one subtlety here:
 *   - manifest-sourced names point into the manifest arena, which lives
 *     until u_manifest_free() at the end of the run;
 *   - glob-sourced names are COPIED into this plan's own arena, because
 *     vfs_readdir returns shared dirent storage that the next call reuses;
 *   - aggregate records carry string literals.
 * Both arenas are released at the same point in test_usermode_run(), after
 * execution, so no plan entry can outlive the bytes it points at.
 * ------------------------------------------------------------------- */

/* Entries one run can plan, from either source. Deliberately larger than
 * the 128-entry runnable array this replaced: the plan, not the manifest
 * parser, is now the binding capacity, so a long manifest no longer loses
 * its tail to a glob fallback that could not reconstruct an entry's type,
 * task cost or ordering. */
#define UTEST_PLAN_MAX 256u
/* Bytes reserved per interned plan name. Fixed-stride rather than packed
 * so an entry's storage is O(1) to reserve and the arena can never be
 * fragmented by a long name; pinned against the derived record bound by a
 * _Static_assert beside UTEST_MAX_BINARY_NAME, which is derived further
 * down this file and so cannot be referenced here. */
#define UTEST_PLAN_NAME_SLOT  64u
#define UTEST_PLAN_NAME_BYTES (UTEST_PLAN_MAX * UTEST_PLAN_NAME_SLOT)
/* The arena holds one slot per ENTRY, and u_plan_intern only ever runs
 * after u_plan_alloc has already taken an entry slot -- so the entry array
 * is always the binding limit and the arena can never be the thing that
 * fills first. That is a deliberate property, not a coincidence: it means
 * a legitimate full plan of interned names cannot be refused for want of
 * name storage. The intern-overflow branch and its rollback are therefore
 * unreachable while this holds, and are kept fail-closed for the same
 * reason u_build_refusal_id's overflow arm is. */
_Static_assert(UTEST_PLAN_NAME_BYTES >= UTEST_PLAN_MAX * UTEST_PLAN_NAME_SLOT,
               "the plan name arena must hold one slot per plan entry, or "
               "u_plan_intern could refuse a name for a plan that still "
               "has an entry slot free");

typedef enum {
    UTEST_PLAN_RUN     = 0,   /* a binary to launch */
    UTEST_PLAN_REFUSAL = 1,   /* an identity to publish as a counted failure */
} utest_plan_kind_t;

struct plan_entry {
    /* Stable for the whole run -- see the lifetime rules above. */
    const char  *name;
    /* REFUSAL only: a literal reason, or NULL to derive one from
     * `verdict`. RUN entries leave it NULL. */
    const char  *reason;
    uint32_t     digest;
    utest_type_t type;
    uint8_t      kind;           /* utest_plan_kind_t */
    uint8_t      verdict;        /* utest_name_verdict_t, REFUSAL only */
    /* Expected total task_create cost for this binary, including nested
     * sys_fork calls. Default 1 (the launcher-spawned task itself).
     * Manifest entries tag fork-heavy binaries with `expects_tasks=<N>`
     * so the pre-flight budget check sums actual slot consumption instead
     * of counting binaries. Clamped to 1..255 -- a binary claiming more
     * than 255 task slots almost certainly has a bug. */
    uint8_t      expects_tasks;
    /* 1 = `name` is the EXACT filesystem identity this entry stands for.
     *
     * Load-bearing for the terminal-refusal rule. A manifest refusal
     * keeps the raw bytes off the manifest line, and a duplicate-policy
     * refusal was a runnable entry a moment ago, so both are exact. A
     * GLOB refusal is not: it stores a sanitized, truncated prefix with
     * illegal bytes rewritten to `_`, so `test_bad?.exe` is stored as
     * `test_bad_.exe` and would identity-match a genuinely different file
     * of that name. Matching a lossy prefix as an identity would suppress
     * a legitimate requested binary and leave its failures unobserved --
     * strictly worse than the double publication the rule prevents. */
    uint8_t      exact_name;
    /* 1 = this record stands for a TRUSTED smoke declaration that the
     * filter selected. The smoke gate is derived from this bit alone.
     *
     * Deliberately ONE bit rather than a (type, selected) pair: merging
     * two declarations for one identity could otherwise pair the SMOKE
     * type of one with the filter selection of another and invent a gate
     * neither line asked for. Both halves are decided together, at the
     * point the declaration is read, and only ever OR-ed afterwards.
     *
     * "Trusted" excludes name refusals -- their type is never derived, so
     * untrusted bytes cannot reach the gate. "Selected" matters because
     * refusals PUBLISH regardless of `utest_filter=` (the filter picks
     * among identities the launcher can trust), while an abort is a
     * statement about the run the operator actually asked for. */
    uint8_t      smoke_selected;
    /* RUN only: SHA-256 of the binary's bytes as the planning walk saw
     * them. The plan freezes WHICH names run; this freezes WHAT runs under
     * each name, which the name alone cannot say -- a child that replaces a
     * planned binary before its turn would otherwise have its replacement
     * launched, reported and attributed to the planned entry with every
     * count reconciling.
     *
     * SHA-256 rather than the 32-bit FNV-1a `digest` above, and the two are
     * deliberately separate fields: `digest` correlates a REFUSED NAME
     * across runs, where u_name_digest's own comment records that collision
     * resistance is not claimed because the ORDINAL supplies uniqueness.
     * Nothing supplies uniqueness here -- this value alone decides whether
     * adversary-influenced bytes may execute -- so a width an attacker
     * could search is not an identity.
     *
     * Meaningful only for UTEST_PLAN_RUN. u_plan_freeze_identities converts
     * any entry it cannot digest into a REFUSAL, so "a RUN entry carries a
     * frozen identity" is structural rather than a flag anyone must test. */
    uint8_t      content_digest[SHA256_DIGEST_LEN];
};

#define UTEST_PLAN_BLOCK_BYTES                                             \
    (UTEST_PLAN_MAX * (uint32_t)sizeof(struct plan_entry)                  \
     + UTEST_PLAN_NAME_BYTES)
#define UTEST_PLAN_PAGES ((UTEST_PLAN_BLOCK_BYTES + 4095u) / 4096u)

/* The plan is PMM-backed, not a stack array: UTEST_PLAN_MAX entries plus
 * their name arena is far past what the launcher's frame can hold, and
 * CLAUDE.md's freestanding rules put anything over 4 KiB on
 * pmm_alloc_contiguous rather than kmalloc. */
struct plan_state {
    struct plan_entry *entries;      /* NULL until u_plan_init succeeds */
    char              *names;
    uint32_t           count;
    uint32_t           names_used;
    /* Runnable identities the `utest_filter=` value excluded. Owned by the
     * plan because exclusion happens exactly once, at plan time, for both
     * enumeration sources. */
    uint32_t           skipped_by_filter;
    uintptr_t          block_phys;
    /* 1 = the entry array or the name arena filled. The plan is then no
     * longer the complete enumeration, so nothing may run off it. */
    int                overflowed;
    /* `count` as it stood at the FIRST transition to `overflowed` -- the
     * entries the plan RETAINED when the cap bit, which is what the
     * plan-full aggregate publishes. Captured here rather than read off
     * `count` at publication time because `count` is mutated afterwards:
     * u_plan_drop_runs rewrites it, and on a manifest that overflows BOTH
     * the plan cap and the refusal cap it is dropped once BEFORE the
     * plan-full aggregate is ever emitted. Reading it late reported the
     * staged-refusal count as the retained count. */
    uint32_t           kept_at_overflow;
    /* 1 = the backing block could not be allocated at all. Distinct from
     * `overflowed` because it must not be reported as a capacity problem
     * with a directory. */
    int                alloc_failed;
    /* 1 = a SMOKE-declared identity was refused during planning, so the
     * suite must abort exactly as it would for a smoke binary that ran
     * and failed.
     *
     * Only a DECLARED smoke type sets this. A name-refusal never does:
     * bytes the launcher declined to trust must not be able to claim
     * smoke policy and abort the suite through the smoke gate, which is
     * why u_glob_next's refusal path never reaches u_type_for_name. A
     * duplicate-policy conflict is the opposite case -- both lines were
     * classified and accepted, so their `type=` is trusted, and dropping
     * a smoke prerequisite to a counted failure while the rest of the
     * suite runs on is exactly the abort bypass the gate exists to
     * prevent. */
    int                smoke_refused;
};

struct manifest_state {
    /* Runnable entries live in the PLAN, not here: an array capped
     * independently of the plan is exactly what used to drop manifest
     * entries past its cap and hand the tail to a glob fallback that
     * could not reconstruct their metadata. */
    /* Manifest lines ACCEPTED as runnable -- which is a statement about
     * the manifest, not about the plan. It is deliberately NOT the number
     * of entries that survived into the plan: `utest_filter=` exclusions
     * and refused-identity vetoes both remove entries afterwards, and a
     * manifest whose every entry was filtered out is still authoritative.
     * Conflating the two lets a filtered run report "no usable manifest"
     * and fall back to scanning C:\, which would execute an on-disk
     * binary the manifest never listed. */
    uint32_t     count;
    /* Refused entries, in their OWN array rather than parallel to the
     * runnable ones. Keeping them separate is load-bearing, not tidiness:
     * when refusals shared the runnable array they competed for the same
     * slots, and the cap check ran BEFORE classification and stopped
     * parsing -- so a malformed entry in the tail of an oversized manifest
     * was never classified at all. The "tail runs via glob" fallback could
     * not recover it either, because a name refused for an embedded NUL or
     * an illegal byte may not exist as a directory entry in the first
     * place. Split apart, a full runnable set can no longer hide a
     * refusal. They stay HERE rather than moving into the plan because
     * u_refusal_already_seen() dedups dirents against the raw, unbounded
     * refused name, which the plan's fixed-stride arena cannot hold.
     *
     * `refused_digest[]` is the FNV-1a of the entry's EXACT line span,
     * taken at parse time because that is the only point where the span
     * length is still known -- an entry refused for an embedded NUL has
     * a C string shorter than the bytes it came from, so hashing it
     * later would hash the truncation. */
    const char  *refused_names[UTEST_MANIFEST_REFUSAL_MAX];
    uint32_t     refused_digest[UTEST_MANIFEST_REFUSAL_MAX];
    uint8_t      refused_verdict[UTEST_MANIFEST_REFUSAL_MAX];
    /* Provenance the smoke gate is DERIVED from, carried per refusal so
     * no single code path has to remember to raise a flag. Set only when
     * the NAME was accepted and merely an attribute was unusable -- that
     * `type=` came off a line the classifier trusted -- AND the filter
     * selected the entry. A name refusal never sets it, which is what
     * stops untrusted bytes from reaching the gate. */
    uint8_t      refused_smoke_selected[UTEST_MANIFEST_REFUSAL_MAX];
    uint32_t     refused_count;
    int          refused_overflowed; /* 1 = more refusals than we can hold */
    /* 1 = a manifest EXISTS but could not be parsed at all (too large for
     * the arena), so none of its entries were examined. Distinct from
     * "absent", which is normal, and published as a counted failure
     * rather than degraded to glob in silence. */
    int          unreadable;
    char        *arena;         /* pmm_alloc_contiguous()'d; NULL if not loaded */
    uintptr_t    arena_phys;    /* matching physical base for pmm_free_frame loop */
    uint32_t     arena_used;
    uint32_t     arena_cap;
};

/* This struct is a LOCAL in test_usermode_run, so every array added to it
 * comes off the stack the launcher runs on. Pinning the size makes a
 * future per-entry field an explicit decision instead of a stack overrun
 * discovered on hardware. */
_Static_assert(sizeof(struct manifest_state) <= 4096,
               "manifest_state is stack-allocated by test_usermode_run -- "
               "bound the STRUCT so a new per-entry field is a deliberate "
               "decision; the FRAME is not bounded by this and the deepest "
               "chain below here is u_rmtree's depth-8 recursion");

/* Defined below, next to the case-folding comparator they depend on. The
 * parser appends runnable entries to the plan as it reads them, which is
 * what removes the old independent runnable cap. */
static int u_plan_add_run(struct plan_state *ps, const char *name,
                          utest_type_t type, uint8_t expects, int intern);

/* The classification half of the parser, split from the loader below so
 * it can be exercised over caller-owned bytes. */
static void u_manifest_parse(struct manifest_state *ms, struct plan_state *ps,
                             char *buf, uint32_t len);
/* Applied by the parser once the refusal set is complete -- a refused
 * identity vetoes a runnable one that was read before it. */
static void u_plan_suppress_refused(struct plan_state *ps,
                                    struct manifest_state *ms);

/* Put a manifest_state into its "nothing loaded" state.
 *
 * SEPARATE from u_manifest_load because the caller must be able to reach
 * this without reaching the loader: manifest_state is an automatic in
 * test_usermode_run, and every later consumer -- the refusal loops, the
 * unreadable flag, and u_manifest_free's pmm_free_frame walk -- reads it
 * unconditionally. A path that skips the loader (plan allocation failing
 * is one) would otherwise hand u_manifest_free a stale `arena_phys` off
 * the stack and free two arbitrary physical frames. */
static void u_manifest_reset(struct manifest_state *ms)
{
    ms->count              = 0;
    ms->refused_count      = 0;
    ms->refused_overflowed = 0;
    ms->unreadable         = 0;
    ms->arena              = (char *)0;
    ms->arena_phys         = 0;
    ms->arena_used         = 0;
    ms->arena_cap          = 0;
}

static int u_manifest_load(struct manifest_state *ms, struct plan_state *ps)
{
    struct vfs_node *f;
    uint8_t *buf;
    int n;
    uint32_t size;

    u_manifest_reset(ms);

    f = vfs_open("C:\\tests\\usermode.manifest", VFS_O_READ);
    if (!f)
        return 0;  /* absent is normal */

    size = f->size;
    /* Cap at ARENA - 1 to guarantee room for a sentinel NUL at
     * arena[size].  Otherwise a file of exactly ARENA bytes whose last
     * line lacks a trailing newline lets the in-place tokenizer's
     * `*line_end = '\0'` write at arena[size], which is past the end
     * of the allocation -- a kernel-heap OOB write triggered by a
     * user-provided file. */
    if (size == 0 || size >= UTEST_MANIFEST_ARENA_BYTES) {
        vfs_close(f);
        if (size >= UTEST_MANIFEST_ARENA_BYTES) {
            /* An oversized manifest is DISCARDED WHOLE, so every entry in
             * it -- including every malformed name this section exists to
             * count -- goes unexamined, and the glob fallback provably
             * cannot recover one: a name refused for an embedded NUL or an
             * illegal byte may not exist as a directory entry at all.
             * Degrading to glob with only a WARN is therefore the same
             * false green the in-array overflow already publishes as a
             * counted record, and it is what the bare-metal
             * disk-sourced-config rule forbids (hard-fail on overflow,
             * never silent truncate -- the 2026-04-21 boot.conf incident).
             * Flagged so the run publishes it as a counted failure. */
            ms->unreadable = 1;
            klog(LOG_WARN, "UTEST",
                 "manifest size %u >= %u -- unparseable, counted as a "
                 "failure and falling back to glob",
                 (uint64_t)size, (uint64_t)UTEST_MANIFEST_ARENA_BYTES);
        }
        return 0;
    }

    /* 8 KiB arena uses pmm_alloc_contiguous (CLAUDE.md Freestanding
     * Kernel rules: kmalloc is for <=4 KiB, larger buffers go through
     * PMM). Physical pages are identity-mapped in the kernel VA, so
     * the physical base doubles as a valid kernel virtual pointer. */
    ms->arena_cap  = UTEST_MANIFEST_ARENA_BYTES;
    ms->arena_phys = pmm_alloc_contiguous(UTEST_MANIFEST_ARENA_PAGES);
    if (!ms->arena_phys) {
        vfs_close(f);
        /* Same false green as the oversized case: a manifest EXISTS and
         * none of it was parsed, so any entry not also present as a
         * dirent -- including every malformed one -- would vanish behind
         * a glob fallback that reported success. Every post-open failure
         * is counted, not just the one that is easy to foresee. */
        ms->unreadable = 1;
        klog(LOG_WARN, "UTEST",
             "manifest pmm_alloc_contiguous(%u pages) failed -- counted as a "
             "failure and falling back to glob",
             (uint64_t)UTEST_MANIFEST_ARENA_PAGES);
        return 0;
    }
    ms->arena = (char *)ms->arena_phys;
    buf = (uint8_t *)ms->arena;

    n = vfs_read(f, 0, size, buf);
    vfs_close(f);
    if (n <= 0 || (uint32_t)n != size) {
        ms->unreadable = 1;
        klog(LOG_WARN, "UTEST",
             "manifest short read -- counted as a failure and falling back "
             "to glob");
        for (uint32_t p = 0; p < UTEST_MANIFEST_ARENA_PAGES; p++)
            pmm_free_frame(ms->arena_phys + (uintptr_t)p * 4096u);
        ms->arena      = (char *)0;
        ms->arena_phys = 0;
        return 0;
    }

    u_manifest_parse(ms, ps, ms->arena, (uint32_t)n);
    return ms->count > 0 ? 1 : 0;
}

/* Walk manifest TEXT and classify every line.
 *
 * Split out of u_manifest_load so the classification -- which is the half
 * this section changed, since runnable entries now land in the plan
 * instead of a capped array -- can be exercised over caller-owned bytes.
 * The loader half still owns vfs_open/vfs_read and the arena lifetime,
 * and stays out of reach of the test policy that forbids live
 * infrastructure in test_*.c.
 *
 * `buf` is tokenized IN PLACE and must remain valid for as long as the
 * plan does: every accepted name is stored as a pointer into it.
 * Callers pass a buffer of at least `len + 1` bytes, because the
 * tokenizer's `*line_end = '\0'` can write at buf[len]. */
static void u_manifest_parse(struct manifest_state *ms, struct plan_state *ps,
                             char *buf, uint32_t len)
{
    {
        char *p = buf;
        char *end = buf + len;
        while (p < end) {
            char *line_start;
            char *line_end;
            /* Skip leading ws on the line. */
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
                p++;
            if (p >= end) break;
            if (*p == '\n') { p++; continue; }       /* blank line */
            if (*p == '#') {                         /* comment */
                while (p < end && *p != '\n') p++;
                continue;
            }
            line_start = p;
            /* A `#` ends the payload only when it INTRODUCES a comment --
             * at line start (handled above) or after whitespace. Treating
             * every `#` as a comment start silently rewrote the name
             * itself: `test_a#b.exe` became `test_a`, which is not
             * test-shaped, so it was ignored as a stray line instead of
             * refused for a charset violation. With any other valid line
             * making the manifest authoritative the glob is skipped too,
             * so that entry reached no counter and no artifact -- the
             * lexer undoing the very refusal the classifier exists for. */
            while (p < end && *p != '\n' && *p != '\r') {
                if (*p == '#' && p > line_start &&
                    (p[-1] == ' ' || p[-1] == '\t'))
                    break;
                p++;
            }
            line_end = p;
            /* Strip trailing ws. */
            while (line_end > line_start &&
                   (line_end[-1] == ' ' || line_end[-1] == '\t'))
                line_end--;
            /* Skip the rest of the line (comment tail or EOL). */
            while (p < end && *p != '\n') p++;
            if (p < end) { *p = '\0'; p++; }
            /* Everything from line_start..line_end is the raw line
             * payload: a whitespace-delimited list of tokens where
             * the first is the binary name and subsequent tokens are
             * `key=value` attributes. Today only `type=<smoke|stress|
             * perf|correctness>` is recognized (of TODO-04); other
             * key/value pairs are silently ignored so a future schema
             * addition doesn't break older kernels. */
            if (line_end > line_start) {
                char *name_start;
                char *tok = line_start;
                utest_type_t entry_type;
                utest_name_verdict_t verdict;
                uint32_t name_span;
                int bad_attr = 0;
                /* Terminate the line in-place; safe by the ARENA-1
                 * cap above. */
                *line_end = '\0';

                /* Extract the first token (binary name). Stops at
                 * first whitespace inside the line. */
                name_start = tok;
                /* True span of the name token, measured against the
                 * LINE's end rather than by C-string scanning, and
                 * captured BEFORE the tokenizer writes its terminators.
                 * The scan below stops at an embedded NUL, so without
                 * this the parser -- and every consumer after it -- would
                 * see only the bytes preceding that NUL and never know
                 * the entry was longer. Comparing this span against the
                 * C length is what makes such an entry a counted refusal
                 * instead of a silently truncated name. */
                {
                    const char *e = name_start;
                    while (e < line_end && *e != ' ' && *e != '\t')
                        e++;
                    name_span = (uint32_t)(e - name_start);
                }
                while (*tok && *tok != ' ' && *tok != '\t')
                    tok++;
                if (*tok) {
                    *tok++ = '\0';
                    /* Skip over any run of whitespace between tokens. */
                    while (*tok == ' ' || *tok == '\t') tok++;
                }
                /* Default: infer type from filename prefix. Default
                 * expects_tasks to 1 (launcher task only). */
                entry_type = u_type_for_name(name_start);
                uint32_t entry_expects = 1u;
                /* Walk remaining tokens, honouring `type=<value>` and
                 * `expects_tasks=<N>`. */
                while (*tok) {
                    char *kv_end = tok;
                    while (*kv_end && *kv_end != ' ' && *kv_end != '\t')
                        kv_end++;
                    if (*kv_end) { *kv_end = '\0'; kv_end++; }
                    if (tok[0] == 't' && tok[1] == 'y' && tok[2] == 'p' &&
                        tok[3] == 'e' && tok[4] == '=') {
                        if (!u_type_attr_known(tok + 5))
                            bad_attr = 1;
                        entry_type = u_type_from_attr(tok + 5);
                    } else if (tok[0] == 'e' && tok[1] == 'x' &&
                               tok[2] == 'p' && tok[3] == 'e' &&
                               tok[4] == 'c' && tok[5] == 't' &&
                               tok[6] == 's' && tok[7] == '_' &&
                               tok[8] == 't' && tok[9] == 'a' &&
                               tok[10] == 's' && tok[11] == 'k' &&
                               tok[12] == 's' && tok[13] == '=') {
                        /* Inline decimal parse -- up to 3 digits fit in
                         * the 1..255 uint8 slot without overflow. */
                        const char *p = tok + 14;
                        const char *d0 = p;
                        uint32_t n = 0;
                        while (*p >= '0' && *p <= '9' && n < 10000u)
                            n = n * 10u + (uint32_t)(*p++ - '0');
                        /* The whole VALUE must be decimal, not merely
                         * start that way. A prefix parse silently turned
                         * `expects_tasks=#note` and `expects_tasks=1O`
                         * into 1, understating task cost until a launch
                         * or a fork failed -- and `#` is no longer
                         * stripped as a comment, so the first spelling is
                         * now reachable. Same recognised-attribute class
                         * as an unusable `type=`. */
                        if (p == d0 || *p != '\0')
                            bad_attr = 1;
                        /* Out of range is REFUSED, not coerced. Silently
                         * turning 0 into 1 or clamping 300 to 255 accepts
                         * a recognised attribute the parser itself calls
                         * nonsensical, and understates the task budget
                         * the preflight check exists to police -- the same
                         * lenience the type= fix just removed. */
                        if (n < 1u || n > 255u)
                            bad_attr = 1;
                        if (n == 0) n = 1;
                        if (n > 255u) n = 255u;
                        entry_expects = n;
                    }
                    tok = kv_end;
                    while (*tok == ' ' || *tok == '\t') tok++;
                }
                /* Enforce the manifest trust boundary on the bare name:
                 * test_*.exe, inside the accepted charset, within the
                 * derived record bound, no `..`, no embedded NUL.
                 *
                 * A refusal is KEPT, not dropped. Dropping it here is
                 * what used to make a rejected entry invisible: it never
                 * reached total_planned, so the artifacts and the host
                 * recount described a run that silently skipped a binary
                 * somebody asked for. The entry stays in the array with
                 * its verdict and its span digest so both walks can count
                 * it and the run walk can publish it as a failure.
                 *
                 * The raw name is NOT logged. It is untrusted bytes, and
                 * a control byte in it would split this very WARN across
                 * physical serial lines -- the sanitized identity the
                 * record carries is emitted by the run walk instead. */
                verdict = u_classify_name_span(name_start, name_span);
                /* A manifest line is an INTENDED binary -- comments and
                 * blank lines were stripped above -- so an entry this
                 * framework cannot identify is a fault in the manifest,
                 * not a stray file to step over. Unlike a readdir entry,
                 * which stays silently ignored, it is counted.
                 *
                 * This is what makes the comment grammar safe in BOTH
                 * directions. `#` introduces a comment only after
                 * whitespace, so `test_a#b.exe` survives intact and earns
                 * a charset refusal; the cost is that `test_ok.exe#note`
                 * is now one token that fails the `.exe` shape check --
                 * and if that were merely ignored, a requested test would
                 * disappear from an otherwise authoritative manifest with
                 * no plan slot and no marker. Counting shape failures
                 * makes both spellings loud instead of trading one silent
                 * drop for another. */
                /* A name we can read, carrying an attribute we cannot:
                 * refuse the LINE rather than run it under a policy the
                 * operator did not ask for. */
                if (verdict == UTEST_NAME_ACCEPT && bad_attr)
                    verdict = UTEST_NAME_REFUSE_ATTR;
                if (verdict != UTEST_NAME_ACCEPT) {
                    /* Refusals are classified and stored FIRST, before
                     * the runnable cap is consulted, so an oversized
                     * manifest can never stop the parser short of a
                     * malformed entry it has not yet examined. */
                    if (ms->refused_count >= UTEST_MANIFEST_REFUSAL_MAX) {
                        ms->refused_overflowed = 1;
                        continue;
                    }
                    /* Provenance for the derived smoke gate. The type is
                     * trusted ONLY when the name itself was accepted and
                     * merely an attribute was unusable; a name refusal
                     * records CORRECTNESS so untrusted bytes can never
                     * reach the gate. The filter decision is recorded here
                     * too, because publication ignores the filter but the
                     * abort must not. */
                    ms->refused_smoke_selected[ms->refused_count] =
                        (uint8_t)((verdict == UTEST_NAME_REFUSE_ATTR &&
                                   entry_type == UTEST_TYPE_SMOKE &&
                                   test_usermode_glob_match(s_filter,
                                                            name_start))
                                  ? 1 : 0);
                    ms->refused_names[ms->refused_count]   = name_start;
                    ms->refused_digest[ms->refused_count]  =
                        u_name_digest(name_start, name_span);
                    ms->refused_verdict[ms->refused_count] = (uint8_t)verdict;
                    ms->refused_count++;
                    continue;
                }
                /* Runnable entry: straight into the plan, which is now the
                 * only capacity that bounds it.
                 *
                 * The old code stopped at a separate UTEST_MANIFEST_MAX
                 * runnable array and left the tail to the directory glob.
                 * That fallback provably could not reconstruct a dropped
                 * entry -- not its `type`, not its `expects_tasks`, not its
                 * position in the manifest's order, and not the entry at all
                 * when the binary is absent from the image -- so a requested
                 * test could vanish behind nothing louder than a WARN.
                 *
                 * The name stays a pointer into the arena rather than an
                 * interned copy: it is stable until u_manifest_free() at
                 * the end of the run, which outlives every plan consumer.
                 *
                 * A refused plan append sets ps->overflowed, which the
                 * caller turns into a fail-closed aggregate; parsing
                 * continues either way so the tail is still scanned for
                 * refusals. */
                if (u_plan_add_run(ps, name_start, entry_type,
                                   (uint8_t)entry_expects, 0))
                    ms->count++;
            }
        }
    }

    /* Only now is the refusal set complete, so only now can a refused
     * identity veto a runnable one that was read before it. */
    u_plan_suppress_refused(ps, ms);
}

static void u_manifest_free(struct manifest_state *ms)
{
    if (ms->arena) {
        uint32_t p;
        for (p = 0; p < UTEST_MANIFEST_ARENA_PAGES; p++)
            pmm_free_frame(ms->arena_phys + (uintptr_t)p * 4096u);
        ms->arena      = (char *)0;
        ms->arena_phys = 0;
    }
    ms->count = 0;
}

/* ---- Task entry: the loader that morphs into the test binary ---------
 *
 * Failure paths use task_exit(STATUS) -- a plain `return` from a kernel
 * task only sets TASK_DEAD but never wakes the parent's polling
 * watchdog.  task_exit is the canonical wakeup path.  Status codes:
 *   -1: NULL pending path (launcher bug)
 *   -2: vfs_open failed
 *   -3: pmm_alloc_contiguous failed
 *   -4: vfs_read short / size mismatch
 *   -5: task_exec failed
 *   TASK_EXIT_UTEST_TIMEOUT (task.h, reserved block below -SIG_MAX):
 *       assigned by the LAUNCHER, never by this loader, when the binary
 *       outran its wall clock. Was -6 until 2026-08-01, which made it
 *       indistinguishable from a SIGABRT-range death
 *   TASK_EXIT_UTEST_IDENTITY (task.h, same reserved block):
 *       the bytes read did not match the identity the plan froze, so the
 *       binary was refused instead of executed. NOT a small negative --
 *       those are indistinguishable from -(signum)
 * The launcher renders any negative exit code as `[UTEST] <name>: FAIL
 * (exit=N)` so loader failures surface even though the binary itself
 * never produced output.
 * --------------------------------------------------------------------- */

/* Staging release for the PMM shape of the ownership token: this loader stages
 * the binary in contiguous frames rather than on the kmalloc heap, so it cannot
 * share task_exec_staging_kfree. Clears the count first so a second call is a
 * no-op even independently of the token's own idempotence. */
static void utest_staging_free_frames(struct task_exec_staging *st)
{
    uint32_t n;

    if (!st || !st->pages)
        return;
    n = st->pages;
    st->pages = 0;
    /* pmm_free_contiguous is the declared symmetric counterpart of the
     * pmm_alloc_contiguous that staged these frames -- the per-frame loop this
     * file already open-codes twice does not need a third copy. */
    pmm_free_contiguous(st->phys, n);
}

/* Record that this loader invocation is exiting BEFORE ring 3, into the
 * slot of the task that IS this invocation. Release ordering pairs with the
 * launcher's acquire load after it has waited for the task, which is the
 * only reader.
 *
 * `self` is threaded in from utest_loader_func rather than re-resolved here
 * so every exit path records against the same slot the entry path read its
 * inputs from -- one resolution per invocation, and no exit can be
 * attributed to a task other than the one taking it. */
static void u_loader_stage_fault(struct task *self)
{
    if (self)
        __atomic_store_n(&self->utest_loader.stage_fault, 1u,
                         __ATOMIC_RELEASE);
}

/* Map the launcher's aggregated verdict onto the one the ARTIFACTS carry.
 * The two vocabularies differ in exactly one place: a binary that failed
 * before reaching ring 3 never ran, so it is an <error>/ERROR rather than
 * an assertion failure. PASS and SKIP pass through untouched -- neither
 * outcome can follow a loader that never reached ring 3, and reclassifying
 * one on a stale flag would turn a green binary red.
 *
 * FOUR ways to be sure about a binary, and they are not the same evidence.
 * `loader_stage_fault` is a loader that EXITED before ring 3 -- exact,
 * recorded at each exit. The other three matter only for a TIMEOUT, where
 * the loader is still alive and cannot report anything itself:
 *
 *   entered_user   PROOF the image executed: a syscall arrived from CPL 3.
 *                  A timeout after that is a hang in the binary's OWN code,
 *                  so it stays a FAIL no matter what else is set.
 *   reached_exec   The loader attempted task_exec. Never reaching it is
 *                  proof the binary never ran.
 *   frame_adopted  The scheduler took the published frame. This is what
 *                  used to be missing: a loader that reached the exec call
 *                  but whose frame was never adopted ALSO never ran, and
 *                  before this evidence existed that case was
 *                  indistinguishable from a real ring-3 hang and had to be
 *                  reported as a FAIL. `exec_pending` could not stand in
 *                  for it: zero there conflates "adopted" with "no frame
 *                  ever published". It is not swept on a timeout -- the
 *                  stuck-tick threshold only gates a WARN -- so the
 *                  distinction it cannot draw is between the two ZERO
 *                  cases, not between a stale and a fresh flag.
 *
 * Adoption is checked only AFTER entered_user has been ruled out, because
 * adoption is the weaker claim: it witnesses the frame being selected, not
 * a user instruction retiring. Every remaining ambiguity resolves toward
 * FAIL, which is still the conservative direction -- calling a real hang
 * "never ran" would hide a genuine test failure.
 *
 * A pure function, deliberately: its only alternative was an inline
 * conditional that no test could reach without driving a real loader
 * failure through live infrastructure the test policy forbids. */
static int u_record_verdict(int verdict, int loader_stage_fault,
                            int timed_out, int reached_exec,
                            int frame_adopted, int entered_user)
{
    if (verdict != 1)
        return verdict;
    if (loader_stage_fault)
        return 3;
    if (!timed_out)
        return 1;
    if (entered_user)
        return 1;
    if (!reached_exec)
        return 3;
    if (!frame_adopted)
        return 3;
    return 1;
}

static void utest_loader_func(void)
{
    /* This invocation's identity. Every input below comes out of THIS
     * slot and every verdict goes back into it, so nothing the loader
     * records can be attributed to another binary -- which is exactly what
     * the file-scope statics this replaced could not guarantee. The
     * constructor armed the inputs before the task was published, so they
     * are already in place the first time this task is scheduled in. */
    struct task *self;
    const char *path;
    struct vfs_node *file;
    uint32_t size, pages, p;
    uintptr_t buf_phys;
    uint8_t *buf;
    int n, rc;

    /* The CPU check comes BEFORE task_current(), and it FAILS CLOSED --
     * this is the third cursor-resolved evidence producer, and it is the
     * most dangerous of them.
     *
     * The syscall-entry site can decline to record one flag and carry on.
     * This one cannot: every field below -- the path it loads, the digest
     * it verifies against, and each verdict it stores -- is reached
     * through a pointer the global cursor hands it. Resolving that pointer
     * on an AP would make this loader read ANOTHER child's path and write
     * ANOTHER child's verdict, which is the whole failure this section
     * exists to make structurally impossible. So it refuses to run at all
     * rather than run misattributed, and it refuses before touching any
     * record, because the record it would mark is the one it cannot
     * trust it owns. Unreachable today (APs park in ap_entry without
     * calling schedule) and unblocked by the per-CPU cursor work in
     * the per-CPU run-queue work in 03-memory-concurrency/TODO-07. */
    if (!task_utest_report_nonbsp_dispatch(TASK_UTEST_PID_UNKNOWN,
                                           "utest loader entry")) {
        klog(LOG_ERROR, "UTEST",
             "loader: refusing to run off the BSP -- evidence would be "
             "attributed to whichever child the global cursor names");
        /* CONTAIN THIS CPU. Nothing here may re-enter the scheduler, and
         * the two obvious exits both do:
         *   - task_exit() opens with `pid = current_task`
         *     (src/kernel/sched/task.c) and marks THAT slot dead, so a
         *     refusal could kill an unrelated child and pin a failure on it.
         *   - yield() enters schedule_now(), which derives prev_task from
         *     the same cursor and writes its saved RSP, states and switch
         *     count -- so even "just parking" would mutate a slot this
         *     branch has declared it cannot identify.
         * A refusal that re-enters the untrusted cursor is not
         * fail-closed, and both of those do.
         *
         * `cli; hlt` is the repo's existing containment idiom for a CPU
         * that must stop participating (src/kernel/smp/smp.c:95). It
         * touches no TCB, dispatches nothing and kills nobody. The
         * launcher then reaps this binary through its OWN timeout path,
         * resolving the pid it holds rather than one this CPU read, and
         * the binary is classified from an all-zero record -- the
         * conservative never-ran-leaning shape. One contained CPU and one
         * stalled binary, correctly attributed, beats a fast exit charged
         * to the wrong child. */
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    self = task_current();
    /* A loader with no slot cannot record a verdict anywhere the launcher
     * would find it, so it must not proceed to execute a binary whose
     * outcome would then be unattributable. */
    if (!self) {
        klog(LOG_ERROR, "UTEST", "loader: no current task -- refusing");
        /* NO-LOADER-STAGE-MARK: u_loader_stage_fault() records the never-ran
         * stage ON a task, and this is the one exit taken because there is no
         * task to record it on. The structural check in scripts/test-tooling.sh
         * counts this waiver rather than tolerating an off-by-one, so a REAL
         * unmarked exit added later still fails it. */
        task_exit(-1);
    }
    path = self->utest_loader.test_path;

    if (!path || !path[0]) {
        klog(LOG_ERROR, "UTEST", "loader: NULL pending path");
        u_loader_stage_fault(self);
        task_exit(-1);
    }

    file = vfs_open(path, VFS_O_READ);
    if (!file) {
        klog(LOG_ERROR, "UTEST", "%s: vfs_open failed", path);
        u_loader_stage_fault(self);
        task_exit(-2);
    }

    /* Bounded BEFORE the page rounding, which is where an unbounded size
     * would overflow: `size + 4095` wraps for a size near UINT32_MAX and
     * yields a page count far too small for the read that follows. The
     * bound is the executor's own contract, so a file this loader would
     * refuse never gets read or hashed first. */
    if (file->size == 0 || file->size > (uint64_t)EXEC_MAX_IMAGE_SIZE) {
        klog(LOG_ERROR, "UTEST",
             "%s: image size %u outside the loadable range -- refusing",
             path, file->size);
        vfs_close(file);
        u_loader_stage_fault(self);
        task_exit(-2);
    }
    size  = (uint32_t)file->size;
    pages = (size + 4095u) / 4096u;
    buf_phys = pmm_alloc_contiguous(pages);
    if (!buf_phys) {
        klog(LOG_ERROR, "UTEST",
             "%s: pmm_alloc_contiguous(%u pages) failed",
             path, (uint64_t)pages);
        vfs_close(file);
        u_loader_stage_fault(self);
        task_exit(-3);
    }
    buf = (uint8_t *)buf_phys;

    n = vfs_read(file, 0, size, buf);
    vfs_close(file);
    if (n <= 0 || (uint32_t)n != size) {
        klog(LOG_ERROR, "UTEST", "%s: vfs_read short (n=%d size=%u)",
             path, (int64_t)n, (uint64_t)size);
        for (p = 0; p < pages; p++)
            pmm_free_frame(buf_phys + (uintptr_t)p * 4096u);
        u_loader_stage_fault(self);
        task_exit(-4);
    }

    /* Verify the bytes about to execute against the identity the plan
     * froze, HERE and not in the launcher.
     *
     * This is the whole reason the check reads `buf` rather than re-opening
     * the path: `buf` is the exact object task_exec consumes, so there is
     * no window between what was verified and what runs. A launcher-side
     * check before the spawn would leave precisely the gap this section
     * exists to close -- a replacement landing between that check and this
     * read would verify one file and execute another.
     *
     * The expected digest comes out of this task's own slot, armed by the
     * constructor before the task was published. That is a PLAIN store
     * read by a plain load: it carries no acquire/release edge and none is
     * claimed, because the ordering rests on BSP-only dispatch (checked at
     * loader entry, not enforced) exactly as task.c states. An earlier
     * version of this comment described a release/acquire publication that
     * the per-child refactor removed -- a stale guarantee is worse than
     * none on a security-sensitive identity check, since it invites future
     * SMP work to rely on it. */
    {
        const uint8_t *expect = self->utest_loader.expect_digest;
        uint8_t actual[SHA256_DIGEST_LEN];

        sha256(buf, size, actual);
        if (!u_identity_matches(expect, actual)) {
            klog(LOG_ERROR, "UTEST",
                 "%s: content does not match the identity the plan froze "
                 "-- refusing to execute it", path);
            __atomic_store_n(&self->utest_loader.identity_mismatch, 1u,
                             __ATOMIC_RELEASE);
            pmm_free_contiguous(buf_phys, pages);
            u_loader_stage_fault(self);
            task_exit(TASK_EXIT_UTEST_IDENTITY);
        }
    }

    /* task_exec stages an iretq frame for user mode (consumed on the
     * next scheduling switch). It DOES NOT take ownership of `buf` --
     * exec_load() inside copies the binary into user pages, so once
     * task_exec returns 0 the staging buffer is no longer needed.
     *
     * This loader runs with interrupts enabled, so the frame release must go
     * through the ownership token: on success task_exec drops these frames
     * before publication, because a tick can carry this task into the new image
     * before the call returns and the loop below would never run. */
    /* Marked BEFORE the call: task_exec re-enables interrupts once the
     * frame is published, so a tick can carry this task into ring 3 and
     * nothing after the call is guaranteed to run. */
    __atomic_store_n(&self->utest_loader.reached_exec, 1u, __ATOMIC_RELEASE);
    {
        struct task_exec_staging st = {
            .release = utest_staging_free_frames,
            .phys = buf_phys, .pages = pages
        };
        rc = task_exec(buf, size, &st);
        /* Covers BOTH failure outcomes: a pre-commit -1 (image intact) and
         * TASK_EXEC_IMAGE_DESTROYED (image gone). Either way the staging
         * frames are released here and the loader task exits below -- it must
         * never fall through to ring 3 with a destroyed image. On success this
         * is a no-op; task_exec already released. */
        task_exec_staging_release(&st);
    }
    if (rc < 0) {
        klog(LOG_ERROR, "UTEST", "%s: task_exec failed (rc=%d)",
             path, (int64_t)rc);
        /* Distinguish the two outcomes like the other three callers do. A
         * post-commit failure destroyed this task's image, and the launcher's
         * own -1..-5 codes sit inside the -(signum) range, so reporting one of
         * those here would be indistinguishable from a signal death to the same
         * oracle the exec lifecycle test relies on. */
        if (rc == TASK_EXEC_IMAGE_DESTROYED) {
            u_loader_stage_fault(self);
            task_exit(TASK_EXIT_EXEC_IMAGE_DESTROYED);   /* no return */
        }
        u_loader_stage_fault(self);
        task_exit(-5);
    }

    /* Force a cooperative reschedule so the prepared user-mode iretq
     * frame is consumed. yield() goes through schedule_now() which
     * switches regardless of the preemptive sched_enabled flag.
     * CRITICAL: do NOT replace this with `for(;;) hlt;` -- a user-mode
     * spinloop would block forever if we relied on HLT here. (
     * regression, 2026-04-20.) */
    for (;;)
        yield();
}

/* Stamp the timeout cause onto a child the launcher gave up waiting for.
 *
 * One line, and deliberately a named function anyway. The value assigned here
 * is the ONLY thing that distinguishes "outran its clock" from "was killed by
 * a signal" once the child is reaped, and it is assigned on a path a kernel
 * unit test cannot drive: u_wait_with_timeout needs live scheduling, signals,
 * a clock and remote termination, all of which the test policy forbids a
 * test_*.c from touching. Factoring the assignment out gives that path a
 * POSTCONDITION a test CAN assert against a zeroed scratch TCB -- outside the
 * -(signum) range and not SIGKILL's -9 -- so reintroducing a signal-range
 * marker fails a test rather than silently restoring the ambiguity this
 * section removed. What it deliberately does NOT prove is that
 * u_wait_with_timeout still calls it; that regression class needs the
 * end-to-end fixture parked in this section's items 1-2. */
static void u_stamp_timeout_status(struct task *t)
{
    t->exit_status = TASK_EXIT_UTEST_TIMEOUT;
}

/* The stall twin of the stamp above, and a SEPARATE function rather than a
 * parameter on it, for the same reason the two reasons are separate values: the
 * only postcondition worth asserting is that these two paths cannot converge on
 * one marker. A shared setter taking the status as an argument would let a
 * caller pass either at either site and no test could tell. */
static void u_stamp_stalled_status(struct task *t)
{
    t->exit_status = TASK_EXIT_UTEST_STALLED;
}

/* The per-binary wait's condition, as a predicate over the task slot. */
static int u_task_is_dead(void *ctx)
{
    const struct task *t = (const struct task *)ctx;

    return t->state == TASK_DEAD;
}

/* ---- Polled wait with timeout -------------------------------------- *
 *
 * Replaces task_waitpid in the path. Semantics:
 *  - Returns normally with the child's exit_status if the child
 *    reaches TASK_DEAD before the deadline.
 *  - On timeout: send SIGKILL, wait KILL_GRACE_MS for cooperative
 *    tear-down, then force state=TASK_DEAD + exit_status=TIMEOUT.
 *    The exit_status the caller sees is always TASK_EXIT_UTEST_TIMEOUT
 *    on the timeout path (overwrites SIGKILL's -9).
 *  - Caller still owns task_cleanup() for the child pid afterward.
 *
 * Needs preemptive scheduling to be enabled (see top-of-file comment
 * on scheduler_enable wrap) -- otherwise yield() gives control to a
 * spinning user task and never comes back. The launcher wraps the
 * whole run in scheduler_enable/disable so this function is safe.
 * ------------------------------------------------------------------ */

static int32_t u_wait_with_timeout(uint32_t pid, uint32_t timeout_ms,
                                   int *out_timed_out, int *out_stalled)
{
    struct task *t = task_get_by_pid(pid);
    int ended;

    *out_timed_out = 0;
    *out_stalled   = 0;

    if (!t)
        return -1;

    if (timeout_ms == 0)
        timeout_ms = UTEST_DEFAULT_TIMEOUT_MS;

    ended = u_bounded_wait(u_task_is_dead, t, timeout_ms, &u_wait_ops_live);
    /* A STALL TAKES THE TIMEOUT'S SAFETY ACTIONS AND KEEPS ITS OWN NAME, and
     * splitting the two is what makes the outcome reportable at all.
     *
     * The tempting shapes are both wrong. Reporting a stall as an ordinary
     * timeout gets the safety right and destroys the distinction the wait was
     * extended to make -- the caller then blames a binary that was never slow.
     * Reporting it as neither keeps the distinction and drops the safety: the
     * launcher would walk on to snapshot this child's TCB and free its slot
     * without ever having established that it is dead, which is the one thing
     * the kill path exists to establish. So `timed_out` continues to mean
     * "the launcher stopped waiting and must now prove the child dead", and
     * `stalled` carries WHY on top of it. */
    if (ended == UTEST_WAIT_STALLED) {
        *out_stalled   = 1;
        *out_timed_out = 1;
        u_wait_report_stall("binary-wait", (uint64_t)pid);
    } else if (ended == UTEST_WAIT_TIMEOUT) {
        *out_timed_out = 1;
    }

    if (*out_timed_out) {
        /* Cooperative kill first -- gives the task a chance to run
         * its signal_check on the next kernel entry, unwind cleanly,
         * and set its own exit_status. */
        signal_send(pid, SIGKILL);
        /* The grace is the SECOND wait on this path and needs the escape as
         * much as the first: reached after a stall it would be waiting on the
         * same stopped clock, so a wait that escaped its own hang would hang
         * here instead.
         *
         * ITS OUTCOME IS NOT DISCARDABLE, and the tempting reasoning for
         * discarding it -- "the run is already marked, the first wait named the
         * site" -- is only true on the branch where the FIRST wait stalled. The
         * other branch is the dangerous one: the binary times out normally on a
         * live clock, the clock stops during the grace, and a discarded result
         * would leave out_stalled false, no record on the wire, the child
         * stamped as an ordinary timeout, and the suite carrying on measuring
         * later binaries against a clock that had already stopped. So the
         * grace reports its OWN site and promotes the outcome. */
        if (u_bounded_wait(u_task_is_dead, t, UTEST_KILL_GRACE_MS,
                           &u_wait_ops_live) == UTEST_WAIT_STALLED &&
            !*out_stalled) {
            *out_stalled = 1;
            u_wait_report_stall("binary-kill-grace", (uint64_t)pid);
        }
        /* Forceful fallback: a user-mode spinloop with no syscall
         * never runs signal_check, so SIGKILL alone cannot land. We
         * have to mark the task DEAD ourselves.
         *
         * Safe in this kernel because task dispatch uses a single
         * global `current_task` ([src/kernel/sched/task.c]) -- APs do
         * not run scheduled tasks, so no other CPU can be dispatching
         * the child while the launcher (currently executing) decides
         * to force-kill. The existing signal_send() writes t->state
         * unlocked under the same assumption (ipc/signal.c:41).
         *
         * Route through task_terminate_remote() so the child gets the
         * SAME shared DEAD-transition teardown as every other death path
         * (OB process object mark-dead, syscall-filter count, Job Object
         * detach, timer-resolution reap). A direct `t->state = TASK_DEAD`
         * would leak Job membership + strand a timer-resolution request
         * into the next test suite.
         *
         * Defensive guard: refuse to force-kill ourselves; would
         * leave the running task DEAD and trip a cascading crash. */
        if (t->state != TASK_DEAD && t != task_current())
            task_terminate_remote(t, *out_stalled ? TASK_EXIT_UTEST_STALLED
                                                  : TASK_EXIT_UTEST_TIMEOUT);
        /* Stamped UNCONDITIONALLY, on both branches, and that is load-bearing
         * rather than belt-and-braces. On the cooperative branch the child
         * already died from SIGKILL and signal_default_action left -9 behind;
         * task_terminate_remote is SKIPPED there precisely because the child
         * is already DEAD, so this call is the ONLY thing that converts that
         * -9 into a named timeout. On the force-kill branch it merely rewrites
         * the value terminate_remote was already handed, which is why the call
         * reads as redundant if only that branch is considered.
         *
         * It is also a store AFTER task_terminate_remote published
         * state = TASK_DEAD and ran the death teardown. Benign today: no
         * DEAD-transition consumer latches exit_status (neither
         * task_death_teardown nor ob_process_mark_dead reads it), and the
         * launcher -- the same task that stores here -- is its only reader,
         * immediately below with no intervening yield. It stops being benign
         * the day a teardown path starts recording an exit status, so anyone
         * adding such a consumer needs to reorder this first. */
        if (*out_stalled)
            u_stamp_stalled_status(t);
        else
            u_stamp_timeout_status(t);
    }

    return t->exit_status;
}

/* ---- CI-friendly output format helpers ------------------------- *
 *
 * XML attribute escaping covers the five spec-required characters
 * (`&`, `<`, `>`, `"`, `'`) plus control bytes (< 0x20, except tab/LF
 * which XML 1.0 allows in attributes). JSON string escaping covers
 * `"`, `\`, and control bytes (< 0x20) per RFC 8259. Both writers use
 * a bounded output buffer with explicit length check on every append.
 *
 * Names today come from u_is_valid_manifest_name (no special chars),
 * so escape never triggers on the happy path; reason strings are
 * launcher-formatted (`exit=N`, `timeout`, `N handle(s) leaked`) and
 * equally safe. The escape is defense-in-depth for future consumers.
 * --------------------------------------------------------------------- */

/* Append `src` to `dst[*pos]`, bounded by `cap`. Returns 1 on
 * success, 0 if `src` would overflow. Leaves dst NUL-terminated. */
static int u_append(char *dst, uint32_t *pos, uint32_t cap, const char *src)
{
    uint32_t p = *pos;
    while (*src) {
        if (p + 1 >= cap) return 0;
        dst[p++] = *src++;
    }
    dst[p] = '\0';
    *pos = p;
    return 1;
}

/* Append a hex escape like `&#x1F;` (XML) or `\u001f` (JSON) for a
 * control byte c (< 0x20). */
static int u_append_hex2(char *dst, uint32_t *pos, uint32_t cap,
                         const char *prefix, const char *suffix, uint8_t c)
{
    static const char hex[] = "0123456789abcdef";
    char buf[8];
    uint32_t i = 0;
    while (prefix[i]) { buf[i] = prefix[i]; i++; }
    buf[i++] = hex[(c >> 4) & 0xF];
    buf[i++] = hex[c & 0xF];
    /* JSON wants 4-digit \u escape; we always pass "\u00" as prefix +
     * two hex digits, matching the JSON spec. For XML the prefix is
     * "&#x" and suffix is ";" -- we emit the suffix via the caller. */
    buf[i] = '\0';
    if (!u_append(dst, pos, cap, buf)) return 0;
    return u_append(dst, pos, cap, suffix);
}

/* Escape `src` into XML attribute-value text. Writes into
 * `dst[*pos..cap]`. Returns 1 on success, 0 on overflow. */
static int u_xml_escape(char *dst, uint32_t *pos, uint32_t cap, const char *src)
{
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        const char *rep = (const char *)0;
        switch (c) {
        case '&':  rep = "&amp;"; break;
        case '<':  rep = "&lt;"; break;
        case '>':  rep = "&gt;"; break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default: break;
        }
        if (rep) {
            if (!u_append(dst, pos, cap, rep)) return 0;
            continue;
        }
        /* Control bytes other than tab/LF/CR are illegal in XML 1.0
         * even as entity references; skip them silently. (Our inputs
         * never contain them today.) */
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
            continue;
        if (*pos + 1 >= cap) return 0;
        dst[(*pos)++] = (char)c;
        dst[*pos] = '\0';
    }
    return 1;
}

/* Escape `src` into a JSON string-body (the bytes BETWEEN the two
 * double-quotes). Writes into `dst[*pos..cap]`. */
static int u_json_escape(char *dst, uint32_t *pos, uint32_t cap, const char *src)
{
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        switch (c) {
        case '\"':
            if (!u_append(dst, pos, cap, "\\\"")) return 0;
            continue;
        case '\\':
            if (!u_append(dst, pos, cap, "\\\\")) return 0;
            continue;
        case '\n':
            if (!u_append(dst, pos, cap, "\\n")) return 0;
            continue;
        case '\r':
            if (!u_append(dst, pos, cap, "\\r")) return 0;
            continue;
        case '\t':
            if (!u_append(dst, pos, cap, "\\t")) return 0;
            continue;
        default: break;
        }
        if (c < 0x20) {
            if (!u_append_hex2(dst, pos, cap, "\\u00", "", c)) return 0;
            continue;
        }
        if (*pos + 1 >= cap) return 0;
        dst[(*pos)++] = (char)c;
        dst[*pos] = '\0';
    }
    return 1;
}

/* Append a decimal unsigned integer. */
static int u_append_uint(char *dst, uint32_t *pos, uint32_t cap, uint64_t v)
{
    char buf[24];
    uint32_t i = 0;
    if (v == 0) return u_append(dst, pos, cap, "0");
    while (v) {
        if (i >= sizeof(buf)) return 0;
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) {
        if (*pos + 1 >= cap) return 0;
        dst[(*pos)++] = buf[i];
    }
    dst[*pos] = '\0';
    return 1;
}

/* Render milliseconds as `S.MMM` (seconds with 3-digit millisecond
 * fraction) for the XML `time` attribute. */
static void u_format_seconds(char *dst, uint32_t cap, uint64_t ms)
{
    uint32_t pos = 0;
    uint64_t secs = ms / 1000ull;
    uint64_t frac = ms % 1000ull;
    u_append_uint(dst, &pos, cap, secs);
    if (pos + 4 < cap) {
        dst[pos++] = '.';
        dst[pos++] = (char)('0' + (frac / 100) % 10);
        dst[pos++] = (char)('0' + (frac / 10) % 10);
        dst[pos++] = (char)('0' + frac % 10);
        dst[pos] = '\0';
    }
}

/* ---- ring-3 self-report (SYS_TEST_REPORT) ----------------------- *
 *
 * A binary's exit code carries two outcomes; honest reporting needs
 * three. The harness submits its counters through syscall 48 and the
 * kernel parks them on the child's TCB (see TASK_UTEST_REPORT_* in
 * sched/task.h); the launcher lifts them here, BEFORE task_cleanup
 * destroys the TCB, exactly like the handle-leak snapshot above.
 *
 * Two dimensions, never summed: `asserts_*` count UTEST_ASSERT calls,
 * `skip_blocks` counts UTEST_SKIP sites taken, and one skip site
 * usually stands in for several assertions that never ran.
 * ------------------------------------------------------------------ */

/* Run-wide ceiling on synthetic skip records. The per-binary bound the
 * syscall enforces (TASK_UTEST_REPORT_SKIP_MAX) is multiplicative across
 * TASK_MAX binaries, so it cannot be the only stop; this is the aggregate
 * one.
 *
 * It is DERIVED from what a burst costs the transport, not picked. The
 * original 1024 was chosen while klog's per-subsystem rate limiter was the
 * real ceiling; the artifact records moved to klog_unrated (see above), so
 * this budget became the only bound on the burst and a picked number stopped
 * being defensible.
 *
 * The derivation is deliberately transport-INDEPENDENT -- a serialized-BYTE
 * allowance, never a time. Pinning it to a baud rate would be wrong on a
 * supported configuration: serial init accepts 9600/19200/38400/57600/115200,
 * preserves an unknown firmware-configured divisor when SPCR reports baud
 * code 0, and falls back to 38400 on an unrecognized rate
 * (src/kernel/drivers/serial.c:57-69), so a 115200-derived constant is off by
 * up to 12x. Completion TIME is the host deadline's job; the kernel's job is
 * to bound how many bytes it may add. */

/* Physical records per logical skip, counted on the WORST path rather than
 * the happy one. Three emitters fire per skip -- a TAP point, a JUnit
 * <testcase> and a JSON record (u_emit_skip_records below) -- and each of
 * them emits TWO records when its buffer overflows: an
 * [UTEST-RECORD-OVERFLOW] marker plus a verdict-preserving fallback record
 * (u_emit_tap_point, u_emit_xml_testcase and u_emit_json_testcase all take
 * that shape at their `overflow:` labels). A binary name long enough to
 * overflow every formatter therefore costs six framed lines per skip, not
 * three, and deriving from three would have understated the worst-case burst
 * by exactly 2x -- which is how a "derived" ceiling silently becomes as
 * wrong as a picked one. */
#define UTEST_SKIP_RECORD_FANOUT 6u

/* Worst-case SERIALIZED cost of one record: the message cap plus every
 * wrapper byte klog puts on the wire. Counting only UTEST_RECORD_LINE_MAX
 * would under-count the real burst by roughly a third, which is the error
 * that makes a "derived" number no better than a picked one. Components are
 * read off klog's serial emit path (src/kernel/klog.c:1240-1341). */
/* "[NNNNNNNN.mmm] " -- klog's serial renderer buffers the seconds field's
 * decimal digits in a local `char tmp[8]` (klog.c, same function) with no
 * bound on the digit COUNT it writes there beyond `sizeof(tmp)`; `sec` is a
 * uint32_t, so digit counts beyond 8 (sec >= 100,000,000, ~3.17 years of
 * continuous uptime) write past `tmp` -- a latent, practically-unreachable
 * stack overflow tracked as its own item (02-kernel-core/TODO-04-system-
 * logging.md item: "Bound the timestamp digit loop in klog.c's serial
 * renderer so it cannot write past its fixed stack buffer regardless of
 * uptime seconds"), out of THIS section's scope. What this macro must do is
 * bound the WIRE cost within klog's actual defined behavior, which caps at
 * 8 digits: '[' + 8 digits + '.' + 3 fractional digits + ']' + ' ' = 15. A
 * post-ship review (2026-07-29) found the prior value (12, assuming <= 5
 * digits) undercounts every timestamp past 99,999 seconds (~27.8h uptime),
 * which a long boot-test session can reach. */
#define KLOG_WIRE_TIMESTAMP_MAX 15u
#define KLOG_WIRE_CPUTAG_MAX     9u  /* "[cpu:NN] " on SMP                 */
#define KLOG_WIRE_LEVEL_ANSI_MAX 7u  /* longest level_ansi[] entry         */
#define KLOG_WIRE_LEVEL_PREFIX   7u  /* "[CRIT] " -- every badge is 7      */
#define KLOG_WIRE_ANSI_RESET     4u  /* the reset sequence                 */
#define KLOG_WIRE_TRUNC_MARK     1u  /* '~' appended on truncation         */
#define KLOG_WIRE_CRLF           2u  /* newline reaches the wire as CR LF  */
/* Subsystem tag plus its ": " separator. The tag is the framed
 * "UTEST-<8hex>" (UTEST_FRAME_TAG_MAX), bounded by klog's own field. */
#define KLOG_WIRE_SUBSYSTEM_MAX (KLOG_SUBSYSTEM_MAX + 2u)

/* Every record this macro bounds is one of the two overflow markers
 * u_emit_skip_records() logs -- [UTEST-SKIP-RECORD-BUDGET] and
 * [UTEST-RECORD-OVERFLOW], both `utest_record_log(LOG_ERROR, ...)` -- and
 * both fire AFTER task_cleanup() clears the launcher's color-scope flag
 * (s_utest_color_active = 0 at test_usermode.c:2895, well before
 * u_emit_skip_records() runs at test_usermode.c:3062). The explicit
 * "UTEST-<8hex>" subsystem match in klog's renderer also does not fire for
 * this tag: `klog_tag_is()` requires the character after "UTEST" to be '\0' or
 * ':', and here it is '-'. So klog's renderer never
 * takes the TEST-color branch for these specific records -- it always takes
 * the plain LOG_ERROR path, where `level_full_line[LOG_ERROR]` is 1
 * (klog.c:276), so the badge's level-ANSI sequence is re-emitted after the
 * message. That path costs: the badge (1x LEVEL_ANSI + LEVEL_PREFIX +
 * reset), a second LEVEL_ANSI re-emission, the message's own reset, and the
 * reset klog appends to EVERY record unconditionally (klog.c:1332-1336) --
 * 2x LEVEL_ANSI, 3x RESET, and no truecolor sequence at all. Modeling the
 * TEST-color branch instead (as an earlier version of this macro did) would
 * both add a sequence these records never carry AND miss the third reset
 * they always do -- the wrong branch, not merely an imprecise one. */
#define UTEST_RECORD_WIRE_MAX                                            \
    (UTEST_RECORD_LINE_MAX + KLOG_WIRE_TIMESTAMP_MAX +                   \
     KLOG_WIRE_CPUTAG_MAX + (2u * KLOG_WIRE_LEVEL_ANSI_MAX) +            \
     KLOG_WIRE_LEVEL_PREFIX + (3u * KLOG_WIRE_ANSI_RESET) +              \
     KLOG_WIRE_SUBSYSTEM_MAX + KLOG_WIRE_TRUNC_MARK + KLOG_WIRE_CRLF)

/* The allowance itself: how many bytes a pathological skip burst may add to
 * the capture. A mebibyte is a rounding error against the multi-MB serial
 * logs this framework already produces, and it is a bound a reader can check
 * against a file size -- unlike a record count, which means nothing without
 * knowing the per-record cost.
 *
 * It is 1 MiB rather than 512 KiB because the floor below is what actually
 * constrains this: with the honest 6x overflow fanout, a 512 KiB allowance
 * divides out to 252 records even BEFORE the refusal reservation below is
 * subtracted -- under the 256 a single binary may legitimately report.
 * Rather than accept a budget that clips an honest binary, the allowance
 * doubles. The _Static_assert is what forced the choice into the open
 * instead of leaving it to arithmetic nobody re-derives. */
#define UTEST_SKIP_BURST_WIRE_MAX (1024u * 1024u)

/* Refusing costs wire too. u_emit_skip_records() emits a
 * [UTEST-SKIP-RECORD-BUDGET] marker on every request it clips, and a request
 * is per BINARY, so a run can pay one marker per task slot. Budgeting only
 * the permitted records left the diagnostics OUTSIDE the allowance the
 * derivation advertises: at the boundary the logical records already consume
 * 1,048,380 of 1,048,576 bytes, so a single marker overruns it. Reserving
 * the refusal traffic FIRST keeps the 1 MiB figure a real bound instead of
 * one that holds only until the budget is actually enforced.
 *
 * TASK_MAX bounds "binaries run" only because task slots are NOT reusable
 * today (test_usermode.c:3288 -- "task_create-failed until scheduler slot
 * reuse ships"); a run cannot dispatch more than TASK_MAX binaries because
 * num_tasks++ never resets. Once the scheduler's reusable-task-slot
 * free-list work ships (tracked in the scheduler enhancement TODO, item
 * "Add reusable slot/free-list logic for dead tasks"), a single run could
 * dispatch more binaries than TASK_MAX and this reservation would silently
 * undercount -- that item is this reservation's consumer too, not just the
 * launcher's stress-loop XREF already on file. */
#define UTEST_SKIP_REFUSAL_WIRE_MAX (TASK_MAX * UTEST_RECORD_WIRE_MAX)

#define UTEST_SKIP_RECORD_BUDGET                                         \
    ((UTEST_SKIP_BURST_WIRE_MAX - UTEST_SKIP_REFUSAL_WIRE_MAX) /         \
     (UTEST_SKIP_RECORD_FANOUT * UTEST_RECORD_WIRE_MAX))

/* The two asserts that pin this derivation live just below
 * UTEST_RECORD_LINE_MAX, which the wire cost is expressed in terms of: a
 * macro body is only expanded where it is USED, so asserting here would
 * evaluate UTEST_RECORD_WIRE_MAX before that cap exists. */

/* Transport cap for EVERY machine-artifact record.
 *
 * Records reach the host through klog, whose ring entry is `message[256]`
 * and which bounds the formatted message to that size. A record formatted
 * into a larger local buffer still gets cut there, silently -- and a cut
 * JSON object is unparseable rather than merely short, while a cut XML
 * element corrupts the assembled document. Formatting against this cap
 * instead means the bounded-append path refuses and the emitter publishes
 * an explicit overflow marker the host fails on, which is a diagnosis
 * rather than a corruption.
 *
 * It bites on real inputs, not theoretical ones: a binary name may be
 * VFS_MAX_NAME (256) bytes, so the JSON prefix plus a long name already
 * exceeds the wire capacity on its own. */
#define UTEST_RECORD_LINE_MAX 256
_Static_assert(UTEST_RECORD_LINE_MAX <= sizeof(((klog_entry_t *)0)->message),
               "UTEST record buffer must fit klog's message field or the "
               "wire copy is silently truncated");

/* --- pins for the derived skip-record budget above ------------------- *
 *
 * This ceiling bounds BYTES, not TIME, and the distinction is deliberate.
 * A ceiling-sized burst is roughly a megabyte of serial traffic, which no
 * supported UART carries inside the host's default 60-second deadline --
 * about 90 seconds at 115200 8N1, far longer at 9600. That is not a
 * contradiction to resolve by shrinking the number: the kernel cannot know
 * the wire rate (the divisor may be firmware-configured and never reported),
 * and a run that actually reaches this ceiling is PATHOLOGICAL by
 * construction. It is the abuse the budget exists to bound, not a run that
 * must be helped to finish; the host deadline failing it is the correct
 * outcome and the reason the deadline exists. Real suites report a handful of
 * skip blocks. The budget stops an unbounded burst from being EMITTED; TIME
 * stays the host's to enforce.
 *
 * The floor is load-bearing, not a sanity check on arithmetic. The syscall
 * side lets ONE binary report TASK_UTEST_REPORT_SKIP_MAX skip blocks, so a
 * run-wide budget below that would clip a single HONEST binary -- turning an
 * aggregate abuse stop into a per-binary truncation, which is the false-green
 * class this path exists to close. Any future retune of the byte allowance or
 * of the wire costs that would breach this fails the BUILD instead of
 * silently shrinking what a legitimate binary may report. */
_Static_assert(UTEST_SKIP_RECORD_BUDGET >= TASK_UTEST_REPORT_SKIP_MAX,
               "run-wide skip budget must cover the per-binary ceiling or a "
               "single legitimate binary's skip blocks get clipped");

/* UTEST_SKIP_RECORD_BUDGET's numerator is an UNSIGNED subtraction
 * (BURST_WIRE_MAX - REFUSAL_WIRE_MAX). If a future retune ever made the
 * reservation exceed the allowance, that subtraction would wrap to
 * approximately UINT32_MAX rather than go negative, and the floor assert
 * above would still pass on the resulting (wrongly huge) budget -- the
 * worst-case-fits assert below DOES catch it, but points at the wrong
 * relationship for whoever has to diagnose the failure. Asserting the
 * subtraction's precondition directly, by name, is what makes the eventual
 * build failure diagnose itself instead of requiring this comment to be
 * re-derived. */
_Static_assert(UTEST_SKIP_REFUSAL_WIRE_MAX < UTEST_SKIP_BURST_WIRE_MAX,
               "the refusal-marker reservation must not consume the whole "
               "serialized-byte allowance or the budget's unsigned "
               "subtraction wraps instead of going negative");

/* The whole point of the derivation: the worst-case burst must actually fit
 * the allowance it was divided out of -- INCLUDING the refusal markers the
 * budget's own enforcement emits, which is the boundary the first version of
 * this assert missed. Integer division guarantees the arithmetic, so the
 * assert exists to catch a future edit that replaces the division with a
 * hand-written number and quietly breaks the relationship. */
_Static_assert((uint64_t)UTEST_SKIP_RECORD_BUDGET *
                       UTEST_SKIP_RECORD_FANOUT * UTEST_RECORD_WIRE_MAX +
                   UTEST_SKIP_REFUSAL_WIRE_MAX <=
                   UTEST_SKIP_BURST_WIRE_MAX,
               "the worst-case skip burst plus its refusal markers must fit "
               "the serialized-byte allowance the budget is derived from");

/* ---- The binary-name bound, derived per formatter ------------------- *
 *
 * A binary name was bounded only by VFS_MAX_NAME (256) while every record
 * is bounded by UTEST_RECORD_LINE_MAX (256), so a long enough name pushed
 * each formatter onto its own overflow fallback: the XML testcase drops
 * the name, the JSON record substitutes "overflow", the TAP point becomes
 * "unrepresentable", and the verdict line loses its `: PASS` / `: FAIL`
 * token off the end -- which is the token the host's recount greps for, so
 * the binary silently left the fail-closed count entirely.
 *
 * The bound below is DERIVED rather than picked: each record kind's fixed
 * cost is summed from that kind's OWN format literals through UTEST_LIT,
 * so editing a format string moves the bound with it instead of leaving a
 * hand-counted number behind. The name's MULTIPLICITY per record matters
 * as much as the fixed cost -- the JSON skip_block carries it twice (as
 * `rec_name` and again as `parent`), so it costs 2N there -- and the
 * answer is the MINIMUM across every kind, not the skip_block's. Checking
 * that assumption is what this block is for: the JSON skip_block and the
 * JSON binary record both leave exactly 37 bytes today -- they are TIED,
 * not merely close. They do NOT move together, though, and the asymmetry
 * is the multiplicity: room divides by it, so one byte added to the JSON
 * binary record's format (mult 1, 255-218=37) drops it straight to 36 and
 * makes it bind, while one byte added to the skip_block's (mult 2,
 * (255-180)/2=37) is absorbed by the integer division -- (255-181)/2 is
 * still 37 -- and it takes two. So a single edit CAN move the binding
 * kind, in one direction only, which is far too subtle to settle by
 * inspection; that is why the unit test asserts the bound equals the
 * minimum over every kind rather than any particular kind's room.
 * (This comment said "37 and 39, two bytes apart" until 2026-07-31; the
 * figure was stale, and it was the stated justification for taking the
 * MIN, so a reader re-deriving the bound from it picked the wrong binding
 * kind. The first correction then overshot by calling the two symmetric.)
 *
 * Scope: the kinds below are the ones a host consumer PARSES -- the
 * verdict line, the XML testcase, both JSON record kinds, the TAP point
 * and the report line. The `%s: format=%s` diagnostic is deliberately not
 * in the minimum: no consumer parses it, its tail truncating loses no
 * machine-read field, and its second operand is a kernel-owned loader
 * constant rather than part of the record contract -- pulling exec.c's
 * label width into this derivation would couple the bound to an unrelated
 * subsystem for a line that carries no accounting. */

/* Length of a string literal, NUL excluded. */
#define UTEST_LIT(s) ((uint32_t)(sizeof(s) - 1u))
#define UTEST_MAX2(a, b) ((a) > (b) ? (a) : (b))
#define UTEST_MIN2(a, b) ((a) < (b) ? (a) : (b))

/* Decimal widths. Each is pinned to the cap the VALUE carries, not to the
 * width of its C type, and each cap is asserted so a future retune that
 * outgrows its digit count fails the build instead of silently overrunning
 * a record whose bound was derived from it. */
#define UTEST_DIGITS_U32     10u   /* 4294967295                        */
#define UTEST_DIGITS_REPORT   7u   /* TASK_UTEST_REPORT_MAX = 1,000,000 */
_Static_assert(TASK_UTEST_REPORT_MAX <= 9999999u,
               "report counters must stay within the digit width the "
               "record bound reserves for them");
_Static_assert(TASK_UTEST_REPORT_SKIP_MAX <= 9999999u,
               "skip-block counts must stay within the reserved digit width");
_Static_assert(UTEST_SKIP_RECORD_BUDGET <= 9999999u,
               "the skip-record budget bounds the skip_index field's digits");

/* Elapsed milliseconds are the one record field with no natural cap:
 * uptime is a uint64 and `end_ms - start_ms` inherits that, so its digit
 * count could not be proven -- only assumed. It is CLAMPED at emit
 * (u_clamp_time_ms) so the reservation below is a fact about the code
 * rather than a bet on how long a run takes. */
#define UTEST_TIME_MS_MAX    0xFFFFFFFFull
#define UTEST_DIGITS_TIME    UTEST_DIGITS_U32
/* u_format_seconds renders `<secs>.<3 digits>`; secs = ms/1000, so it can
 * never be wider than the clamped millisecond field itself. */
#define UTEST_SECONDS_MAX    (UTEST_DIGITS_TIME + UTEST_LIT(".000"))

/* The reason string, derived from the launcher's own reason literals --
 * every site that writes `reason` is enumerated here, so the widest one is
 * a fact rather than an estimate. */
#define UTEST_REASON_BUF     96u
/* The five COMPOSED reasons are built at run time rather than named by one
 * literal, so each one's fragments are macros here and the ONLY code that
 * concatenates them is the matching `u_reason_<name>` helper below. Before
 * that, every fragment existed twice -- once inside the size macro, once as
 * a bare literal at its `u_append(reason, ...)` call site -- with nothing
 * tying the copies together: widening the call-site copy left
 * UTEST_REASON_MAX understating the real width, the derived
 * UTEST_MAX_BINARY_NAME too generous, and bound-length names falling into a
 * truncation fallback the build otherwise proves unreachable. Set membership
 * alone would not close it either -- a helper appending two fragments would
 * satisfy every membership rule and still overrun -- so
 * `scripts/utest-reason-lint.py` binds each helper's ORDERED append sequence
 * to its size macro's ordered terms, which is the property the derivation
 * actually rests on. */
#define UTEST_RSNC_TIMEOUT_PRE  "timeout after "
#define UTEST_RSNC_TIMEOUT_POST "ms"
#define UTEST_RSNC_EXIT         "exit="
/* Conditional at run time, UNCONDITIONAL in the size term: the bound must
 * hold for the widest composition, which is the negative one. */
#define UTEST_RSNC_NEG          "-"
#define UTEST_RSNC_LEAK         " handle(s) leaked"
#define UTEST_RSNC_ISOLATE      "isolation failed"
#define UTEST_RSNC_INVALID      "invalid test report"
/* The launcher stopped waiting because the monotonic clock stopped, so the one
 * number a timeout reason carries -- the deadline it passed -- is exactly the
 * thing this reason must NOT claim. It names the fault and nothing else. */
#define UTEST_RSNC_STALL        "monotonic clock stalled"
#define UTEST_REASON_TIMEOUT (UTEST_LIT(UTEST_RSNC_TIMEOUT_PRE)             \
                              + UTEST_DIGITS_U32                            \
                              + UTEST_LIT(UTEST_RSNC_TIMEOUT_POST))
#define UTEST_REASON_STALL   UTEST_LIT(UTEST_RSNC_STALL)
#define UTEST_REASON_EXIT    (UTEST_LIT(UTEST_RSNC_EXIT)                    \
                              + UTEST_LIT(UTEST_RSNC_NEG)                   \
                              + UTEST_DIGITS_U32)
#define UTEST_REASON_LEAK    (UTEST_DIGITS_U32 + UTEST_LIT(UTEST_RSNC_LEAK))
#define UTEST_REASON_ISOLATE UTEST_LIT(UTEST_RSNC_ISOLATE)
#define UTEST_REASON_INVALID UTEST_LIT(UTEST_RSNC_INVALID)
/* Every refusal reason is NAMED here and used at its call site through
 * these macros, so the set below is the complete set by construction.
 * Enumerating one of them as a proxy (as the first version did) left the
 * others free to grow past the reservation without moving this maximum or
 * tripping its assert -- a compile-time guarantee that quietly did not
 * hold for most of the strings it claimed to cover. */
#define UTEST_RSN_LENGTH     "name refused: length"
#define UTEST_RSN_CHARSET    "name refused: charset"
#define UTEST_RSN_PATH       "name refused: path"
#define UTEST_RSN_NUL        "name refused: nul"
#define UTEST_RSN_GENERIC    "name refused"
#define UTEST_RSN_SHAPE      "manifest entry not test_*.exe"
#define UTEST_RSN_ATTR       "manifest attribute unusable"
#define UTEST_RSN_ARRAY_FULL "refusal array full"
#define UTEST_RSN_UNREADABLE "manifest unparseable"
/* Enumeration-plan reasons. Kept SHORT deliberately: UTEST_REASON_REFUSAL
 * feeds UTEST_REASON_MAX, which every record's fixed shape subtracts from
 * UTEST_RECORD_LINE_MAX to derive UTEST_MAX_BINARY_NAME -- so a verbose
 * reason string here silently narrows the name budget every record kind
 * gets. All four fit inside the pre-existing maximum (UTEST_RSN_SHAPE at
 * 29 bytes), so the derived name bound is unchanged by this section. */
#define UTEST_RSN_PLAN_FULL  "plan full"
#define UTEST_RSN_PLAN_ALLOC "plan alloc failed"
#define UTEST_RSN_DUP_POLICY "manifest duplicate conflict"
#define UTEST_RSN_ABSENT     "planned binary absent"
/* The planned name still resolves, but not to the bytes the plan froze.
 * 23 bytes, inside the pre-existing UTEST_RSN_SHAPE maximum (29), so the
 * derived name budget every record kind gets is unchanged by this section
 * -- enumerated in the tree below regardless, because a reason that is not
 * enumerated is free to grow past the reservation without tripping the
 * assert, which is the exact failure the enumeration exists to prevent. */
#define UTEST_RSN_MISMATCH   "planned binary replaced"
/* The plan could not establish an identity for this entry at all -- the file
 * did not resolve, its size was outside what the executor will load, a read
 * came up short, it changed underneath the walk, or the launcher had no
 * scratch buffer. Deliberately NOT the same string as a replacement: no
 * comparison happened, so reporting these as "replaced" would put a
 * tampering claim in the machine artifacts for what is usually a missing
 * file or an allocation failure. 22 bytes, inside the same pre-existing
 * UTEST_RSN_SHAPE maximum. */
#define UTEST_RSN_UNVERIFIED "identity freeze failed"
/* The launcher asked for a task and the kernel could not create one, so the
 * binary never ran -- the same class as a refused name, and now reported
 * through the same publisher. Named here rather than written inline at the
 * call site for the reason every other reason is: an unenumerated literal is
 * free to grow past the reservation without moving UTEST_REASON_MAX. 18
 * bytes, inside the pre-existing UTEST_RSN_SHAPE maximum (29), so the derived
 * name bound is unchanged. */
#define UTEST_RSN_TASK_CREATE "task_create failed"
#define UTEST_REASON_REFUSAL                                               \
    UTEST_MAX2(UTEST_MAX2(UTEST_MAX2(UTEST_LIT(UTEST_RSN_LENGTH),          \
                                     UTEST_MAX2(UTEST_LIT(UTEST_RSN_CHARSET), \
                                                UTEST_LIT(UTEST_RSN_ATTR))), \
                          UTEST_MAX2(UTEST_MAX2(UTEST_LIT(UTEST_RSN_PATH), \
                                                UTEST_LIT(UTEST_RSN_ABSENT)), \
                                     UTEST_MAX2(UTEST_LIT(UTEST_RSN_NUL),  \
                                                UTEST_LIT(UTEST_RSN_PLAN_FULL)))), \
               UTEST_MAX2(UTEST_MAX2(UTEST_LIT(UTEST_RSN_GENERIC),         \
                                     UTEST_MAX2(UTEST_LIT(UTEST_RSN_SHAPE), \
                                                UTEST_LIT(UTEST_RSN_DUP_POLICY))), \
                          UTEST_MAX2(UTEST_MAX2(UTEST_LIT(UTEST_RSN_ARRAY_FULL), \
                                                UTEST_LIT(UTEST_RSN_PLAN_ALLOC)), \
                                     UTEST_MAX2(UTEST_MAX2(UTEST_LIT(UTEST_RSN_UNREADABLE), \
                                                           UTEST_LIT(UTEST_RSN_TASK_CREATE)), \
                                                UTEST_MAX2(UTEST_LIT(UTEST_RSN_MISMATCH), \
                                                           UTEST_LIT(UTEST_RSN_UNVERIFIED))))))
#define UTEST_REASON_MAX                                                   \
    UTEST_MAX2(UTEST_MAX2(UTEST_MAX2(UTEST_REASON_TIMEOUT,                 \
                                     UTEST_REASON_EXIT),                   \
                          UTEST_MAX2(UTEST_REASON_LEAK,                    \
                                     UTEST_REASON_ISOLATE)),               \
               UTEST_MAX2(UTEST_MAX2(UTEST_REASON_INVALID,                 \
                                     UTEST_REASON_STALL),                  \
                          UTEST_REASON_REFUSAL))
_Static_assert(UTEST_REASON_MAX < UTEST_REASON_BUF,
               "the widest reason the launcher composes must fit the buffer "
               "u_run_one writes it into");

/* The five composed reasons, each built in exactly ONE place.
 *
 * The helper name is the binding: `u_reason_timeout` composes what
 * UTEST_REASON_TIMEOUT measures, and scripts/utest-reason-lint.py checks that
 * its ordered append sequence matches that macro's ordered terms
 * (`u_append(<FRAG>)` against `UTEST_LIT(<FRAG>)`, `u_append_uint` against
 * `UTEST_DIGITS_U32`). A binding by comment would not survive a rename; a
 * binding by name cannot drift without the lint losing its subject, which it
 * reports as a hard error rather than a pass.
 *
 * There is deliberately NO `cap` parameter. `u_append` returns 0 on overflow
 * WITHOUT terminating (it checks `p + 1 >= cap` before each byte and returns
 * having already written the ones that fit), so a helper handed a short
 * buffer would leave it unterminated -- and these five discard that return,
 * because the static assert above proves no composition can reach the bound.
 * That proof is about UTEST_REASON_BUF specifically, so the capacity is
 * pinned by the PARAMETER TYPE.
 *
 * The type is `char (*dst)[UTEST_REASON_BUF]`, pointer-to-array, and callers
 * pass `&reason`. A plain `char dst[UTEST_REASON_BUF]` would NOT do it: a
 * sized array parameter is adjusted to `char *` (C11 6.7.6.3p7), so it
 * documents an intent the compiler never checks and a short buffer would
 * still be accepted while the helper wrote 96 bytes into it -- turning what
 * had been bounded truncation into memory corruption. Pointer-to-array keeps
 * the extent in the type, so passing a `char d[48]` is a constraint
 * violation the build rejects. Each helper still writes a terminator FIRST,
 * so the buffer is a valid empty string before any append runs. */
static void u_reason_timeout(char (*dst)[UTEST_REASON_BUF], uint32_t ms)
{
    uint32_t rp = 0;

    (*dst)[0] = '\0';
    u_append(*dst, &rp, UTEST_REASON_BUF, UTEST_RSNC_TIMEOUT_PRE);
    u_append_uint(*dst, &rp, UTEST_REASON_BUF, ms);
    u_append(*dst, &rp, UTEST_REASON_BUF, UTEST_RSNC_TIMEOUT_POST);
}

static void u_reason_stall(char (*dst)[UTEST_REASON_BUF])
{
    uint32_t rp = 0;

    (*dst)[0] = '\0';
    u_append(*dst, &rp, UTEST_REASON_BUF, UTEST_RSNC_STALL);
}

static void u_reason_exit(char (*dst)[UTEST_REASON_BUF], int32_t status)
{
    uint32_t rp = 0;

    (*dst)[0] = '\0';
    /* u_append_uint is the only numeric appender, so the sign is carried
     * separately -- which is why the size macro counts it unconditionally. */
    u_append(*dst, &rp, UTEST_REASON_BUF, UTEST_RSNC_EXIT);
    if (status < 0)
        u_append(*dst, &rp, UTEST_REASON_BUF, UTEST_RSNC_NEG);
    /* Widened BEFORE the negation: -INT32_MIN is not representable in
     * int32_t, so negating in the parameter's own width would be signed
     * overflow on the one input that most wants to render correctly. */
    u_append_uint(*dst, &rp, UTEST_REASON_BUF,
                  status < 0 ? (uint64_t)(-(int64_t)status)
                             : (uint64_t)status);
}

static void u_reason_leak(char (*dst)[UTEST_REASON_BUF], uint32_t leaked)
{
    uint32_t rp = 0;

    (*dst)[0] = '\0';
    u_append_uint(*dst, &rp, UTEST_REASON_BUF, leaked);
    u_append(*dst, &rp, UTEST_REASON_BUF, UTEST_RSNC_LEAK);
}

static void u_reason_isolate(char (*dst)[UTEST_REASON_BUF])
{
    uint32_t rp = 0;

    (*dst)[0] = '\0';
    u_append(*dst, &rp, UTEST_REASON_BUF, UTEST_RSNC_ISOLATE);
}

static void u_reason_invalid(char (*dst)[UTEST_REASON_BUF])
{
    uint32_t rp = 0;

    (*dst)[0] = '\0';
    u_append(*dst, &rp, UTEST_REASON_BUF, UTEST_RSNC_INVALID);
}

/* The longest `classname` / `type` label either testcase emitter can put
 * on a record: u_type_label's widest return, itself wider than the
 * "skip-block" classname override. */
#define UTEST_LABEL_MAX      UTEST_LIT("correctness")

/* Per-kind fixed cost -- everything on the record that is NOT the name. */
#define UTEST_FIXED_VERDICT                                                \
    (UTEST_LIT(": FAIL (") + UTEST_DIGITS_U32 +                            \
     UTEST_LIT(UTEST_RSNC_LEAK " -- escalated from PASS)"))
/* A non-PASS binary testcase carries ONE of two elements, and the wider of
 * the two is what the bound must reserve. `<failure` binds today; naming
 * both keeps the derivation honest if either literal ever changes, which
 * is the same reason every reason string is enumerated above rather than
 * one being used as a proxy for the set. */
#define UTEST_XML_ELEMENT_MAX                                              \
    UTEST_MAX2(UTEST_LIT("<failure message=\""),                           \
               UTEST_LIT("<error message=\""))
#define UTEST_FIXED_XML                                                    \
    (UTEST_LIT("[UTEST-XML] <testcase name=\"") +                          \
     UTEST_LIT("\" classname=\"") + UTEST_LABEL_MAX +                      \
     UTEST_LIT("\" time=\"") + UTEST_SECONDS_MAX + UTEST_LIT("\">") +      \
     UTEST_XML_ELEMENT_MAX + UTEST_REASON_MAX +                            \
     UTEST_LIT("\"/>") + UTEST_LIT("</testcase>"))
/* The status vocabulary is PASS / FAIL / SKIP / ERROR. ERROR is the
 * never-ran discriminator (see u_format_json_testcase) and it rides the
 * EXISTING status field rather than a new one: the derived name bound has
 * one byte of margin, so an added field would shrink it and newly refuse
 * names that run today.
 *
 * The two shapes below are derived SEPARATELY because the widest one is
 * not the sum of the widest parts. A record carries the assertion-report
 * fields only for a binary that submitted a valid self-report, and a
 * never-ran ERROR by definition submitted none -- u_format_json_testcase
 * enforces exactly that, so "ERROR plus report fields" cannot be emitted.
 * Charging both at once made the derivation reserve a record no run can
 * produce, and paid for it in filename budget: the bound came out one byte
 * short and names that ran yesterday would have been refused today. The
 * maximum over REACHABLE shapes is the honest reservation. */
#define UTEST_FIXED_JSON_HEAD                                              \
    (UTEST_LIT("[UTEST-JSON] {\"record_kind\":\"binary\",\"name\":\"") +    \
     UTEST_LIT("\",\"type\":\"") + UTEST_LABEL_MAX +                       \
     UTEST_LIT("\",\"status\":\"") +                                       \
     UTEST_LIT("\",\"time_ms\":") + UTEST_DIGITS_TIME +                    \
     UTEST_LIT(",\"reason\":\"") + UTEST_REASON_MAX + UTEST_LIT("\"") +    \
     UTEST_LIT("}"))
/* Reachable shape 1: a binary that RAN, so its status is one of the
 * four-byte tokens, and it may carry the report triple. */
#define UTEST_FIXED_JSON_REPORTED                                          \
    (UTEST_FIXED_JSON_HEAD + UTEST_LIT("PASS") +                           \
     UTEST_LIT(",\"asserts_passed\":") + UTEST_DIGITS_REPORT +             \
     UTEST_LIT(",\"asserts_failed\":") + UTEST_DIGITS_REPORT +             \
     UTEST_LIT(",\"skip_blocks\":") + UTEST_DIGITS_REPORT)
/* Reachable shape 2: a binary that never RAN -- the widest status token,
 * and no report fields at all. */
#define UTEST_FIXED_JSON_NEVER_RAN                                         \
    (UTEST_FIXED_JSON_HEAD + UTEST_LIT("ERROR"))
#define UTEST_FIXED_JSON_BINARY                                            \
    UTEST_MAX2(UTEST_FIXED_JSON_REPORTED, UTEST_FIXED_JSON_NEVER_RAN)
/* The synthetic skip-record name the kinds below carry in place of the
 * bare binary name: `<binary>::skipped-block-<k>`. */
#define UTEST_FIXED_SKIP_SUFFIX                                            \
    (UTEST_LIT("::skipped-block-") + UTEST_DIGITS_REPORT)
/* The XML formatter is called TWICE with different shapes: once for a
 * binary testcase (above) and once by u_emit_skip_records for a synthetic
 * skip record, which carries the suffix, the "skip-block" classname
 * override and a FIXED 45-byte reason instead of a launcher-composed one.
 * Modelling only the first shape left the second's room unmeasured -- it
 * is not what binds at 37 today, but a later shortening elsewhere could
 * raise the accepted bound until these records overflow and lose their
 * identity, which is exactly the failure the derivation exists to make
 * impossible rather than merely unlikely. */
#define UTEST_FIXED_XML_SKIP                                               \
    (UTEST_LIT("[UTEST-XML] <testcase name=\"") + UTEST_FIXED_SKIP_SUFFIX + \
     UTEST_LIT("\" classname=\"") + UTEST_LIT("skip-block") +              \
     UTEST_LIT("\" time=\"") + UTEST_SECONDS_MAX + UTEST_LIT("\">") +      \
     UTEST_LIT("<skipped message=\"") +                                    \
     UTEST_LIT("sub-test block skipped (reason on serial log)") +          \
     UTEST_LIT("\"/>") + UTEST_LIT("</testcase>"))
#define UTEST_FIXED_JSON_SKIP                                              \
    (UTEST_LIT("[UTEST-JSON] {\"record_kind\":\"skip_block\",\"name\":\"")  \
     + UTEST_FIXED_SKIP_SUFFIX + UTEST_LIT("\",\"parent\":\"") +           \
     UTEST_LIT("\",\"skip_index\":") + UTEST_DIGITS_REPORT +               \
     UTEST_LIT(",\"status\":\"SKIP\",\"reason\":\"sub-test block skipped "  \
               "(reason on serial log)\"}"))
#define UTEST_FIXED_TAP                                                    \
    (UTEST_LIT("not ok ") + UTEST_DIGITS_U32 + UTEST_LIT(" - ") +          \
     UTEST_FIXED_SKIP_SUFFIX + UTEST_LIT(" # ") +                          \
     UTEST_MAX2(UTEST_LIT("SKIP reported by binary"), UTEST_REASON_MAX))
#define UTEST_FIXED_REPORT                                                 \
    (UTEST_LIT("[UTEST-REPORT] ") + UTEST_LIT(" asserts_passed=") +        \
     UTEST_DIGITS_REPORT + UTEST_LIT(" asserts_failed=") +                 \
     UTEST_DIGITS_REPORT + UTEST_LIT(" skip_blocks=") +                    \
     UTEST_DIGITS_REPORT + UTEST_LIT(" state=") + UTEST_LIT("INVALID"))

/* Room a kind leaves for the name: the appenders refuse at
 * `pos + 1 >= cap` (u_append), so a record holds UTEST_RECORD_LINE_MAX - 1
 * content bytes. Divided by the multiplicity, because a kind that carries
 * the name twice pays for it twice. */
#define UTEST_NAME_ROOM(fixed, mult)                                       \
    (((UTEST_RECORD_LINE_MAX - 1u) - (fixed)) / (mult))

#define UTEST_MAX_BINARY_NAME                                              \
    UTEST_MIN2(                                                            \
        UTEST_MIN2(UTEST_MIN2(UTEST_NAME_ROOM(UTEST_FIXED_VERDICT, 1u),    \
                              UTEST_NAME_ROOM(UTEST_FIXED_XML, 1u)),       \
                   UTEST_MIN2(UTEST_NAME_ROOM(UTEST_FIXED_JSON_BINARY, 1u),\
                              UTEST_NAME_ROOM(UTEST_FIXED_JSON_SKIP, 2u))),\
        UTEST_MIN2(UTEST_MIN2(UTEST_NAME_ROOM(UTEST_FIXED_TAP, 1u),        \
                              UTEST_NAME_ROOM(UTEST_FIXED_REPORT, 1u)),    \
                   UTEST_NAME_ROOM(UTEST_FIXED_XML_SKIP, 1u)))

/* Every kind must still fit at the derived bound. The division above makes
 * that arithmetically true; the assert exists to catch a future edit that
 * replaces the minimum with a literal and quietly breaks the relationship
 * -- the same failure mode the skip-budget asserts above guard against. */
_Static_assert(UTEST_FIXED_JSON_SKIP + 2u * UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_JSON_BINARY + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_XML_SKIP + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_XML + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_TAP + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_VERDICT + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_REPORT + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u,
               "a name at the derived bound must fit EVERY record kind "
               "without any formatter reaching its overflow fallback");

/* The plan's name arena is sized in fixed strides declared far above this
 * derivation (the plan structs have to precede the manifest parser that
 * fills them, and the derivation depends on the record formats declared
 * between the two). This is the assert that ties the two ends together:
 * an ACCEPTED name is bounded by UTEST_MAX_BINARY_NAME, so as long as a
 * slot can hold that plus its terminator, u_plan_intern cannot truncate
 * one. Refusal prefixes are separately bounded by
 * UTEST_REFUSAL_PREFIX_STORE. If a future record format shrinks the
 * derived bound this stays true; if a future edit shrinks the SLOT, this
 * fails the build instead of silently planning a truncated identity. */
_Static_assert(UTEST_MAX_BINARY_NAME + 1u <= UTEST_PLAN_NAME_SLOT,
               "UTEST_PLAN_NAME_SLOT must hold any accepted binary name "
               "plus its NUL, or u_plan_intern would truncate an identity "
               "the classifier already accepted");
_Static_assert(UTEST_REFUSAL_PREFIX_STORE <= UTEST_PLAN_NAME_SLOT,
               "a sanitized refusal prefix must fit an interned plan slot");

/* Compatibility ratchet. The derivation is honest in one direction on
 * its own -- a name past the bound is refused rather than truncated --
 * but nothing stops a future format string from GROWING its fixed cost
 * and silently shrinking this bound, which turns filenames that ran
 * yesterday into counted failures today. That is a compatibility break
 * and belongs in a review, not in a build that stays green.
 *
 * So the floor is today's derived value, not a comfortable margin above
 * the longest name the repository currently builds (test_harness_smoke.exe,
 * 22 bytes). Shrinking the bound now requires deliberately lowering this
 * number, which is exactly the decision that should be explicit. It lives
 * here alone rather than being mirrored into the unit test, so there is
 * one ratchet to move rather than two that can disagree.
 *
 * It held through the never-ran status added on 2026-07-31. "ERROR" is one
 * byte wider than the "PASS" the status field used to reserve, and a first
 * cut of that work charged the wide status and the assertion-report triple
 * to the SAME record -- which lowered this floor to 36 and would have
 * started refusing 37-byte names. That combination is unreachable (a
 * never-ran binary submits no report), so the JSON derivation now takes
 * the maximum over the two REACHABLE shapes and the floor stays where it
 * was. The episode is why the ratchet exists: the cost was visible only
 * because lowering it had to be deliberate. */
#define UTEST_NAME_BOUND_RATCHET 37u
_Static_assert(UTEST_MAX_BINARY_NAME >= UTEST_NAME_BOUND_RATCHET,
               "a record format grew and shrank the derived name bound: "
               "names that were accepted before would now be refused, so "
               "lower UTEST_NAME_BOUND_RATCHET deliberately or don't");

/* Hex digits of the refused-name digest. The digest exists to correlate
 * the SAME bad name across runs; within a run, uniqueness comes from the
 * ordinal below rather than from hash strength, because no digest width
 * can guarantee distinctness and this identity has to. */
#define UTEST_NAME_DIGEST_HEX     8u
#define UTEST_REFUSAL_PREFIX_MIN  4u
/* The identity is assembled at runtime from the ordinal's ACTUAL digit
 * count, so this reserves the worst case (a 10-digit ordinal) and asserts
 * that a readable prefix still survives it. */
#define UTEST_REFUSAL_ID_FIXED                                             \
    (UTEST_LIT("refused_") + UTEST_DIGITS_U32 + UTEST_LIT("_") +           \
     UTEST_LIT("_") + UTEST_NAME_DIGEST_HEX + UTEST_LIT(".exe"))
_Static_assert(UTEST_REFUSAL_ID_FIXED + UTEST_REFUSAL_PREFIX_MIN <=
                   UTEST_MAX_BINARY_NAME,
               "a refusal identity must fit the bound it exists to prove, "
               "with room left for a prefix an operator can read");

/* ---- Aggregate identities ------------------------------------------ *
 *
 * The five fail-closed aggregates are not refusals of a NAME: no file
 * produced them, so there is nothing for the correlator to hash back to.
 * They used to borrow the refusal shape anyway, with the 8-hex digest
 * slot carrying a number instead of `fnv1a32` of anything -- which made
 * them indistinguishable from a per-name refusal to any consumer, and let
 * scripts/utest-refusal-id.py in principle name an unrelated candidate
 * whose digest happened to equal that number. The digest genuinely
 * matched, so the tool's report-every-candidate rule could not catch it.
 *
 * They get their own shape instead: `agg_<label>_<value>.exe`. A consumer
 * tells the two apart STRUCTURALLY, on the leading token, rather than by
 * mirroring a list of kernel labels -- the mirrored-constant failure this
 * subsystem has already refused twice. It cannot collide with a real
 * binary either: u_is_test_binary requires a `test_` prefix, so no
 * accepted name can begin `agg_`.
 *
 * No ordinal. A refusal ordinal exists because arbitrarily many names can
 * be refused in one run and the digest cannot promise to separate them;
 * each aggregate path below fires AT MOST ONCE per run and its label
 * names which one, so the label already is the run-unique identity. The
 * bytes that buys go to the label, which is why it never truncates.
 *
 * The VALUE is not one semantic type across the five, and the identity
 * says which it is rather than presenting all of them as "the count of
 * what was lost" (the previous shape, and the previous documentation,
 * both claimed exactly that and were wrong for four of the five). Each
 * label carries its own unit: `_pages` is a page count that was
 * requested, `_kept` is how many entries were RETAINED before the cap bit
 * -- the loss is whatever came after, which by construction was never
 * counted -- and `manifest_bad` carries no number at all, because a
 * manifest that would not parse leaves the number of lost entries
 * genuinely unknowable. Publishing `0` for it, as the old shape did, is a
 * false statement about an unknown quantity. */
/* The aggregate grammar, named once and used by the builder, by the
 * bound derivation, and by the host regressions that read them back out
 * of this file. They were literals in three places; a host test that
 * hardcodes the same three can keep validating the OLD grammar after the
 * producer changes, and then every newly emitted identity is rejected by
 * the correlator with nothing red. */
#define UTEST_AGG_PREFIX        "agg_"
#define UTEST_AGG_SEP           "_"
#define UTEST_AGG_SUFFIX        ".exe"
#define UTEST_AGG_VALUE_UNKNOWN "unknown"

/* ONE authoritative table. The enum, the label mapping and the per-label
 * bound assertion are all generated from it, so a new aggregate cannot be
 * added to some of those lists and omitted from the others -- the failure
 * the enumerated-reason tree above still has to guard against by hand. In
 * particular the bound is asserted PER LABEL rather than against a
 * hand-maintained maximum: a maximum that a new member is missing from
 * still passes, and the omission would surface only as a lost identity on
 * a rare path at runtime.
 *
 * The third column is whether the kind CARRIES a value at all. It is a
 * property of the KIND, not a decision each call site makes: an
 * unparseable manifest cannot know its loss on any code path, so no
 * caller is in a position to say otherwise. Keeping it here means a
 * publication site cannot pass the wrong one, which is strictly better
 * than testing that all five happen to pass the right one -- flipping
 * `manifest_bad` back to value-bearing would restore exactly the
 * misleading zero this shape exists to remove. */
#define UTEST_AGG_KINDS(X)                                                 \
    X(PLAN_PAGES,    "plan_pages",    1)                                   \
    X(PLAN_KEPT,     "plan_kept",     1)                                   \
    X(MANIFEST_KEPT, "manifest_kept", 1)                                   \
    X(MANIFEST_BAD,  "manifest_bad",  0)                                   \
    X(GLOB_KEPT,     "glob_kept",     1)

typedef enum {
#define UTEST_AGG_ENUM(name, label, has_value) UTEST_AGG_##name,
    UTEST_AGG_KINDS(UTEST_AGG_ENUM)
#undef UTEST_AGG_ENUM
    UTEST_AGG_COUNT
} utest_agg_kind_t;

/* Everything on an aggregate identity that is not the label. The value
 * reserves the wider of a full uint32 in decimal and the literal that
 * replaces it, for the same reason the refusal shape reserves a 10-digit
 * ordinal: the runtime width is smaller, but the assert has to hold for
 * the worst case the code can actually reach. */
#define UTEST_AGG_VALUE_MAX                                                \
    UTEST_MAX2(UTEST_DIGITS_U32, UTEST_LIT(UTEST_AGG_VALUE_UNKNOWN))
#define UTEST_AGGREGATE_ID_FIXED                                           \
    (UTEST_LIT(UTEST_AGG_PREFIX) + UTEST_LIT(UTEST_AGG_SEP) +              \
     UTEST_AGG_VALUE_MAX + UTEST_LIT(UTEST_AGG_SUFFIX))
#define UTEST_AGG_FITS(name, label, has_value)                             \
    _Static_assert(UTEST_AGGREGATE_ID_FIXED + UTEST_LIT(label) <=          \
                       UTEST_MAX_BINARY_NAME,                              \
                   "aggregate label \"" label "\" does not fit the "       \
                   "derived name bound: shorten it rather than letting "   \
                   "the identity truncate, because a truncated label no "  \
                   "longer names which aggregate path published it");
UTEST_AGG_KINDS(UTEST_AGG_FITS)
#undef UTEST_AGG_FITS

/* The accepted charset. Restricting it is what makes the bound above
 * PROVABLE rather than probabilistic: u_xml_escape and u_json_escape are
 * the identity on every byte in this set, so N accepted bytes still cost
 * N bytes on the wire. Without it a single `"` expands to `&quot;` and a
 * name six times shorter than the bound could still overflow a record.
 * The set also subsumes the path-shape checks this validator used to make
 * one at a time -- `\`, `/`, `:`, spaces, `#` and every control byte are
 * outside it -- so `#` can no longer inject a TAP directive either. */
static int u_name_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static utest_name_verdict_t u_classify_name_span(const char *name,
                                                 uint32_t span_len)
{
    uint32_t len = 0;
    const char *p;

    if (!name)
        return UTEST_NAME_NOT_TEST_SHAPED;
    while (name[len])
        len++;
    /* The span check comes FIRST, ahead of the shape check, and the
     * order is the whole point. An embedded NUL means the C string the
     * validator, the filter and the digest all see is SHORTER than the
     * bytes the caller actually holds. Classifying shape first would
     * read that truncation as the real name -- and for `test_bad\0.exe`
     * the truncation is `test_bad`, which is NOT test-shaped, so the
     * entry would be ignored as a stray file rather than counted as the
     * malformed request it is. A NUL anywhere in the span is a refusal
     * regardless of what the visible prefix happens to look like.
     *
     * Only the manifest can produce a mismatch here: dirent names are C
     * strings, so the glob path always passes span_len == len. */
    if (len != span_len)
        return UTEST_NAME_REFUSE_NUL;
    if (!name[0])
        return UTEST_NAME_NOT_TEST_SHAPED;
    /* Shape next: a file that is not test_*.exe is not addressed to this
     * framework, and classifying it as a refusal would turn every stray
     * file in C:\ into a suite failure. Everything below this line is a
     * binary somebody meant to run. */
    if (!u_is_test_binary(name))
        return UTEST_NAME_NOT_TEST_SHAPED;
    if (len > UTEST_MAX_BINARY_NAME)
        return UTEST_NAME_REFUSE_LENGTH;
    for (p = name; *p; p++) {
        if (!u_name_char_ok(*p))
            return UTEST_NAME_REFUSE_CHARSET;
        /* `.` and `..` cannot be test-shaped on their own, but
         * `test_.._foo.exe` is, and both dots are inside the charset --
         * so traversal stays an explicit check. */
        if (p[0] == '.' && p[1] == '.')
            return UTEST_NAME_REFUSE_PATH;
    }
    return UTEST_NAME_ACCEPT;
}

static utest_name_verdict_t u_classify_name(const char *name)
{
    uint32_t len = 0;

    if (!name)
        return UTEST_NAME_NOT_TEST_SHAPED;
    while (name[len])
        len++;
    return u_classify_name_span(name, len);
}

/* Short human label for a refusal, used as the record's `reason`. Every
 * string here is inside UTEST_REASON_REFUSAL's derived width. */
static const char *u_refusal_reason(utest_name_verdict_t v)
{
    switch (v) {
    case UTEST_NAME_REFUSE_LENGTH:  return UTEST_RSN_LENGTH;
    case UTEST_NAME_REFUSE_CHARSET: return UTEST_RSN_CHARSET;
    case UTEST_NAME_REFUSE_PATH:    return UTEST_RSN_PATH;
    case UTEST_NAME_REFUSE_NUL:     return UTEST_RSN_NUL;
    case UTEST_NAME_NOT_TEST_SHAPED: return UTEST_RSN_SHAPE;
    case UTEST_NAME_REFUSE_ATTR:    return UTEST_RSN_ATTR;
    default:                        return UTEST_RSN_GENERIC;
    }
}

/* FNV-1a over the EXACT byte span, not over a C string: the whole point
 * of the digest is to distinguish two names the sanitized prefix renders
 * identically, and a name refused for an embedded NUL is precisely the
 * case where the C string stops early. Constants are the published FNV-1a
 * 32-bit basis and prime. */
#define UTEST_FNV1A_BASIS 2166136261u
#define UTEST_FNV1A_PRIME 16777619u
static uint32_t u_name_digest(const char *p, uint32_t len)
{
    uint32_t h = UTEST_FNV1A_BASIS;
    uint32_t i;

    for (i = 0; i < len; i++) {
        h ^= (uint32_t)(uint8_t)p[i];
        h *= UTEST_FNV1A_PRIME;
    }
    return h;
}

/* Build the identity a refused binary appears under:
 * `refused_<ordinal>_<sanitized prefix>_<8 hex>.exe`.
 *
 * Three properties, in the order they matter. It never echoes the raw
 * name, so a hostile name cannot reach the wire through the record that
 * reports it. Its ordinal is unique WITHIN the run by construction, which
 * is stronger than any digest width could be -- a 32-bit FNV-1a is not
 * collision-resistant, and an operator who cannot tell two refusals apart
 * cannot act on either. The digest is what still correlates the SAME bad
 * name across runs, which the ordinal alone cannot do. It ends in `.exe`
 * because the host's fail-closed recount greps for a `.exe` name followed
 * by a verdict token; an identity that missed it would be reported by the
 * launcher and dropped by the host.
 *
 * The prefix takes whatever the ordinal leaves, so an early refusal gets
 * a readable prefix while the static assert above still holds against a
 * worst-case 10-digit ordinal.
 *
 * `digest` is passed in rather than computed here: it must cover the
 * entry's exact byte span, which only the ENUMERATOR still knows -- an
 * entry refused for an embedded NUL has a C string shorter than the bytes
 * it came from, so hashing `raw` at this point would hash the truncation
 * instead of the name. `raw` is used only for the readable prefix, where
 * stopping at the NUL is harmless. Returns 1 on success, 0 if `cap`
 * cannot hold a bound-conforming identity. */
static int u_build_refusal_id(char *dst, uint32_t cap, uint32_t ordinal,
                              const char *raw, uint32_t digest)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t pos = 0;
    uint32_t tail = UTEST_LIT("_") + UTEST_NAME_DIGEST_HEX + UTEST_LIT(".exe");
    uint32_t budget, i;

    if (!dst || cap < UTEST_MAX_BINARY_NAME + 1u)
        return 0;
    dst[0] = '\0';
    if (!u_append(dst, &pos, cap, "refused_"))
        return 0;
    if (!u_append_uint(dst, &pos, cap, (uint64_t)ordinal))
        return 0;
    if (!u_append(dst, &pos, cap, "_"))
        return 0;
    /* Room left for the prefix once the fixed tail is reserved. */
    if (pos + tail >= UTEST_MAX_BINARY_NAME)
        return 0;
    budget = UTEST_MAX_BINARY_NAME - pos - tail;
    for (i = 0; i < budget && raw && raw[i]; i++)
        dst[pos++] = u_name_char_ok(raw[i]) ? raw[i] : '_';
    dst[pos] = '\0';
    if (!u_append(dst, &pos, cap, "_"))
        return 0;
    for (i = 0; i < UTEST_NAME_DIGEST_HEX; i++) {
        uint32_t shift = (UTEST_NAME_DIGEST_HEX - 1u - i) * 4u;
        if (pos + 1u >= cap)
            return 0;
        dst[pos++] = hex[(digest >> shift) & 0xFu];
    }
    dst[pos] = '\0';
    return u_append(dst, &pos, cap, ".exe");
}

/* The label an aggregate kind publishes under, from the same table the
 * enum and the bound asserts come from. An unenumerated kind returns NULL
 * and the builder below refuses it: a kind that reached this switch
 * without a label has no identity, and publishing it under a blank one
 * would put a record in the artifact that names no path at all. */
static const char *u_aggregate_label(utest_agg_kind_t kind)
{
    switch (kind) {
#define UTEST_AGG_CASE(name, label, has_value) \
    case UTEST_AGG_##name: return label;
    UTEST_AGG_KINDS(UTEST_AGG_CASE)
#undef UTEST_AGG_CASE
    default: return (const char *)0;
    }
}

/* Whether a kind publishes a number at all, from the same table. A caller
 * never decides this: see the table's comment. An unenumerated kind
 * answers 0, but the builder rejects it on the missing label first. */
static int u_aggregate_has_value(utest_agg_kind_t kind)
{
    switch (kind) {
#define UTEST_AGG_HASVAL(name, label, has_value) \
    case UTEST_AGG_##name: return has_value;
    UTEST_AGG_KINDS(UTEST_AGG_HASVAL)
#undef UTEST_AGG_HASVAL
    default: return 0;
    }
}

/* Build the identity a fail-closed aggregate appears under:
 * `agg_<label>_<value>.exe`, or `agg_<label>_unknown.exe` when the value
 * is not knowable.
 *
 * The unparseable-manifest path publishes the `unknown` token instead of
 * a number, because every entry went unexamined and the number lost
 * cannot be derived from anything the launcher still holds. It is a
 * DISTINCT token rather than a zero so a consumer cannot read "nothing
 * was lost" off a record that means "we cannot say". Which kinds those
 * are is read from the table, NOT passed in: a caller is not in a
 * position to know better, and one that passed the wrong answer would
 * silently restore the zero.
 *
 * Unlike u_build_refusal_id there is no truncation arm: the label comes
 * from a compile-time table whose every member is asserted to fit the
 * derived bound, so a label that would not fit fails the BUILD instead of
 * silently losing the bytes that say which path published the record.
 * Returns 1 on success, 0 if the kind has no label or `cap` cannot hold a
 * bound-conforming identity. */
static int u_build_aggregate_id(char *dst, uint32_t cap,
                                utest_agg_kind_t kind,
                                uint32_t value)
{
    const char *label = u_aggregate_label(kind);
    int has_value = u_aggregate_has_value(kind);
    uint32_t pos = 0;

    if (!dst || cap < UTEST_MAX_BINARY_NAME + 1u || !label)
        return 0;
    dst[0] = '\0';
    if (!u_append(dst, &pos, cap, UTEST_AGG_PREFIX))
        return 0;
    if (!u_append(dst, &pos, cap, label))
        return 0;
    if (!u_append(dst, &pos, cap, UTEST_AGG_SEP))
        return 0;
    if (has_value) {
        if (!u_append_uint(dst, &pos, cap, (uint64_t)value))
            return 0;
    } else if (!u_append(dst, &pos, cap, UTEST_AGG_VALUE_UNKNOWN)) {
        return 0;
    }
    if (!u_append(dst, &pos, cap, UTEST_AGG_SUFFIX))
        return 0;
    /* The per-label static asserts prove the shape fits; this catches a
     * caller that passed a buffer smaller than the bound demands, which
     * the asserts cannot see. */
    return pos <= UTEST_MAX_BINARY_NAME;
}

/* Clamp elapsed milliseconds to the width the record bound reserves for
 * them. Uptime is a uint64 with no natural cap, so without this the
 * `time_ms` field's digit count is an assumption rather than a fact --
 * and a bound derived from an assumption is the picked number the whole
 * derivation exists to avoid. A run that reaches 49 days of uptime
 * reports a saturated duration; every other field stays exact. */
static uint64_t u_clamp_time_ms(uint64_t ms)
{
    return ms > UTEST_TIME_MS_MAX ? (uint64_t)UTEST_TIME_MS_MAX : ms;
}

/* Machine-artifact records bypass the per-subsystem rate limiter.
 *
 * klog drops messages past 100 per second per subsystem, and under
 * `xml=1 json=1` the UTEST subsystem emits several records per binary --
 * so a fast enough suite can push the mandatory tail (run_report, summary,
 * run_meta, and the XML closer) past the budget. The host then refuses a
 * perfectly valid run because its terminator never arrived, and the drop
 * gets worse on faster hardware. A rate limiter silently deleting the
 * records an artifact is assembled from is the same false-green class this
 * whole path exists to close. The volume is bounded by the binary count,
 * not by anything unbounded, so the limiter has nothing to protect here.
 *
 * Human-readable UTEST progress lines deliberately keep the rate limit. */

/* ---- Non-forgeable record framing ---------------------------------- *
 *
 * Ring-3 stdout and every launcher record share one serial stream:
 * `sys_write(fd=1, ...)` copies caller-controlled bytes straight to
 * `serial_putchar` (src/kernel/sched/syscall.c) with no annotation of any
 * kind. A test binary could therefore print a byte-identical
 * `UTEST: === 5 passed, 0 failed, 0 skipped of 5 total ===`, and the host's
 * boot-completion poll would accept it, stop QEMU, and report success
 * without ever seeing the launcher's real verdict. The same shared
 * namespace let a binary inject `[UTEST-XML]` / `[UTEST-JSON]` records
 * straight into the assembled artifact, where no budget or counter
 * accounted for them.
 *
 * The frame is a per-boot nonce carried in the klog SUBSYSTEM field, so a
 * launcher record renders as `[INFO] UTEST-1a2b3c4d: <payload>` where a
 * forgery can only manage `[INFO] UTEST: <payload>`. It rides the subsystem
 * rather than the message because every record is bounded by klog's
 * 256-byte message field and the JSON summary already measured 242 of 255
 * usable bytes: framing inside the message would be paid for by every
 * emitter and would push the tightest records over the wire. The subsystem
 * is a `const char *`, so the tag costs the record budget nothing.
 *
 * Ring 3 cannot observe the value. It is derived in kernel context, no
 * syscall reads it back, and it leaves the kernel on SERIAL ONLY: the live
 * disk log is openable from ring 3, so the frame tag is aliased back to the
 * plain "UTEST" on that sink (klog_set_disk_alias) and the announcement
 * body does not restate the nonce. Without both, a binary could read
 * X:\Logs\Serial_*.log -- which klog appends and flushes synchronously,
 * before the first test task runs -- and forge a correctly framed
 * terminator.
 *
 * The nonce is generated ONCE PER BOOT and never rewritten, for two
 * reasons. test_usermode_run() is documented safe to call repeatedly, and
 * klog's ring entries retain the subsystem as a POINTER -- rewriting the
 * buffer for a second run would silently re-attribute every retained
 * first-run entry, including the crash-region evidence the next boot reads
 * back. Each run instead gets its own ordinal and its own record count. */
#define UTEST_FRAME_NONCE_HEX 8u
#define UTEST_FRAME_TAG_MAX   (sizeof("UTEST-") + UTEST_FRAME_NONCE_HEX)
_Static_assert(UTEST_FRAME_TAG_MAX <= KLOG_SUBSYSTEM_MAX,
               "the framed subsystem tag must fit klog's serialized "
               "subsystem field or crash-region and debug-console evidence "
               "is truncated for every launcher record");

/* Written exactly once per boot, then read for the life of the boot: klog
 * keeps the pointer, so this storage must outlive every entry logged under
 * it. `s_frame_ready` publishes the buffer with release ordering. */
static char     s_frame_tag[UTEST_FRAME_TAG_MAX];
static uint32_t s_frame_nonce;    /* 0 = not generated yet (the sentinel) */
static uint32_t s_frame_ready;    /* 1 = s_frame_tag is filled and stable */
/* 1 between u_frame_begin and u_frame_end -- a framed run is OPEN, so any
 * record emitted now is authenticated INTO that run's slice. Distinct from
 * s_frame_ready, which is sticky for the whole boot: "the tag exists" and "a
 * run is currently collecting records" are different facts, and only the
 * second one makes a stray emission dangerous. */
static uint32_t s_frame_open;
static uint32_t s_frame_records;  /* framed records emitted in THIS run */
static uint32_t s_frame_run;      /* run ordinal, 1-based */
/* Binaries THIS run spawned with capture armed. Declared beside the frame
 * counters because the run terminator publishes it; written by u_run_one
 * and reset by u_capture_budget_reset, both far below -- see the block
 * above u_capture_expect for why the count and the expectation records
 * come from deliberately different call sites. */
static uint32_t s_capture_spawned;

/* Format "UTEST-<8 lowercase hex>" into dst. Pure: touches no globals and
 * logs nothing, so a unit test can exercise it directly. Returns 1 on
 * success, 0 if the buffer cannot hold the tag and its NUL. */
static int u_frame_tag_format(char *dst, uint32_t cap, uint32_t nonce)
{
    static const char hex[] = "0123456789abcdef";
    const char       *pre   = "UTEST-";
    uint32_t          i     = 0;
    uint32_t          k;

    if (!dst || cap < UTEST_FRAME_TAG_MAX)
        return 0;
    while (*pre)
        dst[i++] = *pre++;
    for (k = 0; k < UTEST_FRAME_NONCE_HEX; k++)
        dst[i++] = hex[(nonce >> (28u - 4u * k)) & 0xFu];
    dst[i] = '\0';
    return 1;
}

/* The subsystem every framed record is logged under. Falls back to the
 * unframed literal before the frame is published, which is unreachable by
 * construction (the launcher publishes before it emits anything) and fails
 * CLOSED if it ever happens: the record is still counted, so the host's
 * count reconciliation refuses the run rather than accepting a stream with
 * a silently unframed record in it. */
static const char *u_frame_tag(void)
{
    if (__atomic_load_n(&s_frame_ready, __ATOMIC_ACQUIRE))
        return s_frame_tag;
    return "UTEST";
}

/* Machine-artifact records bypass the per-subsystem rate limiter (see
 * above) and carry the frame. Every framed record is counted so the run's
 * terminator can state how many the host must have seen. */
#define utest_record_log(level, ...)                                   \
    do {                                                               \
        __atomic_fetch_add(&s_frame_records, 1u, __ATOMIC_RELAXED);    \
        klog_unrated((level), u_frame_tag(), __VA_ARGS__);             \
    } while (0)

/* The same record, carrying a klog delivery receipt (klog.h). The receipt is
 * what makes a capture claim settle AT the wire instead of after the emitter
 * returns from the log call, so an emitter that dies in between has already
 * been credited for output the host actually has.
 *
 * Declared here beside the plain record macro, but only the capture emitter
 * uses it: every other framed record is the launcher's own, emitted on a
 * thread whose survival across the call is not in question. */
/* The census increment is NOT here, unlike the plain macro above, and the
 * difference is the whole reason a receipted record is worth having. The
 * terminator's `records=` is what the host reconciles its slice against, so it
 * has to count what klog PUT ON THE WIRE -- and a receipted caller is exactly
 * the one that finds out which. Counting the ATTEMPT instead would declare a
 * record the host never received: frame reconciliation would fail first, and
 * the run's own [UTEST-CAPTURE-UNDELIVERED] diagnosis would be unreachable
 * behind generic corruption, for precisely the drop it exists to name. The
 * receipt does the counting, on delivery. */
#define utest_record_log_receipted(level, ack, cookie, ...)            \
    klog_receipted((level), u_frame_tag(), (ack), (cookie), __VA_ARGS__)

/* Derive this boot's nonce. csprng is seeded in Phase 1, long before the
 * launcher runs in Phase 3; the TSC mix is a defence-in-depth salt, not a
 * substitute, since a monotonic counter is approximable from outside. 0 is
 * the "not generated" sentinel, so it is folded to 1. */
static uint32_t u_frame_nonce_fold(uint64_t mixed)
{
    uint32_t nonce = (uint32_t)(mixed ^ (mixed >> 32));

    return nonce ? nonce : 1u;
}

/* A framed record rather than a bare klog line, so it is counted in the run's
 * terminator like every other launcher record, and kept SEPARATE from any
 * timeout reporting: a reader who sees this knows the launcher stopped being
 * able to measure time, which is a different investigation from a producer that
 * hung. `site` names WHICH wait gave up, because the five of them fail for
 * unrelated reasons and one undifferentiated record would send a reader to the
 * wrong one. Setting the sticky flag here rather than at each call site is what
 * makes it impossible to report a stall without also failing the run. */
static void u_wait_report_stall(const char *site, uint64_t detail)
{
    s_run_latches.wait_stalled = 1;
    utest_record_log(LOG_WARN, "[UTEST-WAIT-STALLED] site=%s detail=%u",
                     site, detail);
}

static uint32_t u_frame_nonce_new(void)
{
    /* The nonce authenticates records, so an uncredited entropy source is
     * worth saying out loud rather than assuming away. It is NOT fatal: the
     * threat model is a binary printing a launcher pattern by accident as
     * much as a hostile one, and a caller that cannot observe the value
     * cannot exploit a weak one. The TSC term is a salt, not a backstop --
     * rdtsc_ns() returns 0 outright when the active clocksource is not the
     * TSC. */
    if (!csprng_is_seeded())
        klog(LOG_WARN, "UTEST",
             "record framing nonce drawn before the CSPRNG was seeded -- "
             "the frame is still unguessable from ring 3 but is not "
             "cryptographically random");

    return u_frame_nonce_fold(csprng_u64() ^ rdtsc_ns());
}

/* Open a framed run: publish the per-boot tag on first use, take the next
 * run ordinal, reset the per-run record count, and announce the frame.
 *
 * The announcement is itself framed and is the FIRST framed record of the
 * run, so the host learns the nonce from a line only the kernel can have
 * produced at a point where no ring-3 code of this run has executed yet.
 * Concurrent invocation is not a supported shape (the launcher is a
 * boot-phase singleton driven from boot_tests_run) and fails closed if
 * attempted: the loser's records are counted but unframed, so the host's
 * reconciliation refuses the run. */
/* Defined with the rest of the capture budget, far below: the budget's
 * constants derive from UTEST_CAPTURE_CHUNK_MAX, which is not defined until
 * after the framing block, and splitting the derivation away from the
 * enforcement it feeds would be worse than one forward declaration. */
static void u_capture_budget_reset(void);

/* The run boundary's other two halves, forward-declared for the same reason
 * the reset is: they belong beside the budget state they operate on, and the
 * frame terminator below is the only caller. */
static void u_capture_seal(void);
static uint64_t u_capture_drain(void);
static uint64_t u_capture_close_epoch(uint64_t *undelivered_out);

static void u_frame_begin(void)
{
    uint32_t expected = 0;

    if (!__atomic_load_n(&s_frame_ready, __ATOMIC_ACQUIRE)) {
        uint32_t nonce = u_frame_nonce_new();

        if (__atomic_compare_exchange_n(&s_frame_nonce, &expected, nonce, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            int framed = u_frame_tag_format(s_frame_tag,
                                            (uint32_t)sizeof(s_frame_tag),
                                            nonce);
            /* Keep the nonce off the one sink ring 3 can read. The live
             * disk log (X:\Logs\Serial_*.log) is openable through
             * SYS_OPENFILE, and klog appends+flushes each message to it
             * synchronously -- so without this the announcement is on disk
             * BEFORE the first test task runs, and a binary could read the
             * value back and emit a correctly framed terminator. Serial
             * keeps the frame; disk keeps the records under the plain tag,
             * so post-mortem diagnosis is unaffected. */
            if (framed)
                framed = klog_set_disk_alias(s_frame_tag, "UTEST");

            /* Framing is published ONLY when both halves took. Either
             * refusal leaves the authenticating tag reaching the
             * ring-3-readable disk log, and publishing anyway would assert a
             * property the run does not have -- so this fails CLOSED: the
             * records stay unframed, every host gate refuses them, and the
             * run reports the refusal instead of a false green. */
            if (framed) {
                /* The per-subsystem verbosity ceiling is a whole-string
                 * match, so the boot's klog_set_level("UTEST", LOG_DEBUG)
                 * does NOT cover "UTEST-<nonce>". Without this the records
                 * counted below could be dropped by the level filter before
                 * reaching serial -- klog_unrated bypasses only the RATE
                 * limiter -- and the host's count reconciliation would then
                 * refuse a complete run. */
                klog_set_level(s_frame_tag, LOG_DEBUG);
                __atomic_store_n(&s_frame_ready, 1u, __ATOMIC_RELEASE);
            } else {
                klog(LOG_ERROR, "UTEST",
                     "record framing unavailable -- launcher records will be "
                     "refused by the host (tag format or disk alias refused)");
            }
        }
    }

    __atomic_store_n(&s_frame_records, 0u, __ATOMIC_RELAXED);
    /* Atomic like its siblings: it is a run-identity field the host
     * reconciles against, and the public header documents
     * test_usermode_run() as safe to call repeatedly. */
    (void)__atomic_add_fetch(&s_frame_run, 1u, __ATOMIC_RELAXED);

    /* The capture emission budget is scoped to the FRAMED RUN, for exactly
     * the reason s_frame_records is: the host proves a run-scope stop by
     * counting the chunk records in the canonical run SLICE, so a producer
     * counter spanning boot lifetime would measure a different population
     * than the check it has to satisfy. Both consequences were live, not
     * theoretical: the kernel's own capture regressions
     * (test_usermode_launcher.c:3467) drive the real emit path BEFORE any
     * framed run and charged this counter permanently, and a second
     * test_usermode_run() -- supported per the comment above -- would
     * inherit every earlier charge plus a latched stop, terminating capture
     * for a run that had emitted nothing. Under the lock so a concurrent
     * claim cannot straddle the reset. */
    u_capture_budget_reset();

    /* The body deliberately does NOT restate the nonce. The TAG carries it,
     * and a body copy would survive the disk alias below -- putting the
     * value back into the one sink ring 3 can read. Nothing is lost: a
     * tag-vs-body cross-check only ever caught producer drift, which the
     * record-count reconciliation now covers, and it was never a barrier to
     * a forger who can write both fields. */
    /* Published BEFORE the announcement: from this record onward every framed
     * line belongs to the run's slice, so the flag must already be true when
     * the first one is emitted. */
    __atomic_store_n(&s_frame_open, 1u, __ATOMIC_RELEASE);

    utest_record_log(LOG_INFO, "[UTEST-FRAME] v=1 run=%u",
                     (uint64_t)__atomic_load_n(&s_frame_run, __ATOMIC_RELAXED));
}

/* Close a framed run. `records=` counts every framed record emitted before
 * this line, so a host that saw the whole stream counts exactly
 * `records + 1` framed lines for the run. A binary that prints a plausible
 * summary and then hangs cannot produce this line, and a stream cut short
 * cannot reconcile -- which is what makes the terminator, not the summary,
 * the run's completion signal. */
/* `spawned=` is the run's own count of binaries launched with capture
 * armed, and it is what makes the expectation set falsifiable: the host
 * requires exactly this many distinct [UTEST-CAPTURE-EXPECT] owners, so a
 * vanished expectation record is a mismatch rather than a silently smaller
 * set. Read here, after every u_run_one has returned. */
/* `[UTEST-CAPTURE-PENDING]` -- what the run boundary could not deliver.
 *
 * Emitted only when the drain ends with claims still outstanding, INSIDE the
 * originating run and BEFORE its terminator, and both of those placements are
 * the finding this record exists to answer. A stale arrival reported in the
 * run that INHERITS it cannot make the guilty run fail: that run has closed,
 * so the report either contaminates an innocent successor or is ignored. And
 * a diagnostic emitted from u_frame_begin's pre-announcement window (records
 * reset and generation bumped, but s_frame_open and [UTEST-FRAME] not yet
 * published) would count toward the new run's census while sitting outside
 * the parser's slice for it -- a guaranteed frame-count refusal manufactured
 * by the diagnosis itself.
 *
 * Emitted before the terminator, it is counted in that terminator's
 * `records=` like any other framed line, so nothing about frame
 * reconciliation changes; the host reads it as a SEMANTIC refusal of this
 * run -- records were claimed, a sequence number was drawn for each, and
 * they never reached the wire, so the run's capture stream has holes it can
 * name rather than corruption it has to guess at. */
static void u_frame_end(void)
{
    uint32_t n;
    uint64_t pending;
    uint64_t undelivered;

    /* Seal, THEN drain, THEN close, and the order is the whole protocol.
     *
     * An unsealed drain is a check-then-act: a new claim can be admitted
     * between its last zero reading and the terminator below. Sealed, the
     * outstanding count can only fall, so a zero reading is final.
     *
     * The close is what the drain's RESULT is not: the drain merely observed
     * a count, while the close re-reads it under the lock, writes off what
     * is still outstanding and ends the epoch in one act. Reporting the
     * drain's number instead would over-report a straggler that finished in
     * between, and writing off without ending the epoch would let that
     * straggler settle a claim already forgiven. */
    u_capture_seal();
    (void)u_capture_drain();
    /* One transition returns both facts: what was still outstanding, and what
     * this run settled without klog putting it on the wire. */
    pending = u_capture_close_epoch(&undelivered);
    /* Reported ahead of the pending record so a run that suffered both reads
     * cause-then-consequence on the wire: a record klog declined is a reason
     * the host's stream has a hole, and a reader hitting the refusal first
     * would go looking for a lost emitter instead. */
    if (undelivered) {
        utest_record_log(LOG_ERROR,
                         "[UTEST-CAPTURE-UNDELIVERED] run=%u count=%u",
                         (uint64_t)__atomic_load_n(&s_frame_run,
                                                   __ATOMIC_RELAXED),
                         (uint64_t)(uint32_t)undelivered);
    }
    if (pending) {
        /* Narrowed deliberately, and safe by construction: a run cannot admit
         * more than UTEST_CAPTURE_RUN_RECORD_BUDGET claims, so what is
         * outstanding at its close is bounded far below a uint32. The
         * counters are 64-bit because they span a BOOT; this field describes
         * ONE run, and the record's width assert budgets it as a uint32. */
        utest_record_log(LOG_WARN,
                         "[UTEST-CAPTURE-PENDING] run=%u pending=%u",
                         (uint64_t)__atomic_load_n(&s_frame_run,
                                                   __ATOMIC_RELAXED),
                         (uint64_t)(uint32_t)pending);
    }

    /* Loaded AFTER the pending report so the census counts it. */
    n = __atomic_load_n(&s_frame_records, __ATOMIC_RELAXED);

    utest_record_log(LOG_INFO,
                     "[UTEST-FRAME-END] run=%u records=%u spawned=%u",
                     (uint64_t)__atomic_load_n(&s_frame_run, __ATOMIC_RELAXED),
                     (uint64_t)n,
                     (uint64_t)__atomic_load_n(&s_capture_spawned,
                                               __ATOMIC_RELAXED));

    /* Cleared AFTER the terminator: that record is itself part of the slice
     * it closes. */
    __atomic_store_n(&s_frame_open, 0u, __ATOMIC_RELEASE);
}

/* ---- Source-level per-binary stdout capture -------------------------- *
 *
 * Every ring-3 write() byte that reaches this pipeline is escaped and
 * chunked into "[UTEST-CAPTURE] owner=<pid> wr=<id> seq=<n> len=<raw-bytes>
 * final=<0|1> <escaped>" records, riding the SAME frame-nonce mechanism
 * every other launcher record uses (utest_record_log), so the new record
 * type is non-forgeable for free and counted in the existing
 * records-reconciliation total.
 *
 * Escaping: `\` and `[` are always rewritten as "\xHH" (2 hex digits of
 * the escaped byte's value) -- backslash so the escape sequence stays
 * unambiguous, and '[' because every existing marker ([UTEST-XML],
 * [UTEST-JSON], [UTEST-FRAME], [UTEST-FRAME-END], [UTEST-RECORD-OVERFLOW],
 * and this one) shares that one prefix byte, so escaping it blocks a
 * captured payload from being mistaken for ANY record kind, current or
 * future, by a consumer that greps for markers rather than anchoring to
 * start-of-line. Every byte outside safe printable ASCII (< 0x20 or
 * >= 0x7F) is ALSO escaped -- this covers raw LF/CR (scripts/
 * utest-frame.py's canonical line splitter treats CRLF, bare LF, AND
 * bare CR as line terminators, so an unescaped CR would still split one
 * logical record into two parser-visible lines) and every non-ASCII or
 * invalid-UTF-8 byte (that same parser decodes each line with
 * `.decode("utf-8", errors="replace")`, which would otherwise silently
 * and irreversibly replace a raw high byte or a lone UTF-8 continuation
 * byte with U+FFFD before this section's own byte-exactness goal could
 * be honored). Only printable ASCII minus '\' and '[' -- 0x20-0x7E -- is
 * ever passed through unescaped. */
#define UTEST_CAPTURE_ESCAPE_EXPANSION 4u   /* worst case: 1 raw byte -> "\xHH" */

/* Fixed per-record literal cost, computed the same way UTEST_MAX_BINARY_NAME
 * is -- from the actual format string's shape, never hand-picked -- so a
 * future field added to the format and forgotten here fails the build
 * instead of silently overrunning UTEST_RECORD_LINE_MAX. */
#define UTEST_CAPTURE_FIXED                                                \
    (UTEST_LIT("[UTEST-CAPTURE] owner=") + UTEST_DIGITS_U32 +              \
     UTEST_LIT(" wr=") + UTEST_DIGITS_U32 +                                \
     UTEST_LIT(" seq=") + UTEST_DIGITS_U32 +                               \
     UTEST_LIT(" len=") + UTEST_DIGITS_U32 +                               \
     UTEST_LIT(" final=1 "))

/* Raw (pre-escape) bytes staged per chunk. Divided by the worst-case
 * expansion factor so the ESCAPED output can never overflow the record
 * even if every staged byte needs escaping. */
#define UTEST_CAPTURE_CHUNK_MAX                                            \
    (((UTEST_RECORD_LINE_MAX - 1u) - UTEST_CAPTURE_FIXED) /                \
     UTEST_CAPTURE_ESCAPE_EXPANSION)
_Static_assert(UTEST_CAPTURE_CHUNK_MAX > 0u &&
                   UTEST_CAPTURE_CHUNK_MAX <=
                       sizeof(((struct utest_capture_ctx *)0)->_buf),
               "the capture chunk size must fit the per-write staging "
               "buffer (test_usermode.h utest_capture_ctx._buf) and leave "
               "room for the record's own fixed literal cost");

/* [UTEST-CAPTURE-BEGIN]'s own fixed cost, same derivation style. It
 * carries the binary name at the SAME UTEST_MAX_BINARY_NAME bound every
 * other name-carrying record kind is proven against (the 7-way assert
 * above UTEST_MAX_BINARY_NAME's definition) -- asserted here rather than
 * folded into that MIN() because UTEST_CAPTURE_FIXED is not defined
 * until after UTEST_MAX_BINARY_NAME already is; this assert still
 * catches a future edit to the BEGIN format silently overrunning the
 * record before it ships. */
#define UTEST_CAPTURE_BEGIN_FIXED \
    (UTEST_LIT("[UTEST-CAPTURE-BEGIN] owner=") + UTEST_DIGITS_U32 + \
     UTEST_LIT(" chunk_max=") + UTEST_DIGITS_U32 + \
     UTEST_LIT(" name="))
_Static_assert(UTEST_CAPTURE_BEGIN_FIXED + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u,
               "a name at the derived bound must fit UTEST-CAPTURE-BEGIN "
               "too, not just the 7 kinds UTEST_MAX_BINARY_NAME was "
               "originally derived from");

/* [UTEST-CAPTURE-EXPECT]'s fixed cost. It carries the same owner+name pair
 * BEGIN does, so it is bounded by the same name allowance and asserted the
 * same way. */
#define UTEST_CAPTURE_EXPECT_FIXED \
    (UTEST_LIT("[UTEST-CAPTURE-EXPECT] owner=") + UTEST_DIGITS_U32 + \
     UTEST_LIT(" name="))
_Static_assert(UTEST_CAPTURE_EXPECT_FIXED + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u,
               "a name at the derived bound must fit UTEST-CAPTURE-EXPECT");

/* ---- Producer-side emission budget ---------------------------------- *
 *
 * The host bounds what reaches an ARTIFACT; nothing bounded what reaches
 * the WIRE. A ring-3 binary can emit unlimited perfectly well-formed
 * capture records, and every one costs serial time and log disk before
 * any host code runs, so a chatty or hostile binary could stretch a run's
 * wall-clock or fill the log device without ever producing a malformed
 * byte. These budgets stop the EMISSION; the host deadline stays the
 * host's to enforce, exactly as it is for the skip-record budget above.
 *
 * The budget is a RECORD budget, not a raw-byte one, and the distinction
 * is the whole derivation. A binary calling write() one byte at a time
 * pays a FULL record -- a ~350-byte klog line -- per raw byte, so a cap
 * expressed in raw bytes would let 64 KiB of payload cost 22 MB of wire.
 * Counting the records is what actually bounds the serial cost. */

/* Wire cost of ONE capture record.
 *
 * UTEST_RECORD_WIRE_MAX is deliberately NOT reused: it models the plain
 * LOG_ERROR path taken by the two skip markers, which fire AFTER the
 * launcher's color scope is cleared. Capture records are emitted DURING a
 * binary's run, while test_usermode_color_active() is true, and klog's
 * color-scope fallback (klog.c:1305-1308) sets test_color for ANY
 * subsystem in that window -- so these records take the test_color branch
 * instead: one level-ANSI badge, the truecolor sequence, and three
 * resets, where the skip path pays two level-ANSI and no truecolor.
 * Reusing the skip macro would prove a ceiling BELOW the traffic actually
 * emitted, which is a budget that does not bound. Taking the MAX over
 * both branches keeps the bound honest whichever branch klog picks for a
 * given record (the scope is not active for every possible emitter). */
#define KLOG_WIRE_TEST_COLOR_MAX 19u  /* "\033[38;2;132;178;233m"          */
#define UTEST_CAPTURE_WIRE_COLOR                                           \
    (UTEST_RECORD_LINE_MAX + KLOG_WIRE_TIMESTAMP_MAX +                     \
     KLOG_WIRE_CPUTAG_MAX + KLOG_WIRE_LEVEL_ANSI_MAX +                     \
     KLOG_WIRE_LEVEL_PREFIX + KLOG_WIRE_TEST_COLOR_MAX +                   \
     (3u * KLOG_WIRE_ANSI_RESET) + KLOG_WIRE_SUBSYSTEM_MAX +               \
     KLOG_WIRE_TRUNC_MARK + KLOG_WIRE_CRLF)
#define UTEST_CAPTURE_WIRE_MAX                                             \
    UTEST_MAX2(UTEST_CAPTURE_WIRE_COLOR, UTEST_RECORD_WIRE_MAX)
_Static_assert(UTEST_CAPTURE_WIRE_MAX >= UTEST_RECORD_WIRE_MAX,
               "the capture wire cost must dominate the plain-path cost or "
               "the budgets below bound less traffic than they advertise");

/* Raw captured bytes ONE binary may put on the wire. Anchored to what a
 * consumer keeps: scripts/utest-capture.py retains PER_BINARY_CAP = 64 KiB
 * per binary and declares everything past it truncated, so a producer
 * emitting more would spend serial time on bytes no artifact can ever
 * carry. Stated as a kernel-side allowance rather than read from the host
 * because a silently mirrored constant is its own failure mode -- see the
 * chunk-bound item this file's section 48 owns. */
#define UTEST_CAPTURE_OWNER_RAW_MAX (64u * 1024u)

/* Records needed to deliver that allowance at the worst-case chunk FILL,
 * rounded UP. Deriving from the fill (rather than picking a count) is what
 * keeps an honest binary from being clipped: a binary that fills every
 * chunk must be able to spend its whole raw allowance, and this is exactly
 * the record count that takes. A binary that wastes its records on one-byte
 * writes gets the same count and simply delivers less payload -- which is
 * the abuse the budget exists to bound, not a case to help finish. */
#define UTEST_CAPTURE_OWNER_RECORD_BUDGET                                  \
    ((UTEST_CAPTURE_OWNER_RAW_MAX + UTEST_CAPTURE_CHUNK_MAX - 1u) /        \
     UTEST_CAPTURE_CHUNK_MAX)

/* The serialized-byte ceiling that record count implies, declared so the
 * assert below has something to prove the derivation against -- the same
 * shape UTEST_SKIP_BURST_WIRE_MAX plays for the skip budget. The "+ 1u"
 * is the owner's own [UTEST-CAPTURE-OVER] marker: enforcement emits too,
 * and budgeting only the permitted records would leave the marker outside
 * the allowance the ceiling advertises. */
/* Raised from 512 KiB when the chunk record gained its `wr=` write
 * identity. The ceiling is a CONSEQUENCE of the derivation, not a cap
 * being widened to hide a violation: the raw allowance and the wire cost
 * per record are both unchanged, but a wider fixed field leaves less room
 * for payload (UTEST_CAPTURE_CHUNK_MAX 46 -> 42), so delivering the same
 * 64 KiB takes more records. Pinning the old number would have clipped an
 * honest binary's raw allowance instead -- the one outcome the record
 * budget's own derivation comment rules out. */
#define UTEST_CAPTURE_OWNER_WIRE_MAX (640u * 1024u)
_Static_assert((uint64_t)(UTEST_CAPTURE_OWNER_RECORD_BUDGET + 1u) *
                   UTEST_CAPTURE_WIRE_MAX <=
                   UTEST_CAPTURE_OWNER_WIRE_MAX,
               "a binary's worst-case capture burst plus its own overflow "
               "marker must fit the per-owner serialized-byte allowance");

/* Run-wide allowance, derived the same way from the aggregate a consumer
 * keeps (RUN_AGGREGATE_CAP = 1 MiB in scripts/utest-capture.py). N binaries
 * each individually under the per-owner cap can still sum past what the run
 * as a whole should ever put on the wire, which is the gap this closes. */
#define UTEST_CAPTURE_RUN_RAW_MAX (1024u * 1024u)
#define UTEST_CAPTURE_RUN_RECORD_BUDGET                                    \
    ((UTEST_CAPTURE_RUN_RAW_MAX + UTEST_CAPTURE_CHUNK_MAX - 1u) /          \
     UTEST_CAPTURE_CHUNK_MAX)

/* Every owner may pay one marker when the RUN budget trips, so the run
 * reservation is TASK_MAX markers rather than one -- the same reasoning
 * (and the same TASK_MAX caveat about slot reuse) as
 * UTEST_SKIP_REFUSAL_WIRE_MAX. */
#define UTEST_CAPTURE_RUN_WIRE_MAX (16u * 1024u * 1024u)
_Static_assert((uint64_t)UTEST_CAPTURE_RUN_RECORD_BUDGET *
                       UTEST_CAPTURE_WIRE_MAX +
                   ((uint64_t)TASK_MAX * UTEST_CAPTURE_WIRE_MAX) <=
                   UTEST_CAPTURE_RUN_WIRE_MAX,
               "the worst-case run-wide capture burst plus one overflow "
               "marker per task slot must fit the run allowance");

/* The floor, load-bearing exactly as the skip budget's is: a run-wide
 * ceiling below the per-binary one would turn an AGGREGATE abuse stop into
 * a per-binary truncation of the very first honest binary to run. Any
 * future retune that breaches it fails the build instead of silently
 * clipping. */
_Static_assert(UTEST_CAPTURE_RUN_RECORD_BUDGET >=
                   UTEST_CAPTURE_OWNER_RECORD_BUDGET,
               "the run-wide capture budget must cover a single binary's "
               "per-owner budget or one honest binary is clipped by the "
               "aggregate ceiling");

/* Both budgets ride the record's own uint32 seq field, whose digit width
 * the record bound reserves. */
_Static_assert(UTEST_CAPTURE_RUN_RECORD_BUDGET <= 9999999u,
               "the capture budgets bound the seq and limit fields' digits");

/* TERMINAL RECORDS -- the run-scoped allowance for [UTEST-CAPTURE-CUT] and
 * [UTEST-CAPTURE-ABANDON].
 *
 * These explain what happened to a write whose PAYLOAD stopped, so they must
 * survive the very latches that stopped it: a budget stop and the reap fence
 * both suppress chunks, and a terminal record suppressed alongside them would
 * leave exactly the silence it exists to break. They are therefore exempt
 * from the owner budget and the fence -- and that exemption is precisely why
 * they need a bound of their own.
 *
 * The bound is an EXPLICIT RUN COUNTER, not a slot-cardinality proof, and the
 * difference is load-bearing. THREAD_MAX bounds how many threads exist at
 * once, not how many ever live: kthread_create reuses a THREAD_FREE slot and
 * thread_reap_kernel_slot republishes dead slots, so a task can create a
 * thread, have it killed inside a sub-chunk write, join it, and reuse the
 * slot without limit. TASK_MAX * THREAD_MAX would have been a ceiling on
 * simultaneous open writes mistaken for a ceiling on records.
 *
 * The allowance is deliberately generous relative to any honest run (an
 * honest binary abandons NO writes) because exhausting it is a diagnostic
 * event, not a routine one. On exhaustion the run does not fall silent: one
 * reserved [UTEST-CAPTURE-TERMINAL-OVER] record is emitted and the host
 * refuses the run, so the wire stays bounded AND the loss stays visible --
 * fail-closed in both directions. */
#define UTEST_CAPTURE_TERMINAL_BUDGET ((uint32_t)TASK_MAX * (uint32_t)THREAD_MAX)

/* The "+ 1u" is the terminal-overflow record itself, on the same principle
 * the owner budget reserves its own [UTEST-CAPTURE-OVER] marker: enforcement
 * emits too, and budgeting only the permitted records would leave the marker
 * outside the allowance the ceiling advertises. */
#define UTEST_CAPTURE_TERMINAL_WIRE_MAX                                    \
    (((uint64_t)UTEST_CAPTURE_TERMINAL_BUDGET + 1u) * UTEST_CAPTURE_WIRE_MAX)

_Static_assert(UTEST_CAPTURE_TERMINAL_WIRE_MAX + UTEST_CAPTURE_RUN_WIRE_MAX <=
                   (uint64_t)64u * 1024u * 1024u,
               "the terminal allowance plus the run-wide capture allowance "
               "must stay inside the serial budget a run may spend");

/* A terminal record is a fixed-shape metadata line with no escaped payload,
 * so unlike a chunk it cannot approach the line cap through its variable
 * part -- but it still has to fit, and these asserts are what keep a later
 * field addition from silently truncating one.
 *
 * DERIVED from the same format literals the emitters use, exactly as
 * UTEST_CAPTURE_OVER_FIXED is: a hand-written ceiling proves only that some
 * number fits, and stays green while the record it is supposed to bound grows
 * past it. `reason=` uses the longest alternative the grammar admits. */
#define UTEST_CAPTURE_CUT_FIXED                                            \
    (UTEST_LIT("[UTEST-CAPTURE-CUT] owner=") + UTEST_DIGITS_U32 +          \
     UTEST_LIT(" wr=") + UTEST_DIGITS_U32)
_Static_assert(UTEST_CAPTURE_CUT_FIXED <= UTEST_RECORD_LINE_MAX - 1u,
               "the per-write cut record must fit the record wire cap -- a "
               "truncated cut would reach the host as an unexplained open "
               "write instead of the bounded stop it reports");

#define UTEST_CAPTURE_ABANDON_FIXED                                        \
    (UTEST_LIT("[UTEST-CAPTURE-ABANDON] owner=") + UTEST_DIGITS_U32 +      \
     UTEST_LIT(" task=") + UTEST_DIGITS_U32 +                              \
     UTEST_LIT(" thr=") + UTEST_DIGITS_U32 +                               \
     UTEST_LIT(" haswr=") + 1u +                                           \
     UTEST_LIT(" wr=") + UTEST_DIGITS_U32 +                                \
     UTEST_LIT(" reason=") + UTEST_LIT("killed"))
_Static_assert(UTEST_CAPTURE_ABANDON_FIXED <= UTEST_RECORD_LINE_MAX - 1u,
               "the abandoned-write record must fit the record wire cap -- a "
               "truncated abandon would lose the very evidence it carries");

#define UTEST_CAPTURE_TERMINAL_OVER_FIXED                                  \
    (UTEST_LIT("[UTEST-CAPTURE-TERMINAL-OVER] limit=") + UTEST_DIGITS_U32)
_Static_assert(UTEST_CAPTURE_TERMINAL_OVER_FIXED <= UTEST_RECORD_LINE_MAX - 1u,
               "the terminal-allowance marker must fit the record wire cap");

/* [UTEST-CAPTURE-OVER]'s fixed cost, derived from its own format literals
 * exactly like the two record kinds above. "scope=owner" is the longer of
 * the two scope tokens, so it is the worst case. */
#define UTEST_CAPTURE_OVER_FIXED                                           \
    (UTEST_LIT("[UTEST-CAPTURE-OVER] owner=") + UTEST_DIGITS_U32 +         \
     UTEST_LIT(" seq=") + UTEST_DIGITS_U32 +                               \
     UTEST_LIT(" scope=owner limit=") + UTEST_DIGITS_U32 +                 \
     UTEST_LIT(" charged=") + UTEST_DIGITS_U32)
_Static_assert(UTEST_CAPTURE_OVER_FIXED <= UTEST_RECORD_LINE_MAX - 1u,
               "the overflow marker must fit the record wire cap -- a "
               "truncated terminator would reach the host as corruption "
               "instead of the bounded stop it reports");

/* [UTEST-CAPTURE-PENDING]'s fixed cost, derived from its own format literals
 * for the same reason every record above is: the record has no variable
 * field, so a future edit that adds one has to come through here. Both
 * values are whole uint32s, so the worst case is two full digit runs. */
#define UTEST_CAPTURE_PENDING_FIXED                                        \
    (UTEST_LIT("[UTEST-CAPTURE-PENDING] run=") + UTEST_DIGITS_U32 +        \
     UTEST_LIT(" pending=") + UTEST_DIGITS_U32)
_Static_assert(UTEST_CAPTURE_PENDING_FIXED <= UTEST_RECORD_LINE_MAX - 1u,
               "the run-boundary pending report must fit the record wire "
               "cap -- a truncated one would reach the host as a malformed "
               "capture-family record and refuse the run for the wrong "
               "reason");

/* [UTEST-CAPTURE-UNDELIVERED]'s fixed cost, derived and asserted identically.
 * Both fields are whole uint32s on the wire for the same reason the pending
 * report's are: the counters behind them are 64-bit because they span a boot,
 * while the record describes ONE run, which cannot admit more than
 * UTEST_CAPTURE_RUN_RECORD_BUDGET claims. */
#define UTEST_CAPTURE_UNDELIVERED_FIXED                                    \
    (UTEST_LIT("[UTEST-CAPTURE-UNDELIVERED] run=") + UTEST_DIGITS_U32 +    \
     UTEST_LIT(" count=") + UTEST_DIGITS_U32)
_Static_assert(UTEST_CAPTURE_UNDELIVERED_FIXED <= UTEST_RECORD_LINE_MAX - 1u,
               "the undelivered-record report must fit the record wire cap "
               "-- a truncated one would reach the host as a malformed "
               "capture-family record and refuse the run for the wrong "
               "reason");

/* [UTEST-CAPTURE-UNREAPED]'s fixed cost, derived and asserted identically.
 * `live` counts task slots, so it cannot exceed TASK_MAX, but it is budgeted
 * as a full uint32 anyway: the bound belongs to the scheduler, not to this
 * record, and pinning the record's width to it would make a future TASK_MAX
 * change a silent truncation here instead of a build failure. */
#define UTEST_CAPTURE_UNREAPED_FIXED                                       \
    (UTEST_LIT("[UTEST-CAPTURE-UNREAPED] owner=") + UTEST_DIGITS_U32 +     \
     UTEST_LIT(" live=") + UTEST_DIGITS_U32)
_Static_assert(UTEST_CAPTURE_UNREAPED_FIXED <= UTEST_RECORD_LINE_MAX - 1u,
               "the unreaped-descendant report must fit the record wire cap");

/* [UTEST-CAPTURE-REAP-DEGRADED]'s fixed cost, budgeted identically. Both
 * counts are constructors in flight, bounded in practice by TASK_MAX threads
 * apiece, and both are budgeted as full uint32s anyway for the same reason
 * `live` above is: the bound belongs to the scheduler, not to this record. */
#define UTEST_CAPTURE_REAP_DEGRADED_FIXED                                  \
    (UTEST_LIT("[UTEST-CAPTURE-REAP-DEGRADED] owner=") + UTEST_DIGITS_U32 + \
     UTEST_LIT(" pending=") + UTEST_DIGITS_U32 +                            \
     UTEST_LIT(" stranded=") + UTEST_DIGITS_U32)
_Static_assert(UTEST_CAPTURE_REAP_DEGRADED_FIXED <= UTEST_RECORD_LINE_MAX - 1u,
               "the degraded-reap report must fit the record wire cap -- a "
               "truncated one would reach the host as a malformed "
               "capture-family record and refuse the run for the wrong "
               "reason");

/* The budget arithmetic, as a PURE function of the four state values --
 * no task, no globals, no lock. Split out so the state machine is unit-
 * testable on plain numbers (the caller below is the only thing that
 * needs a live owner), and so the ordering between the two budgets is
 * stated once in one place rather than implied by the call sequence.
 *
 * Order matters: the owner's own budget is checked FIRST so a single
 * abusive binary is reported against ITS OWN limit rather than against
 * whichever aggregate it happened to exhaust on the way there.
 *
 * The SEAL is checked before all of them, and its position is the whole
 * run-boundary fence. A sealed run has stopped admitting claims, so there
 * is nothing left to decide about budgets or latches: answering "which
 * limit did you hit" for a claim the run will not carry would attribute a
 * boundary refusal to a binary that did nothing wrong, and would latch that
 * binary's owner-stop flag for a reason the next run cannot see. */
static enum utest_capture_verdict u_capture_decide(uint32_t owner_seq,
                                               uint32_t run_records,
                                               int owner_stopped,
                                               int run_over,
                                               int run_sealed)
{
    if (run_sealed)
        return UTEST_CAP_SEALED;
    if (owner_stopped)
        return UTEST_CAP_DROP;
    if (owner_seq >= UTEST_CAPTURE_OWNER_RECORD_BUDGET)
        return UTEST_CAP_OVER_OWNER;
    if (run_over || run_records >= UTEST_CAPTURE_RUN_RECORD_BUDGET)
        return UTEST_CAP_OVER_RUN;
    return UTEST_CAP_EMIT;
}

/* The claim's state, as a snapshot the transition below operates on.
 *
 * `admitted` is the run's count of claims that RESERVED a record -- the
 * emitter has drawn its sequence number and will reach klog unless it dies
 * on the way. It is the drain's other half (see u_capture_pending): a claim
 * that consumes a sequence number and one that owes the wire a record are
 * the SAME event, so the reservation is not a second counter kept in step
 * with the sequence draw -- it is the same decision counted once more. */
struct u_capture_state {
    uint32_t owner_seq;
    uint32_t run_records;
    uint8_t  owner_stopped;
    uint8_t  run_over;
    uint8_t  run_sealed;
    /* uint64 to MATCH the lifetime counter it is loaded from and stored back
     * to. A uint32 here silently undid the widening: the global is 64-bit,
     * but every claim round-tripped it through 32 bits, so past UINT32_MAX
     * the high bits were discarded on the very next claim and `completed +
     * forgiven` would then exceed a truncated `admitted` forever -- pending
     * reads zero, the drain stops waiting, and nothing is ever reported
     * again. A field narrower than the state it carries is not a smaller
     * version of that state. */
    uint64_t admitted;
};

/* The claim's FULL state transition, pure over that snapshot: it decides,
 * mutates, and reports, with no task, no globals and no lock. The live
 * wrapper below keeps only the locking and the task-field load/store.
 *
 * Split out because the decision alone is not the part that can break the
 * host's invariants -- the MUTATION is. A regression that charged the run
 * on a terminator, forgot to latch, consumed a sequence number on DROP, or
 * reported the wrong charge would leave every decision-level assertion
 * green while breaking marker uniqueness and run reconciliation. Pure, it
 * is drivable from a unit test over synthetic state; inline in the locked
 * wrapper, it was reachable only from a live multi-threaded emitter this
 * kernel cannot spawn from a test. */
static enum utest_capture_verdict u_capture_apply(struct u_capture_state *st,
                                                  uint32_t *seq_out,
                                                  uint32_t *charged_out)
{
    enum utest_capture_verdict verdict =
        u_capture_decide(st->owner_seq, st->run_records, st->owner_stopped,
                         st->run_over, st->run_sealed);

    *seq_out = st->owner_seq;

    switch (verdict) {
    case UTEST_CAP_EMIT:
        st->run_records++;
        break;
    case UTEST_CAP_OVER_RUN:
        /* Latch the run stop so every LATER owner takes this branch on its
         * next claim instead of re-deciding against a count that only ever
         * grows. Idempotent by construction. */
        st->run_over = 1;
        st->owner_stopped = 1;
        break;
    case UTEST_CAP_OVER_OWNER:
        st->owner_stopped = 1;
        break;
    case UTEST_CAP_DROP:
        break;
    case UTEST_CAP_SEALED:
        /* Deliberately mutates NOTHING. The run has closed and the owner is
         * not at fault, so latching its stop flag would carry a boundary
         * refusal into a slot whose next reader is a different run. */
        break;
    }

    /* A terminator consumes a sequence number exactly as a chunk does --
     * that is what makes it the stream's highest and leaves no hole. A
     * DROP consumes nothing: it emits no record, so a number spent here
     * would be a hole the host reads as output lost on the wire. A SEALED
     * claim consumes nothing for the same reason and one more: the hole it
     * would leave belongs to a run that has already published its census. */
    if (verdict != UTEST_CAP_DROP && verdict != UTEST_CAP_SEALED) {
        st->owner_seq++;
        /* Reserved in the same breath as the sequence number, because they
         * are the same promise seen from two sides: a number is drawn, so a
         * record is owed. The drain's whole guarantee rests on this pairing
         * -- an increment placed anywhere else could describe a claim the
         * sequence stream does not, and the pending count would then bound
         * something other than the records still in flight. */
        st->admitted++;
    }

    *charged_out = st->run_records;
    return verdict;
}

/* Run-wide capture state. Guarded by s_capture_budget_lock, never touched
 * outside it, so the run charge cannot be claimed by one CPU while another
 * is deciding against a stale count. */
static DEFINE_SPINLOCK(s_capture_budget_lock);
static uint32_t s_capture_run_records;
static uint8_t  s_capture_run_over;

/* Has this run CLOSED ADMISSION? Set once, at the run boundary, before the
 * drain below waits on anything; cleared wholesale by the next frame's
 * budget reset.
 *
 * This is the run-boundary fence, and the reason it lives HERE rather than
 * in the launcher's task walk is that u_capture_claim is the one point every
 * emitter must pass. A per-task pass cannot be a fence: a write already past
 * test_usermode_capture_start has latched ctx->_active and ctx->_owner and
 * never consults the task's capture fields again, and the grace waits below
 * yield, so a descendant can fork a NEW capture-inheriting task while the
 * walk is in progress. Both shapes still have to claim, so both are stopped
 * by one flag read under the lock that already linearizes every claim. */
static uint8_t  s_capture_sealed;

/* The drain's counters, and they are deliberately MONOTONIC -- never reset,
 * only ever incremented.
 *
 * `admitted` counts claims that reserved a record; `completed` counts
 * records that reached klog. In-flight is their difference. A per-run reset
 * looks tidier and is a correctness bug: a claim admitted in run N whose
 * emitter is preempted past the boundary would decrement a counter that run
 * N+1 had already zeroed, and an unsigned difference underflows into a
 * pending count of four billion. Monotonic counters cannot be made to
 * disagree by a late arrival, whatever run it belonged to.
 *
 * `forgiven` is how a run still gets a pending count of its own without a
 * reset: a reservation the close below proved abandoned is added here, so
 * the NEXT run's pending starts at zero while the abandoned claim stays
 * permanently accounted rather than deleted.
 *
 * uint64 precisely BECAUSE they are lifetime counters. A run may admit up to
 * UTEST_CAPTURE_RUN_RECORD_BUDGET records, so a uint32 would need on the
 * order of 170,000 framed runs in one boot to wrap -- unreachable today, and
 * exactly the kind of bound that stops being unreachable without anyone
 * revisiting the type. At 64 bits the wrap is not a scenario to reason
 * about, which is worth more than the four bytes. */
/* Run-scoped terminal accounting, both guarded by s_capture_budget_lock for
 * the same reason the payload budget is: the charge, the ceiling test and the
 * one-shot overflow latch are ONE decision, and three individually atomic
 * fields would leave each access safe while the decision raced. */
static uint32_t s_capture_terminal_records;
static uint8_t  s_capture_terminal_over;

static uint64_t s_capture_admitted;
static uint64_t s_capture_completed;
static uint64_t s_capture_forgiven;

/* Records THIS run settled that klog declined to put on the wire. Run-scoped
 * and lock-guarded like its siblings, and separate from `completed` on
 * purpose: both outcomes discharge the same debt, so folding them together
 * would drain correctly while making a producer-side drop indistinguishable
 * from a delivered record locally. The debt and the delivery are two facts,
 * and the run boundary reports them as two. */
static uint64_t s_capture_undelivered;

/* SETTLEMENT HAPPENS AT KLOG'S DELIVERY POINT, not after the log call.
 *
 * A capture claim is credited by u_capture_receipt below, which klog invokes
 * from klog_emit the instant serial_write returns and before the framebuffer
 * and disk sinks (src/kernel/klog.c, contract on klog_receipt_fn in
 * include/kernel/klog.h). The credit is therefore simultaneous with the
 * record reaching the stream the host reconciles, and an emitter that dies
 * anywhere after the log call has already been credited for output the host
 * actually has.
 *
 * The earlier arrangement credited the claim AFTER utest_record_log returned,
 * which had no placement simultaneous with delivery and so had to CHOOSE a
 * direction for a killed emitter: crediting before the log called a lost
 * record delivered (a GREEN run for real data loss), crediting after called a
 * delivered record lost (a REFUSED run whose output the host has). It chose
 * the refusing side, deliberately, because a refused run gets looked at and a
 * silently green one does not. The receipt removes the choice rather than
 * re-making it: neither misreport is reachable through the emitter's own
 * death any more.
 *
 * WHAT REMAINS INEXACT, stated rather than implied. The receipt runs in the
 * emitter's context, so it is still one call away from the serial write, and
 * two things can happen in that gap. The emitter can be preempted and then
 * reaped, so the callback never runs at all; or the run boundary can close
 * the epoch on another CPU, after which u_capture_settle refuses the claim by
 * generation. Either way a record that IS on the wire is reported pending.
 *
 * The window is not eliminated, it is reduced from the entire remainder of
 * the emitter's life -- klog's framebuffer render, its synchronous disk
 * append and flush, the return path, and then the emitter's next statement --
 * to a single call, and reaching it now takes a preemption landing inside
 * that call AND lasting the whole UTEST_CAPTURE_DRAIN_MS. Closing it exactly
 * is not available on this side: publishing delivery inside serial's own
 * critical section would couple the UART driver to this test framework and
 * nest this lock under the serial lock, and the only other producer-local
 * linearization would hold this lock across serial output, which the kernel
 * forbids outright. It is owned by
 * the host reconciliation, which HAS the stream and can tell a claimed-but-
 * absent record from a claimed-and-present one.
 * Owned by the host-side "capture reconciliation on the default test path"
 * work, whose item is to reconcile a provisional pending report against the
 * authenticated record census before refusing the run. */

/* Records claimed but not yet on the wire, as a PURE function of the three
 * counters -- no lock, no globals, so the arithmetic the whole drain rests
 * on is drivable from a unit test over synthetic values.
 *
 * The `delivered >= admitted` floor is DEFENSIVE, and saying which it is
 * matters. With the close below advancing the generation in the same breath
 * as the write-off, a written-off claim's epoch is over and it can never
 * settle again, so deliveries can no longer exceed admissions on any real
 * path. The floor stays because the alternative failure is silent and
 * total: an unsigned subtraction in that state answers with a number near
 * UINT64_MAX and turns a fully delivered run into a drain that always times
 * out. */
static uint64_t u_capture_pending(uint64_t admitted, uint64_t completed,
                                  uint64_t forgiven)
{
    uint64_t delivered = completed + forgiven;

    if (delivered >= admitted)
        return 0;
    return admitted - delivered;
}

/* The run's accounting, as the snapshot the close below transitions. */
struct u_capture_boundary {
    uint64_t admitted;
    uint64_t completed;
    uint64_t forgiven;
    uint32_t generation;
};

/* CLOSE a run's capture epoch: write off whatever is still outstanding and
 * advance the generation, as ONE transition. Returns what was written off.
 *
 * The two halves are inseparable, and separating them was a real defect
 * rather than an aesthetic lapse. Writing off alone leaves the run's
 * generation current until the NEXT u_frame_begin, so an emitter resuming in
 * that window still passes its generation check and credits `completed` for
 * a claim already counted in `forgiven`. That double settlement then cancels
 * a LATER run's live reservation: admitted 2, completed 1, forgiven 1 reads
 * as nothing outstanding while the new run's record is genuinely still in
 * flight, so its drain returns immediately and its pending report never
 * names the record it lost. Advancing the generation in the same breath is
 * what makes the write-off FINAL -- a written-off claim's epoch is over, so
 * it can never settle against anything again.
 *
 * Pure over the snapshot for the same reason the claim's transition is: the
 * schedule that produces the collision (an emitter preempted across a run
 * boundary) is not constructible from a kernel test, but every state it
 * passes through is one call over plain numbers. */
static uint64_t u_capture_close(struct u_capture_boundary *b)
{
    uint64_t pending = u_capture_pending(b->admitted, b->completed,
                                         b->forgiven);

    b->forgiven += pending;
    b->generation++;
    return pending;
}

/* Which framed run the counters above describe. Bumped on every reset so a
 * claim can be checked against the run it was actually charged to.
 *
 * The claim is linearized under the lock but the EMISSION deliberately is
 * not -- klog's serial write is milliseconds, and holding a spinlock across
 * it is the hazard that split exists to avoid. That left a real window: a
 * fork descendant inherits capture ownership (task.c:2459) while the
 * launcher waits only on the top-level pid (u_run_one), so a descendant
 * outliving its binary could claim under one run's aggregate and reach klog
 * after the next run had reset it. Two shipped binaries fork (user/test/
 * test_process.c, user/test/test_faultinject.c), so the shape is reachable
 * rather than theoretical.
 *
 * The generation check is now the LAST of three lines rather than the only
 * one: the run seals admission so no further claim is created, drains the
 * claims already outstanding, and reaps the descendant tree -- and this
 * comparison catches only what survives all three, which is an emitter that
 * was already past its claim and could not be scheduled inside the drain
 * budget. It stays because that residual is real: no fence can make a
 * preempted task's klog write atomic with the frame rollover. */
static uint32_t s_capture_run_generation;

/* Monotonic source for owner stop epochs, guarded by s_capture_budget_lock.
 * It never resets across runs: an epoch only has to be COMPARABLE against the
 * snapshot an armed write took, and restarting the count would let a write
 * armed in an earlier run compare as older than a later run's stop and settle
 * as cut by a stop it never saw. Monotonic-forever is the cheap way to make
 * every comparison mean what it says. */
static uint32_t s_capture_stop_epoch_next;

/* How long the run boundary waits for outstanding claims to reach the wire.
 *
 * Sized against what it is actually waiting for -- one preempted emitter
 * finishing an escape pass and a klog line, on a kernel whose task dispatch
 * is a single global current_task, so the launcher yielding IS the only
 * thing that lets the emitter run. Generous enough that a healthy emitter is
 * never cut off, short enough that a dead one cannot stall the boundary: the
 * abandoned case is REPORTED, not waited out, so nothing is bought by
 * waiting longer. Deliberately below UTEST_KILL_GRACE_MS, which bounds a
 * whole cooperative process teardown rather than one record. */
#define UTEST_CAPTURE_DRAIN_MS 200u

/* The BOUND on how long one kill round waits for its signalled descendants
 * to die -- not the duration it waits. The same number the top-level wait
 * uses, because it is the same question asked of a smaller task: how long
 * does a cooperative SIGKILL teardown get before the forceful path runs. The
 * wait itself re-checks liveness and leaves early, exactly as that one does;
 * sharing the bound is not the same as sharing the shape. */
#define UTEST_CAPTURE_REAP_GRACE_MS UTEST_KILL_GRACE_MS

/* How many times the reap re-scans before it declares the descendant set
 * stable. A descendant can fork WHILE the walk yields, so a single pass is a
 * snapshot, not a fence -- the scan repeats until a full pass finds nothing
 * live. The cap exists so a fork bomb cannot hold the boundary open forever;
 * it is a bound on a pathological producer, not a tuning knob, which is why
 * exhausting it is reported rather than retried. */
#define UTEST_CAPTURE_REAP_ROUNDS 8u

/* The BOUND on the publication drain -- how long the reap waits for forks
 * that were ALREADY in flight when it latched the tree to finish their
 * constructors (task_utest_capture_fork_pending_count, task.c).
 *
 * It is a different question from the grace above and gets its own number.
 * The grace waits for a task to DIE, which is a whole cooperative teardown;
 * this waits for a task_fork already past its admission check to reach its
 * last line, which is a bounded stretch of straight-line kernel code with no
 * blocking call in it. On a kernel whose dispatch is a single global
 * current_task, the launcher yielding is the only thing that lets that
 * forker run at all, so the number has to cover scheduling latency rather
 * than any real work -- the same reason UTEST_CAPTURE_DRAIN_MS is sized the
 * way it is, and the same value for the same reason.
 *
 * Exhausting it is NOT a timeout that proceeds anyway. It is a distinct,
 * reported state: the census stops being exact the moment an in-flight fork
 * may still publish, so the reap says so on the wire
 * ([UTEST-CAPTURE-REAP-DEGRADED]) and the host refuses the run rather than
 * accepting a survivor count that quietly went back to being best-effort. */
#define UTEST_CAPTURE_REAP_DRAIN_MS 200u

/* Scopes the run budget to ONE framed run. Called from u_frame_begin
 * beside the s_frame_records reset -- see the rationale there. */
static void u_capture_budget_reset(void)
{
    uint64_t irq_flags;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
    s_capture_run_records = 0;
    s_capture_run_over = 0;
    /* The terminal allowance is per-RUN like the record budget beside it: a
     * binary that abandoned writes must not spend the next binary's evidence
     * allowance, and a run that exhausted it must not start the next one
     * already silent. */
    s_capture_terminal_records = 0;
    s_capture_terminal_over = 0;
    /* RELEASE, and atomic like the stop latch beside it: an armed capture
     * write snapshots this value LOCK-FREE at capture_start (the fast path
     * cannot afford an acquisition per write), so the write must not become
     * visible before the state that justifies it, and a plain store racing
     * that plain load would be a data race however benign the values. */
    __atomic_store_n(&s_capture_run_generation,
                     s_capture_run_generation + 1u, __ATOMIC_RELEASE);
    /* Admission REOPENS here, with the generation bump and under the same
     * lock, so no window exists in which the new run's generation is live
     * but its gate is still shut (or the reverse). The pair is what a claim
     * reads to decide whether it belongs to this run at all, and reading one
     * half of it from the previous run would be exactly the cross-run
     * accounting this section removes. */
    s_capture_sealed = 0;
    /* Run-scoped for exactly the reason the record counters are: the host
     * reconciles the expectation set against the count carried by THIS
     * run's terminator, so a counter spanning boot lifetime would measure a
     * different population than the check it has to satisfy.
     *
     * Reset INSIDE the lock with its siblings even though no claim path
     * touches it. The caller's comment states that this whole function runs
     * under the lock so a concurrent claim cannot straddle the reset;
     * leaving one of the four resets outside would make that guarantee
     * two-thirds true, and the next counter to join here would be added
     * against a promise the code no longer keeps. */
    __atomic_store_n(&s_capture_spawned, 0u, __ATOMIC_RELAXED);
    /* Run-scoped for the same reason, and reset here rather than at the
     * boundary that reads it: the read happens BEFORE this reset in a run's
     * life, so clearing it at the report would race the next run's first
     * declined record. */
    s_capture_undelivered = 0;
    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
}

/* Close admission for the current run. Idempotent, and deliberately so: the
 * boundary may seal, drain, discover a straggler and seal again without the
 * second call meaning anything different from the first. */
static void u_capture_seal(void)
{
    uint64_t irq_flags;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
    s_capture_sealed = 1;
    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
}

/* A reserved record reached klog. Called AFTER the emission returns, never
 * before: the counter's meaning is "records still owed to the wire", and
 * crediting one at claim time would make the drain wait for nothing while
 * the record it was waiting for was still unwritten.
 *
 * Takes the claim's GENERATION and settles only against the epoch that
 * admitted it. A claim whose epoch has closed was written off by the close
 * that ended it, so crediting it here would settle one reservation twice --
 * and the surplus credit does not vanish, it cancels some LATER run's live
 * reservation and lets that run's drain roll over with a record still in
 * flight. Refusing here is not discarding information: the loss was already
 * reported, against the run that actually suffered it. */
/* The settlement DECISION and mutation, pure over the two generations and
 * the counter -- no lock, no globals. Returns whether the claim settled.
 *
 * Split out because the generation guard is the load-bearing half and it is
 * the half a test cannot otherwise reach: driving it through the live
 * emitter needs a claim that survives a frame rollover, which no kernel test
 * can construct. Inline in the locked wrapper, a regression that deleted the
 * guard would leave every accounting assertion green -- a stale claim would
 * simply credit `completed` again, and the surplus would silently cancel a
 * LATER run's live reservation rather than failing anything locally. */
static int u_capture_settle(uint32_t claim_gen, uint32_t current_gen,
                            uint64_t *completed)
{
    if (claim_gen != current_gen)
        return 0;
    (*completed)++;
    return 1;
}

static void u_capture_complete(uint32_t gen)
{
    uint64_t irq_flags;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
    (void)u_capture_settle(gen, s_capture_run_generation, &s_capture_completed);
    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
}

/* The klog delivery receipt for a capture record: klog calls this at the
 * point the record has reached serial (delivered=1) or been declined by the
 * verbosity filter or rate limiter (delivered=0), with the claim's generation
 * as the cookie.
 *
 * Satisfies klog_receipt_fn's contract by construction (klog.h): one lock
 * acquisition and two counter updates -- no sleeping, no yielding, no
 * faulting, safe with interrupts disabled because the lock is irqsave, safe
 * concurrently because the lock is what serialises it, and it emits NO klog
 * record of any kind, which the contract forbids outright.
 *
 * BOTH outcomes settle. `delivered` says whether the host has the record, not
 * whether the run still owes it: a record klog declined is not going to
 * arrive later, so leaving the claim outstanding would make the drain wait
 * out its whole budget for something that no longer exists. The declined case
 * is counted separately so the boundary can name it instead of it hiding
 * inside a delivery count.
 *
 * The undelivered tally is bumped only when the settle SUCCEEDED, i.e. only
 * for a claim belonging to the live epoch. A stale claim's run has already
 * closed and already reported what it lost; adding to this run's tally would
 * bill one run for another's drop. */
/* The receipt's DECISION and mutation, pure over the two generations, the
 * delivered flag and the two counters -- no lock, no globals.
 *
 * Split out for the same reason u_capture_settle is: the interesting states
 * are the ones a live run cannot be driven into. A declined record needs the
 * verbosity filter to reject a tag the launcher publishes, and a stale
 * receipt needs an emitter preempted across a frame rollover; neither is a
 * schedule the kernel test surface can construct. Over plain numbers each is
 * one call.
 *
 * Returns whether the claim belonged to the LIVE epoch, which is also the
 * caller's authority to move the frame census. */
static int u_capture_receipt_apply(uint32_t claim_gen, uint32_t current_gen,
                                   int delivered, uint64_t *completed,
                                   uint64_t *undelivered)
{
    if (!u_capture_settle(claim_gen, current_gen, completed))
        return 0;
    if (!delivered)
        (*undelivered)++;
    return 1;
}

static void u_capture_receipt(uint64_t cookie, int delivered)
{
    uint64_t irq_flags;
    int      live;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
    live = u_capture_receipt_apply((uint32_t)cookie, s_capture_run_generation,
                                   delivered, &s_capture_completed,
                                   &s_capture_undelivered);
    /* The frame census, counted HERE rather than at the call site so
     * `records=` states what the host will actually find in the slice -- and
     * counted INSIDE this critical section, in the same act as the
     * settlement, which is what makes the drain cover it.
     *
     * The two have to move together or the terminator can publish a count
     * that disagrees with the wire. Sealed, the drain waits for every
     * admitted claim to settle, and a record still mid-emission is admitted
     * and not yet completed -- so a drain that reaches zero has, by
     * construction, already seen this increment. Split across two operations
     * the same conclusion holds only through a release/acquire argument about
     * a counter nothing else locks, which is the kind of reasoning that stops
     * being true the first time someone reorders this function.
     *
     * Guarded by the SETTLEMENT, not by `delivered` alone. A straggler whose
     * epoch closed while it sat between serial_write and this callback has
     * already been written off by that close, and its own run has already
     * published its census -- so there is no counter left that crediting it
     * could correct. What crediting the LIVE counter would do instead is
     * charge the NEXT run for a record that is not in its slice, and the host
     * reads `records=` as an exact count, so that run gets refused for a
     * stream it emitted correctly. Dropping the increment leaves the
     * straggler's own run short by one, which is the run that actually
     * suffered the anomaly and is already reporting it as pending; billing an
     * innocent successor is the strictly worse of the two. The window that
     * produces the straggler is owned by the host-side reconciliation. */
    if (live && delivered)
        __atomic_fetch_add(&s_frame_records, 1u, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
}


/* Close the current epoch: write off everything still outstanding, advance
 * the generation, and report what was written off. The transition itself is
 * u_capture_close; this is only the locking and the load/store around it, so
 * the wrapper cannot drift from the transition the unit tests drive.
 *
 * ONE caller -- the run boundary -- and the exclusivity is load-bearing.
 * Only the boundary can know a reservation is dead rather than slow, and
 * only the epoch that admitted it may write it off. */
static uint64_t u_capture_close_epoch(uint64_t *undelivered_out)
{
    struct u_capture_boundary b;
    uint64_t irq_flags;
    uint64_t pending;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
    /* Read INSIDE the close's critical section, not by a separate accessor
     * before it. A declined receipt landing between an earlier read and this
     * lock would be reported by neither run: this run has already published
     * its tally, and the next run's reset clears the counter before its own
     * boundary reads it. One transition, one lock, both facts. */
    if (undelivered_out)
        *undelivered_out = s_capture_undelivered;
    b.admitted   = s_capture_admitted;
    b.completed  = s_capture_completed;
    b.forgiven   = s_capture_forgiven;
    b.generation = s_capture_run_generation;

    pending = u_capture_close(&b);

    s_capture_forgiven = b.forgiven;
    __atomic_store_n(&s_capture_run_generation, b.generation,
                     __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);

    return pending;
}

/* Records this run has claimed but not yet put on the wire. */
static uint64_t u_capture_inflight(void)
{
    uint64_t irq_flags;
    uint64_t admitted, completed, forgiven;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
    admitted  = s_capture_admitted;
    completed = s_capture_completed;
    forgiven  = s_capture_forgiven;
    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);

    return u_capture_pending(admitted, completed, forgiven);
}

/* Wait, bounded, for every claim admitted by this run to reach the wire, and
 * return what was still outstanding when the wait ended.
 *
 * The caller must have SEALED first, and the order is the whole protocol: a
 * drain over an open run is a check-then-act with nothing stopping a new
 * claim from being admitted between the last zero reading and the caller's
 * next action, which is the same TOCTOU shape the fence exists to remove.
 * Sealed first, the count can only fall, so a single zero reading is final.
 *
 * Yielding is what makes progress possible at all: task dispatch here uses a
 * single global current_task, so the outstanding emitter cannot run until
 * this launcher thread gives up the CPU. */
static int u_capture_drained(void *ctx)
{
    (void)ctx;
    return u_capture_inflight() == 0;
}

static uint64_t u_capture_drain(void)
{
    int ended = u_bounded_wait(u_capture_drained, (void *)0,
                               UTEST_CAPTURE_DRAIN_MS, &u_wait_ops_live);
    uint64_t pending;

    /* Re-read rather than carry a count out of the loop: the predicate answers
     * a yes/no, and the caller needs the NUMBER still outstanding at the moment
     * the wait ended. */
    pending = u_capture_inflight();
    /* A stall needs no separate conservative action here, and that is a
     * property of what this function returns rather than an omission. The
     * caller acts on the outstanding count, which a stalled wait leaves
     * non-zero by construction -- the condition was never met -- so the run is
     * already reported as having claims that never reached the wire. The stall
     * record adds WHY they did not. */
    if (ended == UTEST_WAIT_STALLED)
        u_wait_report_stall("capture-drain", pending);
    return pending;
}

/* Is `t` a capture-owning DESCENDANT of `owner_pid` -- a task that inherited
 * this binary's capture channel through fork() rather than the binary
 * itself?
 *
 * Pure over a task slot for the same reason u_capture_armed_for is: the only
 * way to exercise the branches is a live fork tree, which a kernel test
 * cannot build, so without a seam the owner-exclusion and the
 * different-owner rejection would never execute.
 *
 * Excluding the owner is not bookkeeping tidiness. The owner is the pid the
 * launcher has already waited on and is about to clean up itself; a reap
 * that included it would kill and free a task out from under u_run_one's own
 * teardown, and would do it while the launcher still holds pointers into
 * that slot's report and leak snapshots. */
static int u_capture_descendant_of(const struct task *t, uint32_t owner_pid)
{
    return t != (const struct task *)0 &&
           t->utest_capture_active != 0 &&
           t->utest_capture_owner_pid == owner_pid &&
           t->pid != owner_pid;
}

/* Fence and reap one binary's capture-owning descendant tree.
 *
 * Called once per binary, after the launcher's wait on the TOP-LEVEL pid has
 * returned and before that pid is cleaned up. The wait is what makes this
 * necessary: it observes one task, while fork() hands the same capture
 * channel to every descendant (task.c, TASK_UTEST_CAPTURE_INHERIT), so a
 * descendant can outlive the binary it belongs to and keep emitting into
 * whatever run is current when it next reaches klog.
 *
 * FOUR mechanisms, and the ORDER between them is the design:
 *
 *   1. Latch the OWNER's stop flag first. The claim path reads that latch
 *      from the owner's slot regardless of which task in the tree is
 *      emitting (u_capture_claim takes `owner`), so one store fences the
 *      whole tree at once -- including a descendant already past
 *      test_usermode_capture_start, whose ctx has latched _active and
 *      _owner and will never look at its own task fields again. Every later
 *      claim from any of them decides DROP: no sequence number, no record,
 *      bytes swallowed rather than handed to the unframed serial fallback.
 *      Doing this AFTER the kills would leave exactly the window the fence
 *      exists to remove, because the kills below yield.
 *   2. Latch PUBLICATION, which is step 1 for existence rather than for the
 *      wire (task_utest_capture_reap_latch, task.c). While it is set, a fork
 *      by any member of this tree is refused at admission, before it
 *      allocates anything -- so the tree stops growing under the walk. This
 *      is what steps 3 and 4 rest on; without it neither of them can be more
 *      than best-effort, because task_fork publishes with num_tasks++ under
 *      a protocol this walk does not participate in.
 *   3. DRAIN the forks that were already in flight when step 2 landed. They
 *      were admitted legitimately and will publish, so the reap has to wait
 *      for them -- and has to do it BEFORE the first kill, because after
 *      that a forker can be terminated inside its own constructor and a
 *      later cleanup would free a slot that is still being written.
 *   4. Then kill, to a FIXED POINT. The fixed point is now reached in one or
 *      two rounds rather than defended against a producer racing it: nothing
 *      new can appear. The round cap survives as a backstop, not as the
 *      mechanism.
 *
 * Reports the number of descendants still live after the last round -- zero
 * for every honest binary -- and, separately, whether that number is EXACT.
 * It is exact whenever the drain reached zero, which is every case except a
 * fork that outlasted UTEST_CAPTURE_REAP_DRAIN_MS; there the reap says so
 * rather than presenting a snapshot as a statement.
 *
 * Reaping is not only about the wire. task_terminate_remote marks a task
 * DEAD and runs the shared death-transition teardown, but stacks, CR3,
 * PEB/TEB and the handle table are freed by task_cleanup at the off-CPU reap
 * barrier -- and the launcher cleaned up only the top-level pid, so repeated
 * runs with outliving descendants leaked kernel resources whose framing was
 * otherwise perfectly healthy. */

/* The loop's world is a PARAMETER, not the kernel it happens to run on.
 * struct utest_reap_ops / struct utest_reap_result (include/kernel/test/
 * test_usermode.h) carry the full rationale and are declared there because
 * the assertions that drive a synthetic tree need them too; what follows is
 * the LIVE binding of that world. */

/* Latch the owner's wire fence. Kept as an op rather than inlined so a test
 * can assert the ORDER that matters -- the fence is set before any kill, so a
 * descendant reached in a later round was already unable to emit. */
static void u_reap_live_fence(struct task *owner)
{
    uint64_t irq_flags;

    /* MONOTONICITY COMES FROM THE STORES, NOT FROM THE LOCK, and the
     * distinction matters: a reader who believes the lock is the protection
     * may later "simplify" the claim path's set-only store back into an
     * unconditional store-back of its snapshot, which is exactly the lost
     * update that unfences the whole tree. Both writers store only 1 -- here
     * and in u_capture_claim -- so no interleaving of the two can produce a
     * 1 -> 0 transition, with or without this lock. The only 0-store is the
     * slot constructor, which runs before the task is published.
     *
     * The lock is still taken, because the claim path reads this field
     * inside it and keeping the write under the same lock costs nothing on a
     * once-per-binary path and leaves no reader observing a value the lock
     * was supposed to order. */
    {
        spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
        __atomic_store_n(&owner->utest_capture_stopped, 1u, __ATOMIC_RELEASE);
        /* The fence is latched ALONGSIDE the stop, not instead of it: the
         * stop latch is what the claim fast path already reads to suppress
         * payload, and that suppression is still exactly what the fence
         * wants. The separate flag records WHY, which the stop latch alone
         * could not say -- and the terminal path is the only reader that
         * needs the distinction, to avoid handing a write silenced by an
         * end-of-binary teardown a CUT record claiming it hit a budget it
         * never reached. Deliberately does NOT stamp a stop epoch: a fence is
         * not a budget stop, and stamping one would make every write open at
         * teardown settle as budget-cut. */
        __atomic_store_n(&owner->utest_capture_fenced, 1u, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
    }
}

static void u_reap_live_kill(uint32_t pid)
{
    (void)signal_send(pid, SIGKILL);
}

static void u_reap_live_terminate(struct task *t)
{
    task_terminate_remote(t, TASK_EXIT_UTEST_REAPED);
}

/* DESIGNATED initializers, not positional. `latch`, `kill` and `cleanup` are
 * all void(*)(uint32_t), so a reordering of the struct -- or of this list --
 * would still compile and would silently bind task_cleanup where the kill
 * belongs, freeing task slots during the kill round. There is no diagnostic
 * for that; naming each member is the only thing that makes the binding
 * checkable. */
static const struct utest_reap_ops u_reap_ops_live = {
    .get_by_pid   = task_get_by_pid,
    .fence        = u_reap_live_fence,
    .latch        = task_utest_capture_reap_latch,
    .fork_pending = task_utest_capture_fork_pending_count,
    .kill         = u_reap_live_kill,
    .terminate    = u_reap_live_terminate,
    .cleanup      = task_cleanup,
    .now_ms       = u_uptime_ms,
    .now_ticks    = mono_tsc_raw,
    .ticks_per_ms = u_wait_ticks_per_ms_live,
    .wait         = yield,
};

/* Wait out the fork constructors that were already admitted for this owner.
 *
 * Returns the count still LIVE when it gave up (0 on a clean drain) and, via
 * `stranded`, the registrations on tasks that died mid-constructor -- those
 * can never reach their release, so waiting on them would burn the whole
 * bound and change nothing.
 *
 * The deadline is a bound, not a duration: it re-checks the condition first
 * and leaves the moment it is met, the same shape the grace wait uses. */
/* Project the wait's world out of the reap's. Both waits below run over the
 * SAME seams the rest of the reap uses, so a synthetic tree that freezes the
 * reap's clock freezes its waits with it. */
static struct utest_wait_ops u_reap_wait_ops(const struct utest_reap_ops *ops)
{
    struct utest_wait_ops w;

    w.now_ms       = ops->now_ms;
    w.now_ticks    = ops->now_ticks;
    w.ticks_per_ms = ops->ticks_per_ms;
    w.wait         = ops->wait;
    return w;
}

struct u_reap_wait_ctx {
    const struct utest_reap_ops *ops;
    uint32_t owner_pid;
    uint32_t stranded;
};

static int u_reap_forks_settled(void *ctx)
{
    struct u_reap_wait_ctx *c = (struct u_reap_wait_ctx *)ctx;

    return c->ops->fork_pending(c->owner_pid, &c->stranded) == 0;
}

static uint32_t u_reap_drain(uint32_t owner_pid,
                             const struct utest_reap_ops *ops,
                             uint32_t *stranded, int *stalled)
{
    struct u_reap_wait_ctx c;
    struct utest_wait_ops w = u_reap_wait_ops(ops);
    int ended;

    c.ops       = ops;
    c.owner_pid = owner_pid;
    c.stranded  = 0;

    ended = u_bounded_wait(u_reap_forks_settled, &c,
                           UTEST_CAPTURE_REAP_DRAIN_MS, &w);
    /* THE STALL IS CARRIED OUT, not inferred from the count, and the
     * difference is a real hole rather than a stylistic one. A wait that gave
     * up on a stopped clock had a non-zero count at its last probe, but the
     * re-read below can still return zero if the constructor happened to
     * finish in between -- and a zero would then be presented as a settled
     * drain by a launcher that had just admitted it cannot measure time. The
     * caller uses this flag to refuse exactness regardless of the number.
     *
     * SET-ONLY, never cleared, and the asymmetry is the whole contract. The
     * reap drains TWICE over one flag, so a plain assignment would let the
     * second attempt ERASE a stall the first one suffered: the retry can settle
     * on its own condition even while the clock is dead, and the reap would
     * then report a clean run for a launcher that had already lost its clock --
     * no record, no suite abort, and every later bound in the run measured
     * against nothing. The caller zeroes it once, before the first drain. */
    if (ended == UTEST_WAIT_STALLED)
        *stalled = 1;
    return ops->fork_pending(owner_pid, stranded);
}

/* The kill grace's condition: no capture-owning descendant of this owner is
 * still alive. */
static int u_reap_tree_quiet(void *ctx)
{
    struct u_reap_wait_ctx *c = (struct u_reap_wait_ctx *)ctx;
    struct task *t;
    uint32_t pid;

    for (pid = 0; (t = c->ops->get_by_pid(pid)) != (struct task *)0; pid++) {
        if (u_capture_descendant_of(t, c->owner_pid) && t->state != TASK_DEAD)
            return 0;
    }
    return 1;
}

/* Fence and reap one binary's capture-owning descendant tree, over an
 * injectable world. See u_capture_reap_tree below for the live entry point
 * and struct utest_reap_ops above for why the world is a parameter. */
static void u_capture_reap_tree_ops(uint32_t owner_pid,
                                    const struct utest_reap_ops *ops,
                                    struct utest_reap_result *out)
{
    struct task *owner = ops->get_by_pid(owner_pid);
    uint32_t round;
    uint32_t live = 0;
    uint32_t pid;
    struct task *t;
    uint32_t pending = 0;
    uint32_t stranded = 0;
    struct utest_wait_ops wait_ops = u_reap_wait_ops(ops);
    struct u_reap_wait_ctx wait_ctx;
    int drain_stalled = 0;
    int grace_stalled = 0;

    wait_ctx.ops       = ops;
    wait_ctx.owner_pid = owner_pid;
    wait_ctx.stranded  = 0;

    out->live = 0;
    out->rounds = 0;
    out->drain_pending = 0;
    out->drain_stranded = 0;
    out->exact = 0;
    out->stalled = 0;

    if (owner)
        ops->fence(owner);

    /* PUBLICATION LATCH, and it is a different fence from the one above.
     *
     * The fence stops the tree TALKING; this stops it GROWING. Until it
     * existed the reap could only ever take a census: it walks the task table
     * while task_fork publishes into that table with num_tasks++ under a
     * protocol the reap did not participate in, so a descendant published in
     * the instant after the terminal walk passed its slot was missed
     * entirely -- and missed while `live == 0` suppressed the report, which
     * is the worst shape the miss could take. Latching first means every
     * later fork by any member of this tree is refused at admission
     * (task_utest_fork_admit, task.c) before it allocates anything.
     *
     * Ordered before the drain, not after: draining first would be draining
     * against a tree that is still allowed to add to itself, which never
     * terminates on a producer that forks in a loop. */
    ops->latch(owner_pid);

    /* DRAIN, and it runs BEFORE the first kill rather than before the census.
     *
     * The forks it waits for were admitted before the latch went up, so they
     * are entitled to publish and the reap must see them. Waiting here rather
     * than just before the census is what keeps that safe: after the kills
     * begin, a forker can be terminated in the middle of its own constructor,
     * and a reap that then cleaned up the half-built child would be freeing a
     * slot another CPU is still writing. Before any kill, every in-flight
     * fork is guaranteed to finish on its own.
     *
     * The deadline is a bound, not a duration -- like the grace below, this
     * re-checks the condition and leaves the moment it is met. */
    pending = u_reap_drain(owner_pid, ops, &stranded, &drain_stalled);
    /* A SECOND drain when the first did not settle, and it is not a
     * superstitious retry. u_reap_drain yields between probes, so an expired
     * first drain means one full bound of yielding did not let the preempted
     * constructor finish -- but the launcher is the only thing that can give
     * it CPU on single-global-current_task dispatch, so a second bound is
     * genuinely more of the one resource that helps. It can only improve the
     * answer: the latch has been up since before the first drain, so nothing
     * new can have been admitted in between. */
    out->drain_pending = pending;
    out->drain_stranded = stranded;
    /* EXACTNESS IS RECORDED HERE AND NOWHERE ELSE. Past this point the tree
     * is frozen if and only if NOTHING is outstanding: no new fork can be
     * admitted (the latch), none is still running (the drain), and none was
     * left registered on a task that died inside its constructor.
     *
     * The third term is not the same as the second and cannot be waited out.
     * A constructor whose task is already DEAD will never reach its release,
     * so the drain rightly stops waiting on it -- but it may still publish,
     * which is exactly the thing the interlock exists to rule out. Waiting
     * would burn the bound and prove nothing; ignoring it would launder an
     * unfrozen tree into an exact census. Reporting it is the only honest
     * option left.
     *
     * Either way the KILL ROUNDS below still run, and the asymmetry with the
     * cleanup pass is load-bearing rather than an oversight.
     *
     * Two independent reviews argued the rounds should be gated on this flag
     * too: the victim of a kill can be the very task suspended inside
     * task_fork, and terminating it strands the half-built child it had
     * already allocated. The hazard is real and is NOT closed here.
     *
     * The kills stay unconditional because they are the only step that
     * restores progress -- capture descendants are the forking workload the
     * reap exists to stop, and a launcher that declines to stop them has no
     * other lever. Weighed against a bounded leak of one half-built child,
     * halting the reap is the worse trade. The FREE is the opposite case and
     * is gated below: it is irreversible and it is what a live constructor
     * would still be reading.
     *
     * The precondition stated above the drain is therefore conditional, not
     * an invariant. Closing the residual properly is the same
     * unquiesced-constructor question as the parked boot-poisoning item in
     * this section, and needs the same boot-policy decision. */
    out->exact = (pending == 0 && stranded == 0 && !drain_stalled) ? 1u : 0u;

    for (round = 0; round < UTEST_CAPTURE_REAP_ROUNDS; round++) {
        out->rounds = round + 1u;
        live = 0;
        for (pid = 0; (t = ops->get_by_pid(pid)) != (struct task *)0; pid++) {
            if (!u_capture_descendant_of(t, owner_pid) ||
                t->state == TASK_DEAD)
                continue;
            /* Cooperative first, exactly as the top-level wait does: a
             * descendant that reaches a kernel entry runs its own unwind and
             * sets its own exit status. */
            ops->kill(pid);
            live++;
        }
        if (live == 0)
            break;

        /* Wait for the signalled descendants to actually die, and STOP as
         * soon as they have. The deadline is the bound, not the duration:
         * an unconditional sleep would burn the full grace on every round
         * even when every descendant died on the first yield, which at
         * UTEST_CAPTURE_REAP_ROUNDS rounds is seconds of wall clock added
         * to each binary for nothing. This is the shape the top-level wait
         * already uses -- re-check the condition, then the clock. */
        if (u_bounded_wait(u_reap_tree_quiet, &wait_ctx,
                           UTEST_CAPTURE_REAP_GRACE_MS,
                           &wait_ops) == UTEST_WAIT_STALLED)
            grace_stalled = 1;

        /* Forceful fallback for whatever the signal could not land on -- a
         * ring-3 spinloop never runs signal_check. Safe here for the same
         * reason the top-level path states: task dispatch uses a single
         * global current_task, so no other CPU can be dispatching these
         * tasks while this launcher thread is the one running. */
        for (pid = 0; (t = ops->get_by_pid(pid)) != (struct task *)0; pid++) {
            if (!u_capture_descendant_of(t, owner_pid) ||
                t->state == TASK_DEAD)
                continue;
            ops->terminate(t);
        }

        /* STOP AFTER the terminate pass, not instead of it. A stalled grace
         * means the clock died mid-reap, so every later round would spend its
         * own watchdog budget learning the same thing -- but the kills and the
         * force-terminate above are the only things that restore progress, so
         * this round finishes them first and the loop ends after. */
        if (grace_stalled)
            break;
    }

    /* FINAL CENSUS, and it is not the same number the loop was tracking.
     * `live` above counts what each KILL pass found, so it describes the
     * state before that pass's kills and grace -- reporting it would name
     * survivors that are now dead. Re-counting once, after the last round, is
     * what makes the returned number describe the tree rather than the loop.
     *
     * It is now also EXACT rather than best-effort, and that is the interlock
     * above rather than anything in this walk. Before the latch and the
     * drain, a descendant published just after this pass walked past its slot
     * was missed entirely -- and missed silently, because `live == 0`
     * suppressed the report. With publication refused and no fork in flight,
     * the set this walk enumerates is the set that exists, which is why
     * `exact` is carried out to the caller alongside the number: on the one
     * path where the drain gave up, this reverts to being a snapshot and the
     * caller must not present it as anything more. */
    /* DRAIN A SECOND TIME, after the rounds rather than before them.
     *
     * The POSITION is load-bearing and was established by measurement, not by
     * argument. Hoisting this to sit immediately after the first drain reads
     * tidier -- both drains then bracket nothing -- and it hangs the boot:
     * the full suite stops producing output partway through and never emits a
     * summary. Reverting the hoist alone makes it green again. The mechanism
     * was not isolated further; what is established is that the second ask
     * belongs AFTER the rounds, where the tree has already been quiesced,
     * and not in front of them.
     *
     * It can only improve the answer: the latch has been up since before the
     * first drain, so nothing new can have been admitted in between. */
    if (!out->exact) {
        pending = u_reap_drain(owner_pid, ops, &stranded, &drain_stalled);
        out->drain_pending = pending;
        out->drain_stranded = stranded;
        /* RECOVERED EXACTNESS IS DELIBERATELY NOT GRANTED. A retry that
         * settles proves the tree is quiesced, so the census here really is a
         * statement -- and it is still refused, because `exact` is not only a
         * description: it is the gate on the irreversible free. Once a wait in
         * this reap has stalled, the run is being abandoned anyway (the sticky
         * stall aborts the suite), so granting exactness would buy nothing but
         * the one step that cannot be undone, taken by a launcher that has
         * just reported it can no longer measure time. */
        out->exact = (pending == 0 && stranded == 0 && !drain_stalled)
                         ? 1u : 0u;
    }

    /* A STALLED GRACE REFUSES EXACTNESS TOO, and for a reason the drain flags
     * do not already cover. Exactness is a claim that the census below
     * enumerates the tree that exists; the grace is what quiesces that tree
     * before the census runs, so a grace that gave up on a stopped clock leaves
     * the walk describing a tree that was still moving. Withholding exactness
     * withholds the irreversible free and refuses the run host-side, which is
     * the same conservative answer the drain's own failure gets. */
    if (grace_stalled)
        out->exact = 0;

    out->stalled = (drain_stalled || grace_stalled) ? 1u : 0u;

    live = 0;
    for (pid = 0; (t = ops->get_by_pid(pid)) != (struct task *)0; pid++) {
        if (u_capture_descendant_of(t, owner_pid) && t->state != TASK_DEAD)
            live++;
    }
    out->live = live;

    /* WITHHOLD THE FREE when the drain never settled -- and only the free.
     *
     * Freeing is the one step that cannot be taken back. A constructor still
     * running holds pointers into slots this pass would release -- its own,
     * its parent's -- so cleaning up underneath it trades a bounded leak for
     * a use-after-free, in the exact situation where the reap has already
     * admitted it does not know what is running. The run is refused
     * host-side either way ([UTEST-CAPTURE-REAP-DEGRADED]) and the caller
     * ends the suite, so nothing is salvaged by tidying and something
     * irreversible is risked. */
    if (!out->exact)
        return;

    /* Reap highest pid first. Task slots are allocated monotonically, so a
     * child always holds a higher pid than the parent it forked from, and
     * descending order is therefore children-first -- the direction a tree
     * teardown has to run in for a parent's release not to precede a child
     * that still refers to it. */
    for (pid = 0; ops->get_by_pid(pid) != (struct task *)0; pid++)
        ;
    while (pid-- > 0) {
        t = ops->get_by_pid(pid);
        if (u_capture_descendant_of(t, owner_pid) && t->state == TASK_DEAD)
            ops->cleanup(pid);
    }
}

/* The live entry point: the same reap, bound to the real kernel. */
static void u_capture_reap_tree(uint32_t owner_pid, struct utest_reap_result *out)
{
    u_capture_reap_tree_ops(owner_pid, &u_reap_ops_live, out);
    /* The reporting lives HERE and not in the loop above, which is the only
     * place that knows the world it just ran over was the real kernel. */
    if (out->stalled)
        u_wait_report_stall("capture-reap", (uint64_t)owner_pid);
}

/* Announce that this run SPAWNED a capture-owning binary.
 *
 * The [UTEST-CAPTURE-BEGIN] binding alone cannot carry this: it is emitted
 * by the arming path itself (task_create_internal), so a producer
 * regression that stops arming capture removes the announcement too, and
 * the host then sees an EMPTY capture model that both artifacts accept as
 * ordinary output-free testcases -- the frame count reconciles because
 * those records were never produced. Inferring the expectation host-side
 * is not available either: it would refuse legitimate launch-failure rows,
 * which have no capture channel by construction and are not a defect.
 *
 * So the expectation is published from the LAUNCHER, on the spawn path,
 * and the host requires every expected owner to have a BEGIN. */
static void u_capture_expect(uint32_t owner_pid, const char *name)
{
    utest_record_log(LOG_INFO, "[UTEST-CAPTURE-EXPECT] owner=%u name=%s",
                     (uint64_t)owner_pid, name);
}

/* Is `child` really the armed capture owner for `pid`?
 *
 * The arming postcondition, as a PURE predicate over a task slot -- no
 * globals, no task lookup, no emission -- so every branch is drivable from a
 * scratch struct instead of only by a healthy live spawn. That matters
 * because this is the safeguard for a producer regression: an end-to-end run
 * always hands it a correctly armed child, so without a seam the NULL,
 * inactive and wrong-owner branches would never execute and the guard could
 * rot without a single assertion failing.
 *
 * All three conditions are load-bearing. `utest_capture_active` alone is the
 * flag the write path gates on, and `utest_capture_owner_pid` is the slot the
 * sequence draw targets: a task armed with someone else's owner pid would
 * capture into a channel bound to a different binary. */
static int u_capture_armed_for(const struct task *child, uint32_t pid)
{
    return child != (const struct task *)0 &&
           child->utest_capture_active != 0 &&
           child->utest_capture_owner_pid == pid;
}

/* Is the run this claim was charged against still the current one?
 *
 * Checked immediately before emitting, so a record claimed in an earlier run
 * is DROPPED rather than attributed to the new one. Dropping is strictly
 * better than emitting: the previous run's slice is already closed by its
 * frame-end record, and the new slice carries no [UTEST-CAPTURE-BEGIN]
 * binding that owner, so the record could only ever have reconciled as
 * capture_unbound_owner -- a refusal caused by a stale emitter rather than
 * by anything the new run did.
 *
 * THIS CHECK ALONE NARROWS THE WINDOW RATHER THAN CLOSING IT: the comparison
 * happens under the lock but the emission does not, so a stale emitter can
 * pass here, be preempted, and reach klog after u_frame_begin has moved the
 * generation -- check-then-log, with a genuine TOCTOU between.
 *
 * It is no longer alone. Both fixes the reviews named for that TOCTOU now
 * exist, in this file, and this comparison runs LAST of three: the run seals
 * admission (u_capture_seal) so no further claim is created, drains the
 * claims already outstanding (u_capture_drain) and ends the epoch in the
 * same transition that writes them off (u_capture_close), and the launcher
 * reaps each binary's captured descendant tree before that binary's own
 * cleanup (u_capture_reap_tree). What reaches this check is therefore only
 * what survived all three -- an emitter already past its claim that could
 * not be scheduled inside the drain budget -- and dropping it here is the
 * last line rather than the only one.
 *
 * The residual it cannot close is a preempted task's klog write racing the
 * frame rollover, which no producer-side check can make atomic; the run
 * boundary reports what that costs instead of hiding it. */
static int u_capture_generation_current(uint32_t claimed)
{
    uint64_t irq_flags;
    int current;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
    current = (s_capture_run_generation == claimed);
    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
    return current;
}

/* Claims one record's worth of capture wire for `owner`, linearizing the
 * budget decision, the sequence draw, the run-wide charge and the owner's
 * stop latch into ONE critical section.
 *
 * That is the point of the lock, and it is what a pile of individually
 * atomic fields cannot buy. Two descendants sharing an owner could
 * otherwise both read a seq below the budget and both reserve past it; a
 * delayed emitter could draw a seq AFTER another CPU had already published
 * the terminator, putting a chunk above the marker; and a two-counter
 * claim (owner, then run) that wins the first and loses the second would
 * consume a sequence number no record ever fills -- a permanent gap the
 * host reads as lost output. Linearized, three invariants hold by
 * construction and the host can rely on all of them:
 *
 *   1. Every sequence number drawn maps to exactly one record on the wire
 *      (a chunk, or the one terminator).
 *   2. The terminator is the owner's HIGHEST drawn sequence number, so the
 *      host's "is this stream terminated" question has a positional answer
 *      as well as a semantic one.
 *   3. An owner-scope stop is drawn at exactly UTEST_CAPTURE_OWNER_RECORD_
 *      BUDGET -- the host can therefore demand equality rather than trust
 *      an authenticated marker's word that a budget was reached.
 *
 * The lock does NOT span emission: klog's serial write is milliseconds of
 * hold time, and the kernel-code-quality gate on lock hold time is a hard
 * rule. The caller emits AFTER the unlock. Physical wire order can
 * therefore still differ from seq order -- which is already true of every
 * klog record in this kernel, and already how the host reassembles. */
static enum utest_capture_verdict u_capture_claim(struct task *owner,
                                              uint32_t *seq_out,
                                              uint32_t *charged_out,
                                              uint32_t *gen_out)
{
    enum utest_capture_verdict verdict;
    struct u_capture_state st;
    uint64_t irq_flags;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);

    /* Load, transition, store back. Everything BETWEEN those two edges is
     * u_capture_apply's pure business, so this wrapper cannot drift from
     * the transition the unit tests actually exercise. */
    st.owner_seq = (uint32_t)atomic_read(&owner->utest_capture_seq);
    st.run_records = s_capture_run_records;
    /* Relaxed is enough HERE -- the lock already orders this against every
     * other claim. The access is atomic rather than plain only so it pairs
     * legally with the lock-free fast-path load in capture_start; a plain
     * read racing a plain write is undefined regardless of how benign the
     * values are. */
    st.owner_stopped = __atomic_load_n(&owner->utest_capture_stopped,
                                       __ATOMIC_RELAXED);
    st.run_over = s_capture_run_over;
    st.run_sealed = s_capture_sealed;
    st.admitted = s_capture_admitted;

    verdict = u_capture_apply(&st, seq_out, charged_out);

    atomic_set(&owner->utest_capture_seq, (int32_t)st.owner_seq);
    /* SET-ONLY, never store-back. The transition only ever SETS this latch,
     * so writing the snapshot back unconditionally could only ever write a
     * zero -- and that zero is a lost update: the run boundary's fence sets
     * the same latch on the owner slot, so a claim that read 0, was
     * overtaken by the fence, and then stored its stale 0 back would silently
     * UNFENCE the whole descendant tree. Writing only on the set edge makes
     * the latch monotonic from every writer's side, which is the property
     * the fence relies on and the snapshot alone could not give it.
     *
     * RELEASE: the fast path reads this without the lock, so the latch must
     * not become visible before the state that justifies it. */
    if (st.owner_stopped) {
        __atomic_store_n(&owner->utest_capture_stopped, 1u, __ATOMIC_RELEASE);
        /* Stamp the stop epoch in the SAME critical section that latched the
         * stop, and only on the set edge, so it inherits the latch's
         * monotonicity. Every write armed before this epoch was cut by this
         * stop, whether or not it was the write whose claim tripped it -- the
         * budget stops the whole OWNER, so a peer thread mid-write is cut
         * without ever seeing a verdict. Publishing the epoch lets each write
         * reach that conclusion for itself at its own settlement point,
         * instead of requiring the tripping thread to name threads it cannot
         * enumerate. Set-only: a second stop cannot occur (the latch is
         * one-shot), so the first epoch is the one that matters. */
        if (owner->utest_capture_stop_epoch == 0u) {
            /* SATURATE, never wrap. On wrap the counter returns to 0, the
             * `epoch != 0` guard goes false, and every later comparison
             * inverts -- so armed writes silently stop settling as budget
             * cut, which is the fail-OPEN direction. Unreachable in practice
             * (one increment per owner budget stop), but a saturating counter
             * costs one compare and removes the question. */
            if (s_capture_stop_epoch_next < 0xFFFFFFFFu)
                s_capture_stop_epoch_next++;
            __atomic_store_n(&owner->utest_capture_stop_epoch,
                             s_capture_stop_epoch_next, __ATOMIC_RELEASE);
        }
    }
    s_capture_run_records = st.run_records;
    s_capture_run_over = st.run_over;
    /* The reservation is published in the SAME critical section that drew
     * the sequence number, so a drain reading zero has genuinely seen every
     * claim: there is no instant at which a number exists on an owner's slot
     * without the run knowing a record is owed for it. */
    s_capture_admitted = st.admitted;

    /* Read INSIDE the critical section: the generation the caller checks
     * against must be the one this claim was actually charged to. */
    *gen_out = s_capture_run_generation;

    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
    return verdict;
}

/* What a terminal claim resolved to. */
#define UTEST_CAP_TERM_REFUSE   0  /* admission closed, or allowance already spent */
#define UTEST_CAP_TERM_EMIT     1  /* emit the terminal record */
#define UTEST_CAP_TERM_OVERFLOW 2  /* emit the one overflow marker, then never again */

/* Claim one terminal record's worth of wire.
 *
 * This is the payload claim's sibling, and the differences are exactly three.
 * It draws NO sequence number: a terminal record carries the `wr` its write
 * was already given, so it needs no identity of its own, and taking one from
 * the shared counter would break the two properties the host authenticates an
 * owner stop with -- that the overflow marker sits at seq == limit, and that
 * the chunk sequence has no hole. It is EXEMPT from the owner budget latch
 * and from the reap fence, because a record whose whole job is to explain a
 * stop cannot be suppressed by that stop. And it charges its own run-scoped
 * allowance instead of the owner and run record budgets.
 *
 * What it does NOT change is its coverage by the run boundary. It takes the
 * same lock, refuses once admission is SEALED, and reserves against the same
 * `admitted` counter the drain waits on -- so a terminal record in flight
 * holds the boundary open exactly as a chunk does. Without that reservation
 * the generation check would be a bare check-then-log: this call could pass
 * it, be preempted across a frame rollover, and land an authenticated record
 * in a run it does not belong to, refusing a binary that did nothing wrong.
 * The caller credits the reservation with u_capture_complete once the record
 * has actually reached klog. */
static int u_capture_claim_terminal(uint32_t *gen_out)
{
    uint64_t irq_flags;
    int      verdict;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);

    if (s_capture_sealed) {
        /* The run has stopped admitting. Refusing is right rather than
         * regrettable: the boundary that sealed has already accounted for
         * everything still owed, and emitting past it would attribute this
         * record to a run that never admitted it. */
        verdict = UTEST_CAP_TERM_REFUSE;
    } else if (s_capture_terminal_over) {
        verdict = UTEST_CAP_TERM_REFUSE;
    } else if (s_capture_terminal_records >= UTEST_CAPTURE_TERMINAL_BUDGET) {
        /* One marker, once, then silence -- and the host refuses a run that
         * carries it, so bounding the wire never becomes hiding the loss. */
        s_capture_terminal_over = 1;
        s_capture_admitted++;
        verdict = UTEST_CAP_TERM_OVERFLOW;
    } else {
        s_capture_terminal_records++;
        s_capture_admitted++;
        verdict = UTEST_CAP_TERM_EMIT;
    }

    /* Read INSIDE the critical section, exactly as the payload claim does:
     * the generation the caller checks against must be the one this claim was
     * charged to. */
    *gen_out = s_capture_run_generation;

    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
    return verdict;
}

static const char UTEST_HEX_DIGITS[] = "0123456789abcdef";

/* Emits the currently staged bytes (if any) as one capture record, then
 * clears the stage. `is_final` marks the last chunk of ONE write() call
 * (not of the whole binary's lifetime -- a binary can write() many times,
 * each ending its own chunk sequence with final=1). seq is drawn from the
 * OWNER task's slot regardless of which task in a fork tree is actually
 * emitting, giving an O(1) unique sequence number across the whole tree
 * with no parentage walk. The draw is made under s_capture_budget_lock
 * (u_capture_claim), NOT by the bare atomic_fetch_add this used before the
 * emission budget shipped: the number, the budget verdict and the run
 * charge have to be one decision. */
/* Pure escaping: `\` and `[` (marker-forgery hazards) plus every byte
 * outside safe printable ASCII (< 0x20 or >= 0x7F) become "\xHH"; every
 * other byte -- printable ASCII minus those two -- passes through
 * unchanged. The wider-than-4-classes escape set is load-bearing, not
 * just "safe measure": scripts/utest-frame.py decodes every physical
 * line with `.decode("utf-8", errors="replace")`, so a raw non-ASCII or
 * invalid-UTF-8 byte in the payload (a lone continuation byte, a stray
 * 0x80-0xFF) would be IRREVERSIBLY replaced with U+FFFD before a captured
 * payload could ever be reconstructed byte-exactly -- silently
 * contradicting this section's own byte-exactness goal. Escaping
 * everything outside 0x20-0x7E keeps the wire text pure 7-bit ASCII,
 * which that same UTF-8 decode passes through byte-identical. NUL-
 * terminates `out` and returns the escaped length (excluding the NUL),
 * or 0xFFFFFFFFu if `out_cap` is too small for the worst case (caller-
 * sized buffers in this file are always exactly the worst-case size, so
 * that refusal is a defensive backstop, not a path exercised by the
 * shipped callers). No task/scheduler state -- a pure buffer transform,
 * testable without a live task context. */
static uint32_t u_capture_escape(const char *raw, uint32_t raw_len,
                                 char *out, uint32_t out_cap)
{
    uint32_t i, epos = 0;

    for (i = 0; i < raw_len; i++) {
        unsigned char c = (unsigned char)raw[i];
        uint32_t need = (c == '\\' || c == '[' || c < 0x20u || c >= 0x7Fu)
                        ? 4u : 1u;

        if (epos + need + 1u > out_cap)
            return 0xFFFFFFFFu;
        if (need == 4u) {
            out[epos++] = '\\';
            out[epos++] = 'x';
            out[epos++] = UTEST_HEX_DIGITS[(c >> 4) & 0xFu];
            out[epos++] = UTEST_HEX_DIGITS[c & 0xFu];
        } else {
            out[epos++] = (char)c;
        }
    }
    out[epos] = '\0';
    return epos;
}

/* Escapes+emits one chunk from ctx's own (call-local, never shared)
 * staging buffer. seq is assigned inside u_capture_claim, under
 * s_capture_budget_lock, together with the budget verdict and the run
 * charge -- it was a bare atomic_fetch_add on the OWNER's slot until the
 * emission budget shipped, and the run-wide counters are now shared state
 * alongside it. The value is still O(1), unique and monotonically
 * assigned per chunk (no two chunks ever collide or skip a number), which
 * for one thread's own successive chunks always matches the order those
 * chunks were actually filled.
 *
 * That value does NOT additionally guarantee the physical wire (serial)
 * order across DIFFERENT threads matches seq order -- and neither does
 * any other klog-based record in this kernel: klog_emit() (klog.c)
 * reserves a ring slot under s_klog_lock (which DOES give a consistent
 * ring-position order) but releases that lock before calling
 * serial_write(), so two concurrent callers' physical serial writes can
 * still land in either order under preemption. This is a systemic
 * property of klog, not something this feature introduces or could
 * close without either reintroducing a per-chunk lock spanning
 * seq-assignment through serial commit (undoing the per-byte-lock
 * removal that fixed the prior design's real perf and correctness
 * bugs) or waiting on klog's own ordering guarantees to strengthen.
 * Scope: multi-threaded test binaries only (none shipped today) --
 * the host-side artifact/reconciliation work should treat seq as the
 * authoritative ordering key, not physical log position. */
/* WRITE IDENTITY (`wr=`). Every record of ONE write() call carries the
 * same `wr`, which is the sequence number drawn for that write's FIRST
 * emitted chunk. It needs no counter of its own and no new task field:
 * seq is already unique per owner (drawn under s_capture_budget_lock), so
 * the first number a write draws is an identity no other write can also
 * hold. Without it the host could only check that the owner's HIGHEST seq
 * was final, and a second writer sharing the owner -- a fork descendant or
 * a second thread -- masked an earlier truncated write: A emits
 * `seq=0 final=0` and dies, B emits `seq=1 final=1`, and the sequence is
 * contiguous with a final highest record. With `wr` the host reconciles
 * each write independently and A's unterminated group refuses the run.
 *
 * The identity is assigned on the first EMITted chunk, not at
 * test_usermode_capture_start(): a write that stages nothing must not
 * consume a number (the host reads a hole in the sequence as output lost
 * on the wire), and a write that is refused outright never creates a group
 * for the host to close. The residual is a write killed mid-syscall before
 * its first chunk ever flushed -- under one chunk of payload, so nothing
 * reached the wire to be identified. Closing that needs an emitted
 * write-start event, which section 54 owns.
 *
 * Returns 1 while this owner may keep capturing, 0 once it has spent its
 * budget. The caller latches that into its own ctx so the NEXT chunk skips
 * the escape pass and the lock entirely -- see utest_capture_ctx._discard. */
static int u_capture_emit_chunk(struct utest_capture_ctx *ctx, int is_final)
{
    char escaped[UTEST_CAPTURE_CHUNK_MAX * UTEST_CAPTURE_ESCAPE_EXPANSION + 1u];
    struct task *owner = (struct task *)ctx->_owner;
    const char *raw = ctx->_buf;
    uint32_t raw_len = ctx->_len;
    enum utest_capture_verdict verdict;
    uint32_t seq = 0, charged = 0, gen = 0;

    /* Unconditional: a flush call (mid-loop chunk-full OR end-of-write
     * "final") with nothing staged is always a no-op. Without this a
     * write() landing exactly on a chunk boundary would flush its full
     * chunk mid-loop, then the unconditional end-of-loop
     * test_usermode_capture_end() call would emit a SECOND, spurious
     * len=0 final=1 record for the same write(). */
    if (raw_len == 0)
        return 1;

    /* Escape BEFORE claiming a sequence number. sizeof(escaped) is exactly
     * the worst case for UTEST_CAPTURE_CHUNK_MAX raw bytes, so this can
     * only refuse if raw_len itself exceeded that bound (a caller bug --
     * the ctx buffer is flushed at exactly that fill) -- but the ORDER is
     * load-bearing regardless of how unreachable the refusal is. Claiming
     * first and refusing second would consume a sequence number that no
     * record ever fills, and the host reads a hole in the sequence as
     * output lost on the wire: a defensive backstop would manufacture the
     * very corruption verdict it exists to avoid. */
    if (u_capture_escape(raw, raw_len, escaped, (uint32_t)sizeof(escaped)) ==
        0xFFFFFFFFu)
        return 1;

    verdict = u_capture_claim(owner, &seq, &charged, &gen);

    /* A claim charged to a PREVIOUS framed run must not reach the wire in
     * this one -- see u_capture_generation_current. Checked once here rather
     * than per branch: it applies to a chunk and to a terminator alike.
     *
     * This refusal DOES consume a sequence number without emitting a
     * record, which is the one place the claim's "every number drawn maps
     * to a record" contract does not hold. It is the lesser of the two
     * available behaviours, not an oversight: the alternative is emitting
     * the stale record into the current run, where it has no
     * [UTEST-CAPTURE-BEGIN] binding it and reconciles as
     * capture_unbound_owner -- failing a run that did nothing wrong. The
     * number spent here lands past the end of a slice that is already
     * closed by its own frame-end record, so no consumer can observe the
     * hole. Rolling it back is not available either: another emitter may
     * already have drawn the next number, so a give-back would hand out a
     * duplicate.
     *
     * The reservation this claim took is NOT written off here, and that is
     * deliberate: it already was, by the boundary that made this claim
     * stale. Reaching this branch means the generation moved, which only
     * u_frame_begin does, and the u_frame_end before it sealed the run and
     * drained it -- a claim outstanding at that moment is exactly what the
     * drain counts and forgives. Writing it off a second time here would
     * make `forgiven` overstate what the run actually lost, and `forgiven`
     * is the number the pending report is derived from.
     *
     * The one claim that can be admitted with no boundary behind it is the
     * launcher's own pre-frame capture regression, which drives this path
     * directly before any framed run exists. Those cannot reach this branch:
     * claim, check and emission happen in this one call with nothing between
     * them that could run u_frame_begin. */
    if (verdict != UTEST_CAP_DROP && verdict != UTEST_CAP_SEALED &&
        !u_capture_generation_current(gen))
        return 0;

    switch (verdict) {
    case UTEST_CAP_EMIT:
        /* First accepted chunk of this write() names the write. Assigned
         * here rather than before the claim so a number is never spent on
         * a write that produced no record. */
        if (!ctx->_has_wid) {
            struct thread *thr;

            ctx->_wid = seq;
            ctx->_has_wid = 1;
            /* Mirror the identity onto the thread's open-write evidence. The
             * ctx lives on this thread's stack and dies with it, so a
             * teardown that has to name this write can only do so from state
             * that outlives the frame. Guarded on OPEN so a write that was
             * never armed (it entered discard before staging a byte) cannot
             * plant an identity on a slot that owes no record. */
            thr = (struct thread *)ctx->_thr;
            if (thr &&
                __atomic_load_n(&thr->utest_cap_state, __ATOMIC_ACQUIRE) ==
                    (uint8_t)UTEST_CAP_THREAD_OPEN) {
                /* wid FIRST, then has_wid with RELEASE. A settling reader
                 * keys on has_wid to decide whether this write has a group to
                 * name, so publishing the flag before the value it describes
                 * would let a teardown emit an identity it cannot yet see.
                 * The ordering is the same shape the arm block uses for
                 * utest_cap_state. */
                thr->utest_cap_wid = seq;
                __atomic_store_n(&thr->utest_cap_has_wid, (uint8_t)1,
                                 __ATOMIC_RELEASE);
            }
        }
        utest_record_log_receipted(LOG_INFO, u_capture_receipt, gen,
                         "[UTEST-CAPTURE] owner=%u wr=%u seq=%u len=%u "
                         "final=%u %s",
                         (uint64_t)owner->pid, (uint64_t)ctx->_wid,
                         (uint64_t)seq, (uint64_t)raw_len,
                         (uint64_t)(is_final ? 1u : 0u), escaped);
        break;
    case UTEST_CAP_OVER_OWNER:
        /* LOG_WARN, not LOG_ERROR: a budget stop is the EXPECTED outcome
         * under an abusive binary, so it must stay distinguishable from
         * [UTEST-CAPTURE-LOST], which reports a defensive ownership
         * failure. Conflating them would make a policy stop read as
         * corruption on the host. */
        utest_record_log_receipted(LOG_WARN, u_capture_receipt, gen,
                         "[UTEST-CAPTURE-OVER] owner=%u seq=%u "
                         "scope=owner limit=%u charged=%u",
                         (uint64_t)owner->pid, (uint64_t)seq,
                         (uint64_t)UTEST_CAPTURE_OWNER_RECORD_BUDGET,
                         (uint64_t)charged);
        break;
    case UTEST_CAP_OVER_RUN:
        utest_record_log_receipted(LOG_WARN, u_capture_receipt, gen,
                         "[UTEST-CAPTURE-OVER] owner=%u seq=%u "
                         "scope=run limit=%u charged=%u",
                         (uint64_t)owner->pid, (uint64_t)seq,
                         (uint64_t)UTEST_CAPTURE_RUN_RECORD_BUDGET,
                         (uint64_t)charged);
        break;
    case UTEST_CAP_DROP:
        /* Terminated owner. The bytes are deliberately swallowed rather
         * than handed back to the caller's raw serial_putchar fallback:
         * falling back would put the identical payload on the wire
         * unframed, so the budget would bound nothing at all. */
        break;
    case UTEST_CAP_SEALED:
        /* The run closed before this claim arrived. Swallowed for the same
         * reason a DROP is -- an unframed fallback would put the payload on
         * the wire between two runs' slices, where no consumer can attribute
         * it -- but reported nowhere, because there is nothing to report: no
         * number was drawn, no record is owed, and the emitter belongs to a
         * binary the launcher has already finished with. */
        break;
    }

    /* A reservation is settled exactly once, by the klog delivery receipt the
     * emitting branch above attached to its record -- so settlement is now a
     * property of the EMISSION rather than of this function being reached
     * again afterwards, which is the whole of section 57. The two non-emitting
     * verdicts settle nothing here for the reasons stated in their branches:
     * DROP and SEALED never drew a record to acknowledge.
     *
     * The old post-switch credit is gone rather than kept as a belt: with the
     * receipt already crediting, a second credit here would settle one
     * reservation twice, and the surplus does not vanish -- it cancels a later
     * run's live reservation and lets that run's drain roll over with a record
     * still in flight. A record kind added to the switch without a receipt
     * fails the opposite way: its claim stays outstanding and the next
     * boundary waits out its whole drain budget, which the drain reports.
     *
     * Anything but a plain chunk means this owner is finished: the caller
     * latches it and stops paying the escape pass and the lock. */
    return verdict == UTEST_CAP_EMIT;
}

/* Resolves task_current() and its capture ownership ONCE per write() --
 * not per byte, unlike an earlier design that re-resolved task_current()
 * and re-acquired a per-task lock on every single byte. ctx->_owner
 * resolves the OWNER task (task_get_by_pid on utest_capture_owner_pid),
 * not task_current() itself, so u_capture_claim's sequence draw always
 * targets the right slot even from a fork() descendant. */
/* The generation an arming write records, read lock-free.
 *
 * The payload fast path deliberately refuses to take the budget lock on every
 * write, and arming inherits that constraint: 1427 writes in one suite run
 * would each pay an acquisition for a value only ever used in a later
 * equality test. A stale read can only compare UNEQUAL at emission, which
 * suppresses the terminal record -- the fail-closed direction. */
static uint32_t u_capture_generation_now(void)
{
    return __atomic_load_n(&s_capture_run_generation, __ATOMIC_ACQUIRE);
}

/* Take exclusive responsibility for settling this thread's open write.
 *
 * Returns 1 to exactly one caller per armed write and 0 to every other, so a
 * capture_end racing a remote death cannot both emit. */
static int u_capture_claim_settlement(struct thread *thr)
{
    uint8_t expected = (uint8_t)UTEST_CAP_THREAD_OPEN;

    return __atomic_compare_exchange_n(&thr->utest_cap_state, &expected,
                                       (uint8_t)UTEST_CAP_THREAD_SETTLED,
                                       0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Emit one terminal record for a write that stopped without a final chunk,
 * and mark the thread's evidence settled.
 *
 * `reason` is NULL for a budget CUT and names the death otherwise. Settlement
 * happens on EVERY path out of here, including the refusals: the state means
 * "a record is still owed for this write", and a write the run has declined
 * to record is no longer owed one. Leaving it OPEN would let a later
 * settlement point try again and, in the reap case, emit a duplicate.
 *
 * The generation check is the same last-of-three the chunk path applies, and
 * it matters more here: a descendant that outlived the reap round cap is
 * cleaned up whenever it is cleaned up, which may be after the originating
 * run's frame-end. Dropping in that case is not lost information -- the
 * originating run already reported the descendant as unreaped, against the
 * run that actually suffered it. */
static void u_capture_settle_terminal(struct thread *thr, uint32_t owner_pid,
                                      uint32_t task_pid, const char *reason)
{
    uint32_t gen = 0;
    int      verdict;

    if (!thr)
        return;

    /* CLAIM the settlement, do not merely observe it. capture_end and a remote
     * death path can both reach a write, and "check then store" would let both
     * pass the check and emit two records for one write -- which the host
     * refuses as a duplicate. The exchange is what makes settled-exactly-once
     * a property of the code rather than of the scheduling. ACQUIRE pairs with
     * the release publication in capture_start, so the winner reads the
     * generation and identity this write actually armed with. */
    if (!u_capture_claim_settlement(thr))
        return;

    verdict = u_capture_claim_terminal(&gen);
    if (verdict == UTEST_CAP_TERM_REFUSE)
        return;
    if (gen != thr->utest_cap_gen || !u_capture_generation_current(gen)) {
        u_capture_complete(gen);
        return;
    }

    /* Each branch carries the receipt, replacing the single credit that used
     * to follow the whole if-chain. It matters MORE here than on the chunk
     * path: this function frequently runs on behalf of a thread that is
     * already dead, from a reap or a task teardown, so the credit that used
     * to sit below is executed by a settler whose own survival across the
     * emission is exactly what a teardown does not guarantee. The generation
     * this write armed with travels as the cookie, so the record and its
     * settlement name the same epoch even when the emitting thread is gone. */
    if (verdict == UTEST_CAP_TERM_OVERFLOW) {
        utest_record_log_receipted(LOG_ERROR, u_capture_receipt, gen,
                         "[UTEST-CAPTURE-TERMINAL-OVER] limit=%u",
                         (uint64_t)UTEST_CAPTURE_TERMINAL_BUDGET);
    } else if (!reason) {
        utest_record_log_receipted(LOG_WARN, u_capture_receipt, gen,
                         "[UTEST-CAPTURE-CUT] owner=%u wr=%u",
                         (uint64_t)owner_pid, (uint64_t)thr->utest_cap_wid);
    } else {
        utest_record_log_receipted(LOG_ERROR, u_capture_receipt, gen,
                         "[UTEST-CAPTURE-ABANDON] owner=%u task=%u thr=%u "
                         "haswr=%u wr=%u reason=%s",
                         (uint64_t)owner_pid, (uint64_t)task_pid,
                         (uint64_t)thr->id,
                         (uint64_t)thr->utest_cap_has_wid,
                         (uint64_t)thr->utest_cap_wid,
                         reason);
    }
}

/* Was this write cut by an owner budget stop that landed after it armed?
 *
 * Compared against the write's own arm-time snapshot rather than asked of the
 * verdict it received, because the owner budget stops every write the owner
 * has open, not just the one whose claim tripped it. A peer parked mid-write
 * never sees a verdict at all, and this comparison is how it learns. */
static int u_capture_write_was_budget_cut(const struct task *owner,
                                          const struct thread *thr)
{
    uint32_t epoch;

    if (!owner || !thr)
        return 0;
    epoch = __atomic_load_n(&owner->utest_capture_stop_epoch, __ATOMIC_ACQUIRE);
    return epoch != 0u && epoch > thr->utest_cap_stop_epoch;
}

void test_usermode_capture_start(struct utest_capture_ctx *ctx)
{
    struct task *self = task_current();
    struct task *owner;

    ctx->_len = 0;
    ctx->_active = 0;
    ctx->_discard = 0;
    ctx->_has_wid = 0;
    ctx->_wid = 0;
    ctx->_owner = (void *)0;
    ctx->_thr = (void *)0;
    ctx->_cut_fenced = 0;

    if (!self || !self->utest_capture_active)
        return;

    owner = task_get_by_pid(self->utest_capture_owner_pid);
    if (!owner) {
        /* Owner slot lookup failing should not happen -- pids are
         * monotonic within a boot and never reused, and owner_pid is
         * always either self (at spawn) or inherited from an alive
         * parent (at fork) -- but a corrupted owner pid must never
         * dereference a wild pointer later. Emit an explicit,
         * authenticated loss record so even this defensive-only path
         * leaves evidence, then decline capture for this write (the
         * caller's raw serial_putchar fallback still delivers the
         * bytes, just unframed). */
        utest_record_log(LOG_ERROR,
                         "[UTEST-CAPTURE-LOST] owner=%u len=unknown",
                         (uint64_t)self->utest_capture_owner_pid);
        return;
    }
    ctx->_owner = (void *)owner;
    ctx->_active = 1;

    /* Already-spent owners enter discard mode HERE, before a single byte is
     * staged, so a binary that keeps writing after its stop costs one plain
     * byte read per write() instead of an escape pass plus a global-lock
     * acquisition per chunk, forever.
     *
     * Read WITHOUT the budget lock on purpose, but ATOMICALLY -- the two
     * are separate requirements and only the first is a design choice. The
     * latch is monotonic within a run (set under the lock, cleared only
     * when a task slot is constructed, which happens before that task is
     * published), so the only possible stale answer is a 0 that should have
     * been 1, costing exactly one more trip through the locked claim, which
     * then returns DROP and latches the ctx anyway. Taking the lock here to
     * remove that harmless one-shot slow path would put an acquisition on
     * EVERY write, including every well-behaved one, which is the cost this
     * fast path exists to avoid. The access is atomic because a plain read
     * racing the claim's plain write is undefined behavior no matter how
     * benign the observable values are -- monotonicity is an argument about
     * VALUES, and it cannot license a data race. */
    if (__atomic_load_n(&owner->utest_capture_stopped, __ATOMIC_ACQUIRE))
        ctx->_discard = 1;

    /* ARM the open-write evidence, and ONLY for a write that is genuinely
     * capturing. A write entering discard mode never reaches the wire and is
     * owed no explanation if its thread dies -- arming it would manufacture an
     * ABANDON for a write that was already, correctly, silent.
     *
     * The epoch snapshot is what makes a peer's stop visible to this write
     * later: if the owner is stopped after this point, its epoch will exceed
     * the value captured here, and this write settles as budget-cut at its own
     * capture_end or at teardown without ever having seen a verdict itself. */
    ctx->_thr = (void *)thread_current();
    if (ctx->_active && !ctx->_discard) {
        struct thread *thr = (struct thread *)ctx->_thr;

        if (thr) {
            /* Evidence FIRST, OPEN published LAST, with release. The state is
             * what a remote reader keys on: a death or reap path that sees
             * OPEN will read the generation and write identity beside it and
             * emit a record from them. Storing OPEN first leaves a window in
             * which this thread can be remotely terminated and its evidence
             * read while still stale -- and the terminal path settles before
             * it discovers the mismatch, so the record is lost rather than
             * retried. Publishing last means OPEN is never visible without
             * the fields that give it meaning. */
            thr->utest_cap_has_wid = 0;
            thr->utest_cap_wid = 0;
            thr->utest_cap_death = (uint8_t)UTEST_CAP_DEATH_NONE;
            thr->utest_cap_gen = u_capture_generation_now();
            thr->utest_cap_stop_epoch =
                __atomic_load_n(&owner->utest_capture_stop_epoch,
                                __ATOMIC_ACQUIRE);
            __atomic_store_n(&thr->utest_cap_state,
                             (uint8_t)UTEST_CAP_THREAD_OPEN, __ATOMIC_RELEASE);
        }
    }
}

/* One byte of a ring-3 write(). ctx is a LOCAL variable on the caller's
 * own stack (never shared across threads or CPUs), so staging a byte
 * into it needs no lock -- there is no other execution context that can
 * observe or mutate this particular ctx. Called from the caller's single
 * read of the raw user buffer (never a second dereference of the same
 * byte) so it cannot observe a value the caller's own NUL-check or
 * terminal echo did not also observe. Returns 1 (consumed) or 0 (not
 * capture-owned, caller must fall back to its own raw serial_putchar
 * path).
 *
 * The mid-write flush check runs BEFORE staging this byte, not after --
 * flushing a chunk the moment it fills would emit that chunk with
 * final=0 even when this byte turns out to be the WRITE's last byte,
 * leaving no final=1 record at all for an exact-chunk-multiple write
 * (indistinguishable from an interrupted one). Checking first means a
 * full-but-not-yet-overflowing buffer is only ever flushed non-final when
 * a byte genuinely needs the room; if the loop ends with the buffer
 * sitting exactly full, the caller's end-of-loop test_usermode_capture_
 * end() call flushes it as final=1 instead. */
/* Record why this write's payload stopped, AT the moment it stopped.
 *
 * The owner's reap fence can latch at any time after a write is dropped for an
 * unrelated reason (a stale-generation claim at a run boundary, say), and a
 * settle path that re-read the fence then would report that write as a
 * deliberate teardown -- which the host EXEMPTS. That is the same re-derivation
 * error the death path avoids by snapshotting at the transition, so this path
 * avoids it the same way. */
static void u_capture_note_stop(struct utest_capture_ctx *ctx)
{
    struct task *owner = (struct task *)ctx->_owner;

    ctx->_discard = 1;
    ctx->_cut_fenced =
        (owner && __atomic_load_n(&owner->utest_capture_fenced,
                                  __ATOMIC_ACQUIRE))
            ? (uint8_t)1
            : (uint8_t)0;
}

int test_usermode_capture_byte(struct utest_capture_ctx *ctx, char c)
{
    if (!ctx->_active)
        return 0;

    /* Consumed and dropped: still 1, never 0. Returning 0 would send the
     * caller to its raw serial_putchar fallback and put the payload the
     * budget just stopped straight back on the wire. */
    if (ctx->_discard)
        return 1;

    if (ctx->_len >= UTEST_CAPTURE_CHUNK_MAX) {
        if (!u_capture_emit_chunk(ctx, 0)) {
            u_capture_note_stop(ctx);
            ctx->_len = 0;
            return 1;
        }
        ctx->_len = 0;
    }
    ctx->_buf[ctx->_len++] = c;
    return 1;
}

void test_usermode_capture_end(struct utest_capture_ctx *ctx)
{
    struct task   *owner = (struct task *)ctx->_owner;
    struct thread *thr;

    if (!ctx->_active)
        return;

    if (!ctx->_discard) {
        if (!u_capture_emit_chunk(ctx, 1))
            u_capture_note_stop(ctx);
        ctx->_len = 0;
    }

    /* SETTLE. Reaching here means this write is over, so the evidence armed
     * for it must not survive into the thread's next write or its teardown.
     *
     * A write that flushed its final chunk owes nothing: the host closes its
     * `wr` group on the final=1 record. A write that discarded owes an
     * explanation, because the host would otherwise see a group that opened
     * and never closed and could not tell a policy stop from a truncated
     * stream. Which explanation depends on what stopped it, and the answer is
     * derived from the owner's own latches rather than from the verdict this
     * write happened to receive -- a peer thread's claim can stop this owner
     * while this write is parked between bytes, so the write that gets the
     * verdict is very often not the only write that was cut. */
    thr = (struct thread *)ctx->_thr;
    if (!thr ||
        __atomic_load_n(&thr->utest_cap_state, __ATOMIC_ACQUIRE) !=
            (uint8_t)UTEST_CAP_THREAD_OPEN)
        return;

    if (!ctx->_discard || !thr->utest_cap_has_wid) {
        /* Nothing reached the wire under this write's name, or the final
         * chunk closed it. Either way no group is left open for a terminal
         * record to close, so settling silently is the whole obligation.
         *
         * A PLAIN RELEASE STORE, not the exchange -- and this is the common
         * case, every ordinary write in the suite. Nothing can be racing it:
         * the only other party that ever settles a write is the reap barrier,
         * which by construction runs after this thread is dead AND off-CPU,
         * whereas this code is running on that very thread. Paying a locked
         * read-modify-write here would put exclusive cache-line ownership on
         * the hot path of every captured write and contradict the whole
         * reason this shape was chosen over write-start records. The exchange
         * stays where two parties genuinely can contend: exceptional
         * settlement in u_capture_settle_terminal. */
        __atomic_store_n(&thr->utest_cap_state,
                         (uint8_t)UTEST_CAP_THREAD_SETTLED, __ATOMIC_RELEASE);
        return;
    }

    if (u_capture_write_was_budget_cut(owner, thr)) {
        u_capture_settle_terminal(thr, owner ? owner->pid : 0u, 0u,
                                  (const char *)0);
    } else if (ctx->_cut_fenced) {
        /* The launcher's end-of-binary fence: a deliberate teardown of a
         * descendant that outlived its binary, and it must be SAID -- an
         * unexplained open group refuses the run, which would turn an orderly
         * reap into a corruption verdict. */
        u_capture_settle_terminal(thr, owner->pid, owner->pid, "fenced");
    } else {
        /* Stopped by neither a budget this owner reached nor the fence, which
         * leaves only the run boundary sealing admission underneath an
         * in-flight write. There is deliberately NO record for it: the
         * terminal claim refuses once sealed, so any record named here could
         * never reach the wire, and inventing a vocabulary the producer
         * cannot emit would put a shape in the host grammar that only a
         * forgery could ever produce. The boundary reports its own losses
         * through the pending record, against the run that suffered them. */
        (void)u_capture_claim_settlement(thr);
    }
}

/* Settle a DEAD thread's open capture write, at the reap barrier where
 * logging is safe.
 *
 * The reason was snapshotted at the death transition itself and is only READ
 * here. Re-deriving it now would be wrong in the one case that matters: the
 * launcher latches the reap fence before it starts killing, so by the time a
 * reap runs, the fence is set for every descendant -- including one killed
 * long before it, whose lost payload would then be granted the fence's
 * exemption. A thread that reaches reap OPEN with no recorded death is
 * treated as killed, which is the fail-closed reading: an open write nobody
 * accounted for is exactly the silence this record exists to break. */
void test_usermode_cap_settle_dead_thread(struct task *t, struct thread *thr)
{
    struct task *owner;

    if (!t || !thr ||
        __atomic_load_n(&thr->utest_cap_state, __ATOMIC_ACQUIRE) !=
            (uint8_t)UTEST_CAP_THREAD_OPEN)
        return;

    owner = task_get_by_pid(t->utest_capture_owner_pid);

    /* The stop epoch is consulted BEFORE the death outcome, because a write
     * cut by the owner's budget was already truncated by policy before its
     * thread died -- the death is how it failed to reach capture_end, not what
     * ended its payload. Reporting it as abandoned would refuse an intentional
     * bounded stop; reporting it as fenced would credit the teardown with a
     * truncation the budget had already caused. Only a write with a named
     * group can carry a cut: a write that never reached the wire has no group
     * to close, and naming wr=0 there would exempt an unrelated write. */
    if (thr->utest_cap_has_wid && u_capture_write_was_budget_cut(owner, thr)) {
        u_capture_settle_terminal(
            thr, owner ? owner->pid : t->utest_capture_owner_pid, t->pid,
            (const char *)0);
        return;
    }

    u_capture_settle_terminal(
        thr, owner ? owner->pid : t->utest_capture_owner_pid, t->pid,
        thr->utest_cap_death == (uint8_t)UTEST_CAP_DEATH_FENCED ? "fenced"
                                                                : "killed");
}

/* Called from task_create_internal() (task.c), BEFORE the new task is
 * published (num_tasks++) -- see the header declaration for why the
 * timing is load-bearing: emitting this any later would let an
 * immediately-scheduled task on another CPU emit capture chunk records
 * before their owner binding reaches the wire. */
/* `chunk_max` PUBLISHES the producer's per-record raw payload bound so the
 * host can enforce it. It is a derived value (UTEST_RECORD_LINE_MAX minus
 * the record's own fixed literal cost, divided by the worst-case escape
 * expansion), so a host that hardcoded today's number would refuse EVERY
 * run the day any of those three inputs moved -- a worse failure than the
 * over-long chunk the check exists to reject. Carried on BEGIN rather than
 * the run's [UTEST-FRAME] line because scripts/utest-frame.py owns the
 * framing records, and because a chunk record is only reconcilable at all
 * when its owner has a BEGIN: declaration and use then have exactly the
 * same lifetime. */
void test_usermode_capture_begin(uint32_t owner_pid, const char *name)
{
    utest_record_log(LOG_INFO,
                     "[UTEST-CAPTURE-BEGIN] owner=%u chunk_max=%u name=%s",
                     (uint64_t)owner_pid,
                     (uint64_t)UTEST_CAPTURE_CHUNK_MAX, name);
}

struct u_report {
    uint32_t asserts_passed;
    uint32_t asserts_failed;
    uint32_t skip_blocks;
    uint32_t state;          /* TASK_UTEST_REPORT_* */
};

/* Run-wide totals over every VALID report, plus the census of how the
 * submissions themselves landed. `invalid` and `unreported` are carried
 * so a consumer can tell "nothing was skipped" from "nobody said". */
struct u_report_totals {
    uint32_t asserts_passed;
    uint32_t asserts_failed;
    uint32_t skip_blocks;
    uint32_t skip_records;   /* synthetic skip testcases/TAP points emitted */
    uint32_t reported;       /* binaries with an accepted report            */
    uint32_t invalid;        /* binaries whose report contradicted itself   */
    uint32_t unreported;     /* binaries that never submitted one           */
};

static void u_report_snapshot(uint32_t child_pid, struct u_report *out)
{
    struct task *child = task_get_by_pid(child_pid);

    out->asserts_passed = 0;
    out->asserts_failed = 0;
    out->skip_blocks    = 0;
    out->state          = TASK_UTEST_REPORT_NONE;
    if (!child) {
        /* The TCB should still be live here -- the pid came from a
         * successful task_create and task_cleanup has not run yet -- so
         * its absence is an anomaly, not a legacy non-reporting binary.
         * Fail CLOSED and say so: leaving state NONE would file a
         * vanished task under "never reported" with no trace at all, on
         * the one path whose entire purpose is fail-closed reporting. */
        klog(LOG_ERROR, "UTEST",
             "report snapshot: pid %u vanished before reap -- treating as INVALID",
             (uint64_t)child_pid);
        out->state = TASK_UTEST_REPORT_INVALID;
        return;
    }
    /* Acquire the state first: the submitter publishes it with a release
     * compare-exchange AFTER writing the counts, so acquiring VALID here
     * guarantees the counts below are the ones that go with it. */
    out->state = __atomic_load_n(&child->utest_report.state,
                                 __ATOMIC_ACQUIRE);
    /* Read the counts ONLY behind an acquired VALID. On any other state a
     * submitter may still be mid-write (the three plain stores between
     * the claim and the publish), so reading them would be a formal race
     * for values that are discarded anyway. */
    if (out->state == TASK_UTEST_REPORT_VALID) {
        out->asserts_passed = child->utest_report.asserts_passed;
        out->asserts_failed = child->utest_report.asserts_failed;
        out->skip_blocks    = child->utest_report.skip_blocks;
    }
    /* A record still CLAIMED at reap means the submitter died between
     * taking the slot and publishing. Its counts were never completed, so
     * it is INVALID -- fail-closed, exactly like a contradiction. */
    if (out->state == TASK_UTEST_REPORT_CLAIMED)
        out->state = TASK_UTEST_REPORT_INVALID;
}

/* Reconcile an accepted report against the outcome the kernel actually
 * observed, and return the state the launcher should act on.
 *
 * The kernel-side syscall could not do this: at submission time the
 * binary has not exited yet, so the exit status does not exist. Here it
 * does, and a report that disagrees with it is the precise shape this
 * whole section exists to catch -- a binary claiming "all assertions
 * passed" while exiting non-zero, or claiming a whole-binary skip while
 * reporting assertions it ran.
 *
 * Deliberately NOT an exact `exit_status == failed` equality: test.h
 * documents `return g_fail;` as the convention, not a requirement, so a
 * binary that normalizes its exit code to 1 is well-formed. The
 * equivalence below catches every false-green direction without
 * outlawing that.
 *
 * Pure function of its arguments so the unit tests can drive the whole
 * matrix without spawning anything. */
static uint32_t u_report_reconcile(uint32_t state, uint32_t failed,
                                   int32_t exit_status, int timed_out)
{
    /* A half-written record (submitter died between claiming the slot and
     * publishing) is a contradiction like any other, and is mapped here
     * as well as at snapshot time so the two are not order-dependent:
     * letting CLAIMED fall through would make it neither VALID nor
     * INVALID, and the caller would count it as "never reported". */
    if (state == TASK_UTEST_REPORT_CLAIMED)
        return TASK_UTEST_REPORT_INVALID;
    if (state != TASK_UTEST_REPORT_VALID)
        return state;   /* NONE stays legacy; INVALID is already sticky */

    /* Timeout: exit_status is the launcher's own synthetic marker
     * (u_wait_with_timeout overwrote it), so it can never agree with any
     * report and reconciliation would be meaningless. The binary reached
     * UTEST_END and then hung -- its counts are real, and the timeout
     * verdict dominates on its own. */
    if (timed_out)
        return TASK_UTEST_REPORT_VALID;

    /* Exit 77 means "the whole binary was skippable", which contradicts
     * having reached UTEST_END with counters to submit. */
    if (exit_status == UTEST_EXIT_SKIP)
        return TASK_UTEST_REPORT_INVALID;

    if ((exit_status == 0) != (failed == 0))
        return TASK_UTEST_REPORT_INVALID;

    return TASK_UTEST_REPORT_VALID;
}

/* Apply an INVALID report to the binary's verdict and the run counters.
 * Returns the verdict after escalation.
 *
 * A PASS or a SKIP built on a self-contradicting report becomes a FAIL:
 * silently dropping the counts would leave the run green on a binary that
 * just proved its own reporting untrustworthy, which is the false-green
 * class this section exists to close. An already-FAIL is left alone --
 * its exit-code or timeout reason is more specific than "bad report".
 *
 * Split out of u_run_one because it is the load-bearing half of the
 * fail-closed path and u_run_one itself cannot be unit-tested (it spawns
 * a live child, which the test policy forbids). `counters` is the
 * [pass, fail, skip] triple; the decrement side is only ever reached from
 * a branch that incremented the same slot earlier in the same call, so it
 * cannot underflow. */
static int u_report_apply_invalid(int verdict, uint32_t *counters)
{
    if (verdict == 1)
        return 1;               /* already failed, for a better reason */
    if (verdict == 0)
        counters[0]--;          /* undo PASS */
    else
        counters[2]--;          /* undo SKIP */
    counters[1]++;
    return 1;
}

/* ---- emit helpers ---------------------------------------------- */

static void u_emit_xml_suite_open(void)
{
    if (!s_xml_mode) return;
    /* Attributes (tests/failures/skipped/time) are re-filled by
     * scripts/test.sh's post-processor because we don't yet know the
     * final counts; emit a placeholder header so the XML envelope is
     * recognizable even without post-processing. */
    utest_record_log(LOG_INFO,
         "[UTEST-XML] <testsuite name=\"impossible-os-usermode\" tests=\"0\" "
         "failures=\"0\" skipped=\"0\" errors=\"0\" time=\"0\">");
}

/* Build the [UTEST-XML-SUMMARY] body. Returns 1 on success, 0 if the
 * line would not fit.
 *
 * Split out of the emitter for two reasons. Every append is CHECKED here
 * -- the previous emitter ignored all of them, so a line that outgrew its
 * buffer was published silently truncated, and the host post-processor
 * would read a truncated `tests=` as a valid smaller number. And a
 * formatter that writes into a caller-supplied buffer can be driven to
 * overflow by a unit test with a deliberately small cap, which an emitter
 * that klogs directly cannot.
 *
 * `tests` and `skipped` count RECORDS (binary testcases plus the
 * synthetic per-skip-block ones), so the attributes the host patches into
 * <testsuite> match the number of <testcase>/<skipped> elements actually
 * emitted.
 *
 * `aborted` / `not_run` describe the RUN, not any testcase: a suite the
 * smoke gate cut short describes only the binaries that executed, so
 * without these two fields the artifact reads as a smaller COMPLETE run.
 * They are appended AFTER `time=` deliberately -- every host parser in
 * scripts/test.sh step 6 is a greedy `.*<key>=(...)` sed plus an
 * end-unanchored validating grep, so trailing fields extend the line
 * without disturbing any existing extraction. */
static int u_format_xml_summary(char *dst, uint32_t cap, uint32_t tests,
                                uint32_t failures, uint32_t skipped,
                                uint64_t total_ms, int aborted,
                                uint32_t not_run, uint32_t errors)
{
    uint32_t pos = 0;
    char time_buf[24];

    dst[0] = '\0';
    if (!u_append(dst, &pos, cap, "[UTEST-XML-SUMMARY] tests=")) return 0;
    if (!u_append_uint(dst, &pos, cap, tests)) return 0;
    if (!u_append(dst, &pos, cap, " failures=")) return 0;
    if (!u_append_uint(dst, &pos, cap, failures)) return 0;
    if (!u_append(dst, &pos, cap, " skipped=")) return 0;
    if (!u_append_uint(dst, &pos, cap, skipped)) return 0;
    u_format_seconds(time_buf, sizeof(time_buf), total_ms);
    if (!u_append(dst, &pos, cap, " time=")) return 0;
    if (!u_append(dst, &pos, cap, time_buf)) return 0;
    if (!u_append(dst, &pos, cap, " aborted=")) return 0;
    if (!u_append_uint(dst, &pos, cap, aborted ? 1u : 0u)) return 0;
    if (!u_append(dst, &pos, cap, " not_run=")) return 0;
    if (!u_append_uint(dst, &pos, cap, not_run)) return 0;
    /* `errors` is a SUBSET of `failures`, not a sibling of it: every
     * never-ran binary is counted in both, so the legacy summary line, the
     * serial recount and the TAP stream keep describing the same failure
     * population they always did. The host is what projects the pair into
     * the JUnit attributes (failures = failures - errors, errors = errors),
     * because that projection belongs where the document is assembled --
     * and the host reconciles this number against the <error> elements it
     * actually harvests before trusting it. Trailing, for the same reason
     * aborted=/not_run= are: every host parser is a greedy `.*<key>=`
     * sed plus an end-unanchored grep, so appending extends the line
     * without disturbing any existing extraction. */
    if (!u_append(dst, &pos, cap, " errors=")) return 0;
    if (!u_append_uint(dst, &pos, cap, errors)) return 0;
    return 1;
}

static void u_emit_xml_suite_close(uint32_t passed, uint32_t failed,
                                   uint32_t skipped, uint32_t skip_records,
                                   uint64_t total_ms, int aborted,
                                   uint32_t not_run, uint32_t errors)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_xml_mode) return;
    /* Emit a summary line (not a valid XML fragment on its own --
     * scripts/test.sh patches the opening <testsuite ...> from these
     * numbers). Separate line prefixed with [UTEST-XML-SUMMARY] so the
     * post-processor can grep for it distinctly from the <testcase>
     * and closer lines. */
    if (u_format_xml_summary(line, sizeof(line),
                             passed + failed + skipped + skip_records,
                             failed, skipped + skip_records, total_ms,
                             aborted, not_run, errors)) {
        utest_record_log(LOG_INFO, "%s", line);
    } else {
        /* Never publish a truncated summary: a short `tests=` reads as a
         * smaller-but-plausible run. Emit an unmistakable marker instead
         * and let the host gate fail the run on its presence. */
        utest_record_log(LOG_ERROR,
             "[UTEST-XML-SUMMARY-OVERFLOW] summary line exceeded %u bytes",
             (uint64_t)sizeof(line));
    }
    utest_record_log(LOG_INFO, "[UTEST-XML] </testsuite>");
}

/* Emit one `<testcase>` element for a binary. `verdict` is 0=PASS,
 * 1=FAIL, 2=SKIP, 3=ERROR. `reason` may be NULL; otherwise it's the
 * launcher-formatted reason string for FAIL/SKIP. `type` is the
 * taxonomy value that maps to the XML `classname` attribute (and
 * the JSON `type` field in the sibling emitter).
 *
 * Overflow contract: every u_append / u_xml_escape call is checked.
 * If any returns 0, we bail to an `overflow` label that emits a
 * minimal self-closing `<testcase name="..." classname="overflow"/>`
 * record so the CI artifact stays well-formed. The binary is logged
 * via LOG_WARN so the author knows their name/reason was too long.
 * Codex quality M 2026-04-20: glob-discovered names up to VFS_MAX_NAME
 * (256 chars) could otherwise exceed the line buffer and corrupt the
 * assembled XML file downstream. */
/* Split out of the emitter so the record can be BUILT without being
 * logged. The name bound is derived from these very literals, and a
 * derivation that only ever runs inside a klog call cannot be checked:
 * the unit tests format a bound-length name here and assert the record
 * fits, which is what proves UTEST_FIXED_XML still matches the format
 * string below rather than a copy of it that drifted. Returns 1 on
 * success, 0 if the record would not fit `cap`. */
static int u_format_xml_testcase(char *line, uint32_t cap, const char *name,
                                 utest_type_t type, int verdict,
                                 uint64_t time_ms, const char *reason,
                                 const char *classname_override)
{
    uint32_t pos = 0;
    char time_buf[24];

    if (cap == 0) return 0;
    line[0] = '\0';
    u_format_seconds(time_buf, sizeof(time_buf), u_clamp_time_ms(time_ms));

    #define APP(s)     do { if (!u_append(line, &pos, cap, (s))) goto overflow; } while (0)
    #define APP_XML(s) do { if (!u_xml_escape(line, &pos, cap, (s))) goto overflow; } while (0)

    APP("[UTEST-XML] <testcase name=\"");
    APP_XML(name);
    APP("\" classname=\"");
    /* Synthetic skip records override the taxonomy label so a JUnit
     * consumer can separate them from real binaries structurally, not
     * only by parsing the "::skipped-block-" name suffix. */
    APP(classname_override ? classname_override : u_type_label(type));
    APP("\" time=\"");
    APP(time_buf);
    APP("\"");

    if (verdict == 0) {
        APP("/>");
    } else {
        APP(">");
        /* JUnit's element convention, which this pipeline already follows
         * for the suite-abort testcase scripts/test.sh synthesizes: an
         * assertion the binary failed is a <failure>, and something that
         * stopped the binary from RUNNING AT ALL is an <error>. A refused
         * name and a launch that never produced a task belong to the
         * second class, so a consumer's errors= column reports exactly
         * the binaries that never executed. */
        if (verdict == 2) {
            APP("<skipped");
        } else if (verdict == 3) {
            APP("<error");
        } else {
            APP("<failure");
        }
        if (reason && reason[0]) {
            APP(" message=\"");
            APP_XML(reason);
            APP("\"");
        }
        APP("/>");
        APP("</testcase>");
    }
    #undef APP
    #undef APP_XML
    return 1;

overflow:
    return 0;
}

/* The XML record published when the real one did not fit.
 *
 * It must preserve the VERDICT and the classname, not merely stay
 * well-formed. A self-closing <testcase/> reads as PASSED, so an
 * overflowing skip record used to vanish from the failures/skipped
 * accounting while the suite header still counted it -- a record that
 * silently became a pass. The name is the only part dropped, because the
 * name is what did not fit.
 *
 * ERROR is preserved for the same reason one step further: the host
 * reconciles the producer's errors= against the number of <error> elements
 * it harvests, so a fallback that demoted an ERROR to a <failure> would
 * refuse the whole run for a count mismatch the fallback itself
 * introduced.
 *
 * Built rather than logged so it is TESTABLE. Its own emitter can only be
 * reached by overflowing a record, and the test policy forbids driving
 * live klog from a test -- so a fallback that quietly demoted a verdict
 * would have gone unnoticed until it refused somebody's real run. */
static int u_format_xml_overflow(char *dst, uint32_t cap, int verdict,
                                 const char *classname)
{
    uint32_t pos = 0;

    if (cap == 0) return 0;
    dst[0] = '\0';
    #define OAPP(s) do { if (!u_append(dst, &pos, cap, (s))) return 0; } while (0)
    OAPP("[UTEST-XML] <testcase name=\"overflow\" classname=\"");
    OAPP(classname);
    OAPP("\" time=\"0\"");
    if (verdict == 0) {
        OAPP("/>");
        return 1;
    }
    OAPP(">");
    OAPP(verdict == 2 ? "<skipped message=\"record name too long\"/>"
       : verdict == 3 ? "<error message=\"record name too long\"/>"
                      : "<failure message=\"record name too long\"/>");
    OAPP("</testcase>");
    #undef OAPP
    return 1;
}

static void u_emit_xml_testcase(const char *name, utest_type_t type,
                                 int verdict, uint64_t time_ms,
                                 const char *reason,
                                 const char *classname_override)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_xml_mode) return;
    if (u_format_xml_testcase(line, sizeof(line), name, type, verdict,
                              time_ms, reason, classname_override)) {
        utest_record_log(LOG_INFO, "%s", line);
        return;
    }
    utest_record_log(LOG_ERROR,
         "[UTEST-RECORD-OVERFLOW] XML record for '%s' exceeded its buffer",
         name);
    if (u_format_xml_overflow(line, sizeof(line), verdict,
                              classname_override ? classname_override
                                                 : u_type_label(type)))
        utest_record_log(LOG_INFO, "%s", line);
}

/* Build one `[UTEST-JSON] {...}` per binary. Split from its emitter for
 * the same reason as the XML formatter above: UTEST_FIXED_JSON_BINARY is
 * derived from these literals, and the tests format a bound-length name
 * here to prove the two have not drifted apart. Returns 1 on success, 0
 * if the record would not fit `cap`. */
static int u_format_json_testcase(char *line, uint32_t cap, const char *name,
                                  utest_type_t type, int verdict,
                                  uint64_t time_ms, const char *reason,
                                  const struct u_report *rep)
{
    uint32_t pos = 0;
    const char *status;

    if (cap == 0) return 0;
    line[0] = '\0';
    /* ERROR is the JSON side of the same never-ran distinction the XML
     * side draws with <error>: the binary never executed, so no assertion
     * of its own failed. It is a SUBSET of the FAIL population, not a
     * replacement for it -- summary.failed still counts these records, and
     * summary.errors states how many of them never ran, so a consumer
     * reading only `failed` sees the same number it always did. */
    switch (verdict) {
    case 0: status = "PASS"; break;
    case 2: status = "SKIP"; break;
    case 3: status = "ERROR"; break;
    default: status = "FAIL"; break;
    }

    #define APP(s)      do { if (!u_append(line, &pos, cap, (s))) goto overflow; } while (0)
    #define APP_JSON(s) do { if (!u_json_escape(line, &pos, cap, (s))) goto overflow; } while (0)
    #define APP_UINT(v) do { if (!u_append_uint(line, &pos, cap, (uint64_t)(v))) goto overflow; } while (0)

    /* record_kind is the stream's discriminator and leads every record.
     * The stream carries two kinds of object -- one per BINARY and one per
     * synthetic skip block -- and a consumer that counted every object
     * with a "name" would report more testcases than summary.total and
     * skew every pass rate derived from it. */
    APP("[UTEST-JSON] {\"record_kind\":\"binary\",\"name\":\"");
    APP_JSON(name);
    APP("\",\"type\":\"");
    APP(u_type_label(type));
    APP("\",\"status\":\"");
    APP(status);
    APP("\",\"time_ms\":");
    APP_UINT(u_clamp_time_ms(time_ms));
    if (reason && reason[0]) {
        APP(",\"reason\":\"");
        APP_JSON(reason);
        APP("\"");
    }
    /* Report dimension for this binary. Emitted only for an ACCEPTED
     * report: an invalid one already escalated the verdict to FAIL, and
     * republishing its counts would hand a consumer the very numbers the
     * kernel just refused to believe. JSON has no schema to violate here,
     * so the three-way outcome rides as named fields; XML gets it as
     * standard <skipped/> testcases instead. */
    /* Never for an ERROR record. This is not defensive coding, it is the
     * precondition UTEST_FIXED_JSON_NEVER_RAN is derived from: a never-ran
     * binary submitted no report, so the wide status and the report triple
     * can never appear in one record. Enforcing it HERE rather than
     * trusting every call site keeps the derivation true by construction
     * -- a future caller that passed a report with verdict 3 would
     * otherwise silently outgrow the reserved width. */
    if (verdict != 3 && rep && rep->state == TASK_UTEST_REPORT_VALID) {
        APP(",\"asserts_passed\":");
        APP_UINT(rep->asserts_passed);
        APP(",\"asserts_failed\":");
        APP_UINT(rep->asserts_failed);
        APP(",\"skip_blocks\":");
        APP_UINT(rep->skip_blocks);
    }
    APP("}");
    #undef APP
    #undef APP_JSON
    #undef APP_UINT
    return 1;

overflow:
    return 0;
}

/* The JSON record published when the real one did not fit.
 *
 * `record_kind` is kept because it is the stream's discriminator, and a
 * consumer following the documented contract drops any record without it
 * -- which would make an overflowing binary disappear entirely rather than
 * surface as the failure it is. The STATUS is preserved for the same
 * reason the XML fallback preserves its element: the harvester reconciles
 * the ERROR record count against summary.errors, so a fallback that
 * demoted ERROR to FAIL would refuse the run over a mismatch it created
 * itself. Every other verdict keeps the FAIL this fallback has always
 * published -- an overflowing PASS or SKIP is a record whose identity was
 * lost, and the resulting disagreement with the summary is what makes the
 * host refuse rather than publish it.
 *
 * Built rather than logged, for the same testability reason as the XML
 * fallback above. */
static int u_format_json_overflow(char *dst, uint32_t cap, int verdict)
{
    uint32_t pos = 0;

    if (cap == 0) return 0;
    dst[0] = '\0';
    #define JOAPP(s) do { if (!u_append(dst, &pos, cap, (s))) return 0; } while (0)
    JOAPP("[UTEST-JSON] {\"record_kind\":\"binary\",\"name\":\"overflow\",");
    JOAPP("\"status\":\"");
    JOAPP(verdict == 3 ? "ERROR" : "FAIL");
    JOAPP("\",\"time_ms\":0,\"reason\":\"record name too long\"}");
    #undef JOAPP
    return 1;
}

static void u_emit_json_testcase(const char *name, utest_type_t type,
                                  int verdict, uint64_t time_ms,
                                  const char *reason,
                                  const struct u_report *rep)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_json_mode) return;
    if (u_format_json_testcase(line, sizeof(line), name, type, verdict,
                               time_ms, reason, rep)) {
        utest_record_log(LOG_INFO, "%s", line);
        return;
    }
    utest_record_log(LOG_ERROR,
         "[UTEST-RECORD-OVERFLOW] JSON record for '%s' exceeded its buffer",
         name);
    if (u_format_json_overflow(line, sizeof(line), verdict))
        utest_record_log(LOG_INFO, "%s", line);
}

/* Build the [UTEST-JSON] summary body. Returns 1 on success, 0 if the
 * record would not fit. Same checked-append and testable-cap rationale as
 * u_format_xml_summary; this one matters more because a truncated JSON
 * object is not merely wrong, it is unparseable.
 *
 * The record carries the BINARY counts only. The assertion-level report
 * dimension it used to nest inline moved to its own `run_report` record
 * for transport reasons -- see u_format_json_run_report. The host
 * assembler nests it back under `summary.reported`, so the artifact's
 * shape is unchanged. */
static int u_format_json_summary(char *dst, uint32_t cap, uint32_t passed,
                                 uint32_t failed, uint32_t skipped,
                                 uint64_t total_ms, uint32_t errors)
{
    uint32_t pos = 0;

    dst[0] = '\0';
    #define JAPP(s)  do { if (!u_append(dst, &pos, cap, (s))) return 0; } while (0)
    #define JNUM(v)  do { if (!u_append_uint(dst, &pos, cap, (uint64_t)(v))) return 0; } while (0)

    JAPP("[UTEST-JSON] {\"summary\":{\"passed\":");
    JNUM(passed);
    JAPP(",\"failed\":");
    JNUM(failed);
    /* How many of `failed` never RAN. Emitted next to it rather than in
     * place of it: `failed` stays the whole non-pass-non-skip population,
     * so a consumer that only ever read `failed` sees no change, and the
     * harvester reconciles this number against the status=ERROR records
     * in the same stream. */
    JAPP(",\"errors\":");
    JNUM(errors);
    JAPP(",\"skipped\":");
    JNUM(skipped);
    JAPP(",\"total\":");
    JNUM(passed + failed + skipped);
    JAPP(",\"time_ms\":");
    JNUM(total_ms);
    JAPP("}}");
    #undef JAPP
    #undef JNUM
    return 1;
}

/* Build the assertion-level report record.
 *
 * Split out of the summary object for the same transport reason as
 * run_meta, and measured rather than assumed: with all seven counters
 * nested inline the live summary record reached 242 of the 255 bytes klog
 * can carry, so a suite an order of magnitude larger -- four-digit binary
 * counts, six-digit assertion counts -- overflowed it and the launcher
 * published an overflow marker instead of a summary. Each dimension now
 * gets its own record with room for every field at its uint32 maximum.
 *
 * The units stay separated exactly as before: assertion counts and binary
 * counts are different things, and the host assembler nests these back
 * under `summary.reported` so no consumer can read one as the other. */
static int u_format_json_run_report(char *dst, uint32_t cap,
                                    const struct u_report_totals *rt)
{
    uint32_t pos = 0;

    dst[0] = '\0';
    #define RAPP(s)  do { if (!u_append(dst, &pos, cap, (s))) return 0; } while (0)
    #define RNUM(v)  do { if (!u_append_uint(dst, &pos, cap, (uint64_t)(v))) return 0; } while (0)

    RAPP("[UTEST-JSON] {\"record_kind\":\"run_report\",\"asserts_passed\":");
    RNUM(rt->asserts_passed);
    RAPP(",\"asserts_failed\":");
    RNUM(rt->asserts_failed);
    RAPP(",\"skip_blocks\":");
    RNUM(rt->skip_blocks);
    RAPP(",\"skip_records\":");
    RNUM(rt->skip_records);
    RAPP(",\"binaries_reported\":");
    RNUM(rt->reported);
    RAPP(",\"binaries_invalid\":");
    RNUM(rt->invalid);
    RAPP(",\"binaries_unreported\":");
    RNUM(rt->unreported);
    RAPP("}");
    #undef RAPP
    #undef RNUM
    return 1;
}

/* Build the run-completeness record.
 *
 * This is a RECORD OF ITS OWN rather than two more fields on the summary,
 * and the reason is the transport. Every record reaches the host through
 * klog, whose ring entry is `message[256]` and which bounds the formatted
 * message to that size (klog_emit -> vformat_buf). The summary record
 * already measures 242 characters on a live run, so appending the
 * completeness pair to it produced a record longer than klog can carry:
 * the wire copy is silently cut mid-object, which for JSON means
 * unparseable rather than merely short. Measured 2026-07-29 -- the
 * assertion-report dimension had already spent nearly all the headroom.
 *
 * Splitting keeps every record small and leaves the summary's shape and
 * field order untouched; the host assembler folds this record into the
 * artifact's `summary` object, so a consumer reading the FILE still sees
 * one summary carrying `aborted` and `not_run`. `record_kind` is already
 * this stream's discriminator, so a third kind costs the consumer nothing
 * it was not already required to handle. */
static int u_format_json_run_meta(char *dst, uint32_t cap, int aborted,
                                  uint32_t not_run)
{
    uint32_t pos = 0;

    dst[0] = '\0';
    if (!u_append(dst, &pos, cap,
                  "[UTEST-JSON] {\"record_kind\":\"run_meta\",\"aborted\":"))
        return 0;
    if (!u_append(dst, &pos, cap, aborted ? "true" : "false")) return 0;
    if (!u_append(dst, &pos, cap, ",\"not_run\":")) return 0;
    if (!u_append_uint(dst, &pos, cap, not_run)) return 0;
    if (!u_append(dst, &pos, cap, "}")) return 0;
    return 1;
}

static void u_emit_json_summary(uint32_t passed, uint32_t failed,
                                uint32_t skipped,
                                const struct u_report_totals *rt,
                                uint64_t total_ms, uint32_t errors)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_json_mode) return;
    /* Report dimension first, summary second: the summary is the record the
     * host anchors the stream's tail on, so everything it will be assembled
     * with must already be on the wire. */
    if (u_format_json_run_report(line, sizeof(line), rt)) {
        utest_record_log(LOG_INFO, "%s", line);
    } else {
        utest_record_log(LOG_ERROR,
             "[UTEST-JSON] {\"summary_error\":\"overflow\"}");
        utest_record_log(LOG_ERROR,
             "[UTEST-JSON-SUMMARY-OVERFLOW] run_report record exceeded %u bytes",
             (uint64_t)sizeof(line));
        return;
    }
    if (u_format_json_summary(line, sizeof(line), passed, failed, skipped,
                              total_ms, errors)) {
        utest_record_log(LOG_INFO, "%s", line);
    } else {
        /* Syntactically valid JSON that cannot be mistaken for a run
         * summary, so a consumer fails rather than reading a truncated
         * object as zero tests. */
        utest_record_log(LOG_ERROR,
             "[UTEST-JSON] {\"summary_error\":\"overflow\"}");
        utest_record_log(LOG_ERROR,
             "[UTEST-JSON-SUMMARY-OVERFLOW] summary record exceeded %u bytes",
             (uint64_t)sizeof(line));
    }
}

static void u_emit_json_run_meta(int aborted, uint32_t not_run)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_json_mode) return;
    if (u_format_json_run_meta(line, sizeof(line), aborted, not_run)) {
        utest_record_log(LOG_INFO, "%s", line);
    } else {
        /* Two fixed labels and one decimal count cannot outgrow a 256-byte
         * buffer, so reaching here means the buffer contract itself
         * regressed. Say so on the same overflow channel the host already
         * fails on rather than dropping the record: a missing completeness
         * record is exactly the ambiguity it exists to remove. */
        utest_record_log(LOG_ERROR,
             "[UTEST-JSON-SUMMARY-OVERFLOW] run_meta record exceeded %u bytes",
             (uint64_t)sizeof(line));
    }
}

/* Build the [UTEST-REPORT-SUMMARY] body -- the report dimension's own
 * line, deliberately NOT folded into the launcher's legacy
 * `=== N passed, N failed, N skipped of N total ===` summary.
 *
 * That legacy line is parsed by scripts/test.sh with position-sensitive
 * sed expressions AND is the string its boot-completion poll waits for;
 * appending a second passed/failed/skipped triplet to it would let the
 * greedy patterns capture the wrong numbers and silently redefine what
 * `make test` counts as a failure. Separate tag, separate parser. */
static int u_format_report_summary(char *dst, uint32_t cap,
                                   const struct u_report_totals *rt)
{
    uint32_t pos = 0;

    dst[0] = '\0';
    #define RAPP(s)  do { if (!u_append(dst, &pos, cap, (s))) return 0; } while (0)
    #define RNUM(v)  do { if (!u_append_uint(dst, &pos, cap, (uint64_t)(v))) return 0; } while (0)

    RAPP("[UTEST-REPORT-SUMMARY] asserts_passed=");
    RNUM(rt->asserts_passed);
    RAPP(" asserts_failed=");
    RNUM(rt->asserts_failed);
    RAPP(" skip_blocks=");
    RNUM(rt->skip_blocks);
    RAPP(" skip_records=");
    RNUM(rt->skip_records);
    RAPP(" reported=");
    RNUM(rt->reported);
    RAPP(" invalid=");
    RNUM(rt->invalid);
    RAPP(" unreported=");
    RNUM(rt->unreported);
    #undef RAPP
    #undef RNUM
    return 1;
}

static void u_emit_report_summary(const struct u_report_totals *rt)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (u_format_report_summary(line, sizeof(line), rt))
        utest_record_log(LOG_INFO, "%s", line);
    else
        utest_record_log(LOG_ERROR,
             "[UTEST-REPORT-SUMMARY-OVERFLOW] report summary exceeded %u bytes",
             (uint64_t)sizeof(line));
}

/* How many of `requested` skip records may still be emitted, given
 * `already_emitted` across the run so far. Pure, so the aggregate stop
 * can be unit-tested without spawning anything: it is the arithmetic that
 * decides whether a hostile fan-out is cut off, and an off-by-one here
 * either truncates a legitimate run's artifacts or leaves the amplifica-
 * tion path open by one binary's worth of records. */
static uint32_t u_skip_records_allowed(uint32_t already_emitted,
                                       uint32_t requested)
{
    uint32_t room = (already_emitted < UTEST_SKIP_RECORD_BUDGET)
                        ? (UTEST_SKIP_RECORD_BUDGET - already_emitted) : 0u;
    return (requested < room) ? requested : room;
}

/* Emit one synthetic skip-block record into the JSON stream.
 *
 * Separate from u_emit_json_testcase because it is a DIFFERENT record
 * kind, not a testcase with odd fields: it carries `record_kind`,
 * the `parent` binary it belongs to, and its `skip_index`, so a consumer
 * can group the records under their binary, count binaries and skip
 * blocks separately, and reconcile both against the summary. Reusing the
 * testcase emitter is what made the two indistinguishable.
 *
 * Same bounded-append + overflow-fallback contract as its sibling. */
/* Split from its emitter for the same reason as the two testcase
 * formatters -- and this is the one that matters most: the skip_block
 * carries the name TWICE, so it is the kind that BINDS the derived
 * bound. UTEST_FIXED_JSON_SKIP is a second copy of the literals below,
 * and only formatting a bound-length name through this function proves
 * the copy still matches. Returns 1 on success, 0 if it would not fit. */
static int u_format_json_skip_record(char *line, uint32_t cap,
                                     const char *rec_name, const char *parent,
                                     uint32_t index)
{
    uint32_t pos = 0;

    if (cap == 0) return 0;
    line[0] = '\0';

    #define APP(s)      do { if (!u_append(line, &pos, cap, (s))) goto overflow; } while (0)
    #define APP_JSON(s) do { if (!u_json_escape(line, &pos, cap, (s))) goto overflow; } while (0)
    #define APP_UINT(v) do { if (!u_append_uint(line, &pos, cap, (uint64_t)(v))) goto overflow; } while (0)

    APP("[UTEST-JSON] {\"record_kind\":\"skip_block\",\"name\":\"");
    APP_JSON(rec_name);
    APP("\",\"parent\":\"");
    APP_JSON(parent);
    APP("\",\"skip_index\":");
    APP_UINT(index);
    APP(",\"status\":\"SKIP\",\"reason\":\"sub-test block skipped "
        "(reason on serial log)\"}");
    #undef APP
    #undef APP_JSON
    #undef APP_UINT
    return 1;

overflow:
    return 0;
}

static void u_emit_json_skip_record(const char *rec_name, const char *parent,
                                    uint32_t index)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_json_mode) return;
    if (u_format_json_skip_record(line, sizeof(line), rec_name, parent,
                                  index)) {
        utest_record_log(LOG_INFO, "%s", line);
        return;
    }
    utest_record_log(LOG_ERROR,
         "[UTEST-RECORD-OVERFLOW] JSON skip record for '%s' exceeded its buffer",
         parent);
    utest_record_log(LOG_INFO,
         "[UTEST-JSON] {\"record_kind\":\"skip_block\",\"name\":\"overflow\","
         "\"parent\":\"overflow\",\"skip_index\":0,\"status\":\"SKIP\","
         "\"reason\":\"record name too long\"}");
}

/* Build the synthetic record name for one skipped block:
 * `<binary>::skipped-block-<k>`. Returns 1 on success, 0 if it would not
 * fit (the caller then skips the synthetic record rather than emitting a
 * truncated, ambiguous name).
 *
 * The label is deliberately positional rather than descriptive: the
 * kernel receives a COUNT, not the identity of each skipped block, and
 * inventing a plausible-looking test name would be a fabricated identity
 * in an artifact whose whole purpose is to stop lying about coverage.
 * `::skipped-block-K` says exactly what is known -- the K-th skip site
 * this binary took -- and the reason text stays on serial where the
 * harness printed it. */
static int u_build_skip_record_name(char *dst, uint32_t cap,
                                    const char *base, uint32_t k)
{
    uint32_t pos = 0;

    dst[0] = '\0';
    if (!u_append(dst, &pos, cap, base)) return 0;
    if (!u_append(dst, &pos, cap, "::skipped-block-")) return 0;
    if (!u_append_uint(dst, &pos, cap, k)) return 0;
    return 1;
}

/* Emit one TAP point + one <testcase><skipped/> + one JSON record per
 * skip block an ACCEPTED report declared, and advance the TAP point
 * counter for each.
 *
 * Why records and not just a count on the binary's own record: the
 * binary passed, so its record must stay `ok` / non-skipped -- calling a
 * partly-verified binary skipped is the error the section's warning box
 * forbids. But leaving the skip only in prose means TAP's `# SKIP` and
 * JUnit's `skipped=` still read zero, which is the false-coverage signal
 * the section exists to remove. One record per skip block satisfies both:
 * the standard fields become literally accurate and the binary keeps its
 * true verdict. */
/* Emit one TAP point, bounded by the same transport cap as every other
 * record and FAIL-CLOSED on overflow.
 *
 * TAP puts its directive LAST (`ok 7 - name # SKIP`), and klog truncates at
 * its message field -- so a long enough binary name silently cut the
 * `# SKIP` off the end and turned a skipped test into a bare passing `ok`,
 * with no marker anywhere. Skip-record points are worse: their synthetic
 * name is built into a VFS_MAX_NAME+24 buffer, so for a long parent the
 * directive was ALWAYS lost.
 *
 * A point we cannot represent faithfully is emitted as `not ok` with an
 * overflow directive instead. Turning an unrepresentable record into a
 * failure is the only safe direction: the alternative reads as a pass. */
static int u_format_tap_point(char *dst, uint32_t cap, int ok, uint32_t point,
                              const char *name, const char *directive)
{
    uint32_t pos = 0;
    const char *p;

    if (cap == 0) return 0;
    dst[0] = '\0';
    /* A name carrying `#` would inject a TAP directive of its own: TAP reads
     * everything after ` # ` as SKIP/TODO, so a binary called
     * `test_a # SKIP .exe` turns its own point into a skip. Names are
     * checked by u_is_valid_manifest_name, which rejects path characters
     * and control bytes but PERMITS `#` and spaces -- and glob-discovered
     * names come from the filesystem, not the manifest. Refuse rather than
     * emit a point a binary chose the meaning of. */
    for (p = name; p && *p; p++)
        if (*p == '#')
            return 0;

    if (!u_append(dst, &pos, cap, ok ? "ok " : "not ok ")) return 0;
    if (!u_append_uint(dst, &pos, cap, point)) return 0;
    if (!u_append(dst, &pos, cap, " - ")) return 0;
    if (!u_append(dst, &pos, cap, name)) return 0;
    if (directive && directive[0]) {
        if (!u_append(dst, &pos, cap, " # ")) return 0;
        if (!u_append(dst, &pos, cap, directive)) return 0;
    }
    return 1;
}

static void u_emit_tap_point(int ok, uint32_t point, const char *name,
                             const char *directive)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_tap_mode) return;

    if (u_format_tap_point(line, sizeof(line), ok, point, name, directive)) {
        utest_record_log(LOG_INFO, "%s", line);
        return;
    }
    /* Fail CLOSED. A point we cannot represent faithfully -- too long for
     * the transport, or carrying a forgeable directive -- becomes a
     * FAILURE, never a silent pass. */
    utest_record_log(LOG_ERROR,
         "[UTEST-RECORD-OVERFLOW] TAP point %u could not be represented",
         (uint64_t)point);
    utest_record_log(LOG_INFO,
         "not ok %u - unrepresentable # TAP record refused",
         (uint64_t)point);
}

static uint32_t u_emit_skip_records(const char *name, utest_type_t type,
                                    const struct u_report *rep,
                                    uint32_t already_emitted,
                                    uint32_t *tap_point)
{
    char rec[VFS_MAX_NAME + 24];
    uint32_t k, allowed;

    if (!rep || rep->state != TASK_UTEST_REPORT_VALID || rep->skip_blocks == 0)
        return 0;

    /* No machine artifact requested means no records exist to count. The
     * three emitters below are individually no-ops when their mode is
     * off, so without this the default run would report skip_records it
     * never emitted -- and could even trip the run-wide budget and fail a
     * run that asked for no fan-out-producing artifact at all. The skip
     * BLOCKS themselves are still counted and reported; it is the
     * synthetic RECORDS that do not exist here. */
    if (!s_tap_mode && !s_xml_mode && !s_json_mode)
        return 0;

    /* RUN-WIDE budget, on top of the per-binary ceiling the syscall
     * enforces. The per-task bound is multiplicative: TASK_MAX binaries
     * each reporting the maximum are all individually legal and
     * collectively demand tens of thousands of records. This is the
     * aggregate stop. Exceeding it is never silently truncated -- one
     * explicit marker names what was requested, emitted and omitted, and
     * the host gate counts that marker as a run failure, because an
     * artifact knowingly missing records is not a passing run. */
    allowed = u_skip_records_allowed(already_emitted, rep->skip_blocks);
    if (allowed < rep->skip_blocks) {
        utest_record_log(LOG_ERROR,
             "[UTEST-SKIP-RECORD-BUDGET] %s requested=%u emitted=%u "
             "omitted=%u budget=%u",
             name, (uint64_t)rep->skip_blocks, (uint64_t)allowed,
             (uint64_t)(rep->skip_blocks - allowed),
             (uint64_t)UTEST_SKIP_RECORD_BUDGET);
        if (allowed == 0)
            return 0;
    }

    for (k = 1; k <= allowed; k++) {
        if (!u_build_skip_record_name(rec, sizeof(rec), name, k)) {
            /* Same machine-readable marker its two siblings use (the
             * budget clip and the record overflow), so the host gate
             * fails the run. Omitting records with only a WARN would keep
             * the run GREEN on artifacts that under-report skips, which
             * is the exact false-coverage class this section closes. */
            utest_record_log(LOG_ERROR,
                 "[UTEST-RECORD-OVERFLOW] %s skip-record name too long -- "
                 "%u record(s) omitted",
                 name, (uint64_t)(rep->skip_blocks - k + 1));
            return k - 1;
        }
        (*tap_point)++;
        u_emit_tap_point(1, *tap_point, rec, "SKIP reported by binary");
        u_emit_xml_testcase(rec, type, 2, 0,
                            "sub-test block skipped (reason on serial log)",
                            "skip-block");
        /* The JSON record carries an explicit discriminator. Without one a
         * consumer counting objects with a "name" field would count these
         * as BINARIES -- the stream would show more testcases than
         * summary.total and every pass-rate derived from it would be
         * wrong. XML discriminates structurally (these are the only
         * records with classname="skip-block"); JSON has no such
         * convention, so it gets the fields. */
        u_emit_json_skip_record(rec, name, k);
    }
    return allowed;
}

/* Per-binary report diagnostic. Separate from the verdict line so the
 * host cross-check can recount the report dimension from serial without
 * re-parsing verdict text, and so a NONE binary produces no line at all
 * (absence is the legacy signal). */
static void u_emit_report_line(const char *name, const struct u_report *rep)
{
    if (!rep || rep->state == TASK_UTEST_REPORT_NONE)
        return;
    utest_record_log(LOG_INFO,
         "[UTEST-REPORT] %s asserts_passed=%u asserts_failed=%u "
         "skip_blocks=%u state=%s",
         name, (uint64_t)rep->asserts_passed, (uint64_t)rep->asserts_failed,
         (uint64_t)rep->skip_blocks,
         rep->state == TASK_UTEST_REPORT_VALID ? "VALID" : "INVALID");
}

/* Compare two binary names the way the FILESYSTEM does.
 *
 * C: is IXFS (mounted in partition.c), and IXFS resolves directory names
 * case-insensitively over ASCII -- `ixfs_strcmp` folds, matching the VFS
 * uppercase fold in walk_path. So `test_Bad.exe` in a manifest and
 * `test_bad.exe` on disk are the SAME FILE, and a bytewise comparison
 * treats them as two. Every identity decision the launcher makes between
 * a manifest entry and a dirent has to use the filesystem's notion of
 * sameness, or it plans, runs or refuses one file twice.
 *
 * Only ASCII letters need folding: the accepted charset is
 * [A-Za-z0-9._-], and this is reached only for names a dirent could
 * actually carry.
 *
 * Used for EVERY identity decision now -- refusals and accepted entries
 * alike. The accepted-entry dedup used to stay bytewise for one specific
 * reason: it is followed by a filter match, and `utest_filter=` compared
 * literally, so a folded dedup could suppress an entry whose twin the
 * filter had already rejected on case and run the requested binary zero
 * times while total_planned and total_ran still agreed. That coupling is
 * gone because `test_usermode_glob_match` now folds too, which is the
 * user-visible half of this section: filter and filesystem finally agree
 * on what one name means. */
static int u_name_equal_fs(const char *a, const char *b, uint32_t cap)
{
    uint32_t i;

    for (i = 0; i < cap; i++) {
        char ca = u_fold(a[i]), cb = u_fold(b[i]);
        if (ca != cb)
            return 0;
        if (ca == '\0')
            return 1;
    }
    return 1;
}

/* ---- Enumeration-plan construction --------------------------------- */

/* Defined just below, beside the refusal storage they read. */
static int u_refusal_already_seen(const struct manifest_state *ms,
                                  const char *name);
static int u_refusal_index_of(const struct manifest_state *ms,
                              const char *name);

/* Does this name terminate inside the filesystem's name bound?
 *
 * u_name_equal_fs compares up to a cap and reports EQUAL when it runs off
 * the end of both strings without finding a terminator -- fine when one
 * side is a dirent, which always terminates within the bound, and wrong
 * when both sides are refused manifest names, which may be over-length
 * precisely because that is one reason to refuse them. Two 300-byte names
 * sharing their first 256 bytes are different files, and a name past this
 * bound is not a filesystem identity at all. */
static int u_name_bounded(const char *name)
{
    uint32_t i;

    for (i = 0; i < VFS_MAX_NAME; i++)
        if (!name[i])
            return 1;
    return 0;
}

static int u_plan_init(struct plan_state *ps)
{
    ps->entries      = (struct plan_entry *)0;
    ps->names        = (char *)0;
    ps->count             = 0;
    ps->names_used        = 0;
    ps->skipped_by_filter = 0;
    ps->smoke_refused     = 0;
    ps->block_phys        = 0;
    ps->overflowed   = 0;
    ps->kept_at_overflow = 0;
    ps->alloc_failed = 0;

    /* Well past kmalloc's 4 KiB ceiling, so PMM per CLAUDE.md's
     * freestanding rules. Physical pages are identity-mapped in the
     * kernel VA, so the physical base doubles as a valid pointer -- the
     * same relationship the manifest arena above relies on. */
    ps->block_phys = pmm_alloc_contiguous(UTEST_PLAN_PAGES);
    if (!ps->block_phys) {
        /* NOT merely a degraded run. Without a plan the launcher cannot
         * enumerate at all, and the zero-planned path publishes a
         * successful empty suite -- so this has to reach the caller as a
         * distinct state that fails the run from storage which does not
         * depend on this allocation. */
        ps->alloc_failed = 1;
        klog(LOG_WARN, "UTEST",
             "plan pmm_alloc_contiguous(%u pages) failed -- counted as a "
             "failure, no binary runs",
             (uint64_t)UTEST_PLAN_PAGES);
        return 0;
    }
    ps->entries = (struct plan_entry *)ps->block_phys;
    ps->names   = (char *)(ps->block_phys
                           + (uintptr_t)UTEST_PLAN_MAX
                             * sizeof(struct plan_entry));
    return 1;
}

static void u_plan_free(struct plan_state *ps)
{
    if (ps->block_phys) {
        uint32_t p;
        for (p = 0; p < UTEST_PLAN_PAGES; p++)
            pmm_free_frame(ps->block_phys + (uintptr_t)p * 4096u);
        ps->block_phys = 0;
        ps->entries    = (struct plan_entry *)0;
        ps->names      = (char *)0;
    }
    ps->count = 0;
}

/* Copy a name into the plan's own arena.
 *
 * Only glob-sourced names need this: vfs_readdir hands back shared dirent
 * storage that the next call reuses, so a plan entry pointing at it would
 * read a different file's name by the time execution consumed it.
 * Manifest names are already stable in the manifest arena and are stored
 * by pointer.
 *
 * The fixed stride can only truncate a name longer than the slot, and the
 * static assert beside UTEST_MAX_BINARY_NAME proves no ACCEPTED name can
 * be; refusal prefixes are sanitized to UTEST_REFUSAL_PREFIX_STORE before
 * they get here.
 *
 * The exhaustion arm below is UNREACHABLE while the arena assert holds
 * (one slot per entry, and an entry slot is always taken first), and is
 * kept fail-closed anyway so a future resizing that breaks the
 * relationship refuses to plan a truncated identity rather than silently
 * planning one. */
static const char *u_plan_intern(struct plan_state *ps, const char *name)
{
    char *dst;
    uint32_t i;

    if (!ps->entries)
        return (const char *)0;
    if (ps->names_used + UTEST_PLAN_NAME_SLOT > UTEST_PLAN_NAME_BYTES) {
        if (!ps->overflowed)
            ps->kept_at_overflow = ps->count;
        ps->overflowed = 1;
        return (const char *)0;
    }
    dst = ps->names + ps->names_used;
    for (i = 0; i + 1u < UTEST_PLAN_NAME_SLOT && name[i]; i++)
        dst[i] = name[i];
    dst[i] = '\0';
    ps->names_used += UTEST_PLAN_NAME_SLOT;
    return dst;
}

static struct plan_entry *u_plan_alloc(struct plan_state *ps)
{
    struct plan_entry *e;

    /* An unallocated plan is NOT an overflowing one. Reporting both would
     * reserve two aggregate slots for one failure and publish a capacity
     * problem the run never had. alloc_failed already has its own
     * aggregate, so this returns without touching `overflowed`. */
    if (ps->alloc_failed)
        return (struct plan_entry *)0;
    if (!ps->entries || ps->count >= UTEST_PLAN_MAX) {
        if (!ps->overflowed)
            ps->kept_at_overflow = ps->count;
        ps->overflowed = 1;
        return (struct plan_entry *)0;
    }
    e = &ps->entries[ps->count++];
    e->name          = (const char *)0;
    e->reason        = (const char *)0;
    e->digest        = 0;
    e->type          = UTEST_TYPE_CORRECTNESS;
    e->kind          = (uint8_t)UTEST_PLAN_RUN;
    e->verdict       = 0;
    e->expects_tasks = 1u;
    e->exact_name     = 1u;
    e->smoke_selected = 0u;
    return e;
}

/* Plan one runnable binary.
 *
 * Returns 1 when the identity is represented in the plan (freshly added,
 * folded into an identical earlier entry, or converted to a counted
 * policy-conflict failure) and 0 only when the plan could not hold it.
 *
 * The dedup is case-FOLDED because IXFS resolves names that way: two
 * manifest lines spelling one file differently are one binary, and
 * planning both would run it twice under two ordinals. But filesystem
 * identity does not settle manifest POLICY -- the two lines can carry
 * different `type=` or `expects_tasks=` values, and silently keeping
 * either one can demote a smoke test out of the fast-fail phase or
 * understate the task budget while every count still reconciles. So an
 * identical duplicate folds away, and a conflicting one is refused
 * outright: the earlier entry becomes the counted failure and the binary
 * runs under neither policy. */
static int u_plan_add_run(struct plan_state *ps, const char *name,
                          utest_type_t type, uint8_t expects, int intern)
{
    struct plan_entry *e;
    uint32_t i;

    /* The filter is applied HERE, at plan time, for every source. An entry
     * the filter excludes never enters the plan, so it is neither planned
     * nor expected to run and the completeness reconciliation stays exact
     * by construction. The previous code filtered in BOTH walks and had to
     * special-case phase 0 to keep from counting the same exclusion twice.
     * Refusals deliberately do NOT come through here: the filter selects
     * among identities the launcher can trust, and letting a name it just
     * declined decide whether it gets reported would hand back the
     * authority the refusal withdrew. */
    if (!test_usermode_glob_match(s_filter, name)) {
        ps->skipped_by_filter++;
        return 1;
    }

    for (i = 0; i < ps->count; i++) {
        struct plan_entry *prev = &ps->entries[i];

        if (prev->kind == (uint8_t)UTEST_PLAN_REFUSAL) {
            /* A refusal is TERMINAL for its filesystem identity. Matching
             * only RUN entries left a hole a crafted manifest could walk
             * through: once two conflicting variants had converted the
             * first entry to a refusal, a THIRD variant of the same name
             * matched nothing and took a fresh RUN slot -- so the run
             * published the identity as refused AND executed it, with
             * every count still reconciling and the completeness gate
             * blind. Same rule the manifest-versus-dirent dedup already
             * applies, now enforced inside the plan too.
             *
             * Only EXACT identities are terminal. A sanitized or
             * truncated glob prefix is not the name it came from, and
             * suppressing a legitimate binary that merely collides with
             * one is a silently missing test rather than a loud duplicate.
             *
             * REFUSE_NUL is excluded for the reason u_refusal_already_seen
             * excludes it: the stored string is the truncation at the
             * embedded NUL, not a filename, so a name matching it is a
             * DIFFERENT file that would be wrongly suppressed. */
            if (!prev->exact_name)
                continue;
            if (prev->verdict == (uint8_t)UTEST_NAME_REFUSE_NUL)
                continue;
            if (!u_name_equal_fs(prev->name, name, VFS_MAX_NAME))
                continue;
            /* MERGE the incoming declaration's policy into the refusal
             * rather than dropping it on the floor. This declaration was
             * classified and filtered like any other, so its `type=` is
             * trusted -- and a smoke declaration that gets suppressed by
             * an earlier refusal is still a refused smoke prerequisite.
             * Without the merge, correctness-then-stress-then-smoke left
             * the identity refused with no smoke provenance anywhere, and
             * the gate could not see it. */
            if (type == UTEST_TYPE_SMOKE)
                prev->smoke_selected = 1u;
            return 1;   /* already published as refused; never runs */
        }
        if (!u_name_equal_fs(prev->name, name, VFS_MAX_NAME))
            continue;
        if (prev->type == type && prev->expects_tasks == expects)
            return 1;   /* one file, one policy, one plan slot */
        /* If EITHER declaration called this a smoke binary, the identity
         * is a smoke prerequisite and refusing it is a smoke non-PASS.
         * Recorded before the type is overwritten below. Without this the
         * conflict quietly demoted a declared smoke gate into an ordinary
         * counted failure and let the rest of the suite run on -- with
         * total_planned == total_ran, so neither the completeness gate nor
         * the artifact reconciliation could see the bypass. */
        /* The refusal RECORD carries the policy, so the gate is derived
         * from the plan rather than raised by whichever branch happened to
         * notice. Both declarations reached here THROUGH the filter, so
         * either one calling this a smoke binary makes the refused
         * identity a selected smoke prerequisite. */
        if (prev->type == UTEST_TYPE_SMOKE || type == UTEST_TYPE_SMOKE)
            prev->smoke_selected = 1u;
        prev->kind    = (uint8_t)UTEST_PLAN_REFUSAL;
        prev->reason  = UTEST_RSN_DUP_POLICY;
        prev->verdict = (uint8_t)UTEST_NAME_ACCEPT;
        prev->digest  = 0;
        klog(LOG_WARN, "UTEST",
             "manifest names one file under conflicting policy -- refusing "
             "the identity instead of guessing which line wins");
        return 1;
    }

    e = u_plan_alloc(ps);
    if (!e)
        return 0;
    if (intern) {
        e->name = u_plan_intern(ps, name);
        if (!e->name) {
            /* Roll the slot back so the plan never carries a nameless
             * entry: the caller's fail-closed aggregate is the only thing
             * that may describe an enumeration this incomplete. */
            ps->count--;
            return 0;
        }
    } else {
        e->name = name;
    }
    e->type          = type;
    e->kind          = (uint8_t)UTEST_PLAN_RUN;
    e->expects_tasks = expects;
    return 1;
}

/* Plan one refused identity. `reason` NULL derives the reason from the
 * verdict at publication; a literal pins it (the aggregates). `exact`
 * says whether `name` is the true filesystem identity or a lossy
 * rendering of it -- see plan_entry::exact_name. */
static int u_plan_add_refusal(struct plan_state *ps, const char *name,
                              uint32_t digest, uint8_t verdict,
                              const char *reason, int intern, int exact,
                              int smoke_selected)
{
    struct plan_entry *e;

    /* One filesystem identity gets ONE refusal record.
     *
     * Two case variants of a name can be refused for DIFFERENT reasons --
     * a duplicate-policy conflict converts one entry, and a third variant
     * with an unusable attribute arrives separately from the manifest's
     * refusal array -- and appending both published two failures for one
     * file. The counts still reconciled, so nothing downstream could tell
     * the operator that the two records were the same binary.
     *
     * Only EXACT identities can be compared this way, for the same reason
     * the terminal-refusal rule requires it: a sanitized glob prefix is
     * not the name it came from. First reason wins; both are failures and
     * the run is red either way. */
    /* The INCOMING verdict is checked as well as the stored one. A
     * REFUSE_NUL entry's C string stops at the embedded NUL, so it is a
     * truncation and not a filename: comparing it against an ordinary
     * refusal would let `test_bad\0.exe` collapse into an existing
     * `test_bad` record and lose its distinct span digest, with every
     * published count still reconciling. It is never an identity in
     * either direction. */
    if (exact && verdict != (uint8_t)UTEST_NAME_REFUSE_NUL &&
        u_name_bounded(name)) {
        uint32_t i;
        for (i = 0; i < ps->count; i++) {
            const struct plan_entry *prev = &ps->entries[i];

            if (prev->kind != (uint8_t)UTEST_PLAN_REFUSAL || !prev->exact_name)
                continue;
            if (prev->verdict == (uint8_t)UTEST_NAME_REFUSE_NUL)
                continue;   /* a truncation, not a filename */
            /* Both sides must terminate inside the bound. A length-refused
             * name can run past it, and u_name_equal_fs reports EQUAL when
             * it falls off the end of both strings -- so two distinct
             * over-length names sharing their first VFS_MAX_NAME bytes
             * would collapse into one record and the second span digest
             * would be lost with every count still reconciling. */
            if (!u_name_bounded(prev->name))
                continue;
            if (u_name_equal_fs(prev->name, name, VFS_MAX_NAME)) {
                /* MERGE before returning. Collapsing a later refusal into
                 * an earlier one for the same identity must not discard
                 * what the later declaration said: refusing `test_gate.exe`
                 * under correctness and then again as a selected
                 * `type=smoke` is still a refused smoke prerequisite, and
                 * dropping that here reproduced the exact bypass the
                 * derivation was introduced to close. */
                if (smoke_selected)
                    ps->entries[i].smoke_selected = 1u;
                return 1;   /* already published under this identity */
            }
        }
    }

    e = u_plan_alloc(ps);
    if (!e)
        return 0;
    if (intern) {
        e->name = u_plan_intern(ps, name);
        if (!e->name) {
            ps->count--;
            return 0;
        }
    } else {
        e->name = name;
    }
    e->kind       = (uint8_t)UTEST_PLAN_REFUSAL;
    e->digest     = digest;
    e->verdict    = verdict;
    e->reason     = reason;
    e->exact_name     = exact ? 1u : 0u;
    e->smoke_selected = smoke_selected ? 1u : 0u;
    return 1;
}

/* Drop every planned runnable whose identity the MANIFEST refused.
 *
 * Ordering, not policy: the refusal set is complete only once the whole
 * file has been parsed, while accepted lines enter the plan as they are
 * read. So `test_x.exe type=bogus` followed by a case variant of the same
 * name would otherwise plan the identity as runnable and publish its
 * refusal afterwards -- refusing and executing one file in the same run,
 * with both counters agreeing so the completeness gate stays blind. The
 * old two-walk code got this for free because its planning walk ran after
 * the loader had finished; appending during the parse is what made the
 * ordering explicit work.
 *
 * The entry is REMOVED rather than converted, because the refusal itself
 * is published separately from manifest_state -- converting would publish
 * the same identity twice. */
static void u_plan_suppress_refused(struct plan_state *ps,
                                    struct manifest_state *ms)
{
    uint32_t i, out = 0;

    for (i = 0; i < ps->count; i++) {
        if (ps->entries[i].kind == (uint8_t)UTEST_PLAN_RUN &&
            u_refusal_already_seen(ms, ps->entries[i].name)) {
            /* TRANSFER the removed entry's policy onto the refusal that
             * vetoed it, instead of raising a flag here. The removed entry
             * was a trusted declaration -- it was classified and filtered
             * like any other runnable -- so if it declared smoke, the
             * identity the refusal now stands for is a refused smoke
             * prerequisite, and the derived gate must be able to see that
             * from the record alone. */
            if (ps->entries[i].type == UTEST_TYPE_SMOKE) {
                int idx = u_refusal_index_of(ms, ps->entries[i].name);
                if (idx >= 0)
                    ms->refused_smoke_selected[idx] = 1u;
            }
            continue;
        }
        if (out != i)
            ps->entries[out] = ps->entries[i];
        out++;
    }
    ps->count = out;
}

/* Does a freshly computed identity match the one the plan froze?
 *
 * NULL `expect` is a MISMATCH, not a pass. A RUN entry always carries a
 * frozen identity (u_plan_freeze_identities refuses the ones it cannot
 * freeze), so a NULL here means the launcher lost the plan's identity
 * between freezing it and spawning -- and "we no longer know what was
 * supposed to run" must never be the branch that lets bytes execute.
 *
 * The compare is constant-time over the whole digest. A byte-at-a-time
 * compare that returns on the first difference leaks how many leading
 * bytes matched, which turns the verifier into a search oracle for the
 * very collision the digest width exists to make infeasible. */
static int u_identity_matches(const uint8_t *expect,
                              const uint8_t *actual)
{
    uint32_t d, diff = 0;

    if (!expect)
        return 0;
    for (d = 0; d < SHA256_DIGEST_LEN; d++)
        diff |= (uint32_t)(expect[d] ^ actual[d]);
    return diff == 0;
}

/* Pages in the buffer streamed through while digesting a planned binary.
 *
 * Allocated once for the whole freeze pass and reused across every entry:
 * the alternative is a buffer the size of the largest binary, which is the
 * kind of allocation CLAUDE.md's freestanding rules push onto
 * pmm_alloc_contiguous anyway, and which would scale with a file whose size
 * the launcher does not control. Streaming keeps the cost of freezing N
 * binaries bounded by this buffer regardless of N or of any file's size.
 *
 * SIXTEEN pages and not one, because a vfs_read is far from free on the
 * path this actually runs: ixfs_file_read (src/kernel/fs/ixfs/ixfs_ops.c)
 * kmallocs and frees an IXFS_BLOCK_SIZE bounce buffer on EVERY call, and
 * then stamps i_atime and writes the inode back to disk on every call too.
 * Per-page slices therefore turn one freeze pass over today's 17 binaries
 * into ~147 reads, so ~147 kmalloc/kfree pairs and ~147 inode writebacks,
 * which is per-call overhead the chunk size alone controls. At 64 KiB the
 * same pass is ~18 reads. SHA-256 sees no difference either way: the digest
 * is over the byte stream, and every chunk but the tail is a whole number
 * of 64-byte blocks. */
#define UTEST_DIGEST_PAGES     16u
#define UTEST_DIGEST_CHUNK     (UTEST_DIGEST_PAGES * 4096u)
/* Fallback when the contiguous 64 KiB cannot be had. Correctness is
 * identical -- only the number of vfs_read calls changes -- so a fragmented
 * heap costs speed, never the identity guarantee. */
#define UTEST_DIGEST_PAGES_MIN 1u
/* The read cap and the allocation must be the SAME number of pages. They
 * are derived from one constant above, so this asserts the derivation was
 * not later broken by reintroducing an independent byte limit -- a chunk
 * larger than the buffer would let vfs_read run off the scratch page and
 * corrupt whatever follows it. */
_Static_assert(UTEST_DIGEST_CHUNK == UTEST_DIGEST_PAGES * 4096u,
               "the digest read cap must equal the scratch allocation it "
               "indexes, or a read overruns the buffer");
_Static_assert(UTEST_DIGEST_PAGES_MIN <= UTEST_DIGEST_PAGES,
               "the fallback digest buffer must not exceed the preferred one");

/* Freeze one planned binary's content identity into `out`.
 *
 * Returns 1 on success. Returns 0 -- leaving `out` undefined -- for every
 * reason the bytes could not be established: the path did not resolve, the
 * file reported a size the launcher will not read, a read came up short, or
 * the size changed underneath the walk. The caller must treat 0 as
 * fail-closed; see u_plan_freeze_identities.
 *
 * The size is re-checked against the node AFTER the last read rather than
 * trusted from before the first: a file rewritten mid-walk would otherwise
 * produce a digest over a mix of old and new bytes and freeze an identity
 * that never existed on disk. Catching it here turns that into a refusal,
 * which is the same outcome the mismatch check produces later and for the
 * same reason. */
static int u_digest_binary(const char *name, uint8_t *scratch,
                           uint32_t scratch_bytes,
                           uint8_t out[SHA256_DIGEST_LEN],
                           uint64_t *out_bytes)
{
    char path[VFS_MAX_NAME + 4];
    struct vfs_node *f;
    struct sha256_ctx ctx;
    uint64_t size, done = 0;
    uint32_t ni = 0, pi = 3;

    path[0] = 'C'; path[1] = ':'; path[2] = '\\';
    while (name[ni] && ni + 1u < VFS_MAX_NAME && pi + 1u < sizeof(path))
        path[pi++] = name[ni++];
    path[pi] = '\0';
    if (name[ni])
        return 0;                   /* same bound u_binary_present applies */

    f = vfs_open(path, VFS_O_READ);
    if (!f)
        return 0;

    size = f->size;
    /* Bound by what the EXECUTOR will actually load, not by what vfs_read
     * can express. Hashing is synchronous and happens during PLANNING, before
     * any spawn exists for u_wait_with_timeout to bound, so an oversized
     * entry -- a corrupted directory entry, a stale artifact, a hostile file
     * -- would stall the boot rather than become a counted refusal. Reading
     * up to 4 GiB to then hand it to a loader that rejects it above 16 MiB is
     * work with no possible outcome. Zero is refused for the same reason:
     * an empty file cannot be an executable, and admitting it would let two
     * different empty entries share one identity. */
    if (size == 0 || size > (uint64_t)EXEC_MAX_IMAGE_SIZE) {
        vfs_close(f);
        return 0;
    }

    sha256_init(&ctx);
    while (done < size) {
        uint64_t want = size - done;
        int n;

        if (want > scratch_bytes)
            want = scratch_bytes;
        n = vfs_read(f, (uint32_t)done, (uint32_t)want, scratch);
        if (n <= 0 || (uint64_t)n != want) {
            vfs_close(f);
            return 0;
        }
        sha256_update(&ctx, scratch, (uint32_t)want);
        done += want;
    }

    if (f->size != size) {          /* rewritten while we walked it */
        vfs_close(f);
        return 0;
    }
    vfs_close(f);
    sha256_final(&ctx, out);
    if (out_bytes)
        *out_bytes = size;
    return 1;
}

/* Freeze the content identity of every runnable entry, converting the ones
 * that cannot be frozen into counted refusals.
 *
 * Runs after the plan is complete and BEFORE u_plan_derive_smoke_gate, so a
 * smoke binary demoted here still reaches the gate as a refused smoke
 * prerequisite rather than vanishing from it -- the same ordering every
 * other refusal route depends on.
 *
 * The conversion is what makes the identity contract structural. A flag
 * saying "this entry has no valid digest" would leave an entry that the
 * presence probe still accepts sitting in the plan as runnable, and the
 * execution loop would launch content whose identity was never frozen --
 * exactly the hole this section exists to close, reintroduced one level
 * down. Dropping the entry instead is not an option either: a planned slot
 * that produces no record breaks the artifact reconciliation. A REFUSAL is
 * the shape that already means "planned, counted, never runs".
 *
 * `smoke_selected` is preserved across the conversion for the same reason
 * the duplicate-policy conflict preserves it: the identity was declared a
 * smoke prerequisite, and refusing it is a smoke non-PASS regardless of why
 * it was refused. */
static void u_plan_freeze_identities(struct plan_state *ps)
{
    uintptr_t scratch_phys;
    uint8_t  *scratch;
    uint64_t  started_ms;
    uint32_t  i, frozen = 0, refused = 0;
    uint32_t  scratch_pages = UTEST_DIGEST_PAGES;
    uint64_t  bytes = 0;

    if (!ps->entries)
        return;

    started_ms = u_uptime_ms();
    scratch_phys = pmm_alloc_contiguous(scratch_pages);
    if (!scratch_phys) {
        /* Speed, not correctness: a smaller buffer only means more
         * vfs_read calls over the same bytes. Degrade before refusing. */
        scratch_pages = UTEST_DIGEST_PAGES_MIN;
        scratch_phys  = pmm_alloc_contiguous(scratch_pages);
        if (!scratch_phys)
            scratch_pages = 0u;     /* report what we HAVE, not what we tried */
    }
    if (!scratch_phys) {
        /* No buffer means no identity for ANY entry, so every runnable one
         * becomes a refusal. Fail-closed is the only honest response: the
         * alternative is running the whole suite with the guarantee this
         * section adds silently switched off. */
        klog(LOG_ERROR, "UTEST",
             "identity freeze: no scratch page -- refusing every planned "
             "binary rather than running them unverified");
    }
    scratch = (uint8_t *)scratch_phys;

    for (i = 0; i < ps->count; i++) {
        struct plan_entry *e = &ps->entries[i];

        if (e->kind != (uint8_t)UTEST_PLAN_RUN)
            continue;

        if (scratch) {
            uint64_t this_bytes = 0;

            if (u_digest_binary(e->name, scratch, scratch_pages * 4096u,
                                e->content_digest, &this_bytes)) {
                bytes += this_bytes;
                frozen++;
                continue;
            }
        }

        /* Derive the smoke provenance the gate reads, rather than
         * preserving a bit that was never set. smoke_selected is written
         * only on the paths that convert an ALREADY-REFUSED identity;
         * a fresh RUN entry carries 0 from u_plan_alloc regardless of its
         * type, because until this section a declared smoke binary tripped
         * the gate through its VERDICT after running. An entry refused
         * here never runs, so it has no verdict, and leaving the bit at 0
         * would let the suite continue past a smoke prerequisite whose
         * bytes could not be verified -- the exact abort bypass
         * u_plan_derive_smoke_gate exists to prevent.
         *
         * Reading it off `type` is sound because a RUN entry is
         * filter-SELECTED by construction: u_plan_add_run applies
         * `utest_filter=` before allocating, so an excluded identity never
         * becomes a plan entry at all. */
        if (e->type == UTEST_TYPE_SMOKE)
            e->smoke_selected = 1u;
        e->kind    = (uint8_t)UTEST_PLAN_REFUSAL;
        e->reason  = UTEST_RSN_UNVERIFIED;
        e->verdict = (uint8_t)UTEST_NAME_ACCEPT;
        e->digest  = 0;
        refused++;
    }

    if (scratch_phys)
        pmm_free_contiguous(scratch_phys, scratch_pages);

    /* The measurement the section's own decision rests on: freezing content
     * identity costs one extra read of every planned binary on the walk
     * every boot takes. Logged as a fact per run rather than asserted once
     * in a comment, so a future binary set that changes the cost says so. */
    klog(LOG_INFO, "UTEST",
         "identity freeze: %u binaries, %u bytes, %ums (%u refused, "
         "%uKiB buffer)",
         (uint64_t)frozen, bytes, u_uptime_ms() - started_ms,
         (uint64_t)refused, (uint64_t)(scratch_pages * 4u));
}

/* Derive the smoke gate from the plan, once, after every refusal has been
 * staged.
 *
 * This replaces three separate "remember to set the flag" sites, each of
 * which was found to miss a route: a duplicate-policy conflict, a refused
 * line vetoing an accepted variant, an attribute refusal, and a third
 * declaration suppressed by an existing refusal all end at the same
 * place -- a REFUSAL record standing for an identity that was declared
 * smoke. Reading that off the records means a future refusal route is
 * covered by construction instead of by another flag setter.
 *
 * `selected` is what keeps a focused run honest: refusals publish
 * regardless of `utest_filter=`, but a smoke entry the filter excluded
 * would never have been planned, so refusing it must not abort the run
 * the operator actually asked for. */
static void u_plan_derive_smoke_gate(struct plan_state *ps)
{
    uint32_t i;

    for (i = 0; i < ps->count; i++) {
        const struct plan_entry *e = &ps->entries[i];

        if (e->kind != (uint8_t)UTEST_PLAN_REFUSAL)
            continue;
        if (e->smoke_selected)
            ps->smoke_refused = 1;
    }
}

/* Drop every runnable entry, keeping the refusals already planned.
 *
 * Used by the fail-closed paths: when the refusal identity set overflowed
 * we no longer hold every refused identity, so we cannot certify that an
 * accepted entry is not a duplicate of one we have forgotten. Executing a
 * partial plan while claiming the completeness this section exists to
 * provide is exactly the false green it removes -- so the run publishes
 * what it retained and launches nothing. */
static void u_plan_drop_runs(struct plan_state *ps)
{
    uint32_t i, out = 0;

    for (i = 0; i < ps->count; i++) {
        if (ps->entries[i].kind == (uint8_t)UTEST_PLAN_RUN)
            continue;
        if (out != i)
            ps->entries[out] = ps->entries[i];
        out++;
    }
    ps->count = out;
}

/* Has this directory entry already been refused as a MANIFEST entry?
 *
 * A name refused for length or charset can perfectly well exist on disk,
 * and it is then discovered twice -- once from the manifest, once from
 * the glob -- which would publish ONE bad binary as TWO failures under
 * two ordinals. total_planned and total_ran would agree, so the
 * completeness reconciliation cannot catch it; the dedup has to.
 *
 * REFUSE_NUL entries are deliberately excluded. Their stored C string is
 * the TRUNCATION at the embedded NUL, not the name, and no filename can
 * contain a NUL -- so a dirent that matches that truncation is a
 * different file that would be wrongly suppressed. */
static int u_refusal_index_of(const struct manifest_state *ms,
                              const char *name)
{
    uint32_t i;

    for (i = 0; i < ms->refused_count; i++) {
        if (ms->refused_verdict[i] == UTEST_NAME_REFUSE_NUL)
            continue;
        if (u_name_equal_fs(ms->refused_names[i], name, VFS_MAX_NAME))
            return (int)i;
    }
    return -1;
}

static int u_refusal_already_seen(const struct manifest_state *ms,
                                  const char *name)
{
    return u_refusal_index_of(ms, name) >= 0;
}

/* Publish one refused binary as a counted infrastructure FAILURE.
 *
 * This is the whole point of the refusal taxonomy: the binary was planned
 * (it reached total_planned) and it will never launch, so if it produced
 * no record the run would report fewer results than it planned and still
 * exit green. It therefore takes the SAME shape a planned binary that
 * failed to load takes -- a verdict line, a TAP point, an XML testcase
 * and a JSON record -- which is why the host needs no new record kind to
 * consume it.
 *
 * Three pieces of accounting have to move together or an artifact
 * contradicts itself: counters[1] so the binary counts as failed and the
 * host's fail-closed recount sees a matching verdict line; the TAP point
 * so the trailing plan still reconciles; and rt->unreported, because the
 * JSON harvester requires reported + invalid + unreported to equal
 * summary.total and a refused binary submitted no self-report. The
 * caller adds total_ran, symmetrically with its u_run_one calls, so
 * not_run = total_planned - total_ran stays the completeness statement it
 * claims to be. */
/* `trusted` = `raw` is an identity the classifier already ACCEPTED, so it
 * is inside the accepted charset and within UTEST_MAX_BINARY_NAME, and is
 * published verbatim.
 *
 * The synthesized `refused_<ordinal>_<prefix>_<digest>.exe` identity
 * exists to make UNTRUSTED bytes safe and distinguishable, and it costs a
 * truncation to do it -- only about 14 prefix bytes survive for a small
 * ordinal. For a planned binary that simply vanished, that truncation
 * defeats the point: `test_component_alpha.exe` and
 * `test_component_beta.exe` would land in the artifacts under the same
 * prefix, and this section's whole claim is that the operator learns
 * WHICH planned binary went missing. A trusted name needs no synthesis. */
/* Publish one never-ran record under an identity the caller already
 * built. Shared by every shape that reports a binary which produced no
 * self-report -- a per-name refusal, a trusted planned binary that
 * vanished, and a fail-closed aggregate -- because the counter movement
 * is what the artifacts reconcile against and three copies of it is three
 * chances for one to drift out of step with the other two. */
static void u_publish_never_ran(const char *id, utest_type_t type,
                                const char *reason,
                                uint32_t *tap_point, uint32_t *counters,
                                struct u_report_totals *rt)
{
    uint32_t point;

    counters[1]++;      /* a refusal is a FAILED binary ...             */
    counters[3]++;      /* ... that never RAN, so it is also an error   */
    rt->unreported++;   /* ... and could not submit a self-report       */
    point = ++(*tap_point);
    /* The human verdict line, the TAP point and the legacy `=== N failed`
     * summary all keep the FAIL vocabulary they have always used: the
     * host's fail-closed recount counts these very lines against that
     * summary, so re-spelling them would make the two disagree about a run
     * neither of them got wrong. Only the machine artifacts, which have an
     * element/status for "never ran", draw the finer distinction. */
    utest_record_log(LOG_ERROR, "%s: FAIL (%s)", id, reason);
    u_emit_tap_point(0, point, id, reason);
    u_emit_xml_testcase(id, type, 3, 0, reason, (const char *)0);
    u_emit_json_testcase(id, type, 3, 0, reason,
                         (const struct u_report *)0);
}

static void u_emit_refusal_named(const char *reason, uint32_t ordinal,
                                 const char *raw, uint32_t digest,
                                 int trusted,
                                 uint32_t *tap_point, uint32_t *counters,
                                 struct u_report_totals *rt)
{
    char id[UTEST_MAX_BINARY_NAME + 1u];
    int built;

    /* Build BEFORE counting. The build cannot fail while the identity's
     * static assert holds, but counting first would make the fallback
     * path publish an artifact that CONTRADICTS itself -- summary total
     * above the record count, and the report partition above it too --
     * rather than merely one record short. Cheap ordering, strictly
     * better failure shape. */
    if (trusted) {
        uint32_t i = 0;
        while (raw && raw[i] && i + 1u < sizeof(id)) {
            id[i] = raw[i];
            i++;
        }
        id[i] = '\0';
        /* An accepted name is bounded by construction; a longer one here
         * would mean the classifier let something through, so refuse to
         * publish a truncation under a name that is not the file's. */
        built = (raw && !raw[i]);
    } else {
        built = u_build_refusal_id(id, sizeof(id), ordinal, raw, digest);
    }
    if (!built) {
        /* Unreachable while the identity's static assert holds: the
         * fixed shape plus a minimum prefix is proven to fit the derived
         * bound. Kept fail-closed anyway, and on the marker channel the
         * host already fails the run on, because the alternative to a
         * record without a name is no record at all. */
        utest_record_log(LOG_ERROR,
             "[UTEST-RECORD-OVERFLOW] refusal identity for planned binary "
             "%u could not be built", (uint64_t)ordinal);
        return;
    }

    u_publish_never_ran(id, UTEST_TYPE_CORRECTNESS, reason, tap_point,
                        counters, rt);
}

/* Untrusted-identity refusal: the raw bytes get the synthesized,
 * sanitized identity. This is every enumeration refusal of a NAME. The
 * fail-closed aggregates used to come through here too and no longer do
 * -- see u_emit_aggregate. */
static void u_emit_refusal(const char *reason, uint32_t ordinal,
                           const char *raw, uint32_t digest,
                           uint32_t *tap_point, uint32_t *counters,
                           struct u_report_totals *rt)
{
    u_emit_refusal_named(reason, ordinal, raw, digest, 0,
                         tap_point, counters, rt);
}

/* Fail-closed aggregate: no name was refused, so the record carries the
 * aggregate identity rather than a synthesized one. It moves the same
 * counters as a refusal -- one failed binary that never ran and submitted
 * no report -- because the plan reserved exactly one slot for it and both
 * artifact formats reconcile record count against the summary total. */
static void u_emit_aggregate(const char *reason, utest_agg_kind_t kind,
                             uint32_t value,
                             uint32_t *tap_point, uint32_t *counters,
                             struct u_report_totals *rt)
{
    char id[UTEST_MAX_BINARY_NAME + 1u];

    /* Build before counting, for the reason u_emit_refusal_named states:
     * a counted-but-unpublished record makes the artifact contradict
     * itself, where an unbuilt one merely leaves it a record short. */
    if (!u_build_aggregate_id(id, sizeof(id), kind, value)) {
        /* Unreachable while the per-label static asserts hold. Reported
         * on the marker channel the host already fails the run on. */
        utest_record_log(LOG_ERROR,
             "[UTEST-RECORD-OVERFLOW] aggregate identity for kind %u could "
             "not be built", (uint64_t)kind);
        return;
    }
    u_publish_never_ran(id, UTEST_TYPE_CORRECTNESS, reason, tap_point,
                        counters, rt);
}

/* ---- Per-binary run: spawn, wait, log, cleanup --------------------- *
 *
 * Called from test_usermode_run for each binary (either from the
 * manifest or from the directory glob). Fills out_* with the
 * verdict so the caller can aggregate counters and emit TAP lines.
 * out_verdict values: 0 = PASS, 1 = FAIL, 2 = SKIP.
 * ------------------------------------------------------------------ */

/* Inner helper: spawn + wait for ONE invocation of a binary. Returns
 * the child's exit status (or timeout/verdict markers) via out_*.
 * Kept as a helper so u_run_one stays readable now that the XML/JSON
 * + isolation contract live around it. */
static void u_spawn_one(const char *name_copy, const char *path,
                        int *out_pid, int32_t *out_exit_status,
                        int *out_timed_out, int *out_stalled,
                        uint32_t *out_leaked,
                        struct u_report *out_report, int have_stem,
                        const uint8_t *expect_digest,
                        struct u_loader_evidence *out_loader)
{
    int pid;

    /* No per-spawn clearing here any more, and its absence is the point.
     * These flags used to be file-scope statics that the launcher reset
     * before each spawn and read back after the wait -- a pairing with no
     * invocation identity in it, so a late store from a force-killed
     * loader landed after the reset for the NEXT binary and changed THAT
     * binary's classification. The evidence now lives in the child's own
     * slot, zeroed by its constructor, and is read back below from the
     * specific child being reaped. */
    out_loader->stage_fault = out_loader->reached_exec = 0;
    out_loader->identity_mismatch = 0;
    out_loader->frame_adopted = out_loader->entered_user = 0;

    out_report->asserts_passed = 0;
    out_report->asserts_failed = 0;
    out_report->skip_blocks    = 0;
    out_report->state          = TASK_UTEST_REPORT_NONE;

    /* task_create_captured (not plain task_create) arms this task's
     * capture fields BEFORE it is published to the scheduler, so its
     * earliest possible write() cannot race the arming. It ALSO emits
     * the "[UTEST-CAPTURE-BEGIN] owner=<pid> name=<name>" binding
     * BEFORE publication (not here, after task_create_captured()
     * returns -- an already-published, immediately-scheduled task on
     * another CPU could otherwise emit capture chunk records before
     * their owner binding reaches the wire). See task_create_internal
     * in task.c and test_usermode_capture_begin().
     *
     * The loader inputs are passed to the constructor for the same
     * ordering reason, so they are armed in the child's slot before it can
     * be selected, where handing them over after this call returned would
     * race a loader that is already running. Not called a publication
     * barrier, because it is not one: num_tasks is a plain global and
     * supplies no memory-model edge (task_create_internal says so at the
     * arming site). What it buys is program order on the BSP, which is
     * where dispatch happens today. */
    pid = task_create_captured(utest_loader_func, name_copy,
                               path, expect_digest);
    *out_pid = pid;
    if (pid < 0) {
        *out_exit_status = -1;
        *out_timed_out   = 0;
        *out_stalled     = 0;
        *out_leaked      = 0;
        return;
    }

    {
        struct task *child = task_get_by_pid((uint32_t)pid);

        /* ARMING POSTCONDITION, checked from the LAUNCHER rather than
         * trusted from the arming site. task_create_internal sets
         * utest_capture_active, sets utest_capture_owner_pid and emits the
         * BEGIN binding as three separate statements, so losing either
         * assignment while keeping the binding would publish a capture
         * channel that captures nothing -- the run would then look like a
         * binary that legitimately wrote no output. Reading the fields back
         * here is what makes the arming itself falsifiable.
         *
         * Safe in this window: the slot cannot be recycled before the
         * launcher's own task_cleanup, which is the same guarantee
         * u_report_snapshot and u_isolation_snapshot_leaks already rely on.
         *
         * Published as [UTEST-CAPTURE-LOST] because that is exactly what an
         * unarmed channel means on the wire -- this binary's stdout reaches
         * serial unframed -- and the host already refuses a run carrying
         * one. */
        if (!u_capture_armed_for(child, (uint32_t)pid))
            utest_record_log(LOG_ERROR,
                             "[UTEST-CAPTURE-LOST] owner=%u len=unknown",
                             (uint64_t)pid);

        u_capture_expect((uint32_t)pid, name_copy);
    }
    *out_exit_status = u_wait_with_timeout((uint32_t)pid, s_timeout_ms,
                                           out_timed_out, out_stalled);
    /* Snapshot leaks BEFORE task_cleanup destroys the handle table.
     * Callers must then run task_cleanup; the destructive reap happens
     * even later (contract: vfs_unlink needs the child's handles
     * closed first). */
    *out_leaked = have_stem ? u_isolation_snapshot_leaks((uint32_t)pid) : 0u;
    /* Same window, same reason: the harness self-report lives on the TCB
     * that task_cleanup is about to release. Taken unconditionally --
     * unlike leaks it does not depend on per-test isolation being on. */
    u_report_snapshot((uint32_t)pid, out_report);
    /* Same window and the same reason once more, and this one is what
     * binds the verdict to its producer: the loader evidence is read out
     * of THIS child's slot, after it is dead and before task_cleanup
     * releases it. Nothing another invocation stores can reach these
     * fields, which is precisely what the file-scope statics could not
     * promise. */
    u_loader_snapshot((uint32_t)pid, out_loader);
}

/* Is a planned binary still on disk?
 *
 * The plan is immutable but the DIRECTORY is not: a live child can delete
 * or rename an entry between planning and the moment it would launch. The
 * whole point of planning once is that the launcher then knows exactly
 * WHICH entry went missing instead of watching an aggregate count come up
 * short, so the disappearance is published as a named, counted
 * infrastructure failure rather than a silent decrement.
 *
 * It is deliberately a counted RESULT and not an unrun plan slot: both
 * artifact formats reconcile record count against the summary total (the
 * JSON harvester checks len(testcases) == summary.total), so a slot that
 * produced no record would make the artifact unparseable rather than
 * merely incomplete. `[UTEST-RUN-INCOMPLETE]` therefore stays what it
 * always was -- the gate for entries that produced no result at all, such
 * as the tail a smoke abort never reaches.
 *
 * Bounded by u_run_one's NAME_COPY buffer rather than its path buffer:
 * u_run_one first snapshots the name into `char name_copy[VFS_MAX_NAME]`
 * and truncates at VFS_MAX_NAME - 1 before building any path, so bounding
 * on the path buffer here would call a name reachable that u_run_one
 * would silently shorten. Not live today -- every RUN entry was accepted
 * by u_classify_name_span, which bounds names at UTEST_MAX_BINARY_NAME --
 * but the two must not disagree about what is reachable. */
static int u_binary_present(const char *name)
{
    char path[VFS_MAX_NAME + 4];
    struct vfs_node *f;
    uint32_t ni = 0, pi = 3;

    path[0] = 'C'; path[1] = ':'; path[2] = '\\';
    while (name[ni] && ni + 1u < VFS_MAX_NAME && pi + 1u < sizeof(path))
        path[pi++] = name[ni++];
    path[pi] = '\0';
    if (name[ni])
        return 0;
    f = vfs_open(path, VFS_O_READ);
    if (!f)
        return 0;
    vfs_close(f);
    return 1;
}

static void u_run_one(const char *name, utest_type_t type,
                      uint32_t *tap_point, uint32_t *counters,
                      struct u_report_totals *rt, int *out_verdict,
                      const uint8_t *expect_digest)
{
    char name_copy[VFS_MAX_NAME];
    char path[VFS_MAX_NAME + 4];
    char stem[VFS_MAX_NAME];
    /* Sized by the derivation that bounds the records this string rides
     * in, so widening a reason literal and overrunning the buffer are the
     * same build failure rather than two separate surprises. */
    char reason[UTEST_REASON_BUF];
    int  have_stem;
    int  isolation_failed = 0;
    uint32_t ni, pi;
    int pid = -1;
    int32_t exit_status = 0;
    int timed_out = 0;
    int stalled = 0;
    uint32_t leaked = 0;
    uint32_t test_num;
    struct u_report report;
    struct u_loader_evidence loader;
    uint64_t start_ms;
    uint64_t end_ms;
    /* The verdict the ARTIFACTS carry, which is not always the verdict the
     * launcher aggregates: a loader that never reached ring 3 is a FAIL to
     * the pass/fail/skip counters and the smoke gate, and an <error> /
     * ERROR to a consumer asking which binaries actually executed. -1
     * means "no override -- use the aggregated verdict". */
    int record_verdict = -1;

    reason[0] = '\0';
    (void)type; /* used below for XML/JSON classname only */

    /* Snapshot `name` into launcher-owned storage BEFORE spawning.
     * The caller may have passed in a VFS dirent or manifest arena
     * pointer -- child syscalls (SYS_READDIR, our own kfree at end of
     * run) can invalidate either. */
    for (ni = 0; name[ni] && ni < sizeof(name_copy) - 1; ni++)
        name_copy[ni] = name[ni];
    name_copy[ni] = '\0';

    /* Build C:\<name>. */
    path[0] = 'C'; path[1] = ':'; path[2] = '\\';
    pi = 3;
    for (ni = 0; name_copy[ni] && pi < sizeof(path) - 1; ni++)
        path[pi++] = name_copy[ni];
    path[pi] = '\0';

    /* Derive the scratch-dir / Registry-key stem from the binary
     * name (`test_syscall.exe` -> `test_syscall`). If the name doesn't
     * match the *.exe shape we skip isolation for this run (this
     * shouldn't happen today because u_is_test_binary() gated entry,
     * but defending against the filter being loosened later is cheap). */
    have_stem = s_isolation_enabled &&
                u_derive_test_name(name_copy, stem, sizeof(stem));
    if (have_stem && u_isolation_setup(stem) != 0)
        isolation_failed = 1;

    /* Open color scope: all kernel klog output WHILE this binary is
     * dispatched (sched / exec / elf / signal / etc.) renders in the
     * UTEST color via the per-line override in klog.c. Closed at the
     * bottom of u_run_one after task_cleanup. */
    s_utest_color_active = 1;

    start_ms = u_uptime_ms();

    /* Single spawn per binary. Stress policy is IN-BINARY, not
     * launcher-side: stress binaries iterate inside main() (see
     * user/test/test_stress_libc.c for the canonical shape). The
     * launcher does NOT loop spawning per-iteration because task
     * slots in the kernel are monotonic (`num_tasks++` in task_create
     * with no reuse in task_cleanup); a 100-iteration launcher loop
     * would exhaust TASK_MAX (32) after ~20 binaries.  The
     * s_stress_iters boot.conf knob is RESERVED for a future
     * env-passing syscall that lets stress binaries query the desired
     * iteration count at runtime. */
    u_spawn_one(name_copy, path, &pid, &exit_status,
                &timed_out, &stalled, &leaked, &report, have_stem, expect_digest,
                &loader);

    /* The run's own count of spawned binaries, published on the frame
     * terminator and reconciled against the expectation records u_spawn_one
     * emits. Raised HERE, in a different function from the record it is
     * checked against, so that deleting either half alone is a mismatch the
     * host refuses -- a single site would take both away together and leave
     * an empty expected set that reads as a legitimately silent run. */
    if (pid >= 0)
        (void)__atomic_add_fetch(&s_capture_spawned, 1u, __ATOMIC_RELAXED);

    if (pid < 0) {
        /* Same class as a refusal in the artifacts: no task was ever
         * created, so nothing this binary could assert ever executed --
         * which is why it publishes through the SAME helper every other
         * never-ran shape uses. It used to hand-roll the counter movement
         * beside a plain `klog(LOG_ERROR, "UTEST", ...)` line, and that
         * line did not match the framed verdict grammar the host recounts
         * (scripts/test.sh greps `<frame><name>.exe: FAIL|TIMEOUT|...`), so
         * on exactly this path the observed count came out SMALLER than the
         * summary. Nothing tripped: the host escalates only when observed
         * EXCEEDS the summary, so the two counters disagreed by
         * construction and silently. */
        u_publish_never_ran(name_copy, type, UTEST_RSN_TASK_CREATE,
                            tap_point, counters, rt);
        /* Set EXPLICITLY, not by the helper: `out_verdict` drives the
         * launcher's own aggregation and the smoke gate, which classify by
         * pass/fail/skip and have no never-ran dimension. The caller reads
         * it immediately after u_run_one returns, so leaving it to a helper
         * that does not own it is how a smoke abort would be missed. */
        *out_verdict = 1;
        s_utest_color_active = 0;
        if (have_stem) {
            u_isolation_reap(stem);
            u_cleanup_manifest_apply();
        }
        return;
    }

    /* Format-coverage line: emit the binary-format name the exec
     * dispatcher matched for this binary (ELF / PE32+ / EIF) BEFORE
     * task_cleanup so the name is always visible regardless of
     * PASS/FAIL/timeout/leak verdict. The name string aliases a
     * static entry in the format registry (see exec.c s_formats[])
     * so it stays valid for the lifetime of the kernel; no copy
     * needed. A binary whose format field stays NULL means
     * exec_load_fmt failed before matching -- covered by the
     * "task_exec failed" path in u_spawn_one which already logged
     * FAIL (exit=-5), so we skip the format line there.
     *
     * Without this line, a regression that silently routed PE
     * binaries through the ELF loader would still print PASS --
     * the whole point of the format-coverage probe. */
    {
        struct task *t = task_get_by_pid((uint32_t)pid);
        const char *fmt = (t && t->loaded_format) ? t->loaded_format : (const char *)0;
        if (fmt)
            utest_record_log(LOG_INFO, "%s: format=%s", name_copy, fmt);
    }

    /* Compute preliminary verdict + reason from the child's exit
     * status. Escalations for leaked handles and isolation failures
     * apply BELOW; the single [UTEST]/TAP/XML/JSON emit happens at the
     * bottom so external consumers never see a contradictory "ok N
     * ... not ok N" pair for the same test_num.  Emitting a PASS log
     * before the leak check fires lets a subsequent FAIL produce two
     * lines for the same run. */
    if (exit_status == 0) {
        counters[0]++;  /* passed (may be rolled back by escalations) */
        *out_verdict = 0;
    } else if (exit_status == UTEST_EXIT_SKIP) {
        counters[2]++;  /* skipped */
        *out_verdict = 2;
    } else if (timed_out) {
        counters[1]++;  /* failed (timeout) */
        /* A timeout with nothing published never reached ring 3 -- the
         * loader stalled on its way there. Same class as a refusal. */
        record_verdict = u_record_verdict(
            1, (int)loader.stage_fault,
            1, (int)loader.reached_exec,
            (int)loader.frame_adopted, (int)loader.entered_user);
        if (record_verdict == 3)
            counters[3]++;
        *out_verdict = 1;
        /* A stall names the CLOCK, never a deadline. The two arrive through
         * the same `timed_out` gate because they take the same safety actions,
         * and this is the point at which they stop being the same thing: a
         * reason reading "timeout after 10000ms" for a wait that never had a
         * working clock is a fabricated measurement, and it is the one line a
         * reader triages from. */
        if (stalled)
            u_reason_stall(&reason);
        else
            u_reason_timeout(&reason, s_timeout_ms ? s_timeout_ms
                                                  : UTEST_DEFAULT_TIMEOUT_MS);
    } else {
        counters[1]++;  /* failed */
        /* A loader that never reached ring 3 is the same class as a
         * refused name: nothing this binary could assert ever executed, so
         * the artifacts report it as an ERROR rather than an assertion
         * failure. The STAGE says so; the exit status cannot, because the
         * loader's own -1..-5 codes are indistinguishable from a signalled
         * ring-3 death by number alone. `out_verdict` stays 1 -- it feeds
         * the launcher's pass/fail/skip aggregation and the smoke gate,
         * which have no never-ran dimension. */
        record_verdict = u_record_verdict(
            1, (int)loader.stage_fault,
            0, (int)loader.reached_exec,
            (int)loader.frame_adopted, (int)loader.entered_user);
        if (record_verdict == 3)
            counters[3]++;
        *out_verdict = 1;
        if (loader.identity_mismatch) {
            /* The binary the plan named still resolved, but not to the
             * bytes the plan froze. Reported by NAME as a counted
             * infrastructure failure rather than as a bare `exit=N`: the exit
             * code says only that the loader gave up, and the whole point
             * of freezing identity is to be able to say WHY. The record
             * class is already ERROR here (the loader never reached ring
             * 3), which is the same class a refused name gets, and for the
             * same reason -- nothing this binary could assert ever ran. */
            uint32_t rp = 0;
            u_append(reason, &rp, sizeof(reason), UTEST_RSN_MISMATCH);
        } else {
            u_reason_exit(&reason, exit_status);
        }
    }

    /* Handle leaks escalate a PASS to FAIL (test checkpoint:
     * "A binary that opens C:\\hello.txt without closing it surfaces
     * as [UTEST] FAIL test_x: 1 handle leaked"). Tests that already
     * FAIL/SKIP keep their stronger verdict -- we don't upgrade a
     * SKIP to FAIL just because it also leaked.
     *
     * The rationale for escalation over a WARN: leaking a handle
     * across process exit is the same class of bug as leaking memory,
     * and Linux kselftest / Windows HLK both treat resource leaks as
     * test failures. A passing binary that leaks is lying about its
     * cleanup invariant; surfacing that as FAIL makes CI reject it. */
    if (have_stem && leaked > 0 && *out_verdict == 0) {
        *out_verdict = 1;
        counters[0]--;  /* undo PASS */
        counters[1]++;  /* record FAIL */
    }

    /* Fence and reap the descendant tree BEFORE the owner's own cleanup.
     * The launcher waited on one pid; fork() handed the same capture channel
     * to every descendant, so this is the point at which "the binary is
     * finished" stops being true of only the task that was waited on.
     *
     * Before task_cleanup rather than after, because the fence latches the
     * stop flag on the OWNER's slot -- that is the slot every descendant's
     * claim reads -- and task_cleanup is what releases that slot's
     * resources. Ordering it the other way would fence a tree against a
     * task record already being torn down.
     *
     * A non-zero return means a producer forked faster than the reap could
     * keep up, which is a bounded, reported condition rather than a silent
     * one: the records those survivors go on to claim are already fenced by
     * the owner's stop latch, so what is at stake is kernel resources, not
     * the wire. */
    {
        struct utest_reap_result reap;

        u_capture_reap_tree((uint32_t)pid, &reap);

        if (reap.live)
            utest_record_log(LOG_WARN,
                             "[UTEST-CAPTURE-UNREAPED] owner=%u live=%u",
                             (uint64_t)pid, (uint64_t)reap.live);
        /* The degraded drain gets its OWN record rather than a field on the
         * one above, and is emitted even when `live` is zero -- which is the
         * whole point. An inexact zero is indistinguishable from an exact
         * zero at the reporting site and means something entirely different:
         * a fork was still in flight when the reap gave up waiting, so a
         * descendant may have published after the census walked past its
         * slot. Folding it into UNREAPED would suppress it in exactly that
         * case, because UNREAPED is emitted only when the count is non-zero.
         *
         * The host refuses on it (utest-capture.py). That is deliberate: the
         * run's survivor accounting is unverifiable, and a run whose
         * accounting is unverifiable is not a clean run. */
        if (!reap.exact) {
            /* THE BOOT-STICKY LATCH NEEDS CONSTRUCTOR EVIDENCE, not merely an
             * inexact census, and the two stopped being the same thing once a
             * stalled wait could also withhold exactness.
             *
             * What this latch means is that an unfinished fork constructor
             * already owns the slot the next task_create_internal would take,
             * which is why it survives the run that observed it. A grace that
             * gave up on a stopped CLOCK is not that: nothing is half-built,
             * and the run-scoped stall latch already aborts this suite. Setting
             * it here anyway would poison the next run -- which clears the
             * stall latch, keeps this one, and aborts a healthy binary as `reap
             * degraded` with no collision to point at. */
            if (reap.drain_pending || reap.drain_stranded)
                s_reap_degraded = 1;
            utest_record_log(LOG_WARN,
                             "[UTEST-CAPTURE-REAP-DEGRADED] owner=%u pending=%u "
                             "stranded=%u",
                             (uint64_t)pid, (uint64_t)reap.drain_pending,
                             (uint64_t)reap.drain_stranded);
        }

        /* The OWNER's cleanup is withheld on the same grounds the reap
         * withholds its descendants'. A fork constructor still in flight for
         * this owner reads the parent slot it is copying from, and the reap
         * has just said it cannot account for that constructor; freeing the
         * owner underneath it is the one mistake that cannot be undone. The
         * slot leaks, the run is refused, and nothing that is still running
         * has the ground pulled out from under it. */
        if (reap.exact)
            task_cleanup((uint32_t)pid);
    }

    /* Close color scope: subsequent klog lines (the launcher's own
     * `[UTEST] <name>: PASS/FAIL` and the cleanup WARNs) still route
     * through the subsystem-name "UTEST" path and get the UTEST color
     * that way; we only need the global override while OTHER
     * subsystems (sched/exec/elf) are emitting on behalf of the
     * spawned binary. */
    s_utest_color_active = 0;

    /* Destructive cleanup runs AFTER task_cleanup: the child's handles
     * are now closed (ob_handle_table_destroy ran inside task_cleanup),
     * so vfs_unlink can reach files that were held open at exit. Any
     * failure to delete after this point is a genuine FS or Registry
     * bug, not a ref_count race. */
    if (have_stem) {
        if (u_isolation_reap(stem) != 0)
            isolation_failed = 1;
        u_cleanup_manifest_apply();
    }

    /* Isolation contract violated: if setup or reap left stale state
     * behind (rmtree cap exhausted, vfs_create failed), the next
     * binary can no longer assume a clean scratch dir. Escalate the
     * verdict: a PASS becomes FAIL so CI surfaces the broken
     * invariant. Already-FAIL/SKIP keeps its stronger verdict plus a
     * WARN noting the isolation breach. Codex quality M 2026-04-20. */
    if (isolation_failed && *out_verdict == 0) {
        *out_verdict = 1;
        counters[0]--;
        counters[1]++;
        u_reason_isolate(&reason);
    }

    /* Handle-leak reason: only fill `reason` when we escalated from
     * PASS. For FAIL/SKIP the exit-code or timeout reason already
     * dominates; adding the leak count to a JSON blob would be nice
     * but would require a structured reason schema and is a
     * nice-to-have, not a correctness issue. */
    if (reason[0] == '\0' && leaked > 0 && *out_verdict == 1) {
        u_reason_leak(&reason, leaked);
    }

    /* Ring-3 self-report reconciliation. The counts are the binary's own
     * claim about itself, so they are checked against the outcome the
     * kernel observed before any of them reach an artifact.
     *
     * An INVALID report escalates a PASS or a SKIP to FAIL. A binary that
     * says "no assertions failed" and exits non-zero -- or claims a
     * whole-binary skip while reporting assertions it ran -- is producing
     * exactly the contradictory signal this section exists to remove, and
     * silently dropping its counts would leave the run green on a binary
     * that just proved its own reporting untrustworthy. An already-FAIL
     * keeps its stronger, more specific reason. */
    report.state = u_report_reconcile(report.state, report.asserts_failed,
                                      exit_status, timed_out);
    if (report.state == TASK_UTEST_REPORT_INVALID) {
        int was_failing = (*out_verdict == 1);
        rt->invalid++;
        *out_verdict = u_report_apply_invalid(*out_verdict, counters);
        if (!was_failing && reason[0] == '\0') {
            u_reason_invalid(&reason);
        }
        klog(LOG_WARN, "UTEST",
             "%s: self-report contradicts outcome (exit=%d, reported "
             "failed=%u) -- counts discarded",
             name_copy, (int64_t)exit_status,
             (uint64_t)report.asserts_failed);
    } else if (report.state == TASK_UTEST_REPORT_VALID) {
        /* Bounded by TASK_UTEST_REPORT_MAX per binary and TASK_MAX
         * binaries per run, so these 32-bit sums cannot wrap. */
        rt->reported++;
        rt->asserts_passed += report.asserts_passed;
        rt->asserts_failed += report.asserts_failed;
        rt->skip_blocks    += report.skip_blocks;
    } else {
        rt->unreported++;
    }

    /* Single verdict emit: exactly one [UTEST] line and (when TAP is
     * on) one TAP line per binary. All escalations have applied above,
     * so `*out_verdict`, `reason`, and `leaked`/`isolation_failed` are
     * their final values.  Emitting inside the raw exit-status branches
     * produces a PASS followed by a FAIL (escalation) for the same
     * test_num. */
    test_num = ++(*tap_point);
    if (*out_verdict == 0) {
        utest_record_log(LOG_INFO, "%s: PASS (exit=0)", name_copy);
        u_emit_tap_point(1, test_num, name_copy, (const char *)0);
    } else if (*out_verdict == 2) {
        utest_record_log(LOG_INFO, "%s: SKIP (exit=77)", name_copy);
        u_emit_tap_point(1, test_num, name_copy, "SKIP");
    } else if (have_stem && leaked > 0 && exit_status == 0 && !timed_out &&
               !isolation_failed) {
        /* Escalated from PASS by leak detection only. */
        utest_record_log(LOG_ERROR,
             "%s: FAIL (%u" UTEST_RSNC_LEAK " -- escalated from PASS)",
             name_copy, (uint64_t)leaked);
    } else if (isolation_failed && exit_status == 0 && !timed_out) {
        /* Escalated from PASS by isolation failure only. */
        utest_record_log(LOG_ERROR,
             "%s: FAIL (" UTEST_RSNC_ISOLATE " -- escalated from PASS)",
             name_copy);
    } else if (timed_out && stalled) {
        /* SERIAL AGREES WITH THE ARTIFACTS. This branch re-derives its text
         * from the flags rather than using the finalized `reason` below, which
         * is fine while the flags pick one cause -- and stops being fine the
         * moment two causes arrive through the same flag. A stall reaches here
         * with `timed_out` set, because it takes the timeout's safety actions,
         * so without this branch the human-readable verdict would blame the
         * binary and quote a duration the launcher just said it could not
         * measure, while TAP, XML and JSON all named the clock. */
        utest_record_log(LOG_ERROR, "%s: FAIL (" UTEST_RSNC_STALL ")",
                         name_copy);
    } else if (timed_out) {
        utest_record_log(LOG_ERROR,
             "%s: FAIL (" UTEST_RSNC_TIMEOUT_PRE "%u" UTEST_RSNC_TIMEOUT_POST ")",
             name_copy, (uint64_t)(s_timeout_ms ? s_timeout_ms
                                                : UTEST_DEFAULT_TIMEOUT_MS));
    } else {
        utest_record_log(LOG_ERROR, "%s: FAIL (" UTEST_RSNC_EXIT "%d)",
             name_copy, (int64_t)exit_status);
    }
    /* ONE reason, published to every consumer.
     *
     * The failing TAP point carries the SAME finalized `reason` the XML and
     * JSON records carry, instead of re-deriving one from the flags. The
     * re-derivation disagreed with the artifacts on every case where the
     * flags do not pick the reason the escalation chain stored: a PASS that
     * both leaked AND failed isolation stores the LEAK reason but the
     * `!isolation_failed` guard sent TAP down the isolation branch, and an
     * invalid self-report that escalates a PASS or SKIP stores "invalid
     * test report" while the fallback emitted `exit=0` / `exit=77`. Both are
     * reachable, and both hand two consumers of one verdict different
     * explanations for it. The human klog lines above still name the
     * DOMINANT escalation, which is what a person reading serial wants;
     * machine records get the single finalized string. */
    if (*out_verdict == 1)
        u_emit_tap_point(0, test_num, name_copy, reason);

    /* Extra WARN context for leaks / isolation failures that ride on
     * top of an already-FAIL/SKIP verdict (exit_status != 0). The
     * primary verdict line above already reflects the dominant
     * reason (exit / timeout). */
    if (have_stem && leaked > 0 && !(exit_status == 0 && !timed_out)) {
        klog(LOG_WARN, "UTEST",
             "%s: %u" UTEST_RSNC_LEAK " (open at exit)",
             name_copy, (uint64_t)leaked);
    }
    if (isolation_failed && !(exit_status == 0 && !timed_out)) {
        klog(LOG_WARN, "UTEST",
             "%s: " UTEST_RSNC_ISOLATE " (scratch/registry state may persist)",
             name_copy);
    }

    /* XML + JSON per-binary emit: one pair per run, reason string
     * reflects the final verdict including any escalations. time_ms
     * is the wall-clock elapsed from task_create to just before this
     * emit.
     *
     * `record_verdict` overrides only where the two vocabularies differ
     * (a pre-ring-3 loader failure); everywhere else the artifacts carry
     * exactly what the launcher aggregated. Escalations can only move a
     * verdict from PASS to FAIL, and a loader-stage fault is never a PASS,
     * so the override cannot mask one. */
    end_ms = u_uptime_ms();
    {
        int rv = record_verdict >= 0 ? record_verdict : *out_verdict;
        u_emit_xml_testcase(name_copy, type, rv, end_ms - start_ms,
                            reason[0] ? reason : (const char *)0,
                            (const char *)0);
        u_emit_json_testcase(name_copy, type, rv, end_ms - start_ms,
                             reason[0] ? reason : (const char *)0, &report);
    }

    /* Report dimension, emitted last so the binary's own records are
     * already on the wire: the diagnostic line the host cross-check
     * recounts, then one standard skip record per reported skip block. */
    u_emit_report_line(name_copy, &report);
    rt->skip_records += u_emit_skip_records(name_copy, type, &report,
                                            rt->skip_records, tap_point);
}

/* ---- Enumerate binaries via directory glob (fallback path) --------- */

struct glob_state {
    struct vfs_node *root;
    uint32_t         idx;
};

static const char *u_glob_next(struct glob_state *gs,
                               char *out_name, uint32_t out_cap,
                               utest_name_verdict_t *out_verdict)
{
    struct vfs_dirent *de;

    while ((de = vfs_readdir(gs->root, gs->idx)) != (struct vfs_dirent *)0) {
        utest_name_verdict_t v;

        gs->idx++;
        if (de->type & VFS_DIRECTORY)
            continue;
        /* Apply the SAME classifier used for manifest entries: both
         * must reject path separators, control bytes, and `..`
         * components.  Without the stricter gate, a directory entry
         * like `test_bad\nline.exe` would pass `u_is_test_binary`
         * (prefix+suffix only) and split a subsequent [UTEST-XML]
         * testcase record across physical log lines, corrupting the
         * post-processed JUnit XML.
         *
         * The two outcomes are no longer the same. A file that is not
         * test_*.exe is skipped exactly as before -- C:\ holds plenty of
         * files this framework has no opinion about. A file that IS
         * test-shaped and still fails the gate is RETURNED, carrying its
         * verdict, so the caller counts it as the planned-and-refused
         * binary it is instead of stepping over it in silence. */
        v = u_classify_name(de->name);
        if (v == UTEST_NAME_NOT_TEST_SHAPED)
            continue;
        if (out_verdict)
            *out_verdict = v;
        /* Snapshot the name -- de->name is shared dirent storage. */
        {
            uint32_t i;
            for (i = 0; de->name[i] && i < out_cap - 1; i++)
                out_name[i] = de->name[i];
            out_name[i] = '\0';
        }
        return out_name;
    }
    return (const char *)0;
}

/* ---- Public entry: scan + run + report ----------------------------- */

void test_usermode_run(void)
{
    struct manifest_state manifest;
    struct vfs_node      *root;
    /* [3] is an OVERLAPPING SUBSET of [1], never a fourth disjoint bucket:
     * a binary that never ran is counted once as failed and again as an
     * error, so `passed + failed + skipped` remains the run total and no
     * existing consumer of the first three changes meaning. */
    uint32_t              counters[4] = { 0, 0, 0, 0 }; /* pass, fail,
                                                         * skip, error */
    /* Report dimension, kept strictly separate from `counters` above:
     * those count BINARIES (what the exit codes said), these count what
     * the binaries reported about their own assertions and skip blocks.
     * Folding the two together would make every total ambiguous. */
    struct u_report_totals rt = { 0, 0, 0, 0, 0, 0, 0 };
    /* Running TAP point number. No longer equal to the binary ordinal:
     * a binary that reports skip blocks contributes extra points after
     * its own, which is why the `1..N` plan moved to the end of the
     * stream (TAP permits leading or trailing; only a trailing one can
     * state a count the launcher does not know up front). */
    uint32_t              tap_point = 0;
    /* Set when a `Bail out!` was emitted. TAP makes bail-out terminal, so
     * the trailing plan must not follow it. */
    int                   tap_bailed = 0;
    /* Run-completeness state, published in both machine summaries. Declared
     * at function scope because the abort is DETECTED inside the phase loop
     * but ANNOUNCED and SERIALISED after it. */
    int                   suite_aborted = 0;
    uint32_t              not_run = 0;
    uint32_t              total_planned = 0;
    uint32_t              total_ran = 0;
    uint32_t              skipped_by_filter = 0;
    /* Per-run refusal counter. Function-local, so the launcher gains no
     * shared mutable state: test_usermode_run is the single enumerator,
     * and a static here would make two runs in one boot collide. It is
     * what makes refusal identities distinct BY CONSTRUCTION -- a digest
     * alone can only make collisions unlikely, and an operator who cannot
     * tell two refused binaries apart cannot act on either. */
    uint32_t              refusal_ordinal = 0;
    /* The single enumeration this run executes from. Built once below,
     * consumed unchanged by the execution loop -- the two can no longer
     * describe different sets of binaries. */
    struct plan_state     plan;
    uint32_t              glob_refusal_count = 0;
    int                   glob_refusal_overflowed = 0;
    /* Sum of expected task_create costs across planned binaries. A
     * plain `test_*.exe` costs 1 slot; fork-heavy binaries like
     * test_process.exe tag `expects_tasks=<N>` in the manifest so the
     * pre-flight budget check reflects real TASK_MAX pressure. Codex
     * quality 2026-04-21. */
    uint32_t              total_task_budget = 0;
    int                   use_manifest;
    char                  scratch_name[VFS_MAX_NAME];
    uint64_t              run_start_ms;
    uint64_t              run_end_ms;
    uint32_t              i;

    if (!vfs_is_mounted('C')) {
        klog(LOG_DEBUG, "UTEST", "C:\\ not mounted -- skipping user-mode tests");
        return;
    }

    root = vfs_get_drive_root('C');
    if (!root || !root->ops || !root->ops->readdir) {
        klog(LOG_DEBUG, "UTEST", "C:\\ root has no readdir -- skipping");
        return;
    }

    /* Open the framed run BEFORE anything else can reach serial. Every
     * return path past this point pairs it with u_frame_end(); the two
     * checks above return without a frame on purpose, because an
     * unterminated frame is a stronger signal than no frame at all and
     * neither of those paths emits a single record. */
    u_frame_begin();

    /* CLEAR THE RUN LATCHES AT THE FRAMED BOUNDARY, before any wait or record.
     *
     * They are sticky WITHIN a run, and this function is documented as safe to
     * call repeatedly -- so a latch left set by an earlier invocation would
     * abort this one, skip its remaining binaries, and serialize aborted
     * artifacts carrying no record of the fault that caused the abort, because
     * that record belongs to the previous run's frame. Placed after
     * u_frame_begin so a fault raised by this run's very first wait is
     * retained, and after the two early returns above, which emit nothing and
     * are not runs. */
    u_run_latches_reset(&s_run_latches);

    /* Enable preemptive scheduler for the timeout watchdog. Pair with
     * scheduler_disable before returning so boot_phase3 continues in
     * its expected non-preemptive state. */
    scheduler_enable();

    /* ---- ENUMERATE ONCE ------------------------------------------- *
     *
     * Everything below builds the plan. Nothing after it enumerates: the
     * execution loop consumes this array and never touches readdir, which
     * is what makes `total_ran` a walk of the same list `total_planned`
     * counted rather than a second opinion about the directory.
     *
     * Aggregate records (the four fail-closed markers) are published from
     * STATIC storage and counted here rather than planned as entries --
     * an aggregate that needed a plan slot could be lost to the very
     * exhaustion it exists to report. They are counted in the PLANNING
     * phase, not at publication, so the task-slot preflight and the
     * "=== Running N binary/binaries ===" banner -- both of which read
     * total_planned before anything is published -- do not under-report.
     * ---------------------------------------------------------------- */
    /* BEFORE the plan, because the branch below can skip the loader and
     * every later consumer -- including u_manifest_free's frame walk --
     * reads this struct unconditionally. */
    u_manifest_reset(&manifest);

    if (!u_plan_init(&plan))
        total_planned++;                /* the alloc-failure aggregate */

    /* The parser appends runnable entries straight to the plan; refusals
     * stay in manifest_state, whose raw names the dirent dedup needs. */
    use_manifest = plan.alloc_failed ? 0 : u_manifest_load(&manifest, &plan);

    /* When the refusal identity set OVERFLOWED, nothing runs.
     *
     * No bounded array can hold an unbounded manifest, so a bigger cap
     * only moves this boundary. What matters is what the launcher may
     * CLAIM at it: past the cap it no longer holds every refused
     * identity, so it cannot certify that an accepted entry is not a
     * duplicate of a refusal it has already forgotten -- and executing a
     * partial plan while asserting the completeness this section exists
     * to provide is precisely the false green it removes. So the run
     * publishes the retained refusals plus one aggregate and executes
     * nothing; planned and ran reconcile on exactly that set, and the
     * run fails on the aggregate either way. */
    if (manifest.refused_overflowed)
        u_plan_drop_runs(&plan);

    /* Refused manifest entries are planned UNCONDITIONALLY -- outside the
     * `use_manifest` gate and outside the filter.
     *
     * Outside the gate because `use_manifest` only decides whether the
     * manifest is authoritative for what RUNS; a malformed line somebody
     * wrote is a fault whether or not any valid line kept it company.
     * Outside the filter because the filter selects among binaries we can
     * IDENTIFY, and letting a name we just declined to trust decide
     * whether it gets reported would hand it back the authority the
     * refusal withdrew -- matching against the raw bytes is the only
     * alternative, and those are exactly what must not be used.
     *
     * Stored by POINTER: these names live in the manifest arena, which
     * outlives the plan, and a refused name may exceed the plan arena's
     * fixed stride precisely because over-length is one reason to refuse. */
    for (i = 0; i < manifest.refused_count; i++) {
        if (!u_plan_add_refusal(&plan, manifest.refused_names[i],
                                manifest.refused_digest[i],
                                manifest.refused_verdict[i],
                                (const char *)0, 0, 1,
                                manifest.refused_smoke_selected[i]))
            break;      /* plan.overflowed is now set; its aggregate reports it */
    }
    if (manifest.refused_overflowed)
        total_planned++;
    if (manifest.unreadable)
        total_planned++;

    /* Glob discovery runs ONLY when no usable manifest exists.
     *
     * The old "manifest entries truncated -- tail runs via glob" fallback
     * is gone with the cap that produced it. It could never do the job
     * claimed for it: a dropped entry's `type`, `expects_tasks` and
     * position in manifest order are not recoverable from a directory
     * entry, and an entry naming a binary absent from the image is not
     * recoverable at all. Manifest runnables now enter the plan directly,
     * so there is no tail to pick up.
     *
     * Glob discovery is also disabled outright once the refusal identity
     * set has overflowed. Past the cap we no longer HOLD the identities of
     * every refused manifest entry, so u_refusal_already_seen cannot
     * promise suppression -- and a refusal whose NAME is valid (an
     * unusable attribute, say) would then be published as refused and
     * launched by the glob in the same run. Fail closed: the run is
     * already failing on the aggregate refusal, so discovering fewer
     * binaries costs nothing a green run depended on. */
    /* An UNREADABLE manifest is execution-fatal, not a reason to guess.
     *
     * The file existed and declared policy the launcher never got to read
     * -- an oversized, unallocatable or short-read manifest may well have
     * carried `type=smoke` lines. Falling back to the glob would rebuild a
     * plan from filenames alone, silently downgrading those declarations
     * to correctness and running stateful binaries with no smoke gate in
     * front of them. The run is already failing on the counted
     * `manifest unparseable` record; discovering fewer binaries costs
     * nothing a green run depended on, and no post-planning scan can
     * recover metadata that was never parsed. */
    if (!use_manifest && !manifest.refused_overflowed && !plan.alloc_failed &&
        !manifest.unreadable) {
        struct glob_state gs = { root, 0 };
        utest_name_verdict_t gv = UTEST_NAME_ACCEPT;
        while (u_glob_next(&gs, scratch_name, sizeof(scratch_name), &gv)) {
            /* An identity the MANIFEST already refused is skipped here
             * whatever the glob makes of it -- including when the glob
             * would accept it. That case is not hypothetical: an entry
             * refused for an unusable attribute has a perfectly valid
             * NAME, so the directory walk classifies the same file
             * ACCEPT and would launch it under exactly the policy the
             * refusal rejected, publishing a refusal and running the
             * binary in the same run. It is already planned and
             * published as a refusal, so skipping keeps planned and ran
             * consistent. Checked before the verdict split precisely
             * because the accepted branch needs it too. */
            if (u_refusal_already_seen(&manifest, scratch_name))
                continue;
            if (gv != UTEST_NAME_ACCEPT) {
                /* CAPTURED here, not re-discovered later. The refusal
                 * preflight used to walk C:\ again to publish these,
                 * which on IXFS is another O(N^2) pass on the path most
                 * runs take. This walk already holds the entry, so it
                 * keeps the sanitized prefix, the span digest and the
                 * verdict -- everything publication needs and nothing
                 * untrusted. */
                if (glob_refusal_count >= UTEST_GLOB_REFUSAL_MAX) {
                    glob_refusal_overflowed = 1;
                    continue;
                }
                {
                    char prefix[UTEST_REFUSAL_PREFIX_STORE];
                    uint32_t k, nlen = 0;
                    while (scratch_name[nlen]) nlen++;
                    for (k = 0; k + 1u < UTEST_REFUSAL_PREFIX_STORE &&
                                scratch_name[k]; k++)
                        prefix[k] = u_name_char_ok(scratch_name[k])
                                        ? scratch_name[k] : '_';
                    prefix[k] = '\0';
                    /* INTERNED: the sanitized bytes are built on this
                     * frame and the raw name lives in shared dirent
                     * storage, so neither survives the next readdir. */
                    /* NOT exact: `prefix` is sanitized and truncated, so
                     * it must never be identity-matched against a name. */
                    if (!u_plan_add_refusal(&plan, prefix,
                                            u_name_digest(scratch_name, nlen),
                                            (uint8_t)gv, (const char *)0, 1, 0,
                                            0))
                        break;
                    glob_refusal_count++;
                }
                continue;   /* refusals never launch, so no task budget */
            }
            /* Glob-discovered binaries carry no manifest metadata, so the
             * default 1-task cost applies. Fork-heavy binaries should be
             * declared in the manifest with expects_tasks=<N>. */
            if (!u_plan_add_run(&plan, scratch_name,
                                u_type_for_name(scratch_name), 1u, 1))
                break;
        }
    }

    /* Freeze WHAT runs under each planned name, not just which names run.
     * Ordered BEFORE the gate derivation because it can itself refuse an
     * entry: a smoke binary whose identity cannot be frozen has to reach
     * the gate as a refused smoke prerequisite, exactly as every other
     * refusal route does. */
    u_plan_freeze_identities(&plan);

    /* Every refusal is staged by now, so the gate can be read off the
     * plan in one pass. */
    u_plan_derive_smoke_gate(&plan);

    if (glob_refusal_overflowed)
        total_planned++;                /* the glob-refusal aggregate */

    /* The plan is now IMMUTABLE. Everything below reads it.
     *
     * An incomplete plan cannot certify what it did not enumerate, so it
     * runs nothing at all -- the same contract the refusal overflow above
     * takes, and for the same reason. */
    if (plan.overflowed) {
        total_planned++;                /* the plan-full aggregate */
        u_plan_drop_runs(&plan);
    }

    total_planned      += plan.count;
    skipped_by_filter   = plan.skipped_by_filter;
    for (i = 0; i < plan.count; i++)
        if (plan.entries[i].kind == (uint8_t)UTEST_PLAN_RUN)
            total_task_budget += plan.entries[i].expects_tasks;

    if (total_planned == 0) {
        if (s_filter)
            klog(LOG_DEBUG, "UTEST",
                 "no test_*.exe matches filter '%s' -- skipping",
                 s_filter);
        else
            klog(LOG_DEBUG, "UTEST",
                 "no test_*.exe found at C:\\ -- skipping summary");
        /* empty-suite artifact contract: when xml=1 or json=1 the
         * CI integration expects a parseable file regardless of
         * whether any binary ran. Emit a zero-test envelope so
         * scripts/test.sh always produces a valid build/test-results.xml
         * and tooling doesn't fail on missing artifact. Codex quality
         * M1 2026-04-20. */
        u_emit_xml_suite_open();
        u_emit_xml_suite_close(0, 0, 0, 0, 0, 0, 0, 0);
        u_emit_json_summary(0, 0, 0, &rt, 0, 0);
        u_emit_json_run_meta(0, 0);
        /* Empty suite still gets a plan (`1..0`) and a report summary, so
         * a consumer can tell "ran nothing" from "the launcher died before
         * emitting either". */
        if (s_tap_mode)
            utest_record_log(LOG_INFO, "1..0");
        u_emit_report_summary(&rt);
        /* The legacy summary line is emitted for the empty suite too. It
         * is not decoration: scripts/test.sh waits for exactly this string
         * before it post-processes anything (its poll requires the
         * user-mode summary whenever xml=/json=/tap= is set), so returning
         * without it made a zero-binary run under xml=1/json=1 hang until
         * the host timeout and exit 1 -- with the parseable empty envelope
         * sitting unread on serial. Same shape as the populated path
         * below, so the host's position-sensitive sed patterns apply
         * unchanged. */
        utest_record_log(LOG_INFO,
             "=== 0 passed, 0 failed, 0 skipped of 0 total ===");
        u_frame_end();
        u_manifest_free(&manifest);
        u_plan_free(&plan);
        scheduler_disable();
        return;
    }

    /* Pre-flight task-slot budget check: task_create uses monotonic
     * pid = num_tasks++ and rejects once num_tasks == TASK_MAX. Slots
     * never come back today (owner: [scheduler enhancement TODO]).
     * Emitting `1..total_planned` when we know the tail would hit
     * task_create failures produces misleading TAP output. Clamp the
     * plan to the available budget, log a WARN naming the ceiling, and
     * let u_run_one surface the remaining binaries as task_create-fail
     * FAILs (deterministic, named, not silent). Codex quality H2,
     * 2026-04-20. */
    {
        uint32_t live = task_count();
        uint32_t budget = (live < TASK_MAX) ? (TASK_MAX - live) : 0u;
        /* Compare against total_task_budget (sum of per-binary
         * expects_tasks), not total_planned, so fork-heavy binaries
         * like test_process.exe get counted accurately. Codex quality
         * 2026-04-21: previously the check compared against
         * total_planned (one slot per binary) and missed the extra
         * slots consumed by nested sys_fork. */
        if (budget < total_task_budget) {
            klog(LOG_WARN, "UTEST",
                 "plan %u binaries need %u task slots, only %u free "
                 "(TASK_MAX=%u, live=%u) -- tail will FAIL with "
                 "task_create-failed until scheduler slot reuse ships",
                 (uint64_t)total_planned, (uint64_t)total_task_budget,
                 (uint64_t)budget, (uint64_t)TASK_MAX, (uint64_t)live);
        }
    }

    /* XML envelope opener. The testsuite attributes are filled in
     * by scripts/test.sh post-processing using the emitted
     * [UTEST-XML-SUMMARY] line; the raw launcher doesn't know the
     * final counts yet. */
    u_emit_xml_suite_open();

    /* Opening banner, matching the kernel test runner's shape so mixed
     * boot logs visually separate the kernel TEST_CAT_* sweep from the
     * user-mode launcher. The closing summary at the end of this
     * function already follows the same `=== N passed ... ===` form. */
    klog(LOG_INFO, "UTEST",
         "============ USER-MODE TEST BINARIES ============");
    if (s_filter && s_filter[0])
        klog(LOG_INFO, "UTEST",
             "=== Running %u binary/binaries [filter='%s'] ===",
             (uint64_t)total_planned, s_filter);
    else
        klog(LOG_INFO, "UTEST",
             "=== Running %u binary/binaries ===",
             (uint64_t)total_planned);

    run_start_ms = u_uptime_ms();

    /* two-phase execution: smoke binaries always run FIRST, and a
     * single smoke FAIL aborts the rest of the suite. Everything else
     * runs in the second phase, both phases in plan order (which is
     * manifest-then-glob, since that is the order they were enumerated).
     *
     * Phase 1: smokes only. The whole plan is scanned but only entries
     * whose type is UTEST_TYPE_SMOKE dispatch. A FAIL or SKIP in this
     * phase sets smoke_failed and skips phase 2.
     *
     * Phase 2: everything non-smoke. Same order.
     *
     * Filter exclusions are counted ONCE, at plan time, so neither phase
     * has to special-case them to avoid double counting. */
    /* Enumeration refusals are published BEFORE any binary launches.
     *
     * They are not executions -- they are results the ENUMERATION already
     * produced -- and emitting them inside the phase loop made them
     * hostage to it: refusals are correctness-typed (a name we refused to
     * trust must not claim smoke policy), so they landed in the non-smoke
     * phase, which a failing smoke binary breaks out of before reaching.
     * The run still went red through the abort gate, but every specific
     * refusal record this section promises -- verdict line, TAP point,
     * XML testcase, JSON record, and the counters behind them -- was lost
     * exactly when the operator most needed to know what was refused.
     *
     * Emitting here also removes the interaction entirely: a refusal can
     * neither trigger the smoke fast-fail nor be skipped by it, and the
     * phase loop below simply steps over refused entries. */
    for (i = 0; i < plan.count; i++) {
        const struct plan_entry *e = &plan.entries[i];

        if (e->kind != (uint8_t)UTEST_PLAN_REFUSAL)
            continue;
        total_ran++;
        /* A pinned literal reason (the policy-conflict case) wins;
         * otherwise the reason follows from the name verdict. */
        u_emit_refusal(e->reason ? e->reason
                                 : u_refusal_reason(
                                       (utest_name_verdict_t)e->verdict),
                       ++refusal_ordinal, e->name, e->digest,
                       &tap_point, counters, &rt);
    }
    if (plan.alloc_failed) {
        /* Published from static storage on purpose: the plan that would
         * normally carry a record is exactly what could not be allocated.
         * Its slot was reserved during planning, so planned and ran still
         * reconcile, and the run fails on this record instead of taking
         * the zero-planned path that publishes a successful empty suite. */
        total_ran++;
        u_emit_aggregate(UTEST_RSN_PLAN_ALLOC, UTEST_AGG_PLAN_PAGES,
                         (uint32_t)UTEST_PLAN_PAGES,
                         &tap_point, counters, &rt);
    }
    if (plan.overflowed) {
        /* More entries than one run can enumerate. Past the cap the plan
         * is not the complete enumeration, so nothing ran (see above) and
         * the aggregate is what the run reconciles and fails on. */
        total_ran++;
        u_emit_aggregate(UTEST_RSN_PLAN_FULL, UTEST_AGG_PLAN_KEPT,
                         plan.kept_at_overflow, &tap_point, counters, &rt);
    }
    if (manifest.refused_overflowed) {
        /* More malformed manifest entries than the run can publish
         * individually. It rides the SAME path as any other refusal
         * rather than a bare diagnostic, because both artifact formats
         * reconcile record COUNT against the summary total (the JSON
         * harvester at scripts/utest-json-harvest.py checks
         * len(testcases) == summary.total, separately from the report
         * partition): a counter bumped without a record does not merely
         * under-describe the run, it makes the whole artifact
         * unparseable. Dropping the tail with a WARN instead is the
         * false-green this section exists to close. */
        /* total_planned is NOT touched here: the planning phase already
         * added this aggregate's single slot. Adding it again publishes a
         * phantom unrun binary -- not_run=1 and an [UTEST-RUN-INCOMPLETE]
         * on a run where every refusal, aggregate included, WAS published. */
        total_ran++;
        u_emit_aggregate(UTEST_RSN_ARRAY_FULL, UTEST_AGG_MANIFEST_KEPT,
                         manifest.refused_count,
                         &tap_point, counters, &rt);
    }
    if (manifest.unreadable) {
        /* A manifest that EXISTS but could not be parsed at all: every
         * entry in it went unexamined, including any this section would
         * have refused. Its plan slot was reserved during planning, and a
         * reserved slot with no record is an artifact a JUnit consumer
         * reads as an empty successful suite -- so it publishes like any
         * other refusal rather than leaving only a generic marker. */
        /* No value: the manifest did not parse, so how many entries it
         * held -- and therefore how many were lost -- is not knowable
         * from anything the launcher still has. The old shape published
         * `0` here, which reads as "nothing was lost" for the one path
         * where the loss is total. */
        total_ran++;
        u_emit_aggregate(UTEST_RSN_UNREADABLE, UTEST_AGG_MANIFEST_BAD,
                         0u, &tap_point, counters, &rt);
    }
    if (glob_refusal_overflowed) {
        total_ran++;
        u_emit_aggregate(UTEST_RSN_ARRAY_FULL, UTEST_AGG_GLOB_KEPT,
                         glob_refusal_count,
                         &tap_point, counters, &rt);
    }

    /* EXECUTE THE PLAN.
     *
     * One array, two passes over it -- smoke first, everything else
     * second -- and no directory traversal anywhere in here. That is the
     * whole change: `total_ran` counts entries consumed from the same list
     * `total_planned` measured, so the two can no longer describe
     * different sets of binaries, and a directory that changed under the
     * run surfaces as a NAMED entry that could not be launched rather
     * than as an anonymous shortfall.
     *
     * The filter and the refused-identity dedup already ran at plan time,
     * so neither is repeated here; every RUN entry in the plan is one the
     * launcher committed to executing. */
    {
        /* A SMOKE identity refused during planning never gets to run, so
         * the smoke gate has to fire from here rather than from a verdict.
         * Seeded before the phases so no RUN entry executes past it, which
         * is what a smoke non-PASS means everywhere else in this loop. */
        int smoke_failed = plan.smoke_refused;
        int infra_fatal = 0;
        int phase;
        for (phase = 0; phase < 2 && !smoke_failed; phase++) {
            int want_smoke = (phase == 0);
            for (i = 0; i < plan.count; i++) {
                const struct plan_entry *e = &plan.entries[i];
                int verdict;

                if (e->kind != (uint8_t)UTEST_PLAN_RUN)
                    continue;
                if ((e->type == UTEST_TYPE_SMOKE) != want_smoke)
                    continue;
                if (!u_binary_present(e->name)) {
                    /* Planned, then removed or renamed before it could
                     * launch. Published by NAME as a counted failure --
                     * the aggregate-only report this section replaced
                     * could say a binary was lost but never which one. */
                    total_ran++;
                    /* TRUSTED: e->name is an identity the classifier
                     * accepted, so it is published verbatim. The
                     * synthesized refusal id would truncate it to a
                     * ~14-byte prefix and two long planned names would
                     * become indistinguishable in the artifacts -- which
                     * is precisely the question this record exists to
                     * answer. */
                    u_emit_refusal_named(UTEST_RSN_ABSENT, ++refusal_ordinal,
                                         e->name, 0u, 1,
                                         &tap_point, counters, &rt);
                    /* A vanished SMOKE binary is a smoke non-PASS, and the
                     * gate's rule is that ANY smoke non-PASS stops the run
                     * immediately. Publishing the failure and carrying on
                     * would run stateful binaries past a foundational
                     * prerequisite that is no longer even present, and
                     * would describe the result as a completed suite where
                     * the artifacts should say aborted. */
                    if (want_smoke) {
                        smoke_failed = 1;
                        break;
                    }
                    continue;
                }
                total_ran++;
                u_run_one(e->name, e->type, &tap_point, counters, &rt,
                          &verdict, e->content_digest);
                if (s_run_latches.wait_stalled) {
                    /* Infrastructure-fatal for the same reason a degraded reap
                     * is, one step earlier in the chain: every remaining bound
                     * in this run -- the next binary's timeout, the next
                     * drain, the next reap grace -- is measured against the
                     * clock that just stopped. Continuing would produce
                     * verdicts with nothing behind them, and would do it while
                     * each of those waits burns its full watchdog budget. */
                    infra_fatal = 1;
                    break;
                }
                if (s_reap_degraded) {
                    /* Infrastructure-fatal, and it fast-fails for a STRONGER
                     * reason than the smoke gate below. A smoke failure means
                     * a prerequisite is broken; this means the launcher does
                     * not know whether a fork constructor is still running,
                     * and the next binary would be created into the slot that
                     * constructor already claimed. Continuing is not a
                     * reporting problem, it is a corruption. */
                    infra_fatal = 1;
                    break;
                }
                if (want_smoke && verdict != 0) {
                    /* Fast-fail is IMMEDIATE, per the gate's own spec:
                     * stop right here rather than after the rest of the
                     * phase. Continuing would run further binaries past a
                     * smoke failure that may have left the system in the
                     * state the smoke exists to detect, and would inflate
                     * `total_ran` so the `not_run` count published in the
                     * artifacts described a boundary the gate never had. */
                    smoke_failed = 1;
                    break;
                }
            }
            if (infra_fatal)
                break;
            if (smoke_failed && want_smoke) {
                /* One smoke non-PASS aborts the whole suite: skip phase 1.
                 * The abort is ANNOUNCED below, outside this loop, not in
                 * a phase-1 preamble -- this `break` leaves the loop
                 * entirely, so a preamble here would never execute (which
                 * is exactly how the announcement and the TAP bail-out
                 * silently stopped firing before 2026-07-29, leaving the
                 * host ABORT gate dead and TAP streams plan-inconsistent). */
                break;
            }
        }
        /* QUIESCE THE CAPTURE EPOCH HERE, before the verdict below reads the
         * stall state, and NOT only at the terminator.
         *
         * The seal and the drain also run in u_frame_end, which is the
         * protocol's home for them -- but u_frame_end is the LAST thing this
         * function does, long after `suite_aborted` is fixed and after both
         * machine artifacts have been serialized. A drain that stalled there
         * could set the sticky flag and put its record on the wire while the
         * run's own metadata had already been written as a clean, completed
         * suite: a framed stall record inside an artifact claiming success,
         * which is the false-green shape the flag exists to prevent.
         *
         * Safe to hoist because both halves are idempotent and neither can
         * lose information: the seal is a flag store, and the drain re-run at
         * the terminator finds the count already at zero (nothing can be
         * admitted once sealed, and no binary is running at this point). The
         * epoch CLOSE deliberately stays at the terminator, where its
         * write-off and its two records are counted like every other framed
         * line. */
        u_capture_seal();
        (void)u_capture_drain();

        /* A stalled clock aborts on the same footing as a degraded reap. The
         * in-loop check catches a stall suffered by a binary; this catches one
         * suffered by the run-boundary drain, which has no later reader. */
        suite_aborted = smoke_failed || infra_fatal || s_run_latches.wait_stalled;
    }

    /* Completeness reconciliation, for EVERY run and not only aborted
     * ones.
     *
     * total_planned comes from the planning walk and total_ran from a
     * SECOND traversal of C:\ taken after live children have run, so the
     * two can disagree if the directory changed in between. A blind
     * unsigned subtraction would then publish ~4.29e9 as the completeness
     * count into both machine summaries.
     *
     * Clamping alone is not enough either: reporting not_run=0 would
     * describe the run as having skipped nothing, which is the kind of
     * internally-consistent lie the completeness dimension exists to
     * prevent. So clamp AND say so on the channel the host already fails
     * on -- a disagreement between the two walks is a producer bug, not a
     * number to quietly round.
     *
     * Running it for COMPLETED runs is what closes the hole this section
     * would otherwise have left: a passing binary can delete or rename a
     * later entry between the two walks, the execution walk then omits it,
     * and a refusal that the planning walk had already counted disappears
     * from a run that still exits green. With the reconciliation here, a
     * planned binary that never produced a result is a diagnosed failure
     * instead of a silent subtraction. */
    if (total_ran > total_planned) {
        utest_record_log(LOG_ERROR,
             "[UTEST-RECORD-OVERFLOW] planned %u binaries but ran %u -- "
             "not_run is unknown",
             (uint64_t)total_planned, (uint64_t)total_ran);
        not_run = 0u;
    } else {
        not_run = total_planned - total_ran;
    }
    if (!suite_aborted && not_run > 0u) {
        /* Not an abort: nothing stopped the walk, so every planned binary
         * should have produced a record. The gap is the finding. */
        utest_record_log(LOG_ERROR,
             "[UTEST-RUN-INCOMPLETE] planned %u binaries but ran %u -- "
             "%u planned binary/binaries produced no result",
             (uint64_t)total_planned, (uint64_t)total_ran,
             (uint64_t)not_run);
    }

    /* Abort announcement -- unconditionally reachable, because it lives
     * outside the phase loop that every abort path breaks out of. */
    if (suite_aborted) {
        /* NAME THE CAUSE. Both abort routes reach this one announcement, and
         * hard-coding "smoke failed" made a degraded reap -- an unaccounted
         * fork constructor, i.e. possible task-slot corruption -- arrive at
         * CI as a smoke-gate failure that never happened. The two want
         * opposite responses (fix the prerequisite vs. investigate the
         * scheduler), so the artifacts must not blur them. The host matches
         * on `suite ABORT` / `Bail out!` and never on the cause text, so
         * naming it costs nothing downstream. */
        const char *abort_cause = s_run_latches.wait_stalled  ? "clock stalled"
                                : s_reap_degraded ? "reap degraded"
                                                  : "smoke failed";

        utest_record_log(LOG_ERROR,
             "suite ABORT (%s) -- skipping %u remaining binaries",
             abort_cause, (uint64_t)not_run);
        /* TAP contract: if the plan `1..N` was emitted but we will not
         * produce N results, emit a `Bail out!` record so TAP consumers
         * (scripts/test.sh, kselftest-style runners, CI parsers) treat the
         * run as aborted instead of "missing N-K results = malformed".
         * Codex quality Phase-2, 2026-04-20. */
        if (s_tap_mode) {
            utest_record_log(LOG_INFO,
                 "Bail out! %s -- %u remaining binaries skipped",
                 abort_cause, (uint64_t)not_run);
            /* Bail-out is TERMINAL in TAP: no further points and no plan
             * may follow it. Recorded so the trailing plan at the bottom
             * of this function stays suppressed. */
            tap_bailed = 1;
        }
    }

    /* Both arenas are released HERE, after execution, so no plan entry
     * ever outlives the bytes its name points at. */
    u_manifest_free(&manifest);
    u_plan_free(&plan);
    scheduler_disable();

    run_end_ms = u_uptime_ms();

    /* Summary. Counters: [0]=pass, [1]=fail, [2]=skip(exit=77),
     * [3]=error (the subset of [1] that never ran). The legacy line below
     * reports the first three only -- it is what the host recount and the
     * boot-completion poll key on, and the never-ran split rides the
     * machine artifacts instead. */
    if (skipped_by_filter > 0) {
        utest_record_log(LOG_INFO,
             "=== %u passed, %u failed, %u skipped of %u total "
             "(%u filtered) ===",
             (uint64_t)counters[0], (uint64_t)counters[1],
             (uint64_t)counters[2], (uint64_t)total_ran,
             (uint64_t)skipped_by_filter);
    } else {
        utest_record_log(LOG_INFO,
             "=== %u passed, %u failed, %u skipped of %u total ===",
             (uint64_t)counters[0], (uint64_t)counters[1],
             (uint64_t)counters[2], (uint64_t)total_ran);
    }

    /* TAP plan, emitted LAST. TAP 13 allows the plan at either end of the
     * stream, and only the trailing position can state a count that
     * includes the per-skip-block points -- the launcher does not know how
     * many binaries will report skips until they have all run.
     *
     * SUPPRESSED after a `Bail out!`. The TAP format makes a bail-out
     * terminal: nothing may follow it, and a consumer that has stopped
     * parsing would either miss the plan or treat the stream as malformed.
     * An aborted run is correctly plan-less -- the bail-out line IS the
     * verdict. */
    if (s_tap_mode && !tap_bailed)
        utest_record_log(LOG_INFO, "1..%u", (uint64_t)tap_point);

    /* summary emissions: close the XML envelope and drop the final
     * JSON summary record. Both are no-ops if the respective modes
     * were never enabled. The report summary is unconditional -- it is a
     * plain diagnostic line, not part of either machine artifact, and the
     * host cross-check needs it whenever any binary reported. */
    u_emit_xml_suite_close(counters[0], counters[1], counters[2],
                           rt.skip_records, run_end_ms - run_start_ms,
                           suite_aborted, not_run, counters[3]);
    u_emit_json_summary(counters[0], counters[1], counters[2], &rt,
                        run_end_ms - run_start_ms, counters[3]);
    /* Emitted AFTER the summary so the host assembler sees the completeness
     * record as the stream's terminator and can reject anything that
     * follows it as a cut-and-resumed stream. */
    u_emit_json_run_meta(suite_aborted, not_run);
    u_emit_report_summary(&rt);
    /* Terminator, emitted last on every path including an aborted suite:
     * it is what the host waits on instead of the summary, and what it
     * reconciles its own framed-line count against. */
    u_frame_end();
}

/* ---- Test-only exports --------------------------------------------- *
 *
 * Thin wrappers around file-local helpers so kernel unit tests can
 * exercise them without promoting the helpers to the public header.
 * The main public API (test_usermode_run + setters) stays minimal. */
#ifdef KERNEL_TESTS
int test_usermode_is_valid_manifest_name(const char *name)
{
    return u_is_valid_manifest_name(name);
}

/* The refusal taxonomy and the derived name bound. Exported as plain
 * integers so the test file does not need the file-local enum: the
 * numeric values ARE the contract the tests pin, and a reordering of the
 * enum that changed them would be caught by those assertions. */
int test_usermode_classify_name(const char *name);
int test_usermode_classify_name(const char *name)
{
    return (int)u_classify_name(name);
}

int test_usermode_classify_name_span(const char *name, uint32_t span_len);
int test_usermode_classify_name_span(const char *name, uint32_t span_len)
{
    return (int)u_classify_name_span(name, span_len);
}

uint32_t test_usermode_max_binary_name(void);
uint32_t test_usermode_max_binary_name(void)
{
    return (uint32_t)UTEST_MAX_BINARY_NAME;
}

/* Per-record room left at the derived bound. The tests use it to prove
 * the minimum was taken across EVERY formatter rather than assumed from
 * the one that looks worst -- the property the derivation exists for. */
uint32_t test_usermode_name_room(uint32_t kind);
uint32_t test_usermode_name_room(uint32_t kind)
{
    switch (kind) {
    case 0:  return UTEST_NAME_ROOM(UTEST_FIXED_VERDICT, 1u);
    case 1:  return UTEST_NAME_ROOM(UTEST_FIXED_XML, 1u);
    case 2:  return UTEST_NAME_ROOM(UTEST_FIXED_JSON_BINARY, 1u);
    case 3:  return UTEST_NAME_ROOM(UTEST_FIXED_JSON_SKIP, 2u);
    case 4:  return UTEST_NAME_ROOM(UTEST_FIXED_TAP, 1u);
    case 5:  return UTEST_NAME_ROOM(UTEST_FIXED_REPORT, 1u);
    /* The SEVENTH kind. UTEST_MAX_BINARY_NAME minimizes over seven
     * UTEST_NAME_ROOM terms, but this switch exported only six, so the
     * test that claims to prove "the minimum was taken across EVERY
     * formatter" could not see the XML skip record at all. It leaves 80
     * bytes today and is nowhere near binding -- which is exactly why the
     * omission was invisible, and exactly what a completeness claim must
     * not rest on. */
    case 6:  return UTEST_NAME_ROOM(UTEST_FIXED_XML_SKIP, 1u);
    default: return 0u;
    }
}

uint32_t test_usermode_name_digest(const char *p, uint32_t len);
uint32_t test_usermode_name_digest(const char *p, uint32_t len)
{
    return u_name_digest(p, len);
}

int test_usermode_build_refusal_id(char *dst, uint32_t cap, uint32_t ordinal,
                                   const char *raw, uint32_t digest);
int test_usermode_build_refusal_id(char *dst, uint32_t cap, uint32_t ordinal,
                                   const char *raw, uint32_t digest)
{
    return u_build_refusal_id(dst, cap, ordinal, raw, digest);
}

/* The aggregate table, exported so the tests can walk EVERY kind rather
 * than a hand-copied list of five. A test that enumerates 0 ..
 * test_usermode_aggregate_count() - 1 fails the moment a new aggregate is
 * added without a conforming identity, which a mirrored list in the test
 * would not. */
uint32_t test_usermode_aggregate_count(void);
uint32_t test_usermode_aggregate_count(void)
{
    return (uint32_t)UTEST_AGG_COUNT;
}

const char *test_usermode_aggregate_label(uint32_t kind);
const char *test_usermode_aggregate_label(uint32_t kind)
{
    return u_aggregate_label((utest_agg_kind_t)kind);
}

int test_usermode_build_aggregate_id(char *dst, uint32_t cap, uint32_t kind,
                                     uint32_t value);
int test_usermode_build_aggregate_id(char *dst, uint32_t cap, uint32_t kind,
                                     uint32_t value)
{
    return u_build_aggregate_id(dst, cap, (utest_agg_kind_t)kind, value);
}

int test_usermode_aggregate_has_value(uint32_t kind);
int test_usermode_aggregate_has_value(uint32_t kind)
{
    return u_aggregate_has_value((utest_agg_kind_t)kind);
}

uint64_t test_usermode_clamp_time_ms(uint64_t ms);
uint64_t test_usermode_clamp_time_ms(uint64_t ms)
{
    return u_clamp_time_ms(ms);
}

/* The two per-testcase formatters, so a test can build a worst-case
 * record at the derived bound and assert it fits. This is what closes
 * the loop on the derivation: the fixed-cost macros are a SECOND copy of
 * these format strings, and only formatting through the real ones proves
 * the copies have not drifted. `report_valid` selects the widest shape
 * (the three report fields ride only on an accepted report). */
int test_usermode_format_xml_testcase(char *dst, uint32_t cap,
                                      const char *name, int verdict,
                                      uint64_t time_ms, const char *reason);
int test_usermode_format_xml_testcase(char *dst, uint32_t cap,
                                      const char *name, int verdict,
                                      uint64_t time_ms, const char *reason)
{
    return u_format_xml_testcase(dst, cap, name, UTEST_TYPE_CORRECTNESS,
                                 verdict, time_ms, reason,
                                 (const char *)0);
}

int test_usermode_format_json_testcase(char *dst, uint32_t cap,
                                       const char *name, int verdict,
                                       uint64_t time_ms, const char *reason,
                                       int report_valid);
int test_usermode_format_json_testcase(char *dst, uint32_t cap,
                                       const char *name, int verdict,
                                       uint64_t time_ms, const char *reason,
                                       int report_valid)
{
    struct u_report rep;

    rep.state          = TASK_UTEST_REPORT_VALID;
    rep.asserts_passed = TASK_UTEST_REPORT_MAX;
    rep.asserts_failed = TASK_UTEST_REPORT_MAX;
    rep.skip_blocks    = TASK_UTEST_REPORT_SKIP_MAX;
    return u_format_json_testcase(dst, cap, name, UTEST_TYPE_CORRECTNESS,
                                  verdict, time_ms, reason,
                                  report_valid ? &rep
                                               : (const struct u_report *)0);
}

/* The widest reason the launcher can compose, so a test can build the
 * worst-case record without hard-coding a literal that would drift from
 * the derivation it is meant to check. */
uint32_t test_usermode_reason_max(void);
uint32_t test_usermode_reason_max(void)
{
    return (uint32_t)UTEST_REASON_MAX;
}

/* Seam over the PRODUCTION exit-reason renderer, so a test observes what the
 * launcher actually composes for a given status rather than a copy of it.
 *
 * Exists because moving the timeout marker into the reserved exit-reason
 * block widened the status values this renderer must carry: every previous
 * kernel-assigned cause it saw was a single digit, and a reserved reason is
 * four plus a sign. The width proof (UTEST_REASON_MAX, asserted below
 * UTEST_REASON_BUF) is derived from UTEST_DIGITS_U32 and so already covers
 * it, but "the assert covers it" and "the renderer emits it" are different
 * claims, and only the second one is what a consumer reads.
 *
 * Renders into a correctly-extended local rather than into the caller's
 * buffer: u_reason_exit takes `char (*)[UTEST_REASON_BUF]` precisely so a
 * short buffer is a constraint violation the build rejects, and forwarding a
 * caller pointer would throw that guarantee away at the seam. Copies out
 * under the caller's cap and returns 0 if it does not fit, so a mis-sized
 * test buffer fails the assertion instead of truncating silently. */
/* Seam over the timeout path's status stamp, so a test asserts the
 * postcondition the PRODUCTION helper establishes rather than a copy of the
 * constant. Takes the scratch TCB the caller owns; touches only exit_status,
 * so it is safe against a zeroed struct with no live subsystems behind it.
 *
 * The NULL guard is the SEAM's own, and production does not have it: the
 * helper dereferences unconditionally, and u_wait_with_timeout is safe only
 * because it returns early when task_get_by_pid finds nothing. So this guard
 * says "a test may pass NULL without faulting the suite", NOT "production
 * survives a NULL child" -- the difference matters, because the second claim
 * would be false. */
void test_usermode_stamp_timeout_status(struct task *t);
void test_usermode_stamp_timeout_status(struct task *t)
{
    if (t)
        u_stamp_timeout_status(t);
}

/* The stall stamp's seam, on the same contract as the timeout one above and a
 * SEPARATE entry point for the same reason the two stamps are separate
 * functions: the property worth asserting is that these two causes cannot
 * converge, and a shim taking the status as an argument could not tell. */
void test_usermode_stamp_stalled_status(struct task *t);
void test_usermode_stamp_stalled_status(struct task *t)
{
    if (t)
        u_stamp_stalled_status(t);
}

/* The run-boundary latch clear, over the CALLER's struct rather than the
 * launcher's own. The property that matters -- a run does not inherit the
 * previous run's abort -- is otherwise reachable only by running the launcher
 * twice, which needs live scheduling, signals, a clock and a mounted C:, all
 * of which a test_*.c is forbidden from touching. */
void test_usermode_run_latches_reset(struct utest_run_latches *l)
{
    if (l)
        u_run_latches_reset(l);
}

uint32_t test_usermode_reason_exit(char *dst, uint32_t cap, int32_t status);
uint32_t test_usermode_reason_exit(char *dst, uint32_t cap, int32_t status)
{
    char     buf[UTEST_REASON_BUF];
    uint32_t n = 0;

    if (!dst || !cap)
        return 0;
    dst[0] = '\0';
    u_reason_exit(&buf, status);
    while (buf[n] != '\0')
        n++;
    if (n + 1u > cap)
        return 0;
    for (uint32_t i = 0; i <= n; i++)
        dst[i] = buf[i];
    return n;
}

/* Filesystem-identity comparison. Exported because getting it wrong does
 * not crash -- it silently plans, runs or refuses one file twice. */
int test_usermode_name_equal_fs(const char *a, const char *b);
int test_usermode_name_equal_fs(const char *a, const char *b)
{
    return u_name_equal_fs(a, b, VFS_MAX_NAME);
}

/* The manifest-vs-dirent refusal dedup, over a synthetic one-entry state.
 * This predicate is the ONLY net for its own failure mode: publishing one
 * bad binary as two failures leaves total_planned and total_ran in
 * agreement, so the completeness reconciliation cannot see it. */
int test_usermode_refusal_already_seen(const char *stored, int stored_verdict,
                                       const char *candidate);
int test_usermode_refusal_already_seen(const char *stored, int stored_verdict,
                                       const char *candidate)
{
    struct manifest_state ms;

    ms.refused_count      = 1;
    ms.refused_names[0]   = stored;
    ms.refused_verdict[0] = (uint8_t)stored_verdict;
    ms.refused_digest[0]  = 0u;
    return u_refusal_already_seen(&ms, candidate);
}

/* ---- Enumeration-plan test shims ----------------------------------- *
 *
 * Each builds a PRIVATE plan, exercises one rule and frees it, so the
 * test file needs neither `struct plan_state` nor the PMM lifecycle, and
 * no test can leave a plan behind. They exercise the same u_plan_* code
 * the launcher runs; none of them touches the live launcher, the
 * scheduler, or the VFS.
 * ------------------------------------------------------------------- */

/* Plan two runnable identities and report how the plan resolved them.
 * Returns 0 if the plan could not be allocated (the caller skips), else 1
 * with the RUN and REFUSAL entry counts. */
int test_usermode_plan_dedup(const char *a_name, int a_type,
                             uint32_t a_expects, const char *b_name,
                             int b_type, uint32_t b_expects,
                             uint32_t *out_runs, uint32_t *out_refusals,
                             uint32_t *out_smoke_refused);
int test_usermode_plan_dedup(const char *a_name, int a_type,
                             uint32_t a_expects, const char *b_name,
                             int b_type, uint32_t b_expects,
                             uint32_t *out_runs, uint32_t *out_refusals,
                             uint32_t *out_smoke_refused)
{
    struct plan_state ps;
    uint32_t i, runs = 0, refusals = 0;
    /* u_plan_add_run applies the live `utest_filter=` value. Save and
     * restore it around the probe so a filtered boot cannot make this
     * assertion vacuous by excluding the synthetic names. */
    const char *saved_filter = s_filter;

    if (!u_plan_init(&ps))
        return 0;
    s_filter = (const char *)0;
    u_plan_add_run(&ps, a_name, (utest_type_t)a_type, (uint8_t)a_expects, 1);
    u_plan_add_run(&ps, b_name, (utest_type_t)b_type, (uint8_t)b_expects, 1);
    s_filter = saved_filter;
    for (i = 0; i < ps.count; i++) {
        if (ps.entries[i].kind == (uint8_t)UTEST_PLAN_RUN) runs++;
        else                                               refusals++;
    }
    u_plan_derive_smoke_gate(&ps);
    if (out_smoke_refused)
        *out_smoke_refused = (uint32_t)ps.smoke_refused;
    u_plan_free(&ps);
    if (out_runs)     *out_runs = runs;
    if (out_refusals) *out_refusals = refusals;
    return 1;
}

/* Fill the plan past its capacity, then apply the fail-closed rule the
 * launcher applies. Reports whether the plan flagged the exhaustion and
 * how many runnable entries survived it -- which must be zero, because an
 * incomplete enumeration may not execute a partial plan. */
/* `out_kept_before_drop` is the value the plan-full aggregate publishes:
 * plan.count as it stands BEFORE u_plan_drop_runs rewrites it. It is
 * reported separately from the post-drop count precisely because the two
 * differ on this path, and publishing the post-drop one would report a
 * full plan as `agg_plan_kept_0.exe`. */
int test_usermode_plan_overflow(uint32_t *out_overflowed,
                                uint32_t *out_runs_after,
                                uint32_t *out_kept_before_drop);
int test_usermode_plan_overflow(uint32_t *out_overflowed,
                                uint32_t *out_runs_after,
                                uint32_t *out_kept_before_drop)
{
    struct plan_state ps;
    uint32_t i, runs = 0;
    char name[16];
    const char *saved_filter = s_filter;   /* see the dedup probe above */

    if (!u_plan_init(&ps))
        return 0;
    s_filter = (const char *)0;
    /* Distinct names so the dedup never folds two of them together --
     * this test is about capacity, not identity. */
    for (i = 0; i < UTEST_PLAN_MAX + 4u; i++) {
        uint32_t v = i, p = 0, d;
        name[p++] = 't'; name[p++] = '_';
        for (d = 100000u; d > 0u; d /= 10u) {
            name[p++] = (char)('0' + (v / d) % 10u);
        }
        name[p] = '\0';
        u_plan_add_run(&ps, name, UTEST_TYPE_CORRECTNESS, 1u, 1);
    }
    s_filter = saved_filter;
    if (out_overflowed) *out_overflowed = (uint32_t)ps.overflowed;
    /* The IMMUTABLE capture, not ps.count: the point of the field is that
     * it survives every later compaction. */
    if (out_kept_before_drop) *out_kept_before_drop = ps.kept_at_overflow;
    /* Drop TWICE, the way a manifest that overflows both the plan cap and
     * the refusal cap does -- the early drop at the refusal-overflow site
     * runs before the plan-full aggregate is emitted, and reading the
     * count late reported the staged-refusal count as the retained one. */
    u_plan_drop_runs(&ps);
    u_plan_drop_runs(&ps);
    if (out_kept_before_drop && *out_kept_before_drop != ps.kept_at_overflow)
        *out_kept_before_drop = 0;   /* mutated by a drop -- fail the test */
    for (i = 0; i < ps.count; i++)
        if (ps.entries[i].kind == (uint8_t)UTEST_PLAN_RUN) runs++;
    u_plan_free(&ps);
    if (out_runs_after) *out_runs_after = runs;
    return 1;
}

/* The plan's entry capacity, so a test can state an expectation derived
 * from it (half the slots are refusals in the interleaved probe) instead
 * of hardcoding a number that silently stops matching. */
uint32_t test_usermode_plan_capacity(void);
uint32_t test_usermode_plan_capacity(void)
{
    return UTEST_PLAN_MAX;
}

/* Parse caller-owned manifest TEXT into a private plan and report what
 * survived. `buf` must have room for a terminator at buf[len] (the
 * tokenizer writes one), which the caller sizes.
 *
 * This is the parser-to-plan seam this section introduced: entries no
 * longer stop at a capped runnable array, so a regression there would
 * silently restore the metadata loss the section removed. Returns 0 if
 * the plan could not be allocated. */
int test_usermode_manifest_parse(char *buf, uint32_t len, const char *filter,
                                 uint32_t *out_runs,
                                 uint32_t *out_refusals,
                                 uint32_t *out_authoritative,
                                 uint32_t *out_smoke_refused,
                                 uint32_t *out_first_type,
                                 uint32_t *out_first_expects,
                                 uint32_t *out_last_type,
                                 uint32_t *out_last_expects);
int test_usermode_manifest_parse(char *buf, uint32_t len, const char *filter,
                                 uint32_t *out_runs,
                                 uint32_t *out_refusals,
                                 uint32_t *out_authoritative,
                                 uint32_t *out_smoke_refused,
                                 uint32_t *out_first_type,
                                 uint32_t *out_first_expects,
                                 uint32_t *out_last_type,
                                 uint32_t *out_last_expects)
{
    struct plan_state    ps;
    struct manifest_state ms;
    uint32_t i, runs = 0, refusals = 0;
    int first = -1, last = -1;
    const char *saved_filter = s_filter;

    if (!u_plan_init(&ps))
        return 0;
    u_manifest_reset(&ms);
    /* The filter is a PARAMETER, not a forced NULL: forcing it hid the
     * path where a manifest whose entries are all excluded must still be
     * authoritative. Saved and restored so a filtered boot is unaffected. */
    s_filter = filter;
    u_manifest_parse(&ms, &ps, buf, len);
    s_filter = saved_filter;
    /* Exactly what u_manifest_load returns to the launcher, and therefore
     * what decides whether the directory glob runs at all. */
    if (out_authoritative) *out_authoritative = (ms.count > 0) ? 1u : 0u;
    /* Stage manifest refusals into the plan exactly as test_usermode_run
     * does. Without this the shim stops one step short of the launcher's
     * actual plan: manifest refusals would still be sitting in
     * manifest_state, so the identity dedup u_plan_add_refusal performs at
     * staging -- the thing that keeps one file from publishing two failure
     * records -- would never run, and counting the two stores separately
     * would report that duplicate as normal. */
    for (i = 0; i < ms.refused_count; i++) {
        if (!u_plan_add_refusal(&ps, ms.refused_names[i], ms.refused_digest[i],
                                ms.refused_verdict[i], (const char *)0, 0, 1,
                                ms.refused_smoke_selected[i]))
            break;
    }
    /* Derived AFTER staging, because the refusal records are what carry
     * the smoke provenance -- reading the flag before they exist is how
     * this shim previously reported 0 for a run the launcher aborts. */
    u_plan_derive_smoke_gate(&ps);
    if (out_smoke_refused) *out_smoke_refused = (uint32_t)ps.smoke_refused;
    for (i = 0; i < ps.count; i++) {
        if (ps.entries[i].kind == (uint8_t)UTEST_PLAN_RUN) {
            if (first < 0) first = (int)i;
            last = (int)i;
            runs++;
        } else {
            refusals++;
        }
    }
    if (out_runs)     *out_runs = runs;
    if (out_refusals) *out_refusals = refusals;
    if (out_first_type)
        *out_first_type = (first < 0) ? 0xFFFFFFFFu
                                      : (uint32_t)ps.entries[first].type;
    if (out_first_expects)
        *out_first_expects = (first < 0) ? 0u
                                    : ps.entries[first].expects_tasks;
    if (out_last_type)
        *out_last_type = (last < 0) ? 0xFFFFFFFFu
                                    : (uint32_t)ps.entries[last].type;
    if (out_last_expects)
        *out_last_expects = (last < 0) ? 0u : ps.entries[last].expects_tasks;
    u_plan_free(&ps);
    return 1;
}

/* Plan a refusal whose stored name is a LOSSY rendering (a sanitized glob
 * prefix), then plan a runnable binary that happens to spell the same
 * bytes, and report whether the runnable survived.
 *
 * `test_bad?.exe` is stored as `test_bad_.exe` because `?` is outside the
 * accepted charset; a genuinely different `test_bad_.exe` on disk must
 * still run. Suppressing it would drop a requested test silently, which
 * is worse than the double publication the terminal rule prevents. */
int test_usermode_plan_lossy_refusal_suppresses(int exact, uint32_t *out_runs);
int test_usermode_plan_lossy_refusal_suppresses(int exact, uint32_t *out_runs)
{
    struct plan_state ps;
    uint32_t i, runs = 0;
    const char *saved_filter = s_filter;

    if (!u_plan_init(&ps))
        return 0;
    s_filter = (const char *)0;
    u_plan_add_refusal(&ps, "test_bad_.exe", 0u,
                       (uint8_t)UTEST_NAME_REFUSE_CHARSET, (const char *)0,
                       1, exact, 0);
    u_plan_add_run(&ps, "test_bad_.exe", UTEST_TYPE_CORRECTNESS, 1u, 1);
    s_filter = saved_filter;
    for (i = 0; i < ps.count; i++)
        if (ps.entries[i].kind == (uint8_t)UTEST_PLAN_RUN)
            runs++;
    u_plan_free(&ps);
    if (out_runs) *out_runs = runs;
    return 1;
}

/* Force the plan's backing allocation to fail and report the resulting
 * state. The alloc-failure path is what keeps an unallocatable plan from
 * publishing a successful empty suite, so it must be asserted rather than
 * skipped past. */
int test_usermode_plan_alloc_failure(uint32_t *out_alloc_failed,
                                     uint32_t *out_overflowed,
                                     uint32_t *out_count,
                                     uint32_t *out_pointers_null);
int test_usermode_plan_alloc_failure(uint32_t *out_alloc_failed,
                                     uint32_t *out_overflowed,
                                     uint32_t *out_count,
                                     uint32_t *out_pointers_null)
{
    struct plan_state ps;
    int ok;

    pmm_alloc_fail_next();
    ok = u_plan_init(&ps);
    /* Disarm unconditionally. pmm_fault_should_fire() returns without
     * consuming the countdown when the IRQL is not PASSIVE, so on the
     * not-fired path the injection would stay armed and claim the next
     * unrelated allocation. */
    pmm_alloc_fail_countdown_clear();
    if (out_alloc_failed)   *out_alloc_failed = (uint32_t)ps.alloc_failed;
    if (out_overflowed)     *out_overflowed   = (uint32_t)ps.overflowed;
    if (out_count)          *out_count        = ps.count;
    if (out_pointers_null)
        *out_pointers_null = (ps.entries == (struct plan_entry *)0 &&
                              ps.names == (char *)0 &&
                              ps.block_phys == 0) ? 1u : 0u;
    /* Free unconditionally: on the (unexpected) success path this returns
     * the block, and on the failure path it is a documented no-op. */
    u_plan_free(&ps);
    return ok ? 1 : 0;
}

/* Interleave refusals among runnable entries, exhaust the NAME arena
 * rather than the entry array, then compact the runnables away.
 *
 * Covers three branches the capacity test cannot reach: u_plan_intern's
 * own exhaustion (distinct from u_plan_alloc's), the rollback that keeps
 * a failed intern from leaving a nameless entry, and u_plan_drop_runs
 * compaction with entries of both kinds interleaved. */
int test_usermode_plan_intern_and_compaction(uint32_t *out_overflowed,
                                             uint32_t *out_count_stable,
                                             uint32_t *out_refusals_kept,
                                             uint32_t *out_order_kept);
int test_usermode_plan_intern_and_compaction(uint32_t *out_overflowed,
                                             uint32_t *out_count_stable,
                                             uint32_t *out_refusals_kept,
                                             uint32_t *out_order_kept)
{
    struct plan_state ps;
    uint32_t i, before, refusals = 0, order_kept = 1;
    char name[16];
    const char *saved_filter = s_filter;

    if (!u_plan_init(&ps))
        return 0;
    s_filter = (const char *)0;
    /* Alternate kinds so compaction has something to preserve, and give
     * every refusal a digest equal to its ordinal so order is checkable
     * after the runnables are removed. */
    for (i = 0; i < UTEST_PLAN_MAX; i++) {
        uint32_t v = i, p = 0, d;
        name[p++] = 't'; name[p++] = '_';
        for (d = 100000u; d > 0u; d /= 10u)
            name[p++] = (char)('0' + (v / d) % 10u);
        name[p] = '\0';
        if (i & 1u)
            u_plan_add_refusal(&ps, name, i, (uint8_t)UTEST_NAME_REFUSE_CHARSET,
                               (const char *)0, 1, 0, 0);
        else
            u_plan_add_run(&ps, name, UTEST_TYPE_CORRECTNESS, 1u, 1);
    }
    /* The arena holds exactly UTEST_PLAN_MAX slots, so it is exhausted at
     * the same point the entry array is. Ask for one more INTERNED entry
     * while forcing the arena to be the binding limit: count must not
     * grow and no nameless entry may remain. */
    before = ps.count;
    u_plan_add_refusal(&ps, "t_extra.exe", 0u,
                       (uint8_t)UTEST_NAME_REFUSE_CHARSET, (const char *)0,
                       1, 0, 0);
    s_filter = saved_filter;
    if (out_overflowed)   *out_overflowed = (uint32_t)ps.overflowed;
    if (out_count_stable) *out_count_stable = (ps.count == before) ? 1u : 0u;
    for (i = 0; i < ps.count; i++)
        if (ps.entries[i].name == (const char *)0)
            order_kept = 0;             /* a nameless entry survived */

    u_plan_drop_runs(&ps);
    for (i = 0; i < ps.count; i++) {
        if (ps.entries[i].kind != (uint8_t)UTEST_PLAN_REFUSAL) {
            order_kept = 0;             /* a runnable survived the drop */
            continue;
        }
        refusals++;
        /* Digests were assigned in ascending plan order; compaction must
         * preserve that relative order. */
        if (i > 0 && ps.entries[i].digest <= ps.entries[i - 1].digest)
            order_kept = 0;
    }
    u_plan_free(&ps);
    if (out_refusals_kept) *out_refusals_kept = refusals;
    if (out_order_kept)    *out_order_kept = order_kept;
    return 1;
}

/* Freeze one binary's content identity exactly as the planning walk does.
 * Owns the scratch page the launcher's freeze pass reuses across entries,
 * so a test can digest a single file without standing up a plan. */
int test_usermode_digest_binary(const char *name,
                                uint8_t out[SHA256_DIGEST_LEN]);
int test_usermode_digest_binary(const char *name,
                                uint8_t out[SHA256_DIGEST_LEN])
{
    uintptr_t phys;
    int rc;

    phys = pmm_alloc_contiguous(1);
    if (!phys)
        return 0;
    /* One page deliberately: the shim exercises the MULTI-chunk path for
     * any binary over 4 KiB, which is the loop the freeze pass would
     * otherwise only reach on a file larger than its 64 KiB buffer. */
    rc = u_digest_binary(name, (uint8_t *)phys, 4096u, out, (uint64_t *)0);
    pmm_free_contiguous(phys, 1);
    return rc;
}

/* The verifier the loader gates task_exec on. Exported so the fail-closed
 * NULL case and the all-bytes compare are assertable without a spawn. */
int test_usermode_identity_matches(const uint8_t *expect,
                                   const uint8_t *actual);
int test_usermode_identity_matches(const uint8_t *expect,
                                   const uint8_t *actual)
{
    return u_identity_matches(expect, actual);
}

/* Plan one runnable name, freeze identities over it, and report what the
 * freeze did. Answers the question the section turns on: an entry whose
 * bytes cannot be established must leave the plan as a counted REFUSAL --
 * never as a runnable the execution loop would launch unverified -- and it
 * must keep its smoke provenance so the gate still sees it.
 *
 * out_kind receives the entry's kind, out_smoke_refused the gate the plan
 * derives afterwards. Returns 0 if the plan could not be built. */
int test_usermode_freeze_refuses(const char *name, int as_smoke,
                                 uint32_t *out_kind,
                                 uint32_t *out_smoke_refused,
                                 const char **out_reason);
int test_usermode_freeze_refuses(const char *name, int as_smoke,
                                 uint32_t *out_kind,
                                 uint32_t *out_smoke_refused,
                                 const char **out_reason)
{
    struct plan_state ps;

    if (!u_plan_init(&ps))
        return 0;
    if (!u_plan_add_run(&ps, name,
                        as_smoke ? UTEST_TYPE_SMOKE : UTEST_TYPE_CORRECTNESS,
                        1u, 1)) {
        u_plan_free(&ps);
        return 0;
    }
    /* Deliberately does NOT set smoke_selected: deriving it is the
     * production behaviour under test. Setting it here would have made
     * this probe pass against a freeze that dropped the provenance. */
    u_plan_freeze_identities(&ps);
    u_plan_derive_smoke_gate(&ps);

    if (ps.count == 0) {
        u_plan_free(&ps);
        return 0;
    }
    if (out_kind)          *out_kind = ps.entries[0].kind;
    if (out_reason)        *out_reason = ps.entries[0].reason;
    if (out_smoke_refused) *out_smoke_refused = (uint32_t)ps.smoke_refused;
    u_plan_free(&ps);
    return 1;
}

/* Does a name the plan carries survive the path build the executor uses?
 * A name too long for u_run_one's buffer is refused BEFORE any VFS call,
 * which is the part that is pure enough to assert on here. */
int test_usermode_binary_present(const char *name);
int test_usermode_binary_present(const char *name)
{
    return u_binary_present(name);
}

/* The BINDING formatter: the skip_block carries the name twice, so it is
 * the kind the derived bound comes from. Exported so a test can build it
 * at the bound with a worst-case skip index. */
int test_usermode_format_json_skip(char *dst, uint32_t cap,
                                   const char *rec_name, const char *parent,
                                   uint32_t index);
int test_usermode_format_json_skip(char *dst, uint32_t cap,
                                   const char *rec_name, const char *parent,
                                   uint32_t index)
{
    return u_format_json_skip_record(dst, cap, rec_name, parent, index);
}


int test_usermode_frame_tag_format(char *dst, uint32_t cap, uint32_t nonce)
{
    return u_frame_tag_format(dst, cap, nonce);
}

uint32_t test_usermode_frame_nonce_fold(uint64_t mixed)
{
    return u_frame_nonce_fold(mixed);
}

/* Is a framed run OPEN right now? A test that emits a REAL launcher record
 * needs this: while a run is open the record is authenticated INTO that run's
 * slice and counted in its reconciliation, which for a synthetic owner pid the
 * launcher never spawned is precisely the population drift
 * `capture_unexpected_channel` refuses. Outside an open run the same emission
 * lands between slices, where utest-frame.py retains nothing.
 *
 * Deliberately NOT `s_frame_ready`: that flag is sticky for the whole boot, so
 * a guard on it would permanently disable the caller after the first run ever
 * completes -- masking the regression the test exists to catch instead of
 * avoiding a side effect. */
int test_usermode_frame_run_is_open(void);
int test_usermode_frame_run_is_open(void)
{
    return __atomic_load_n(&s_frame_open, __ATOMIC_ACQUIRE) ? 1 : 0;
}

uint32_t test_usermode_frame_tag_cap(void)
{
    return (uint32_t)UTEST_FRAME_TAG_MAX;
}

int test_usermode_capture_escape(const char *raw, uint32_t raw_len,
                                 char *out, uint32_t out_cap);
int test_usermode_capture_escape(const char *raw, uint32_t raw_len,
                                 char *out, uint32_t out_cap)
{
    uint32_t r = u_capture_escape(raw, raw_len, out, out_cap);
    return (r == 0xFFFFFFFFu) ? -1 : (int)r;
}

uint32_t test_usermode_capture_chunk_max(void);
uint32_t test_usermode_capture_chunk_max(void)
{
    return (uint32_t)UTEST_CAPTURE_CHUNK_MAX;
}

/* The arming postcondition, exported as the pure predicate it is: a task
 * slot and a pid in, one verdict out. A live spawn only ever exercises the
 * armed branch, so this is the only way the refusal branches get covered. */
int test_usermode_capture_armed_for(const struct task *child, uint32_t pid);
int test_usermode_capture_armed_for(const struct task *child, uint32_t pid)
{
    return u_capture_armed_for(child, pid);
}

/* The emission-budget state machine, exported as the PURE function it is:
 * four state values in, one verdict out, no task and no globals. A test
 * can therefore drive every transition -- including the exhaustion
 * boundaries, which a live binary would take megabytes of serial to reach
 * -- without touching the run-wide counters a concurrently running suite
 * depends on. The locked wrapper around it (u_capture_claim) is the only
 * thing that needs a live owner. */
int test_usermode_capture_decide(uint32_t owner_seq, uint32_t run_records,
                                 int owner_stopped, int run_over,
                                 int run_sealed);
int test_usermode_capture_decide(uint32_t owner_seq, uint32_t run_records,
                                 int owner_stopped, int run_over,
                                 int run_sealed)
{
    return (int)u_capture_decide(owner_seq, run_records, owner_stopped,
                                 run_over, run_sealed);
}

/* The budget-cut predicate, over two caller-owned scratch structs. Exported
 * because the whole per-write narrowing rests on its comparison DIRECTION: if
 * it ever answered "cut" for a write armed AFTER the stop, every abandoned
 * write in the run would settle as an exempt budget cut and the loss this
 * evidence exists to surface would go silent again. That regression builds
 * green and produces no wrong record -- only a missing refusal -- so a test is
 * the only thing that catches it. */
int test_usermode_capture_was_budget_cut(const struct task *owner,
                                         const struct thread *thr);
int test_usermode_capture_was_budget_cut(const struct task *owner,
                                         const struct thread *thr)
{
    return u_capture_write_was_budget_cut(owner, thr);
}

/* The settle-once claim. Exported so the exactly-once property is checkable
 * directly rather than inferred from the absence of duplicate records. */
int test_usermode_capture_claim_settlement(struct thread *thr);
int test_usermode_capture_claim_settlement(struct thread *thr)
{
    return u_capture_claim_settlement(thr);
}

/* The claim's full state TRANSITION, over caller-owned state. The decision
 * helper above says what should happen; this says what actually changes,
 * which is the half the host's invariants rest on -- the run charge, both
 * latches, and whether a sequence number was consumed at all. */
int test_usermode_capture_apply(uint32_t *owner_seq, uint32_t *run_records,
                                int *owner_stopped, int *run_over,
                                uint32_t *seq_out, uint32_t *charged_out,
                                int run_sealed, uint64_t *admitted);
int test_usermode_capture_apply(uint32_t *owner_seq, uint32_t *run_records,
                                int *owner_stopped, int *run_over,
                                uint32_t *seq_out, uint32_t *charged_out,
                                int run_sealed, uint64_t *admitted)
{
    struct u_capture_state st;
    enum utest_capture_verdict verdict;

    st.owner_seq = *owner_seq;
    st.run_records = *run_records;
    st.owner_stopped = (uint8_t)(*owner_stopped ? 1 : 0);
    st.run_over = (uint8_t)(*run_over ? 1 : 0);
    st.run_sealed = (uint8_t)(run_sealed ? 1 : 0);
    st.admitted = admitted ? *admitted : 0u;

    verdict = u_capture_apply(&st, seq_out, charged_out);

    *owner_seq = st.owner_seq;
    *run_records = st.run_records;
    *owner_stopped = (int)st.owner_stopped;
    *run_over = (int)st.run_over;
    if (admitted)
        *admitted = st.admitted;
    return (int)verdict;
}

/* The drain's arithmetic, exported as the pure saturating function it is.
 *
 * This is the half of the run-boundary fence a live run can never be made to
 * exercise: reaching the interesting states needs a completion that arrives
 * after its own reservation was written off, which requires an emitter
 * preempted across a frame rollover -- a schedule the kernel test surface
 * cannot construct. Over plain numbers, every one of them is one call. */
uint64_t test_usermode_capture_pending(uint64_t admitted, uint64_t completed,
                                       uint64_t forgiven);
uint64_t test_usermode_capture_pending(uint64_t admitted, uint64_t completed,
                                       uint64_t forgiven)
{
    return u_capture_pending(admitted, completed, forgiven);
}

/* The settlement transition, exported as the pure decision it is: a claim's
 * generation, the run's current generation, and the counter it would credit.
 *
 * This is the guard that stops a written-off claim settling a second time,
 * and a test that only re-reads an unchanged pending value does NOT pin it
 * -- deleting the guard leaves such a test green. Driving the transition
 * itself is the only way to assert that a stale claim is refused and a
 * current one is not. */
int test_usermode_capture_settle(uint32_t claim_gen, uint32_t current_gen,
                                 uint64_t *completed);
int test_usermode_capture_settle(uint32_t claim_gen, uint32_t current_gen,
                                 uint64_t *completed)
{
    return u_capture_settle(claim_gen, current_gen, completed);
}

/* The delivery receipt's transition, exported as the pure decision it is: the
 * claim's generation, the run's, whether klog put the record on the wire, and
 * the two counters it may move.
 *
 * This is the seam that makes section 57 checkable. The behavior it pins --
 * a record already on the wire settles WITHOUT the emitter running again --
 * cannot be observed from the live path, because observing it requires the
 * emitter to not run again, and a test that lets it run proves the opposite
 * of what it set out to.
 *
 * Returns the live-epoch verdict, which is what the caller of the real
 * receipt uses to decide whether the frame census may move. Exporting it
 * rather than only the two counters is what lets a test pin that a STALE
 * delivered record is kept out of the census -- the state that would
 * otherwise bill an innocent successor run. */
int test_usermode_capture_receipt(uint32_t claim_gen, uint32_t current_gen,
                                  int delivered, uint64_t *completed,
                                  uint64_t *undelivered);
int test_usermode_capture_receipt(uint32_t claim_gen, uint32_t current_gen,
                                  int delivered, uint64_t *completed,
                                  uint64_t *undelivered)
{
    return u_capture_receipt_apply(claim_gen, current_gen, delivered,
                                   completed, undelivered);
}

/* The LIVE run accounting, read under its own lock.
 *
 * Read-only, and it exists so a test can bind the production emission path to
 * the receipt rather than only exercising the pure helper above: a test that
 * drives a real capture write and watches THESE counters move is one that
 * fails if the emitting call sites stop carrying u_capture_receipt, which no
 * amount of testing the helper in isolation can catch. */
void test_usermode_capture_live_counters(uint64_t *admitted,
                                         uint64_t *completed,
                                         uint64_t *undelivered);
void test_usermode_capture_live_counters(uint64_t *admitted,
                                         uint64_t *completed,
                                         uint64_t *undelivered)
{
    uint64_t irq_flags;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
    if (admitted)    *admitted    = s_capture_admitted;
    if (completed)   *completed   = s_capture_completed;
    if (undelivered) *undelivered = s_capture_undelivered;
    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
}

/* The run boundary's CLOSE transition, over caller-owned accounting.
 *
 * Exported separately from the pending arithmetic because the two answer
 * different questions, and only the second one is the fix for the
 * double-settlement defect: pending says how much is outstanding, close says
 * that the write-off and the epoch's end are ONE act. A regression that
 * forgave without advancing the generation would leave every pending
 * assertion green while letting a written-off claim settle again. */
uint64_t test_usermode_capture_close(uint64_t *admitted, uint64_t *completed,
                                     uint64_t *forgiven, uint32_t *generation);
uint64_t test_usermode_capture_close(uint64_t *admitted, uint64_t *completed,
                                     uint64_t *forgiven, uint32_t *generation)
{
    struct u_capture_boundary b;
    uint64_t pending;

    b.admitted   = *admitted;
    b.completed  = *completed;
    b.forgiven   = *forgiven;
    b.generation = *generation;

    pending = u_capture_close(&b);

    *forgiven   = b.forgiven;
    *generation = b.generation;
    return pending;
}

/* The reap's selection predicate, exported for the same reason the arming
 * postcondition is: a live run only ever hands it healthy input, so the
 * owner-exclusion and wrong-owner branches would otherwise never execute --
 * and those two branches are the difference between reaping a binary's
 * descendants and reaping the task the launcher is still working with. */
int test_usermode_capture_descendant_of(const struct task *t,
                                        uint32_t owner_pid);
int test_usermode_capture_descendant_of(const struct task *t,
                                        uint32_t owner_pid)
{
    return u_capture_descendant_of(t, owner_pid);
}

/* The reap LOOP, exported for the reason its selection predicate was -- only
 * more so. The predicate at least ran on every live reap; the loop's
 * interesting states (a descendant appearing inside the grace window, a fork
 * still in flight at the latch, a producer that outlasts the round cap) are
 * ones a healthy run never reaches, so without this shim they were verified
 * by inspection alone. Every call goes through the caller's ops, so nothing
 * here can touch the real task table. */
void test_usermode_capture_reap_tree_ops(uint32_t owner_pid,
                                         const struct utest_reap_ops *ops,
                                         struct utest_reap_result *out)
{
    u_capture_reap_tree_ops(owner_pid, ops, out);
}

/* Test-only snapshot/install of the RUN-wide capture accounting, taken
 * under the same lock the emitter uses.
 *
 * A regression that drives the real emitter necessarily charges these
 * globals, and one that seeded only the OWNER's sequence would make the
 * terminator report a `charged` value the real path can never produce --
 * proving nothing about whether that marker would satisfy host
 * reconciliation. Both are the same problem: run accounting is state, so a
 * probe has to seed it coherently and put back what it found.
 *
 * s_frame_records is deliberately NOT exposed here. It belongs to the
 * framing layer, is reset by u_frame_begin before any run is published, and
 * a capture-budget test reaching into it would cross a boundary this
 * mechanism does not own. */
/* The SEAL is part of this state, not a separate concern. It is set at
 * u_frame_end and cleared only by the NEXT u_frame_begin, so after the last
 * framed run it stays set for the rest of the boot -- and a probe that drives
 * the real emitter without restoring it would be silently refused. That the
 * live-emitter tests pass today is an accident of ordering (boot_tests runs
 * the kernel suite before test_usermode_run), which is precisely the kind of
 * undeclared dependency this save/restore pair exists to remove. */
void test_usermode_capture_run_state_get(uint32_t *records, int *over,
                                         int *sealed);
void test_usermode_capture_run_state_get(uint32_t *records, int *over,
                                         int *sealed)
{
    uint64_t irq_flags;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
    *records = s_capture_run_records;
    *over = (int)s_capture_run_over;
    if (sealed)
        *sealed = (int)s_capture_sealed;
    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
}

void test_usermode_capture_run_state_set(uint32_t records, int over,
                                         int sealed);
void test_usermode_capture_run_state_set(uint32_t records, int over,
                                         int sealed)
{
    uint64_t irq_flags;

    spin_lock_irqsave(&s_capture_budget_lock, &irq_flags);
    s_capture_run_records = records;
    s_capture_run_over = (uint8_t)(over ? 1 : 0);
    s_capture_sealed = (uint8_t)(sealed ? 1 : 0);
    spin_unlock_irqrestore(&s_capture_budget_lock, irq_flags);
}

uint32_t test_usermode_capture_owner_budget(void);
uint32_t test_usermode_capture_owner_budget(void)
{
    return (uint32_t)UTEST_CAPTURE_OWNER_RECORD_BUDGET;
}

uint32_t test_usermode_capture_run_budget(void);
uint32_t test_usermode_capture_run_budget(void)
{
    return (uint32_t)UTEST_CAPTURE_RUN_RECORD_BUDGET;
}

/* The derived per-record wire cost and the two allowances it is proven
 * against. Exported so a test can re-derive the worst-case-fits
 * relationship at RUNTIME rather than only trusting the _Static_asserts:
 * the asserts catch a broken derivation at build time, the test states
 * what the relationship IS, so a future retune that satisfies the
 * arithmetic while destroying the intent still has something to fail. */
uint32_t test_usermode_capture_wire_max(void);
uint32_t test_usermode_capture_wire_max(void)
{
    return (uint32_t)UTEST_CAPTURE_WIRE_MAX;
}

uint32_t test_usermode_capture_owner_wire_allowance(void);
uint32_t test_usermode_capture_owner_wire_allowance(void)
{
    return (uint32_t)UTEST_CAPTURE_OWNER_WIRE_MAX;
}

uint32_t test_usermode_capture_run_wire_allowance(void);
uint32_t test_usermode_capture_run_wire_allowance(void)
{
    return (uint32_t)UTEST_CAPTURE_RUN_WIRE_MAX;
}

/* The raw allowance each record budget was derived FROM. A test can then
 * assert the property the derivation exists to guarantee -- that a binary
 * filling every chunk can spend its whole raw allowance without being
 * clipped -- rather than re-stating the arithmetic that produced it. */
uint32_t test_usermode_capture_owner_raw_max(void);
uint32_t test_usermode_capture_owner_raw_max(void)
{
    return (uint32_t)UTEST_CAPTURE_OWNER_RAW_MAX;
}

uint32_t test_usermode_capture_run_raw_max(void);
uint32_t test_usermode_capture_run_raw_max(void)
{
    return (uint32_t)UTEST_CAPTURE_RUN_RAW_MAX;
}

int test_usermode_derive_test_name(const char *name_in,
                                   char *out, uint32_t out_cap)
{
    return u_derive_test_name(name_in, out, out_cap);
}

int test_usermode_path_join(const char *parent, const char *name,
                            char *out, uint32_t out_cap)
{
    return u_path_join(parent, name, out, out_cap);
}

int test_usermode_path_has_traversal(const char *p)
{
    return u_path_has_traversal(p);
}

int test_usermode_xml_escape(const char *src, char *dst, uint32_t cap);
int test_usermode_xml_escape(const char *src, char *dst, uint32_t cap)
{
    uint32_t pos = 0;
    if (cap == 0) return 0;
    dst[0] = '\0';
    return u_xml_escape(dst, &pos, cap, src);
}

int test_usermode_json_escape(const char *src, char *dst, uint32_t cap);
int test_usermode_json_escape(const char *src, char *dst, uint32_t cap)
{
    uint32_t pos = 0;
    if (cap == 0) return 0;
    dst[0] = '\0';
    return u_json_escape(dst, &pos, cap, src);
}

/* Ring-3 self-report helpers. The launcher's spawn path is hard-banned
 * from unit tests (it drives live task_create), so the report machinery
 * is exposed as pure functions instead: the reconciliation matrix and the
 * bounded formatters are the parts that can be wrong, and both are
 * reachable without a child process. */
uint32_t test_usermode_report_reconcile(uint32_t state, uint32_t failed,
                                        int32_t exit_status, int timed_out);
uint32_t test_usermode_report_reconcile(uint32_t state, uint32_t failed,
                                        int32_t exit_status, int timed_out)
{
    return u_report_reconcile(state, failed, exit_status, timed_out);
}

int test_usermode_report_apply_invalid(int verdict, uint32_t *counters);
int test_usermode_report_apply_invalid(int verdict, uint32_t *counters)
{
    return u_report_apply_invalid(verdict, counters);
}

uint32_t test_usermode_skip_records_allowed(uint32_t already, uint32_t want);
uint32_t test_usermode_skip_records_allowed(uint32_t already, uint32_t want)
{
    return u_skip_records_allowed(already, want);
}

uint32_t test_usermode_skip_record_budget(void);
uint32_t test_usermode_skip_record_budget(void)
{
    return UTEST_SKIP_RECORD_BUDGET;
}

int test_usermode_build_skip_record_name(char *dst, uint32_t cap,
                                         const char *base, uint32_t k);
int test_usermode_build_skip_record_name(char *dst, uint32_t cap,
                                         const char *base, uint32_t k)
{
    if (cap == 0) return 0;
    return u_build_skip_record_name(dst, cap, base, k);
}

/* The two overflow fallbacks. Exposed because they are otherwise
 * unreachable from a test: their emitters run only when a real record did
 * not fit, and driving that through klog is exactly what the test policy
 * forbids. A fallback that demoted a verdict would then stay invisible
 * until it refused a real run for a mismatch it caused itself. */
int test_usermode_format_xml_overflow(char *dst, uint32_t cap, int verdict,
                                      const char *classname);
int test_usermode_format_xml_overflow(char *dst, uint32_t cap, int verdict,
                                      const char *classname)
{
    return u_format_xml_overflow(dst, cap, verdict, classname);
}

int test_usermode_format_json_overflow(char *dst, uint32_t cap, int verdict);
int test_usermode_format_json_overflow(char *dst, uint32_t cap, int verdict)
{
    return u_format_json_overflow(dst, cap, verdict);
}

/* The evidence field copy, exposed so a fixture can prove every field
 * lands where it belongs. Takes and returns the five flags positionally
 * rather than the internal struct, so the test file needs no view of
 * u_loader_evidence and a reordered struct cannot silently re-map them. */
void test_usermode_loader_evidence_from_task(const struct task *t,
                                             uint32_t *stage_fault,
                                             uint32_t *reached_exec,
                                             uint32_t *identity_mismatch,
                                             uint32_t *frame_adopted,
                                             uint32_t *entered_user);
void test_usermode_loader_evidence_from_task(const struct task *t,
                                             uint32_t *stage_fault,
                                             uint32_t *reached_exec,
                                             uint32_t *identity_mismatch,
                                             uint32_t *frame_adopted,
                                             uint32_t *entered_user)
{
    struct u_loader_evidence ev;

    u_loader_evidence_from_task(t, &ev);
    *stage_fault       = ev.stage_fault;
    *reached_exec      = ev.reached_exec;
    *identity_mismatch = ev.identity_mismatch;
    *frame_adopted     = ev.frame_adopted;
    *entered_user      = ev.entered_user;
}

/* The never-ran classification rule. Pure, so the artifacts' half of the
 * verdict can be asserted without driving a loader failure through live
 * infrastructure. */
uint32_t test_usermode_record_verdict(int verdict, int loader_stage_fault,
                                      int timed_out, int reached_exec,
                                      int frame_adopted, int entered_user);
uint32_t test_usermode_record_verdict(int verdict, int loader_stage_fault,
                                      int timed_out, int reached_exec,
                                      int frame_adopted, int entered_user)
{
    return (uint32_t)u_record_verdict(verdict, loader_stage_fault, timed_out,
                                      reached_exec, frame_adopted,
                                      entered_user);
}

int test_usermode_format_xml_summary(char *dst, uint32_t cap, uint32_t tests,
                                     uint32_t failures, uint32_t skipped,
                                     uint64_t total_ms, int aborted,
                                     uint32_t not_run, uint32_t errors);
int test_usermode_format_xml_summary(char *dst, uint32_t cap, uint32_t tests,
                                     uint32_t failures, uint32_t skipped,
                                     uint64_t total_ms, int aborted,
                                     uint32_t not_run, uint32_t errors)
{
    if (cap == 0) return 0;
    return u_format_xml_summary(dst, cap, tests, failures, skipped, total_ms,
                                aborted, not_run, errors);
}

int test_usermode_format_json_summary(char *dst, uint32_t cap, uint32_t passed,
                                      uint32_t failed, uint32_t skipped,
                                      uint64_t total_ms, uint32_t errors);
int test_usermode_format_json_summary(char *dst, uint32_t cap, uint32_t passed,
                                      uint32_t failed, uint32_t skipped,
                                      uint64_t total_ms, uint32_t errors)
{
    if (cap == 0) return 0;
    return u_format_json_summary(dst, cap, passed, failed, skipped, total_ms,
                                 errors);
}

/* The report totals are passed as a flat argument list rather than the
 * struct so the test file does not need the file-local type. */
int test_usermode_format_json_run_report(char *dst, uint32_t cap,
                                         uint32_t a_pass, uint32_t a_fail,
                                         uint32_t blocks, uint32_t records,
                                         uint32_t reported, uint32_t invalid,
                                         uint32_t unreported);
int test_usermode_format_json_run_report(char *dst, uint32_t cap,
                                         uint32_t a_pass, uint32_t a_fail,
                                         uint32_t blocks, uint32_t records,
                                         uint32_t reported, uint32_t invalid,
                                         uint32_t unreported)
{
    struct u_report_totals rt;
    if (cap == 0) return 0;
    rt.asserts_passed = a_pass;
    rt.asserts_failed = a_fail;
    rt.skip_blocks    = blocks;
    rt.skip_records   = records;
    rt.reported       = reported;
    rt.invalid        = invalid;
    rt.unreported     = unreported;
    return u_format_json_run_report(dst, cap, &rt);
}

/* The completeness record travels separately from the summary because it
 * must fit klog's message field; the emitter's real cap is exported too so
 * a test can prove the record fits the transport rather than a number the
 * test picked. */
uint32_t test_usermode_json_line_max(void);
uint32_t test_usermode_json_line_max(void)
{
    return UTEST_RECORD_LINE_MAX;
}

int test_usermode_format_json_run_meta(char *dst, uint32_t cap, int aborted,
                                       uint32_t not_run);
int test_usermode_format_json_run_meta(char *dst, uint32_t cap, int aborted,
                                       uint32_t not_run)
{
    if (cap == 0) return 0;
    return u_format_json_run_meta(dst, cap, aborted, not_run);
}

int test_usermode_format_tap_point(char *dst, uint32_t cap, int ok,
                                   uint32_t point, const char *name,
                                   const char *directive);
int test_usermode_format_tap_point(char *dst, uint32_t cap, int ok,
                                   uint32_t point, const char *name,
                                   const char *directive)
{
    return u_format_tap_point(dst, cap, ok, point, name, directive);
}

int test_usermode_format_report_summary(char *dst, uint32_t cap,
                                        uint32_t a_pass, uint32_t a_fail,
                                        uint32_t blocks, uint32_t records,
                                        uint32_t reported, uint32_t invalid,
                                        uint32_t unreported);
int test_usermode_format_report_summary(char *dst, uint32_t cap,
                                        uint32_t a_pass, uint32_t a_fail,
                                        uint32_t blocks, uint32_t records,
                                        uint32_t reported, uint32_t invalid,
                                        uint32_t unreported)
{
    struct u_report_totals rt;
    if (cap == 0) return 0;
    rt.asserts_passed = a_pass;
    rt.asserts_failed = a_fail;
    rt.skip_blocks    = blocks;
    rt.skip_records   = records;
    rt.reported       = reported;
    rt.invalid        = invalid;
    rt.unreported     = unreported;
    return u_format_report_summary(dst, cap, &rt);
}

/* taxonomy helpers -- return integer for ABI-stable test binding. */
int test_usermode_type_for_name(const char *name);
int test_usermode_type_for_name(const char *name)
{
    return (int)u_type_for_name(name);
}

int test_usermode_type_from_attr(const char *val);
int test_usermode_type_from_attr(const char *val)
{
    return (int)u_type_from_attr(val);
}

const char *test_usermode_type_label(int type);
const char *test_usermode_type_label(int type)
{
    return u_type_label((utest_type_t)type);
}
#endif
