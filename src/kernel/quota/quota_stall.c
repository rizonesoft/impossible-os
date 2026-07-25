/* ============================================================================
 * quota_stall.c -- PSI-shaped resource-pressure stall telemetry.
 *
 * Two planes, and the split is the whole design:
 *
 *   PRODUCER plane -- quota_stall_task_stalled / _unstalled / _runnable_delta.
 *                     Runs on whichever CPU the stalling thread is on, at
 *                     whatever IRQL that path runs at (a scheduler tick, an
 *                     allocator wait, a block-I/O completion wait). Takes ONLY
 *                     that CPU's own lock, so producers on different CPUs never
 *                     contend and no cache line bounces between them.
 *   AGGREGATE plane -- quota_stall_aggregate. Runs every two seconds on the one
 *                     service CPU that drives the pressure sampler. Walks the
 *                     per-CPU integrals, folds them into totals, averages, and
 *                     this lane's hysteresis.
 *
 * Lock order (the only two locks this file takes):
 *     aggregate lock  ->  per-CPU lock
 * Producers take a per-CPU lock alone, so the order can never invert. Neither
 * lock is ever held across a call out of this file, and no quota block lock is
 * ever taken here.
 *
 * INTEGRATION, NOT SUMMATION. A producer never reports a duration. It reports
 * that its CPU's stalled count changed, and the CPU integrates elapsed wall
 * time against the count it had BEFORE the change. Ten threads blocked for the
 * same second therefore contribute one second, not ten -- the metric is a
 * fraction of real time by construction rather than by a clamp bolted on
 * afterwards.
 *
 * INDEPENDENT OF THE BUDGET LANE. quota_pressure.c derives levels from budget
 * saturation on a 50 ms cadence. If stall observations were fed into those same
 * domains, roughly forty budget samples would land between two stall updates and
 * would reset a pending stall rise (or complete a five-sample fall) using an
 * unrelated signal. This lane keeps its own validity, sample, debounce counters
 * and level; the two meet only in quota_pressure_system_level(), as the worse of
 * the two.
 * ========================================================================== */

#include "kernel/quota/quota_stall.h"
#include "kernel/klog.h"
#include "kernel/timer.h"                /* uptime_ns */
#include "kernel/smp.h"                  /* smp_this_cpu, smp_cpu_count, MAX_CPUS */
#include "kernel/sched/spinlock.h"

/* Freestanding: no string.h. Declared locally, same as every other kernel TU
 * that needs it (quota_pressure.c does the same). */
extern void *memset(void *s, int c, size_t n);

/* ==========================================================================
 * Per-CPU producer state
 * ========================================================================== */

/* One CPU's state per cache line. The producers are the scheduler, the
 * allocator, and the I/O completion path -- three of the hottest paths in the
 * kernel -- so two CPUs' counters sharing a line would turn every stall
 * transition into a cross-CPU invalidation for no reason. */
#define QUOTA_STALL_CPU_ALIGN  64u

/* Counts are clamped rather than allowed to wrap. An unbalanced producer is a
 * bug in the producer, but a wrapped count would turn "nothing is stalled" into
 * "four billion things are stalled" and pin the domain at CRITICAL for the rest
 * of the boot -- a far worse failure than a stuck-high count. */
#define QUOTA_STALL_COUNT_MAX  0xFFFFFFFFu

typedef struct stall_cpu {
    spinlock_t lock;
    uint64_t   last_ns;       /* integration point; 0 = not seeded yet        */
    uint32_t   nr_stalled[QUOTA_STALL_DOMAIN_COUNT];
    uint32_t   nr_running;    /* tasks that could otherwise make progress     */
    uint64_t   some_ns[QUOTA_STALL_DOMAIN_COUNT];   /* cumulative, saturating */
    uint64_t   full_ns[QUOTA_STALL_DOMAIN_COUNT];   /* cumulative, saturating */
    /* Time this CPU had ANY measurable activity -- something stalled or
     * something runnable. It is the denominator the ratios are taken against;
     * see weighted_ratio_q16 for how it is used and why the naive denominator
     * is wrong. */
    uint64_t   nonidle_ns;
    /* Pad the ELEMENT to a whole cache line. Aligning only the array base
     * aligns element 0 and nothing else: at 88 bytes of payload, CPU 1's lock
     * would share a line with CPU 0's counters and every stall transition on one
     * core would invalidate the other's line -- the exact false sharing the
     * per-CPU split exists to avoid. */
    uint8_t    _pad[(2u * QUOTA_STALL_CPU_ALIGN) -
                    (sizeof(spinlock_t) + 2u * sizeof(uint64_t) +
                     (QUOTA_STALL_DOMAIN_COUNT + 1u) * sizeof(uint32_t) +
                     2u * QUOTA_STALL_DOMAIN_COUNT * sizeof(uint64_t))];
} __attribute__((aligned(QUOTA_STALL_CPU_ALIGN))) stall_cpu_t;

/* Named g_stall_cpu, not g_cpu: cpuid.h publishes a global `g_cpu` feature
 * block, and smp.h drags it in. */
static stall_cpu_t g_stall_cpu[MAX_CPUS];

/* Both halves of the isolation claim, asserted rather than asserted-in-prose:
 * the element must START on a cache line and must OCCUPY whole cache lines. */
_Static_assert(__builtin_offsetof(stall_cpu_t, _pad) < sizeof(stall_cpu_t),
    "per-CPU stall padding must not overflow the element");
_Static_assert(sizeof(stall_cpu_t) % QUOTA_STALL_CPU_ALIGN == 0,
    "per-CPU stall state must occupy whole cache lines (no false sharing)");
_Static_assert(_Alignof(stall_cpu_t) >= QUOTA_STALL_CPU_ALIGN,
    "per-CPU stall state must start on a cache line");

/* ==========================================================================
 * Aggregated state
 * ========================================================================== */

typedef struct stall_domain {
    uint64_t some_total_ns;   /* cumulative across CPUs, saturating           */
    uint64_t full_total_ns;
    uint64_t some_avg[QUOTA_STALL_WINDOW_COUNT];  /* permille << AVG_SHIFT    */
    uint64_t full_avg[QUOTA_STALL_WINDOW_COUNT];
    uint8_t  level;           /* quota_pressure_level_t                       */
    uint8_t  valid;           /* a seam exists AND a window has closed        */
    uint8_t  instrumented;    /* a seam declared itself                       */
    uint8_t  _pad;
    uint16_t rise_count;
    uint16_t fall_count;
    uint16_t last_permille;   /* the avg10 `some` value the level was stepped on */
    uint16_t _pad2;
} stall_domain_t;

static spinlock_t     g_agg_lock = SPINLOCK_INIT;
static stall_domain_t g_domain[QUOTA_STALL_DOMAIN_COUNT];
/* PRIVATE aggregation anchor: where the current window started. Set at init and
 * moved on every fold, including the ones that close no window. */
static uint64_t       g_anchor_ns;
/* PUBLISHED timestamp of the most recent CLOSED window. Kept separate from the
 * anchor because the public contract is "0 = no window has closed yet", and
 * publishing the anchor would report a non-zero time from init onward -- so a
 * consumer could not tell "not started" from "started and quiet". */
static uint64_t       g_last_closed_ns;
static uint64_t       g_windows_closed;

/* Decay coefficient per averaging window, indexed by quota_stall_window_t. */
static const uint64_t g_decay[QUOTA_STALL_WINDOW_COUNT] = {
    QUOTA_STALL_EXP_10S,
    QUOTA_STALL_EXP_60S,
    QUOTA_STALL_EXP_300S
};

/* Rise / fall thresholds indexed by the level being entered / left, mirroring
 * quota_pressure.c so the two lanes cross the same bands. Index 0 is unused
 * (there is no threshold to enter NORMAL from below) and present only so the
 * arrays index directly by level. */
static const uint16_t g_rise_permille[QUOTA_PRESSURE_LEVEL_COUNT] = {
    0,
    QUOTA_PRESSURE_RISE_WATCH_PERMILLE,
    QUOTA_PRESSURE_RISE_WARNING_PERMILLE,
    QUOTA_PRESSURE_RISE_CRITICAL_PERMILLE
};
static const uint16_t g_fall_permille[QUOTA_PRESSURE_LEVEL_COUNT] = {
    0,
    QUOTA_PRESSURE_FALL_WATCH_PERMILLE,
    QUOTA_PRESSURE_FALL_WARNING_PERMILLE,
    QUOTA_PRESSURE_FALL_CRITICAL_PERMILLE
};

static const char *const g_domain_name[QUOTA_STALL_DOMAIN_COUNT] = {
    "cpu", "mem", "io"
};

/* An enum that grows must grow every array indexed by it. */
_Static_assert(sizeof(g_decay) / sizeof(g_decay[0]) == QUOTA_STALL_WINDOW_COUNT,
    "decay table must cover every averaging window");
_Static_assert(sizeof(g_domain_name) / sizeof(g_domain_name[0]) == QUOTA_STALL_DOMAIN_COUNT,
    "domain name table must cover every stall domain");
_Static_assert(sizeof(g_rise_permille) / sizeof(g_rise_permille[0]) == QUOTA_PRESSURE_LEVEL_COUNT &&
               sizeof(g_fall_permille) / sizeof(g_fall_permille[0]) == QUOTA_PRESSURE_LEVEL_COUNT,
    "stall threshold tables must cover every pressure level");

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static int domain_valid(quota_stall_domain_t domain)
{
    return (uint32_t)domain < (uint32_t)QUOTA_STALL_DOMAIN_COUNT;
}

/* `full` means "everything that could run was stalled". At the SYSTEM level
 * that has no meaning for the CPU domain -- some task is by definition running,
 * or the CPU would be idle rather than contended -- so Linux reports cpu.full as
 * zero and documents it as undefined. Reporting a plausible-looking number
 * instead would be a fabrication a consumer could act on. */
static int domain_full_defined(quota_stall_domain_t domain)
{
    return domain != QUOTA_STALL_CPU;
}

static uint64_t sat_add_u64(uint64_t a, uint64_t b)
{
    uint64_t sum = a + b;
    return (sum < a) ? 0xFFFFFFFFFFFFFFFFull : sum;
}

#ifdef KERNEL_TESTS
/* Test clock override. Every internal time read goes through stall_now_ns(), so
 * a test drives the REAL producer and aggregation paths at synthetic timestamps
 * instead of a parallel injection seam that would prove nothing about them. */
static uint64_t g_test_clock_ns;
#endif

static uint64_t stall_now_ns(void)
{
#ifdef KERNEL_TESTS
    uint64_t forced = __atomic_load_n(&g_test_clock_ns, __ATOMIC_RELAXED);
    if (forced != 0)
        return forced;
#endif
    return uptime_ns();
}

/* PIN the calling CPU, then resolve its slot. Returns the slot's state, or NULL
 * when there is no per-CPU area to attribute to; on success the caller must
 * local_irq_restore(*irq_flags) after releasing the slot lock.
 *
 * The pin is load-bearing, not defensive. Involuntary preemption on this kernel
 * is interrupt-driven, so masking local interrupts fixes the CPU identity for
 * the whole update. Without it, a migration between reading smp_this_cpu() and
 * acquiring that slot's lock would land a stall on one slot while the runnable
 * count that qualifies it stays on another -- and since the aggregator weights
 * each CPU's stall by that SAME CPU's non-idle time, the pair being split across
 * slots does not merely relabel the interval, it changes the answer. Two CPUs,
 * one running 2 s with 1 s of stall: correctly paired that is 500 permille;
 * split across slots the weights become 2048 and 4096 and it reads 166. An
 * earlier revision left this unpinned on the argument that the aggregator summed
 * everything anyway, which was true only while the combination was an unweighted
 * sum. */
static stall_cpu_t *stall_pin_this_cpu(uint64_t *irq_flags, uint32_t *out_slot)
{
    struct per_cpu_data *pc;
    uint64_t flags = local_irq_save();

    /* GS_BASE is programmed in early BSP init; a producer that somehow ran
     * before that has no CPU to attribute to. Dropping the sample is correct --
     * attributing it to CPU 0 would corrupt a real CPU's integral. */
    pc = smp_this_cpu();
    if (!pc || pc->cpu_id >= (uint32_t)MAX_CPUS) {
        local_irq_restore(flags);
        return (stall_cpu_t *)0;
    }
    *irq_flags = flags;
    *out_slot  = pc->cpu_id;
    return &g_stall_cpu[pc->cpu_id];
}

/* Integrate elapsed wall time into `c`'s cumulative totals using the counts it
 * held BEFORE this instant, then move the integration point. Caller holds
 * `c->lock`. */
static void stall_cpu_integrate_locked(stall_cpu_t *c, uint64_t now)
{
    uint64_t delta;
    int      counted_nonidle = 0;

    /* Unseeded, or a clock that did not advance (a test clock reset, or two
     * reads inside the same timer granule). Re-anchor and charge nothing: the
     * interval before the first observation is not a measurement. */
    if (c->last_ns == 0 || now <= c->last_ns) {
        c->last_ns = now;
        return;
    }
    delta = now - c->last_ns;
    c->last_ns = now;

    if (c->nr_running != 0)
        c->nonidle_ns = sat_add_u64(c->nonidle_ns, delta);

    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
        if (c->nr_stalled[d] == 0)
            continue;
        /* Stalled implies non-idle. Charged once per interval no matter how
         * many domains are stalled, which is what keeps the denominator a
         * measure of TIME rather than of domain-time. */
        if (c->nr_running == 0 && !counted_nonidle) {
            c->nonidle_ns = sat_add_u64(c->nonidle_ns, delta);
            counted_nonidle = 1;
        }
        c->some_ns[d] = sat_add_u64(c->some_ns[d], delta);
        /* FULL implies SOME by construction: it is only reached from inside the
         * same non-zero-stalled branch. Enforcing it here rather than trusting
         * the producer means an inconsistent seam cannot publish a full that
         * exceeds its own some. */
        if (c->nr_running == 0 &&
            domain_full_defined((quota_stall_domain_t)d))
            c->full_ns[d] = sat_add_u64(c->full_ns[d], delta);
    }
}

/* decay^n in 1/2^32 fixed point, by binary exponentiation.
 *
 * Every intermediate is a 32-bit fixed-point fraction, so the product of two of
 * them is at most 2^64 and the >> 32 brings it back into range exactly. At most
 * 64 iterations for any n, which is what lets the caller apply an arbitrary
 * number of missed periods in CLOSED FORM instead of looping over them. Once the
 * result underflows to zero the answer is simply "all of the old value has
 * decayed", which is the correct limit. */
static uint64_t pow_q32(uint64_t base, uint64_t n)
{
    uint64_t result = QUOTA_STALL_FP_ONE;   /* 1.0 */

    while (n != 0) {
        if (n & 1ull)
            result = (result * base) >> QUOTA_STALL_FP_SHIFT;
        base = (base * base) >> QUOTA_STALL_FP_SHIFT;
        n >>= 1;
        if (base == 0)          /* fully decayed; further squaring stays zero */
            break;
    }
    /* Finish any remaining odd bits after base underflowed: they would all
     * multiply result by zero. */
    if (n != 0)
        result = 0;
    return result;
}

/* Apply `periods` EWMA steps of a CONSTANT sample, in closed form.
 *
 *     avg_n = avg_0 * d^n + sample * (1 - d^n)
 *
 * which is exactly what the step-by-step recurrence converges to, computed in a
 * handful of multiplies instead of n of them. This is what removed the old
 * catch-up clamp: an earlier revision stepped the loop at most 150 times and
 * claimed the averages had converged by then, but with the 300 s coefficient
 * d^150 is 0.368 -- more than a third of the old value still standing. A gap
 * longer than the clamp therefore published an average that was neither the old
 * one nor the new one. In closed form there is no bound to get wrong.
 *
 * The shift FLOORS, which is what keeps zero reachable: a residue of one
 * fixed-point unit decays to (1 * d) >> 32 == 0 rather than rounding back to
 * itself forever. */
static uint64_t ewma_apply(uint64_t avg, uint64_t sample_fp, uint64_t decay,
                           uint64_t periods)
{
    uint64_t d_n = pow_q32(decay, periods);

    uint64_t kept  = (avg * d_n) >> QUOTA_STALL_FP_SHIFT;
    uint64_t added = (sample_fp * (QUOTA_STALL_FP_ONE - d_n)) >> QUOTA_STALL_FP_SHIFT;
    return kept + added;
}

/* Narrow a carried average to the reported permille unit, ROUNDING to nearest.
 *
 * Truncating here would be a second floor on a value ewma_apply already floored,
 * and the two together are not a rounding nuisance -- they are an off-by-one on
 * every exact sample. An EWMA fed a constant sample S converges from below and
 * never quite reaches it: with the 10 s coefficient a sustained exact 700
 * permille settles at 699.999847. Truncating publishes 699 forever, so a
 * workload sitting precisely on the WATCH rise threshold never crosses it, and a
 * steady one-permille stall reports zero.
 *
 * The floor INSIDE ewma_apply stays, because that is what lets a decaying
 * average reach exactly zero; rounding belongs at the publication boundary,
 * where the question is which whole permille the carried value is nearest. The
 * hysteresis reads through here too, so the bands are crossed on the same number
 * a consumer sees. */
static uint16_t avg_to_permille(uint64_t avg)
{
    uint64_t permille = (avg + (QUOTA_STALL_AVG_ONE / 2u)) >> QUOTA_STALL_AVG_SHIFT;

    return (permille > (uint64_t)QUOTA_STALL_PERMILLE_MAX)
         ? (uint16_t)QUOTA_STALL_PERMILLE_MAX : (uint16_t)permille;
}

/* Step one domain's hysteresis by at most one level, on the same bands and
 * debounce depths the budget lane uses. Caller holds the aggregate lock. */
static void stall_level_step(stall_domain_t *sd, uint16_t permille)
{
    uint8_t level = sd->level;

    sd->last_permille = permille;

    if (level < (uint8_t)QUOTA_PRESSURE_CRITICAL &&
        permille >= g_rise_permille[level + 1]) {
        sd->fall_count = 0;
        if (++sd->rise_count >= (uint16_t)QUOTA_PRESSURE_RISE_SAMPLES) {
            sd->rise_count = 0;
            sd->level = (uint8_t)(level + 1);
        }
    } else if (level > (uint8_t)QUOTA_PRESSURE_NORMAL &&
               permille < g_fall_permille[level]) {
        sd->rise_count = 0;
        if (++sd->fall_count >= (uint16_t)QUOTA_PRESSURE_FALL_SAMPLES) {
            sd->fall_count = 0;
            sd->level = (uint8_t)(level - 1);
        }
    } else {
        /* Inside the band for the current level: neither direction is being
         * sustained, so both counters reset. Letting them persist would let
         * unrelated excursions minutes apart add up to a transition. */
        sd->rise_count = 0;
        sd->fall_count = 0;
    }
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

void quota_stall_init(void)
{
    uint64_t flags;
    uint64_t now = stall_now_ns();

    spin_lock_irqsave(&g_agg_lock, &flags);
    g_anchor_ns = now;
    spin_unlock_irqrestore(&g_agg_lock, flags);

    klog(LOG_INFO, "quota",
         "stall telemetry armed (%u domains, %u ms window)\n",
         (unsigned)QUOTA_STALL_DOMAIN_COUNT, (unsigned)QUOTA_STALL_UPDATE_MS);
}

/* ==========================================================================
 * Producer seam
 * ========================================================================== */

void quota_stall_declare_instrumented(quota_stall_domain_t domain)
{
    uint64_t flags;

    if (!domain_valid(domain))
        return;

    spin_lock_irqsave(&g_agg_lock, &flags);
    g_domain[domain].instrumented = 1;
    spin_unlock_irqrestore(&g_agg_lock, flags);
}

quota_stall_token_t quota_stall_task_stalled(quota_stall_domain_t domain)
{
    quota_stall_token_t token = quota_stall_token_none();
    stall_cpu_t        *c;
    uint64_t            irq_flags;
    uint64_t            flags;
    uint32_t            slot;

    if (!domain_valid(domain))
        return token;
    c = stall_pin_this_cpu(&irq_flags, &slot);
    if (!c)
        return token;

    spin_lock_irqsave(&c->lock, &flags);
    stall_cpu_integrate_locked(c, stall_now_ns());
    if (c->nr_stalled[domain] < QUOTA_STALL_COUNT_MAX)
        c->nr_stalled[domain]++;
    spin_unlock_irqrestore(&c->lock, flags);
    local_irq_restore(irq_flags);

    /* The token names the slot that was actually incremented, so the matching
     * leave decrements THIS counter even if the task resumes on another CPU. */
    token.cpu    = slot;
    token.domain = (uint32_t)domain;
    return token;
}

void quota_stall_task_unstalled(quota_stall_token_t *token)
{
    stall_cpu_t *c;
    uint64_t     flags;
    uint32_t     cpu, domain;

    if (!token)
        return;

    /* An unrecorded enter hands back an invalid token; consuming it must be a
     * no-op so a producer can call leave unconditionally on its exit path. */
    if (token->domain >= (uint32_t)QUOTA_STALL_DOMAIN_COUNT ||
        token->cpu >= (uint32_t)MAX_CPUS)
        return;

    cpu    = token->cpu;
    domain = token->domain;

    /* CONSUME the token before touching the counter. The count carries no
     * per-token identity, so a token consumed twice would decrement a second,
     * still-blocked task's stall -- and the zero-clamp would not catch it,
     * because with two waiters the count never reaches zero. Invalidating the
     * caller's copy is what makes the documented idempotence real. */
    token->domain = (uint32_t)QUOTA_STALL_DOMAIN_COUNT;

    /* Not necessarily the CURRENT CPU: this is the cross-CPU case the token
     * exists for. The per-CPU lock makes it safe -- it is a normal irqsave
     * spinlock, not a "only the owner touches this" convention -- and the
     * contention is one lock per migrated wait, against an aggregator that runs
     * every two seconds. */
    c = &g_stall_cpu[cpu];
    spin_lock_irqsave(&c->lock, &flags);
    stall_cpu_integrate_locked(c, stall_now_ns());
    /* Still clamped: a copied-then-double-consumed token, or a producer that
     * fabricates one, must not wrap the count to ~4 billion and pin the domain
     * saturated for the rest of the boot. */
    if (c->nr_stalled[domain] > 0)
        c->nr_stalled[domain]--;
    spin_unlock_irqrestore(&c->lock, flags);
}

/* No ownership token here, and none is needed: a runnable count is a property of
 * a CPU's run queue rather than of a task, so the producer that changes it is by
 * definition reporting about the CPU it is running on. */
void quota_stall_runnable_delta(int32_t delta)
{
    stall_cpu_t *c;
    uint64_t     irq_flags;
    uint64_t     flags;
    uint32_t     slot;

    if (delta == 0)
        return;
    /* Pinned for the same reason the stall enter is: the runnable count is half
     * of the pair the aggregator weights, and splitting it from the stall it
     * qualifies changes the measurement rather than just relabelling it. */
    c = stall_pin_this_cpu(&irq_flags, &slot);
    if (!c)
        return;

    spin_lock_irqsave(&c->lock, &flags);
    stall_cpu_integrate_locked(c, stall_now_ns());
    if (delta > 0) {
        uint32_t room = QUOTA_STALL_COUNT_MAX - c->nr_running;
        c->nr_running += ((uint32_t)delta < room) ? (uint32_t)delta : room;
    } else {
        /* -delta computed in a wider signed type first: negating INT32_MIN in
         * int32_t is undefined, and the count is unsigned anyway. */
        uint64_t drop = (uint64_t)(-(int64_t)delta);
        c->nr_running = (drop < (uint64_t)c->nr_running)
                      ? (uint32_t)(c->nr_running - drop) : 0u;
    }
    spin_unlock_irqrestore(&c->lock, flags);
    local_irq_restore(irq_flags);
}

/* ==========================================================================
 * Aggregation
 * ========================================================================== */

/* Snapshot every CPU's cumulative integrals, bringing each up to `now` first so
 * a stall in progress is attributed to the window it actually spans rather than
 * landing entirely in the next one. Caller holds the aggregate lock; each CPU
 * lock is taken and released inside the loop, so the aggregate lock is the only
 * one held across the walk. */
/* One window's per-CPU deltas. Kept per CPU rather than summed on the spot
 * because the combination is WEIGHTED by each CPU's non-idle time, so the
 * association between a CPU's stall and that same CPU's activity has to survive
 * until the weighting is applied. */
typedef struct stall_window {
    uint64_t some[MAX_CPUS][QUOTA_STALL_DOMAIN_COUNT];
    uint64_t full[MAX_CPUS][QUOTA_STALL_DOMAIN_COUNT];
    uint64_t nonidle[MAX_CPUS];
} stall_window_t;

/* Aggregator-private: the previous reading of each CPU's cumulative counters, so
 * a window measures GROWTH. Per CPU rather than summed, for the reason above. */
static uint64_t g_prev_some_ns[MAX_CPUS][QUOTA_STALL_DOMAIN_COUNT];
static uint64_t g_prev_full_ns[MAX_CPUS][QUOTA_STALL_DOMAIN_COUNT];
static uint64_t g_prev_nonidle_ns[MAX_CPUS];

static uint64_t growth_of(uint64_t now_val, uint64_t *prev)
{
    /* A cumulative counter can only decrease if a test reset the per-CPU state
     * under a live aggregator. Re-anchor rather than underflowing to ~2^64. */
    uint64_t g = (now_val >= *prev) ? (now_val - *prev) : 0u;
    *prev = now_val;
    return g;
}

static void collect_percpu(uint64_t now, stall_window_t *win)
{
    /* Walk EVERY slot, not just the currently-online count. A CPU that went
     * offline still holds the stall time it accumulated while it was up, and
     * dropping it would make that slot's cumulative counter appear to go
     * backwards -- read as a re-anchor, silently discarding a window. */
    for (uint32_t i = 0; i < (uint32_t)MAX_CPUS; i++) {
        stall_cpu_t *c = &g_stall_cpu[i];
        uint64_t     flags;
        uint64_t     cur_some[QUOTA_STALL_DOMAIN_COUNT];
        uint64_t     cur_full[QUOTA_STALL_DOMAIN_COUNT];
        uint64_t     cur_nonidle;

        spin_lock_irqsave(&c->lock, &flags);
        stall_cpu_integrate_locked(c, now);
        for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
            cur_some[d] = c->some_ns[d];
            cur_full[d] = c->full_ns[d];
        }
        cur_nonidle = c->nonidle_ns;
        spin_unlock_irqrestore(&c->lock, flags);

        /* Differencing happens OUTSIDE the per-CPU lock: it touches only
         * aggregator-private state, and holding a producer's lock across it
         * would put the scheduler and I/O paths behind bookkeeping. */
        for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
            win->some[i][d] = growth_of(cur_some[d], &g_prev_some_ns[i][d]);
            win->full[i][d] = growth_of(cur_full[d], &g_prev_full_ns[i][d]);
        }
        win->nonidle[i] = growth_of(cur_nonidle, &g_prev_nonidle_ns[i]);
    }
}

/* Combine one domain's per-CPU stall times into a single time comparable to the
 * wall interval, weighting each CPU by how busy it was:
 *
 *     stall = sum(stall_i * nonidle_i) / sum(nonidle_i)
 *
 * This is Linux PSI's formula, and the weighting is the whole point. The naive
 * alternative, sum(stall_i) / sum(nonidle_i), silently answers a different
 * question. Two CPUs over a 2 s window, CPU 0 non-idle and stalled for 1 s,
 * CPU 1 running the full 2 s: the naive form reports 333 permille, the weighted
 * form 167. The naive number is not a fraction of anything a reader can name --
 * it divides one CPU's stall by two CPUs' activity.
 *
 * Overflow safety: weights are normalized against the busiest CPU into 0..4096,
 * and each CPU's stall is converted to a 0..1000-permille ratio (in 2^-16 fixed
 * point) BEFORE weighting. Every product is therefore bounded by 1000 * 65536 *
 * 4096 * MAX_CPUS, about 2^42, no matter how long the interval was. */
/* One CPU's stall as a fraction of the interval, in permille scaled by 2^16.
 *
 * The multiply MUST come before the divide. Computing (stall << 16) / elapsed
 * first and scaling to permille afterwards throws away everything below one part
 * in 65536 of the interval, which is coarser than the permille unit it is meant
 * to refine: a stall of exactly one permille (2,000,001 ns of a 2 s window)
 * lands on floor(65.5) = 65, and 65 * 1000 >> 16 is ZERO. A domain stalling at
 * one permille forever would have reported nothing at all, averages and
 * cumulative totals alike.
 *
 * Overflow is avoided by quotient/remainder decomposition rather than a wider
 * type. With stall <= elapsed <= QUOTA_STALL_MAX_INTERVAL_NS (4.32e13):
 *   stall * 1000            <= 4.32e16   fits
 *   remainder << 16         <  elapsed << 16 <= 2.83e18   fits
 * so every intermediate stays inside 64 bits with three orders of magnitude to
 * spare. */
static uint64_t stall_ratio_q16(uint64_t stall, uint64_t elapsed)
{
    uint64_t num, whole, rem;

    if (elapsed == 0)
        return 0;
    if (stall > elapsed)
        stall = elapsed;   /* a CPU cannot stall for longer than the interval */

    num   = stall * (uint64_t)QUOTA_STALL_PERMILLE_MAX;
    whole = num / elapsed;                 /* 0 .. 1000  */
    rem   = num % elapsed;
    return (whole << QUOTA_STALL_AVG_SHIFT) +
           ((rem << QUOTA_STALL_AVG_SHIFT) / elapsed);
}

/* Combine one domain's per-CPU stall ratios into a single figure, weighting each
 * CPU by how busy it was:
 *
 *     stall = sum(stall_i * nonidle_i) / sum(nonidle_i)
 *
 * This is Linux PSI's formula, and the weighting is the whole point. The naive
 * alternative, sum(stall_i) / sum(nonidle_i), silently answers a different
 * question. Two CPUs over a 2 s window, CPU 0 non-idle and stalled for 1 s,
 * CPU 1 running the full 2 s: the naive form reports 333 permille, the weighted
 * form 167. The naive number is not a fraction of anything a reader can name --
 * it divides one CPU's stall by two CPUs' activity.
 *
 * Returns permille in 2^-16 fixed point, NOT a whole permille: the caller feeds
 * it to the EWMA and to the cumulative total, and rounding to a whole permille
 * here would erase every sub-permille window before either of them saw it.
 *
 * Overflow: ratios are bounded by 1000 << 16 (about 6.6e7) and weights by 4096,
 * so the accumulator is bounded by 6.6e7 * 4096 * MAX_CPUS, about 4.3e12,
 * regardless of how long the interval was. */
static uint64_t weighted_ratio_q16(const uint64_t *stall_per_cpu, uint32_t stride,
                                   const uint64_t *weight, uint64_t sum_weight,
                                   uint64_t elapsed)
{
    uint64_t acc = 0;

    if (sum_weight == 0 || elapsed == 0)
        return 0;

    for (uint32_t i = 0; i < (uint32_t)MAX_CPUS; i++) {
        uint64_t s = stall_per_cpu[(uint64_t)i * stride];

        if (weight[i] == 0 || s == 0)
            continue;
        acc += stall_ratio_q16(s, elapsed) * weight[i];
    }
    acc /= sum_weight;
    return (acc > QUOTA_STALL_AVG_MAX) ? QUOTA_STALL_AVG_MAX : acc;
}

/* Convert a Q16 permille ratio over `elapsed` back into nanoseconds.
 *
 * Split into whole and fractional permille so the product cannot overflow for
 * any interval the ceiling admits: (ratio >> 16) <= 1000 times (elapsed / 1000)
 * <= 4.32e10 is 4.32e13, and the fractional term is bounded by 65535 times the
 * same figure before its own shift. */
static uint64_t ratio_q16_to_ns(uint64_t ratio_q16, uint64_t elapsed)
{
    uint64_t per_permille = elapsed / (uint64_t)QUOTA_STALL_PERMILLE_MAX;
    uint64_t whole = ratio_q16 >> QUOTA_STALL_AVG_SHIFT;
    uint64_t frac  = ratio_q16 & (QUOTA_STALL_AVG_ONE - 1u);

    return whole * per_permille +
           ((frac * per_permille) >> QUOTA_STALL_AVG_SHIFT);
}

/* Fold one interval's measurement into a domain's averages.
 *
 * `periods` is how many aggregation periods the interval covered, and the sample
 * is applied ONCE PER PERIOD rather than once in total. The difference is not
 * cosmetic: an earlier revision decayed for the missed periods and then applied
 * the interval's ratio a single time, so forty seconds of continuous full-scale
 * stall arriving as one delayed aggregation moved avg10 from 0 to about 83
 * permille instead of about 825, and handed the hysteresis one qualifying rise
 * instead of twenty. A sampler that fell behind would therefore under-report
 * exactly the sustained pressure it exists to catch.
 *
 * The ratio is the interval AVERAGE, so attributing it uniformly to every period
 * it covers is the honest reconstruction: the distribution inside the gap is not
 * recoverable, and assuming it was uniform neither invents pressure nor hides
 * it. */
static void apply_window(stall_domain_t *sd, uint64_t some_fp,
                         uint64_t full_fp, uint64_t periods, int step_level)
{
    for (uint32_t w = 0; w < (uint32_t)QUOTA_STALL_WINDOW_COUNT; w++) {
        sd->some_avg[w] = ewma_apply(sd->some_avg[w], some_fp, g_decay[w], periods);
        sd->full_avg[w] = ewma_apply(sd->full_avg[w], full_fp, g_decay[w], periods);
    }

    /* The hysteresis advances ONCE per aggregation, however many periods the
     * interval spanned. A delayed aggregation produces one number -- the
     * interval AVERAGE -- and the per-CPU counters retain no record of how the
     * stall was distributed inside it. Feeding that one number to the debounce
     * N times would fabricate N consecutive observations that were never made,
     * and the debounce exists precisely to require consecutiveness: an interval
     * averaging 800 permille might have been sixteen saturated periods followed
     * by four quiet ones, and pretending it was twenty identical ones can move
     * the level in either direction on evidence that does not exist.
     *
     * The consequence is honest and deliberate: after a sampler outage the level
     * takes its normal debounce to catch up, because one observation is all that
     * was actually made. The AVERAGES do catch up in full (they are a function
     * of elapsed time, not of observation count), so nothing is lost from the
     * metric itself. */
    if (step_level)
        stall_level_step(sd, avg_to_permille(sd->some_avg[QUOTA_STALL_AVG10]));
}

int quota_stall_aggregate(void)
{
    stall_window_t win;
    uint64_t weight[MAX_CPUS];
    uint64_t sum_weight = 0;
    uint64_t max_nonidle = 0;
    uint64_t flags;
    uint64_t now = stall_now_ns();
    uint64_t elapsed;
    uint64_t periods;

    spin_lock_irqsave(&g_agg_lock, &flags);

    /* Unseeded, or a clock that moved backwards (only a test clock can do
     * that). Re-anchor and discard the interval rather than dividing by it. */
    if (g_anchor_ns == 0 || now < g_anchor_ns) {
        g_anchor_ns = now;
        spin_unlock_irqrestore(&g_agg_lock, flags);
        return 0;
    }

    elapsed = now - g_anchor_ns;
    if (elapsed < QUOTA_STALL_UPDATE_NS) {
        spin_unlock_irqrestore(&g_agg_lock, flags);
        return 0;   /* the 50 ms sampler calls this 40 times per window */
    }

    /* An interval this long is not a delayed sample, it is a broken clock. The
     * arithmetic below would still be defined, but the "measurement" would not
     * describe anything, so re-anchor and take the counters as the new baseline
     * instead of publishing a fiction. */
    if (elapsed > QUOTA_STALL_MAX_INTERVAL_NS) {
        collect_percpu(now, &win);   /* re-baselines g_prev_* as a side effect */
        g_anchor_ns = now;
        spin_unlock_irqrestore(&g_agg_lock, flags);
        return 0;
    }

    collect_percpu(now, &win);

    /* Normalize each CPU's non-idle time against the busiest CPU, so the weights
     * are a bounded 0..QUOTA_STALL_WEIGHT_ONE regardless of interval length.
     * A CPU that was idle throughout weighs nothing: it neither dilutes another
     * core's stall nor contributes one. */
    for (uint32_t i = 0; i < (uint32_t)MAX_CPUS; i++) {
        if (win.nonidle[i] > max_nonidle)
            max_nonidle = win.nonidle[i];
    }
    for (uint32_t i = 0; i < (uint32_t)MAX_CPUS; i++) {
        weight[i] = (max_nonidle == 0)
                  ? 0u
                  : ((win.nonidle[i] << QUOTA_STALL_WEIGHT_SHIFT) / max_nonidle);
        sum_weight += weight[i];
    }

    periods = elapsed / QUOTA_STALL_UPDATE_NS;
    if (periods == 0)
        periods = 1;

    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
        stall_domain_t *sd = &g_domain[d];
        uint64_t some_fp;
        uint64_t full_fp;

        some_fp = weighted_ratio_q16(&win.some[0][d],
                                     QUOTA_STALL_DOMAIN_COUNT,
                                     weight, sum_weight, elapsed);
        full_fp = domain_full_defined((quota_stall_domain_t)d)
                ? weighted_ratio_q16(&win.full[0][d],
                                     QUOTA_STALL_DOMAIN_COUNT,
                                     weight, sum_weight, elapsed)
                : 0u;

        /* The cumulative total is the WEIGHTED time too, not the raw sum of
         * per-CPU stalls. Publishing the raw sum beside a weighted average would
         * mean the two numbers answered different questions, and a reader
         * dividing the total by uptime would get a figure the averages never
         * agree with. Ordered to avoid overflow: elapsed/1000 first. */
        sd->some_total_ns = sat_add_u64(sd->some_total_ns,
                                        ratio_q16_to_ns(some_fp, elapsed));
        sd->full_total_ns = sat_add_u64(sd->full_total_ns,
                                        ratio_q16_to_ns(full_fp, elapsed));

        /* An uninstrumented domain still accumulates (it accumulates zero), but
         * it never becomes VALID and therefore never steps a level: reporting
         * "calm" for a resource nobody measures is the specific dishonesty the
         * VALID flag exists to prevent. */
        apply_window(sd, some_fp, full_fp, periods, sd->instrumented);
        if (sd->instrumented)
            sd->valid = 1;
    }

    g_anchor_ns      = now;
    g_last_closed_ns = now;
    /* Every period the interval actually crossed, not one per call. A consumer
     * uses this to tell how much history the averages represent; reporting one
     * closed window for a forty-second gap would understate that by twenty. */
    g_windows_closed = sat_add_u64(g_windows_closed, periods);
    spin_unlock_irqrestore(&g_agg_lock, flags);
    return 1;
}

/* ==========================================================================
 * Queries
 * ========================================================================== */

/* Narrow one domain's carried state into the published snapshot shape. Caller
 * holds the aggregate lock. */
static void fill_snapshot_locked(quota_stall_domain_t domain,
                                 quota_stall_snapshot_t *out)
{
    stall_domain_t *sd = &g_domain[domain];
    int             full_ok = domain_full_defined(domain);

    out->some_total_ns = sd->some_total_ns;
    out->full_total_ns = full_ok ? sd->full_total_ns : 0ull;
    for (uint32_t w = 0; w < (uint32_t)QUOTA_STALL_WINDOW_COUNT; w++) {
        out->some_avg[w] = avg_to_permille(sd->some_avg[w]);
        out->full_avg[w] = full_ok ? avg_to_permille(sd->full_avg[w]) : 0u;
    }
    out->level          = sd->level;
    out->valid          = sd->valid;
    out->instrumented   = sd->instrumented;
    out->full_undefined = full_ok ? 0u : 1u;
}

/* Worst level across every VALID domain. Caller holds the aggregate lock. */
static quota_pressure_level_t system_level_locked(void)
{
    quota_pressure_level_t worst = QUOTA_PRESSURE_NORMAL;

    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
        /* An invalid domain is UNKNOWN, not calm: it can neither raise nor
         * lower the answer. Same rule as quota_pressure_system_level. */
        if (!g_domain[d].valid)
            continue;
        if ((quota_pressure_level_t)g_domain[d].level > worst)
            worst = (quota_pressure_level_t)g_domain[d].level;
    }
    return worst;
}

int quota_stall_get(quota_stall_domain_t domain, quota_stall_snapshot_t *out)
{
    uint64_t flags;

    if (!domain_valid(domain) || !out)
        return -1;

    memset(out, 0, sizeof(*out));
    spin_lock_irqsave(&g_agg_lock, &flags);
    fill_snapshot_locked(domain, out);
    spin_unlock_irqrestore(&g_agg_lock, flags);
    return 0;
}

int quota_stall_snapshot_all(quota_stall_summary_t *out)
{
    uint64_t flags;

    if (!out)
        return -1;

    memset(out, 0, sizeof(*out));

    /* ONE acquisition for the whole picture. Assembling this from the individual
     * queries lets the 2 s aggregation land between two of them, so a caller
     * could receive a window count from after a fold beside rows from before it,
     * or a system level matching none of the rows returned with it. */
    spin_lock_irqsave(&g_agg_lock, &flags);
    out->last_update_ns = g_last_closed_ns;
    out->windows_closed = g_windows_closed;
    out->system_level   = (uint8_t)system_level_locked();
    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++)
        fill_snapshot_locked((quota_stall_domain_t)d, &out->domain[d]);
    spin_unlock_irqrestore(&g_agg_lock, flags);
    return 0;
}

quota_pressure_level_t quota_stall_system_level(void)
{
    quota_pressure_level_t worst;
    uint64_t flags;

    spin_lock_irqsave(&g_agg_lock, &flags);
    worst = system_level_locked();
    spin_unlock_irqrestore(&g_agg_lock, flags);
    return worst;
}

uint64_t quota_stall_last_update_ns(void)
{
    uint64_t flags, v;

    spin_lock_irqsave(&g_agg_lock, &flags);
    v = g_last_closed_ns;
    spin_unlock_irqrestore(&g_agg_lock, flags);
    return v;
}

uint64_t quota_stall_windows_closed(void)
{
    uint64_t flags, v;

    spin_lock_irqsave(&g_agg_lock, &flags);
    v = g_windows_closed;
    spin_unlock_irqrestore(&g_agg_lock, flags);
    return v;
}

const char *quota_stall_domain_name(quota_stall_domain_t domain)
{
    return domain_valid(domain) ? g_domain_name[domain] : "?";
}

void quota_stall_dump(void)
{
    quota_stall_summary_t summary;

    /* Snapshot ONCE and OUTSIDE the lock, then log. Two reasons: klog reaches
     * the serial port and must never be called with a spinlock held, and a dump
     * assembled from separate reads could print a header and rows from different
     * generations. */
    if (quota_stall_snapshot_all(&summary) != 0)
        return;

    klog(LOG_INFO, "quota", "stall: system=%s windows=%llu\n",
         quota_pressure_level_name((quota_pressure_level_t)summary.system_level),
         (unsigned long long)summary.windows_closed);

    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
        const quota_stall_snapshot_t s = summary.domain[d];

        klog(LOG_INFO, "quota",
             "  %-3s %-8s some %u/%u/%u full %u/%u/%u total %llu/%llu ns%s\n",
             quota_stall_domain_name((quota_stall_domain_t)d),
             s.valid ? quota_pressure_level_name((quota_pressure_level_t)s.level)
                     : "unknown",
             (unsigned)s.some_avg[QUOTA_STALL_AVG10],
             (unsigned)s.some_avg[QUOTA_STALL_AVG60],
             (unsigned)s.some_avg[QUOTA_STALL_AVG300],
             (unsigned)s.full_avg[QUOTA_STALL_AVG10],
             (unsigned)s.full_avg[QUOTA_STALL_AVG60],
             (unsigned)s.full_avg[QUOTA_STALL_AVG300],
             (unsigned long long)s.some_total_ns,
             (unsigned long long)s.full_total_ns,
             s.full_undefined ? " (full undefined)" : "");
    }
}

#ifdef KERNEL_TESTS
void quota_stall_test_set_clock(uint64_t ns)
{
    __atomic_store_n(&g_test_clock_ns, ns, __ATOMIC_RELAXED);
}

void quota_stall_test_reset(void)
{
    uint64_t flags;

    for (uint32_t i = 0; i < (uint32_t)MAX_CPUS; i++) {
        stall_cpu_t *c = &g_stall_cpu[i];
        uint64_t     cflags;

        spin_lock_irqsave(&c->lock, &cflags);
        c->last_ns     = 0;
        c->nr_running  = 0;
        c->nonidle_ns  = 0;
        for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
            c->nr_stalled[d] = 0;
            c->some_ns[d]    = 0;
            c->full_ns[d]    = 0;
        }
        spin_unlock_irqrestore(&c->lock, cflags);
    }

    spin_lock_irqsave(&g_agg_lock, &flags);
    memset(g_domain, 0, sizeof(g_domain));
    memset(g_prev_some_ns, 0, sizeof(g_prev_some_ns));
    memset(g_prev_full_ns, 0, sizeof(g_prev_full_ns));
    memset(g_prev_nonidle_ns, 0, sizeof(g_prev_nonidle_ns));
    g_anchor_ns      = 0;
    g_last_closed_ns = 0;
    g_windows_closed = 0;
    spin_unlock_irqrestore(&g_agg_lock, flags);
}
#endif
