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
     * see weighted_ns for how it is used and why the naive denominator
     * is wrong. */
    uint64_t   nonidle_ns;
    /* The ELEMENT is cache-line aligned, which also rounds its SIZE up to a
     * multiple of the alignment -- so element N+1 starts on its own line without
     * a hand-written pad. Aligning only the ARRAY would align element 0 and
     * nothing else, and CPU 1's lock would share a line with CPU 0's counters.
     *
     * An earlier revision added an explicit pad sized by summing the field
     * widths, which silently missed the 4-byte hole between the 4-byte spinlock
     * and the 8-aligned last_ns: the payload is 88 bytes, not 84, so the pad
     * pushed sizeof to 192 (three lines, 3 KiB of BSS) instead of 128. The
     * asserts below could not catch it because 192 is also a clean multiple of
     * 64. Letting the compiler do the rounding removes the arithmetic that was
     * wrong in the first place; the size assert now pins the real answer. */
} __attribute__((aligned(QUOTA_STALL_CPU_ALIGN))) stall_cpu_t;

/* Named g_stall_cpu, not g_cpu: cpuid.h publishes a global `g_cpu` feature
 * block, and smp.h drags it in. */
static stall_cpu_t g_stall_cpu[MAX_CPUS];

/* Both halves of the isolation claim, asserted rather than asserted-in-prose:
 * the element must START on a cache line and must OCCUPY whole cache lines. */
_Static_assert(sizeof(stall_cpu_t) % QUOTA_STALL_CPU_ALIGN == 0,
    "per-CPU stall state must occupy whole cache lines (no false sharing)");
_Static_assert(_Alignof(stall_cpu_t) >= QUOTA_STALL_CPU_ALIGN,
    "per-CPU stall state must start on a cache line");
/* Pin the ACTUAL size, not just its divisibility: a future field that pushed the
 * element from two lines to three would otherwise pass every assert above while
 * quietly costing 50% more BSS and an extra line per CPU. */
_Static_assert(sizeof(stall_cpu_t) == 2u * QUOTA_STALL_CPU_ALIGN,
    "per-CPU stall state is expected to be exactly two cache lines");

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
/* Guarded by g_agg_lock. */
static stall_domain_t g_domain[QUOTA_STALL_DOMAIN_COUNT];
/* PRIVATE aggregation anchor: where the current window started. Set at init and
 * moved on every fold, including the ones that close no window. */
static uint64_t       g_anchor_ns;
/* PUBLISHED timestamp of the most recent CLOSED window. Kept separate from the
 * anchor because the public contract is "0 = no window has closed yet", and
 * publishing the anchor would report a non-zero time from init onward -- so a
 * consumer could not tell "not started" from "started and quiet". */
static uint64_t       g_last_closed_ns;
/* Guarded by g_agg_lock (g_anchor_ns is also read locklessly, atomically). */
static uint64_t       g_windows_closed;

/* Periods that elapsed without a fold of their own, measured AT THE FOLD from
 * the span it actually had to represent.
 *
 * This is the SERVICE-side measure, and it is the one that describes the
 * signal. The producer-side counter (quota_pressure_stall_folds_deferred)
 * records arms that could not be issued -- useful for diagnosing why, but it
 * over- and under-counts what was lost: an arm that succeeded still leaves a
 * gap if the worker was slow to run it, and a run of dropped arms inside one
 * period costs no periods at all. A period counted here is a period whose
 * distribution was reconstructed from a span average rather than observed, so
 * a nonzero delta over an interval means the averages and levels for that
 * interval are SMEARED -- suspect in shape, though still conserved in total.
 * Accumulated under g_agg_lock by the fold, read atomically. */
static uint64_t       g_fold_missed_periods;
/* Non-zero while a fold is in progress. The anchor alone cannot serialize a
 * fold: advancing it up front only keeps a second caller out until the NEXT
 * deadline, so a folder delayed past one window could resume and overwrite a
 * newer aggregate with stale data. This flag is held from claim to publish, and
 * the anchor moves only on a successful publish -- so a fold that never
 * completes leaves the window open for the next caller instead of losing it. */
static uint32_t       g_fold_busy;

/* Set once a producer declares it maintains the runnable count. Until then
 * `full` is NOT derivable: it means "stalled with nothing else to run", and with
 * nr_running pinned at zero every stalled interval would trivially satisfy that
 * and report full == some. Reporting a fabricated full is exactly the failure
 * the VALID flag exists to prevent, so full stays zero-and-undefined until the
 * scheduler seam says otherwise. Atomic: read by the aggregator, set by a
 * producer, never under a common lock. */
static uint32_t       g_runnable_tracked;

/* A producer asking for runnable tracking to be turned on. The producer CANNOT
 * perform the transition itself: enabling `full` requires re-anchoring every
 * per-CPU slot and re-baselining the aggregator's private g_prev_* counters, and
 * those belong to the fold. A producer that grabbed them would race a fold in
 * progress -- and it cannot WAIT for one either, because it may be running in
 * interrupt context on the very CPU the fold is on. So the producer only raises
 * this request; the next fold performs the transition inside its own
 * exclusivity, which is also the only honest place to draw the line: a window
 * boundary. */
static uint32_t       g_runnable_request;

/* Aggregator-private: the previous reading of each CPU's cumulative counters, so
 * a window measures GROWTH. Kept PER CPU rather than summed because the windows
 * are combined with a per-CPU weight, so a slot's stall has to stay associated
 * with that same slot's activity. Written only by the fold (serialized by
 * g_fold_busy) and by the re-baseline helper. */
static uint64_t g_prev_some_ns[MAX_CPUS][QUOTA_STALL_DOMAIN_COUNT];
static uint64_t g_prev_full_ns[MAX_CPUS][QUOTA_STALL_DOMAIN_COUNT];
static uint64_t g_prev_nonidle_ns[MAX_CPUS];
/* The timestamp each slot's previous snapshot corresponded to. */
static uint64_t g_prev_stamp_ns[MAX_CPUS];

/* Decay coefficient per averaging window, indexed by quota_stall_window_t. */
static const uint64_t g_decay[QUOTA_STALL_WINDOW_COUNT] = {
    QUOTA_STALL_EXP_10S,
    QUOTA_STALL_EXP_60S,
    QUOTA_STALL_EXP_300S
};

static const char *const g_domain_name[QUOTA_STALL_DOMAIN_COUNT] = {
    "cpu", "mem", "io"
};

/* An enum that grows must grow every array indexed by it. */
_Static_assert(sizeof(g_decay) / sizeof(g_decay[0]) == QUOTA_STALL_WINDOW_COUNT,
    "decay table must cover every averaging window");
_Static_assert(sizeof(g_domain_name) / sizeof(g_domain_name[0]) == QUOTA_STALL_DOMAIN_COUNT,
    "domain name table must cover every stall domain");


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
    if (domain == QUOTA_STALL_CPU)
        return 0;
    /* `full` also needs someone maintaining the runnable count. Without it
     * nr_running is permanently zero, so every stalled interval would qualify as
     * "nothing else to run" and full would simply mirror some -- a fabricated
     * number that looks like a measurement. */
    return __atomic_load_n(&g_runnable_tracked, __ATOMIC_ACQUIRE) != 0;
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

    /* Unseeded: anchor here and charge nothing -- the interval before the first
     * observation is not a measurement. */
    if (c->last_ns == 0) {
        c->last_ns = now;
        return;
    }
    /* NEVER move a slot's integration point backwards. The fold captures one
     * `now` before walking the CPUs, so a producer on another CPU can integrate
     * that slot at a LATER timestamp before the walk reaches it; assigning the
     * fold's older stamp here would rewind the slot and let the overlapping
     * interval be charged a second time by the next producer. Returning without
     * touching last_ns costs at most the sub-microsecond sliver between the two
     * timestamps, which the next integration picks up anyway. */
    if (now <= c->last_ns)
        return;
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
        permille >= quota_pressure_rise_threshold((uint32_t)level + 1u)) {
        sd->fall_count = 0;
        if (++sd->rise_count >= (uint16_t)QUOTA_PRESSURE_RISE_SAMPLES) {
            sd->rise_count = 0;
            sd->level = (uint8_t)(level + 1);
        }
    } else if (level > (uint8_t)QUOTA_PRESSURE_NORMAL &&
               permille < quota_pressure_fall_threshold((uint32_t)level)) {
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

/* Re-anchor every per-CPU integration point at `now` AND baseline the
 * aggregator's previous-counter snapshots to whatever those slots hold.
 *
 * CALLER MUST OWN THE FOLD (g_fold_busy), or run before any fold is possible:
 * it writes g_prev_*, which the fold reads and writes, and those are plain
 * fields with no lock of their own.
 *
 * Both halves are needed to draw a line under everything that came before.
 * Moving last_ns alone stops the elapsed TIME from being re-integrated, but the
 * cumulative counters still carry their old growth, and the next window would
 * difference them against zero -- publishing a full second of pre-line stall as
 * a 500-permille window that never happened. */
static void stall_rebaseline_all(uint64_t now)
{
    for (uint32_t i = 0; i < (uint32_t)MAX_CPUS; i++) {
        stall_cpu_t *c = &g_stall_cpu[i];
        uint64_t     cflags;

        spin_lock_irqsave(&c->lock, &cflags);
        /* Monotonic, for the same reason the integrator is: a producer may have
         * moved this slot past the caller's `now` already. */
        if (now > c->last_ns)
            c->last_ns = now;
        for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
            g_prev_some_ns[i][d] = c->some_ns[d];
            g_prev_full_ns[i][d] = c->full_ns[d];
        }
        g_prev_nonidle_ns[i] = c->nonidle_ns;
        g_prev_stamp_ns[i]   = c->last_ns;
        spin_unlock_irqrestore(&c->lock, cflags);
    }
}

void quota_stall_init(void)
{
    uint64_t flags;
    uint64_t now = stall_now_ns();

    /* The state machine is live from the first producer call; only publication
     * waits for init. Anything a producer recorded before this point belongs to
     * an interval nobody was measuring, so draw the line here.
     *
     * Safe without claiming the fold: this runs in Phase 3 before the periodic
     * sampler's timer is armed, so no fold can be in progress. The claim is
     * taken anyway rather than asserted, so a future caller that moves this
     * cannot silently start racing the baselines. */
    if (__atomic_exchange_n(&g_fold_busy, 1u, __ATOMIC_ACQ_REL) == 0u) {
        stall_rebaseline_all(now);
        __atomic_store_n(&g_fold_busy, 0u, __ATOMIC_RELEASE);
    }

    spin_lock_irqsave(&g_agg_lock, &flags);
    __atomic_store_n(&g_anchor_ns, now, __ATOMIC_RELEASE);
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

    /* Validate only the SLOT with an ordinary read: `cpu` is written once when
     * the token is issued and never mutated afterwards. The claim word is a
     * different matter -- every access to token->domain goes through an atomic
     * below, because mixing a plain read with another CPU's atomic write to the
     * same word is a data race the compiler is free to break even where the
     * hardware would not. */
    if (token->cpu >= (uint32_t)MAX_CPUS)
        return;

    cpu = token->cpu;

    /* CLAIM the token ATOMICALLY, before touching the counter.
     *
     * A plain check-then-store loses the race that matters: a timeout path and a
     * completion path can both cancel the same wait. Both read the token as
     * valid, both store the invalid marker, and both decrement -- driving the
     * count to zero while a SECOND waiter is still blocked, so the domain
     * reports quiet during a real stall. The zero-clamp cannot catch it, because
     * with two waiters the count never reaches zero. One exchange makes exactly
     * one caller the consumer. */
    domain = __atomic_load_n(&token->domain, __ATOMIC_ACQUIRE);
    for (;;) {
        /* Not a live STALL token: already consumed, never issued, or a runnable
         * token. Return WITHOUT writing -- a bare exchange would invalidate a
         * token this function has no claim on. */
        if (domain >= (uint32_t)QUOTA_STALL_DOMAIN_COUNT)
            return;
        if (__atomic_compare_exchange_n(&token->domain, &domain,
                                        (uint32_t)QUOTA_STALL_DOMAIN_COUNT, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;   /* claimed it; `domain` still holds the value we won */
        /* Lost the race; `domain` now holds what the winner left. */
    }

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

quota_stall_token_t quota_stall_runnable_begin(void)
{
    quota_stall_token_t token = quota_stall_token_none();
    stall_cpu_t        *c;
    uint64_t            irq_flags;
    uint64_t            flags;
    uint32_t            slot;

    c = stall_pin_this_cpu(&irq_flags, &slot);
    if (!c)
        return token;

    spin_lock_irqsave(&c->lock, &flags);
    stall_cpu_integrate_locked(c, stall_now_ns());
    if (c->nr_running < QUOTA_STALL_COUNT_MAX)
        c->nr_running++;
    spin_unlock_irqrestore(&c->lock, flags);
    local_irq_restore(irq_flags);

    /* REQUEST runnable tracking; the next fold enables it. A producer that
     * maintains the count has demonstrated it by calling this, but it must not
     * perform the transition here -- see g_runnable_request. */
    if (__atomic_load_n(&g_runnable_tracked, __ATOMIC_ACQUIRE) == 0u)
        __atomic_store_n(&g_runnable_request, 1u, __ATOMIC_RELEASE);

    token.cpu    = slot;
    token.domain = QUOTA_STALL_RUNNABLE_TOKEN;
    return token;
}

void quota_stall_runnable_end(quota_stall_token_t *token)
{
    stall_cpu_t *c;
    uint64_t     flags;
    uint32_t     claimed;
    uint32_t     cpu;

    if (!token || token->cpu >= (uint32_t)MAX_CPUS)
        return;

    /* Claim ONLY a runnable token, and only once. A bare exchange would
     * invalidate a stall token handed to the wrong consumer. */
    claimed = QUOTA_STALL_RUNNABLE_TOKEN;
    if (!__atomic_compare_exchange_n(&token->domain, &claimed,
                                     (uint32_t)QUOTA_STALL_DOMAIN_COUNT, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;

    cpu = token->cpu;
    c = &g_stall_cpu[cpu];
    spin_lock_irqsave(&c->lock, &flags);
    stall_cpu_integrate_locked(c, stall_now_ns());
    if (c->nr_running > 0)
        c->nr_running--;
    spin_unlock_irqrestore(&c->lock, flags);
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
    /* The interval each slot's deltas actually cover, which is NOT the fold's
     * elapsed. The fold captures one timestamp and then walks the CPUs; a
     * producer can advance a slot past that timestamp before the walk reaches
     * it, so the slot's counters include time the fold's interval does not. An
     * earlier revision divided every slot by the fold's elapsed and advanced the
     * baselines to the newer counters anyway -- permanently deleting the
     * difference. Dividing each slot by its OWN span conserves it exactly. */
    uint64_t span[MAX_CPUS];
} stall_window_t;


static uint64_t growth_of(uint64_t now_val, uint64_t *prev)
{
    /* A cumulative counter can only decrease if a test reset the per-CPU state
     * under a live aggregator. Re-anchor rather than underflowing to ~2^64. */
    uint64_t g = (now_val >= *prev) ? (now_val - *prev) : 0u;
    *prev = now_val;
    return g;
}

static void collect_percpu(uint64_t now, uint64_t anchor, stall_window_t *win)
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
        uint64_t     cur_stamp;

        spin_lock_irqsave(&c->lock, &flags);
        stall_cpu_integrate_locked(c, now);
        for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
            cur_some[d] = c->some_ns[d];
            cur_full[d] = c->full_ns[d];
        }
        cur_nonidle = c->nonidle_ns;
        /* Read the slot's OWN integration point under its lock: that, not the
         * fold's `now`, is the instant these counters describe. */
        cur_stamp = c->last_ns;
        spin_unlock_irqrestore(&c->lock, flags);

        /* Differencing happens OUTSIDE the per-CPU lock: it touches only
         * aggregator-private state, and holding a producer's lock across it
         * would put the scheduler and I/O paths behind bookkeeping. */
        for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
            win->some[i][d] = growth_of(cur_some[d], &g_prev_some_ns[i][d]);
            win->full[i][d] = growth_of(cur_full[d], &g_prev_full_ns[i][d]);
        }
        win->nonidle[i] = growth_of(cur_nonidle, &g_prev_nonidle_ns[i]);
        {
            /* An unseeded slot has no previous stamp; its span starts at the
             * window ANCHOR, not at zero. Measuring from zero would divide the
             * slot's stall by all of uptime and report a fraction of nothing. */
            uint64_t prev = g_prev_stamp_ns[i] ? g_prev_stamp_ns[i] : anchor;

            win->span[i] = (cur_stamp > prev) ? (cur_stamp - prev) : 0u;
            g_prev_stamp_ns[i] = cur_stamp;
        }
    }
}

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

/* Combine per-CPU values into one figure, weighting each CPU by how busy it was:
 *
 *     value = sum(value_i * nonidle_i) / sum(nonidle_i)
 *
 * Linux PSI's weighting, and the reason the collector keeps per-CPU deltas
 * instead of summing them. The naive alternative, sum(stall_i) / sum(nonidle_i),
 * silently answers a different question: two CPUs over a 2 s window, CPU 0
 * non-idle and stalled for 1 s, CPU 1 running the full 2 s, gives 333 permille
 * naively and 167 weighted. The naive number is not a fraction of anything a
 * reader can name -- it divides one CPU's stall by two CPUs' activity.
 *
 * Everything the fold publishes comes through here in NANOSECONDS: the stall
 * times AND the interval they are measured over. That is what keeps them
 * coherent. An earlier revision computed a per-slot RATIO here and rebuilt a
 * time from it using the fold-wide elapsed -- two different intervals -- so a
 * slot a producer advanced past the fold timestamp had its excess either deleted
 * or double-counted depending on which side of the boundary the stall fell.
 * Weighting the times and the spans identically removes the mismatch rather than
 * reconciling it afterwards.
 *
 * Overflow: each value is bounded by QUOTA_STALL_MAX_INTERVAL_NS (4.32e13) and
 * each weight by QUOTA_STALL_WEIGHT_ONE (4096), so the accumulator stays under
 * 2^62 across MAX_CPUS. */
static uint64_t weighted_ns(const uint64_t *per_cpu, uint32_t stride,
                            const uint64_t *weight, uint64_t sum_weight)
{
    uint64_t acc = 0;

    if (sum_weight == 0)
        return 0;

    for (uint32_t i = 0; i < (uint32_t)MAX_CPUS; i++) {
        uint64_t v = per_cpu[(uint64_t)i * stride];

        if (weight[i] == 0 || v == 0)
            continue;
        acc += v * weight[i];
    }
    return acc / sum_weight;
}

int quota_stall_fold_due(void)
{
    /* MUTATION-FREE service-needed predicate, for a DISPATCH_LEVEL caller that
     * must decide whether to hand the fold to a PASSIVE worker without doing
     * any of the fold's work itself.
     *
     * It deliberately mirrors quota_stall_aggregate's STAGE 1 conditions and
     * NOTHING else. Stage 1 is not purely a deadline test: on an unseeded
     * anchor or a backward clock it CLAIMS fold ownership and rebaselines every
     * CPU, which is exactly the MAX_CPUS lock walk that must not run in a DPC.
     * Reporting those states as "due" and leaving the seeding to the aggregate
     * on the passive side keeps the walk off DISPATCH_LEVEL while still letting
     * a system that never seeded reach its first fold -- returning false there
     * would strand self-seeding forever.
     *
     * A relaxed-ordering ACQUIRE load and two comparisons: no lock, no
     * interrupt masking, no stores. Racing with a concurrent fold is harmless;
     * the aggregate re-reads the anchor under its own claim and rechecks. */
    const uint64_t now    = stall_now_ns();
    const uint64_t anchor = __atomic_load_n(&g_anchor_ns, __ATOMIC_ACQUIRE);

    if (anchor == 0 || now < anchor)
        return 1;                        /* unseeded / backward clock: seed it */
    return (now - anchor >= QUOTA_STALL_UPDATE_NS) ? 1 : 0;
}

int quota_stall_aggregate(void)
{
    stall_window_t win;
    uint64_t weight[MAX_CPUS];
    uint64_t some_fp[QUOTA_STALL_DOMAIN_COUNT];
    uint64_t full_fp[QUOTA_STALL_DOMAIN_COUNT];
    uint64_t some_ns[QUOTA_STALL_DOMAIN_COUNT];
    uint64_t full_ns[QUOTA_STALL_DOMAIN_COUNT];
    uint64_t total_some[QUOTA_STALL_DOMAIN_COUNT];
    uint64_t total_full[QUOTA_STALL_DOMAIN_COUNT];
    uint64_t raw_some;
    uint64_t raw_full;
    uint64_t span_ns;
    uint64_t carry_some[QUOTA_STALL_DOMAIN_COUNT][QUOTA_STALL_WINDOW_COUNT];
    uint64_t carry_full[QUOTA_STALL_DOMAIN_COUNT][QUOTA_STALL_WINDOW_COUNT];
    uint64_t sum_weight = 0;
    uint64_t max_nonidle = 0;
    uint64_t flags;
    uint64_t now = stall_now_ns();
    uint64_t anchor;
    uint64_t elapsed;
    uint64_t periods;

    /* STAGE 1 -- lockless deadline check.
     *
     * The 50 ms pressure tick calls this forty times per window, so forty of
     * every forty-one calls do nothing. Taking an IRQ-disabling global spinlock
     * to discover that was pure cost on the timer service CPU; a relaxed load of
     * the anchor answers it with no lock and no interrupt masking at all. */
    anchor = __atomic_load_n(&g_anchor_ns, __ATOMIC_ACQUIRE);

    if (anchor == 0 || now < anchor) {
        /* Unseeded, or a clock that moved backwards (only a test clock can do
         * that). Baseline the SLOTS as well as the anchor: quota_stall_init is
         * documented as optional, so a producer may have accrued time before
         * anything anchored the window. Leaving g_prev_* at zero would let the
         * next fold difference those cumulative counters against zero and
         * publish pre-anchor history as if it had happened inside the first
         * measured window. Needs fold ownership because it writes the private
         * baselines; losing the claim means a fold is already running, which
         * means an anchor already exists and this path is moot. */
        if (__atomic_exchange_n(&g_fold_busy, 1u, __ATOMIC_ACQ_REL) == 0u) {
            stall_rebaseline_all(now);
            __atomic_store_n(&g_anchor_ns, now, __ATOMIC_RELEASE);
            __atomic_store_n(&g_fold_busy, 0u, __ATOMIC_RELEASE);
        }
        return 0;
    }
    if (now - anchor < QUOTA_STALL_UPDATE_NS)
        return 0;

    /* STAGE 2 -- CLAIM the fold, for its whole duration. Exactly one caller wins
     * the flag; it is released only after publication, so a delayed folder
     * cannot be lapped by a second one and later overwrite the newer result. The
     * anchor is deliberately NOT advanced here -- see the release path. */
    if (__atomic_exchange_n(&g_fold_busy, 1u, __ATOMIC_ACQ_REL) != 0u)
        return 0;

    /* Re-read under exclusivity: the deadline check above was lockless, so the
     * window may have been folded between that read and this claim. */
    anchor = __atomic_load_n(&g_anchor_ns, __ATOMIC_ACQUIRE);
    if (anchor == 0 || now < anchor ||
        now - anchor < QUOTA_STALL_UPDATE_NS) {
        __atomic_store_n(&g_fold_busy, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    elapsed = now - anchor;

    /* STAGE 3 -- collect. Each per-CPU lock is taken and released on its own,
     * for just long enough to integrate and copy that slot's counters. No
     * global lock is held across the walk, so a producer on another CPU never
     * waits on the aggregator's arithmetic. */
    collect_percpu(now, anchor, &win);

    /* Pending runnable-tracking activation, published AFTER this window was
     * collected and BEFORE the next one opens.
     *
     * The ordering is the whole fix. `full` cannot have accrued before tracking
     * -- the integrator gates full accrual on domain_full_defined(), which is
     * false until this flag is set -- so there is nothing to discard and no
     * baseline to reset. An earlier revision re-baselined everything here, which
     * did suppress pre-tracking full but ALSO advanced the `some` and non-idle
     * baselines, silently deleting a whole window of perfectly valid stall time.
     * Setting the flag after collection suppresses the untracked full without
     * touching anything else: this window publishes its real `some`, and the
     * next window is the first that describes `full`. */
    if (__atomic_load_n(&g_runnable_request, __ATOMIC_ACQUIRE) != 0u)
        __atomic_store_n(&g_runnable_tracked, 1u, __ATOMIC_RELEASE);

    /* An interval this long is not a delayed sample, it is a broken clock. The
     * counters have already been re-baselined by collect_percpu, so discarding
     * here leaves consistent state for the next window. */
    if (elapsed > QUOTA_STALL_MAX_INTERVAL_NS) {
        __atomic_store_n(&g_anchor_ns, now, __ATOMIC_RELEASE);
        __atomic_store_n(&g_fold_busy, 0u, __ATOMIC_RELEASE);
        return 0;
    }

    /* STAGE 4 -- compute, still with interrupts ENABLED and no lock held. This
     * is the expensive part: 16 weight normalizations, up to 6 weighted-ratio
     * passes over 16 slots, and 18 closed-form decays each costing a bounded
     * binary exponentiation. Doing it under an IRQ-disabling global spinlock (as
     * the first revision did) put roughly 270 exponentiation iterations plus a
     * 16-slot walk inside one interrupt-masked region on the timer service CPU,
     * against a repo contract that spinlock sections stay far below that. */
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

    /* The interval the published numbers actually represent, weighted exactly
     * like the stall times themselves. This -- not the fold's wall-clock elapsed
     * -- is the ratio denominator and the period divisor, so every published
     * quantity refers to one and the same interval. */
    span_ns = weighted_ns(win.span, 1u, weight, sum_weight);
    if (span_ns == 0) {
        /* Nothing was non-idle anywhere, so there is no weighted interval to
         * speak of -- but wall time still passed, and the averages must still
         * decay across it. Fall back to the fold's elapsed: every stall time is
         * zero in this case, so the fold contributes pure decay, which is
         * exactly right for an idle system. Without this a long quiet gap
         * advanced the EWMA by a single period and the averages never bled off.
         */
        span_ns = elapsed;
    }

    /* Periods over the REPRESENTED interval, so the EWMA advances by exactly the
     * history the sample describes. */
    periods = span_ns / QUOTA_STALL_UPDATE_NS;
    if (periods == 0)
        periods = 1;

    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
        some_ns[d] = weighted_ns(&win.some[0][d], QUOTA_STALL_DOMAIN_COUNT,
                                 weight, sum_weight);
        full_ns[d] = domain_full_defined((quota_stall_domain_t)d)
                   ? weighted_ns(&win.full[0][d], QUOTA_STALL_DOMAIN_COUNT,
                                 weight, sum_weight)
                   : 0u;
        if (some_ns[d] > span_ns)
            some_ns[d] = span_ns;      /* cannot stall longer than the interval */
        if (full_ns[d] > some_ns[d])
            full_ns[d] = some_ns[d];   /* full implies some */
        some_fp[d] = stall_ratio_q16(some_ns[d], span_ns);
        full_fp[d] = stall_ratio_q16(full_ns[d], span_ns);

        /* The CUMULATIVE totals come from the RAW per-CPU deltas, not from the
         * weighted mean above.
         *
         * A weighted mean is the right answer for a WINDOW -- it is a fraction
         * of real time, comparable across machines with different core counts.
         * It is the wrong thing to accumulate: the weights are recomputed per
         * fold, so the same physical stall split across two folds at a different
         * boundary sums to a different total. (Two non-idle CPUs, one stalled:
         * folding at 2 s and 4 s adds 2.0 s, but folding at 3 s and 4 s adds
         * about 2.13 s -- no CPU time was created, the nonlinear weights simply
         * do not distribute over a partition.) A running total that depends on
         * where the sampler happened to tick is not a counter.
         *
         * The raw sum IS additive and exactly conserved, at the cost of being
         * CPU-nanoseconds rather than wall-nanoseconds -- which is what the ABI
         * documents it as. Ratios stay weighted; totals stay conserved. */
        raw_some = 0;
        raw_full = 0;
        for (uint32_t i = 0; i < (uint32_t)MAX_CPUS; i++) {
            raw_some = sat_add_u64(raw_some, win.some[i][d]);
            if (domain_full_defined((quota_stall_domain_t)d))
                raw_full = sat_add_u64(raw_full, win.full[i][d]);
        }
        total_some[d] = raw_some;
        total_full[d] = raw_full;
    }

    /* Snapshot the carried averages, advance them outside the lock, publish the
     * results. Safe because stage 2 made this fold the only writer. */
    spin_lock_irqsave(&g_agg_lock, &flags);
    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
        for (uint32_t w = 0; w < (uint32_t)QUOTA_STALL_WINDOW_COUNT; w++) {
            carry_some[d][w] = g_domain[d].some_avg[w];
            carry_full[d][w] = g_domain[d].full_avg[w];
        }
    }
    spin_unlock_irqrestore(&g_agg_lock, flags);

    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
        for (uint32_t w = 0; w < (uint32_t)QUOTA_STALL_WINDOW_COUNT; w++) {
            carry_some[d][w] = ewma_apply(carry_some[d][w],
                                          some_fp[d], g_decay[w], periods);
            carry_full[d][w] = ewma_apply(carry_full[d][w],
                                          full_fp[d], g_decay[w], periods);
        }
    }

    /* STAGE 5 -- publish. Short, bounded, no division and no exponentiation. */
    spin_lock_irqsave(&g_agg_lock, &flags);
    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
        stall_domain_t *sd = &g_domain[d];

        for (uint32_t w = 0; w < (uint32_t)QUOTA_STALL_WINDOW_COUNT; w++) {
            sd->some_avg[w] = carry_some[d][w];
            sd->full_avg[w] = carry_full[d][w];
        }
        sd->some_total_ns = sat_add_u64(sd->some_total_ns, total_some[d]);
        sd->full_total_ns = sat_add_u64(sd->full_total_ns, total_full[d]);

        /* An uninstrumented domain still folds (it folds zero), but it never
         * becomes VALID and therefore never steps a level: reporting "calm" for
         * a resource nobody measures is the specific dishonesty the VALID flag
         * exists to prevent. */
        if (!sd->instrumented)
            continue;
        sd->valid = 1;
        /* ONE fold is ONE debounce observation, however many periods it spanned.
         *
         * Deliberately NOT scaled by `periods`, even though ewma_apply above is.
         * The two advance differently because they measure different things: an
         * EWMA is a time-decaying quantity, so advancing it by elapsed time is
         * what keeps it correct, whereas the rise/fall debounce is a count of
         * INDEPENDENT CONSECUTIVE observations whose whole purpose is to refuse
         * to act on a single reading. Replaying one span average N times would
         * manufacture the very corroboration the debounce exists to demand, and
         * would let a reconstructed span drive a domain to CRITICAL on evidence
         * the counters never held.
         *
         * The level is not stranded by this: the averages catch up in full here,
         * and the next RISE_SAMPLES real folds move the level at its normal
         * rate. Pinned by test_stall_over_long_stalled_gap_keeps_its_measurement
         * (test_quota_stall.c), which asserts both halves -- no jump on the
         * delayed fold, and a rise immediately afterward. */
        stall_level_step(sd, avg_to_permille(sd->some_avg[QUOTA_STALL_AVG10]));
    }
    /* Charge every SERVICE period this fold had to cover beyond its own.
     *
     * Computed from `elapsed` (the fold's own wall interval, now - anchor) and
     * deliberately NOT from `periods`. Only one of them answers "how late was
     * the service": `periods` derives from the WEIGHTED represented span, and
     * collect_percpu lets a producer advance a slot past the fold timestamp, so
     * it drifts by one at the boundary -- a fold exactly ten wall periods late
     * can compute nine from a slot stamp a nanosecond short. That drift is
     * correct for the EWMA (it is the interval the sample describes) and wrong
     * for a health counter a reader uses to decide whether to trust the numbers
     * above it.
     *
     * Written under g_agg_lock with the publish, so the aggregate snapshot
     * carries the averages and the count that qualifies them together. */
    {
        uint64_t service_periods = elapsed / QUOTA_STALL_UPDATE_NS;
        if (service_periods > 1ull)
            g_fold_missed_periods = sat_add_u64(g_fold_missed_periods,
                                                service_periods - 1ull);
    }

    g_last_closed_ns = now;
    g_windows_closed = sat_add_u64(g_windows_closed, periods);
    spin_unlock_irqrestore(&g_agg_lock, flags);

    /* Anchor advances ONLY here, after the result is published, and the fold is
     * released after it. Ordering matters: a caller that observes the new anchor
     * must also be able to observe everything this fold published. */
    __atomic_store_n(&g_anchor_ns, now, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fold_busy, 0u, __ATOMIC_RELEASE);
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

/* Worst level across every VALID domain, or QUOTA_PRESSURE_UNKNOWN when no
 * domain has been measured. Caller holds the aggregate lock.
 *
 * Seeding at UNKNOWN rather than NORMAL is what makes the composite honest while
 * every stall seam is still unwired (sections 12 and 17 own the producers): with
 * no valid domain there is no evidence, and NORMAL would assert calm on the
 * strength of an instrument that has never run. */
static quota_pressure_level_t system_level_locked(void)
{
    quota_pressure_level_t worst = QUOTA_PRESSURE_UNKNOWN;

    for (uint32_t d = 0; d < (uint32_t)QUOTA_STALL_DOMAIN_COUNT; d++) {
        /* An invalid domain is UNKNOWN, not calm: it can neither raise nor
         * lower the answer. Same rule as quota_pressure_system_level. */
        if (!g_domain[d].valid)
            continue;
        /* First measurement replaces UNKNOWN outright; later ones only if worse.
         * UNKNOWN sorts above every real level, so a bare `>` would throw away
         * the first real reading. */
        if (!quota_pressure_level_measured(worst) ||
            (quota_pressure_level_t)g_domain[d].level > worst)
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
    out->missed_periods = g_fold_missed_periods;
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

uint64_t quota_stall_missed_periods(void)
{
    uint64_t flags, v;

    spin_lock_irqsave(&g_agg_lock, &flags);
    v = g_fold_missed_periods;
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

    /* Qualify the rows BEFORE printing them, from the SAME snapshot. A separate
     * quota_stall_missed_periods() call here would be a second generation, and a
     * fold landing between the two could certify rows it had just smeared. */
    if (summary.missed_periods)
        klog(LOG_WARN, "quota",
             "  %llu period(s) below are RECONSTRUCTED from a span average "
             "(service fell behind; totals conserved, distribution smeared)\n",
             (unsigned long long)summary.missed_periods);

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

    /* ACQUIRE the fold rather than clearing its flag. g_agg_lock does not
     * exclude a fold in its lock-free compute stage, and holding the sampler
     * only stops FUTURE timer passes -- it does not join a DPC already running.
     * Clearing the flag instead of taking it would let an in-flight fold keep
     * writing the baselines this function is resetting. Bounded spin: this is a
     * test-only path at PASSIVE_LEVEL and a fold is microseconds long. */
    int owned = 0;

    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        if (__atomic_exchange_n(&g_fold_busy, 1u, __ATOMIC_ACQ_REL) == 0u) {
            owned = 1;
            break;
        }
    }
    /* If the claim never succeeded, do NOTHING. Resetting anyway would race the
     * fold that still owns the baselines, and clearing the flag on the way out
     * would hand a second fold a half-reset structure -- strictly worse than a
     * test that observes stale state and fails loudly. */
    if (!owned)
        return;

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
    __atomic_store_n(&g_runnable_tracked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_runnable_request, 0u, __ATOMIC_RELEASE);
    memset(g_prev_some_ns, 0, sizeof(g_prev_some_ns));
    memset(g_prev_full_ns, 0, sizeof(g_prev_full_ns));
    memset(g_prev_nonidle_ns, 0, sizeof(g_prev_nonidle_ns));
    memset(g_prev_stamp_ns, 0, sizeof(g_prev_stamp_ns));
    __atomic_store_n(&g_anchor_ns, 0ull, __ATOMIC_RELEASE);
    g_last_closed_ns = 0;
    g_windows_closed = 0;
    g_fold_missed_periods = 0;
    spin_unlock_irqrestore(&g_agg_lock, flags);

    /* Release the fold LAST, so nothing can start one against half-reset state. */
    __atomic_store_n(&g_fold_busy, 0u, __ATOMIC_RELEASE);
}
#endif
