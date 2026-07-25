/* ============================================================================
 * test_quota_stall.c -- PSI-shaped resource-pressure stall telemetry.
 *
 * Section 12 coverage: the per-CPU state-transition integrator (overlapping
 * waits are wall time, not summed durations), the fixed-point EWMA (reaches
 * exactly zero, saturates at full scale, decays across missed periods), the
 * VALID contract (an uninstrumented seam reads unknown, never calm), the
 * cpu.full undefined-at-system-level contract, the independence of this lane
 * from the budget lane, and the SystemResourcePressureInformation marshalling.
 *
 * Everything here drives the REAL producer and aggregation paths at a synthetic
 * clock (quota_stall_test_set_clock) rather than through an injection seam, so
 * what the tests exercise is what ships. The periodic pressure DPC is held for
 * the duration: it calls quota_stall_aggregate() every 50 ms and would otherwise
 * consume the windows these tests are stepping by hand.
 *
 * XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/quota/quota_stall.h"
#include "kernel/quota/quota_pressure.h"
#include "kernel/nt/quota_pressure_info.h"
#include "kernel/nt/ntstatus.h"

extern NTSTATUS nt_query_resource_pressure_information(void *buffer,
                                                       uint32_t buf_size,
                                                       uint32_t *return_length);

/* A synthetic base well away from zero: zero is the "unseeded" sentinel in both
 * the per-CPU integrator and the aggregator, so anchoring a test there would
 * exercise the seeding path instead of the measuring one. */
#define STALL_BASE_NS   1000000000ull
#define STALL_WINDOW_NS QUOTA_STALL_UPDATE_NS

static uint64_t g_now_ns;

/* Hold the periodic sampler, drop all telemetry, and seed the aggregation clock
 * at the synthetic base. The seeding aggregate() returns 0 by design -- the
 * interval before the first observation is not a measurement -- so every test
 * starts with a live anchor and an empty history. */
static void stall_begin(void)
{
    quota_pressure_test_reset();
    quota_stall_test_reset();
    g_now_ns = STALL_BASE_NS;
    quota_stall_test_set_clock(g_now_ns);
    quota_stall_aggregate();
}

static void stall_advance(uint64_t ns)
{
    g_now_ns += ns;
    quota_stall_test_set_clock(g_now_ns);
}

/* Advance one aggregation window and close it. */
static int stall_step_window(void)
{
    stall_advance(STALL_WINDOW_NS);
    return quota_stall_aggregate();
}

static void stall_step_windows(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        (void)stall_step_window();
}

static void stall_snap(quota_stall_domain_t d, quota_stall_snapshot_t *out)
{
    TEST_ASSERT_EQ(quota_stall_get(d, out), 0, "snapshot of a live domain succeeds");
}

/* ==========================================================================
 * Integration semantics
 * ========================================================================== */

/* A stall in one domain must not appear in any other domain's counters. This is
 * the property that makes a per-resource metric worth having at all: an operator
 * seeing io pressure needs to know the memory number was measured independently
 * and really is quiet. */
static void test_stall_domain_isolation(void)
{
    quota_stall_snapshot_t mem, cpu, io;
    quota_stall_token_t tok;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);
    (void)stall_step_window();
    quota_stall_task_unstalled(&tok);

    stall_snap(QUOTA_STALL_MEM, &mem);
    stall_snap(QUOTA_STALL_CPU, &cpu);
    stall_snap(QUOTA_STALL_IO, &io);

    TEST_ASSERT_EQ((uint32_t)mem.some_total_ns, (uint32_t)STALL_WINDOW_NS,
                   "the stalled domain accrued exactly one window of some time");
    TEST_ASSERT_EQ((uint32_t)cpu.some_total_ns, 0u,
                   "cpu accrued no some time");
    TEST_ASSERT_EQ((uint32_t)io.some_total_ns, 0u,
                   "io accrued no some time");
    TEST_ASSERT(mem.some_avg[QUOTA_STALL_AVG10] > 0,
                "the stalled domain's avg10 moved off zero");
    TEST_ASSERT_EQ((uint32_t)cpu.some_avg[QUOTA_STALL_AVG10], 0u,
                   "an unstalled domain's avg10 stays zero");
}

/* The core of the state-transition design. Two threads blocked across the SAME
 * window contribute one window of pressure, not two -- a duration-summing seam
 * would report 200 percent here, and the metric would be meaningless. */
static void test_stall_overlapping_waits_not_double_counted(void)
{
    quota_stall_snapshot_t s;
    quota_stall_token_t a, b;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_IO);
    a = quota_stall_task_stalled(QUOTA_STALL_IO);
    b = quota_stall_task_stalled(QUOTA_STALL_IO);
    (void)stall_step_window();

    stall_snap(QUOTA_STALL_IO, &s);
    TEST_ASSERT_EQ((uint32_t)s.some_total_ns, (uint32_t)STALL_WINDOW_NS,
                   "two overlapping waits accrue one window, not two");
    TEST_ASSERT(s.some_avg[QUOTA_STALL_AVG10] <= QUOTA_STALL_PERMILLE_MAX,
                "an overlapping-wait window cannot exceed full scale");

    /* One leaves; the other is still blocked, so accrual continues at the same
     * rate rather than stopping or doubling. */
    quota_stall_task_unstalled(&a);
    (void)stall_step_window();
    stall_snap(QUOTA_STALL_IO, &s);
    TEST_ASSERT_EQ((uint32_t)s.some_total_ns, (uint32_t)(2ull * STALL_WINDOW_NS),
                   "the surviving waiter keeps accruing at one window per window");

    quota_stall_task_unstalled(&b);
    (void)stall_step_window();
    stall_snap(QUOTA_STALL_IO, &s);
    TEST_ASSERT_EQ((uint32_t)s.some_total_ns, (uint32_t)(2ull * STALL_WINDOW_NS),
                   "accrual stops once the last waiter leaves");
}

/* An unbalanced leave is a producer bug, but it must not wrap the count: a
 * wrapped stalled-count would pin the domain saturated for the rest of the boot,
 * which is far worse than the missing decrement it came from. */
static void test_stall_unbalanced_leave_clamps(void)
{
    quota_stall_snapshot_t s;
    quota_stall_token_t tok;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);

    /* An invalid token must be a safe no-op: a producer whose enter was not
     * recorded still calls leave on its exit path. */
    {
        quota_stall_token_t none = quota_stall_token_none();
        quota_stall_task_unstalled(&none);
        quota_stall_task_unstalled(&none);
    }

    /* A real token consumed twice must clamp, not underflow to ~4 billion. */
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);
    quota_stall_task_unstalled(&tok);
    quota_stall_task_unstalled(&tok);
    (void)stall_step_window();

    stall_snap(QUOTA_STALL_MEM, &s);
    TEST_ASSERT_EQ((uint32_t)s.some_total_ns, 0u,
                   "unmatched and double-consumed leaves accrue nothing");

    /* And the counter is still usable afterwards rather than stuck negative. */
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);
    (void)stall_step_window();
    stall_snap(QUOTA_STALL_MEM, &s);
    TEST_ASSERT_EQ((uint32_t)s.some_total_ns, (uint32_t)STALL_WINDOW_NS,
                   "a balanced enter after clamped leaves still accrues");
    quota_stall_task_unstalled(&tok);
}

/* The case a bare zero-clamp cannot catch. With TWO waiters the count never
 * reaches zero, so consuming one token twice would decrement the OTHER task's
 * still-active stall and report the domain quiet while a task is still blocked.
 * Consuming the token invalidates it, so the second call does nothing. */
static void test_stall_double_consume_cannot_cancel_another_waiter(void)
{
    quota_stall_snapshot_t s;
    quota_stall_token_t a, b;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_IO);
    a = quota_stall_task_stalled(QUOTA_STALL_IO);
    b = quota_stall_task_stalled(QUOTA_STALL_IO);

    quota_stall_task_unstalled(&a);
    quota_stall_task_unstalled(&a);   /* must NOT decrement b's stall */

    (void)stall_step_window();
    stall_snap(QUOTA_STALL_IO, &s);
    TEST_ASSERT_EQ((uint32_t)s.some_total_ns, (uint32_t)STALL_WINDOW_NS,
                   "the second waiter is still counted as stalled");

    quota_stall_task_unstalled(&b);
    (void)stall_step_window();
    stall_snap(QUOTA_STALL_IO, &s);
    TEST_ASSERT_EQ((uint32_t)s.some_total_ns, (uint32_t)STALL_WINDOW_NS,
                   "and accrual stops once the real last waiter leaves");
}

/* A stall far below one permille of the window must still be MEASURED. An
 * earlier revision narrowed each CPU's ratio to a whole permille before
 * weighting, so anything under one part in a thousand -- and, because the
 * multiply came after the divide, even a stall of exactly one permille --
 * collapsed to zero before the averages or the cumulative total ever saw it. A
 * domain stalling steadily at that level would have reported nothing at all. */
static void test_stall_sub_permille_is_not_erased(void)
{
    quota_stall_snapshot_t s;
    quota_stall_token_t tok;
    const uint64_t brief_ns = 200000ull;   /* 0.1 permille of a 2 s window */

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_IO);

    tok = quota_stall_task_stalled(QUOTA_STALL_IO);
    stall_advance(brief_ns);
    quota_stall_task_unstalled(&tok);
    stall_advance(STALL_WINDOW_NS - brief_ns);
    TEST_ASSERT_EQ(quota_stall_aggregate(), 1, "the window closes");

    stall_snap(QUOTA_STALL_IO, &s);
    /* The reported permille rounds to zero -- that is the ABI's resolution, and
     * it is honest. The cumulative TOTAL must not: it is nanoseconds, and the
     * nanoseconds happened. Allow a small integer-rounding band rather than an
     * exact match; the point is that it is the measured magnitude, not zero. */
    TEST_ASSERT(s.some_total_ns > (brief_ns - (brief_ns / 100u)),
                "a sub-permille stall still lands in the cumulative total");
    TEST_ASSERT(s.some_total_ns <= brief_ns,
                "and it is not inflated beyond what actually stalled");
}

/* Stall for an EXACT permille of every window, forever, and check the published
 * number is that permille. An EWMA fed a constant sample converges from below
 * and never reaches it, so truncating at the publication boundary would report
 * one permille less than the real, sustained, exactly-representable pressure --
 * indefinitely. Two cases matter: the smallest reportable value, and a value
 * sitting exactly on a hysteresis threshold. */
static void stall_hold_exact_permille(quota_stall_domain_t domain,
                                      uint64_t permille, uint32_t windows)
{
    uint64_t on_ns = (STALL_WINDOW_NS / (uint64_t)QUOTA_STALL_PERMILLE_MAX) * permille;

    for (uint32_t i = 0; i < windows; i++) {
        quota_stall_token_t tok = quota_stall_task_stalled(domain);
        stall_advance(on_ns);
        quota_stall_task_unstalled(&tok);
        stall_advance(STALL_WINDOW_NS - on_ns);
        (void)quota_stall_aggregate();
    }
}

static void test_stall_exact_permille_is_published_exactly(void)
{
    quota_stall_snapshot_t s;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_IO);
    stall_hold_exact_permille(QUOTA_STALL_IO, 1u, 60u);

    stall_snap(QUOTA_STALL_IO, &s);
    TEST_ASSERT_EQ((uint32_t)s.some_avg[QUOTA_STALL_AVG10], 1u,
                   "a sustained exact 1 permille publishes 1, not 0");
}

/* The same defect where it actually bites: a workload sitting exactly on the
 * WATCH rise threshold must cross it. Converging to 699.999847 and publishing
 * 699 would leave the level at normal forever. */
static void test_stall_exact_threshold_crosses(void)
{
    quota_stall_snapshot_t s;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    stall_hold_exact_permille(QUOTA_STALL_MEM,
                              (uint64_t)QUOTA_PRESSURE_RISE_WATCH_PERMILLE, 60u);

    stall_snap(QUOTA_STALL_MEM, &s);
    TEST_ASSERT_EQ((uint32_t)s.some_avg[QUOTA_STALL_AVG10],
                   (uint32_t)QUOTA_PRESSURE_RISE_WATCH_PERMILLE,
                   "a sustained exact-threshold sample publishes the threshold");
    TEST_ASSERT(s.level >= (uint8_t)QUOTA_PRESSURE_WATCH,
                "and the level actually crosses into watch");
}

/* ==========================================================================
 * The VALID contract
 * ========================================================================== */

/* Every seam is unwired today, so every domain must read UNKNOWN. A zero here
 * that a consumer could mistake for "no pressure" is the specific dishonesty
 * this flag exists to prevent -- and it is the one thing Linux PSI cannot say. */
static void test_stall_uninstrumented_reports_unknown(void)
{
    quota_stall_snapshot_t s;

    stall_begin();
    stall_step_windows(8);

    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
        stall_snap((quota_stall_domain_t)d, &s);
        TEST_ASSERT_EQ((uint32_t)s.valid, 0u,
                       "an uninstrumented domain never becomes valid");
        TEST_ASSERT_EQ((uint32_t)s.instrumented, 0u,
                       "an uninstrumented domain reports no seam");
        TEST_ASSERT_EQ((uint32_t)s.level, (uint32_t)QUOTA_PRESSURE_NORMAL,
                       "an unmeasured domain never steps a level");
    }
    TEST_ASSERT_EQ((uint32_t)quota_stall_system_level(),
                   (uint32_t)QUOTA_PRESSURE_NORMAL,
                   "unknown domains neither raise nor lower the system level");
}

/* Declaring a seam is not enough on its own: validity also needs a closed
 * window, so the very first read after instrumentation cannot report a level
 * derived from no samples at all. */
static void test_stall_validity_needs_a_closed_window(void)
{
    quota_stall_snapshot_t s;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_IO);
    stall_snap(QUOTA_STALL_IO, &s);
    TEST_ASSERT_EQ((uint32_t)s.instrumented, 1u, "the seam is recorded immediately");
    TEST_ASSERT_EQ((uint32_t)s.valid, 0u,
                   "validity waits for a window, not just a declaration");

    (void)stall_step_window();
    stall_snap(QUOTA_STALL_IO, &s);
    TEST_ASSERT_EQ((uint32_t)s.valid, 1u, "one closed window makes the domain valid");
}

/* cpu.full has no meaning at system level -- some task is running or the CPU
 * would be idle rather than contended -- so it is reported as a hard zero with
 * an explicit flag, exactly as Linux does, rather than as a plausible number a
 * consumer might act on. */
static void test_stall_cpu_full_is_undefined(void)
{
    quota_stall_snapshot_t cpu, mem;
    quota_stall_token_t tcpu, tmem;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_CPU);
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);

    /* Both domains stalled with nothing runnable: the condition under which
     * `full` accrues for any domain that defines it. */
    tcpu = quota_stall_task_stalled(QUOTA_STALL_CPU);
    tmem = quota_stall_task_stalled(QUOTA_STALL_MEM);
    stall_step_windows(4);

    stall_snap(QUOTA_STALL_CPU, &cpu);
    stall_snap(QUOTA_STALL_MEM, &mem);

    TEST_ASSERT_EQ((uint32_t)cpu.full_undefined, 1u,
                   "cpu declares its full metric undefined");
    TEST_ASSERT_EQ((uint32_t)cpu.full_total_ns, 0u,
                   "cpu.full total is zero by contract, not by measurement");
    TEST_ASSERT_EQ((uint32_t)cpu.full_avg[QUOTA_STALL_AVG10], 0u,
                   "cpu.full avg10 is zero by contract");
    TEST_ASSERT(cpu.some_total_ns > 0,
                "cpu.some is still measured normally");

    TEST_ASSERT_EQ((uint32_t)mem.full_undefined, 0u,
                   "mem defines its full metric");
    TEST_ASSERT(mem.full_total_ns > 0,
                "mem.full accrues while everything is stalled and nothing runnable");
    TEST_ASSERT(mem.full_total_ns <= mem.some_total_ns,
                "full can never exceed some");
    quota_stall_task_unstalled(&tcpu);
    quota_stall_task_unstalled(&tmem);
}

/* A runnable task means the CPU was not fully blocked, so `full` must stop
 * accruing while `some` continues. */
static void test_stall_full_requires_nothing_runnable(void)
{
    quota_stall_snapshot_t before, after;
    quota_stall_token_t tok;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_IO);
    tok = quota_stall_task_stalled(QUOTA_STALL_IO);
    (void)stall_step_window();
    stall_snap(QUOTA_STALL_IO, &before);
    TEST_ASSERT(before.full_total_ns > 0, "full accrues with nothing runnable");

    quota_stall_runnable_delta(1);
    (void)stall_step_window();
    stall_snap(QUOTA_STALL_IO, &after);

    TEST_ASSERT_EQ((uint32_t)after.full_total_ns, (uint32_t)before.full_total_ns,
                   "full stops accruing once something is runnable");
    TEST_ASSERT(after.some_total_ns > before.some_total_ns,
                "some keeps accruing while a task is still blocked");
    quota_stall_runnable_delta(-1);
    quota_stall_task_unstalled(&tok);
}

/* ==========================================================================
 * The averaging arithmetic
 * ========================================================================== */

/* The failure this test exists for: an EWMA carried in permille cannot reach
 * zero, because a stored 1 permille decays to 0.9995 and rounds straight back to
 * 1 forever -- a resource that stopped stalling would report pressure for the
 * rest of the boot. All three windows must land on EXACTLY zero. */
static void test_stall_averages_decay_to_exactly_zero(void)
{
    quota_stall_snapshot_t s;
    quota_stall_token_t tok;
    uint32_t steps = 0;
    int      cleared = 0;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);
    (void)stall_step_window();
    quota_stall_task_unstalled(&tok);

    stall_snap(QUOTA_STALL_MEM, &s);
    TEST_ASSERT(s.some_avg[QUOTA_STALL_AVG10] > 0,
                "the averages actually rose before the decay is measured");

    /* Bounded generously: the 300 s window needs a few thousand 2 s periods to
     * bleed a single-window impulse down to nothing. The point of the bound is
     * that the loop terminates, not that the exact count matters. */
    while (steps < 8000u) {
        (void)stall_step_window();
        steps++;
        stall_snap(QUOTA_STALL_MEM, &s);
        if (s.some_avg[QUOTA_STALL_AVG10] == 0 &&
            s.some_avg[QUOTA_STALL_AVG60] == 0 &&
            s.some_avg[QUOTA_STALL_AVG300] == 0) {
            cleared = 1;
            break;
        }
    }

    TEST_ASSERT_EQ(cleared, 1, "every averaging window decays to exactly zero");
    TEST_ASSERT(s.some_total_ns > 0,
                "the cumulative total is not erased by the averages decaying");
}

/* Sustained full-scale pressure converges toward, and never past, full scale. */
static void test_stall_averages_saturate_at_full_scale(void)
{
    quota_stall_snapshot_t s;
    quota_stall_token_t tok;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);
    stall_step_windows(200);

    stall_snap(QUOTA_STALL_MEM, &s);
    for (uint32_t w = 0; w < (uint32_t)QUOTA_STALL_WINDOW_COUNT; w++) {
        TEST_ASSERT(s.some_avg[w] <= QUOTA_STALL_PERMILLE_MAX,
                    "no averaging window can exceed full scale");
    }
    TEST_ASSERT(s.some_avg[QUOTA_STALL_AVG10] >= 900,
                "the 10 s window converges near full scale under sustained stall");
    TEST_ASSERT(s.some_avg[QUOTA_STALL_AVG10] >= s.some_avg[QUOTA_STALL_AVG300],
                "the short window leads the long one while pressure is rising");
    quota_stall_task_unstalled(&tok);
}

/* A delayed aggregation must decay for the periods it missed. Charging one
 * period's decay for a twenty-period gap would hold stale pressure high. */
static void test_stall_missed_periods_decay(void)
{
    quota_stall_snapshot_t one_step, many_steps;
    quota_stall_token_t tok;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);
    stall_step_windows(30);
    quota_stall_task_unstalled(&tok);

    (void)stall_step_window();
    stall_snap(QUOTA_STALL_MEM, &one_step);

    /* One aggregation covering twenty windows of quiet. */
    stall_advance(20ull * STALL_WINDOW_NS);
    TEST_ASSERT_EQ(quota_stall_aggregate(), 1, "a long gap still closes a window");
    stall_snap(QUOTA_STALL_MEM, &many_steps);

    TEST_ASSERT(many_steps.some_avg[QUOTA_STALL_AVG10] <
                one_step.some_avg[QUOTA_STALL_AVG10],
                "a twenty-period gap decays further than a single period does");
    TEST_ASSERT(many_steps.some_avg[QUOTA_STALL_AVG60] <=
                one_step.some_avg[QUOTA_STALL_AVG60],
                "the 60 s window also decays across the gap");
}

/* Past the catch-up bound the period count is CLAMPED, not the measurement
 * discarded. An earlier revision cleared every average and threw away the growth
 * it had just collected, so a first fully-stalled interval longer than the bound
 * published zeroes while the cumulative totals climbed: pressure measured and
 * then deleted. The stall must survive the gap that delayed it. */
static void test_stall_over_long_stalled_gap_keeps_its_measurement(void)
{
    quota_stall_snapshot_t s;
    quota_stall_token_t tok;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);

    /* One aggregation covering more than the catch-up bound, fully stalled. */
    stall_advance(200ull * STALL_WINDOW_NS);   /* far past every window */
    TEST_ASSERT_EQ(quota_stall_aggregate(), 1, "an over-long gap closes a window");

    stall_snap(QUOTA_STALL_MEM, &s);
    TEST_ASSERT(s.some_total_ns > 0,
                "the cumulative total records the stalled interval");
    TEST_ASSERT(s.some_avg[QUOTA_STALL_AVG10] >= 900,
                "a fully stalled over-long interval reports high pressure, not zero");
    TEST_ASSERT_EQ((uint32_t)s.valid, 1u,
                   "the domain is valid: a window closed and a seam exists");
    /* The LEVEL deliberately does NOT jump. One delayed aggregation is one
     * observation, however many periods it spanned, and the debounce exists to
     * require consecutive ones; synthesizing N observations from a single
     * interval average would fabricate history the counters never held. The
     * averages catch up in full, the level catches up at its normal rate. */
    TEST_ASSERT_EQ((uint32_t)s.level, (uint32_t)QUOTA_PRESSURE_NORMAL,
                   "one delayed aggregation is one debounce observation, not many");
    for (uint32_t i = 0; i < (uint32_t)QUOTA_PRESSURE_RISE_SAMPLES; i++)
        (void)stall_step_window();
    stall_snap(QUOTA_STALL_MEM, &s);
    TEST_ASSERT(s.level > (uint8_t)QUOTA_PRESSURE_NORMAL,
                "and the level rises once the debounce has its samples");
    quota_stall_task_unstalled(&tok);
}

/* The mirror case: a long QUIET gap must decay rather than hold the old value,
 * and must not invent pressure. */
static void test_stall_over_long_quiet_gap_decays(void)
{
    quota_stall_snapshot_t before, after;
    quota_stall_token_t tok;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);
    stall_step_windows(80);
    stall_snap(QUOTA_STALL_MEM, &before);
    TEST_ASSERT(before.some_avg[QUOTA_STALL_AVG10] >= 900,
                "pressure was high before the gap");

    quota_stall_task_unstalled(&tok);
    stall_advance(200ull * STALL_WINDOW_NS);   /* far past every window */
    TEST_ASSERT_EQ(quota_stall_aggregate(), 1, "an over-long quiet gap closes a window");

    stall_snap(QUOTA_STALL_MEM, &after);
    TEST_ASSERT_EQ((uint32_t)after.some_avg[QUOTA_STALL_AVG10], 0u,
                   "the short window bleeds to zero across a long quiet gap");
    TEST_ASSERT(after.some_avg[QUOTA_STALL_AVG300] <
                before.some_avg[QUOTA_STALL_AVG300],
                "the long window decays rather than holding its old value");
    TEST_ASSERT_EQ((uint32_t)after.some_total_ns, (uint32_t)before.some_total_ns,
                   "a quiet gap adds no stall time to the cumulative total");
}

/* A clock that did not advance must close no window and change nothing. The 50 ms
 * sampler calls the aggregator about forty times per window, so this is the
 * common case, not an edge case. */
static void test_stall_non_advancing_clock_closes_no_window(void)
{
    quota_stall_snapshot_t before, after;
    uint64_t windows_before;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    stall_step_windows(3);
    windows_before = quota_stall_windows_closed();
    stall_snap(QUOTA_STALL_MEM, &before);

    TEST_ASSERT_EQ(quota_stall_aggregate(), 0,
                   "an aggregate at the same timestamp closes no window");
    /* Part of a window is still not a window. */
    stall_advance(STALL_WINDOW_NS / 2u);
    TEST_ASSERT_EQ(quota_stall_aggregate(), 0,
                   "an aggregate part-way through a window closes no window");

    stall_snap(QUOTA_STALL_MEM, &after);
    TEST_ASSERT_EQ((uint32_t)quota_stall_windows_closed(), (uint32_t)windows_before,
                   "the window count is unchanged");
    TEST_ASSERT_EQ((uint32_t)after.some_avg[QUOTA_STALL_AVG10],
                   (uint32_t)before.some_avg[QUOTA_STALL_AVG10],
                   "the averages are unchanged");
    TEST_ASSERT_EQ((uint32_t)after.some_total_ns, (uint32_t)before.some_total_ns,
                   "the cumulative total is unchanged");
}

/* A clock that moved BACKWARDS must re-anchor rather than compute a negative
 * interval, which would wrap to an enormous elapsed value and fabricate a window. */
static void test_stall_backwards_clock_reanchors(void)
{
    quota_stall_snapshot_t before, after;
    uint64_t windows_before;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    stall_step_windows(3);
    windows_before = quota_stall_windows_closed();
    stall_snap(QUOTA_STALL_MEM, &before);

    g_now_ns = STALL_BASE_NS / 2u;
    quota_stall_test_set_clock(g_now_ns);
    TEST_ASSERT_EQ(quota_stall_aggregate(), 0,
                   "a backwards clock closes no window");
    TEST_ASSERT_EQ((uint32_t)quota_stall_windows_closed(), (uint32_t)windows_before,
                   "and fabricates no window count");

    stall_snap(QUOTA_STALL_MEM, &after);
    TEST_ASSERT_EQ((uint32_t)after.some_total_ns, (uint32_t)before.some_total_ns,
                   "and fabricates no stall time");

    /* Re-anchored, so the NEXT window measures from the new clock normally. */
    stall_advance(STALL_WINDOW_NS);
    TEST_ASSERT_EQ(quota_stall_aggregate(), 1,
                   "the aggregator resumes from the re-anchored clock");
}

/* The published last-update timestamp means "the most recent CLOSED window",
 * so it must stay zero until one actually closes -- otherwise a consumer cannot
 * tell "not started" from "started and quiet". */
static void test_stall_last_update_is_zero_until_a_window_closes(void)
{
    quota_stall_summary_t summary;

    stall_begin();
    TEST_ASSERT_EQ(quota_stall_snapshot_all(&summary), 0, "snapshot succeeds");
    TEST_ASSERT_EQ((uint32_t)summary.windows_closed, 0u,
                   "no window has closed yet");
    TEST_ASSERT_EQ((uint32_t)summary.last_update_ns, 0u,
                   "so the published timestamp is still zero, not the anchor");

    (void)stall_step_window();
    TEST_ASSERT_EQ(quota_stall_snapshot_all(&summary), 0, "snapshot succeeds");
    TEST_ASSERT_EQ((uint32_t)summary.windows_closed, 1u, "one window closed");
    TEST_ASSERT_EQ((uint32_t)summary.last_update_ns, (uint32_t)g_now_ns,
                   "and the timestamp is that window's close time");
}

/* Bad arguments are refused rather than dereferenced or indexed out of range. */
static void test_stall_rejects_bad_arguments(void)
{
    quota_stall_snapshot_t s;
    quota_stall_token_t tok;

    stall_begin();
    TEST_ASSERT_EQ(quota_stall_get(QUOTA_STALL_MEM, (quota_stall_snapshot_t *)0), -1,
                   "a NULL snapshot pointer is refused");
    TEST_ASSERT_EQ(quota_stall_get((quota_stall_domain_t)QUOTA_STALL_DOMAIN_COUNT, &s), -1,
                   "an out-of-range domain is refused");
    TEST_ASSERT_EQ(quota_stall_snapshot_all((quota_stall_summary_t *)0), -1,
                   "a NULL summary pointer is refused");

    tok = quota_stall_task_stalled((quota_stall_domain_t)QUOTA_STALL_DOMAIN_COUNT);
    TEST_ASSERT_EQ(tok.domain, (uint32_t)QUOTA_STALL_DOMAIN_COUNT,
                   "an out-of-range enter hands back an invalid token");
    quota_stall_task_unstalled(&tok);   /* must be a safe no-op */

    TEST_ASSERT_EQ((uint32_t)quota_stall_domain_name(QUOTA_STALL_CPU)[0], (uint32_t)'c',
                   "a valid domain names itself");
    TEST_ASSERT_EQ((uint32_t)quota_stall_domain_name(
                       (quota_stall_domain_t)QUOTA_STALL_DOMAIN_COUNT)[0], (uint32_t)'?',
                   "an out-of-range domain renders as unknown");

    /* A runnable delta at the extreme of its signed range must clamp, not
     * negate into undefined behavior or wrap the unsigned count. */
    quota_stall_runnable_delta((int32_t)0x80000000);   /* INT32_MIN */
    quota_stall_runnable_delta(1);
    quota_stall_runnable_delta(-1);
}

/* ==========================================================================
 * The level lane
 * ========================================================================== */

/* Sustained stall walks up through every level one step at a time, and quiet
 * walks it back down -- on this lane's own debounce counters, which is the whole
 * reason the lane is separate from the budget one. */
static void test_stall_level_rises_and_falls(void)
{
    quota_stall_snapshot_t s;
    quota_stall_token_t tok;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);
    stall_step_windows(120);

    stall_snap(QUOTA_STALL_MEM, &s);
    TEST_ASSERT_EQ((uint32_t)s.level, (uint32_t)QUOTA_PRESSURE_CRITICAL,
                   "sustained full-scale stall reaches critical");
    TEST_ASSERT_EQ((uint32_t)quota_stall_system_level(),
                   (uint32_t)QUOTA_PRESSURE_CRITICAL,
                   "the system level reflects the worst valid domain");

    quota_stall_task_unstalled(&tok);
    stall_step_windows(200);
    stall_snap(QUOTA_STALL_MEM, &s);
    TEST_ASSERT_EQ((uint32_t)s.level, (uint32_t)QUOTA_PRESSURE_NORMAL,
                   "sustained quiet walks the level back to normal");
}

/* The two lanes are INDEPENDENT, which is what lets each one be debounced
 * honestly. Forty budget passes -- one stall window's worth at the 50 ms sampler
 * cadence -- must not touch the stall lane's level, validity, or counters. The
 * discarded alternative (letting a stall observation hold a budget domain) is
 * documented at quota_pressure_submit_stall: it could suppress budget sampling
 * of a saturated domain entirely, so independence is the whole mechanism. */
static void test_stall_lane_independent_of_budget_lane(void)
{
    quota_stall_snapshot_t before, after;
    quota_stall_token_t tok;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);
    stall_step_windows(120);
    stall_snap(QUOTA_STALL_MEM, &before);
    TEST_ASSERT_EQ((uint32_t)before.level, (uint32_t)QUOTA_PRESSURE_CRITICAL,
                   "the stall lane reached critical on its own counters");

    for (uint32_t i = 0; i < 40u; i++)
        quota_pressure_sample_all();

    stall_snap(QUOTA_STALL_MEM, &after);
    TEST_ASSERT_EQ((uint32_t)after.level, (uint32_t)before.level,
                   "budget sampling cannot move the stall lane's level");
    TEST_ASSERT_EQ((uint32_t)after.valid, (uint32_t)before.valid,
                   "budget sampling cannot change the stall lane's validity");
    TEST_ASSERT_EQ((uint32_t)after.some_avg[QUOTA_STALL_AVG10],
                   (uint32_t)before.some_avg[QUOTA_STALL_AVG10],
                   "budget sampling cannot move the stall lane's averages");

    /* And the composite answer takes the worse of the two lanes, so a stalling
     * system is never reported calm just because every quota block is idle. */
    TEST_ASSERT_EQ((uint32_t)quota_pressure_system_level(),
                   (uint32_t)QUOTA_PRESSURE_CRITICAL,
                   "the system level folds in the stall lane as a max");
    quota_stall_task_unstalled(&tok);
}

/* ==========================================================================
 * Information class ABI
 * ========================================================================== */

/* Layer 3 of the 5-layer defense: the compile-time offset asserts are re-checked
 * at runtime, so a toolchain that laid the struct out differently than the
 * header claims is caught by a failing test rather than by a consumer decoding
 * the wrong field. */
static void test_stall_info_abi_pinned(void)
{
    TEST_ASSERT_EQ((uint32_t)sizeof(SYSTEM_RESOURCE_PRESSURE_DOMAIN), 56u,
                   "domain row is 56 bytes on the wire");
    TEST_ASSERT_EQ((uint32_t)sizeof(SYSTEM_RESOURCE_PRESSURE_INFORMATION), 248u,
                   "information class is 248 bytes on the wire");
    TEST_ASSERT_EQ((uint32_t)__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN,
                                                SomeTotalNs), 8u,
                   "SomeTotalNs sits at offset 8");
    TEST_ASSERT_EQ((uint32_t)__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN,
                                                SomeAvg10), 24u,
                   "SomeAvg10 sits at offset 24");
    TEST_ASSERT_EQ((uint32_t)__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN,
                                                Level), 36u,
                   "Level sits at offset 36");
    TEST_ASSERT_EQ((uint32_t)__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_INFORMATION,
                                                Domains), 48u,
                   "the row array starts at offset 48");
    TEST_ASSERT_EQ((uint32_t)SYSTEM_RESOURCE_PRESSURE_DOMAIN_COUNT,
                   (uint32_t)QUOTA_STALL_DOMAIN_COUNT,
                   "the wire row count matches the kernel domain count");
}

/* A sized query round-trips every field, and an undersized one reports the
 * required length instead of a partial copy. */
static void test_stall_info_class_roundtrip(void)
{
    SYSTEM_RESOURCE_PRESSURE_INFORMATION info;
    quota_stall_snapshot_t mem;
    quota_stall_token_t tok;
    uint32_t rl = 0;
    NTSTATUS st;

    stall_begin();
    quota_stall_declare_instrumented(QUOTA_STALL_MEM);
    tok = quota_stall_task_stalled(QUOTA_STALL_MEM);
    stall_step_windows(6);
    stall_snap(QUOTA_STALL_MEM, &mem);

    st = nt_query_resource_pressure_information(&info, (uint32_t)sizeof(info), &rl);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "a correctly sized query succeeds");
    TEST_ASSERT_EQ(rl, (uint32_t)sizeof(info),
                   "the returned length is the whole class");
    TEST_ASSERT_EQ((uint32_t)info.Version,
                   (uint32_t)SYSTEM_RESOURCE_PRESSURE_INFORMATION_VERSION,
                   "the version field is populated");
    TEST_ASSERT_EQ((uint32_t)info.Size, (uint32_t)sizeof(info),
                   "Size reports the struct size");
    TEST_ASSERT_EQ(info.DomainCount, (uint32_t)QUOTA_STALL_DOMAIN_COUNT,
                   "DomainCount lets a reader stride the rows");
    TEST_ASSERT_EQ(info.DomainSize,
                   (uint32_t)sizeof(SYSTEM_RESOURCE_PRESSURE_DOMAIN),
                   "DomainSize lets a reader stride the rows");
    TEST_ASSERT_EQ((uint32_t)info.UpdateIntervalNs, (uint32_t)QUOTA_STALL_UPDATE_NS,
                   "the aggregation cadence is published");
    TEST_ASSERT(info.WindowsClosed > 0, "closed windows are counted");
    TEST_ASSERT((info.Flags & SYSTEM_RESOURCE_PRESSURE_FLAG_ARMED) != 0,
                "the class reports itself armed once a window has closed");

    /* Row identity and the measured row's contents. */
    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++)
        TEST_ASSERT_EQ(info.Domains[d].Domain, d, "each row names its own domain");

    TEST_ASSERT((info.Domains[QUOTA_STALL_MEM].Flags &
                 SYSTEM_RESOURCE_PRESSURE_FLAG_VALID) != 0,
                "the instrumented row is marked valid");
    TEST_ASSERT((info.Domains[QUOTA_STALL_MEM].Flags &
                 SYSTEM_RESOURCE_PRESSURE_FLAG_INSTRUMENTED) != 0,
                "the instrumented row says a seam exists");
    TEST_ASSERT_EQ((uint32_t)info.Domains[QUOTA_STALL_MEM].SomeTotalNs,
                   (uint32_t)mem.some_total_ns,
                   "the row's some total matches the kernel snapshot");
    TEST_ASSERT_EQ((uint32_t)info.Domains[QUOTA_STALL_MEM].SomeAvg10,
                   (uint32_t)mem.some_avg[QUOTA_STALL_AVG10],
                   "the row's avg10 matches the kernel snapshot");
    TEST_ASSERT_EQ((uint32_t)info.Domains[QUOTA_STALL_MEM].SomeAvg300,
                   (uint32_t)mem.some_avg[QUOTA_STALL_AVG300],
                   "the row's avg300 matches the kernel snapshot");

    /* The uninstrumented rows carry the honest answer, not a calm-looking one. */
    TEST_ASSERT_EQ(info.Domains[QUOTA_STALL_IO].Flags &
                   SYSTEM_RESOURCE_PRESSURE_FLAG_VALID, 0u,
                   "an uninstrumented row is not marked valid");
    TEST_ASSERT((info.Domains[QUOTA_STALL_CPU].Flags &
                 SYSTEM_RESOURCE_PRESSURE_FLAG_FULL_UNDEFINED) != 0,
                "the cpu row declares its full metric undefined");
    TEST_ASSERT_EQ((uint32_t)info.Domains[QUOTA_STALL_CPU].FullAvg10, 0u,
                   "the cpu row's full average is zero on the wire");

    quota_stall_task_unstalled(&tok);
}

static void test_stall_info_class_rejects_short_buffer(void)
{
    SYSTEM_RESOURCE_PRESSURE_INFORMATION info;
    uint32_t rl = 0;
    NTSTATUS st;

    st = nt_query_resource_pressure_information(&info, 16u, &rl);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INFO_LENGTH_MISMATCH,
                   "an undersized buffer is refused");
    TEST_ASSERT_EQ(rl, (uint32_t)sizeof(info),
                   "the required length is reported before the capacity check");

    rl = 0;
    st = nt_query_resource_pressure_information((void *)0,
                                                (uint32_t)sizeof(info), &rl);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INFO_LENGTH_MISMATCH,
                   "a NULL buffer is refused rather than dereferenced");
}

/* LAST: hand the pressure transport and the clock back to the live kernel. */
static void test_stall_release_clock_and_transport(void)
{
    quota_stall_test_reset();
    quota_stall_test_set_clock(0);
    /* Clears the stall hold this file's arbitration test armed, so the live
     * kernel does not resume with a domain suppressed from budget sampling. */
    quota_pressure_test_reset();
    quota_pressure_test_release();
    TEST_ASSERT_EQ((uint32_t)quota_stall_windows_closed(), 0u,
                   "telemetry is returned to its boot state for the live kernel");
}

void test_register_quota_stall(void)
{
    test_suite_register_cat("Quota: stall domains are isolated",
                            test_stall_domain_isolation, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: overlapping waits are not double counted",
                            test_stall_overlapping_waits_not_double_counted, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: unbalanced stall leave clamps at zero",
                            test_stall_unbalanced_leave_clamps, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: exact permille is published exactly",
                            test_stall_exact_permille_is_published_exactly, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: exact-threshold stall crosses the band",
                            test_stall_exact_threshold_crosses, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: sub-permille stall is not erased",
                            test_stall_sub_permille_is_not_erased, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: double-consumed token cannot cancel another waiter",
                            test_stall_double_consume_cannot_cancel_another_waiter, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: uninstrumented stall domain reads unknown",
                            test_stall_uninstrumented_reports_unknown, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stall validity needs a closed window",
                            test_stall_validity_needs_a_closed_window, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: cpu.full is undefined at system level",
                            test_stall_cpu_full_is_undefined, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stall full requires nothing runnable",
                            test_stall_full_requires_nothing_runnable, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stall averages decay to exactly zero",
                            test_stall_averages_decay_to_exactly_zero, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stall averages saturate at full scale",
                            test_stall_averages_saturate_at_full_scale, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stall decays across missed periods",
                            test_stall_missed_periods_decay, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: over-long stalled gap keeps its measurement",
                            test_stall_over_long_stalled_gap_keeps_its_measurement, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: over-long quiet stall gap decays",
                            test_stall_over_long_quiet_gap_decays, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: non-advancing clock closes no stall window",
                            test_stall_non_advancing_clock_closes_no_window, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: backwards clock re-anchors stall telemetry",
                            test_stall_backwards_clock_reanchors, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stall last-update is zero until a window closes",
                            test_stall_last_update_is_zero_until_a_window_closes, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stall API rejects bad arguments",
                            test_stall_rejects_bad_arguments, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stall level rises and falls",
                            test_stall_level_rises_and_falls, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stall lane independent of budget lane",
                            test_stall_lane_independent_of_budget_lane, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: resource-pressure ABI pinned",
                            test_stall_info_abi_pinned, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: resource-pressure class round-trips",
                            test_stall_info_class_roundtrip, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: resource-pressure class rejects short buffer",
                            test_stall_info_class_rejects_short_buffer, TEST_CAT_QUOTA);

    /* LAST: returns the clock and the transport to the live kernel. */
    test_suite_register_cat("Quota: stall clock and transport released",
                            test_stall_release_clock_and_transport, TEST_CAT_QUOTA);
}

#endif /* KERNEL_TESTS */
