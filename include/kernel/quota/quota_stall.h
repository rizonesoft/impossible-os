/* ============================================================================
 * quota_stall.h -- PSI-shaped resource-pressure stall telemetry.
 *
 * Linux's Pressure Stall Information gives, per resource, the fraction of wall
 * time in which work was LOST to contention: `some` (at least one task stalled)
 * and `full` (every task that could have run was stalled), each smoothed over
 * 10 / 60 / 300 second windows. That shape is what this header reproduces --
 * not a single opaque ratio, and not the budget-saturation ratio the pressure
 * machine in quota_pressure.h already derives.
 *
 * WHY A SEPARATE DOMAIN SPACE. quota_resource_type_t is frozen at 16 values
 * (quota.h; the job-quota wire class pins the count), and none of the 16 is
 * "cpu" or "io". Coercing a system-wide memory stall into QUOTA_RES_PAGED_POOL
 * would relabel the signal and leave CPU and I/O with no legal identity at all,
 * so stall telemetry gets its own three-entry domain space. The two planes stay
 * independent all the way through their hysteresis -- a budget sample must not
 * be able to reset or complete a stall debounce, and vice versa -- and meet only
 * at the system-wide answer, which is the worse of the two.
 *
 * WHY THE COUNTERS ARE STATE TRANSITIONS, NOT DURATIONS. A seam that reported
 * "this wait took N ns" would sum overlapping waits: ten threads each blocked
 * for one second inside the same second would report ten seconds of pressure in
 * one second of wall time. Producers therefore report ENTER / LEAVE transitions
 * and the owning CPU integrates elapsed wall time while its stalled count is
 * non-zero, which is a fraction of real time by construction.
 *
 * WHAT IS NOT WIRED YET. All three seams live outside the quota module and are
 * blocked on work owned elsewhere, so nothing calls the producer API today and
 * every domain reports VALID clear -- "unknown", never a zero that would read as
 * "no pressure". The resource-pressure stall-telemetry roadmap names the three.
 * ========================================================================== */
#ifndef KERNEL_QUOTA_QUOTA_STALL_H
#define KERNEL_QUOTA_QUOTA_STALL_H

#include "kernel/types.h"
#include "kernel/quota/quota_pressure.h"   /* quota_pressure_level_t + thresholds */

/* --- Domains -------------------------------------------------------------- *
 * Deliberately NOT quota_resource_type_t. Append only; the enum value is the
 * identity (per-CPU array index, info-class row order, dump rows). */
typedef enum quota_stall_domain {
    QUOTA_STALL_CPU = 0,   /* runnable but not running (scheduler contention) */
    QUOTA_STALL_MEM = 1,   /* waiting on reclaim / allocation                 */
    QUOTA_STALL_IO  = 2,   /* waiting on block-I/O completion                 */
    QUOTA_STALL_DOMAIN_COUNT = 3
} quota_stall_domain_t;

/* --- What `some` and `full` actually mean --------------------------------- *
 * Both are integrated PER CPU and combined as an average weighted by non-idle
 * time, which is Linux PSI's definition and worth stating precisely because the
 * shorter phrasing is misleading:
 *
 *   some -- time during which at least one task on a CPU was stalled on the
 *           domain, averaged across the CPUs that had anything to do.
 *   full -- time during which a CPU had stalled work and nothing else it could
 *           run, averaged the same way. It is NOT "every task in the system was
 *           stalled": on a two-CPU box with one core blocked on I/O and the
 *           other running, full reads about 500 permille, not zero, and that is
 *           the intended answer -- half the machine's usable capacity was lost.
 *
 * A genuinely system-wide `full` would need every scheduler transition on every
 * CPU to update one shared structure, which is the cross-CPU bouncing this
 * design exists to avoid. Linux made the same trade.
 *
 * FULL implies SOME: a window in which everything was stalled is also a window
 * in which something was stalled. The integrator enforces that rather than
 * trusting producers to keep the two consistent. */
typedef enum quota_stall_kind {
    QUOTA_STALL_SOME = 0,
    QUOTA_STALL_FULL = 1,
    QUOTA_STALL_KIND_COUNT = 2
} quota_stall_kind_t;

/* --- Averaging windows ---------------------------------------------------- */
typedef enum quota_stall_window {
    QUOTA_STALL_AVG10  = 0,
    QUOTA_STALL_AVG60  = 1,
    QUOTA_STALL_AVG300 = 2,
    QUOTA_STALL_WINDOW_COUNT = 3
} quota_stall_window_t;

/* --- Aggregation cadence -------------------------------------------------- *
 * Two seconds, the same period Linux PSI aggregates on -- which is what makes
 * the decay coefficients below reusable verbatim. The 50 ms pressure sampler
 * calls the aggregator on every tick; the aggregator itself is what enforces
 * this period, so the caller needs no counter of its own. */
#define QUOTA_STALL_UPDATE_MS      2000u
#define QUOTA_STALL_NSEC_PER_MSEC  1000000ull
#define QUOTA_STALL_UPDATE_NS \
    ((uint64_t)QUOTA_STALL_UPDATE_MS * QUOTA_STALL_NSEC_PER_MSEC)

/* --- Decay coefficients --------------------------------------------------- *
 * exp(-P/W) for the aggregation period P = 2 s and windows W = 10 / 60 / 300 s,
 * in 1/2^32 fixed point:
 *
 *     exp(-2/10)  = 0.8187307531 -> 3516421809
 *     exp(-2/60)  = 0.9672161005 -> 4154161520
 *     exp(-2/300) = 0.9933554618 -> 4266429413
 *
 * Carried at 2^-32 rather than Linux's 2^-11 because the catch-up path raises
 * these to a power (see quota_stall.c): an 11-bit base compounds its own
 * rounding error on every squaring, and after a few dozen periods the result
 * would no longer describe the window it is named after.
 *
 * A NOTE ON GETTING THESE WRONG. The first revision used 1877 / 2014 / 2037 at
 * 2^-11 -- close to Linux's LOADAVG constants, which are for a FIVE-second
 * period and 1 / 5 / 15 MINUTE windows. Solving exp(-2/W) = c/2048 for those
 * values gives W = 22.9 s, 119.5 s and 371.4 s: every window reacted far more
 * slowly than its name promised, and no test could have caught it, because the
 * constants are only meaningful against the period they were derived for. If
 * QUOTA_STALL_UPDATE_MS ever changes, these MUST be recomputed. */
#define QUOTA_STALL_FP_SHIFT    32u
#define QUOTA_STALL_FP_ONE      (1ull << QUOTA_STALL_FP_SHIFT)
#define QUOTA_STALL_EXP_10S     3516421809ull
#define QUOTA_STALL_EXP_60S     4154161520ull
#define QUOTA_STALL_EXP_300S    4266429413ull

_Static_assert(QUOTA_STALL_EXP_10S < QUOTA_STALL_EXP_60S &&
               QUOTA_STALL_EXP_60S < QUOTA_STALL_EXP_300S &&
               QUOTA_STALL_EXP_300S < QUOTA_STALL_FP_ONE,
    "stall decay coefficients must be ordered and strictly below unity");
_Static_assert(QUOTA_STALL_UPDATE_MS == 2000u,
    "decay coefficients are derived for a 2 s period; recompute them if it moves");
/* Pin the VALUES too. The period assert alone catches a changed cadence but not
 * an edited coefficient, and a wrong coefficient is invisible at runtime -- it
 * just makes a window mean a different number of seconds than its name. These
 * three literals are exp(-2/W) * 2^32 for W = 10, 60, 300; changing one has to
 * be a deliberate edit here, not a drive-by. */
_Static_assert(QUOTA_STALL_EXP_10S  == 3516421809ull &&
               QUOTA_STALL_EXP_60S  == 4154161520ull &&
               QUOTA_STALL_EXP_300S == 4266429413ull,
    "decay coefficients are exp(-2/W)*2^32 for W = 10/60/300 s -- recompute, do not tweak");

/* --- Per-CPU weighting ---------------------------------------------------- *
 * A CPU contributes to a window in proportion to how much of it that CPU was
 * NON-IDLE, which is what makes the answer a fraction of real time rather than
 * a fraction of installed cores. Weights are normalized against the busiest CPU
 * and carried at 1/4096, which keeps every product in the weighted sum far
 * inside 64 bits however long the interval was. */
#define QUOTA_STALL_WEIGHT_SHIFT  12u
#define QUOTA_STALL_WEIGHT_ONE    (1ull << QUOTA_STALL_WEIGHT_SHIFT)

/* --- Average precision ---------------------------------------------------- *
 * The averages are CARRIED in permille scaled by 2^16 and only narrowed to a
 * uint16_t permille when a caller snapshots them. Carrying them as permille
 * directly is what makes an EWMA latch: with a 300 s window the decay factor is
 * 2037/2048, so a stored value of 1 permille decays to 0.9995 and rounds back to
 * 1 forever -- a resource that stopped stalling would report pressure for the
 * rest of the boot. Sixteen extra bits put the fixed point far enough below the
 * reporting unit that the floor-rounded decay always reaches exactly zero. */
#define QUOTA_STALL_AVG_SHIFT   16u
#define QUOTA_STALL_AVG_ONE     (1ull << QUOTA_STALL_AVG_SHIFT)

/* Permille domain, shared with the pressure machine so the two agree on scale. */
#define QUOTA_STALL_PERMILLE_MAX  QUOTA_PRESSURE_PERMILLE_MAX

/* The widest value an average can carry, and the proof that the EWMA step
 * cannot leave the 64-bit domain: avg <= 1000 << 16 (about 2^26) and the
 * multiplier is bounded by 2^32, so the product stays under 2^58. */
#define QUOTA_STALL_AVG_MAX \
    ((uint64_t)QUOTA_STALL_PERMILLE_MAX << QUOTA_STALL_AVG_SHIFT)

_Static_assert(QUOTA_STALL_AVG_MAX <= (0xFFFFFFFFFFFFFFFFull / QUOTA_STALL_FP_ONE),
    "EWMA step must not overflow: avg * 2^32 has to stay inside 64 bits");

/* Sanity ceiling on a single aggregation interval. Not a catch-up bound -- the
 * decay is applied in CLOSED FORM, so any period count costs the same handful
 * of multiplies -- but a guard against a clock that jumped by an absurd amount,
 * where the arithmetic would still be defined yet the "measurement" would be
 * meaningless. Past this the interval is discarded and the aggregator
 * re-anchors. Twelve hours is far beyond any real sampler outage and still
 * leaves every intermediate product decades from overflow. */
#define QUOTA_STALL_NSEC_PER_SEC     1000000000ull
#define QUOTA_STALL_MAX_INTERVAL_SEC 43200ull      /* twelve hours */
#define QUOTA_STALL_MAX_INTERVAL_NS \
    (QUOTA_STALL_MAX_INTERVAL_SEC * QUOTA_STALL_NSEC_PER_SEC)

_Static_assert(QUOTA_STALL_MAX_INTERVAL_NS > 300ull * QUOTA_STALL_NSEC_PER_SEC,
    "the interval ceiling must comfortably exceed the widest averaging window");

/* --- Snapshot (the query shape) ------------------------------------------- */
typedef struct quota_stall_snapshot {
    /* Cumulative CPU-nanoseconds (sum over CPUs), saturating -- an ADDITIVE
     * conserved counter. Deliberately not the weighted mean the averages use:
     * that is a per-window fraction of real time and does not distribute over a
     * partition, so accumulating it would make the running total depend on the
     * sampler's tick alignment. */
    uint64_t some_total_ns;
    uint64_t full_total_ns;   /* always 0 for CPU, and until runnable tracking */
    uint16_t some_avg[QUOTA_STALL_WINDOW_COUNT];  /* permille, 0..1000        */
    uint16_t full_avg[QUOTA_STALL_WINDOW_COUNT];  /* permille, 0..1000        */
    uint8_t  level;           /* quota_pressure_level_t of this domain's lane */
    uint8_t  valid;           /* 1 = the numbers mean something               */
    uint8_t  instrumented;    /* 1 = a seam declared itself                   */
    uint8_t  full_undefined;  /* 1 = full is not derivable (CPU domain)       */
} quota_stall_snapshot_t;

/* --- Lifecycle ------------------------------------------------------------ */

/* Seed the aggregation clock. Called once from quota_pressure_init(). Safe to
 * skip entirely: an unseeded aggregator seeds itself on its first call and
 * discards that first (unbounded) interval rather than dividing by it. */
void quota_stall_init(void);

/* --- Producer seam (nothing calls these yet) ------------------------------ *
 * All are callable at any IRQL, including interrupt context. The hot paths --
 * the ENTER side and the runnable count -- take only the CALLING CPU's own lock,
 * pinned across the update, so the scheduler / allocator / I/O paths that will
 * own them never bounce a line between cores. The LEAVE side takes the lock of
 * the slot its token names, which is a different CPU exactly when the task
 * migrated mid-wait; that is the case the token exists to get right, and it
 * costs one uncontended remote lock per migrated wait. quota_stall_declare_
 * instrumented takes the global aggregate lock, but it runs once per seam at
 * init, not on any hot path.
 *
 * A domain reports VALID only once a seam has declared itself AND at least one
 * aggregation window has closed, so an uninstrumented domain reads "unknown"
 * rather than "calm". */
void quota_stall_declare_instrumented(quota_stall_domain_t domain);

/* Ownership token returned by an ENTER and consumed by the matching LEAVE.
 *
 * It exists because a stalling task does not reliably resume on the CPU it
 * blocked on. If LEAVE simply decremented "this CPU", a task that entered an I/O
 * wait on CPU 1 and woke on CPU 2 would increment CPU 1 and then clamp CPU 2's
 * already-zero counter -- leaving CPU 1 permanently stalled and driving the
 * domain to critical over a workload that had finished. The token carries the
 * originating slot so a LEAVE always decrements the counter its ENTER
 * incremented, whatever CPU it runs on. */
/* `domain` doubles as the claim word: it is exchanged to
 * QUOTA_STALL_DOMAIN_COUNT when the token is consumed, so exactly one caller can
 * ever consume it even if a timeout path and a completion path race. */
#define QUOTA_STALL_RUNNABLE_TOKEN  0xFFFFFFFFu   /* a runnable, not a stall */

typedef struct quota_stall_token {
    uint32_t cpu;      /* originating per-CPU slot          */
    uint32_t domain;   /* QUOTA_STALL_DOMAIN_COUNT = invalid */
} quota_stall_token_t;

_Static_assert(QUOTA_STALL_RUNNABLE_TOKEN > (uint32_t)QUOTA_STALL_DOMAIN_COUNT,
    "the runnable-token marker must not collide with a real stall domain");

/* One more task stalled on `domain`, charged to the calling CPU. The returned
 * token MUST be passed to quota_stall_task_unstalled; a token whose `domain` is
 * QUOTA_STALL_DOMAIN_COUNT means the enter was not recorded (bad domain, or no
 * per-CPU area yet) and is safe to pass back. */
quota_stall_token_t quota_stall_task_stalled(quota_stall_domain_t domain);

/* One fewer task stalled, against the slot the token names. CONSUMES the token:
 * `*token` is invalidated in place, so calling this twice on the same token is a
 * no-op on the second call.
 *
 * Taking it by pointer is what makes that true. A by-value token could be
 * consumed twice, and because the counter is a plain count with no per-token
 * identity, the second consume would decrement a DIFFERENT task's still-active
 * stall -- two waiters, one leaves twice, and the domain reports quiet while one
 * of them is still blocked. Clamping at zero does not help: the count never
 * reached zero. Invalidating the caller's token closes that hole for the cost of
 * one store.
 *
 * Residual, stated plainly: a caller that COPIES a token and consumes both
 * copies still miscounts. That is the same class of bug as freeing a copied
 * pointer twice, and detecting it would need per-token identity on a path meant
 * for the scheduler and the I/O completion handler. Producers must treat the
 * token as owned by the waiting task, not as a value to pass around. */
void quota_stall_task_unstalled(quota_stall_token_t *token);

/* A token that records nothing, for a producer that needs an initialized value
 * before it knows whether it will stall. */
static inline quota_stall_token_t quota_stall_token_none(void)
{
    quota_stall_token_t t;
    t.cpu    = 0;
    t.domain = (uint32_t)QUOTA_STALL_DOMAIN_COUNT;
    return t;
}

/* One task became runnable on this CPU / stopped being runnable.
 *
 * Token-based for the SAME reason the stall pair is, and the reason is not
 * hypothetical here: this kernel has a single global run queue (`current_task`
 * in src/kernel/sched/task.c), so a task made runnable while one CPU runs the
 * scheduler and dequeued while another does would otherwise leave the first
 * slot's count permanently high and clamp the second at zero. That is worse than
 * a mis-labelled interval: a slot with nr_running stuck non-zero can never
 * report `full` and always carries maximum weight, while a slot stuck at zero
 * reads idle and drops out of the denominator entirely.
 *
 * Two things depend on this count: `full` accrues only while a domain has
 * stalled tasks and this count is zero, and a CPU counts as NON-IDLE while it is
 * non-zero. Non-idle time is the denominator every ratio is taken against, so a
 * core with nothing to run neither dilutes another core's stall nor contributes
 * one of its own.
 *
 * Until something calls these, `full` is reported as zero-and-undefined for
 * EVERY domain, not just cpu -- see quota_stall_snapshot_t.full_undefined. */
quota_stall_token_t quota_stall_runnable_begin(void);
void quota_stall_runnable_end(quota_stall_token_t *token);

/* --- Aggregation ---------------------------------------------------------- */

/* Fold the per-CPU integrals into totals, averages, and levels, if at least
 * QUOTA_STALL_UPDATE_NS has elapsed since the last fold. Driven from the
 * pressure sampler's 50 ms tick; exposed so a test can step it deterministically.
 * Returns 1 if a window closed, 0 if the call was too early. */
/* Mutation-free "does the window need servicing" predicate. Safe to call at
 * DISPATCH_LEVEL: one ACQUIRE load and two comparisons, no lock, no stores.
 * Returns non-zero when quota_stall_aggregate has work -- an elapsed deadline,
 * an unseeded anchor, or a backward clock. The seeding/rebaselining those last
 * two need is deliberately NOT done here; it belongs with the fold, at
 * PASSIVE_LEVEL, because it walks every CPU under a lock. */
int quota_stall_fold_due(void);

int quota_stall_aggregate(void);

/* --- Queries -------------------------------------------------------------- */

/* Fill `out` for `domain`. Returns 0 on success, -1 on a bad domain or NULL.
 * A domain whose seam is absent still fills `out` -- with valid == 0, which is
 * the whole point: the reader learns "unmeasured", not "no pressure". */
int quota_stall_get(quota_stall_domain_t domain, quota_stall_snapshot_t *out);

/* Worst level across every VALID domain. Domains whose seam is absent are
 * skipped rather than counted as normal, matching quota_pressure_system_level. */
quota_pressure_level_t quota_stall_system_level(void);

/* Wall-clock timestamp of the most recent CLOSED window (0 = none yet).
 *
 * Distinct from the aggregator's internal anchor, which is set at init and moved
 * on every fold: publishing the anchor would report a timestamp before any
 * window had closed, contradicting the zero-means-none contract that a consumer
 * uses to tell "not started" from "started and quiet". */
uint64_t quota_stall_last_update_ns(void);

/* Number of windows the aggregator has closed since boot. */
uint64_t quota_stall_windows_closed(void);

/* --- Coherent whole-state snapshot ---------------------------------------- *
 * Everything a reporting surface needs, taken under ONE lock acquisition.
 * Assembling the same picture from the individual queries above lets the 2 s
 * aggregation land between two of them, so a caller could observe a window count
 * from after a fold beside domain rows from before it, or a system level that
 * matches none of the rows it is returned with. */
typedef struct quota_stall_summary {
    uint64_t last_update_ns;
    uint64_t windows_closed;
    uint8_t  system_level;    /* quota_pressure_level_t */
    uint8_t  _pad[7];
    quota_stall_snapshot_t domain[QUOTA_STALL_DOMAIN_COUNT];
} quota_stall_summary_t;

/* Fill `out` with a single-generation view of every domain. Returns 0, or -1 on
 * a NULL argument. */
int quota_stall_snapshot_all(quota_stall_summary_t *out);

/* Short domain name ("cpu"/"mem"/"io"), or "?" for an out-of-range domain. */
const char *quota_stall_domain_name(quota_stall_domain_t domain);

/* Render the per-domain stall table to the serial log. */
void quota_stall_dump(void);

#ifdef KERNEL_TESTS
/* Deterministic clock for the unit tests. Every internal time read goes through
 * one accessor, so an override drives the REAL producer and aggregation paths
 * rather than a parallel injection seam that would prove nothing about them.
 * `ns` of 0 restores the live uptime clock. */
void quota_stall_test_set_clock(uint64_t ns);

/* Drop all telemetry back to its boot state so one test cannot see another's
 * accumulated pressure. */
void quota_stall_test_reset(void);
#endif

#endif /* KERNEL_QUOTA_QUOTA_STALL_H */
