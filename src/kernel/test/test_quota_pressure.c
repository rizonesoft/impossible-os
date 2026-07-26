/* ============================================================================
 * test_quota_pressure.c -- Resource pressure levels, quota-failure events, and
 *                          cooperative escalation.
 *
 * Section 9 coverage: the four-level hysteresis machine (rise/fall debounce, no
 * flapping inside a band, one step per debounce), the source-validity contract
 * (an unlimited cap is UNKNOWN, never "no pressure", and never de-escalates),
 * the quota-failure diagnostic event contract end to end, the token-bucket rate
 * limit and its per-cause drop counters, and cooperative victim nomination.
 *
 * Everything here drives the state machine directly. The publication side
 * (quota_pressure_init) is deliberately NOT exercised: it creates notification
 * states and arms a threaded DPC, and tests must not call live boot
 * infrastructure. The machine is reachable without it by design, so the records
 * these tests inspect are pulled straight out of the ring.
 *
 * XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/quota/quota.h"
#include "kernel/quota/quota_pressure.h"
#include "kernel/security/sid.h"

/* A distinct resource type per test so tests cannot contaminate each other's
 * domain even if one leaves state behind. */
#define PT_A  QUOTA_RES_TIMER
#define PT_B  QUOTA_RES_SECTION
#define PT_C  QUOTA_RES_MAPPED_VIEW

/* Feed `n` samples at `permille` into `type`.
 *
 * Drives the machine through the test sample seam rather than through a live
 * charge. The production trigger derives its sample from the quota registry on
 * a time window, which is the right thing for the kernel and the wrong thing
 * for a hysteresis test: the debounce needs an exact, known sequence of
 * samples, not whatever the registry happened to hold when a window opened. */
static void pressure_feed(quota_resource_type_t type, uint16_t permille, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        quota_pressure_test_sample(type, permille);
}

/* Drain and discard whatever the ring holds. */
static void pressure_drain(void)
{
    QUOTA_PRESSURE_RECORD t;
    QUOTA_FAILURE_RECORD  f;
    while (quota_pressure_test_pop(&t, &f) != 0)
        ;
}

static SID *pressure_make_sid(uint8_t *buf, uint32_t rid)
{
    SID *sid = (SID *)buf;
    const uint8_t nt_authority[6] = { 0, 0, 0, 0, 0, 5 };

    RtlInitializeSid(sid, nt_authority, 1);
    uint32_t *sub = RtlSubAuthoritySid(sid, 0);
    if (sub)
        *sub = rid;
    return sid;
}

/* ==========================================================================
 * Saturation arithmetic
 * ========================================================================== */

/* The ratio helper is the single definition of saturation shared by the sampler
 * and the registry walk, so its boundaries are the contract both rely on. */
static void test_pressure_permille_boundaries(void)
{
    const uint64_t cap = QUOTA_PRESSURE_PERMILLE_MAX;   /* usage == permille */

    TEST_ASSERT_EQ((uint64_t)quota_pressure_permille(0, cap),
                   0ull, "zero usage is zero permille");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_permille(cap / 2, cap),
                   cap / 2, "half of the cap is half scale");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_permille(cap - 1, cap),
                   cap - 1, "one under the cap is one under full scale");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_permille(cap, cap),
                   (uint64_t)QUOTA_PRESSURE_PERMILLE_MAX,
                   "usage at the cap saturates at full scale");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_permille(cap * 5, cap),
                   (uint64_t)QUOTA_PRESSURE_PERMILLE_MAX,
                   "usage over the cap clamps to full scale, never wraps");
}

/* An unlimited cap has no ratio at all. Reporting 0 there would read as "no
 * pressure" for a resource nobody is capping -- the exact false-calm the
 * validity contract exists to prevent. */
static void test_pressure_permille_unlimited_is_invalid(void)
{
    TEST_ASSERT_EQ((uint64_t)quota_pressure_permille(0, QUOTA_LIMIT_UNLIMITED),
                   (uint64_t)QUOTA_PRESSURE_INVALID_PERMILLE,
                   "unlimited cap yields INVALID, not zero");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_permille(1ull << 40, QUOTA_LIMIT_UNLIMITED),
                   (uint64_t)QUOTA_PRESSURE_INVALID_PERMILLE,
                   "unlimited cap is INVALID regardless of usage");
}

/* The scaled form would overflow 64 bits for a huge usage, so the helper scales
 * both operands down together. The ratio must survive that, exactly. */
static void test_pressure_permille_no_overflow(void)
{
    uint64_t huge_usage = 0x2000000000000000ull;   /* 2^61 */
    uint64_t huge_limit = 0x4000000000000000ull;   /* 2^62, so the ratio is 500 */
    uint16_t p = quota_pressure_permille(huge_usage, huge_limit);

    TEST_ASSERT(p <= (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                "huge operands stay inside full scale (no wrap)");
    TEST_ASSERT_EQ((uint64_t)p, 500ull,
                   "2^61 of 2^62 is 500 permille through the long-division path");

    /* A limit outside the counter domain has no trustworthy ratio: report
     * UNKNOWN rather than approximate it, because approximating is exactly the
     * rounding that invents threshold crossings. */
    TEST_ASSERT_EQ((uint64_t)quota_pressure_permille(1ull << 62, 1ull << 63),
                   (uint64_t)QUOTA_PRESSURE_INVALID_PERMILLE,
                   "a limit past QUOTA_AMOUNT_MAX reads UNKNOWN, not a guess");
}

/* The regression the divide-first form had: rounding the DIVISOR before
 * dividing reads high, so operands just under a threshold reported a level
 * that was never reached. These are the exact values from that finding. */
static void test_pressure_permille_large_no_false_threshold(void)
{
    uint64_t usage = 21000000000000000ull;
    uint64_t limit = 30000000000000001ull;
    uint16_t p = quota_pressure_permille(usage, limit);

    TEST_ASSERT(p < QUOTA_PRESSURE_RISE_WATCH_PERMILLE,
                "a ratio just under the watch threshold does not trip it");
    TEST_ASSERT_EQ((uint64_t)p, 699ull,
                   "21e15 of 30e15+1 is 699 permille, not the 700 a divisor-first "
                   "rounding would report");
}

/* Threshold comparisons are >= to rise and < to fall. One permille either side
 * of each boundary pins that, so a later edit cannot quietly flip a bound. */
static void test_pressure_threshold_boundaries_exact(void)
{
    quota_pressure_test_reset();

    /* One under the watch rise threshold: never enters watch, however long. */
    pressure_feed(PT_A, (uint16_t)(QUOTA_PRESSURE_RISE_WATCH_PERMILLE - 1u),
                  QUOTA_PRESSURE_RISE_SAMPLES * 3u);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_NORMAL,
                   "one permille under the rise threshold never enters the level");

    /* Exactly at it: enters, because the comparison is >=. */
    pressure_feed(PT_A, QUOTA_PRESSURE_RISE_WATCH_PERMILLE,
                  QUOTA_PRESSURE_RISE_SAMPLES);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_WATCH,
                   "exactly at the rise threshold enters the level");

    /* Exactly at the fall threshold: does NOT leave, because fall is <. */
    pressure_feed(PT_A, QUOTA_PRESSURE_FALL_WATCH_PERMILLE,
                  QUOTA_PRESSURE_FALL_SAMPLES * 3u);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_WATCH,
                   "exactly at the fall threshold holds the level");

    /* One under it: leaves. */
    pressure_feed(PT_A, (uint16_t)(QUOTA_PRESSURE_FALL_WATCH_PERMILLE - 1u),
                  QUOTA_PRESSURE_FALL_SAMPLES);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_NORMAL,
                   "one permille under the fall threshold leaves the level");
}

/* ==========================================================================
 * Hysteresis
 * ========================================================================== */

/* One sample above the rise threshold is not a level change: the debounce is
 * the whole point, and a single spike must not move a consumer. */
static void test_pressure_rise_needs_debounce(void)
{
    quota_pressure_test_reset();

    pressure_feed(PT_A, QUOTA_PRESSURE_RISE_WATCH_PERMILLE,
                  QUOTA_PRESSURE_RISE_SAMPLES - 1u);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_NORMAL,
                   "one sample short of the debounce leaves the level at normal");

    pressure_feed(PT_A, QUOTA_PRESSURE_RISE_WATCH_PERMILLE, 1);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_WATCH,
                   "the sample that completes the debounce enters watch");
}

/* A saturated resource walks up one level per debounce rather than jumping to
 * critical, so every intermediate level is observable to a consumer that wanted
 * to act early. */
static void test_pressure_one_step_per_debounce(void)
{
    quota_pressure_test_reset();

    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_WATCH,
                   "full saturation reaches watch first, not critical");

    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_WARNING,
                   "the next debounce reaches warning");

    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_CRITICAL,
                   "the third debounce reaches critical");

    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES * 3u);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_CRITICAL,
                   "critical is the ceiling; further samples do not overrun it");
}

/* The band between a level's fall and rise thresholds is where a naive machine
 * flaps. Sitting in it must hold the level indefinitely, in EITHER direction. */
static void test_pressure_no_flap_inside_band(void)
{
    quota_pressure_test_reset();

    pressure_feed(PT_B, QUOTA_PRESSURE_RISE_WATCH_PERMILLE,
                  QUOTA_PRESSURE_RISE_SAMPLES);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_B),
                   (uint64_t)QUOTA_PRESSURE_WATCH, "entered watch");

    /* Inside the watch band: above the fall threshold, below the next rise. */
    uint16_t inside = (uint16_t)(QUOTA_PRESSURE_FALL_WATCH_PERMILLE + 1u);
    TEST_ASSERT(inside < QUOTA_PRESSURE_RISE_WARNING_PERMILLE,
                "the chosen sample really is inside the watch band");

    uint64_t before = quota_pressure_transition_count();
    pressure_feed(PT_B, inside, QUOTA_PRESSURE_FALL_SAMPLES * 4u);

    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_B),
                   (uint64_t)QUOTA_PRESSURE_WATCH,
                   "a long run inside the band holds the level");
    TEST_ASSERT_EQ(quota_pressure_transition_count(), before,
                   "no transition was recorded while inside the band");
}

/* De-escalation is the property that charge-only sampling would have broken:
 * usage that falls (because something was returned) must be able to walk the
 * level back down. */
static void test_pressure_falls_with_debounce(void)
{
    quota_pressure_test_reset();

    pressure_feed(PT_B, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES * 2u);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_B),
                   (uint64_t)QUOTA_PRESSURE_WARNING, "climbed to warning");

    pressure_feed(PT_B, 0, QUOTA_PRESSURE_FALL_SAMPLES - 1u);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_B),
                   (uint64_t)QUOTA_PRESSURE_WARNING,
                   "one sample short of the fall debounce holds the level");

    pressure_feed(PT_B, 0, 1);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_B),
                   (uint64_t)QUOTA_PRESSURE_WATCH,
                   "the completing sample steps down one level");

    pressure_feed(PT_B, 0, QUOTA_PRESSURE_FALL_SAMPLES);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_B),
                   (uint64_t)QUOTA_PRESSURE_NORMAL,
                   "a sustained idle run returns all the way to normal");
}

/* Rising is deliberately faster than falling. A machine with symmetric counts
 * would leave a consumer undoing its own remediation on the first dip. */
static void test_pressure_debounce_is_asymmetric(void)
{
    TEST_ASSERT(QUOTA_PRESSURE_RISE_SAMPLES < QUOTA_PRESSURE_FALL_SAMPLES,
                "a level is entered faster than it is left");
}

/* ==========================================================================
 * Source validity
 * ========================================================================== */

/* An unlimited cap marks the domain UNKNOWN. Critically, it must NOT reset the
 * debounce or lower the level: an absent measurement is not a low reading. */
static void test_pressure_invalid_sample_does_not_deescalate(void)
{
    quota_pressure_test_reset();

    pressure_feed(PT_C, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES * 2u);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_C),
                   (uint64_t)QUOTA_PRESSURE_WARNING, "climbed to warning");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_valid(PT_C), 1ull,
                   "a real ratio marks the domain valid");

    /* A run of INVALID samples SHORTER than the reset threshold: exactly what
     * an uncapped or uninstrumented source produces. */
    for (uint32_t i = 0; i < QUOTA_PRESSURE_INVALID_RESET_SAMPLES - 1u; i++)
        quota_pressure_test_sample(PT_C, QUOTA_PRESSURE_INVALID_PERMILLE);

    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_valid(PT_C), 0ull,
                   "an unlimited cap clears the validity flag");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_C),
                   (uint64_t)QUOTA_PRESSURE_WARNING,
                   "unknown samples do not de-escalate a level");
}

/* An unknown must not de-escalate, but it must not LATCH either: once the
 * subject of the measurement is gone (the capped principal exited, or its cap
 * was removed) the level has nothing left to describe and must reset, or it
 * would report a stale condition for the rest of the boot. */
static void test_pressure_sustained_unknown_resets_level(void)
{
    quota_pressure_test_reset();

    pressure_feed(PT_C, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES * 2u);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_C),
                   (uint64_t)QUOTA_PRESSURE_WARNING, "climbed to warning");

    for (uint32_t i = 0; i < QUOTA_PRESSURE_INVALID_RESET_SAMPLES; i++)
        quota_pressure_test_sample(PT_C, QUOTA_PRESSURE_INVALID_PERMILLE);

    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_C),
                   (uint64_t)QUOTA_PRESSURE_NORMAL,
                   "a sustained run of unknowns resets the level rather than latching it");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_valid(PT_C), 0ull,
                   "and the domain still reports unknown, not zero pressure");

    /* The reset must be PUBLISHED, not silent: a consumer that acted on the old
     * level needs an event saying the condition ended. */
    {
        QUOTA_PRESSURE_RECORD rec;
        QUOTA_FAILURE_RECORD  unused;
        uint32_t kind, saw_reset = 0;

        while ((kind = quota_pressure_test_pop(&rec, &unused)) != 0) {
            if (kind != QUOTA_PRESSURE_KIND_TRANSITION)
                continue;
            /* THE UNKNOWN SENTINEL MUST NOT REACH THE WIRE. from_level and
             * to_level are uint8_t fields in a published record, and
             * QUOTA_PRESSURE_UNKNOWN is 0xFF -- a value a consumer decoding
             * "0 normal .. 3 critical" has no case for. Only the two system-level
             * composites may ever answer UNKNOWN; a DOMAIN says "unmeasured" with
             * source_valid, which is exactly what this record already carries.
             * Asserted on EVERY popped transition, not just the reset, so the
             * confinement is proved for the whole stream. */
            TEST_ASSERT_EQ((uint32_t)quota_pressure_level_measured(
                               (quota_pressure_level_t)rec.from_level), 1u,
                           "a published from_level is inside the ordered band");
            TEST_ASSERT_EQ((uint32_t)quota_pressure_level_measured(
                               (quota_pressure_level_t)rec.to_level), 1u,
                           "a published to_level is inside the ordered band");
            TEST_ASSERT((uint32_t)rec.from_level < QUOTA_PRESSURE_LEVEL_COUNT &&
                        (uint32_t)rec.to_level < QUOTA_PRESSURE_LEVEL_COUNT,
                        "and both index the level-name table safely");

            if (rec.to_level == (uint8_t)QUOTA_PRESSURE_NORMAL &&
                rec.source_valid == 0)
                saw_reset = 1;
        }
        TEST_ASSERT_EQ((uint64_t)saw_reset, 1ull,
                       "the unknown-driven reset emits a transition marked source_valid=0");
    }
}

/* A never-sampled domain reports NONE rather than claiming a budget source it
 * does not have. */
static void test_pressure_unsampled_domain_reports_none(void)
{
    quota_pressure_test_reset();

    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_kind(QUOTA_RES_CRASH_BUFFER),
                   (uint64_t)QUOTA_PRESSURE_SRC_NONE,
                   "a domain nothing has sampled reports no source");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_valid(QUOTA_RES_CRASH_BUFFER),
                   0ull, "and it is not valid");
}

/* The stall seam records its own source kind, so a consumer can tell PSI-shaped
 * input from budget saturation without guessing from the domain. */
static void test_pressure_stall_seam_tags_its_source(void)
{
    quota_pressure_test_reset();

    quota_pressure_submit_stall(PT_C, 200, 10000);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_kind(PT_C),
                   (uint64_t)QUOTA_PRESSURE_SRC_STALL,
                   "a stall sample is tagged as the stall source");

    quota_pressure_test_sample(PT_C, 100);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_kind(PT_C),
                   (uint64_t)QUOTA_PRESSURE_SRC_BUDGET,
                   "a budget sample re-tags the domain as budget-sourced");
}

/* An invalid domain must not be able to lower the system answer. */
static void test_pressure_system_level_skips_invalid(void)
{
    quota_pressure_test_reset();

    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_system_level(),
                   (uint64_t)QUOTA_PRESSURE_WATCH,
                   "the system level is the worst valid domain");

    /* PT_B has never been sampled, so it is invalid; it must not pull the
     * system answer back down to normal. */
    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_valid(PT_B), 0ull,
                   "the second domain is genuinely unknown");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_system_level(),
                   (uint64_t)QUOTA_PRESSURE_WATCH,
                   "an unknown domain does not lower the system level");
}

/* Section 16: with NOTHING measured in either lane, the system level is UNKNOWN
 * rather than NORMAL. "Unmeasured" and "calm" were previously the same answer at
 * the top, which is exactly the confusion the per-domain VALID flag refuses to
 * make one level down.
 *
 * Also pins UNKNOWN out of the ORDERED band. A measured level must replace it
 * outright: if UNKNOWN were merely appended to the enum it would sort above
 * CRITICAL, so a fold written as `worse-than` would discard the first real
 * measurement and latch "unmeasured" for the rest of the boot. */
static void test_pressure_system_level_unknown_when_unmeasured(void)
{
    quota_pressure_level_t level;

    quota_pressure_test_reset();

    level = quota_pressure_system_level();
    TEST_ASSERT_EQ((uint64_t)level, (uint64_t)QUOTA_PRESSURE_UNKNOWN,
                   "with no valid domain in either lane the answer is UNKNOWN");
    TEST_ASSERT_EQ((uint32_t)quota_pressure_level_measured(level), 0u,
                   "UNKNOWN is not a measured level");
    TEST_ASSERT_EQ((uint64_t)QUOTA_PRESSURE_NORMAL, 0ull,
                   "and it is NOT normal, whose published value stays 0");

    /* One measurement, at the CALMEST level, must still replace UNKNOWN --
     * the case a bare greater-than comparison gets wrong. */
    pressure_feed(PT_A, (uint16_t)(QUOTA_PRESSURE_RISE_WATCH_PERMILLE - 1u),
                  QUOTA_PRESSURE_RISE_SAMPLES);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_valid(PT_A), 1ull,
                   "the sampled domain is now measured");
    level = quota_pressure_system_level();
    TEST_ASSERT_EQ((uint64_t)level, (uint64_t)QUOTA_PRESSURE_NORMAL,
                   "a single NORMAL measurement replaces UNKNOWN outright");
    TEST_ASSERT_EQ((uint32_t)quota_pressure_level_measured(level), 1u,
                   "and the answer is now a comparable level");

    /* Named, not rendered "?": an honest unknown and a corrupted value must not
     * print the same glyph on a dashboard. */
    TEST_ASSERT(quota_pressure_level_name(QUOTA_PRESSURE_UNKNOWN)[0] == 'u',
                "UNKNOWN has its own name");
    TEST_ASSERT(quota_pressure_level_name((quota_pressure_level_t)
                    (QUOTA_PRESSURE_LEVEL_COUNT))[0] == '?',
                "a value that is neither a level nor UNKNOWN still renders as ?");
}

/* ==========================================================================
 * Transition records
 * ========================================================================== */

/* A transition is RECORDED synchronously even though publication is deferred,
 * and it carries the full contract a consumer decodes by event id. */
static void test_pressure_transition_record_contract(void)
{
    QUOTA_PRESSURE_RECORD rec;
    QUOTA_FAILURE_RECORD  unused;
    uint32_t kind;

    quota_pressure_test_reset();
    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES);

    kind = quota_pressure_test_pop(&rec, &unused);
    TEST_ASSERT_EQ((uint64_t)kind, (uint64_t)QUOTA_PRESSURE_KIND_TRANSITION,
                   "the ring holds a transition record");
    TEST_ASSERT_EQ((uint64_t)rec.layout_version,
                   (uint64_t)QUOTA_PRESSURE_RECORD_VERSION,
                   "the record carries its layout version");
    TEST_ASSERT_EQ((uint64_t)rec.resource, (uint64_t)PT_A,
                   "the record names the domain that moved");
    TEST_ASSERT_EQ((uint64_t)rec.from_level, (uint64_t)QUOTA_PRESSURE_NORMAL,
                   "from_level is where it was");
    TEST_ASSERT_EQ((uint64_t)rec.to_level, (uint64_t)QUOTA_PRESSURE_WATCH,
                   "to_level is where it went");
    TEST_ASSERT_EQ((uint64_t)rec.source_kind, (uint64_t)QUOTA_PRESSURE_SRC_BUDGET,
                   "the record names the instrument that drove it");
    TEST_ASSERT_EQ((uint64_t)rec.source_valid, 1ull,
                   "a transition is only ever driven by a valid sample");
    TEST_ASSERT_EQ((uint64_t)rec.sample_permille,
                   (uint64_t)QUOTA_PRESSURE_PERMILLE_MAX,
                   "the completing sample is carried with the transition");
    TEST_ASSERT(rec.transition_seq > 0ull,
                "the transition sequence starts at 1, so 0 means no record");
}

/* Sequences are monotonic so a consumer can order and de-duplicate what it
 * receives from a best-effort transport. */
static void test_pressure_transition_seq_monotonic(void)
{
    QUOTA_PRESSURE_RECORD a, b;
    QUOTA_FAILURE_RECORD  unused;

    quota_pressure_test_reset();
    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES * 2u);

    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pop(&a, &unused),
                   (uint64_t)QUOTA_PRESSURE_KIND_TRANSITION, "first transition");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pop(&b, &unused),
                   (uint64_t)QUOTA_PRESSURE_KIND_TRANSITION, "second transition");
    TEST_ASSERT(b.transition_seq > a.transition_seq,
                "transition sequences strictly increase");
    TEST_ASSERT_EQ((uint64_t)a.to_level, (uint64_t)QUOTA_PRESSURE_WATCH,
                   "records arrive in the order the levels were entered");
    TEST_ASSERT_EQ((uint64_t)b.to_level, (uint64_t)QUOTA_PRESSURE_WARNING,
                   "and the second is the next level up");
}

/* ==========================================================================
 * Quota-failure event contract
 * ========================================================================== */

/* Every field a consumer needs must survive the trip through the ring: by the
 * time the record is read the counters have moved, so the record has to be
 * self-sufficient. */
static void test_pressure_failure_record_contract(void)
{
    quota_failure_source_t src;
    QUOTA_FAILURE_RECORD   rec;
    QUOTA_PRESSURE_RECORD  unused;

    quota_pressure_test_reset();

    src.type           = QUOTA_RES_HANDLE;
    src.principal      = QUOTA_PRINCIPAL_USER;
    src.block_id       = 0x1234ull;
    src.requested      = 64ull;
    src.current        = 900ull;
    src.limit          = 950ull;
    src.owner_sid_hash = 0xABCDEF01ull;
    src.owner_rid      = 4321u;
    src.result         = STATUS_QUOTA_EXCEEDED;
    src.pid            = 77u;
    src.tid            = 88u;
    src.attribution    = QUOTA_ATTRIB_BEST_EFFORT | QUOTA_ATTRIB_PID_VALID;
    quota_pressure_note_failure(&src);

    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pop(&unused, &rec),
                   (uint64_t)QUOTA_PRESSURE_KIND_FAILURE,
                   "the ring holds a failure record");
    TEST_ASSERT_EQ((uint64_t)rec.layout_version,
                   (uint64_t)QUOTA_FAILURE_RECORD_VERSION, "layout version");
    TEST_ASSERT_EQ((uint64_t)rec.resource, (uint64_t)QUOTA_RES_HANDLE, "resource");
    TEST_ASSERT_EQ((uint64_t)rec.principal, (uint64_t)QUOTA_PRINCIPAL_USER,
                   "which accounting layer refused");
    TEST_ASSERT_EQ(rec.block_id, 0x1234ull, "block identity");
    TEST_ASSERT_EQ(rec.requested, 64ull, "amount asked for");
    TEST_ASSERT_EQ(rec.current, 900ull, "usage at refusal");
    TEST_ASSERT_EQ(rec.limit, 950ull, "cap in force");
    TEST_ASSERT_EQ(rec.owner_sid_hash, 0xABCDEF01ull, "owner digest");
    TEST_ASSERT_EQ((uint64_t)rec.owner_rid, 4321ull, "owner RID");
    TEST_ASSERT_EQ((uint64_t)(uint32_t)rec.result,
                   (uint64_t)(uint32_t)STATUS_QUOTA_EXCEEDED, "result status");
    TEST_ASSERT_EQ((uint64_t)rec.pid, 77ull, "caller-captured pid round-trips");
    TEST_ASSERT_EQ((uint64_t)rec.tid, 88ull, "caller-captured tid round-trips");
    TEST_ASSERT_EQ((uint64_t)(rec.attribution & QUOTA_ATTRIB_BEST_EFFORT),
                   (uint64_t)QUOTA_ATTRIB_BEST_EFFORT,
                   "pid/tid are marked best-effort in the record itself");
    TEST_ASSERT(rec.event_seq > 0ull, "event sequence assigned");
    TEST_ASSERT_EQ(quota_pressure_events_recorded(), 1ull,
                   "exactly one event was recorded");
}

/* A refused charge through the real API must produce the event, with the
 * numbers the refusal actually saw rather than a later re-read. */
static void test_pressure_charge_refusal_emits_event(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = pressure_make_sid(buf, 9021);
    QUOTA_FAILURE_RECORD  rec;
    QUOTA_PRESSURE_RECORD unused;
    quota_block_t *block;
    uint32_t kind;

    quota_pressure_test_reset();

    block = quota_block_create(QUOTA_PRINCIPAL_PROCESS, sid, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)block, "test block created");

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(block, QUOTA_RES_TIMER, 100),
                   (uint64_t)STATUS_SUCCESS, "limit set");
    TEST_ASSERT_EQ((uint64_t)quota_charge(block, QUOTA_RES_TIMER, 90),
                   (uint64_t)STATUS_SUCCESS, "charge under the cap succeeds");

    /* Drain the transitions the two mutations above may have produced, so the
     * next pop is unambiguously the refusal. */
    pressure_drain();

    TEST_ASSERT_EQ((uint64_t)quota_charge(block, QUOTA_RES_TIMER, 50),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "charge over the cap refused");

    kind = quota_pressure_test_pop(&unused, &rec);
    TEST_ASSERT_EQ((uint64_t)kind, (uint64_t)QUOTA_PRESSURE_KIND_FAILURE,
                   "the refusal produced a failure event");
    TEST_ASSERT_EQ((uint64_t)rec.resource, (uint64_t)QUOTA_RES_TIMER,
                   "the event names the refused resource");
    TEST_ASSERT_EQ(rec.requested, 50ull, "the event carries the requested amount");
    TEST_ASSERT_EQ(rec.current, 90ull,
                   "the event carries usage as it stood at the refusal");
    TEST_ASSERT_EQ(rec.limit, 100ull, "the event carries the cap in force");
    TEST_ASSERT_EQ(rec.block_id, quota_block_id(block),
                   "the event names the block that refused");
    TEST_ASSERT_EQ((uint64_t)rec.owner_rid, 9021ull,
                   "the event carries the owner RID");
    TEST_ASSERT(rec.owner_sid_hash != 0ull,
                "an owned block produces a non-zero SID digest");

    quota_block_deref(block);
}

/* A successful charge is not a failure. The event stream must not be polluted
 * with the normal path. */
static void test_pressure_success_emits_no_event(void)
{
    quota_block_t *block;

    quota_pressure_test_reset();

    block = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)block, "test block created");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(block, QUOTA_RES_THREAD, 1000),
                   (uint64_t)STATUS_SUCCESS, "limit set");
    TEST_ASSERT_EQ((uint64_t)quota_charge(block, QUOTA_RES_THREAD, 10),
                   (uint64_t)STATUS_SUCCESS, "charge succeeds");

    TEST_ASSERT_EQ(quota_pressure_events_recorded(), 0ull,
                   "a successful charge records no failure event");

    quota_block_deref(block);
}

/* ==========================================================================
 * Rate limit and drop accounting
 * ========================================================================== */

/* The bucket admits a burst then throttles, and the throttled events are
 * COUNTED. A silent throttle would make a gap in the sequence unexplainable. */
static void test_pressure_rate_limit_counts_drops(void)
{
    quota_failure_source_t src;

    quota_pressure_test_reset();
    quota_pressure_test_refill_tokens();

    src.type           = QUOTA_RES_ALPC_MESSAGE;
    src.principal      = QUOTA_PRINCIPAL_PROCESS;
    src.block_id       = 7ull;
    src.requested      = 1ull;
    src.current        = 10ull;
    src.limit          = 10ull;
    src.owner_sid_hash = 0ull;
    src.owner_rid      = 0u;
    src.result         = STATUS_QUOTA_EXCEEDED;
    src.pid            = 0u;
    src.tid            = 0u;
    src.attribution    = QUOTA_ATTRIB_BEST_EFFORT;

    /* Exactly the burst: every one admitted. */
    for (uint32_t i = 0; i < QUOTA_FAILURE_EVENT_BURST; i++)
        quota_pressure_note_failure(&src);

    TEST_ASSERT_EQ(quota_pressure_dropped_ratelimit(), 0ull,
                   "a burst up to the bucket size is admitted whole");
    TEST_ASSERT_EQ(quota_pressure_events_recorded(),
                   (uint64_t)QUOTA_FAILURE_EVENT_BURST,
                   "every admitted event was recorded");

    /* One past the burst, within the same window: throttled and counted. */
    quota_pressure_note_failure(&src);
    TEST_ASSERT_EQ(quota_pressure_dropped_ratelimit(), 1ull,
                   "the first event past the burst is counted as a rate drop");
    TEST_ASSERT_EQ(quota_pressure_events_recorded(),
                   (uint64_t)QUOTA_FAILURE_EVENT_BURST,
                   "and it was not recorded");
}

/* The sequence advances for a dropped event too, so a consumer seeing a gap
 * knows something was dropped instead of guessing. */
static void test_pressure_dropped_event_still_advances_seq(void)
{
    quota_failure_source_t src;
    QUOTA_FAILURE_RECORD   first, second;
    QUOTA_PRESSURE_RECORD  unused;

    quota_pressure_test_reset();
    quota_pressure_test_refill_tokens();

    src.type           = QUOTA_RES_PROCESS;
    src.principal      = QUOTA_PRINCIPAL_JOB;
    src.block_id       = 11ull;
    src.requested      = 1ull;
    src.current        = 5ull;
    src.limit          = 5ull;
    src.owner_sid_hash = 0ull;
    src.owner_rid      = 0u;
    src.result         = STATUS_QUOTA_EXCEEDED;
    src.pid            = 0u;
    src.tid            = 0u;
    src.attribution    = QUOTA_ATTRIB_BEST_EFFORT;

    quota_pressure_note_failure(&src);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pop(&unused, &first),
                   (uint64_t)QUOTA_PRESSURE_KIND_FAILURE, "first event recorded");

    /* Exhaust the bucket, then admit one more after a manual refill. */
    for (uint32_t i = 0; i < QUOTA_FAILURE_EVENT_BURST + 4u; i++)
        quota_pressure_note_failure(&src);
    TEST_ASSERT(quota_pressure_dropped_ratelimit() > 0ull,
                "the bucket ran dry and drops were counted");
    pressure_drain();

    quota_pressure_test_refill_tokens();
    quota_pressure_note_failure(&src);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pop(&unused, &second),
                   (uint64_t)QUOTA_PRESSURE_KIND_FAILURE, "later event recorded");

    TEST_ASSERT(second.event_seq > first.event_seq + 1ull,
                "the sequence skipped over the dropped events, making the gap visible");
}

/* A full ring is a different failure from a throttled producer, and the two
 * counters must not be conflated: one says the publisher fell behind, the other
 * says the producer was deliberately capped. */
static void test_pressure_ring_overflow_counted_separately(void)
{
    quota_failure_source_t src;

    quota_pressure_test_reset();

    src.type           = QUOTA_RES_SECTION;
    src.principal      = QUOTA_PRINCIPAL_PROCESS;
    src.block_id       = 3ull;
    src.requested      = 1ull;
    src.current        = 1ull;
    src.limit          = 1ull;
    src.owner_sid_hash = 0ull;
    src.owner_rid      = 0u;
    src.result         = STATUS_QUOTA_EXCEEDED;
    src.pid            = 0u;
    src.tid            = 0u;
    src.attribution    = QUOTA_ATTRIB_BEST_EFFORT;

    /* Refill between batches so the BUCKET never throttles: the only limit
     * this test may hit is the ring's capacity. */
    for (uint32_t i = 0; i < QUOTA_PRESSURE_RING_SLOTS + 8u; i++) {
        quota_pressure_test_refill_tokens();
        quota_pressure_note_failure(&src);
    }

    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pending(),
                   (uint64_t)QUOTA_PRESSURE_RING_SLOTS,
                   "the ring holds exactly its capacity, never more");
    TEST_ASSERT_EQ(quota_pressure_dropped_ring(), 8ull,
                   "the overflow is counted as a ring drop");
    TEST_ASSERT_EQ(quota_pressure_dropped_ratelimit(), 0ull,
                   "and not misattributed to the rate limit");
}

/* ==========================================================================
 * Cooperative escalation
 * ========================================================================== */

/* With nobody near a cap, nomination returns an explicit CLEAR rather than a
 * zeroed record a consumer could mistake for a nominee. */
static void test_pressure_nomination_clear_when_nobody_qualifies(void)
{
    QUOTA_NOMINATION_RECORD nom;
    int found;

    quota_pressure_test_reset();

    found = quota_pressure_nominate(QUOTA_RES_CRASH_BUFFER, &nom);
    TEST_ASSERT_EQ((uint64_t)found, 0ull, "no principal qualifies");
    TEST_ASSERT_EQ((uint64_t)nom.valid, 0ull,
                   "the record is an explicit clear, not a silent zero");
    TEST_ASSERT_EQ((uint64_t)nom.layout_version,
                   (uint64_t)QUOTA_NOMINATION_RECORD_VERSION,
                   "a clear still carries its layout version");
    TEST_ASSERT(nom.nomination_seq > 0ull,
                "a clear is sequenced so a consumer can order it against a nominee");
}

/* A saturated USER principal is nominated by identity, with the saturation that
 * qualified it and an expiry after which the nomination must be re-derived. */
static void test_pressure_nominates_saturated_user(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = pressure_make_sid(buf, 9107);
    QUOTA_NOMINATION_RECORD nom;
    quota_block_t *user;
    int found;

    quota_pressure_test_reset();

    user = quota_user_block_acquire(sid, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)user, "user block acquired");

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(user, QUOTA_RES_CRASH_BUFFER, 100),
                   (uint64_t)STATUS_SUCCESS, "limit set");
    TEST_ASSERT_EQ((uint64_t)quota_charge(user, QUOTA_RES_CRASH_BUFFER, 99),
                   (uint64_t)STATUS_SUCCESS, "charged to 990 permille");

    found = quota_pressure_nominate(QUOTA_RES_CRASH_BUFFER, &nom);
    TEST_ASSERT_EQ((uint64_t)found, 1ull, "the saturated principal qualifies");
    TEST_ASSERT_EQ((uint64_t)nom.valid, 1ull, "the record names a nominee");
    TEST_ASSERT_EQ(nom.block_id, quota_block_id(user),
                   "the nominee is the block that is over budget");
    TEST_ASSERT_EQ((uint64_t)nom.owner_rid, 9107ull, "the nominee's owner RID");
    TEST_ASSERT_EQ((uint64_t)nom.principal, (uint64_t)QUOTA_PRINCIPAL_USER,
                   "nomination is by user principal");
    TEST_ASSERT_EQ((uint64_t)nom.over_permille, 990ull,
                   "the saturation that qualified the nominee is carried");
    TEST_ASSERT(nom.expires_at_ns > nom.nominated_at_ns,
                "a nominee carries an expiry, so a stale nomination is detectable");

    /* Returning the charge must retract the nomination: a consumer acting on a
     * principal that has since freed its budget is exactly the stale-record
     * hazard the expiry and the clear exist to prevent. */
    TEST_ASSERT_EQ((uint64_t)quota_return(user, QUOTA_RES_CRASH_BUFFER, 99),
                   (uint64_t)STATUS_SUCCESS, "charge returned");
    found = quota_pressure_nominate(QUOTA_RES_CRASH_BUFFER, &nom);
    TEST_ASSERT_EQ((uint64_t)found, 0ull,
                   "a principal back under the threshold is no longer nominated");
    TEST_ASSERT_EQ((uint64_t)nom.valid, 0ull, "and the record is an explicit clear");

    quota_block_deref(user);
}

/* An uncapped principal has no ratio, so it can never be "furthest over" and
 * must not be nominated no matter how much it holds. */
static void test_pressure_nomination_skips_uncapped(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = pressure_make_sid(buf, 9108);
    QUOTA_NOMINATION_RECORD nom;
    quota_block_t *user;

    quota_pressure_test_reset();

    user = quota_user_block_acquire(sid, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)user, "user block acquired");

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(user, QUOTA_RES_MAPPED_VIEW,
                                             QUOTA_LIMIT_UNLIMITED),
                   (uint64_t)STATUS_SUCCESS, "cap removed");
    TEST_ASSERT_EQ((uint64_t)quota_charge(user, QUOTA_RES_MAPPED_VIEW, 100000),
                   (uint64_t)STATUS_SUCCESS, "large charge admitted (no cap)");

    TEST_ASSERT_EQ((uint64_t)quota_pressure_nominate(QUOTA_RES_MAPPED_VIEW, &nom),
                   0ull, "an uncapped principal is never nominated");
    TEST_ASSERT_EQ((uint64_t)nom.valid, 0ull, "the record is a clear");

    TEST_ASSERT_EQ((uint64_t)quota_return(user, QUOTA_RES_MAPPED_VIEW, 100000),
                   (uint64_t)STATUS_SUCCESS, "charge returned");
    quota_block_deref(user);
}

/* ==========================================================================
 * Diagnostics
 * ========================================================================== */

/* The dashboard must be a pure READER: rendering the table cannot be allowed to
 * disturb the levels or counters an operator is trying to read. */
static void test_pressure_dump_is_read_only(void)
{
    uint64_t transitions_before;
    uint8_t  level_before;

    quota_pressure_test_reset();
    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES);

    level_before       = (uint8_t)quota_pressure_level(PT_A);
    transitions_before = quota_pressure_transition_count();

    quota_pressure_dump();

    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A), (uint64_t)level_before,
                   "dumping the table does not move a level");
    TEST_ASSERT_EQ(quota_pressure_transition_count(), transitions_before,
                   "dumping the table records no transition");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_valid(PT_A), 1ull,
                   "and does not disturb the validity flag");
}

/* Publication is armed at Phase 3 by boot code, not by a test. What a test can
 * assert is that the machine does not require it: every record above was
 * produced with the transport unarmed. */
static void test_pressure_records_without_transport(void)
{
    quota_pressure_test_reset();
    pressure_feed(PT_B, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES);

    TEST_ASSERT_EQ(quota_pressure_transition_count(), 1ull,
                   "a transition is recorded whether or not transport is armed");
    TEST_ASSERT(quota_pressure_test_pending() > 0u,
                "and the record is waiting in the ring for the publisher");
}


/* ==========================================================================
 * Coherent domain sampling, ring and drain mechanics
 * ========================================================================== */

/* The defect this design replaced: with each block pushing its OWN ratio into a
 * shared domain, a run of low samples from an unrelated principal walked the
 * level down while the saturated one stayed pinned at its cap. The domain now
 * derives one coherent sample -- the worst live user -- so an idle second user
 * cannot mask a saturated first one. */
static void test_pressure_domain_tracks_worst_principal(void)
{
    uint8_t buf_hot[SID_MAX_SIZE], buf_idle[SID_MAX_SIZE];
    SID *sid_hot  = pressure_make_sid(buf_hot, 9201);
    SID *sid_idle = pressure_make_sid(buf_idle, 9202);
    quota_block_t *hot, *idle;
    uint64_t block_id = 0, sid_hash = 0;
    uint32_t rid = 0;
    uint16_t permille = 0;

    quota_pressure_test_reset();

    hot  = quota_user_block_acquire(sid_hot, SID_MAX_SIZE);
    idle = quota_user_block_acquire(sid_idle, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)hot, "saturated user block acquired");
    TEST_ASSERT_NOT_NULL((void *)idle, "idle user block acquired");

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(hot, QUOTA_RES_CRASH_BUFFER, 100),
                   (uint64_t)STATUS_SUCCESS, "hot user capped");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(idle, QUOTA_RES_CRASH_BUFFER, 100),
                   (uint64_t)STATUS_SUCCESS, "idle user capped");
    TEST_ASSERT_EQ((uint64_t)quota_charge(hot, QUOTA_RES_CRASH_BUFFER, 98),
                   (uint64_t)STATUS_SUCCESS, "hot user at 980 permille");

    /* The idle user holds nothing, so a per-block sampler would see 0 from it
     * and pull the domain down. The derived sample must report the hot one. */
    TEST_ASSERT_EQ((uint64_t)quota_registry_worst_user(QUOTA_RES_CRASH_BUFFER, 0,
                                                       &block_id, &rid,
                                                       &sid_hash, &permille),
                   1ull, "a capped principal is found");
    TEST_ASSERT_EQ((uint64_t)permille, 980ull,
                   "the domain sample is the WORST principal, not the last one");
    TEST_ASSERT_EQ(block_id, quota_block_id(hot),
                   "and it identifies the saturated principal");
    TEST_ASSERT_EQ((uint64_t)rid, 9201ull, "with that principal's RID");

    TEST_ASSERT_EQ((uint64_t)quota_return(hot, QUOTA_RES_CRASH_BUFFER, 98),
                   (uint64_t)STATUS_SUCCESS, "charge returned");
    quota_block_deref(hot);
    quota_block_deref(idle);
}

/* An empty registry (or one with no capped principal for this resource) is not
 * "zero pressure"; it is no measurement at all. */
static void test_pressure_worst_user_none_when_uncapped(void)
{
    uint64_t block_id = 1, sid_hash = 1;
    uint32_t rid = 1;
    uint16_t permille = 1;

    quota_pressure_test_reset();

    TEST_ASSERT_EQ((uint64_t)quota_registry_worst_user(QUOTA_RES_NAMESPACE_ENTRY,
                                                       QUOTA_PRESSURE_RISE_WATCH_PERMILLE,
                                                       &block_id, &rid,
                                                       &sid_hash, &permille),
                   0ull, "no principal qualifies above the threshold");
    TEST_ASSERT_EQ(block_id, 0ull, "the out-params are cleared, not left stale");
    TEST_ASSERT_EQ((uint64_t)rid, 0ull, "RID cleared");
    TEST_ASSERT_EQ((uint64_t)permille, 0ull, "saturation cleared");
}

/* A limit change moves saturation without any charge, so it must force a
 * sample rather than wait for the window -- otherwise lowering a cap would go
 * unobserved until an unrelated mutation happened by. */
static void test_pressure_set_limit_drives_a_sample(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID *sid = pressure_make_sid(buf, 9203);
    quota_block_t *user;

    quota_pressure_test_reset();

    user = quota_user_block_acquire(sid, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)user, "user block acquired");

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(user, QUOTA_RES_NAMESPACE_ENTRY, 1000),
                   (uint64_t)STATUS_SUCCESS, "generous cap set");
    TEST_ASSERT_EQ((uint64_t)quota_charge(user, QUOTA_RES_NAMESPACE_ENTRY, 990),
                   (uint64_t)STATUS_SUCCESS, "charged");

    /* Lowering the cap under the existing usage saturates it instantly. */
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(user, QUOTA_RES_NAMESPACE_ENTRY, 991),
                   (uint64_t)STATUS_SUCCESS, "cap lowered under usage");

    /* set_limit MARKS the domain; the sampler is what measures it. */
    quota_pressure_sample_all();

    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_valid(QUOTA_RES_NAMESPACE_ENTRY),
                   1ull, "the sample after the limit change marked the domain valid");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_kind(QUOTA_RES_NAMESPACE_ENTRY),
                   (uint64_t)QUOTA_PRESSURE_SRC_BUDGET,
                   "and tagged it as budget-sourced");

    TEST_ASSERT_EQ((uint64_t)quota_return(user, QUOTA_RES_NAMESPACE_ENTRY, 990),
                   (uint64_t)STATUS_SUCCESS, "charge returned");
    quota_block_deref(user);
}

/* A transition must survive a ring that a failure burst has already filled:
 * the LEVEL is the one fact a consumer cannot reconstruct from anything else. */
static void test_pressure_transition_survives_full_ring(void)
{
    quota_failure_source_t src;
    QUOTA_PRESSURE_RECORD  rec;
    QUOTA_FAILURE_RECORD   fail;
    uint32_t kind;
    int      saw_transition = 0;

    quota_pressure_test_reset();

    src.type = QUOTA_RES_THREAD;   src.principal = QUOTA_PRINCIPAL_PROCESS;
    src.block_id = 5ull;           src.requested = 1ull;
    src.current = 1ull;            src.limit = 1ull;
    src.owner_sid_hash = 0ull;     src.owner_rid = 0u;
    src.result = STATUS_QUOTA_EXCEEDED;
    src.pid = 0u; src.tid = 0u; src.attribution = QUOTA_ATTRIB_BEST_EFFORT;

    /* Fill the ring completely with failure events. */
    for (uint32_t i = 0; i < QUOTA_PRESSURE_RING_SLOTS; i++) {
        quota_pressure_test_refill_tokens();
        quota_pressure_note_failure(&src);
    }
    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pending(),
                   (uint64_t)QUOTA_PRESSURE_RING_SLOTS, "ring is full");

    /* Now force a transition. The ring cannot take it. */
    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_WATCH, "the level still moved");

    /* Drain everything: the latched transition must come out, so the level was
     * never lost even though the ring refused it. */
    quota_pressure_test_drain();
    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pending(), 0ull,
                   "the drain emptied the ring");

    /* A second transition now fits in the emptied ring and is popped normally,
     * proving the latch did not permanently displace the ring path. */
    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES);
    while ((kind = quota_pressure_test_pop(&rec, &fail)) != 0) {
        if (kind == QUOTA_PRESSURE_KIND_TRANSITION)
            saw_transition = 1;
    }
    TEST_ASSERT_EQ((uint64_t)saw_transition, 1ull,
                   "a later transition goes through the ring again");
}

/* The ring is circular: a partial drain followed by more pushes must wrap
 * without losing or duplicating a slot. */
static void test_pressure_ring_wraps_after_partial_drain(void)
{
    quota_failure_source_t src;
    QUOTA_FAILURE_RECORD   rec;
    QUOTA_PRESSURE_RECORD  unused;
    uint64_t first_seq = 0, last_seq = 0;

    quota_pressure_test_reset();

    src.type = QUOTA_RES_OBJECT_BODY; src.principal = QUOTA_PRINCIPAL_PROCESS;
    src.block_id = 9ull;              src.requested = 1ull;
    src.current = 1ull;               src.limit = 1ull;
    src.owner_sid_hash = 0ull;        src.owner_rid = 0u;
    src.result = STATUS_QUOTA_EXCEEDED;
    src.pid = 0u; src.tid = 0u; src.attribution = QUOTA_ATTRIB_BEST_EFFORT;

    /* Fill, drain most of it, then push enough to wrap the head past the end. */
    for (uint32_t i = 0; i < QUOTA_PRESSURE_RING_SLOTS; i++) {
        quota_pressure_test_refill_tokens();
        quota_pressure_note_failure(&src);
    }
    for (uint32_t i = 0; i < QUOTA_PRESSURE_RING_SLOTS - 4u; i++)
        (void)quota_pressure_test_pop(&unused, &rec);

    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pending(), 4ull,
                   "four records left before the wrap");

    for (uint32_t i = 0; i < 20u; i++) {
        quota_pressure_test_refill_tokens();
        quota_pressure_note_failure(&src);
    }
    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pending(), 24ull,
                   "the ring wrapped and accepted every push");

    /* Sequences must still come out strictly increasing across the wrap. */
    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pop(&unused, &rec),
                   (uint64_t)QUOTA_PRESSURE_KIND_FAILURE, "popped across the wrap");
    first_seq = rec.event_seq;
    while (quota_pressure_test_pop(&unused, &rec) != 0)
        last_seq = rec.event_seq;
    TEST_ASSERT(last_seq > first_seq,
                "records come out in push order across the wrap, not shuffled");
}

/* The publish path itself, run synchronously. Both record kinds must be
 * consumed and the ring left empty.
 *
 * This deliberately does NOT assert that a transport drop was counted. The
 * suite runs in a booted kernel where the notification states already exist, so
 * the publish SUCCEEDS here; the drop branch is the pre-init case, which a test
 * cannot stage without calling the init it is forbidden to call. What is
 * checkable is that the counter is monotone and the drain is exhaustive. */
static void test_pressure_drain_consumes_every_record(void)
{
    quota_failure_source_t src;
    uint64_t before;

    quota_pressure_test_reset();

    pressure_feed(PT_B, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES);

    src.type = QUOTA_RES_TIMER;       src.principal = QUOTA_PRINCIPAL_PROCESS;
    src.block_id = 12ull;             src.requested = 1ull;
    src.current = 1ull;               src.limit = 1ull;
    src.owner_sid_hash = 0ull;        src.owner_rid = 0u;
    src.result = STATUS_QUOTA_EXCEEDED;
    src.pid = 0u; src.tid = 0u; src.attribution = QUOTA_ATTRIB_BEST_EFFORT;
    quota_pressure_note_failure(&src);

    TEST_ASSERT(quota_pressure_test_pending() >= 2u,
                "a transition and a failure event are both queued");

    before = quota_pressure_dropped_transport();
    quota_pressure_test_drain();

    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pending(), 0ull,
                   "the drain consumed both record kinds and emptied the ring");
    TEST_ASSERT(quota_pressure_dropped_transport() >= before,
                "the transport-drop counter is monotone across a drain");
}

/* An ownerless block has no SID to digest, and must produce zeros rather than
 * reading past a buffer that was never filled. */
static void test_pressure_digest_ownerless_block(void)
{
    quota_block_t *block;
    QUOTA_FAILURE_RECORD  rec;
    QUOTA_PRESSURE_RECORD unused;

    quota_pressure_test_reset();

    block = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)block, "ownerless block created");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(block, QUOTA_RES_TIMER, 1),
                   (uint64_t)STATUS_SUCCESS, "cap set");
    TEST_ASSERT_EQ((uint64_t)quota_charge(block, QUOTA_RES_TIMER, 5),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "charge refused");

    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pop(&unused, &rec),
                   (uint64_t)QUOTA_PRESSURE_KIND_FAILURE, "refusal recorded");
    TEST_ASSERT_EQ(rec.owner_sid_hash, 0ull,
                   "an ownerless block digests to zero, not to garbage");
    TEST_ASSERT_EQ((uint64_t)rec.owner_rid, 0ull, "and carries no RID");
    TEST_ASSERT_EQ(rec.block_id, quota_block_id(block),
                   "identity still comes from the block id");

    quota_block_deref(block);
}

/* The RID is the LAST sub-authority, not the first. A multi-subauthority SID is
 * what distinguishes the two. */
static void test_pressure_digest_multi_subauthority_sid(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID *sid = (SID *)buf;
    const uint8_t nt_authority[6] = { 0, 0, 0, 0, 0, 5 };
    quota_block_t *block;
    QUOTA_FAILURE_RECORD  rec;
    QUOTA_PRESSURE_RECORD unused;

    quota_pressure_test_reset();

    RtlInitializeSid(sid, nt_authority, 3);
    uint32_t *s0 = RtlSubAuthoritySid(sid, 0);
    uint32_t *s1 = RtlSubAuthoritySid(sid, 1);
    uint32_t *s2 = RtlSubAuthoritySid(sid, 2);
    TEST_ASSERT_NOT_NULL((void *)s0, "sub-authority 0 addressable");
    TEST_ASSERT_NOT_NULL((void *)s1, "sub-authority 1 addressable");
    TEST_ASSERT_NOT_NULL((void *)s2, "sub-authority 2 addressable");
    *s0 = 21u; *s1 = 1000u; *s2 = 7777u;

    block = quota_block_create(QUOTA_PRINCIPAL_PROCESS, sid, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)block, "block with a 3-subauthority SID created");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(block, QUOTA_RES_SECTION, 1),
                   (uint64_t)STATUS_SUCCESS, "cap set");
    TEST_ASSERT_EQ((uint64_t)quota_charge(block, QUOTA_RES_SECTION, 9),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "charge refused");

    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pop(&unused, &rec),
                   (uint64_t)QUOTA_PRESSURE_KIND_FAILURE, "refusal recorded");
    TEST_ASSERT_EQ((uint64_t)rec.owner_rid, 7777ull,
                   "the RID is the LAST sub-authority, not the first");
    TEST_ASSERT(rec.owner_sid_hash != 0ull, "the full SID digests to non-zero");

    quota_block_deref(block);
}

/* An arithmetic-overflow refusal is a different failure from a quota refusal
 * and must be reported as itself rather than vanishing into a deferred count. */
static void test_pressure_overflow_refusal_emits_event(void)
{
    quota_block_t *block;
    QUOTA_FAILURE_RECORD  rec;
    QUOTA_PRESSURE_RECORD unused;

    quota_pressure_test_reset();

    block = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)block, "block created");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(block, QUOTA_RES_PAGED_POOL,
                                             QUOTA_LIMIT_UNLIMITED),
                   (uint64_t)STATUS_SUCCESS, "uncapped so the limit cannot refuse");

    /* Poke usage to the top of the domain so the next add cannot be represented. */
    quota_test_poke_usage(block, QUOTA_RES_PAGED_POOL, QUOTA_AMOUNT_MAX);
    pressure_drain();

    TEST_ASSERT_EQ((uint64_t)quota_charge(block, QUOTA_RES_PAGED_POOL, 1000),
                   (uint64_t)STATUS_INTEGER_OVERFLOW, "charge overflows the domain");

    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pop(&unused, &rec),
                   (uint64_t)QUOTA_PRESSURE_KIND_FAILURE,
                   "an overflow refusal produces a record too");
    TEST_ASSERT_EQ((uint64_t)(uint32_t)rec.result,
                   (uint64_t)(uint32_t)STATUS_INTEGER_OVERFLOW,
                   "and reports its own status, not a quota refusal");

    quota_test_poke_usage(block, QUOTA_RES_PAGED_POOL, 0);
    quota_block_deref(block);
}

/* A saturated principal reaching critical must NOMINATE, not merely log. This
 * is the check that keeps the escalation path from decaying into dead code. */
static void test_pressure_critical_publishes_nomination(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID *sid = pressure_make_sid(buf, 9204);
    quota_block_t *user;
    QUOTA_NOMINATION_RECORD nom;

    quota_pressure_test_reset();

    user = quota_user_block_acquire(sid, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)user, "user block acquired");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(user, QUOTA_RES_REGISTRY_BYTES, 100),
                   (uint64_t)STATUS_SUCCESS, "cap set");
    TEST_ASSERT_EQ((uint64_t)quota_charge(user, QUOTA_RES_REGISTRY_BYTES, 99),
                   (uint64_t)STATUS_SUCCESS, "charged to 990 permille");

    /* Walk the domain to critical through the real machine. */
    pressure_feed(QUOTA_RES_REGISTRY_BYTES, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES * 3u);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(QUOTA_RES_REGISTRY_BYTES),
                   (uint64_t)QUOTA_PRESSURE_CRITICAL, "domain reached critical");

    /* The drain is what runs escalation; it must find the saturated principal. */
    quota_pressure_test_drain();

    TEST_ASSERT_EQ((uint64_t)quota_pressure_nominate(QUOTA_RES_REGISTRY_BYTES, &nom),
                   1ull, "the saturated principal is nominated at critical");
    TEST_ASSERT_EQ(nom.block_id, quota_block_id(user),
                   "and the nominee is the principal that is over budget");

    TEST_ASSERT_EQ((uint64_t)quota_return(user, QUOTA_RES_REGISTRY_BYTES, 99),
                   (uint64_t)STATUS_SUCCESS, "charge returned");
    quota_block_deref(user);
}

/* Runs last: hands the ring back to the real publisher, which the first reset
 * in this suite took away. */
static void test_pressure_release_transport(void)
{
    quota_pressure_test_release();

    /* Also the one thing only a booted kernel can prove: init got past the
     * dpc_worker_started gate and armed the real transport. A test cannot
     * call the init, but it can check the result. */
    TEST_ASSERT_EQ((uint64_t)quota_pressure_ready(), 1ull,
                   "boot armed the pressure transport (DPC worker was running)");
    TEST_ASSERT_EQ((uint64_t)quota_pressure_test_pending(), 0ull,
                   "no suite records are left behind for the live publisher");
}


/* The defect a mutation-driven sampler had: rising needs three consecutive
 * samples, but a principal that charges to its cap once and then goes quiet
 * produces exactly ONE mark. Without a periodic sampler its level could never
 * rise at all -- the most ordinary pressure case there is. */
static void test_pressure_stable_load_still_escalates(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID *sid = pressure_make_sid(buf, 9205);
    quota_block_t *user;

    quota_pressure_test_reset();

    user = quota_user_block_acquire(sid, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)user, "user block acquired");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(user, QUOTA_RES_MAPPED_VIEW, 100),
                   (uint64_t)STATUS_SUCCESS, "cap set");

    /* ONE mutation, then nothing. This is the whole point. */
    TEST_ASSERT_EQ((uint64_t)quota_charge(user, QUOTA_RES_MAPPED_VIEW, 99),
                   (uint64_t)STATUS_SUCCESS, "charged to 990 permille and went quiet");

    /* The sampler keeps measuring while a debounce is in flight. */
    for (uint32_t i = 0; i < QUOTA_PRESSURE_RISE_SAMPLES; i++)
        quota_pressure_sample_all();

    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(QUOTA_RES_MAPPED_VIEW),
                   (uint64_t)QUOTA_PRESSURE_WATCH,
                   "steady pressure completes its debounce with no further mutations");

    TEST_ASSERT_EQ((uint64_t)quota_return(user, QUOTA_RES_MAPPED_VIEW, 99),
                   (uint64_t)STATUS_SUCCESS, "charge returned, again a single mutation");
    for (uint32_t i = 0; i < QUOTA_PRESSURE_FALL_SAMPLES; i++)
        quota_pressure_sample_all();
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(QUOTA_RES_MAPPED_VIEW),
                   (uint64_t)QUOTA_PRESSURE_NORMAL,
                   "and a single return likewise settles back to normal");

    quota_block_deref(user);
}

/* An idle domain must cost nothing: clean, resting, no debounce in flight means
 * the sampler skips it entirely rather than walking the registry every tick. */
static void test_pressure_idle_domain_is_not_sampled(void)
{
    quota_pressure_test_reset();

    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_valid(QUOTA_RES_CRASH_BUFFER),
                   0ull, "domain starts unknown");

    for (uint32_t i = 0; i < 5u; i++)
        quota_pressure_sample_all();

    TEST_ASSERT_EQ((uint64_t)quota_pressure_source_valid(QUOTA_RES_CRASH_BUFFER),
                   0ull, "an idle, clean domain is skipped, not resampled");
    TEST_ASSERT_EQ(quota_pressure_transition_count(), 0ull,
                   "and produces no transitions");
}

/* Transitions the ring refused must come out OLDEST FIRST and ahead of ring
 * records, or a consumer sees a newer level before an older one and acts on a
 * state the system had already left. */
static void test_pressure_pending_transitions_keep_order(void)
{
    quota_failure_source_t src;
    QUOTA_PRESSURE_RECORD  rec;
    QUOTA_FAILURE_RECORD   fail;
    uint64_t last_seq = 0;
    uint32_t kind, seen = 0;

    quota_pressure_test_reset();

    src.type = QUOTA_RES_THREAD;    src.principal = QUOTA_PRINCIPAL_PROCESS;
    src.block_id = 5ull;            src.requested = 1ull;
    src.current = 1ull;             src.limit = 1ull;
    src.owner_sid_hash = 0ull;      src.owner_rid = 0u;
    src.result = STATUS_QUOTA_EXCEEDED;
    src.pid = 0u; src.tid = 0u; src.attribution = QUOTA_ATTRIB_BEST_EFFORT;

    for (uint32_t i = 0; i < QUOTA_PRESSURE_RING_SLOTS; i++) {
        quota_pressure_test_refill_tokens();
        quota_pressure_note_failure(&src);
    }

    /* Two transitions with the ring full: both must be queued, in order. */
    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES * 2u);
    TEST_ASSERT_EQ((uint64_t)quota_pressure_level(PT_A),
                   (uint64_t)QUOTA_PRESSURE_WARNING, "walked two levels up");

    /* Free the ring, then read every transition that comes out. */
    while ((kind = quota_pressure_test_pop(&rec, &fail)) != 0)
        (void)kind;

    quota_pressure_test_drain();

    /* Re-run the same climb with room, and confirm sequences never go backwards
     * across the pending-then-ring boundary. */
    quota_pressure_test_reset();
    pressure_feed(PT_A, (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX,
                  QUOTA_PRESSURE_RISE_SAMPLES * 3u);
    while ((kind = quota_pressure_test_pop(&rec, &fail)) != 0) {
        if (kind != QUOTA_PRESSURE_KIND_TRANSITION)
            continue;
        TEST_ASSERT(rec.transition_seq > last_seq,
                    "transition sequences never go backwards");
        last_seq = rec.transition_seq;
        seen++;
    }
    TEST_ASSERT_EQ((uint64_t)seen, 3ull,
                   "every level entered produced its own record, none coalesced");
}

/* The notification-channel retry SCHEDULE, asserted DIRECTLY -- the decision, in
 * isolation from the path that consumes it. The policy is pure functions over
 * explicit state and an explicit `now` precisely so every branch a regression could
 * break is reachable without a clock or a create call: the retryable
 * classification, the backoff floor, the doubling, the cap, the "not yet" refusal,
 * warn-exactly-once, and the reset after recovery.
 *
 * The live WIRING used to be unprovable and no longer is: the test below drives the
 * real create path through an injected clock and creator. This one still earns its
 * place -- it covers the arithmetic exhaustively and cheaply, where the live test
 * covers that the path actually consults it. */
static void test_pressure_retry_policy_schedule(void)
{
    quota_pressure_retry_t st = { 0 };
    int warn = 0;
    uint64_t now = 1000ull;

    /* CLASSIFICATION. Only the two genuinely transient statuses are retryable;
     * everything else must be permanent, or a retry loop would spin on a decision
     * that will never change. */
    TEST_ASSERT_EQ((uint32_t)quota_pressure_retry_status_retryable(STATUS_RETRY), 1u,
                   "a closed charge gate is retryable");
    TEST_ASSERT_EQ((uint32_t)quota_pressure_retry_status_retryable(
                       STATUS_INSUFFICIENT_RESOURCES), 1u,
                   "resource exhaustion is retryable");
    TEST_ASSERT_EQ((uint32_t)quota_pressure_retry_status_retryable(
                       STATUS_OBJECT_NAME_COLLISION), 0u,
                   "a name collision is permanent, never retried");
    TEST_ASSERT_EQ((uint32_t)quota_pressure_retry_status_retryable(
                       STATUS_PRIVILEGE_NOT_HELD), 0u,
                   "a privilege refusal is permanent");
    TEST_ASSERT_EQ((uint32_t)quota_pressure_retry_status_retryable(
                       STATUS_OBJECT_PATH_NOT_FOUND), 0u,
                   "a missing category is permanent");

    /* NOTHING PENDING is never due, whatever the clock says. */
    TEST_ASSERT_EQ((uint32_t)quota_pressure_retry_due(&st, 0u, now), 0u,
                   "an empty pending mask is never due");

    /* FIRST failure takes the floor interval, not zero -- an immediate retry is
     * the storm the backoff exists to prevent. */
    quota_pressure_retry_advance(&st, 0x1u, now, &warn);
    TEST_ASSERT_EQ(st.gap_ns, QUOTA_PRESSURE_RETRY_FIRST_NS,
                   "the first failure schedules the floor interval");
    TEST_ASSERT_EQ(st.at_ns, now + QUOTA_PRESSURE_RETRY_FIRST_NS,
                   "and the deadline is that interval from now");
    TEST_ASSERT_EQ((uint64_t)st.tries, 1ull, "the attempt is counted");
    TEST_ASSERT_EQ((uint32_t)warn, 0u, "one failure is not worth a message");

    /* NOT YET: a trigger that fires before the deadline must be refused. This is
     * the assertion that pins the storm fix -- the drain re-arms for queued
     * records, and without it every re-arm would carry another attempt. */
    TEST_ASSERT_EQ((uint32_t)quota_pressure_retry_due(&st, 0x1u, st.at_ns - 1ull), 0u,
                   "a trigger before the deadline is refused");
    TEST_ASSERT_EQ((uint32_t)quota_pressure_retry_due(&st, 0x1u, st.at_ns), 1u,
                   "and admitted exactly at the deadline");

    /* DOUBLING, checked at EVERY step rather than only at the end. A regression
     * that jumped straight from 200 ms to the 30 s cap on the third failure would
     * satisfy a first-and-last assertion while delaying every recovery by half
     * a minute, so each transition is pinned against min(previous * 2, MAX) -- and
     * the WARNING EDGE is observed on the exact attempt that crosses the
     * threshold, because `warn` is a one-shot pulse that later calls clear. */
    for (uint32_t step = 2u; step <= QUOTA_PRESSURE_RETRY_WARN_TRIES + 3u; step++) {
        uint64_t prev_gap = st.gap_ns;
        uint64_t expect   = (prev_gap * 2ull > QUOTA_PRESSURE_RETRY_MAX_NS)
                                ? QUOTA_PRESSURE_RETRY_MAX_NS
                                : prev_gap * 2ull;

        now  = st.at_ns;
        warn = 0xBAD;                    /* poisoned: the callee must write it */
        quota_pressure_retry_advance(&st, 0x1u, now, &warn);

        TEST_ASSERT_EQ(st.gap_ns, expect,
                       "each failure doubles the interval until the cap, then holds");
        TEST_ASSERT_EQ(st.at_ns, now + st.gap_ns,
                       "and the deadline always follows the current interval");
        TEST_ASSERT_EQ((uint64_t)st.tries, (uint64_t)step,
                       "every attempt is counted exactly once");

        /* The pulse fires on the threshold attempt and on no other. */
        if (step == QUOTA_PRESSURE_RETRY_WARN_TRIES)
            TEST_ASSERT_EQ((uint32_t)warn, 1u,
                           "the warning fires exactly on the threshold attempt");
        else
            TEST_ASSERT_EQ((uint32_t)warn, 0u,
                           "and on no other attempt, before or after it");
    }
    TEST_ASSERT_EQ(st.gap_ns, QUOTA_PRESSURE_RETRY_MAX_NS,
                   "the interval saturates at the ceiling rather than growing");
    /* Still pending, still due later: the schedule NEVER gives up. An attempt
     * ceiling here was tried and removed -- it rebuilt the permanent-disable
     * defect, because neither retryable status has a time bound. */
    TEST_ASSERT_EQ((uint32_t)quota_pressure_retry_due(&st, 0x1u, st.at_ns), 1u,
                   "retries continue indefinitely at the capped cadence");

    /* WARN EXACTLY ONCE. The counter kept counting past the threshold above, so by
     * now the single warning must already have been spent. */
    TEST_ASSERT(st.tries > QUOTA_PRESSURE_RETRY_WARN_TRIES,
                "the attempt count keeps rising past the reporting threshold");
    TEST_ASSERT_EQ((uint32_t)st.warned, 1u, "the latch records that it was reported");

    /* RESET ON RECOVERY: an empty pending mask clears the whole schedule, so a
     * later deferral starts from the floor rather than inheriting a 30 s gap. */
    quota_pressure_retry_advance(&st, 0u, now, &warn);
    TEST_ASSERT_EQ(st.gap_ns, 0ull, "recovery clears the interval");
    TEST_ASSERT_EQ(st.at_ns, 0ull, "and the deadline");
    TEST_ASSERT_EQ((uint64_t)st.tries, 0ull, "and the attempt count");
    TEST_ASSERT_EQ((uint32_t)st.warned, 0u, "and re-arms the single warning");
    quota_pressure_retry_advance(&st, 0x2u, now, &warn);
    TEST_ASSERT_EQ(st.gap_ns, QUOTA_PRESSURE_RETRY_FIRST_NS,
                   "a deferral after recovery starts from the floor again");

    /* NULL is a no-op on both, not a fault. */
    TEST_ASSERT_EQ((uint32_t)quota_pressure_retry_due((const quota_pressure_retry_t *)0,
                                                      0x1u, now), 0u,
                   "a NULL state is never due");
    quota_pressure_retry_advance((quota_pressure_retry_t *)0, 0x1u, now, &warn);
    TEST_ASSERT_EQ((uint32_t)warn, 0u, "advancing a NULL state warns nothing");
}

/* The LIVE channel-create path, end to end (TODO-25 s19).
 *
 * The policy test above proves the backoff ARITHMETIC against a local struct.
 * This proves the WIRING: that the live create loop actually reaches a creator,
 * actually feeds its own clock to the policy, actually republishes the resulting
 * deadline, and that its own due gate honours it. That seam did not exist before,
 * so the create path was the one part of the retry machinery covered only by
 * inspection.
 *
 * Three hazards, three mechanisms:
 *  - racing the live drain -> quiesce (hold, then FLUSH the queued DPCs; the hold
 *    alone is not a barrier, because the drain clears its armed flag before its
 *    body finishes);
 *  - a 30-second capped interval -> an injected clock, so there is no real wait;
 *  - a creator hook that can never run, because boot published every channel and
 *    the loop skips a populated slot -> isolate exactly one channel, saving its
 *    live state pointer and putting it back untouched.
 * The creator-call COUNT is asserted throughout: without it this test could pass
 * having never entered the code it claims to cover. */
static void test_pressure_live_create_backoff(void)
{
    /* ZERO-INITIALIZED, and every isolate return code BRANCHED on, not merely
     * asserted. TEST_ASSERT_EQ records a failure and CONTINUES, so treating an
     * assertion as a stop would hand a garbage save record to the restore -- whose
     * index guard would pass whenever the garbage index happened to be in range,
     * publishing an arbitrary pointer into a live notification slot. A test that
     * corrupts the kernel when it fails is worse than no test. */
    quota_pressure_channel_save_t save = { 0 };
    void    *live_state;
    uint64_t now        = 5ull * 1000ull * 1000ull * 1000ull;   /* arbitrary base */
    uint64_t expect_gap = QUOTA_PRESSURE_RETRY_FIRST_NS;
    uint32_t pending;

    quota_pressure_test_quiesce();
    quota_pressure_test_seam_install(now, STATUS_RETRY);

    /* An out-of-range index must be refused and must mutate NOTHING -- a guard that
     * half-applied would strand a live channel. Checked BEFORE the real isolate, so
     * the comparison baseline is the untouched live state. */
    {
        quota_pressure_channel_save_t bad = { 0 };
        void    *state_before   = quota_pressure_test_channel_state(0u);
        uint64_t deadline_before = quota_pressure_test_retry_deadline();

        TEST_ASSERT_EQ(quota_pressure_test_channel_isolate(0xFFFFFFFFu, &bad), -1,
                       "an out-of-range channel index is refused");
        TEST_ASSERT_EQ((uint64_t)(uintptr_t)bad.state, 0ull,
                       "a refused isolate writes nothing into the save record");
        TEST_ASSERT_EQ((uint64_t)(uintptr_t)quota_pressure_test_channel_state(0u),
                       (uint64_t)(uintptr_t)state_before,
                       "and leaves every live channel exactly as it was");
        TEST_ASSERT_EQ(quota_pressure_test_retry_deadline(), deadline_before,
                       "and does not disturb the live retry schedule");
        TEST_ASSERT_EQ(quota_pressure_test_channel_isolate(0u,
                           (quota_pressure_channel_save_t *)0), -1,
                       "a NULL save record is refused");
    }

    live_state = quota_pressure_test_channel_state(0u);
    {
        int rc = quota_pressure_test_channel_isolate(0u, &save);

        /* Asserted so a failure is VISIBLE (a silent return would let the suite
         * pass without the seam ever being exercised), then branched on so a
         * failure is CONTAINED -- nothing below may touch an unfilled save. */
        TEST_ASSERT_EQ(rc, 0, "one channel is isolated for the live create path");
        if (rc != 0) {
            quota_pressure_test_seam_remove();
            return;
        }
    }

    /* A channel deferred with a cleared schedule is due at once: the backoff
     * delays RETRIES, it must not delay the first attempt. */
    TEST_ASSERT_EQ((uint32_t)quota_pressure_test_retry_due(), 1u,
                   "a pending channel with no deadline is due immediately");

    pending = quota_pressure_test_create_attempt(1u << 0);
    TEST_ASSERT_EQ(pending, 1u, "a transient failure leaves the channel pending");
    TEST_ASSERT_EQ(quota_pressure_test_create_calls(), 1u,
                   "the live create path reached the creator");
    TEST_ASSERT_EQ(quota_pressure_test_retry_tries(), 1u,
                   "the live path counted the attempt");
    TEST_ASSERT_EQ(quota_pressure_test_retry_deadline(), now + expect_gap,
                   "the live path published the floor interval from ITS clock");

    /* The live gate, answered against the injected clock. */
    quota_pressure_test_seam_set_now(now + expect_gap - 1ull);
    TEST_ASSERT_EQ((uint32_t)quota_pressure_test_retry_due(), 0u,
                   "the live gate refuses an attempt before the deadline");
    quota_pressure_test_seam_set_now(now + expect_gap);
    TEST_ASSERT_EQ((uint32_t)quota_pressure_test_retry_due(), 1u,
                   "and admits one exactly at the deadline");

    /* DOUBLING through the live path, checked at every step. A regression that
     * jumped straight to the 30 s cap would satisfy a first-and-last assertion
     * while delaying every channel recovery by half a minute. */
    for (uint32_t step = 2u; step <= QUOTA_PRESSURE_RETRY_WARN_TRIES + 3u; step++) {
        now = quota_pressure_test_retry_deadline();
        quota_pressure_test_seam_set_now(now);
        expect_gap = (expect_gap * 2ull > QUOTA_PRESSURE_RETRY_MAX_NS)
                         ? QUOTA_PRESSURE_RETRY_MAX_NS
                         : expect_gap * 2ull;

        pending = quota_pressure_test_create_attempt(1u << 0);
        TEST_ASSERT_EQ(pending, 1u, "still pending while the creator defers");
        TEST_ASSERT_EQ(quota_pressure_test_create_calls(), step,
                       "exactly one creator call per live attempt");
        TEST_ASSERT_EQ(quota_pressure_test_retry_tries(), step,
                       "every live attempt counted exactly once");
        TEST_ASSERT_EQ(quota_pressure_test_retry_deadline(), now + expect_gap,
                       "the live deadline follows the doubled interval");
    }
    /* SATURATION on the live path. The loop runs past the reporting threshold on
     * purpose: doubling from the 100 ms floor only reaches 12.8 s by the eighth
     * attempt, so stopping at the threshold would never observe the ceiling -- and
     * an interval that kept doubling past it would go unnoticed. */
    TEST_ASSERT_EQ(quota_pressure_test_retry_deadline() - now,
                   QUOTA_PRESSURE_RETRY_MAX_NS,
                   "the live interval saturates at the ceiling rather than growing");

    /* A PERMANENT failure must CLEAR the bit on the live path, not keep retrying:
     * a name collision or a missing category has no later answer, and a retry
     * would re-allocate and re-log for a decision that cannot change. */
    quota_pressure_test_seam_install(now, STATUS_OBJECT_NAME_COLLISION);
    pending = quota_pressure_test_create_attempt(1u << 0);
    TEST_ASSERT_EQ(pending, 0u, "a permanent failure drops the channel from pending");
    TEST_ASSERT_EQ(quota_pressure_test_create_calls(), 1u,
                   "the permanent attempt reached the creator exactly once");

    /* RESTORE, and prove it was TRANSACTIONAL rather than just pointer-correct.
     * The test mutated the pending mask and every retry field, so releasing the
     * transport with a lost pending bit -- or with this test's 30-second capped
     * schedule still in force -- would be a silent live-subsystem regression.
     *
     * A second isolate is the comparison: it captures the CURRENT live values
     * BEFORE mutating anything, so save2 must equal save1 field for field. */
    quota_pressure_test_channel_restore(&save);
    quota_pressure_test_seam_remove();
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)quota_pressure_test_channel_state(0u),
                   (uint64_t)(uintptr_t)live_state,
                   "the ORIGINAL notification state is back, not a replacement");
    {
        quota_pressure_channel_save_t after = { 0 };
        int rc = quota_pressure_test_channel_isolate(0u, &after);

        TEST_ASSERT_EQ(rc, 0, "re-isolating captures the restored live state");
        if (rc != 0) {
            /* The comparison isolate failed, so `after` is unusable -- put the
             * channel back from the save that IS valid rather than from garbage. */
            quota_pressure_test_channel_restore(&save);
            return;
        }
        TEST_ASSERT_EQ((uint64_t)(uintptr_t)after.state,
                       (uint64_t)(uintptr_t)save.state, "state pointer restored");
        TEST_ASSERT_EQ(after.pending, save.pending, "pending mask restored");
        TEST_ASSERT_EQ(after.retry.at_ns, save.retry.at_ns, "retry deadline restored");
        TEST_ASSERT_EQ(after.retry.gap_ns, save.retry.gap_ns, "retry interval restored");
        TEST_ASSERT_EQ((uint64_t)after.retry.tries, (uint64_t)save.retry.tries,
                       "retry attempt count restored");
        TEST_ASSERT_EQ((uint32_t)after.retry.warned, (uint32_t)save.retry.warned,
                       "warn latch restored");
        quota_pressure_test_channel_restore(&after);   /* undo the comparison isolate */
    }
}

void test_register_quota_pressure(void)
{
    test_suite_register_cat("Quota: permille boundaries",
                            test_pressure_permille_boundaries, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: unlimited cap has no ratio",
                            test_pressure_permille_unlimited_is_invalid, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: permille overflow-safe",
                            test_pressure_permille_no_overflow, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: pressure rise needs debounce",
                            test_pressure_rise_needs_debounce, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: pressure steps one level per debounce",
                            test_pressure_one_step_per_debounce, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: pressure does not flap inside a band",
                            test_pressure_no_flap_inside_band, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: pressure falls with debounce",
                            test_pressure_falls_with_debounce, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: pressure debounce is asymmetric",
                            test_pressure_debounce_is_asymmetric, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: invalid sample does not de-escalate",
                            test_pressure_invalid_sample_does_not_deescalate, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: sustained unknown resets the level",
                            test_pressure_sustained_unknown_resets_level, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: unsampled domain reports no source",
                            test_pressure_unsampled_domain_reports_none, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stall seam tags its source",
                            test_pressure_stall_seam_tags_its_source, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: system level skips invalid domains",
                            test_pressure_system_level_skips_invalid, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: system level UNKNOWN when unmeasured",
                            test_pressure_system_level_unknown_when_unmeasured,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: channel retry policy schedule",
                            test_pressure_retry_policy_schedule, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: transition record contract",
                            test_pressure_transition_record_contract, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: transition sequence monotonic",
                            test_pressure_transition_seq_monotonic, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: failure record contract",
                            test_pressure_failure_record_contract, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: charge refusal emits event",
                            test_pressure_charge_refusal_emits_event, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: successful charge emits no event",
                            test_pressure_success_emits_no_event, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: failure event rate limit counts drops",
                            test_pressure_rate_limit_counts_drops, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: dropped event still advances sequence",
                            test_pressure_dropped_event_still_advances_seq, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: ring overflow counted separately",
                            test_pressure_ring_overflow_counted_separately, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: nomination clear when nobody qualifies",
                            test_pressure_nomination_clear_when_nobody_qualifies, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: nominates saturated user principal",
                            test_pressure_nominates_saturated_user, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: nomination skips uncapped principal",
                            test_pressure_nomination_skips_uncapped, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: pressure dump is read-only",
                            test_pressure_dump_is_read_only, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: records without transport armed",
                            test_pressure_records_without_transport, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: large ratio does not fake a threshold",
                            test_pressure_permille_large_no_false_threshold, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: threshold boundaries exact",
                            test_pressure_threshold_boundaries_exact, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: domain tracks the worst principal",
                            test_pressure_domain_tracks_worst_principal, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: worst-user reports none when uncapped",
                            test_pressure_worst_user_none_when_uncapped, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: set_limit drives a sample",
                            test_pressure_set_limit_drives_a_sample, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: transition survives a full ring",
                            test_pressure_transition_survives_full_ring, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: ring wraps after a partial drain",
                            test_pressure_ring_wraps_after_partial_drain, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: drain consumes every record",
                            test_pressure_drain_consumes_every_record, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: ownerless block digests to zero",
                            test_pressure_digest_ownerless_block, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: RID is the last sub-authority",
                            test_pressure_digest_multi_subauthority_sid, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: overflow refusal emits its own event",
                            test_pressure_overflow_refusal_emits_event, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: critical pressure nominates a principal",
                            test_pressure_critical_publishes_nomination, TEST_CAT_QUOTA);

    test_suite_register_cat("Quota: stable load still escalates",
                            test_pressure_stable_load_still_escalates, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: idle domain is not sampled",
                            test_pressure_idle_domain_is_not_sampled, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: pending transitions keep order",
                            test_pressure_pending_transitions_keep_order, TEST_CAT_QUOTA);

    /* LAST: returns the ring to the live publisher (see test_reset). */
    /* Before the transport release below: this one takes the publication hold and
     * leaves it set, exactly as the reset helper does, so the release must stay
     * the last registration in the suite. */
    test_suite_register_cat("Quota: live channel-create backoff end to end",
                            test_pressure_live_create_backoff, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: pressure transport hold released",
                            test_pressure_release_transport, TEST_CAT_QUOTA);
}

#endif /* KERNEL_TESTS */
