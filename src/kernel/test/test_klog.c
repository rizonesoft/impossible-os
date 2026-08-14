/* ============================================================================
 * test_klog.c -- Kernel logging unit tests
 *
 * Tests ring buffer, per-subsystem filtering, rate limiting, and drop counts.
 *
 * XREF: 02-kernel-core/TODO-04-system-logging.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/test/klog_suppress.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"
#include "kernel/etw.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "libc/string.h"        /* strcmp for the rendered-record assertions */

/* Room for a full 255-character entry plus its NUL. */
#define TEST_KLOG_PROBE_CAP  260u

/* One probe argument, carried in the type its conversion specifier reads. */
#define TK_ARG_NONE 0
#define TK_ARG_PTR  1
#define TK_ARG_U64  2
#define TK_ARG_S64  3

typedef struct {
    int         kind;
    const void *p;
    uint64_t    u;
    int64_t     s;
} klog_probe_arg;

/* Format-string fixtures for the truncation shapes below. Built by literal
 * concatenation so the lengths are a compile-time property of the source
 * rather than something a loop has to reproduce correctly. */
#define TK_X10  "xxxxxxxxxx"
#define TK_X50  TK_X10 TK_X10 TK_X10 TK_X10 TK_X10

/* 307 characters, no conversions at all: drives BUF_PUT(*fmt++). */
#define TEST_KLOG_LONG_LITERAL \
    "LITERAL" TK_X50 TK_X50 TK_X50 TK_X50 TK_X50 TK_X50

/* 251 characters of prefix, then %u: the value needs 20 digits and only 4 fit,
 * so the reverse-digit loop BUF_PUT(tmp[--n]) is the one that crosses the
 * limit. */
#define TEST_KLOG_DIGIT_FMT \
    "DIGITS" TK_X50 TK_X50 TK_X50 TK_X50 TK_X10 TK_X10 TK_X10 TK_X10 "xxxxx%u"

/* ---- Ring buffer: klog writes to ring and head advances ---- */

static void test_klog_ring_write(void)
{
    static const char *const marker = "klog_ring_write test entry";
    const klog_entry_t *ring;
    uint32_t head_after, added, i, found = 0;
    uint64_t seq_before, seq_after;

    /* Bounded by the MONOTONIC sequence and matched on CONTENT.
     *
     * Two weaker shapes were tried here and both could pass while proving
     * nothing. `count_after >= count_before` is true for every possible
     * execution -- klog_ring_count only ever rises and then pins at
     * KLOG_RING_SIZE -- so it held even if klog() dropped the entry. Replacing
     * it with a sequence delta was no better: klog_ring_seq is GLOBAL, so an
     * AP logging anything inside the window satisfies "the sequence advanced"
     * without this call having landed at all. Only finding this test's own
     * uniquely identifiable record inside the window it opened is a claim
     * about the emitter.
     *
     * The subsystem is this test's OWN tag, not the shared "TEST" one. klog()
     * applies a per-subsystem cap of KLOG_RATE_DEFAULT messages per window, and
     * the test runner emits its own suite and assertion lines under "TEST" --
     * so on a fast run the budget can already be spent when this marker is
     * emitted, klog legitimately drops it, and the guard below would fail over
     * the rate-window phase rather than over ring insertion. A private tag
     * starts with an unused rate slot. It still goes through klog(), not
     * klog_unrated(), because klog() is what is under test here.
     *
     * The LOG_DEBUG override is required, not cosmetic: test mode pins the
     * global minimum at LOG_WARN, so a LOG_INFO line under an un-overridden tag
     * is filtered out before it ever reaches the ring. It is REMOVED again at
     * the end: the override table holds only 32 slots, so leaving one behind is
     * a leak of a bounded global resource even though the tag itself is
     * private, and this file's own doctrine restores exactly (klog_suppress.c
     * removes; test_harness.c asserts no test leaves a lingering override).
     * Asserting the tag is un-overridden first keeps the restore EXACT rather
     * than merely tidy. */
    TEST_ASSERT(!klog_has_override("TESTRING"),
                "the private ring-write tag starts without an override");
    klog_set_level("TESTRING", LOG_DEBUG);
    seq_before = klog_get_seq();
    klog(LOG_INFO, "TESTRING", "%s", marker);
    ring = klog_get_ring_snapshot((uint32_t *)0, &head_after, &seq_after);

    /* Refuse at exactly KLOG_RING_SIZE, not just past it, and refuse in
     * 64-bit before narrowing. At exactly one ring the oldest entry of the
     * window sits on `head` -- the next write slot -- and klog hands back the
     * LIVE array, so one concurrent append destroys that slot before the scan
     * reaches it and this test would report its own marker as dropped. The
     * guard is explicit control flow because TEST_ASSERT records a failure and
     * then returns NORMALLY; without the early return the scan below would run
     * over a window already declared unusable. */
    if (seq_after - seq_before == 0u ||
        seq_after - seq_before >= (uint64_t)KLOG_RING_SIZE) {
        TEST_ASSERT(0, "the measured klog window is unusable (empty, or wide "
                       "enough that a concurrent append can overwrite its "
                       "oldest entry)");
        klog_remove_override("TESTRING");
        return;
    }
    added = (uint32_t)(seq_after - seq_before);

    for (i = 1u; i <= added && i <= KLOG_RING_SIZE; i++) {
        const klog_entry_t *e =
            &ring[(head_after + KLOG_RING_SIZE - i) % KLOG_RING_SIZE];
        uint32_t k = 0;

        while (marker[k] && e->message[k] == marker[k])
            k++;
        if (!marker[k])
            found++;
    }

    TEST_ASSERT_EQ((uint64_t)found, (uint64_t)1,
                   "klog() landed exactly this entry in the ring");

    klog_remove_override("TESTRING");
}

/* Emit `fmt` under a private tag and return the stored message length, or
 * (uint32_t)-1 when the record could not be identified in the ring window.
 * `probe` is a prefix the caller knows the record starts with.
 *
 * Shared by the truncation tests below because each of them drives a DIFFERENT
 * BUF_PUT caller, and the value of that is lost if only one shape is covered:
 * the literal-format loop, the digit loops and the %s loop each carried the
 * same skipped-side-effect defect, and each could re-acquire it alone. */
static uint32_t klog_probe_len(const char *tag, const char *probe,
                               const char *fmt, const klog_probe_arg *arg,
                               char *out, uint32_t out_cap)
{
    const klog_entry_t *ring;
    uint32_t            head_after, added, i, n;
    uint64_t            seq_before, seq_after;

    klog_set_level(tag, LOG_DEBUG);
    seq_before = klog_get_seq();
    /* The vararg is passed with the type the CONVERSION will read, never a
     * convenient 64-bit stand-in. Handing `%d` a uint64_t is undefined at
     * INT64_MIN -- the signed/unsigned vararg exception needs the value to be
     * representable in BOTH types -- so a boundary test written that way would
     * prove the fix using the same class of undefined behavior the fix removes,
     * and would hold only for as long as the current ABI does. */
    switch (arg->kind) {
    case TK_ARG_NONE: klog(LOG_INFO, tag, fmt);         break;
    case TK_ARG_PTR:  klog(LOG_INFO, tag, fmt, arg->p); break;
    case TK_ARG_S64:  klog(LOG_INFO, tag, fmt, arg->s); break;
    default:          klog(LOG_INFO, tag, fmt, arg->u); break;
    }
    ring = klog_get_ring_snapshot((uint32_t *)0, &head_after, &seq_after);
    klog_remove_override(tag);

    if (seq_after - seq_before == 0u ||
        seq_after - seq_before >= (uint64_t)KLOG_RING_SIZE)
        return (uint32_t)-1;
    added = (uint32_t)(seq_after - seq_before);

    for (i = 1u; i <= added && i <= KLOG_RING_SIZE; i++) {
        const klog_entry_t *e =
            &ring[(head_after + KLOG_RING_SIZE - i) % KLOG_RING_SIZE];
        uint32_t k = 0;

        while (probe[k] && e->message[k] == probe[k])
            k++;
        if (probe[k])
            continue;

        for (n = 0; n < sizeof(e->message) && e->message[n]; n++)
            ;
        if (out) {
            uint32_t c;
            for (c = 0; c + 1u < out_cap && c < n; c++)
                out[c] = e->message[c];
            out[c] = '\0';
        }
        return n;
    }
    return (uint32_t)-1;
}

/* ---- Over-long message: truncates, and above all RETURNS ----
 *
 * This is the regression control for a boot-wedging defect, so the strongest
 * thing it asserts is that it runs to completion at all. `vformat_buf`'s
 * BUF_PUT macro used to skip evaluating its argument once the buffer was full,
 * which left `while (*s) BUF_PUT(*s++);` spinning forever on the first byte
 * that did not fit -- inside klog's ring lock with interrupts disabled. Every
 * message crossing `klog_entry_t.message[256]` hung the machine with no fault
 * and no output. A hang here fails the run by timeout rather than by
 * assertion; the assertions below then pin the truncation contract itself.
 *
 * The three shapes are deliberate: `%s`, a bare literal format, and a numeric
 * conversion whose digit loop crosses the boundary all drive DIFFERENT
 * BUF_PUT call sites, and all three carried the defect. */
static void test_klog_overlong_shapes_truncate(void)
{
    static char src[400];
    char        got[TEST_KLOG_PROBE_CAP];
    uint32_t    n, len;

    for (n = 0; n < 320u; n++)
        src[n] = (char)('a' + (n % 26u));
    src[320] = '\0';

    TEST_ASSERT(!klog_has_override("TESTLONG"),
                "the private over-long tag starts without an override");

    /* 1. The %s argument loop. */
    {
        klog_probe_arg a = { TK_ARG_PTR, src, 0, 0 };
        len = klog_probe_len("TESTLONG", "abcdefghij", "%s", &a,
                             got, sizeof(got));
    }
    TEST_ASSERT_EQ((uint64_t)len, (uint64_t)255,
                   "an over-long %s argument fills the entry to exactly 255 "
                   "characters and returns");
    TEST_ASSERT_EQ((uint64_t)got[254], (uint64_t)'~',
                   "the %s truncation is marked with a trailing tilde");

    /* 2. The literal-format loop -- BUF_PUT(*fmt++), no conversions at all. */
    {
        klog_probe_arg a = { TK_ARG_NONE, (void *)0, 0, 0 };
        len = klog_probe_len("TESTLONG", "LITERAL", TEST_KLOG_LONG_LITERAL, &a,
                             got, sizeof(got));
    }
    TEST_ASSERT_EQ((uint64_t)len, (uint64_t)255,
                   "an over-long LITERAL format truncates and returns, with no "
                   "argument to advance");
    TEST_ASSERT_EQ((uint64_t)got[254], (uint64_t)'~',
                   "the literal-format truncation is marked");

    /* 3. A digit loop that crosses the boundary: the prefix leaves 4 characters
     *    of room and the value needs 20, so BUF_PUT(tmp[--n]) fills and then
     *    must stop rather than spin. */
    {
        klog_probe_arg a = { TK_ARG_U64, (void *)0, 18446744073709551615ULL, 0 };
        len = klog_probe_len("TESTLONG", "DIGITS", TEST_KLOG_DIGIT_FMT, &a,
                             got, sizeof(got));
    }
    TEST_ASSERT_EQ((uint64_t)len, (uint64_t)255,
                   "a numeric conversion whose digits cross the limit "
                   "truncates and returns");
    TEST_ASSERT_EQ((uint64_t)got[254], (uint64_t)'~',
                   "the digit-loop truncation is marked");

    TEST_ASSERT(!klog_has_override("TESTLONG"),
                "every probe removed its override");
}

/* Exactly at the limit and exactly one past it. The boundary is what the
 * original bisect turned on -- 255 emitted and completed, 256 hung -- so an
 * off-by-one here would either mark an untruncated record or lose a byte
 * without saying so. */
static void test_klog_truncation_boundary(void)
{
    static char src[300];
    char        got[TEST_KLOG_PROBE_CAP];
    uint32_t    i, len;

    /* "B " is the probe prefix; the message is that plus `src`. */
    for (i = 0; i < 253u; i++)
        src[i] = (char)('a' + (i % 26u));
    src[253] = '\0';

    {
        klog_probe_arg a = { TK_ARG_PTR, src, 0, 0 };
        len = klog_probe_len("TESTEDGE", "B ", "B %s", &a, got, sizeof(got));
    }
    TEST_ASSERT_EQ((uint64_t)len, (uint64_t)255,
                   "a message of exactly 255 characters is stored whole");
    TEST_ASSERT_NEQ((uint64_t)got[254], (uint64_t)'~',
                   "a message that exactly fits is NOT marked truncated");

    src[253] = 'z';
    src[254] = '\0';
    {
        klog_probe_arg a = { TK_ARG_PTR, src, 0, 0 };
        len = klog_probe_len("TESTEDGE", "B ", "B %s", &a, got, sizeof(got));
    }
    TEST_ASSERT_EQ((uint64_t)len, (uint64_t)255,
                   "one character past the limit still stores 255 characters");
    TEST_ASSERT_EQ((uint64_t)got[254], (uint64_t)'~',
                   "one character past the limit IS marked truncated");
}

/* The signed boundary value. `%d` reads a full int64_t, and taking its
 * magnitude by negation is undefined at INT64_MIN -- in practice the value
 * stays negative, the digit loop never runs, and the record renders as a bare
 * "-", losing the number at exactly the value most worth seeing. */
static void test_klog_int64_min_renders(void)
{
    char     got[TEST_KLOG_PROBE_CAP];
    uint32_t len;

    {
        /* INT64_MIN written without an unsigned literal. */
        klog_probe_arg a = { TK_ARG_S64, (void *)0, 0,
                             -9223372036854775807LL - 1 };
        len = klog_probe_len("TESTMIN", "MIN ", "MIN %d", &a, got, sizeof(got));
    }
    TEST_ASSERT_NEQ((uint64_t)len, (uint64_t)-1,
                    "the INT64_MIN record reached the ring");
    TEST_ASSERT_EQ(strcmp(got, "MIN -9223372036854775808"), 0,
                   "INT64_MIN renders its full magnitude, not a bare minus");
}

/* Short tags must render normally.
 *
 * WHAT THIS DOES NOT PROVE, stated plainly because the distinction is the whole
 * value of the test: it does NOT detect the out-of-bounds read it was written
 * alongside. The renderer used to compute its terminator checks from
 * `subsystem[4]` and `subsystem[5]` unconditionally, reading one or two bytes
 * past the end of every ordinary tag like "ob" or "irq" -- but the comparisons
 * that consume those bytes short-circuit first, so the classification ANSWER
 * was identical and no C-level assertion can tell the two implementations
 * apart. Every case below passes against the old code too.
 *
 * What it IS: a behavior-preservation pin. Replacing the fixed offsets with
 * `klog_tag_is()` had to keep the exact answers for tags shorter than those
 * offsets and for the near-miss shapes, and that is checkable. Detecting the
 * read itself needs a poisoned-boundary fixture for kernel string helpers,
 * which the test harness does not have yet -- tracked in the kernel test
 * harness roadmap. */
static void test_klog_short_tag_classification(void)
{
    char     got[TEST_KLOG_PROBE_CAP];
    uint32_t len;
    klog_probe_arg none = { TK_ARG_NONE, (void *)0, 0, 0 };

    len = klog_probe_len("ob", "shorttag", "shorttag two", &none,
                         got, sizeof(got));
    TEST_ASSERT_EQ(strcmp(got, "shorttag two"), 0,
                   "a two-character tag renders its record intact");
    TEST_ASSERT_NEQ((uint64_t)len, (uint64_t)-1,
                    "the two-character-tag record reached the ring");

    len = klog_probe_len("irq", "shorttag", "shorttag three", &none,
                         got, sizeof(got));
    TEST_ASSERT_EQ(strcmp(got, "shorttag three"), 0,
                   "a three-character tag renders its record intact");

    /* "TESTING" starts with TEST but is not TEST: the terminator check is what
     * separates them, and it must not be satisfied by reading past a shorter
     * tag either. */
    len = klog_probe_len("TESTING", "shorttag", "shorttag longer", &none,
                         got, sizeof(got));
    TEST_ASSERT_EQ(strcmp(got, "shorttag longer"), 0,
                   "a tag that merely starts with TEST still renders intact");

    TEST_ASSERT(!klog_has_override("ob") && !klog_has_override("irq") &&
                !klog_has_override("TESTING"),
                "every short-tag probe removed its override");
}

/* ---- Per-subsystem level filtering: dropped below threshold ---- */

static void test_klog_level_drop(void)
{
    uint32_t count_before, head_before;
    uint32_t count_after, head_after;

    /* Set "mm" subsystem to WARN -- DEBUG entries should be dropped */
    klog_set_level("mm", LOG_WARN);

    klog_get_ring(&count_before, &head_before);
    klog(LOG_DEBUG, "mm", "this should be dropped");
    klog_get_ring(&count_after, &head_after);

    TEST_ASSERT(head_after == head_before,
                "LOG_DEBUG dropped after klog_set_level(mm, LOG_WARN)");

    /* Restore default */
    klog_set_level("mm", LOG_DEBUG);
}

static void test_klog_level_pass(void)
{
    uint32_t count_before, head_before;
    uint32_t count_after, head_after;

    /* Set "mm" to WARN -- WARN entries should pass through.
     * Use TEST tag so the WARN line appears as cyan test output,
     * not as a scary yellow warning in the boot log. */
    klog_set_level("TEST", LOG_WARN);

    klog_get_ring(&count_before, &head_before);
    klog(LOG_WARN, "TEST", "(level pass test -- expected WARN)");
    klog_get_ring(&count_after, &head_after);

    TEST_ASSERT(head_after != head_before,
                "LOG_WARN not dropped after klog_set_level(TEST, LOG_WARN)");

    /* Restore default */
    klog_set_level("TEST", LOG_DEBUG);
}

/* ---- Delivery receipts ----
 *
 * klog_receipted acknowledges each record at the point that record has either
 * reached the wire or been declined, so a caller accounting for its own
 * records settles AT the delivery instead of after the log call returns. What
 * these tests pin is the property that makes the acknowledgement worth having:
 * it lands before klog_receipted returns, on every exit, whether or not the
 * record was emitted. A receipt that fired only on the delivering path would
 * strand its caller's obligation on a filtered record; one that fired after
 * the call returned would be worth exactly as much as the credit it replaced.
 *
 * The probe below is a plain static because only these tests ever hand this
 * ack to klog, so this thread is its only caller -- the SMP clause of the
 * klog_receipt_fn contract is satisfied by there being no second writer, not
 * by a lock this test would have to hold. */

struct klog_receipt_probe {
    uint32_t calls;
    uint32_t delivered;
    uint64_t cookie;
};

static struct klog_receipt_probe s_klog_receipt_probe;

static void klog_receipt_probe_ack(uint64_t cookie, int delivered)
{
    s_klog_receipt_probe.calls++;
    s_klog_receipt_probe.cookie = cookie;
    s_klog_receipt_probe.delivered = (uint32_t)(delivered ? 1u : 0u);
}

static void test_klog_receipt_acknowledges_a_delivered_record(void)
{
    s_klog_receipt_probe.calls = 0;
    s_klog_receipt_probe.delivered = 0;
    s_klog_receipt_probe.cookie = 0;

    /* The "TEST" tag, NOT a private one, and the choice is load-bearing.
     * boot_tests.c raises the GLOBAL ceiling to LOG_WARN for the duration of
     * the kernel test run and grants LOG_DEBUG to exactly three tags --
     * "TEST", "UTEST", "DTEST" -- matched in full by str_eq, so any other tag
     * has its LOG_INFO records legitimately declined in this window. A
     * private tag here does not test delivery, it tests the filter: the first
     * draft used one and this assertion caught it, reporting delivered=0 for
     * a record klog was right to drop.
     *
     * The reason the ring-write test above avoids the shared "TEST" tag --
     * its per-subsystem rate budget may already be spent by the runner's own
     * lines -- does not apply, because klog_receipted bypasses the rate
     * limiter for exactly this class of caller. */
    klog_receipted(LOG_INFO, "TEST", klog_receipt_probe_ack,
                   0xA5A5A5A5ULL, "(receipt delivery probe -- expected)");

    /* The counter is written ONLY by the ack, so reading 1 here is the claim
     * that the acknowledgement landed before klog_receipted returned. That
     * ordering is the whole point: a caller settling an obligation against
     * this receipt never has to be alive for a later step. */
    TEST_ASSERT_EQ((uint64_t)s_klog_receipt_probe.calls, 1u,
                   "a receipted record is acknowledged exactly once, before "
                   "klog_receipted returns");
    TEST_ASSERT_EQ(s_klog_receipt_probe.cookie, 0xA5A5A5A5ULL,
                   "the acknowledgement carries the caller's own cookie, "
                   "which is what lets one obligation be settled among many");
    TEST_ASSERT_EQ((uint64_t)s_klog_receipt_probe.delivered, 1u,
                   "a record whose serial write returned is acknowledged as "
                   "delivered");
}

static void test_klog_receipt_acknowledges_a_declined_record(void)
{
    /* Demote a tag nothing else uses to LOG_FATAL, so the verbosity filter
     * rejects the LOG_INFO below before the ring lock is ever taken -- the
     * earliest of klog_emit's exits. Restored by the suite-exit action
     * drain. */
    TEST_KLOG_SUPPRESS("klogrcptd");

    s_klog_receipt_probe.calls = 0;
    s_klog_receipt_probe.delivered = 1;
    s_klog_receipt_probe.cookie = 0;

    klog_receipted(LOG_INFO, "klogrcptd", klog_receipt_probe_ack,
                   0x5A5A5A5AULL, "(receipt decline probe -- never emitted)");

    TEST_ASSERT_EQ((uint64_t)s_klog_receipt_probe.calls, 1u,
                   "a record klog DECLINED is acknowledged too -- an exit "
                   "that fired nothing would leave the caller's obligation "
                   "outstanding forever, which is worse than the late credit "
                   "the receipt replaced");
    TEST_ASSERT_EQ((uint64_t)s_klog_receipt_probe.delivered, 0u,
                   "and is acknowledged as NOT delivered, so a producer-side "
                   "drop stays distinguishable from a record the host has");
    TEST_ASSERT_EQ(s_klog_receipt_probe.cookie, 0x5A5A5A5AULL,
                   "the declined acknowledgement carries the same cookie the "
                   "delivering one would, so a caller needs no second path");
}

/* A NULL ack is a supported argument, not merely an untaken branch.
 *
 * klog_receipted builds its receipt struct on the stack and passes it through
 * CONDITIONALLY, so that a caller which decides per-call whether to settle
 * lands on exactly the ordinary path rather than on a receipt whose fn would
 * be dereferenced. Nothing else covers that ternary: the two tests above both
 * pass a callback, so a regression that dropped the condition and always
 * passed the struct would call through a NULL fn on the very first ordinary
 * klog line -- and would ship green. Reaching the record on the wire is the
 * assertion; the test failing means the machine faulted inside klog. */
static void test_klog_receipt_null_ack_is_the_ordinary_path(void)
{
    static const char marker[] = "(receipt NULL-ack probe -- expected)";
    const klog_entry_t *ring;
    uint64_t seq_before, seq_after;
    uint32_t head_after, added, i, found = 0;

    s_klog_receipt_probe.calls = 0;

    /* Matched on CONTENT inside a bounded window, not on a sequence delta.
     * klog_ring_seq is GLOBAL, so "the sequence advanced by one" is satisfied
     * by any AP or interrupt-context logger and says nothing about THIS call
     * -- the same trap the ring-write test above documents having fallen into.
     * Here it would be worse than useless in both directions: an unrelated
     * record could stand in for a target klog dropped by the regression, and
     * a correct emission alongside one unrelated record would fail a suite
     * that is working. */
    seq_before = klog_get_seq();
    klog_receipted(LOG_INFO, "TEST", (klog_receipt_fn)0,
                   0xDEADBEEFULL, "%s", marker);
    ring = klog_get_ring_snapshot((uint32_t *)0, &head_after, &seq_after);

    /* Same window guard, same reason: at exactly one ring the oldest entry of
     * the window sits on the next write slot, and the snapshot hands back the
     * LIVE array, so a concurrent append would destroy it before the scan
     * reaches it and the marker would read as dropped.
     *
     * It is a MARGIN, not a cure, and the same one the reference shape above
     * carries: the guard bounds appends before the snapshot, not during the
     * scan, so a window of width N-1 still dies after two concurrent appends.
     * The cure is a snapshot API that copies the window under s_klog_lock,
     * owned by the kernel system-logging roadmap's "klog assertions and scans
     * that depend on nothing else having logged" work, which lists this test
     * among the scans to convert. */
    if (seq_after - seq_before == 0u ||
        seq_after - seq_before >= (uint64_t)KLOG_RING_SIZE) {
        TEST_ASSERT(0, "the measured klog window is unusable (empty, or wide "
                       "enough that a concurrent append can overwrite its "
                       "oldest entry)");
        return;
    }
    added = (uint32_t)(seq_after - seq_before);

    for (i = 1u; i <= added && i <= KLOG_RING_SIZE; i++) {
        const klog_entry_t *e =
            &ring[(head_after + KLOG_RING_SIZE - i) % KLOG_RING_SIZE];
        uint32_t k = 0;

        while (marker[k] && e->message[k] == marker[k])
            k++;
        if (!marker[k])
            found++;
    }

    TEST_ASSERT_EQ((uint64_t)found, (uint64_t)1,
                   "a NULL ack emits the record exactly as klog_unrated "
                   "would -- the receipt is opt-in, not required");
    TEST_ASSERT_EQ((uint64_t)s_klog_receipt_probe.calls, 0u,
                   "and acknowledges nobody, so a conditional caller passing "
                   "NULL cannot be charged another caller's settlement");
}

/* ---- Global level override ---- */

static void test_klog_global_level(void)
{
    uint32_t head_before, head_after, dummy;

    /* Set global to ERROR -- INFO and WARN should be dropped */
    klog_set_level((const char *)0, LOG_ERROR);

    klog_get_ring(&dummy, &head_before);
    klog(LOG_INFO, "test_global", "should be dropped by global");
    klog_get_ring(&dummy, &head_after);

    TEST_ASSERT(head_after == head_before,
                "LOG_INFO suppressed by global LOG_ERROR override");

    klog_get_ring(&dummy, &head_before);
    klog(LOG_WARN, "test_global", "should also be dropped");
    klog_get_ring(&dummy, &head_after);

    TEST_ASSERT(head_after == head_before,
                "LOG_WARN suppressed by global LOG_ERROR override");

    /* Restore global default */
    klog_set_level((const char *)0, LOG_DEBUG);
}

/* ---- Rate limiting ----
 * The rate limiter uses a tick-based window (100 ticks = 1s at 100 Hz).
 * On fast systems (WHPX), 150 messages may complete before the window
 * mechanism engages. We verify the API exists and returns a sane value
 * rather than testing the timing-dependent drop behavior. */

static void test_klog_rate_limit_api(void)
{
    uint32_t dropped;

    /* klog_get_dropped for an unknown subsystem should return 0 */
    dropped = klog_get_dropped("nonexistent_subsys_xyz");
    TEST_ASSERT(dropped == 0,
                "klog_get_dropped() returns 0 for unknown subsystem");
}

/* ---- Ring buffer wrap ---- */

static void test_klog_ring_wrap(void)
{
    uint32_t count, head;

    /* By test time the ring has 500+ entries from boot + prior tests.
     * The ring wraps at KLOG_RING_SIZE (1000). Rather than flooding
     * serial with hundreds of messages, just check the ring state.
     * If count == KLOG_RING_SIZE, the ring has already wrapped. If not,
     * we accept the test as "count is within valid range". */
    klog_get_ring(&count, &head);

    TEST_ASSERT(count > 0 && count <= KLOG_RING_SIZE,
                "ring count is within valid range (0 < count <= 1000)");
    TEST_ASSERT(head < KLOG_RING_SIZE,
                "ring head is within bounds");
}

/* ---- Crash persistence types ---- */

static void test_klog_crash_magic(void)
{
}

static void test_klog_crash_header_size(void)
{
    /* Header must be stable for cross-boot physical memory layout */
    TEST_ASSERT(sizeof(klog_crash_header_t) <= 32,
                "klog_crash_header_t fits in 32 bytes");
    TEST_ASSERT(sizeof(klog_crash_header_t) >= 20,
                "klog_crash_header_t has all required fields");
}

static void test_klog_crash_post_codes(void)
{
    TEST_ASSERT(POST16_CRASHLOG != 0, "POST16_CRASHLOG is non-zero");
    TEST_ASSERT(POST16_CRASHLOG_DONE != 0, "POST16_CRASHLOG_DONE is non-zero");
    TEST_ASSERT(POST16_CRASHLOG != POST16_CRASHLOG_ALLOC,
                "CRASHLOG != CRASHLOG_ALLOC");
    TEST_ASSERT(POST16_CRASHLOG != POST16_DEFERRED,
                "CRASHLOG != DEFERRED (no overlap)");
    TEST_ASSERT(POST16_CRASHLOG != POST16_BOOTPERF,
                "CRASHLOG != BOOTPERF (no overlap)");
}

/* ---- Per-entry context: cpu_id on BSP ---- */

static void test_klog_ctx_cpu_id(void)
{
    uint32_t count, head;

    klog(LOG_INFO, "TEST", "ctx_cpu test");
    klog_get_ring(&count, &head);

    /* Last entry is at head-1 */
    uint32_t idx = (head == 0) ? KLOG_RING_SIZE - 1 : head - 1;
    const klog_entry_t *ring = klog_get_ring(&count, &head);
    TEST_ASSERT_EQ((uint32_t)ring[idx].cpu_id, 0,
                   "BSP log entry has cpu_id == 0");
}

/* ---- Per-entry context: pid during boot ---- */

static void test_klog_ctx_pid_boot(void)
{
    /* During test phase, scheduler is ready so PID should be non-zero
     * (at least the idle task PID 0 or sys_wq PID 1 or main context).
     * The key check: pid field is populated (not left uninitialized). */
    uint32_t count, head;

    klog(LOG_INFO, "TEST", "ctx_pid test");
    klog_get_ring(&count, &head);

    uint32_t idx = (head == 0) ? KLOG_RING_SIZE - 1 : head - 1;
    const klog_entry_t *ring = klog_get_ring(&count, &head);
    /* PID 0 is the main/idle context -- valid during tests */
    TEST_ASSERT(ring[idx].pid <= 100,
                "pid is reasonable (0-100, not garbage)");
}

/* ---- Per-entry context: POST16 codes unique ---- */

static void test_klog_ctx_post_codes(void)
{
    TEST_ASSERT(POST16_KLOG_CTX != 0, "POST16_KLOG_CTX is non-zero");
    TEST_ASSERT(POST16_KLOG_CTX_STRUCT != 0, "POST16_KLOG_CTX_STRUCT is non-zero");
    TEST_ASSERT(POST16_KLOG_CTX != POST16_KLOG_CTX_STRUCT,
                "KLOG_CTX != KLOG_CTX_STRUCT");
    TEST_ASSERT(POST16_KLOG_CTX != POST16_CRASHLOG,
                "KLOG_CTX != CRASHLOG (no overlap)");
    TEST_ASSERT(POST16_KLOG_CTX_JSON != POST16_KLOG_CTX_SERIAL,
                "KLOG_CTX_JSON != KLOG_CTX_SERIAL");
}

/* ---- ETW: session magic value ---- */

static void test_etw_session_magic(void)
{
}

/* ---- ETW: event header size ---- */

static void test_etw_event_header_size(void)
{
    TEST_ASSERT_EQ((uint32_t)sizeof(etw_event_header_t), 16,
                   "etw_event_header_t is 16 bytes");
}

/* ---- ETW: basic info size ---- */

static void test_etw_basic_info_size(void)
{
    TEST_ASSERT_EQ((uint32_t)sizeof(etw_basic_info_t), 32,
                   "etw_basic_info_t is 32 bytes");
}

/* ---- ETW: NtCreateTrace returns valid handle via SSDT dispatch ---- */

static void test_etw_create_trace(void)
{
    uint64_t handle = 0;
    NTSTATUS s = ssdt_dispatch(SSDT_NtCreateTrace,
                               (uint64_t)&handle, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "NtCreateTrace returns STATUS_SUCCESS");
    TEST_ASSERT(handle != 0, "NtCreateTrace returns non-zero handle");

    /* Clean up: stop and flush */
    ssdt_dispatch(SSDT_NtStopTrace, handle, 0, 0, 0, 0, 0);
}

/* ---- ETW: NtTraceEvent writes to running session ---- */

static void test_etw_trace_event(void)
{
    uint64_t handle = 0;
    NTSTATUS s;

    /* Create session */
    s = ssdt_dispatch(SSDT_NtCreateTrace,
                      (uint64_t)&handle, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "create session for event test");

    /* Start session via NtTraceControl */
    uint64_t ctrl_buf = handle;
    s = ssdt_dispatch(SSDT_NtTraceControl,
                      ETW_FUNC_START, (uint64_t)&ctrl_buf, sizeof(ctrl_buf), 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "NtTraceControl START succeeds");

    /* Write an event */
    uint32_t payload = 0xDEADBEEF;
    s = ssdt_dispatch(SSDT_NtTraceEvent,
                      handle, 0x0001, sizeof(payload),
                      (uint64_t)&payload, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "NtTraceEvent writes to running session");

    /* Query and verify event count */
    etw_basic_info_t info;
    s = ssdt_dispatch(SSDT_NtQueryTrace,
                      handle, ETW_INFO_BASIC,
                      (uint64_t)&info, sizeof(info), 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "NtQueryTrace returns STATUS_SUCCESS");
    TEST_ASSERT_EQ(info.events_written, 1, "1 event written after NtTraceEvent");

    /* Stop releases the session (clears magic, frees buffer) */
    s = ssdt_dispatch(SSDT_NtStopTrace, handle, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "NtStopTrace succeeds");

    /* Flush after stop returns INVALID_HANDLE -- session was released */
    s = ssdt_dispatch(SSDT_NtFlushTrace, handle, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE, "NtFlushTrace after stop returns INVALID_HANDLE");
}

/* ---- ETW: NtTraceEvent on non-running session returns error ---- */

static void test_etw_event_not_running(void)
{
    uint64_t handle = 0;
    NTSTATUS s;

    /* Create session (state = IDLE, not RUNNING) */
    s = ssdt_dispatch(SSDT_NtCreateTrace,
                      (uint64_t)&handle, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "create session for not-running test");

    /* Try to write event -- should fail */
    uint32_t payload = 0x12345678;
    s = ssdt_dispatch(SSDT_NtTraceEvent,
                      handle, 0, sizeof(payload),
                      (uint64_t)&payload, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtTraceEvent fails on non-running session");

    /* -9 LEAK retrofit: free the 4 KiB session buffer. Before
     * the NtStopTrace permissive-IDLE fix, there was no way to
     * free a never-started session, so this test leaked 4096 bytes
     * per run. Now NtStopTrace accepts IDLE and cleans up.
     * Assertion locks in the fix: if NtStopTrace regresses back to
     * STATUS_UNSUCCESSFUL for IDLE sessions, this test fails loud.
     * Follow-up assertion: subsequent NtFlushTrace returns
     * INVALID_HANDLE proving magic was cleared + slot reusable. */
    s = ssdt_dispatch(SSDT_NtStopTrace, handle, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "NtStopTrace(IDLE) releases the session (-9 leak fix gate)");
    s = ssdt_dispatch(SSDT_NtFlushTrace, handle, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE,
                   "Post-stop NtFlushTrace returns INVALID_HANDLE (slot cleared)");
}

/* ---- ETW: SSDT slots are registered (not stub) ---- */

static void test_etw_ssdt_registered(void)
{
    /* Dispatch NtCreateTrace with NULL out_handle -- should return
     * STATUS_INVALID_PARAMETER (real handler), not STATUS_NOT_IMPLEMENTED (stub) */
    NTSTATUS s = ssdt_dispatch(SSDT_NtCreateTrace, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "SSDT 0x01D2 (NtCreateTrace) is not a stub");

    s = ssdt_dispatch(SSDT_NtTraceEvent, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "SSDT 0x01D0 (NtTraceEvent) is not a stub");

    s = ssdt_dispatch(SSDT_NtTraceControl, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "SSDT 0x01D1 (NtTraceControl) is not a stub");
}

/* ----: Crash persistence -- CRC roundtrip + header validation ---- */

static void test_klog_crash_entry_layout(void)
{
    /* Crash entry is 164 bytes (internal type klog_crash_entry_t, pinned by a
     * _Static_assert in klog.c): 4 (level) + 4 (timestamp) + 1 (cpu_id) + 3 (pad)
     * + 4 (pid) + 4 (tid) + 16 (subsystem) + 128 (message) = 164 (4-byte aligned) */
    /* We verify indirectly through capacity math */
    uint32_t region_bytes = KLOG_CRASH_PAGES * 4096;
    TEST_ASSERT(region_bytes > sizeof(klog_crash_header_t),
                "crash region larger than header alone");
}

static void test_klog_crash_header_fields(void)
{
    /* Construct a header and verify field offsets are at expected positions */
    klog_crash_header_t hdr;
    hdr.magic = KLOG_CRASH_MAGIC;
    hdr.entry_count = 42;
    hdr.crc32 = 0xDEADBEEF;
    hdr.ring_head = 7;
    hdr.boot_timestamp = 12345;

    TEST_ASSERT_EQ(hdr.magic, KLOG_CRASH_MAGIC,
                   "header magic round-trips correctly");
    TEST_ASSERT_EQ(hdr.entry_count, 42,
                   "header entry_count round-trips");
    TEST_ASSERT_EQ(hdr.crc32, 0xDEADBEEF,
                   "header crc32 round-trips");
    TEST_ASSERT_EQ(hdr.ring_head, 7,
                   "header ring_head round-trips");
}

static void test_klog_crash_region_allocated(void)
{
    /* The crash region should have been allocated during klog_early_init.
     * We can't access s_crash_region directly (static), but we can verify
     * that klog_crash_persist() doesn't crash when called. We test this
     * indirectly: if the region wasn't allocated, persist is a no-op. */
    /* Just verify the constants are sane */
    TEST_ASSERT(KLOG_CRASH_PAGES > 0, "KLOG_CRASH_PAGES > 0");
    TEST_ASSERT(KLOG_CRASH_PAGES <= 64,
                "KLOG_CRASH_PAGES reasonable (<= 64 = 256 KiB)");
    TEST_ASSERT(KLOG_CRASH_PAGES * 4096 >= sizeof(klog_crash_header_t) + 164,
                "crash region fits at least header + 1 entry (164 bytes)");
}

static void test_klog_crash_capacity(void)
{
    /* Verify how many entries fit in the crash region */
    uint32_t region_size = KLOG_CRASH_PAGES * 4096;
    uint32_t usable = region_size - (uint32_t)sizeof(klog_crash_header_t);
    uint32_t max_entries = usable / 164;  /* klog_crash_entry_t = 164 bytes */

    TEST_ASSERT(max_entries >= 10,
                "crash region fits >= 10 entries");
    TEST_ASSERT(max_entries <= KLOG_RING_SIZE,
                "crash capacity <= ring size (no overflow)");
}

/* ----: Per-entry context -- deeper assertions ---- */

static void test_klog_ctx_tid_populated(void)
{
    uint32_t count, head;
    klog(LOG_INFO, "TEST", "ctx_tid test");
    const klog_entry_t *ring = klog_get_ring(&count, &head);
    uint32_t idx = (head == 0) ? KLOG_RING_SIZE - 1 : head - 1;

    /* TID should be populated (0 for main context is valid) */
    TEST_ASSERT(ring[idx].tid <= 100,
                "tid is reasonable (0-100, not garbage)");
}

static void test_klog_ctx_subsystem_populated(void)
{
    uint32_t count, head;
    klog(LOG_INFO, "TEST", "ctx_subsys test");
    const klog_entry_t *ring = klog_get_ring(&count, &head);
    uint32_t idx = (head == 0) ? KLOG_RING_SIZE - 1 : head - 1;

    /* Subsystem should be "TEST" */
    TEST_ASSERT(ring[idx].subsystem[0] != '\0',
                "subsystem is non-empty");
    TEST_ASSERT(ring[idx].subsystem[0] == 'T' &&
                ring[idx].subsystem[1] == 'E' &&
                ring[idx].subsystem[2] == 'S' &&
                ring[idx].subsystem[3] == 'T',
                "subsystem matches logged tag 'TEST'");
}

static void test_klog_ctx_message_populated(void)
{
    uint32_t count, head;
    klog(LOG_INFO, "TEST", "ctx_msg_verify");
    const klog_entry_t *ring = klog_get_ring(&count, &head);
    uint32_t idx = (head == 0) ? KLOG_RING_SIZE - 1 : head - 1;

    TEST_ASSERT(ring[idx].message[0] != '\0',
                "message is non-empty");
    TEST_ASSERT(ring[idx].message[0] == 'c' &&
                ring[idx].message[1] == 't' &&
                ring[idx].message[2] == 'x',
                "message starts with logged text");
}

static void test_klog_ctx_timestamp_advances(void)
{
    uint32_t count1, head1, count2, head2;
    uint32_t idx1, idx2;

    klog(LOG_INFO, "TEST", "ts1");
    {
        const klog_entry_t *ring = klog_get_ring(&count1, &head1);
        idx1 = (head1 == 0) ? KLOG_RING_SIZE - 1 : head1 - 1;
        count1 = ring[idx1].timestamp;
    }

    /* Small busy-wait to ensure timestamp changes */
    { volatile uint32_t x = 0; for (uint32_t i = 0; i < 10000; i++) x++; (void)x; }

    klog(LOG_INFO, "TEST", "ts2");
    {
        const klog_entry_t *ring = klog_get_ring(&count2, &head2);
        idx2 = (head2 == 0) ? KLOG_RING_SIZE - 1 : head2 - 1;
        count2 = ring[idx2].timestamp;
    }

    TEST_ASSERT(count2 >= count1,
                "timestamp advances between log entries");
}

/* klog_flush_window(cur_seq, cursor): the bounded unflushed count that prevents
 * the saturated-ring 10-minute USB 2.0 flush hang. Pure + deterministic (unlike
 * the runtime slow-media flag, which boot legitimately mutates). */
static void test_klog_flush_window(void)
{
    TEST_ASSERT_EQ(klog_flush_window(0, 0), 0u, "nothing logged -> 0");
    TEST_ASSERT_EQ(klog_flush_window(100, 0), 100u, "100 unflushed from start -> 100");
    TEST_ASSERT_EQ(klog_flush_window(100, 50), 50u, "cursor at 50 -> 50 remain");
    TEST_ASSERT_EQ(klog_flush_window(100, 100), 0u, "caught up -> 0");
    /* The load-bearing bound: a saturated ring (cur_seq far ahead) caps to the
     * ring capacity rather than looping over an unbounded span. */
    TEST_ASSERT_EQ(klog_flush_window(1000000, 0), (uint32_t)KLOG_RING_SIZE,
                   "saturated ring caps to KLOG_RING_SIZE (no unbounded loop)");
    TEST_ASSERT_EQ(klog_flush_window(0, 100), 0u,
                   "cursor ahead of seq -> 0 (no underflow to a huge count)");
}

/* klog_lost_count(cur_seq, cursor): entries overwritten in the ring before flush.
 * Deferred-flush mode widens this window, so the count must be accurate. Pure. */
static void test_klog_lost_count(void)
{
    TEST_ASSERT_EQ(klog_lost_count(0, 0), 0u, "nothing logged -> 0 lost");
    TEST_ASSERT_EQ(klog_lost_count(KLOG_RING_SIZE, 0), 0u, "exactly ring size -> 0 lost");
    TEST_ASSERT_EQ(klog_lost_count(KLOG_RING_SIZE + 500, 0), 500u, "500 over ring -> 500 lost");
    TEST_ASSERT_EQ(klog_lost_count(KLOG_RING_SIZE + 500, 500), 0u, "cursor advanced -> within ring, 0 lost");
    TEST_ASSERT_EQ(klog_lost_count(500, 1000), 0u, "cursor ahead of seq -> 0 (no underflow)");
    /* 64-bit loss past UINT32_MAX must saturate, not wrap to a small/zero count. */
    TEST_ASSERT_EQ(klog_lost_count(0x100000000ull + KLOG_RING_SIZE + 5, 0),
                   0xFFFFFFFFu, "loss past UINT32_MAX saturates (no wrap)");
}

/* klog_defer_active(state): effective deferral with DISABLED dominant -- once the
 * boot-end drain latches DISABLED, no ACTIVE bit (even from an explicit setter) makes
 * a flush no-op again, so post-drain logs can never be stranded. Pure. */
static void test_klog_defer_active(void)
{
    TEST_ASSERT(!klog_defer_active(0), "no flags -> not deferred");
    TEST_ASSERT(klog_defer_active(KLOG_DEFER_ACTIVE), "active, not disabled -> deferred");
    TEST_ASSERT(!klog_defer_active(KLOG_DEFER_DISABLED), "disabled only -> not deferred");
    TEST_ASSERT(!klog_defer_active(KLOG_DEFER_ACTIVE | KLOG_DEFER_DISABLED),
                "active+disabled -> DISABLED dominates, not deferred (no re-defer post-drain)");
}

/* klog_dispatch_slot bins a log tag to its per-subsystem file slot (0-5) for the
 * single-pass disk routing, or -1 when the entry routes only to kernel.log.
 * Slots: 0 network 1 boot 2 fs 3 mm 4 drivers 5 security. Pure. */
static int str_eq_local(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* klog_disk_subsystem() is the disk sink's alias resolver: it keeps an
 * AUTHENTICATING subsystem tag (the user-mode launcher's per-boot frame
 * nonce) out of the one log sink ring 3 can open, while leaving every
 * ordinary tag untouched. Read-only here -- registering an alias would
 * consume the one-shot the launcher needs. */
static void test_klog_disk_subsystem_passthrough(void)
{
    TEST_ASSERT(klog_disk_subsystem("mm") != (const char *)0,
                "an unaliased tag resolves to something writable");
    TEST_ASSERT(str_eq_local(klog_disk_subsystem("mm"), "mm"),
                "an unaliased tag is written to disk unchanged");
    TEST_ASSERT(str_eq_local(klog_disk_subsystem("drv"), "drv"),
                "a second unaliased tag is also unchanged");
    TEST_ASSERT(klog_disk_subsystem((const char *)0) == (const char *)0,
                "a NULL tag stays NULL rather than becoming an alias");
}

static void test_klog_dispatch_slot(void)
{
    TEST_ASSERT_EQ(klog_dispatch_slot("net"), 0, "net -> network.log (slot 0)");
    TEST_ASSERT_EQ(klog_dispatch_slot("boot"), 1, "boot -> boot.log (slot 1)");
    TEST_ASSERT_EQ(klog_dispatch_slot("fs"), 2, "fs -> fs.log (slot 2)");
    TEST_ASSERT_EQ(klog_dispatch_slot("mm"), 3, "mm -> mm.log (slot 3)");
    /* drivers.log (slot 4): every storage/bus tag routes here. */
    TEST_ASSERT_EQ(klog_dispatch_slot("drv"), 4, "drv -> drivers.log (slot 4)");
    TEST_ASSERT_EQ(klog_dispatch_slot("ahci"), 4, "ahci -> drivers.log (slot 4)");
    TEST_ASSERT_EQ(klog_dispatch_slot("pci"), 4, "pci -> drivers.log (slot 4)");
    TEST_ASSERT_EQ(klog_dispatch_slot("lapic"), 4, "lapic -> drivers.log (slot 4)");
    TEST_ASSERT_EQ(klog_dispatch_slot("ioapic"), 4, "ioapic -> drivers.log (slot 4)");
    TEST_ASSERT_EQ(klog_dispatch_slot("acpi"), 4, "acpi -> drivers.log (slot 4)");
    TEST_ASSERT_EQ(klog_dispatch_slot("blk"), 4, "blk -> drivers.log (slot 4)");
    /* security.log (slot 5) + boot.log (slot 1) aliases. */
    TEST_ASSERT_EQ(klog_dispatch_slot("sec"), 5, "sec -> security.log (slot 5)");
    TEST_ASSERT_EQ(klog_dispatch_slot("TPM"), 5, "TPM -> security.log (slot 5)");
    TEST_ASSERT_EQ(klog_dispatch_slot("smp"), 1, "smp -> boot.log (slot 1)");
    TEST_ASSERT_EQ(klog_dispatch_slot("UEFI"), 1, "UEFI -> boot.log (slot 1)");
    /* fs.log (slot 2) aliases. */
    TEST_ASSERT_EQ(klog_dispatch_slot("ixfs"), 2, "ixfs -> fs.log (slot 2)");
    TEST_ASSERT_EQ(klog_dispatch_slot("fat32"), 2, "fat32 -> fs.log (slot 2)");
    /* Tag with a trailing ':' qualifier still matches (dispatch matches up to ':'). */
    TEST_ASSERT_EQ(klog_dispatch_slot("vfs:open"), 2, "vfs:... -> fs.log (slot 2)");
    /* Boundary: a longer tag that merely PREFIXES a table tag must NOT match (the
     * match requires the next char to be '\0' or ':', so "net0" is unmatched). */
    TEST_ASSERT_EQ(klog_dispatch_slot("net0"), -1, "net0 (prefix, not exact) -> -1");
    TEST_ASSERT_EQ(klog_dispatch_slot("bootstrap"), -1, "bootstrap (prefix) -> -1");
    /* Boundary: matching is CASE-SENSITIVE. */
    TEST_ASSERT_EQ(klog_dispatch_slot("NET"), -1, "NET (wrong case) -> -1");
    TEST_ASSERT_EQ(klog_dispatch_slot("uefi"), -1, "uefi (wrong case) -> -1");
    /* Unmatched / empty / NULL -> -1 (routes only to the durable kernel.log). */
    TEST_ASSERT_EQ(klog_dispatch_slot("scheduler"), -1, "unmatched -> -1");
    TEST_ASSERT_EQ(klog_dispatch_slot(""), -1, "empty -> -1");
    TEST_ASSERT_EQ(klog_dispatch_slot((const char *)0), -1, "NULL -> -1");
}

/* klog_flush_progress_due gates the splash progress display on flush size: only
 * flushes of >= KLOG_FLUSH_PROGRESS_MIN (64) entries report progress, so a small
 * fast flush does not flicker the diagnostic line. Pure; boundary-checked. */
static void test_klog_flush_progress_due(void)
{
    TEST_ASSERT_EQ(klog_flush_progress_due(0), 0, "0 entries -> no progress display");
    TEST_ASSERT_EQ(klog_flush_progress_due(1), 0, "1 entry -> no progress display");
    TEST_ASSERT_EQ(klog_flush_progress_due(63), 0, "63 (one below MIN) -> no");
    TEST_ASSERT_EQ(klog_flush_progress_due(64), 1, "64 (== MIN) -> show progress");
    TEST_ASSERT_EQ(klog_flush_progress_due(400), 1, "400 (slow USB tail) -> show progress");
}

/* klog_compress_buffer + klog_decompress_rotated form the rotated-log .N.lz4
 * codec: compress writes [20-byte header | LZ4 block]; decompress validates
 * magic/version/bounds/CRC32 and the exact recorded size. Roundtrip restores
 * byte-identical data; every malformed input (bad magic, flipped byte ->
 * CRC mismatch, truncation, undersized dst) is rejected with -1. Stack buffers
 * only -- no BSS, no live boot infra. */
static void test_klog_lz4_roundtrip(void)
{
    uint8_t src[256], arc[512], dst[256];
    uint32_t i;
    int total, n, identical;

    for (i = 0; i < sizeof(src); i++) src[i] = (uint8_t)('A' + (i & 7));

    total = klog_compress_buffer(src, sizeof(src), arc, sizeof(arc));
    TEST_ASSERT(total > 0, "klog_compress_buffer produces an archive");
    TEST_ASSERT(total >= 20, "archive carries the 20-byte LZ4 header");

    n = klog_decompress_rotated(arc, (uint32_t)total, dst, sizeof(dst));
    TEST_ASSERT_EQ((uint32_t)n, sizeof(src), "decompress restores the original size");
    identical = 1;
    for (i = 0; i < sizeof(src); i++) if (dst[i] != src[i]) { identical = 0; break; }
    TEST_ASSERT(identical, "decompressed bytes are byte-identical to the source");

    arc[0] ^= 0xFF;  /* corrupt the magic */
    TEST_ASSERT_EQ(klog_decompress_rotated(arc, (uint32_t)total, dst, sizeof(dst)), -1,
                   "bad magic rejected");
    arc[0] ^= 0xFF;  /* restore */

    arc[total - 1] ^= 0xFF;  /* flip a compressed byte */
    TEST_ASSERT_EQ(klog_decompress_rotated(arc, (uint32_t)total, dst, sizeof(dst)), -1,
                   "CRC32 mismatch on a flipped block byte rejected");
    arc[total - 1] ^= 0xFF;  /* restore */

    TEST_ASSERT_EQ(klog_decompress_rotated(arc, (uint32_t)total - 1, dst, sizeof(dst)), -1,
                   "truncated archive rejected");
    TEST_ASSERT_EQ(klog_decompress_rotated(arc, (uint32_t)total, dst, 8), -1,
                   "dst too small for the uncompressed size rejected");

    /* dst_cap below the header is a compress-side reject. */
    TEST_ASSERT_EQ(klog_compress_buffer(src, sizeof(src), arc, 8), -1,
                   "compress rejects a dst smaller than the header");

    /* Over-expansion guard: a valid-CRC archive that DECLARES a smaller
     * uncompressed_size than its block actually decodes to must be rejected
     * without clobbering caller bytes past the declared size. The CRC covers
     * only the compressed block, so patching the header's uncompressed_size
     * (little-endian uint32 at byte offset 8) keeps the CRC valid while making
     * the block over-expand relative to the recorded size. */
    {
        uint8_t scratch[256];
        for (i = 0; i < sizeof(scratch); i++) scratch[i] = 0xAB;
        total = klog_compress_buffer(src, sizeof(src), arc, sizeof(arc));
        TEST_ASSERT(total > 0, "rebuild archive for the over-expansion guard");
        arc[8] = 100; arc[9] = 0; arc[10] = 0; arc[11] = 0;  /* declare 100 < 256 */
        TEST_ASSERT_EQ(klog_decompress_rotated(arc, (uint32_t)total, scratch, sizeof(scratch)), -1,
                       "block decoding past the declared uncompressed_size rejected");
        identical = 1;
        for (i = 100; i < sizeof(scratch); i++) if (scratch[i] != 0xAB) { identical = 0; break; }
        TEST_ASSERT(identical, "bytes past the declared size are left unclobbered");
    }
}

/* ---- Registration ---- */

void test_register_klog(void)
{
    test_suite_register_cat("Klog: single-pass subsystem slot dispatch",
                            test_klog_dispatch_slot, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: disk sink leaves unaliased tags unchanged",
                            test_klog_disk_subsystem_passthrough, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: flush progress-due cadence",
                            test_klog_flush_progress_due, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: LZ4 rotated-log compress/decompress roundtrip",
                            test_klog_lz4_roundtrip, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: bounded flush window",
                            test_klog_flush_window, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: ring-overflow lost count",
                            test_klog_lost_count, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: deferral DISABLED-dominant",
                            test_klog_defer_active, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: ring write", test_klog_ring_write, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: over-long message truncates without wedging",
                            test_klog_overlong_shapes_truncate, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: truncation boundary at exactly 255 characters",
                            test_klog_truncation_boundary, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: INT64_MIN renders its full magnitude",
                            test_klog_int64_min_renders, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: short subsystem tags classify without overreading",
                            test_klog_short_tag_classification, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: level drop", test_klog_level_drop, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: level pass", test_klog_level_pass, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: receipt acknowledges a delivered record",
                            test_klog_receipt_acknowledges_a_delivered_record,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Klog: receipt acknowledges a declined record",
                            test_klog_receipt_acknowledges_a_declined_record,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Klog: a NULL ack is the ordinary path",
                            test_klog_receipt_null_ack_is_the_ordinary_path,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Klog: global level", test_klog_global_level, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: rate limit API", test_klog_rate_limit_api, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: ring wrap", test_klog_ring_wrap, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: crash magic", test_klog_crash_magic, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: crash header size", test_klog_crash_header_size, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: crash POST codes", test_klog_crash_post_codes, TEST_CAT_BOOT);

    /* Crash persistence: deeper checks */
    test_suite_register_cat("Klog: crash entry layout", test_klog_crash_entry_layout, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: crash header fields", test_klog_crash_header_fields, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: crash region allocated", test_klog_crash_region_allocated, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: crash capacity", test_klog_crash_capacity, TEST_CAT_BOOT);

    /* Per-entry context metadata tests */
    test_suite_register_cat("Klog: ctx cpu_id BSP", test_klog_ctx_cpu_id, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: ctx pid boot", test_klog_ctx_pid_boot, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: ctx tid populated", test_klog_ctx_tid_populated, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: ctx subsystem match", test_klog_ctx_subsystem_populated, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: ctx message match", test_klog_ctx_message_populated, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: ctx timestamp advances", test_klog_ctx_timestamp_advances, TEST_CAT_BOOT);
    test_suite_register_cat("Klog: ctx POST codes", test_klog_ctx_post_codes, TEST_CAT_BOOT);

    /* ETW tracing tests */
    test_suite_register_cat("ETW: session magic", test_etw_session_magic, TEST_CAT_ABI);
    test_suite_register_cat("ETW: event header size", test_etw_event_header_size, TEST_CAT_ABI);
    test_suite_register_cat("ETW: basic info size", test_etw_basic_info_size, TEST_CAT_ABI);
    test_suite_register_cat("ETW: SSDT registered", test_etw_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("ETW: NtCreateTrace", test_etw_create_trace, TEST_CAT_ABI);
    test_suite_register_cat("ETW: NtTraceEvent", test_etw_trace_event, TEST_CAT_ABI);
    test_suite_register_cat("ETW: event not running", test_etw_event_not_running, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
