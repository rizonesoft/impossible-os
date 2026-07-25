/* ============================================================================
 * quota_pressure.h -- Resource pressure levels, quota-failure events, and the
 *                     escalation surface for the kernel quota subsystem.
 *
 * Owns three things that the quota core deliberately does not:
 *
 *   1. A four-level pressure state machine (normal/watch/warning/critical) per
 *      resource type, driven by ASYMMETRIC hysteresis so a resource hovering on
 *      a threshold cannot flap a level (and therefore cannot flap whatever a
 *      consumer does about it).
 *   2. The quota-failure diagnostic event CONTRACT -- the versioned record a
 *      refused charge produces, its rate limit, and the counters that say how
 *      many were dropped and why.
 *   3. Escalation input: which principal is furthest over its budget. The
 *      nomination is ADVISORY. This kernel does not terminate a nominee; see
 *      the cooperative-only note below.
 *
 * WHAT "PRESSURE" MEANS HERE (read this before consuming a level):
 *   The signal wired today is BUDGET SATURATION -- how close a principal is to
 *   its administrative quota -- and it is published under its own source kind
 *   (QUOTA_PRESSURE_SRC_BUDGET). It is NOT the Linux-PSI-style stall-time
 *   metric: a small administrative quota can saturate on a completely idle
 *   machine, and an unlimited quota has no meaningful ratio at all. The
 *   stall-derived source is a separate, unimplemented input; until it lands its
 *   domains report source_valid == 0.
 *
 *   A domain's sample is ONE COHERENT QUANTITY: the saturation of the most
 *   saturated live USER principal for that resource. It is deliberately not
 *   "whatever block was charged last". Feeding each block's own ratio into a
 *   shared domain would mix identities inside one debounce -- a run of low
 *   samples from unrelated principals would walk the level down while the
 *   offender stayed pinned at its cap, and one uncapped block would mark the
 *   whole domain unknown. The sample is therefore DERIVED from the quota
 *   registry (the USER blocks, one per owner SID) rather than pushed by
 *   whichever charge happened last. Process- and job-level blocks are not in
 *   that registry and so do not contribute; per-process pressure needs a
 *   process-level enumeration that this kernel does not have yet.
 *
 *   VALID means all four of: the source is implemented, it applies to this
 *   domain, it sampled successfully, and its operands were meaningful (an
 *   unlimited limit is NOT meaningful -- there is no ratio). A CLEAR valid flag
 *   means "unknown", never "no pressure": a consumer must ignore the value, and
 *   an invalid sample never feeds debounce and never de-escalates a level.
 *
 * DELIVERY IS BEST EFFORT, RECORDING IS NOT.
 *   Every transition and every admitted failure event is RECORDED exactly once
 *   and carries a monotonic sequence, so a consumer can order and de-duplicate
 *   what it receives. Transport (KNF publish, ETW) can still fail or find no
 *   session; those losses are counted, not hidden. Nothing here claims
 *   exactly-once delivery, because neither transport can provide it.
 *
 * COOPERATIVE-ONLY ESCALATION (architecture decision, 2026-07-24).
 *   The kernel NOMINATES an over-budget principal and publishes the nomination.
 *   It never terminates one. That matches Windows (the resource-exhaustion path
 *   informs; user mode decides) and it is the conservative choice while no
 *   supervisor exists to arbitrate: an in-kernel killer with no policy plane
 *   above it can only pick a victim by a heuristic nobody authorized.
 *
 * CONTEXT RULES.
 *   The sampling and failure-event entry points are callable wherever the quota
 *   charge API itself is callable, INCLUDING at DISPATCH_LEVEL and from
 *   interrupt context, and they never call out to a transport inline: a record
 *   is copied into a bounded preallocated ring and a threaded DPC publishes it
 *   at PASSIVE_LEVEL. That is a requirement, not an optimization -- knf_publish
 *   takes its own IRQ-disabling spinlock and is rejected above DISPATCH_LEVEL,
 *   so publishing inline from a refusal path would both invent a lock order
 *   through the quota charge path and be illegal at raised IRQL.
 * ========================================================================== */

#ifndef KERNEL_QUOTA_PRESSURE_H
#define KERNEL_QUOTA_PRESSURE_H

#include "kernel/types.h"
#include "kernel/quota/quota.h"

/* --- Levels --------------------------------------------------------------- */

typedef enum quota_pressure_level {
    QUOTA_PRESSURE_NORMAL   = 0,
    QUOTA_PRESSURE_WATCH    = 1,
    QUOTA_PRESSURE_WARNING  = 2,
    QUOTA_PRESSURE_CRITICAL = 3,
    QUOTA_PRESSURE_LEVEL_COUNT = 4,
    /* "No instrument has reported yet" -- NOT a fifth degree of pressure.
     *
     * WHY IT IS OUT OF BAND rather than appended as 4. The four levels above are
     * an ORDER, and every fold over them is a `worse-than` comparison. An
     * appended value would sit above CRITICAL, so a domain nobody has measured
     * would outrank a domain that is genuinely on fire, and any future
     * `level >= WARNING` test would read "unmeasured" as "act now". Numbering it
     * below NORMAL is no better: NORMAL is 0 in a published ABI record.
     *
     * So it sits outside both the order and QUOTA_PRESSURE_LEVEL_COUNT, which
     * means it is ALSO not an array index -- nothing sized by LEVEL_COUNT grows,
     * and a fold must handle it explicitly (see quota_pressure_level_measured)
     * rather than absorb it silently. It still fits the uint8_t the published
     * record uses for a level, so it costs no ABI width.
     *
     * It is a RETURN value from the two system-level composites only. No domain
     * ever stores it: a domain says "unmeasured" with its VALID flag, which is
     * the same distinction this expresses one level up. */
    QUOTA_PRESSURE_UNKNOWN  = 0xFFu
} quota_pressure_level_t;

/* Is `level` one of the four ordered degrees, i.e. is it comparable and
 * indexable? Use this before any `worse-than` comparison on a value that could
 * have come from a system-level composite. */
static inline int quota_pressure_level_measured(quota_pressure_level_t level)
{
    return (uint32_t)level < (uint32_t)QUOTA_PRESSURE_LEVEL_COUNT;
}

_Static_assert(QUOTA_PRESSURE_UNKNOWN > QUOTA_PRESSURE_CRITICAL,
               "UNKNOWN must not collide with an ordered level");
_Static_assert(QUOTA_PRESSURE_UNKNOWN >= QUOTA_PRESSURE_LEVEL_COUNT,
               "UNKNOWN must stay outside the array-index space LEVEL_COUNT "
               "sizes, so no table indexed by a level has to hold a slot for it");
_Static_assert(QUOTA_PRESSURE_UNKNOWN <= 0xFFu,
               "a level travels as uint8_t in the published pressure record");

/* --- Source kinds --------------------------------------------------------- *
 * WHICH instrument produced a sample. Carried in every published record so a
 * consumer can tell budget saturation from stall time rather than inferring it
 * from the domain. Values are ABI; append only. */
typedef enum quota_pressure_source {
    QUOTA_PRESSURE_SRC_NONE   = 0,  /* never sampled, or not applicable       */
    QUOTA_PRESSURE_SRC_BUDGET = 1,  /* administrative quota saturation        */
    QUOTA_PRESSURE_SRC_STALL  = 2   /* PSI-shaped stall time (not wired yet)  */
} quota_pressure_source_t;

/* --- Domains -------------------------------------------------------------- *
 * One pressure domain per quota resource type: pressure is a property of a
 * budgeted resource, so reusing the taxonomy keeps the two from drifting. */
#define QUOTA_PRESSURE_DOMAIN_COUNT  QUOTA_RESOURCE_TYPE_COUNT

_Static_assert(QUOTA_PRESSURE_DOMAIN_COUNT == QUOTA_RESOURCE_TYPE_COUNT,
               "pressure domains must track the quota resource taxonomy 1:1");

/* --- Hysteresis band ------------------------------------------------------ *
 * Saturation is expressed in PERMILLE (0..1000) rather than percent so the
 * band between a rise and a fall threshold has usable resolution.
 *
 * The RISE threshold for a level is strictly above the FALL threshold for that
 * same level, and the debounce counts are asymmetric (a level is entered faster
 * than it is left). Leaving a level slowly is deliberate: a consumer that
 * trimmed something in response should not immediately be told to undo it
 * because usage dipped for one sample. */
#define QUOTA_PRESSURE_RISE_WATCH_PERMILLE      700u
#define QUOTA_PRESSURE_RISE_WARNING_PERMILLE    850u
#define QUOTA_PRESSURE_RISE_CRITICAL_PERMILLE   950u

#define QUOTA_PRESSURE_FALL_WATCH_PERMILLE      600u
#define QUOTA_PRESSURE_FALL_WARNING_PERMILLE    780u
#define QUOTA_PRESSURE_FALL_CRITICAL_PERMILLE   900u

_Static_assert(QUOTA_PRESSURE_FALL_WATCH_PERMILLE < QUOTA_PRESSURE_RISE_WATCH_PERMILLE &&
               QUOTA_PRESSURE_FALL_WARNING_PERMILLE < QUOTA_PRESSURE_RISE_WARNING_PERMILLE &&
               QUOTA_PRESSURE_FALL_CRITICAL_PERMILLE < QUOTA_PRESSURE_RISE_CRITICAL_PERMILLE,
               "each level must have a non-empty hysteresis band, or it flaps");
_Static_assert(QUOTA_PRESSURE_RISE_WATCH_PERMILLE < QUOTA_PRESSURE_RISE_WARNING_PERMILLE &&
               QUOTA_PRESSURE_RISE_WARNING_PERMILLE < QUOTA_PRESSURE_RISE_CRITICAL_PERMILLE,
               "rise thresholds must be strictly increasing with level");
_Static_assert(QUOTA_PRESSURE_FALL_WATCH_PERMILLE < QUOTA_PRESSURE_FALL_WARNING_PERMILLE &&
               QUOTA_PRESSURE_FALL_WARNING_PERMILLE < QUOTA_PRESSURE_FALL_CRITICAL_PERMILLE,
               "fall thresholds must be strictly increasing with level");

/* The bands, as accessors rather than as a table each lane copies.
 *
 * The stall lane (quota_stall.c) crosses the SAME bands as the budget lane, and
 * an earlier revision gave each its own `g_rise_permille` / `g_fall_permille`
 * array built from these macros. Same values, two definitions -- so an edit to
 * one table silently changed only one lane's thresholds while the other kept the
 * old behavior, with nothing in the build to notice. Indexed by the level being
 * ENTERED (rise) or LEFT (fall); NORMAL has no threshold in either direction. */
static inline uint16_t quota_pressure_rise_threshold(uint32_t level)
{
    switch (level) {
    case QUOTA_PRESSURE_WATCH:    return QUOTA_PRESSURE_RISE_WATCH_PERMILLE;
    case QUOTA_PRESSURE_WARNING:  return QUOTA_PRESSURE_RISE_WARNING_PERMILLE;
    case QUOTA_PRESSURE_CRITICAL: return QUOTA_PRESSURE_RISE_CRITICAL_PERMILLE;
    default:                      return 0;
    }
}

static inline uint16_t quota_pressure_fall_threshold(uint32_t level)
{
    switch (level) {
    case QUOTA_PRESSURE_WATCH:    return QUOTA_PRESSURE_FALL_WATCH_PERMILLE;
    case QUOTA_PRESSURE_WARNING:  return QUOTA_PRESSURE_FALL_WARNING_PERMILLE;
    case QUOTA_PRESSURE_CRITICAL: return QUOTA_PRESSURE_FALL_CRITICAL_PERMILLE;
    default:                      return 0;
    }
}

/* Consecutive qualifying samples required to move one level. Rising is faster
 * than falling, matching the block-telemetry mode machine's shape. */
#define QUOTA_PRESSURE_RISE_SAMPLES   3u
#define QUOTA_PRESSURE_FALL_SAMPLES   5u

/* Consecutive UNKNOWN samples that reset a domain sitting at a live level.
 * Longer than the fall debounce on purpose: an unknown is weaker evidence than
 * a measured low reading, so it should take longer to act on. */
#define QUOTA_PRESSURE_INVALID_RESET_SAMPLES  10u

_Static_assert(QUOTA_PRESSURE_INVALID_RESET_SAMPLES > QUOTA_PRESSURE_FALL_SAMPLES,
               "an unknown must clear a level more slowly than a measured low does");

/* NO SOURCE ARBITRATION INSIDE THIS SPACE, deliberately.
 *
 * A domain here carries ONE set of debounce counters, shared by whichever source
 * last sampled it. Since real stall telemetry lives in its own lane
 * (quota_stall.h) with its own validity, debounce, and level, nothing in a
 * running kernel drives a domain from two sources -- so there is nothing to
 * arbitrate. See quota_pressure_submit_stall's comment for why the obvious
 * arbitration (letting a stall observation hold the domain against the budget
 * sampler) was implemented, measured against its failure mode, and removed: it
 * could suppress budget sampling of a saturated domain indefinitely, which
 * trades a mistimed reading for a hidden one. */

/* Full scale for a saturation sample. */
#define QUOTA_PRESSURE_PERMILLE_MAX   1000u

/* The slow-path long division walks a fixed ten-bit multiplier. Changing the
 * scale without widening that traversal would silently ignore the higher bits
 * and compile clean, so the two are pinned together here. */
#define QUOTA_PRESSURE_PERMILLE_BITS  10u
_Static_assert(QUOTA_PRESSURE_PERMILLE_MAX < (1u << QUOTA_PRESSURE_PERMILLE_BITS) &&
               QUOTA_PRESSURE_PERMILLE_MAX >= (1u << (QUOTA_PRESSURE_PERMILLE_BITS - 1u)),
               "permille traversal width must cover exactly the permille scale");

/* --- Published records (ABI) ---------------------------------------------- *
 * PACKED and size-asserted for the same reason the KNF publish record is: an
 * out-of-tree reader reconstructs these from the event id alone, so a natural
 * layout's tail padding would misalign every consumer. Append-only; bump the
 * layout version when a field is added. */

#define QUOTA_PRESSURE_RECORD_VERSION    1u
#define QUOTA_FAILURE_RECORD_VERSION     1u
#define QUOTA_NOMINATION_RECORD_VERSION  1u

/* A level transition for one domain. */
typedef struct quota_pressure_record {
    uint32_t layout_version;   /* QUOTA_PRESSURE_RECORD_VERSION            */
    uint32_t resource;         /* quota_resource_type_t of the domain      */
    uint8_t  from_level;       /* quota_pressure_level_t before            */
    uint8_t  to_level;         /* quota_pressure_level_t after             */
    uint8_t  source_kind;      /* quota_pressure_source_t that drove it    */
    uint8_t  source_valid;     /* 1 = operands meaningful, 0 = unknown     */
    uint16_t sample_permille;  /* the sample that completed the debounce   */
    uint16_t window_ms;        /* interval summarized (0 = event-driven)   */
    uint64_t transition_seq;   /* monotonic across all domains             */
    uint64_t timestamp_ns;     /* uptime_ns at the transition              */
} __attribute__((packed)) QUOTA_PRESSURE_RECORD;

_Static_assert(sizeof(QUOTA_PRESSURE_RECORD) == 32,
               "pressure record layout is ABI: consumers decode it by event id");
_Static_assert(__builtin_offsetof(QUOTA_PRESSURE_RECORD, transition_seq) == 16 &&
               __builtin_offsetof(QUOTA_PRESSURE_RECORD, timestamp_ns) == 24,
               "pressure record field offsets are ABI");

/* One refused charge. This is the section's owned diagnostic contract: who was
 * refused, for what, how much they wanted, where they stood, and what the cap
 * was -- enough to act on without a second query, which matters because by the
 * time a consumer reads this the counters have already moved. */
typedef struct quota_failure_record {
    uint32_t layout_version;   /* QUOTA_FAILURE_RECORD_VERSION             */
    uint32_t resource;         /* quota_resource_type_t refused            */
    uint64_t block_id;         /* identity of the refusing quota block     */
    uint64_t requested;        /* amount asked for                         */
    uint64_t current;          /* usage at the moment of refusal           */
    uint64_t limit;            /* cap in force (0 = unlimited)             */
    uint64_t owner_sid_hash;   /* digest of the owner SID, 0 if none       */
    uint64_t event_seq;        /* monotonic; survives rate-limit drops     */
    uint64_t timestamp_ns;     /* uptime_ns at the refusal                 */
    int32_t  result;           /* NTSTATUS returned to the caller          */
    uint32_t pid;              /* refused task, 0 if none current          */
    uint32_t tid;              /* refused thread, 0 if none current        */
    uint32_t owner_rid;        /* last sub-authority of the owner SID      */
    uint8_t  principal;        /* quota_principal_t of the refusing block  */
    uint8_t  attribution;      /* QUOTA_ATTRIB_* flags for pid/tid trust   */
    uint8_t  _reserved[6];     /* zero; keeps the record 8-byte sized      */
} __attribute__((packed)) QUOTA_FAILURE_RECORD;

/* How far a consumer may trust the pid/tid in a failure record.
 *
 * block_id plus the SID digest are AUTHORITATIVE -- they come from the block
 * that actually refused. pid/tid are a convenience, and today they are only
 * BEST_EFFORT: this kernel resolves the current task and thread through global
 * scheduler cursors rather than per-CPU state, so a concurrent schedule on
 * another CPU can make them name an unrelated process. The flag is in the
 * record rather than in a comment because a consumer that acts on identity
 * needs to know which fields it may act on. */
#define QUOTA_ATTRIB_PID_VALID       0x1u  /* a task was current at capture     */
#define QUOTA_ATTRIB_TID_VALID       0x2u  /* a thread was current at capture   */
#define QUOTA_ATTRIB_BEST_EFFORT     0x4u  /* pid/tid read from a global cursor */

_Static_assert(sizeof(QUOTA_FAILURE_RECORD) == 88,
               "failure record layout is ABI: consumers decode it by event id");
_Static_assert(__builtin_offsetof(QUOTA_FAILURE_RECORD, requested) == 16 &&
               __builtin_offsetof(QUOTA_FAILURE_RECORD, limit) == 32 &&
               __builtin_offsetof(QUOTA_FAILURE_RECORD, result) == 64 &&
               __builtin_offsetof(QUOTA_FAILURE_RECORD, principal) == 80 &&
               __builtin_offsetof(QUOTA_FAILURE_RECORD, attribution) == 81,
               "failure record field offsets are ABI");

/* An escalation nominee. Carries its own expiry because it names a principal a
 * consumer may act on LATER: a nomination that outlives the condition that
 * produced it would otherwise keep pointing at a principal that has since
 * returned its charges. A consumer must re-validate before acting. */
typedef struct quota_nomination_record {
    uint32_t layout_version;   /* QUOTA_NOMINATION_RECORD_VERSION          */
    uint32_t resource;         /* quota_resource_type_t under pressure     */
    uint64_t block_id;         /* identity of the nominated quota block    */
    uint64_t owner_sid_hash;   /* digest of the owner SID, 0 if none       */
    uint64_t nomination_seq;   /* monotonic                                */
    uint64_t nominated_at_ns;  /* uptime_ns when selected                  */
    uint64_t expires_at_ns;    /* after this, re-nominate instead of using */
    uint32_t owner_rid;        /* last sub-authority of the owner SID      */
    uint16_t over_permille;    /* saturation that qualified the nominee    */
    uint8_t  principal;        /* quota_principal_t of the nominated block */
    uint8_t  valid;            /* 1 = a nominee; 0 = explicit clear        */
} __attribute__((packed)) QUOTA_NOMINATION_RECORD;

_Static_assert(sizeof(QUOTA_NOMINATION_RECORD) == 56,
               "nomination record layout is ABI");
_Static_assert(__builtin_offsetof(QUOTA_NOMINATION_RECORD, expires_at_ns) == 40 &&
               __builtin_offsetof(QUOTA_NOMINATION_RECORD, valid) == 55,
               "nomination record field offsets are ABI");

/* How long a nomination stays actionable. */
#define QUOTA_NOMINATION_TTL_MS   5000u

/* --- Failure-event rate limit --------------------------------------------- *
 * A token bucket rather than a fixed window: a refusal STORM is exactly when
 * the events matter most, and a fixed window either truncates the burst that
 * carries the diagnosis or admits a steady flood. The bucket admits a burst,
 * then settles to the refill rate; everything refused is counted. */
#define QUOTA_FAILURE_EVENT_BURST        32u
#define QUOTA_FAILURE_EVENT_REFILL       8u
#define QUOTA_FAILURE_EVENT_WINDOW_MS    1000u

/* Bounded deferred-publish ring. Sized so a burst that fills the bucket still
 * fits without dropping, leaving the drop counter meaningful (a non-zero ring
 * drop means the publisher, not the producer, fell behind). */
#define QUOTA_PRESSURE_RING_SLOTS        64u

_Static_assert(QUOTA_PRESSURE_RING_SLOTS >= QUOTA_FAILURE_EVENT_BURST,
               "ring must absorb a full token-bucket burst or drops are noise");

/* Per-domain overflow FIFO for transitions the shared ring could not take.
 * Four is enough to hold a full walk across every level in one direction, so a
 * complete escalation or recovery sequence survives a ring that a failure burst
 * has filled. */
#define QUOTA_PRESSURE_PENDING_SLOTS  4u

_Static_assert(QUOTA_PRESSURE_PENDING_SLOTS >= QUOTA_PRESSURE_LEVEL_COUNT - 1u,
               "pending FIFO must hold a full climb from normal to critical");

/* Records published per drain invocation before the callback re-arms and
 * returns. The threaded-DPC worker is shared system-wide, so an unbounded
 * drain under sustained production would starve every other threaded DPC. */
#define QUOTA_PRESSURE_DRAIN_BUDGET   32u

/* Slice of that budget reserved for the per-domain overflow FIFO, drained
 * before the ring so sustained ring traffic cannot starve level records. */
#define QUOTA_PRESSURE_PENDING_RESERVE  8u

_Static_assert(QUOTA_PRESSURE_PENDING_RESERVE < QUOTA_PRESSURE_DRAIN_BUDGET,
               "the reserve must leave budget for the ring, or the ring starves instead");

/* --- KNF publication identity --------------------------------------------- */
#define QUOTA_PRESSURE_KNF_CATEGORY  "Kernel"
#define QUOTA_PRESSURE_KNF_STATE     "QuotaPressure"
#define QUOTA_FAILURE_KNF_STATE      "QuotaFailure"
#define QUOTA_NOMINATION_KNF_STATE   "QuotaNomination"

/* --- Saturation helper ---------------------------------------------------- *
 * Shared by the sampler and the registry walk so the two cannot disagree about
 * what saturation means. Returns permille in 0..QUOTA_PRESSURE_PERMILLE_MAX, or
 * QUOTA_PRESSURE_INVALID_PERMILLE when the operands carry no ratio (an
 * unlimited cap). Overflow-safe: usage scaled by the permille factor is only
 * formed when the product cannot leave the 64-bit domain. */
#define QUOTA_PRESSURE_INVALID_PERMILLE  0xFFFFu
#define QUOTA_PRESSURE_U64_MAX           0xFFFFFFFFFFFFFFFFULL

static inline uint16_t quota_pressure_permille(uint64_t usage, uint64_t limit)
{
    if (limit == QUOTA_LIMIT_UNLIMITED)
        return QUOTA_PRESSURE_INVALID_PERMILLE;   /* no ratio exists */
    if (usage >= limit)
        return (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX;

    /* Fast path: the product fits, so compute it directly. */
    if (usage <= (QUOTA_PRESSURE_U64_MAX / QUOTA_PRESSURE_PERMILLE_MAX))
        return (uint16_t)((usage * QUOTA_PRESSURE_PERMILLE_MAX) / limit);

    /* Slow path: usage * 1000 would leave the 64-bit domain.
     *
     * Both obvious shortcuts are WRONG in the same direction, and wrong exactly
     * where it matters. usage/(limit/1000) rounds the DIVISOR down, and scaling
     * both operands down together truncates the divisor too -- either way the
     * ratio reads HIGH and can report a threshold that was never crossed (21e15
     * of 30e15+1 is 699 permille; both shortcuts answer 700 and trip the watch
     * threshold on a resource that never reached it).
     *
     * So compute floor(usage * 1000 / limit) EXACTLY, by bitwise long division
     * over the multiplier. The running value is carried as (quotient q,
     * remainder rem) with rem < limit invariant; doubling and conditionally
     * adding `usage` reproduces the multiply one bit at a time. Nothing ever
     * exceeds 2*limit, and limit is bounded by the counter domain
     * (QUOTA_AMOUNT_MAX < 2^63), so 2*limit still fits in 64 bits.
     *
     * Ten iterations, and only for a limit above roughly 1.8e16 -- this is off
     * the charge path entirely (the sampler derives at most once per window). */
    /* The loop needs 2*limit to stay in 64 bits. Every limit a quota block can
     * hold is clamped to QUOTA_AMOUNT_MAX (< 2^63), so that always holds for
     * real operands. A caller passing something larger is outside the counter
     * domain entirely: report UNKNOWN rather than an approximation, because
     * approximating here is exactly the rounding that invents threshold
     * crossings. */
    if (limit > (uint64_t)QUOTA_AMOUNT_MAX)
        return QUOTA_PRESSURE_INVALID_PERMILLE;

    uint64_t q = 0, rem = 0;
    for (int bit = (int)QUOTA_PRESSURE_PERMILLE_BITS - 1; bit >= 0; bit--) {
        q   <<= 1;
        rem <<= 1;
        if (rem >= limit) { rem -= limit; q += 1; }
        if ((QUOTA_PRESSURE_PERMILLE_MAX >> bit) & 1u) {
            rem += usage;
            if (rem >= limit) { rem -= limit; q += 1; }
        }
    }
    return (q > QUOTA_PRESSURE_PERMILLE_MAX)
         ? (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX : (uint16_t)q;
}

/* --- Lifecycle ------------------------------------------------------------ */

/* Create the publication states and arm the deferred-publish DPC. Called once
 * in Phase 3, after KNF exists and the threaded-DPC worker is running. The
 * state machine itself works without this: sampling, levels, and recording are
 * live from the first charge, and only PUBLICATION waits for init. */
void quota_pressure_init(void);

/* Non-zero once publication is armed. */
int quota_pressure_ready(void);

/* --- Sampling seams ------------------------------------------------------- */

/* Cadence of the periodic sampler. The derivation walks the quota registry, so
 * it runs on a timer rather than on every charge. */
#define QUOTA_PRESSURE_SAMPLE_WINDOW_MS  50u

/* MARK `type` as possibly moved. One relaxed atomic store; no lock, no registry
 * walk, no clock read -- so it is callable at any IRQL the quota charge API is,
 * including interrupt context, and costs the charge path essentially nothing.
 *
 * Called from EVERY mutation that can move saturation: charge, return, both
 * sides of a transfer, a limit change, and the per-user default re-limit.
 *
 * It does NOT sample. Sampling is the periodic sampler's job, and that
 * separation is load-bearing twice over. First, a mutation-driven sample cannot
 * complete a debounce: rising takes three consecutive samples and falling five,
 * so a process that charges to its cap once and then goes quiet would never
 * produce the follow-up samples its own pressure needs, and the level would
 * never rise. Second, a single periodic producer means samples have a
 * well-defined ORDER; concurrent derivations could otherwise apply an older
 * reading after a newer one and reverse a transition. */
void quota_pressure_note_activity(quota_resource_type_t type);

/* Sample every domain that needs it and step each one's hysteresis.
 *
 * A domain needs sampling when it is dirty, when its level is not normal, or
 * when a debounce is part-way through -- so a resource under steady pressure
 * keeps being measured until its level settles, and a fully idle system stops
 * paying for the walk entirely. Driven by a periodic timer once armed; exposed
 * so a test can step the sampler deterministically. */
void quota_pressure_sample_all(void);

/* Feed one PSI-shaped stall observation for `type`, in permille of the window.
 * This is the seam the stall-telemetry work fills; nothing calls it today, and
 * until something does, stall-sourced domains report source_valid == 0 rather
 * than a fabricated zero. `window_ms` is the interval the sample summarizes. */
void quota_pressure_submit_stall(quota_resource_type_t type,
                                 uint16_t permille, uint16_t window_ms);

/* --- Failure events ------------------------------------------------------- */

/* Everything a refusal knows, captured by the caller while it still holds the
 * facts. Passed by pointer for argument sanity; the callee COPIES it into the
 * ring before returning, so caller stack storage is fine. */
typedef struct quota_failure_source {
    quota_resource_type_t type;
    quota_principal_t     principal;
    uint64_t              block_id;
    uint64_t              requested;
    uint64_t              current;
    uint64_t              limit;
    uint64_t              owner_sid_hash;
    uint32_t              owner_rid;
    NTSTATUS              result;
    /* Captured by the CALLER at the instant of refusal, not re-read later: the
     * event path takes a lock on the way in, and a reschedule during it would
     * otherwise let the record name a process that had nothing to do with the
     * refusal. Still best-effort (see QUOTA_ATTRIB_BEST_EFFORT). */
    uint32_t              pid;
    uint32_t              tid;
    uint8_t               attribution;   /* QUOTA_ATTRIB_* */
} quota_failure_source_t;

/* Record one refused charge. Rate-limited; a refused event still advances the
 * sequence so a consumer can see that something was dropped between two it
 * received. MUST be called with no quota block lock held. */
void quota_pressure_note_failure(const quota_failure_source_t *src);

/* --- Queries -------------------------------------------------------------- */

quota_pressure_level_t  quota_pressure_level(quota_resource_type_t type);
quota_pressure_source_t quota_pressure_source_kind(quota_resource_type_t type);
int                     quota_pressure_source_valid(quota_resource_type_t type);

/* Highest level across every VALID domain of EITHER lane (budget saturation and
 * PSI-shaped stall), or QUOTA_PRESSURE_UNKNOWN when not one domain in either
 * lane has been measured yet.
 *
 * Domains whose source is invalid are skipped rather than counted as normal --
 * an unknown is not a zero -- and that rule is what forces the UNKNOWN return:
 * with every domain skipped there is no evidence at all, and answering NORMAL
 * would state the system is calm on the strength of having measured nothing.
 * That is exactly the confusion this subsystem refuses to make one level down,
 * where a domain carries a VALID flag beside its numbers.
 *
 * CALLERS MUST NOT ORDER-COMPARE THE RESULT BLINDLY: UNKNOWN is outside the
 * ordered band (see the enum). Test quota_pressure_level_measured first, or
 * treat UNKNOWN as its own case. */
quota_pressure_level_t  quota_pressure_system_level(void);

/* Human-readable level name ("normal"/"watch"/"warning"/"critical"/"unknown"),
 * or "?" for a value that is none of those. */
const char *quota_pressure_level_name(quota_pressure_level_t level);

/* --- Diagnostics ---------------------------------------------------------- *
 * Every drop has its own counter: a consumer that sees gaps needs to know
 * whether the producer was throttled, the ring overflowed, or the transport
 * failed, because the three call for different responses. */
uint64_t quota_pressure_transition_count(void);
uint64_t quota_pressure_events_recorded(void);
uint64_t quota_pressure_dropped_ratelimit(void);
uint64_t quota_pressure_dropped_ring(void);
uint64_t quota_pressure_dropped_transport(void);

/* Render the per-domain level table to the serial log. */
void quota_pressure_dump(void);

/* --- Escalation ----------------------------------------------------------- */

/* Select the MOST SATURATED live USER principal for `type` -- at or above the
 * watch rise threshold -- and fill `out`.
 *
 * Deliberately "most saturated at or above watch", not "furthest over budget":
 * saturation is clamped at full scale, so a principal at 101 percent of its cap
 * and one at 1000 percent are indistinguishable here, and ranking them would be
 * inventing precision the measurement does not carry. A consumer that needs
 * true overage must read the raw counters.
 *
 * This function only FILLS the record. Publication happens from the drain when
 * a domain transitions into or out of critical; a direct caller gets the
 * nominee, not a published event.
 *
 * Walks the quota registry, which is the only principal enumeration in this
 * kernel with a lifetime contract: the registry lock covers the linkage and the
 * counters are read through their lock-free atomic loads, so no block lock is
 * taken under the registry lock (the quota core forbids that ordering) and no
 * block can be freed mid-walk.
 *
 * Returns 1 when a nominee qualified (saturation at or above the watch rise
 * threshold), 0 otherwise -- in which case `out` is filled as an explicit CLEAR
 * (valid == 0) so a consumer can distinguish "nobody qualifies now" from "no
 * answer". Advisory: the caller must re-validate before acting, and this kernel
 * never terminates a nominee. */
int quota_pressure_nominate(quota_resource_type_t type,
                            QUOTA_NOMINATION_RECORD *out);

/* Registry-side half of the nomination, implemented in quota.c because the
 * block layout and the registry lock are private there. Fills the identity and
 * saturation of the most-saturated live USER block for `type`, and returns 1
 * when one qualified at or above `min_permille`. Not part of the escalation
 * contract itself -- callers use quota_pressure_nominate. */
int quota_registry_worst_user(quota_resource_type_t type, uint16_t min_permille,
                              uint64_t *out_block_id, uint32_t *out_rid,
                              uint64_t *out_sid_hash, uint16_t *out_permille);

/* Same walk, but answering EVERY resource at once: fills out_permille[t] with
 * the worst live USER principal's saturation for type t, or
 * QUOTA_PRESSURE_INVALID_PERMILLE where no capped principal exists. One pass
 * under one registry-lock hold, because the sampler needs all of them each tick
 * and the per-type variant would take that IRQ-off lock once per type over the
 * same blocks. */
void quota_registry_worst_all(uint16_t *out_permille, uint32_t count,
                              const uint8_t *wanted);

/* --- Ring record kinds ---------------------------------------------------- *
 * Which record a publish-ring slot holds. NOT test-only: the drain and the
 * enqueue paths in quota_pressure.c switch on these unconditionally, so they
 * must exist in a KERNEL_TESTS=off build too. They previously sat inside the
 * test-only block below, which left the release build unbuildable. */
#define QUOTA_PRESSURE_KIND_TRANSITION  1u
#define QUOTA_PRESSURE_KIND_FAILURE     2u

/* --- Notification-channel retry policy ------------------------------------ *
 *
 * The SCHEDULE for retrying a notification channel whose creation was refused for
 * a transient reason, as pure state plus pure functions over an explicit clock.
 *
 * Separated from the live path rather than left inline, because inline it is
 * unprovable: the live path owns the clock and the create call, so exercising the
 * schedule through it needs an injectable clock and an injectable creator (filed
 * in section 17). The POLICY is arithmetic over explicit state, so it lives here
 * and is tested directly; the live path just holds one of these structs. What
 * stays unproven is then the wiring alone, not the decision.
 *
 * Every mutation goes through the functions below, so the invariants stay in one
 * place. */
typedef struct quota_pressure_retry {
    uint64_t at_ns;    /* earliest uptime at which the next attempt may run */
    uint64_t gap_ns;   /* current interval; doubles per failure, capped     */
    uint32_t tries;    /* consecutive failed attempts                      */
    uint8_t  warned;   /* the threshold message has been emitted once      */
} quota_pressure_retry_t;

#define QUOTA_PRESSURE_RETRY_FIRST_NS   (100ull * 1000ull * 1000ull)          /* 100 ms */
#define QUOTA_PRESSURE_RETRY_MAX_NS     (30ull * 1000ull * 1000ull * 1000ull) /*   30 s */

/* Attempts after which the situation is REPORTED once. A reporting threshold,
 * deliberately NOT a give-up: retries continue at the capped cadence for the life
 * of the system. An attempt ceiling was written and then removed -- neither
 * retryable status (a charge gate closed by a job-membership transition, or
 * resource exhaustion) has any upper bound on how long it lasts, so a finite
 * deadline turns "not yet" back into "never" and loses the channel for the rest of
 * the boot. Bounding the RATE is what a retry storm needs, and the backoff does
 * exactly that. */
#define QUOTA_PRESSURE_RETRY_WARN_TRIES  8u

/* Would retrying this status plausibly succeed later? STATUS_RETRY means a
 * job-membership transition holds the creating task's charge gate shut, which ends
 * on its own; resource exhaustion may also clear. Everything else (a bad argument,
 * a missing category, a privilege refusal, a name collision) fails identically
 * forever, and retrying it would be a log-spam loop rather than recovery. */
int  quota_pressure_retry_status_retryable(NTSTATUS status);

/* Is an attempt DUE? False when nothing is pending, and false until the deadline
 * -- which is what stops a caller that runs often (the drain re-arms for queued
 * records) from turning a pending channel into a retry storm. */
int  quota_pressure_retry_due(const quota_pressure_retry_t *st, uint32_t pending,
                              uint64_t now_ns);

/* Fold one attempt's outcome into the schedule. `pending` is the mask STILL
 * unfinished after that attempt: nonzero doubles the interval toward the cap and
 * counts the try; zero resets the struct, because the retry is over. `*out_warn`
 * (optional) is set to 1 exactly once, on the attempt that first reaches
 * QUOTA_PRESSURE_RETRY_WARN_TRIES, so a caller logs once and keeps going. */
void quota_pressure_retry_advance(quota_pressure_retry_t *st, uint32_t pending,
                                  uint64_t now_ns, int *out_warn);

#ifdef KERNEL_TESTS
/* --- Test-only control ---------------------------------------------------- *
 * The state machine is deliberately reachable without quota_pressure_init, so
 * tests exercise levels, debounce, the event contract, and the rate limit
 * without touching live boot infrastructure. */

/* Clear every domain, counter, and ring slot back to first-boot state, AND
 * hold publication so the live drain cannot consume the records a test is
 * about to inspect. In a booted test kernel the threaded-DPC publisher is
 * already running, so without the hold every ring assertion would race it. */
void quota_pressure_test_reset(void);

/* Release the publication hold taken by the reset above. Registered as the
 * last test in the suite so the transport is live again afterwards. */
void quota_pressure_test_release(void);

/* Run the drain body synchronously, ignoring the hold. Gives the publish path
 * deterministic coverage without waiting on (or depending on) the DPC worker. */
void quota_pressure_test_drain(void);

/* Step the state machine with a sample directly, bypassing the registry
 * derivation. Lets the hysteresis tests be exact without staging live blocks.
 *
 */
void quota_pressure_test_sample(quota_resource_type_t type, uint16_t permille);

/* Pop the oldest undrained ring record. Returns the record kind (one of the
 * QUOTA_PRESSURE_KIND_* values above), or 0 when the ring is empty, and fills
 * the matching out-pointer. */
uint32_t quota_pressure_test_pop(QUOTA_PRESSURE_RECORD *out_transition,
                                 QUOTA_FAILURE_RECORD *out_failure);

/* Number of records waiting in the ring. */
uint32_t quota_pressure_test_pending(void);

/* Refill the failure-event token bucket to full without waiting a window. */
void quota_pressure_test_refill_tokens(void);
#endif /* KERNEL_TESTS */

#endif /* KERNEL_QUOTA_PRESSURE_H */
