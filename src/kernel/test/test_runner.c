/* ============================================================================
 * test_runner.c -- Kernel unit test runner
 *
 * Runs all registered test suites and prints pass/fail summary to serial.
 * Supports category filtering (test_suite= in boot.conf) and quiet mode
 * (test_quiet=1 suppresses PASS lines, only shows FAIL + summary).
 *
 * Only compiled when KERNEL_TESTS is defined (-DKERNEL_TESTS in CFLAGS).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/klog.h"
#include "kernel/timer.h"
#include "kernel/mm/heap.h"             /* kmalloc_fail_* (hygiene) */
#include "kernel/mm/pmm.h"              /* pmm_alloc_fail_* (hygiene) */
#include "kernel/mm/vmm.h"              /* vmm_map_fail_* (hygiene) */
#include "kernel/cpu_security.h"        /* copy_user_fail_* (hygiene) */
#include "kernel/sched/irql.h"          /* KeGetCurrentIrql / KeLowerIrql for action drain */
#include "kernel/quota/quota.h"         /* quota_leak_snapshot (per-category sweep) */

/* ---- Category names (must match test_category_t order) ---- */

static const char *cat_names[] = {
    [TEST_CAT_MM]       = "mm",
    [TEST_CAT_FS]       = "fs",
    [TEST_CAT_SCHED]    = "sched",
    [TEST_CAT_OB]       = "ob",
    [TEST_CAT_SECURITY] = "security",
    [TEST_CAT_IPC]      = "ipc",
    [TEST_CAT_BOOT]     = "boot",
    [TEST_CAT_ABI]      = "abi",
    [TEST_CAT_STORAGE]  = "storage",
    [TEST_CAT_EXEC]     = "exec",
    [TEST_CAT_X86]      = "x86",
    [TEST_CAT_DESKTOP]  = "desktop",
    [TEST_CAT_EX]       = "ex",
    [TEST_CAT_NLS]      = "nls",
    [TEST_CAT_KNF]      = "knf",
    [TEST_CAT_EXCEPT]   = "except",
    [TEST_CAT_QUOTA]    = "quota",
};

static const char *cat_labels[] = {
    [TEST_CAT_MM]       = "Memory Management",
    [TEST_CAT_FS]       = "Filesystem",
    [TEST_CAT_SCHED]    = "Scheduler",
    [TEST_CAT_OB]       = "Object Manager",
    [TEST_CAT_SECURITY] = "Security",
    [TEST_CAT_IPC]      = "IPC",
    [TEST_CAT_BOOT]     = "Boot & Logging",
    [TEST_CAT_ABI]      = "ABI Compatibility",
    [TEST_CAT_STORAGE]  = "Storage Drivers",
    [TEST_CAT_EXEC]     = "Binary System",
    [TEST_CAT_X86]      = "x86-64 Architecture",
    [TEST_CAT_DESKTOP]  = "Desktop UI",
    [TEST_CAT_EX]       = "Executive Support",
    [TEST_CAT_NLS]      = "Atom/NLS/Locale",
    [TEST_CAT_KNF]      = "Notification Facility",
    [TEST_CAT_EXCEPT]   = "Exception Dispatch",
    [TEST_CAT_QUOTA]    = "Resource Quotas",
};

/* Layer 1 -- both tables are indexed by test_category_t. A new enum entry
 * without a matching row here would otherwise read a NULL name at runtime. */
_Static_assert(sizeof(cat_names) / sizeof(cat_names[0]) == TEST_CAT_COUNT,
               "cat_names must have one entry per test_category_t");
_Static_assert(sizeof(cat_labels) / sizeof(cat_labels[0]) == TEST_CAT_COUNT,
               "cat_labels must have one entry per test_category_t");

/* ---- Global test state ---- */

test_state_t g_test_state = {
    .passed        = 0,
    .failed        = 0,
    .skipped       = 0,
    .pending       = 0,
    .leaked        = 0,
    .quota_leaked  = 0,
    .suite_count   = 0,
    .suites_passed = 0,
    .suites_failed = 0,
    .current_suite = "unknown",
};

/* Per-test heap-leak detection state. File-scope static (not in the
 * public test_state_t) so the ABI stays minimal. Reset before each
 * suite body; read after the action drain completes. */
static uint64_t    s_heap_used_at_entry;
static int64_t     s_expected_leak_bytes;
static int         s_leak_ignore;
static const char *s_leak_reason;

/* Klog subsystem tag for the currently-running suite. "TEST" by default,
 * flipped to "DTEST" while a TEST_CAT_DESKTOP suite is active so the
 * klog.c test-runner coloring path recolors those lines pink-mauve
 * (#C586B5). The comment block at klog.c:976 documents the three test
 * layers (TEST kernel, UTEST user-mode, DTEST desktop). */
static const char *s_current_tag = "TEST";
static const char *test_tag(void) { return s_current_tag; }
static int64_t     s_last_leak_delta;    /* diagnostic for harness tests */

void _test_expect_leak(int64_t bytes, const char *reason)
{
    /* Clamp non-positive at the API boundary: a stale
     * TEST_EXPECT_LEAK(-1, ...) or TEST_EXPECT_LEAK(0, ...) must NOT
     * arm the opt-out. The classifier double-checks bytes > 0 before
     * accepting a [LEAK-OK]. */
    s_expected_leak_bytes = (bytes > 0) ? bytes : 0;
    s_leak_reason         = reason;
}

void _test_leak_ignore(const char *reason)
{
    s_leak_ignore = 1;
    s_leak_reason = reason;
}

int64_t test_runner_last_leak_delta(void)
{
    return s_last_leak_delta;
}

static test_category_t g_filter = TEST_CAT_ALL;
static int             g_quiet  = 0;

/* [COUNT] trace switch -- OFF unless boot.conf asked for it
 * (test_quiet=TEST_QUIET_COUNT_TRACE). Opt-in rather than tied to verbosity;
 * the reasoning is at TEST_QUIET_* in include/kernel/test/test.h. */
static int             g_count_trace = 0;

/* ---- Per-suite assertion accounting: the [COUNT] trace ----
 *
 * The runner's headline "=== N tests passed" is what every reader treats as
 * the regression signal, and it has been observed MOVING on an unchanged tree
 * (a 6-assertion swing, zero failures either way). The delta was
 * unattributable: the suite loop computed each suite's pass count and threw it
 * away on the next line, so the only artifact of a drifting total was the
 * total. These records make each suite's contribution a readable fact, so two
 * runs diff to the SUITE that moved instead of to a number nobody can place.
 *
 * Three properties are load-bearing; none are decoration.
 *
 *  - LOSSLESS. Emitted through klog_unrated(), never plain klog(). The shared
 *    per-subsystem limiter drops above KLOG_RATE_DEFAULT (100) messages per
 *    KLOG_RATE_WINDOW_MS (1000 ms) window, and a dropped record is INVISIBLE
 *    to a consumer -- a host-side comparison would then compare the same
 *    truncated subset across runs and report "identical" precisely when the
 *    varying suite's record was the one discarded. The TEST tag escapes the
 *    limiter today only by accident: klog.c sizes the table at 32 rate slots,
 *    the boot path burns all of them before the first TEST message, and
 *    rate_slot() returns NULL so rate_check() is never consulted. That is a
 *    property of tag ordering, not a contract, and it is the kind of thing a
 *    later subsystem rename revokes silently.
 *
 *  - COMPLETENESS-CHECKABLE. Every record carries a monotonic ordinal from 1,
 *    and the trailer announces how many were emitted alongside the run totals.
 *    A consumer seeing a gap, a duplicate, or a trailer that disagrees with
 *    what it counted knows its input is truncated rather than stable, which is
 *    the difference between "the totals match" and "I could not tell".
 *
 *  - OFF BY DEFAULT, on its own switch. Enabled only by boot.conf
 *    test_quiet=TEST_QUIET_COUNT_TRACE (2), never implied by verbosity. This
 *    is not caution, it is two measurements. Emitting unconditionally on the
 *    QUIET path -- the one every automated gate runs -- took the suite from a
 *    3.8-4.6s baseline to 10.7s, 2704 extra records of real serial I/O pushed
 *    into the very timing environment whose stability is under measurement.
 *    Those are EMULATED-serial seconds and the bare-metal cost is worse, not
 *    better: serial_write() holds the port lock with interrupts DISABLED while
 *    polling the UART for each line, so on a real 115200 port a record is
 *    milliseconds of interrupts-off. That is inherent to klog rather than new
 *    here (verbose mode is an order of magnitude more of it), which is exactly
 *    why this rides an explicit opt-in instead of any default path.
 *    Riding VERBOSE instead was worse: a verbose run emits ~28k per-assertion
 *    PASS lines and does not finish inside scripts/test.sh's 60s window at all
 *    (measured still mid-run at suite 2188 after 54.2s), so the only mode that
 *    could attribute a drift would have been the one mode that cannot
 *    complete. Quiet-plus-trace keeps both properties: no PASS-line flood, and
 *    every suite's contribution on the wire.
 *
 *    Enabling it does cost ring history: klog's ring holds 1000 entries, and a
 *    traced run reports ~5275 early entries lost against ~2573 for the same
 *    untraced quiet suite. The ring already overflows by 2.5x without the
 *    trace, the loss is confined to a diagnostic mode nothing gates on, and
 *    the alternative -- a second sink with deferred export -- buys back log
 *    history the trace run is not there to read. Measured, not overlooked.
 *
 * The record body is built by a PURE formatter rather than handed to klog's
 * varargs directly, for the same reason test_usermode.c builds its wire
 * records by hand: klog's ring entry is `message[256]` and a longer line is
 * cut SILENTLY, which for a completeness-checked format turns a load-bearing
 * field into a parse failure with no fault. Building it here bounds the width
 * at the source, makes truncation an explicit marked outcome instead of an
 * invisible one, and leaves every branch reachable from a unit test with
 * synthetic inputs (the seam quota_sweep_classify uses for the same reason). */

/* Append `src` to `dst[*pos]`, bounded by `cap`. Returns 1 on success, 0 if
 * `src` would overflow. Leaves dst NUL-terminated either way. */
static int tc_append(char *dst, uint32_t *pos, uint32_t cap, const char *src)
{
    uint32_t p = *pos;
    while (*src) {
        if (p + 1 >= cap) { dst[p] = '\0'; *pos = p; return 0; }
        dst[p++] = *src++;
    }
    dst[p] = '\0';
    *pos = p;
    return 1;
}

/* Append `v` in decimal. Same contract as tc_append(). */
static int tc_append_u32(char *dst, uint32_t *pos, uint32_t cap, uint32_t v)
{
    char     tmp[11];               /* 4294967295 + NUL */
    uint32_t n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        char rev[10];
        uint32_t r = 0;
        while (v > 0) { rev[r++] = (char)('0' + (v % 10u)); v /= 10u; }
        while (r > 0) tmp[n++] = rev[--r];
    }
    tmp[n] = '\0';
    return tc_append(dst, pos, cap, tmp);
}

/* Append one `<key>=<value>` field preceded by a space. */
static int tc_append_field(char *dst, uint32_t *pos, uint32_t cap,
                           const char *key, uint32_t v)
{
    if (!tc_append(dst, pos, cap, " ")) return 0;
    if (!tc_append(dst, pos, cap, key)) return 0;
    return tc_append_u32(dst, pos, cap, v);
}

int test_count_record_format(char *dst, uint32_t cap, uint32_t ordinal,
                             const char *cat, const char *suite,
                             uint32_t passed, uint32_t failed,
                             uint32_t skipped, uint32_t pending)
{
    uint32_t pos = 0;
    int      ok  = 1;

    if (!dst || cap == 0) return 0;
    dst[0] = '\0';

    /* The suite NAME is the only unbounded field, and it is also the
     * comparison KEY -- two suites truncated to the same prefix would compare
     * equal and hide exactly the drift this record exists to expose. So a
     * truncated record is marked rather than quietly shortened: the host
     * parser treats `trunc=1` as an unusable key, not as a name. */
    if (!tc_append(dst, &pos, cap, "[COUNT] #"))        ok = 0;
    if (ok && !tc_append_u32(dst, &pos, cap, ordinal))  ok = 0;
    if (ok && !tc_append(dst, &pos, cap, " "))          ok = 0;
    if (ok && !tc_append(dst, &pos, cap, cat ? cat : "?")) ok = 0;
    if (ok && !tc_append(dst, &pos, cap, " "))          ok = 0;
    if (ok && !tc_append(dst, &pos, cap, suite ? suite : "?")) ok = 0;
    if (ok && !tc_append_field(dst, &pos, cap, "p=", passed))  ok = 0;
    if (ok && !tc_append_field(dst, &pos, cap, "f=", failed))  ok = 0;
    if (ok && !tc_append_field(dst, &pos, cap, "s=", skipped)) ok = 0;
    if (ok && !tc_append_field(dst, &pos, cap, "P=", pending)) ok = 0;

    if (!ok) {
        /* Overwrite the tail with the marker. Reserving room for it up front
         * would shorten every record for a case that the 224-byte budget puts
         * out of reach anyway (the longest registered suite name is 85 bytes);
         * rewinding here keeps the common record full-width and still makes
         * the rare one self-describing. */
        uint32_t back = (cap > TEST_COUNT_TRUNC_MARK_LEN)
                            ? cap - 1u - TEST_COUNT_TRUNC_MARK_LEN
                            : 0u;
        if (back < pos) pos = back;
        dst[pos] = '\0';
        (void)tc_append(dst, &pos, cap, TEST_TRUNC_MARK);
        return 0;
    }
    return 1;
}

/* Append `v` in decimal, 64-bit. Same contract as tc_append(). */
static int tc_append_u64(char *dst, uint32_t *pos, uint32_t cap, uint64_t v)
{
    char     tmp[21];               /* 18446744073709551615 + NUL */
    uint32_t n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        char     rev[20];
        uint32_t r = 0;
        while (v > 0) { rev[r++] = (char)('0' + (uint32_t)(v % 10u)); v /= 10u; }
        while (r > 0) tmp[n++] = rev[--r];
    }
    tmp[n] = '\0';
    return tc_append(dst, pos, cap, tmp);
}

/* Append at most `n` characters of `src`, also bounded by `cap`. Used where a
 * field is deliberately given a share of the budget rather than all-or-nothing:
 * the record still carries the marker, so a shortened field is never mistaken
 * for the whole one. */
static void tc_append_n(char *dst, uint32_t *pos, uint32_t cap,
                        const char *src, uint32_t n)
{
    uint32_t p = *pos;

    while (n > 0u && *src) {
        if (p + 1u >= cap) break;
        dst[p++] = *src++;
        n--;
    }
    dst[p] = '\0';
    *pos = p;
}

/* Length of `s`, measured only as far as `limit`. Stopping early is what keeps
 * the budget arithmetic below in range: a full-width measurement of several
 * fields can in principle sum past what a uint32_t holds, and the wrapped total
 * would then compare as "fits" and return success for a record that was in fact
 * cut. Nothing longer than the budget can change any decision here, so there is
 * no reason to walk it. */
static uint32_t tc_len_bounded(const char *s, uint32_t limit)
{
    uint32_t n = 0;

    if (!s) return 0;
    while (n < limit && s[n]) n++;
    return n;
}

/* Last path component. Both separators are handled because __FILE__ carries
 * whatever the build used, and a Windows-style path must not defeat the
 * shortening ladder below by looking like one long component. */
static const char *tc_basename(const char *path)
{
    const char *base = path;
    const char *p;

    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

/* Build "  (<file>:<line>)", shedding path components until it fits both `cap`
 * and `budget`. Returns 1 when some form fit, 0 when even ":<line>" did not.
 *
 * The suffix is built BEFORE any author text is spent because it is the half
 * of a failure record that makes the failure locatable: a left-to-right
 * bounded append would cut precisely this and leave a loud, unlocatable
 * message. */
static int fail_suffix_build(char *dst, uint32_t cap, const char *file,
                             int line, uint32_t budget, int *degraded)
{
    const char *cand[3];
    uint32_t    i;
    /* Keep room beside the suffix for the marker, so a pathological path
     * cannot consume the record and erase the evidence that anything was
     * cut. */
    uint32_t    room = (budget > TEST_COUNT_TRUNC_MARK_LEN)
                           ? budget - TEST_COUNT_TRUNC_MARK_LEN
                           : 0u;

    cand[0] = file ? file : "";
    cand[1] = file ? tc_basename(file) : "";
    cand[2] = "";
    *degraded = 0;

    for (i = 0; i < 3u; i++) {
        uint32_t pos = 0;
        uint64_t mag = (line < 0) ? (uint64_t)(-(int64_t)line)
                                  : (uint64_t)(int64_t)line;

        dst[0] = '\0';
        if (!tc_append(dst, &pos, cap, "  ("))        continue;
        if (!tc_append(dst, &pos, cap, cand[i]))      continue;
        if (!tc_append(dst, &pos, cap, ":"))          continue;
        if (line < 0 && !tc_append(dst, &pos, cap, "-")) continue;
        if (!tc_append_u64(dst, &pos, cap, mag))      continue;
        if (!tc_append(dst, &pos, cap, ")"))          continue;
        if (pos <= room) {
            /* A shed path is a CUT, and the record must say so. Without this,
             * `(module_name.c:900)` is indistinguishable from a record whose
             * file really was named that, and a reader trusts a path the
             * formatter invented by subtraction. */
            *degraded = (i != 0u);
            return 1;
        }
    }
    dst[0] = '\0';
    *degraded = 1;
    return 0;
}

int test_fail_detail_format(char *dst, uint32_t cap, const char *lead,
                            uint64_t a, const char *mid, uint64_t b)
{
    uint32_t pos = 0;

    if (!dst || cap == 0) return 0;
    dst[0] = '\0';

    /* All-or-nothing: a half-written diagnostic ("got 12, expec") reads as a
     * real one, and the caller has no way to tell. Empty is honest. */
    if (!tc_append(dst, &pos, cap, "  ("))                  goto fail;
    if (!tc_append(dst, &pos, cap, lead ? lead : ""))       goto fail;
    if (!tc_append_u64(dst, &pos, cap, a))                  goto fail;
    if (mid) {
        if (!tc_append(dst, &pos, cap, mid))                goto fail;
        if (!tc_append_u64(dst, &pos, cap, b))              goto fail;
    }
    if (!tc_append(dst, &pos, cap, ")"))                    goto fail;
    return 1;

fail:
    dst[0] = '\0';
    return 0;
}

int test_fail_record_format(char *dst, uint32_t cap, const char *suite,
                            const char *msg, const char *detail,
                            const char *file, int line)
{
    char     suffix[128];
    uint32_t budget, suffix_len, detail_len, suite_len, msg_len;
    uint32_t pos = 0;
    int      suffix_degraded = 0;

    if (!dst || cap < 2u) {
        if (dst && cap) dst[0] = '\0';
        return 0;
    }
    dst[0] = '\0';

    budget = cap - 1u;                  /* usable characters, NUL excluded */
    if (!suite)  suite  = "?";
    if (!msg)    msg    = "?";
    if (!detail) detail = "";

    if (!fail_suffix_build(suffix, sizeof(suffix), file, line, budget,
                           &suffix_degraded))
        suffix[0] = '\0';

    /* Measured against the budget, never full width: see tc_len_bounded. A
     * field reported as budget+1 is "too long" for every decision below, which
     * is all the arithmetic needs to know. */
    suffix_len = tc_len_bounded(suffix, budget + 1u);
    detail_len = tc_len_bounded(detail, budget + 1u);
    suite_len  = tc_len_bounded(suite,  budget + 1u);
    msg_len    = tc_len_bounded(msg,    budget + 1u);

    if (!suffix_degraded && suite_len <= TEST_FAIL_SUITE_MAX &&
        suite_len + 4u + msg_len + detail_len + suffix_len <= budget) {
        int ok = 1;

        /* The arithmetic above already proved this fits, so a failing append
         * would mean the two disagree. Returning what actually happened rather
         * than what was predicted keeps the contract honest either way. */
        if (!tc_append(dst, &pos, cap, suite))    ok = 0;
        if (!tc_append(dst, &pos, cap, " :: "))   ok = 0;
        if (!tc_append(dst, &pos, cap, msg))      ok = 0;
        if (!tc_append(dst, &pos, cap, detail))   ok = 0;
        if (!tc_append(dst, &pos, cap, suffix))   ok = 0;
        if (ok) return 1;
        /* Fall through and re-compose as a marked, truncated record. */
        pos = 0;
        dst[0] = '\0';
    }

    {
        /* Everything except the author text is reserved first, so the cut lands
         * on the message rather than on the suffix or the diagnostic. */
        uint32_t fixed = 4u + detail_len + TEST_COUNT_TRUNC_MARK_LEN + suffix_len;
        uint32_t text  = (budget > fixed) ? budget - fixed : 0u;
        uint32_t s_use = (suite_len < TEST_FAIL_SUITE_MAX)
                             ? suite_len : TEST_FAIL_SUITE_MAX;

        if (s_use > text) s_use = text;

        if (text == 0u) {
            /* Degenerate budget: keep the two fields a consumer can still act
             * on -- the marker that says this is not a whole record, and the
             * location. The marker is the FULL shared token including its
             * leading space; emitting a bare "trunc=1" here would save one
             * byte and make the record unrecognisable to the consumer the
             * token exists for. */
            (void)tc_append(dst, &pos, cap, TEST_TRUNC_MARK);
            (void)tc_append(dst, &pos, cap, suffix);
            return 0;
        }

        tc_append_n(dst, &pos, cap, suite, s_use);
        (void)tc_append(dst, &pos, cap, " :: ");
        tc_append_n(dst, &pos, cap, msg, text - s_use);
        (void)tc_append(dst, &pos, cap, detail);
        (void)tc_append(dst, &pos, cap, TEST_TRUNC_MARK);
        (void)tc_append(dst, &pos, cap, suffix);
    }
    return 0;
}

int test_count_trailer_format(char *dst, uint32_t cap, uint32_t records,
                              uint32_t passed, uint32_t failed,
                              uint32_t skipped, uint32_t pending)
{
    uint32_t pos = 0;

    if (!dst || cap == 0) return 0;
    dst[0] = '\0';

    if (!tc_append(dst, &pos, cap, "[COUNT-END]"))                 return 0;
    if (!tc_append_field(dst, &pos, cap, "records=", records))     return 0;
    if (!tc_append_field(dst, &pos, cap, "p=", passed))            return 0;
    if (!tc_append_field(dst, &pos, cap, "f=", failed))            return 0;
    if (!tc_append_field(dst, &pos, cap, "s=", skipped))           return 0;
    return tc_append_field(dst, &pos, cap, "P=", pending);
}

/* ---- Category helpers ---- */

const char *test_category_name(test_category_t cat)
{
    if (cat < TEST_CAT_COUNT)
        return cat_names[cat];
    return "all";
}

static int streq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

test_category_t test_category_from_string(const char *str)
{
    if (!str || !str[0])
        return TEST_CAT_ALL;
    for (uint32_t i = 0; i < TEST_CAT_COUNT; i++) {
        if (streq_ci(str, cat_names[i]))
            return (test_category_t)i;
    }
    return TEST_CAT_ALL;
}

/* ---- Registration ---- */

/* Does this suite name survive the [COUNT] record format?
 *
 * The rule is narrower than it first looks, and getting it wrong in the SAFE
 * direction is still a bug. The first version of this rejected any name
 * containing " p=", " f=", " s=" or " P=", on the theory that those are the
 * record's field delimiters. They are -- but the consumer parses the four
 * numeric fields RIGHT-ANCHORED, so a name containing them is unambiguous and
 * parses correctly. Verified against the host regex: a name as adversarial as
 * `evil p=9 f=9 s=9 P=9 tail` still yields the right name and the right four
 * numbers, because only the trailing group can satisfy the anchor. Banning
 * those names would have produced a LOG_ERROR for names that work fine, and a
 * validator that cries wolf is one that gets switched off.
 *
 * What the consumer genuinely cannot survive is ` trunc=1`. That is the marker
 * an over-budget record ends with, and the parser treats its presence anywhere
 * in a [COUNT] line as proof the record was truncated -- so a suite named
 * "Harness: trunc=1 behavior" would make every sweep refuse its own trace, and
 * refuse it for a reason that has nothing to do with the run.
 *
 * Checked on the RESOLVED runtime string rather than by grepping the source: a
 * grep cannot see through adjacent string literals (this tree already splits
 * names that way, e.g. test_quota_ledger.c:2926), macro-built names, or
 * escapes, and a check that misses those reports the invariant holding when it
 * does not.
 *
 * Pure and side-effect free, so the harness can drive every branch. */
int test_count_name_is_safe(const char *name)
{
    static const char reserved[] = TEST_TRUNC_MARK;
    uint32_t i, j;

    if (!name)
        return 0;

    for (i = 0; name[i]; i++) {
        for (j = 0; reserved[j] && name[i + j] == reserved[j]; j++)
            ;
        if (!reserved[j])
            return 0;
    }
    return 1;
}

void test_suite_register_cat(const char *name, test_fn_t fn, test_category_t cat)
{
    if (g_test_state.suite_count >= TEST_MAX_SUITES) {
        klog(LOG_ERROR, "TEST", "Max test suites reached (%u)",
             (uint64_t)TEST_MAX_SUITES);
        return;
    }
    /* Loud at boot rather than silently unparseable later. The suite is still
     * registered and still runs -- refusing it would turn a naming slip into
     * missing coverage -- but anyone reading a [COUNT] trace built from this
     * name has been told it cannot be trusted as a key. */
    if (!test_count_name_is_safe(name))
        klog(LOG_ERROR, "TEST",
             "suite name contains the reserved [COUNT] token ' trunc=1', which "
             "makes every consumer treat its record as truncated: %s",
             name ? name : "(null)");
    test_suite_t *s = &g_test_state.suites[g_test_state.suite_count++];
    s->name = name;
    s->fn   = fn;
    s->cat  = cat;
}

void test_suite_register(const char *name, test_fn_t fn)
{
    test_suite_register_cat(name, fn, TEST_CAT_ALL);
}

/* ---- Filter & quiet ---- */

void test_runner_set_filter(test_category_t cat)
{
    g_filter = cat;
}

void test_runner_set_quiet(int quiet)
{
    g_quiet = quiet;
}

void test_runner_set_count_trace(int on)
{
    g_count_trace = on ? 1 : 0;
}

/* ---- Assert implementations ---- */

/* Run-length collapse for repeated PASSING assertions.
 *
 * A loop-driven test emits one identical line per iteration, and a few of them
 * dominate the whole serial log: the 1000-key AVL suite and the counted-string
 * fuzzer together account for roughly 7600 of a 30244-line run (25%), all of it
 * the same dozen strings. That volume buries the lines an operator actually
 * needs, and on a slow serial link it is also the run's wall-clock cost.
 *
 * The first occurrence still prints IMMEDIATELY -- collapsing must not turn
 * into buffering, or a hang mid-loop would lose the very line that localizes
 * it. Subsequent identical passes are counted and the total is emitted when the
 * run ends, so no information is lost, only repetition.
 *
 * Identity is a POINTER compare, deliberately. The clutter case is one string
 * literal re-passed by a loop, which is always the same address; two distinct
 * literals that happen to read alike stay separate, so this can never merge
 * assertions the author wrote as different checks.
 *
 * FAILURES ARE NEVER COLLAPSED -- every failing assertion prints in full, with
 * its own file:line. Counters are untouched either way, so the summary totals
 * and scripts/test.sh's parse of them are unaffected. */
static const char *s_pass_last_msg;
static const char *s_pass_last_suite;
static uint32_t    s_pass_run;

static void test_pass_run_flush(void)
{
    if (s_pass_run > 1u && !g_quiet)
        klog(LOG_INFO, test_tag(), "%s :: %s  (repeated x%u)",
             s_pass_last_suite ? s_pass_last_suite : "unknown",
             s_pass_last_msg ? s_pass_last_msg : "?",
             (uint64_t)s_pass_run);
    s_pass_last_msg   = (const char *)0;
    s_pass_last_suite = (const char *)0;
    s_pass_run        = 0u;
}

/* Record one passing assertion, printing it unless it repeats the previous
 * one. Callers must have already bumped g_test_state.passed. */
static void test_pass_emit(const char *msg)
{
    const char *suite = g_test_state.current_suite;

    if (s_pass_run > 0u && msg == s_pass_last_msg && suite == s_pass_last_suite) {
        s_pass_run++;
        return;
    }
    test_pass_run_flush();
    s_pass_last_msg   = msg;
    s_pass_last_suite = suite;
    s_pass_run        = 1u;
    if (!g_quiet)
        klog(LOG_INFO, test_tag(), "%s :: %s", suite, msg);
}

void _test_assert(int condition, const char *msg, const char *file, int line)
{
    if (condition) {
        g_test_state.passed++;
        test_pass_emit(msg);
    } else {
        char rec[TEST_FAIL_RECORD_MAX];

        /* Flush first so the collapsed run reads BEFORE the failure that
         * followed it, rather than after. */
        test_pass_run_flush();
        g_test_state.failed++;
        (void)test_fail_record_format(rec, sizeof(rec),
                                      g_test_state.current_suite, msg,
                                      (const char *)0, file, line);
        klog(LOG_ERROR, test_tag(), "%s", rec);
    }
}

void _test_assert_eq(uint64_t a, uint64_t b, const char *msg,
                     const char *file, int line)
{
    if (a == b) {
        g_test_state.passed++;
        test_pass_emit(msg);
    } else {
        char rec[TEST_FAIL_RECORD_MAX];
        char det[64];

        test_pass_run_flush();
        g_test_state.failed++;
        (void)test_fail_detail_format(det, sizeof(det), "got ", a,
                                      ", expected ", b);
        (void)test_fail_record_format(rec, sizeof(rec),
                                      g_test_state.current_suite, msg, det,
                                      file, line);
        klog(LOG_ERROR, test_tag(), "%s", rec);
    }
}

void _test_assert_neq(uint64_t a, uint64_t b, const char *msg,
                      const char *file, int line)
{
    if (a != b) {
        g_test_state.passed++;
        test_pass_emit(msg);
    } else {
        char rec[TEST_FAIL_RECORD_MAX];
        char det[64];

        test_pass_run_flush();
        g_test_state.failed++;
        (void)test_fail_detail_format(det, sizeof(det), "both are ", a,
                                      (const char *)0, 0);
        (void)test_fail_record_format(rec, sizeof(rec),
                                      g_test_state.current_suite, msg, det,
                                      file, line);
        klog(LOG_ERROR, test_tag(), "%s", rec);
    }
}

void _test_skip(const char *msg, const char *file, int line)
{
    (void)file;
    (void)line;
    test_pass_run_flush();
    g_test_state.skipped++;
    if (!g_quiet)
        klog(LOG_WARN, test_tag(), "%s :: SKIP: %s",
             g_test_state.current_suite, msg);
}

/* TEST_PENDING -- contract holds AND feature is intentionally deferred.
 * On hold: log [STUB] line and bump pending counter. On break: log a
 * normal failure (the deferred contract was supposed to hold and did
 * not -- e.g. a slot that was supposed to return the deferred status
 * returned something else, indicating mis-registration).  */
void _test_pending(int condition, const char *msg,
                   const char *file, int line)
{
    /* Flush the collapsed pass-run FIRST, on both branches, for the reason
     * _test_assert states at its own flush: the "(repeated xN)" summary must
     * read BEFORE the line that followed the run, not after it. This emitter
     * was the only one of the five that did not, so a [STUB] or a broken
     * contract landed mid-run and inverted the log order in exactly the
     * collapse case the flush exists to order. */
    test_pass_run_flush();

    if (condition) {
        g_test_state.pending++;
        klog(LOG_WARN, test_tag(), "%s :: [STUB] %s",
             g_test_state.current_suite, msg);
    } else {
        char rec[TEST_FAIL_RECORD_MAX];

        g_test_state.failed++;
        (void)test_fail_record_format(rec, sizeof(rec),
                                      g_test_state.current_suite,
                                      msg, "  (PENDING-CONTRACT BROKEN)",
                                      file, line);
        klog(LOG_ERROR, test_tag(), "%s", rec);
    }
}

/* ---- Per-suite action/cleanup registry ----
 *
 * Each running suite gets a fresh 32-slot LIFO cleanup stack. Actions
 * registered via test_add_action() fire in reverse registration order
 * after the suite body returns, pass or fail. File-scope static keeps
 * this out of the public test_state_t ABI and simplifies reset between
 * suites to a single field assignment.
 *
 * SMP note: the test runner is sequential single-CPU (suites run one at
 * a time on the test-runner thread), so no spinlock is needed on the
 * action stack. If the runner ever fans out, this needs revisiting.
 */
#define TEST_MAX_ACTIONS  32

static struct {
    uint8_t count;
    uint8_t draining;           /* 1 while test_actions_drain() is running */
    struct {
        void (*fn)(void *);
        void *ctx;
    } actions[TEST_MAX_ACTIONS];
} s_test_actions;

int test_add_action(void (*fn)(void *), void *ctx)
{
    if (!fn) {
        /* Warn on the same %s :: ... convention as the other -1 paths
         * so a silent drop during test authoring shows up in the boot
         * log rather than hiding behind a forgotten return-value check. */
        klog(LOG_WARN, test_tag(),
             "%s :: test_add_action NULL fn -- rejected",
             g_test_state.current_suite);
        return -1;
    }
    /* Reject re-entrant registration during drain. Allowing it would let
     * an action re-populate the slot the drain just vacated and spin the
     * drain loop forever. The contract is: register from the suite body,
     * drain when the body returns. Re-registration is a bug in the
     * action callback; surfacing it via -1 + warning lets the test
     * author fix it without hanging the runner. */
    if (s_test_actions.draining) {
        klog(LOG_WARN, test_tag(),
             "%s :: test_add_action called during drain -- rejected",
             g_test_state.current_suite);
        return -1;
    }
    if (s_test_actions.count >= TEST_MAX_ACTIONS) {
        klog(LOG_WARN, test_tag(), "%s :: action list full (%u slots)",
             g_test_state.current_suite, (uint64_t)TEST_MAX_ACTIONS);
        return -1;
    }
    uint8_t idx = s_test_actions.count++;
    s_test_actions.actions[idx].fn  = fn;
    s_test_actions.actions[idx].ctx = ctx;
    return 0;
}

static void test_actions_reset(void)
{
    s_test_actions.count = 0;
    s_test_actions.draining = 0;
}

static void test_actions_drain(void)
{
    /* IRQL hygiene: the drain runs at PASSIVE_LEVEL. A suite that left
     * IRQL elevated (forgotten KeLowerIrql after KeRaiseIrql) would be
     * unsafe to drain from: event_wait / kfree / klog assume PASSIVE.
     * Warn + force-lower so the drain completes deterministically and
     * the subsequent suite starts from a known IRQL. Doing nothing here
     * is worse -- the next suite would inherit the elevated IRQL.
     *
     * Ordering matters: capture the leaked level, KeLowerIrql FIRST,
     * THEN klog. klog reaches into spinlock-protected ring buffers and
     * disk-flush paths that assume PASSIVE; logging before the lower
     * would be the exact unsafe pattern this guard is meant to prevent. */
    KIRQL irql = KeGetCurrentIrql();
    if (irql != PASSIVE_LEVEL) {
        KeLowerIrql(PASSIVE_LEVEL);
        klog(LOG_WARN, test_tag(), "%s :: left IRQL elevated (%u) before drain",
             g_test_state.current_suite, (uint64_t)irql);
    }

    /* Set draining flag so re-entrant test_add_action calls from inside
     * an action callback are rejected instead of re-populating the
     * stack and hanging the loop. See test_add_action() for rationale. */
    s_test_actions.draining = 1;

    /* LIFO drain: last registered runs first. After each callback,
     * re-check IRQL -- a cleanup that raises DISPATCH_LEVEL and
     * returns without lowering would otherwise poison every
     * subsequent action AND the next suite (kfree/klog/event_wait
     * all require PASSIVE). Per-iteration recovery keeps each
     * action's promised runtime context intact. */
    while (s_test_actions.count > 0) {
        uint8_t idx = --s_test_actions.count;
        void (*fn)(void *) = s_test_actions.actions[idx].fn;
        void *ctx = s_test_actions.actions[idx].ctx;
        if (fn)
            fn(ctx);
        /* Same ordering rule as the pre-loop check: lower FIRST, log
         * after, so the klog itself runs at PASSIVE. */
        KIRQL post = KeGetCurrentIrql();
        if (post != PASSIVE_LEVEL) {
            KeLowerIrql(PASSIVE_LEVEL);
            klog(LOG_WARN, test_tag(),
                 "%s :: action leaked IRQL (%u) -- lowering",
                 g_test_state.current_suite, (uint64_t)post);
        }
    }

    s_test_actions.draining = 0;
}

/* ---- Register built-in test suites ---- */

extern void test_register_pmm(void);
extern void test_register_heap(void);
extern void test_register_vmm(void);
extern void test_register_swap(void);
extern void test_register_mmap(void);
extern void test_register_vfs(void);
extern void test_register_sched(void);
extern void test_register_proc_ext(void);
extern void test_register_kworker(void);
extern void test_register_registry(void);
extern void test_register_boot_init(void);
extern void test_register_smp_lifecycle(void);
extern void test_register_kernel_config(void);
extern void test_register_boot_timing(void);
extern void test_register_boot_info(void);
extern void test_register_boot_reserved(void);
extern void test_register_serial_emergency(void);
extern void test_register_idt(void);
extern void test_register_boot_version(void);
extern void test_register_boot_caps(void);
extern void test_register_boot_decision(void);
extern void test_register_boot_rollback(void);
extern void test_register_ab_boot(void);
extern void test_register_boot_warm_update(void);
extern void test_register_boot_history(void);
extern void test_register_boot_audit(void);
extern void test_register_boot_diag(void);
extern void test_register_vpd(void);
extern void test_register_klog(void);
extern void test_register_uefi_boot(void);
extern void test_register_boot_entry_parser(void);
extern void test_register_boot_entry_kind(void);
extern void test_register_boot_policy(void);
extern void test_register_firmware_tables(void);
extern void test_register_firmware_platform(void);
extern void test_register_firmware_advisor(void);
extern void test_register_boot_health(void);
extern void test_register_boot_health_check(void);
extern void test_register_timer_tick_cb(void);
extern void test_register_time(void);
extern void test_register_boot_trend(void);
extern void test_register_smbios(void);
extern void test_register_smbios_parse(void);
extern void test_register_uefi_advanced(void);
extern void test_register_mat_violation(void);
extern void test_register_json_builder(void);
extern void test_register_boot_perf_budget(void);
extern void test_register_irq_timer(void);
extern void test_register_entropy(void);
extern void test_register_boot_seed(void);
extern void test_register_klibs(void);
extern void test_register_tpm_transport(void);
extern void test_register_tpm_event_log(void);
extern void test_register_tpm_sb_reconcile(void);
extern void test_register_tpm_pcr_alloc(void);
extern void test_register_sha256(void);
extern void test_register_acpi_global_lock(void);
extern void test_register_sha1(void);
extern void test_register_sha384(void);
extern void test_register_tpm_replay(void);
extern void test_register_tpm_nv(void);
extern void test_register_tpm_baseline(void);
extern void test_register_tpm_enroll_gate(void);
extern void test_register_tpm_seal(void);
extern void test_register_tpm_attest(void);
extern void test_register_tpm_attest_report(void);
extern void test_register_boot_device(void);
extern void test_register_bare_metal(void);
extern void test_register_x86(void);
extern void test_register_cpu_seq(void);
extern void test_register_ob(void);
extern void test_register_security(void);
extern void test_register_ci(void);
extern void test_register_peb_teb(void);
extern void test_register_env(void);
extern void test_register_nt_types(void);
extern void test_register_nt_sync(void);
extern void test_register_nt_misc(void);
extern void test_register_nt_audit(void);
extern void test_register_syscall_filter(void);
extern void test_register_ipc(void);
extern void test_register_alpc(void);
extern void test_register_storage(void);
extern void test_register_nvme(void);
extern void test_register_usb_boot(void);
extern void test_register_usb_hid(void);
extern void test_register_ixfs(void);
extern void test_register_ixfs_fsck(void);
extern void test_register_watchdog(void);
extern void test_register_bulletproof(void);
extern void test_register_blackbox(void);
extern void test_register_acpi_power(void);
extern void test_register_exec(void);
extern void test_register_kimage(void);
extern void test_register_usermode_launcher(void);
extern void test_register_fastpath_hardening(void);
extern void test_register_crashdump(void);
extern void test_register_panic(void);
extern void test_register_except(void);
extern void test_register_quota(void);
extern void test_register_quota_owner(void);
extern void test_register_quota_perf(void);
extern void test_register_quota_config(void);
extern void test_register_quota_syscall(void);
extern void test_register_quota_pressure(void);
extern void test_register_quota_dashboard(void);
extern void test_register_quota_ledger(void);
extern void test_register_quota_stall(void);
extern void test_register_unwind(void);
extern void test_register_harness(void);
extern void test_register_desktop(void);
extern void test_register_ex(void);
extern void test_register_nls(void);
extern void test_register_knf(void);

void test_runner_init(void)
{
    klog(LOG_INFO, test_tag(), "============ KERNEL UNIT TESTS ============");

    /* MM */
    test_register_pmm();
    test_register_heap();
    test_register_vmm();
    test_register_swap();
    test_register_mmap();

    /* FS */
    test_register_vfs();
    test_register_ixfs();
    test_register_ixfs_fsck();
    test_register_watchdog();

    /* Sched */
    test_register_sched();
    test_register_proc_ext();
    test_register_kworker();

    /* ABI */
    test_register_registry();

    /* Boot */
    test_register_boot_init();
    test_register_smp_lifecycle();
    test_register_kernel_config();
    test_register_boot_timing();
    test_register_boot_info();
    test_register_boot_reserved();
    test_register_serial_emergency();
    test_register_idt();
    test_register_boot_version();
    test_register_boot_caps();
    test_register_boot_decision();
    test_register_boot_rollback();
    test_register_ab_boot();
    test_register_boot_warm_update();
    test_register_boot_history();
    test_register_boot_audit();
    test_register_boot_diag();
    test_register_vpd();
    test_register_klog();
    test_register_uefi_boot();
    test_register_boot_entry_parser();
    test_register_boot_entry_kind();
    test_register_boot_policy();
    test_register_firmware_tables();
    test_register_firmware_platform();
    test_register_firmware_advisor();
    test_register_boot_health();
    test_register_boot_health_check();
    test_register_timer_tick_cb();
    test_register_time();
    test_register_boot_trend();
    test_register_smbios();
    test_register_smbios_parse();
    test_register_uefi_advanced();
    test_register_mat_violation();
    test_register_json_builder();
    test_register_boot_perf_budget();
    test_register_irq_timer();
    test_register_entropy();
    test_register_boot_seed();
    test_register_klibs();
    test_register_tpm_transport();
    test_register_tpm_event_log();
    test_register_tpm_sb_reconcile();
    test_register_tpm_pcr_alloc();
    test_register_sha256();
    test_register_acpi_global_lock();
    test_register_sha1();
    test_register_sha384();
    test_register_tpm_replay();
    test_register_tpm_nv();
    test_register_tpm_baseline();
    test_register_tpm_enroll_gate();
    test_register_tpm_seal();
    test_register_tpm_attest();
    test_register_tpm_attest_report();
    test_register_boot_device();
    test_register_bare_metal();
    test_register_harness();

    /* x86-64 Architecture (CPUID, MSR, KPTI, CPU security) */
    test_register_x86();
    test_register_cpu_seq();

    /* Exec / Binary System */
    test_register_exec();
    test_register_kimage();
    test_register_usermode_launcher();
    test_register_fastpath_hardening();

    /* OB */
    test_register_ob();

    /* Security */
    test_register_security();
    test_register_ci();

    /* Crash dump */
    test_register_crashdump();
    test_register_panic();

    /* Exception dispatch / SEH */
    test_register_except();
    test_register_unwind();

    /* Resource accounting & quotas */
    test_register_quota();
    test_register_quota_owner();
    test_register_quota_perf();
    test_register_quota_config();
    test_register_quota_syscall();
    test_register_quota_pressure();
    test_register_quota_dashboard();
    test_register_quota_ledger();
    test_register_quota_stall();

    /* ABI */
    test_register_peb_teb();
    test_register_env();
    test_register_nt_types();
    test_register_nt_sync();
    test_register_nt_misc();
    test_register_nt_audit();
    test_register_syscall_filter();
    test_register_bulletproof();

    /* IPC */
    test_register_ipc();
    test_register_alpc();

    /* Storage */
    test_register_storage();
    test_register_nvme();
    test_register_usb_boot();
    test_register_usb_hid();
    test_register_blackbox();
    test_register_acpi_power();

    /* Desktop UI */
    test_register_desktop();

    /* Executive support runtime */
    test_register_ex();

    /* Atom/NLS/locale subsystem */
    test_register_nls();

    /* Kernel Notification Facility */
    test_register_knf();

    klog(LOG_INFO, test_tag(), "%u suite(s) registered",
         (uint64_t)g_test_state.suite_count);
}

/* Close a category's quota sweep: compare the outstanding USER-block quota
 * now against `open`, taken when the category started.
 *
 * Only a POSITIVE per-type usage delta is gated. Three distinct outcomes, kept
 * distinct on purpose:
 *
 *   - indeterminate: either snapshot mixed two registry membership
 *     generations, so the comparison is between two different populations.
 *     Reported, never counted -- an indeterminate reading is not evidence.
 *   - count-only: blocks were created or retained but no type gained usage.
 *     A canonical USER block is legitimately kept alive by the registry once
 *     a principal has ever charged, so gating this would fail CI forever
 *     after the first quota test ran. Reported at INFO.
 *   - usage: at least one type ends with more charged than it started with.
 *     That is an obligation nobody returned. Gated.
 */
quota_sweep_verdict_t quota_sweep_classify(const quota_leak_snapshot_t *open,
                                           const quota_leak_snapshot_t *close,
                                           uint32_t *leaked_types)
{
    uint32_t leaked = 0;

    if (leaked_types)
        *leaked_types = 0;
    if (!open || !close || !open->coherent || !close->coherent)
        return QUOTA_SWEEP_INDETERMINATE;

    for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++) {
        /* A NEGATIVE delta is a return, never a leak: a category may legally
         * end holding less than it started with (an earlier category's charge
         * returned inside this one). Only growth is an unmet obligation. */
        if (close->usage[t] > open->usage[t])
            leaked++;
    }

    if (leaked_types)
        *leaked_types = leaked;
    if (leaked > 0)
        return QUOTA_SWEEP_LEAKED;
    if (close->user_blocks != open->user_blocks)
        return QUOTA_SWEEP_COUNT_ONLY;
    return QUOTA_SWEEP_CLEAN;
}

/* Retries allowed when a closing snapshot comes back indeterminate. Membership
 * churn and mutation are transient at a category boundary (no suite is
 * running), so a couple of retries almost always settle. */
#define QUOTA_SWEEP_RETRIES 3u

/* `close_snap` is the boundary reading, taken ONCE by the caller and shared
 * with the next category's opening baseline -- see the call site for why two
 * separate readings would open a leak-hiding window. */
static void quota_sweep_close(const quota_leak_snapshot_t *open,
                              const char *cat_name,
                              const quota_leak_snapshot_t *close_snap)
{
    const quota_leak_snapshot_t now = *close_snap;
    uint32_t leaked_types = 0;

    switch (quota_sweep_classify(open, &now, &leaked_types)) {
    case QUOTA_SWEEP_INDETERMINATE:
        /* FAIL CLOSED. An indeterminate pair means the gate could not be
         * established for this category -- not that the category was clean.
         * Reporting it and returning would disarm the gate precisely when
         * concurrent activity makes measurement hard, and the summary would
         * still print a reassuring "0 quota-leaked". A gate that cannot run
         * is a failed gate, so it counts like one. */
        klog(LOG_ERROR, test_tag(),
             "[QLEAK] %s :: gate INDETERMINATE after %u retries "
             "(opening coherent=%u, closing coherent=%u)",
             cat_name, (uint64_t)QUOTA_SWEEP_RETRIES,
             (uint64_t)open->coherent, (uint64_t)now.coherent);
        g_test_state.quota_leaked++;
        return;

    case QUOTA_SWEEP_LEAKED:
        for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++) {
            int64_t delta = now.usage[t] - open->usage[t];
            if (delta <= 0)
                continue;
            klog(LOG_ERROR, test_tag(),
                 "[QLEAK] %s :: %s +%llu still charged (entry %llu -> exit %llu)",
                 cat_name, quota_resource_type_name((quota_resource_type_t)t),
                 (uint64_t)delta, (uint64_t)open->usage[t],
                 (uint64_t)now.usage[t]);
        }
        /* ONE per leaking category, not one per type: the counter is folded
         * into the host FAILED total, and a category leaking four types is
         * still one category that failed to clean up. */
        g_test_state.quota_leaked++;
        return;

    case QUOTA_SWEEP_COUNT_ONLY:
        klog(LOG_INFO, test_tag(),
             "[QLEAK-OK] %s :: USER blocks %u -> %u, no outstanding usage",
             cat_name, (uint64_t)open->user_blocks, (uint64_t)now.user_blocks);
        return;

    case QUOTA_SWEEP_CLEAN:
        return;
    }
}

/* ---- Run all registered suites ---- */

void test_runner_run(void)
{
    uint64_t run_start = system_get_ticks();
    uint32_t run_count = 0;

    /* Count suites that will run */
    for (uint32_t i = 0; i < g_test_state.suite_count; i++) {
        test_suite_t *s = &g_test_state.suites[i];
        if (g_filter == TEST_CAT_ALL || s->cat == g_filter || s->cat == TEST_CAT_ALL)
            run_count++;
    }

    if (g_filter != TEST_CAT_ALL && g_filter < TEST_CAT_COUNT) {
        klog(LOG_INFO, test_tag(), "=== Running %u suite(s) [%s] ===",
             (uint64_t)run_count, cat_labels[g_filter]);
    } else {
        klog(LOG_INFO, test_tag(), "=== Running %u suite(s) ===",
             (uint64_t)run_count);
    }

    test_category_t current_cat = TEST_CAT_COUNT; /* sentinel: no category printed yet */

    /* [COUNT] trace ordinal. A LOCAL, not a static: the suite loop is walked
     * once by the BSP, so a file-scope counter would add a mutable global to a
     * kernel that must be SMP-safe by default and would carry stale state into
     * a second test_runner_run() call. Scoped here it is neither. */
    uint32_t count_records = 0;

    /* Per-CATEGORY quota leak sweep. Suite granularity is deliberately NOT
     * used: a quota block is a shared, refcounted, cross-suite object (a
     * canonical USER block is created once and reused by every later suite
     * charging as that principal), so a per-suite delta would flag ordinary
     * sharing as a leak. A category is the smallest unit over which
     * outstanding quota genuinely should return to where it started.
     *
     * The boundary is sampled at the point the runner switches categories,
     * which is AFTER the previous category's last suite ran its action drain
     * -- so deferred cleanup has already happened when the closing snapshot is
     * taken. */
    quota_leak_snapshot_t cat_snap_open;
    int                   cat_snap_valid = 0;

    /* Forward decl: TEST_CAT_DESKTOP auto-isolation. Codex [H] section
     * 15 review -- wiring the reset into the runner so every desktop
     * suite starts from a known baseline without each test having to
     * register the action manually. Symbol lives in
     * src/kernel/test/test_desktop_reset.c under KERNEL_TESTS; both
     * are compiled together so the link is always satisfied. */
    extern void test_desktop_reset(void);

    for (uint32_t i = 0; i < g_test_state.suite_count; i++) {
        test_suite_t *s = &g_test_state.suites[i];

        /* Skip suites not matching the filter */
        if (g_filter != TEST_CAT_ALL && s->cat != g_filter && s->cat != TEST_CAT_ALL)
            continue;

        /* Print category header on first suite in each category */
        if (s->cat < TEST_CAT_COUNT && s->cat != current_cat) {
            /* ONE snapshot both closes the outgoing category and opens the
             * incoming one. Taking two separate readings leaves a window
             * between them, and any charge landing in that window is folded
             * into the NEW baseline: it then produces no positive delta at any
             * later boundary, so a genuine cross-category leak could end the
             * run reporting "0 quota-leaked". A shared boundary sample has no
             * such window by construction. */
            quota_leak_snapshot_t boundary;
            for (uint32_t try = 0; try < QUOTA_SWEEP_RETRIES; try++)
                if (quota_leak_snapshot(&boundary))
                    break;

            if (cat_snap_valid)
                quota_sweep_close(&cat_snap_open, cat_names[current_cat],
                                  &boundary);

            current_cat = s->cat;
            klog(LOG_INFO, test_tag(), "--- [%s] %s ---",
                 cat_names[current_cat], cat_labels[current_cat]);

            cat_snap_open  = boundary;
            cat_snap_valid = 1;
        }

        g_test_state.current_suite = s->name;

        /* Flip the klog subsystem tag so desktop suites render in the
         * #C586B5 pink-mauve band (klog.c DTEST route) instead of the
         * default #D2A8FF kernel-test lavender. */
        s_current_tag = (s->cat == TEST_CAT_DESKTOP) ? "DTEST" : "TEST";

        uint32_t pre_pass = g_test_state.passed;
        uint32_t pre_fail = g_test_state.failed;
        uint32_t pre_skip = g_test_state.skipped;
        uint32_t pre_pend = g_test_state.pending;

        /* Start each suite with an empty action stack. */
        test_actions_reset();

        /* Per-test heap-leak snapshot. Reset opt-out flags so a suite
         * that does NOT call TEST_EXPECT_LEAK / TEST_LEAK_IGNORE gets
         * the strict (delta != 0 -> [LEAK]) contract.
         *
         * s_last_leak_delta is intentionally NOT reset here -- a verify
         * suite reads the previous suite's delta via
         * test_runner_last_leak_delta() inside its own body. The post-
         * drain block below overwrites it with the current suite's
         * delta after this body returns, so each suite reads the
         * IMMEDIATELY-PRECEDING completed suite's value. */
        s_heap_used_at_entry  = heap_get_used();
        s_expected_leak_bytes = 0;
        s_leak_ignore         = 0;
        s_leak_reason         = (void *)0;

        /* Per-category auto-isolation. TEST_CAT_DESKTOP suites touch
         * shared WM / keyboard / terminal / compositor globals; reset
         * that state before every desktop suite so a leak from an
         * earlier suite cannot poison the next. Other categories do
         * not need equivalent hooks today (kernel subsystem tests
         * already have their own scratch allocator / heap snapshot
         * infrastructure). */
        if (s->cat == TEST_CAT_DESKTOP)
            test_desktop_reset();

        s->fn();

        /* Close any collapsed PASS run before the suite's own trailing output
         * (action drain, leak check, summary). Without this the "repeated xN"
         * line would surface after those, reading as if it belonged to the next
         * suite. The pointer-identity check would also stop matching across the
         * boundary anyway, but flushing HERE is what keeps the ordering honest
         * rather than relying on that. */
        test_pass_run_flush();

        /* Drain any actions the suite registered via test_add_action().
         * Runs regardless of whether the body's assertions passed or
         * failed -- that's the whole point of the registry: resources
         * are freed even on assertion-induced early return. */
        test_actions_drain();

        /* Leak check runs AFTER the action drain so tests that use
         * TEST_SCRATCH_KBUF or TEST_KLOG_SUPPRESS (both register
         * cleanup via) get clean delta=0. The delta is the signed
         * difference in heap_get_used(); kmalloc includes block-
         * header overhead so an intentional kmalloc(64) without
         * kfree typically produces delta > 64. */
        int64_t heap_after = (int64_t)heap_get_used();
        s_last_leak_delta = heap_after - (int64_t)s_heap_used_at_entry;

        if (s_leak_ignore) {
            klog(LOG_INFO, test_tag(),
                 "%s :: [LEAK-SKIP] %s",
                 s->name,
                 s_leak_reason ? s_leak_reason : "(no reason)");
        } else if (s_last_leak_delta > 0) {
            /* Only positive deltas are leaks. A negative delta means
             * the heap shrank during the suite (e.g. a verify suite
             * frees a buffer the previous suite intentionally leaked,
             * or a background kthread freed boot-path state). Silent
             * by design -- not a leak. */
            if (s_expected_leak_bytes > 0 &&
                s_last_leak_delta >= s_expected_leak_bytes) {
                /* TEST_EXPECT_LEAK armed with a positive byte count and
                 * the observed positive delta meets-or-exceeds it. The
                 * lower-bound match (>=) tolerates kmalloc block-header
                 * overhead -- a kmalloc(64) unfreed surfaces as ~72-80
                 * byte delta. Tests that need strict equality should
                 * use TEST_ASSERT_EQ on heap_get_used() directly.
                 *
                 * Negative or zero `bytes` is rejected as malformed
                 * (not an opt-out) so a stale TEST_EXPECT_LEAK(-1, ...)
                 * or TEST_EXPECT_LEAK(0, ...) cannot mask a real leak. */
                klog(LOG_INFO, test_tag(),
                     "%s :: [LEAK-OK] intentional %lu bytes (%s)",
                     s->name,
                     (uint64_t)(s_last_leak_delta > 0 ? s_last_leak_delta : -s_last_leak_delta),
                     s_leak_reason ? s_leak_reason : "(no reason)");
            } else {
                klog(LOG_ERROR, test_tag(),
                     "%s :: [LEAK] leaked %lu bytes (entry %lu -> exit %lu)",
                     s->name,
                     (uint64_t)(s_last_leak_delta > 0 ? s_last_leak_delta : -s_last_leak_delta),
                     s_heap_used_at_entry,
                     (uint64_t)heap_after);
                g_test_state.leaked++;
            }
        }

        /* Defensive hygiene: clear every fault-injection arm state so
         * a suite that armed a countdown and then failed an assertion
         * before it fired cannot poison the next suite. Each _clear()
         * is a single-field zero write; no-op when already zero. */
        kmalloc_fail_countdown_clear();
        kmalloc_fail_task_filter_clear();
        kmalloc_fail_max_injections_clear();
        pmm_alloc_fail_countdown_clear();
        pmm_alloc_fail_task_filter_clear();
        pmm_alloc_fail_max_injections_clear();
        vmm_map_fail_countdown_clear();
        vmm_map_fail_task_filter_clear();
        vmm_map_fail_max_injections_clear();
        copy_user_fail_countdown_clear();
        copy_user_fail_task_filter_clear();
        copy_user_fail_max_injections_clear();

        uint32_t suite_fails = g_test_state.failed - pre_fail;

        /* The [COUNT] record. This delta used to be computed and discarded on
         * the very next line, which is why a drifting headline total had no
         * attribution anywhere in the log. Emitted AFTER the action drain and
         * the leak check so it reports what the suite finally accounted for,
         * not a mid-teardown reading. */
        if (g_count_trace) {
            char     rec[TEST_COUNT_RECORD_MAX];
            uint32_t ord = ++count_records;
            (void)test_count_record_format(rec, sizeof(rec), ord,
                                           (s->cat < TEST_CAT_COUNT)
                                               ? cat_names[s->cat] : "all",
                                           s->name,
                                           g_test_state.passed  - pre_pass,
                                           suite_fails,
                                           g_test_state.skipped - pre_skip,
                                           g_test_state.pending - pre_pend);
            /* Unrated: a record the shared limiter dropped is invisible, and a
             * consumer comparing two runs would then read the same truncated
             * subset as "identical" -- silently, and most likely for the one
             * suite that moved. The `trunc=1` return is deliberately ignored:
             * the formatter always leaves a well-formed, self-describing line,
             * and SKIPPING the emission would open the ordinal gap that the
             * completeness check is there to catch.
             *
             * Tagged "TEST" unconditionally rather than test_tag(): the tag is
             * flipped to "DTEST" while a desktop suite runs, which would split
             * these records across two subsystems and hand every consumer a
             * second pattern to remember. Measured while building the host
             * comparison -- a single-tag grep silently returned 2656 of 2704
             * records, and only the trailer's count caught it. */
            klog_unrated(LOG_INFO, TEST_COUNT_TAG, "%s", rec);
        }

        if (suite_fails > 0)
            g_test_state.suites_failed++;
        else
            g_test_state.suites_passed++;
    }

    /* Close the LAST category: the loop's boundary check only fires on a
     * transition, so without this the final category is never swept. Nothing
     * follows, so this closing reading is not shared with anything. */
    if (cat_snap_valid) {
        quota_leak_snapshot_t final_snap;
        for (uint32_t try = 0; try < QUOTA_SWEEP_RETRIES; try++)
            if (quota_leak_snapshot(&final_snap))
                break;
        quota_sweep_close(&cat_snap_open, cat_names[current_cat], &final_snap);
    }

    /* Restore default tag so the final summary renders in the kernel
     * test color even after the last-run suite was a desktop suite. */
    s_current_tag = "TEST";

    /* [COUNT] trailer. Announcing how many records were emitted is what makes
     * the trace completeness-CHECKABLE rather than merely present: a consumer
     * that counted a different number is looking at truncated input and must
     * report that, instead of comparing a subset across runs and calling the
     * result stable. The run totals ride along so a consumer can also confirm
     * the per-suite records sum to the headline it is trying to explain. */
    if (g_count_trace) {
        char trailer[TEST_COUNT_RECORD_MAX];
        (void)test_count_trailer_format(trailer, sizeof(trailer),
                                        count_records,
                                        g_test_state.passed,
                                        g_test_state.failed,
                                        g_test_state.skipped,
                                        g_test_state.pending);
        klog_unrated(LOG_INFO, TEST_COUNT_TAG, "%s", trailer);
    }

    /* ---- Summary ---- */
    uint64_t run_ms   = (system_get_ticks() - run_start) * 10;
    uint64_t run_sec  = run_ms / 1000;
    uint64_t run_frac = (run_ms % 1000) / 100;
    uint32_t total = g_test_state.passed + g_test_state.failed;
    uint32_t skipped = g_test_state.skipped;
    uint32_t pending = g_test_state.pending;
    uint32_t leaked  = g_test_state.leaked;
    uint32_t qleaked = g_test_state.quota_leaked;

    /* Single canonical summary including pending (deferred-feature
     * stubs) and leaked (suites with non-zero heap_get_used delta
     * absent TEST_EXPECT_LEAK). 'tests passed' prefix is load-bearing
     * for scripts/test.sh's summary regex. scripts/test.sh parses the
     * 'N leaked' number and folds it into FAILED -- any unannotated
     * leak fails the test run (TODO-03 -9 CI-gating flip). Suites
     * with intentional leaks use TEST_EXPECT_LEAK / TEST_LEAK_IGNORE
     * which suppresses the counter at the kernel level. */
    if (g_test_state.failed == 0) {
        klog(LOG_INFO, "TEST",
             "=== %u tests passed, 0 failed, %u skipped, %u pending, %u leaked, %u quota-leaked (%u.%us) ===",
             (uint64_t)total, (uint64_t)skipped, (uint64_t)pending,
             (uint64_t)leaked, (uint64_t)qleaked, run_sec, run_frac);
    } else {
        /* Failure form keeps the 'tests passed' prefix so
         * scripts/test.sh:184's `=== [0-9]+ tests? passed` regex still
         * extracts the summary line on a non-zero-failure run. The
         * extra fields (FAILED count, of-total) are appended after. */
        klog(LOG_ERROR, "TEST",
             "=== %u tests passed, %u FAILED, %u skipped, %u pending, %u leaked, %u quota-leaked (of %u) (%u.%us) ===",
             (uint64_t)g_test_state.passed, (uint64_t)g_test_state.failed,
             (uint64_t)skipped, (uint64_t)pending, (uint64_t)leaked,
             (uint64_t)qleaked, (uint64_t)total, run_sec, run_frac);
    }
}

#endif /* KERNEL_TESTS */
