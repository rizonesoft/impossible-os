/* ============================================================================
 * quota_pressure.c -- Pressure levels, quota-failure events, and escalation
 *                     input for the kernel quota subsystem.
 *
 * Structure (three planes, deliberately separated):
 *
 *   SAMPLE plane   -- quota_pressure_note_saturation / _submit_stall. Runs on
 *                     the charge path, at whatever IRQL the charge ran at.
 *                     Takes at most one domain lock, does no I/O, calls out to
 *                     nothing. Its only side effect beyond the level is to drop
 *                     a record into the ring.
 *   RECORD plane   -- the bounded ring. One irqsave spinlock, held across a
 *                     struct copy and nothing else.
 *   PUBLISH plane  -- the threaded DPC drain. Runs at PASSIVE_LEVEL with no
 *                     lock of ours held, so it may call klog, ETW, and KNF.
 *
 * The separation is load-bearing rather than tidy. quota.h documents the charge
 * API as safe at DISPATCH_LEVEL and from interrupt context, while knf_publish
 * is rejected above DISPATCH_LEVEL and takes its own IRQ-disabling spinlock. A
 * transport call made inline from a refusal would therefore be illegal at
 * raised IRQL AND would thread a foreign lock through the charge path's
 * ordering. Recording is synchronous so nothing is lost; publishing is
 * deferred so nothing is done in a context that forbids it.
 *
 * Lock order, and the only two locks this file takes:
 *     domain lock  ->  ring lock       (a transition records under its domain)
 *     bucket lock  ->  ring lock       (an admitted failure event records)
 * The ring lock is a leaf: nothing is called while it is held. No path takes a
 * domain lock and a bucket lock together, and no path here ever takes a quota
 * BLOCK lock -- quota.h forbids re-entering the quota API under one, and this
 * file only ever uses the lock-free counter queries.
 * ========================================================================== */

#include "kernel/quota/quota_pressure.h"
#include "kernel/quota/quota_stall.h"    /* the independent stall lane */
#include "kernel/quota/quota.h"
#include "kernel/knf/knf.h"
#include "kernel/etw.h"
#include "kernel/klog.h"
#include "kernel/timer.h"                /* uptime_ns */
#include "kernel/sched/spinlock.h"
#include "kernel/sched/dpc.h"
#include "kernel/sched/ktimer.h"
#include "kernel/sched/irql.h"           /* the PASSIVE_LEVEL guard on creation */

/* Freestanding: no string.h. Declared locally, same as every other kernel TU
 * that needs it (knf.c does the same). */
extern void *memset(void *s, int c, size_t n);

/* Nanoseconds per millisecond. timer.h names NSEC_PER_SEC for the same reason
 * (accounting math should not carry bare literals) but has no millisecond
 * counterpart. */
#define QUOTA_NSEC_PER_MSEC  1000000ull

/* ==========================================================================
 * Per-domain state
 * ========================================================================== */

/* One domain's whole state lives under one lock rather than in a packed word
 * updated by CAS. A CAS can serialize the word but not the MEANING of a
 * sequence of samples: a CPU that loses the race can replay an older sample
 * after a newer one and corrupt the debounce, and the level plus counters can
 * cycle back to a previously loaded value so a stale CAS succeeds in a later
 * epoch of the state machine. Serializing the transition also lets the
 * transition record be produced while the transition is still owned, so a
 * transition can never be decided and then lost. */
typedef struct pressure_domain {
    spinlock_t lock;
    uint8_t    level;          /* quota_pressure_level_t                    */
    uint8_t    source_kind;    /* quota_pressure_source_t of the last sample */
    uint8_t    valid;          /* last sample had meaningful operands        */
    uint8_t    has_pending;    /* `pending` holds an unpublished transition  */
    uint16_t   last_permille;
    uint16_t   window_ms;
    uint16_t   rise_count;     /* consecutive samples above the rise band    */
    uint16_t   fall_count;     /* consecutive samples below the fall band    */
    uint16_t   invalid_run;    /* consecutive UNKNOWN samples at a live level */
    /* Set by any mutation that could have moved this domain's saturation.
     * A mutation does NOT sample: it only marks. The periodic sampler is the
     * single producer of samples, which is what keeps their ORDER meaningful
     * and keeps an unbounded registry walk off the charge path (which can run
     * in interrupt context). */
    uint8_t    dirty;
    /* Guaranteed-delivery FIFO for transitions the ring could not take.
     *
     * The ring is shared with failure events, and an event burst can fill it.
     * Dropping a transition there would lose the LEVEL -- the one piece of
     * state a recovery consumer cannot reconstruct. A FIFO rather than a single
     * coalescing slot because ORDER carries meaning here: collapsing an
     * into-critical record under a later out-of-critical one would erase the
     * escalation entirely, and a consumer replaying them out of order would act
     * on a level the system had already left. Overflow is counted, never
     * silent. */
    QUOTA_PRESSURE_RECORD pending[QUOTA_PRESSURE_PENDING_SLOTS];
    uint8_t    pending_head;
    uint8_t    pending_tail;
    uint8_t    pending_used;
} pressure_domain_t;

/* One domain per cache line. Domains are updated from different CPUs (any CPU
 * can charge any resource), so packing two domains' hot state into one line
 * would make every charge on resource A invalidate resource B's line for no
 * reason. The alignment also keeps a domain's hot fields off the same line as
 * its rarely-touched pending FIFO. */
#define QUOTA_PRESSURE_DOMAIN_ALIGN  64u

static pressure_domain_t g_domain[QUOTA_PRESSURE_DOMAIN_COUNT]
    __attribute__((aligned(QUOTA_PRESSURE_DOMAIN_ALIGN)));

/* Rise and fall thresholds indexed by the level being entered / left. Index 0
 * is unused (there is no threshold to enter NORMAL from below, and none to
 * leave it from above); it is present so the arrays index directly by level
 * with no offset arithmetic at every use. */
/* Unbounded for the same reason g_source_name is: with the bound written out, a
 * grown level enum zero-fills the new row and the coverage assert below compares
 * the declared bound with itself, so quota_pressure_level_name would return NULL
 * into a %s. Letting the initializer set the length makes the assert real. */
static const char *const g_level_name[] = {
    "normal", "watch", "warning", "critical"
};

/* An enum that grows must grow every array indexed by it. */
_Static_assert(sizeof(g_level_name) / sizeof(g_level_name[0]) == QUOTA_PRESSURE_LEVEL_COUNT,
               "the level name table must cover every pressure level");

/* ==========================================================================
 * Deferred-publish ring
 * ========================================================================== */

typedef struct pressure_ring_entry {
    uint32_t kind;                       /* QUOTA_PRESSURE_KIND_*           */
    union {
        QUOTA_PRESSURE_RECORD transition;
        QUOTA_FAILURE_RECORD  failure;
    } u;
} pressure_ring_entry_t;

static pressure_ring_entry_t g_ring[QUOTA_PRESSURE_RING_SLOTS];
static spinlock_t            g_ring_lock = SPINLOCK_INIT;
static uint32_t              g_ring_head;   /* next slot to write */
static uint32_t              g_ring_tail;   /* next slot to read  */
static uint32_t              g_ring_used;

/* ==========================================================================
 * Counters, sequences, publication state
 * ========================================================================== */

static uint64_t g_transition_seq;
static uint64_t g_event_seq;
static uint64_t g_nomination_seq;
static uint64_t g_transitions;
static uint64_t g_events_recorded;
static uint64_t g_drop_ratelimit;
static uint64_t g_drop_ring;
static uint64_t g_drop_transport;

/* Failure-event token buckets, ONE PER RESOURCE TYPE rather than one global.
 *
 * A single bucket lets one principal hammering its own configured cap consume
 * the whole allowance and silence diagnostics for every other resource in the
 * system -- a denial of observability, and precisely when observability matters
 * most. Per-resource buckets bound the blast radius to the resource actually
 * under attack. (Per-PRINCIPAL buckets would be better still, but the principal
 * set is unbounded and attacker-growable, which is the same reason the quota
 * registry lists one entry per SID rather than one per object.) */
typedef struct failure_bucket {
    uint32_t tokens;
    uint64_t last_refill_ns;
} failure_bucket_t;

static spinlock_t      g_bucket_lock = SPINLOCK_INIT;
static failure_bucket_t g_bucket[QUOTA_PRESSURE_DOMAIN_COUNT];
static int              g_buckets_seeded;

/* Publication. `g_ready` gates ONLY the transport: the state machine, the ring,
 * and every counter are live from the first charge, so a refusal during early
 * boot is still recorded and still visible to a test. */
static int        g_ready;
static KNF_STATE *g_state_pressure;
static KNF_STATE *g_state_failure;
static KNF_STATE *g_state_nomination;
static KDPC       g_drain_dpc;

/* Channels whose creation was refused for a TRANSIENT reason, one bit each,
 * indexed by the channel table below. Nonzero means the drain should try again
 * before it publishes.
 *
 * This bitmask is the whole of the retry mechanism, and it exists because the
 * obvious alternative does not work: a bounded loop around creation inside
 * quota_pressure_init cannot span the condition it is waiting on. The charge gate
 * that produces STATUS_RETRY stays shut for the WHOLE of a job-membership
 * transition (quota.h is explicit that immediate re-probing cannot outlast it),
 * so a spin there would normally exhaust while the gate is still closed, and then
 * -- because init is one-shot -- the channel would stay dead for the rest of the
 * boot over a race that lasted microseconds. Retrying from the drain instead puts
 * the next attempt at PASSIVE_LEVEL, arbitrarily later, as many times as it
 * takes, and costs nothing while the mask is clear. */
static uint32_t   g_channels_pending;

/* Retry SCHEDULE for the pending channels above. The policy itself (backoff,
 * cap, warn-once, reset) lives in quota_pressure.h as pure functions over this
 * struct precisely so it can be asserted without a clock or a create hook; this
 * module supplies the clock and the attempts. Written only from the retry pass
 * (serialized by g_drain_armed) or from init before any DPC can run, so it needs
 * no lock -- the trigger reads are relaxed and a stale one only delays or admits a
 * single attempt. */
static quota_pressure_retry_t g_channels_retry;

/* Read a published channel. Acquire, because the drain can now publish one
 * LONG AFTER init: a publisher on another CPU must not see the pointer before
 * the state it points at is fully constructed. */
static KNF_STATE *pressure_channel_get(KNF_STATE **slot)
{
    return (KNF_STATE *)__atomic_load_n(slot, __ATOMIC_ACQUIRE);
}

/* The CPU whose queues actually get serviced. Matches ktimer's service CPU:
 * today only the BSP runs the timer ISR and drains DPC queues. */
#define QUOTA_PRESSURE_SERVICE_CPU  0u

/* Periodic sampler: the single producer of pressure samples. */
static KDPC           g_sample_dpc;
static kernel_timer_t g_sample_timer;
static uint64_t       g_sample_period_ticks;

/* Exactly one CPU may hand this KDPC to KeInsertQueueDpc at a time: dpc.h
 * documents concurrent same-DPC inserts from two CPUs as caller misuse, and
 * every charge path in the kernel can reach this producer. The atomic exchange
 * is what makes that single-ownership true -- a plain read-then-set would let
 * two CPUs both observe 0. */
static uint32_t   g_drain_armed;

#ifdef KERNEL_TESTS
/* Publication hold. In a booted test kernel the threaded-DPC publisher is
 * already running, so it would race a test for the very records the test just
 * produced. While the hold is set the drain neither arms nor consumes, and the
 * test owns the ring outright. */
static uint32_t   g_test_hold;
#endif

/* --- LIVE channel-create seam --------------------------------------------
 * The create path was the one part of the retry machinery no test could reach.
 * The policy helpers in quota_pressure.h are pure and take `now_ns` as a
 * parameter, so backoff arithmetic was already provable -- but
 * pressure_create_channels calls the creator and reads the clock DIRECTLY, so the
 * end-to-end schedule (attempt -> defer -> back off -> warn once) was provable
 * only in pieces, and the wiring between the policy and the live path was not
 * covered at all.
 *
 * The seam is ONE immutable descriptor published as a SINGLE pointer, never two
 * independently-visible function-pointer globals: the drain runs on another CPU
 * and must never observe a half-installed pair. A test clock paired with the real
 * creator would make the LIVE channels retry against a fake deadline -- exactly
 * the kind of damage a test seam must be incapable of. Swapping one pointer with
 * release/acquire makes the pair atomic by construction.
 *
 * The creator is deliberately NARROW (name + out-parameter) rather than a mirror
 * of knf_create_state_ex's seven arguments: the fixed category, lifetime, scope
 * and access mode are properties of this subsystem, not of the seam, and a
 * mirrored signature would drift the moment knf grows a parameter.
 *
 * Production builds carry no seam: both wrappers compile to a direct call. */
typedef uint64_t (*pressure_clock_fn)(void);
typedef NTSTATUS (*pressure_create_fn)(const char *name, KNF_STATE **out);

static NTSTATUS pressure_create_live(const char *name, KNF_STATE **out)
{
    return knf_create_state_ex(QUOTA_PRESSURE_KNF_CATEGORY, name,
                               KNF_LIFETIME_PERMANENT, KNF_SCOPE_SYSTEM,
                               (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE, out);
}

#ifdef KERNEL_TESTS
typedef struct pressure_seam {
    pressure_clock_fn  clock;
    pressure_create_fn create;
} pressure_seam_t;

/* Both descriptors are `const` and statically initialized, so "install" is a
 * pointer swap between two immutable objects -- there is no window in which a
 * reader can see a descriptor being built. */
static const pressure_seam_t  g_seam_live = { uptime_ns, pressure_create_live };
static const pressure_seam_t *g_seam      = &g_seam_live;

static inline const pressure_seam_t *pressure_seam(void)
{
    return (const pressure_seam_t *)__atomic_load_n(&g_seam, __ATOMIC_ACQUIRE);
}

static inline uint64_t pressure_now_ns(void)
{
    return pressure_seam()->clock();
}

static inline NTSTATUS pressure_create_state(const char *name, KNF_STATE **out)
{
    return pressure_seam()->create(name, out);
}
#else
static inline uint64_t pressure_now_ns(void)
{
    return uptime_ns();
}

static inline NTSTATUS pressure_create_state(const char *name, KNF_STATE **out)
{
    return pressure_create_live(name, out);
}
#endif

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

static inline int domain_valid(quota_resource_type_t type)
{
    return (uint32_t)type < QUOTA_PRESSURE_DOMAIN_COUNT;
}

const char *quota_pressure_level_name(quota_pressure_level_t level)
{
    /* UNKNOWN is checked BEFORE the range test and named, not rendered as "?".
     * It is a legitimate answer meaning "nothing measured yet", and printing the
     * same glyph for it as for a corrupted value would put the honest case and
     * the broken case on the same line of a dashboard. */
    if (level == QUOTA_PRESSURE_UNKNOWN)
        return "unknown";
    if ((uint32_t)level >= QUOTA_PRESSURE_LEVEL_COUNT)
        return "?";
    return g_level_name[level];
}

/* Copy one record into the ring. Returns 1 when it was stored, 0 when the ring
 * was full (counted as a ring drop -- distinct from a rate-limit drop, because
 * the two mean different things to whoever is reading the gaps: one says the
 * producer was throttled on purpose, the other says the publisher fell
 * behind). Holds the ring lock across a struct copy and nothing else. */
static int ring_push(const pressure_ring_entry_t *entry)
{
    uint64_t flags;
    int stored = 0;

    spin_lock_irqsave(&g_ring_lock, &flags);
    if (g_ring_used < QUOTA_PRESSURE_RING_SLOTS) {
        g_ring[g_ring_head] = *entry;
        g_ring_head = (g_ring_head + 1u) % QUOTA_PRESSURE_RING_SLOTS;
        g_ring_used++;
        stored = 1;
    }
    spin_unlock_irqrestore(&g_ring_lock, flags);

    if (!stored)
        __atomic_fetch_add(&g_drop_ring, 1ull, __ATOMIC_RELAXED);
    return stored;
}

static int ring_pop(pressure_ring_entry_t *out)
{
    uint64_t flags;
    int got = 0;

    spin_lock_irqsave(&g_ring_lock, &flags);
    if (g_ring_used > 0) {
        *out = g_ring[g_ring_tail];
        g_ring_tail = (g_ring_tail + 1u) % QUOTA_PRESSURE_RING_SLOTS;
        g_ring_used--;
        got = 1;
    }
    spin_unlock_irqrestore(&g_ring_lock, flags);
    return got;
}

static uint32_t ring_used(void)
{
    uint64_t flags;
    uint32_t n;

    spin_lock_irqsave(&g_ring_lock, &flags);
    n = g_ring_used;
    spin_unlock_irqrestore(&g_ring_lock, flags);
    return n;
}

/* Arm the drain. Safe from any IRQL the producers run at: KeInsertQueueDpc
 * neither blocks nor allocates. Before init there is no worker to drain the
 * ring, so arming is skipped and the records simply wait (or age out as ring
 * drops, which is the honest accounting for "recorded before anything could
 * publish"). */
static void drain_arm(void)
{
#ifdef KERNEL_TESTS
    if (__atomic_load_n(&g_test_hold, __ATOMIC_ACQUIRE))
        return;
#endif
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
        return;
    /* Checked on every arm rather than once at init: a worker that was not up
     * when the subsystem initialized may be up later, and a transient startup
     * failure must not disable publication for the rest of the boot. Records
     * accumulate in the ring meanwhile and their loss is counted. */
    if (!dpc_worker_started())
        return;
    if (__atomic_exchange_n(&g_drain_armed, 1u, __ATOMIC_ACQ_REL) == 0u)
        /* PINNED to the service CPU, never DPC_TARGET_CURRENT. A threaded DPC
         * still lands on the queueing CPU's normal queue until that CPU drains
         * it, and an idle AP parks in sti/hlt without self-draining (there is no
         * DPC IPI yet). Arming from an AP -- which the charge path can do, since
         * it is documented callable from interrupt context -- would strand the
         * callback there, and because only the callback clears g_drain_armed the
         * exchange would stay wedged at 1 and publication would be dead for the
         * rest of the boot. ktimer pins its DPCs for exactly this reason. */
        KeInsertQueueDpcOnCpu(&g_drain_dpc, QUOTA_PRESSURE_SERVICE_CPU,
                              (void *)0, (void *)0, (int *)0);
}

/* ==========================================================================
 * Publication (PASSIVE_LEVEL, no lock of ours held)
 * ========================================================================== */

/* Forward declarations: the publish helpers below reference each other (a
 * latched transition is published and then escalated, and escalation itself
 * publishes a nomination). */
static void publish_transition(const QUOTA_PRESSURE_RECORD *rec);
static void escalate_if_critical(const QUOTA_PRESSURE_RECORD *rec);
static uint32_t drain_pending_transitions(uint32_t budget);
static int  pending_transitions_exist(void);

static void publish_transition(const QUOTA_PRESSURE_RECORD *rec)
{
    /* ETW severity: 0 = critical .. 5 = verbose. A move INTO warning or
     * critical is worth a session's attention; everything else is progress
     * reporting. */
    uint8_t etw_level = (rec->to_level >= (uint8_t)QUOTA_PRESSURE_WARNING) ? 2u : 4u;

    klog((rec->to_level >= (uint8_t)QUOTA_PRESSURE_WARNING) ? LOG_WARN : LOG_INFO,
         "quota",
         "pressure %s: %s -> %s (%u permille, source %u, seq %llu)",
         quota_resource_type_name((quota_resource_type_t)rec->resource),
         quota_pressure_level_name((quota_pressure_level_t)rec->from_level),
         quota_pressure_level_name((quota_pressure_level_t)rec->to_level),
         (uint32_t)rec->sample_permille, (uint32_t)rec->source_kind,
         (uint64_t)rec->transition_seq);

    etw_emit_kernel_event(ETW_EVT_QUOTA_PRESSURE, etw_level, rec,
                          (uint32_t)sizeof(*rec));

    KNF_STATE *state = pressure_channel_get(&g_state_pressure);

    if (state) {
        NTSTATUS st = knf_publish(state, (const KNF_TYPE_ID *)0,
                                  rec, (uint32_t)sizeof(*rec),
                                  (const uint64_t *)0, (uint64_t *)0, (uint64_t *)0);
        if (st != STATUS_SUCCESS)
            __atomic_fetch_add(&g_drop_transport, 1ull, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&g_drop_transport, 1ull, __ATOMIC_RELAXED);
    }
}

static void publish_failure(const QUOTA_FAILURE_RECORD *rec)
{
    klog(LOG_WARN, "quota",
         "refused %s: want %llu, at %llu of %llu (pid %u, status 0x%x, seq %llu)",
         quota_resource_type_name((quota_resource_type_t)rec->resource),
         (uint64_t)rec->requested, (uint64_t)rec->current, (uint64_t)rec->limit,
         (uint32_t)rec->pid, (uint32_t)rec->result, (uint64_t)rec->event_seq);

    etw_emit_kernel_event(ETW_EVT_QUOTA_FAILURE, 2u, rec, (uint32_t)sizeof(*rec));

    KNF_STATE *state = pressure_channel_get(&g_state_failure);

    if (state) {
        NTSTATUS st = knf_publish(state, (const KNF_TYPE_ID *)0,
                                  rec, (uint32_t)sizeof(*rec),
                                  (const uint64_t *)0, (uint64_t *)0, (uint64_t *)0);
        if (st != STATUS_SUCCESS)
            __atomic_fetch_add(&g_drop_transport, 1ull, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&g_drop_transport, 1ull, __ATOMIC_RELAXED);
    }
}

static void publish_nomination(const QUOTA_NOMINATION_RECORD *rec)
{
    klog(LOG_WARN, "quota",
         "escalation %s: nominee block %llu rid %u at %u permille (seq %llu, valid %u)",
         quota_resource_type_name((quota_resource_type_t)rec->resource),
         (uint64_t)rec->block_id, (uint32_t)rec->owner_rid,
         (uint32_t)rec->over_permille, (uint64_t)rec->nomination_seq,
         (uint32_t)rec->valid);

    etw_emit_kernel_event(ETW_EVT_QUOTA_NOMINATION, 2u, rec, (uint32_t)sizeof(*rec));

    KNF_STATE *state = pressure_channel_get(&g_state_nomination);

    if (state) {
        NTSTATUS st = knf_publish(state, (const KNF_TYPE_ID *)0,
                                  rec, (uint32_t)sizeof(*rec),
                                  (const uint64_t *)0, (uint64_t *)0, (uint64_t *)0);
        if (st != STATUS_SUCCESS)
            __atomic_fetch_add(&g_drop_transport, 1ull, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&g_drop_transport, 1ull, __ATOMIC_RELAXED);
    }
}

/* Publish the transitions the ring could not take, OLDEST FIRST and BEFORE any
 * ring record, so a consumer never sees a later level ahead of an earlier one.
 * Returns the number published, so the caller can charge them against the same
 * drain budget the ring records use. */
static uint32_t drain_pending_transitions(uint32_t budget)
{
    uint32_t published = 0;

    for (uint32_t i = 0; i < QUOTA_PRESSURE_DOMAIN_COUNT && published < budget; i++) {
        pressure_domain_t    *d = &g_domain[i];
        QUOTA_PRESSURE_RECORD rec;
        uint64_t              flags;
        int                   have = 0;

        spin_lock_irqsave(&d->lock, &flags);
        if (d->pending_used > 0) {
            rec = d->pending[d->pending_tail];
            d->pending_tail = (uint8_t)((d->pending_tail + 1u) %
                                        QUOTA_PRESSURE_PENDING_SLOTS);
            d->pending_used--;
            have = 1;
        }
        spin_unlock_irqrestore(&d->lock, flags);

        if (have) {
            publish_transition(&rec);
            escalate_if_critical(&rec);
            published++;
            i--;    /* same domain may hold more, still in order */
        }
    }
    return published;
}

static int pending_transitions_exist(void)
{
    for (uint32_t i = 0; i < QUOTA_PRESSURE_DOMAIN_COUNT; i++) {
        pressure_domain_t *d = &g_domain[i];
        uint64_t flags;
        uint8_t  have;

        spin_lock_irqsave(&d->lock, &flags);
        have = (d->pending_used > 0) ? 1u : 0u;
        spin_unlock_irqrestore(&d->lock, flags);
        if (have)
            return 1;
    }
    return 0;
}

/* A transition INTO critical is what escalation means. Deriving and publishing
 * the nomination here -- in the drain, at PASSIVE_LEVEL, with no lock held --
 * is what makes the nomination a live part of the pipeline rather than a query
 * nobody calls. The kernel still only NAMES the principal: cooperative-only.
 *
 * A move DOWN out of critical publishes an explicit clear, so a consumer is
 * told the condition passed instead of waiting out the expiry. */
static void escalate_if_critical(const QUOTA_PRESSURE_RECORD *rec)
{
    QUOTA_NOMINATION_RECORD nom;

    if (rec->to_level == (uint8_t)QUOTA_PRESSURE_CRITICAL) {
        (void)quota_pressure_nominate((quota_resource_type_t)rec->resource, &nom);
        publish_nomination(&nom);
    } else if (rec->from_level == (uint8_t)QUOTA_PRESSURE_CRITICAL) {
        memset(&nom, 0, sizeof(nom));
        nom.layout_version  = QUOTA_NOMINATION_RECORD_VERSION;
        nom.resource        = rec->resource;
        nom.principal       = (uint8_t)QUOTA_PRINCIPAL_USER;
        nom.nomination_seq  = __atomic_add_fetch(&g_nomination_seq, 1ull, __ATOMIC_RELAXED);
        nom.nominated_at_ns = uptime_ns();
        nom.expires_at_ns   = nom.nominated_at_ns;
        nom.valid           = 0;
        publish_nomination(&nom);
    }
}

/* Threaded DPC routine: PASSIVE_LEVEL, so klog's disk path and knf_publish's
 * allocation are both legal here.
 *
 * The disarm-then-drain-then-recheck shape closes the lost-wakeup window: a
 * producer that pushes between the last pop and the disarm would otherwise find
 * the DPC still armed, skip the insert, and leave its record unpublished until
 * the next unrelated event. Clearing the flag FIRST means such a producer
 * either arms us again (its exchange sees 0) or we see its record in the
 * recheck below. */
/* The three channels this subsystem publishes on, described as DATA so creation
 * is one loop rather than three open-coded copies -- which is also what makes a
 * uniform retry possible at all. */
typedef struct pressure_channel {
    KNF_STATE  **slot;           /* where the created state is published       */
    const char  *name;           /* leaf under QUOTA_PRESSURE_KNF_CATEGORY     */
    uint32_t     payload_bytes;  /* retention reservation, pre-sized once      */
    const char  *degraded;       /* what is lost while this channel is absent  */
} pressure_channel_t;

static const pressure_channel_t g_channel[] = {
    { &g_state_pressure,   QUOTA_PRESSURE_KNF_STATE,
      (uint32_t)sizeof(QUOTA_PRESSURE_RECORD),   "transitions log only" },
    { &g_state_failure,    QUOTA_FAILURE_KNF_STATE,
      (uint32_t)sizeof(QUOTA_FAILURE_RECORD),    "refusals log only" },
    { &g_state_nomination, QUOTA_NOMINATION_KNF_STATE,
      (uint32_t)sizeof(QUOTA_NOMINATION_RECORD), "escalation logs only" },
};

#define QUOTA_PRESSURE_CHANNEL_COUNT \
    (sizeof(g_channel) / sizeof(g_channel[0]))

/* One pending bit per channel must fit the mask. */
_Static_assert(QUOTA_PRESSURE_CHANNEL_COUNT <= 32,
    "g_channels_pending carries one bit per channel");

/* --- Retry policy (pure; contract in quota_pressure.h) -------------------- */

int quota_pressure_retry_status_retryable(NTSTATUS status)
{
    return status == STATUS_RETRY || status == STATUS_INSUFFICIENT_RESOURCES;
}

int quota_pressure_retry_due(const quota_pressure_retry_t *st, uint32_t pending,
                             uint64_t now_ns)
{
    if (!st || pending == 0)
        return 0;
    /* RELAXED ATOMIC, not a plain read. The deadline is written by the retry pass
     * on whichever CPU ran the threaded drain and read here from the periodic DPC
     * on the service CPU, so a plain dereference is a data race -- g_drain_armed
     * serializes the WRITERS, it does not publish the write to a reader. Relaxed is
     * sufficient: the value is self-contained and guards nothing else, so a reader
     * that sees the previous deadline merely admits or delays one attempt. */
    return now_ns >= __atomic_load_n(&st->at_ns, __ATOMIC_RELAXED);
}

void quota_pressure_retry_advance(quota_pressure_retry_t *st, uint32_t pending,
                                  uint64_t now_ns, int *out_warn)
{
    if (out_warn)
        *out_warn = 0;
    if (!st)
        return;

    if (pending == 0) {
        /* Everything published: the schedule is over. Cleared rather than left
         * behind so a later reader is not told about attempts that no longer bound
         * anything, and so a future deferral starts from the short interval
         * instead of inheriting a 30-second gap it never earned. */
        __atomic_store_n(&st->at_ns, 0ull, __ATOMIC_RELAXED);
        st->gap_ns = 0;
        st->tries  = 0;
        st->warned = 0;
        return;
    }

    /* Double toward the cap. The FIRST failure has no previous interval to double,
     * so it takes the floor; every one after that doubles what it inherited and
     * saturates at the ceiling rather than growing without bound. */
    st->gap_ns = st->gap_ns ? (st->gap_ns * 2ull) : QUOTA_PRESSURE_RETRY_FIRST_NS;
    if (st->gap_ns > QUOTA_PRESSURE_RETRY_MAX_NS)
        st->gap_ns = QUOTA_PRESSURE_RETRY_MAX_NS;
    /* Paired with the relaxed load in quota_pressure_retry_due: only this field
     * crosses CPUs, so only this one needs publishing. */
    __atomic_store_n(&st->at_ns, now_ns + st->gap_ns, __ATOMIC_RELAXED);

    /* Count the try, and report ONCE when it stops looking like a passing race.
     * `tries` keeps counting past the threshold -- it is evidence for the log line,
     * not a budget -- and `warned` is what makes the message single. */
    st->tries++;
    if (st->tries >= QUOTA_PRESSURE_RETRY_WARN_TRIES && !st->warned) {
        st->warned = 1;
        if (out_warn)
            *out_warn = 1;
    }
}

/* Attempt the channels named by `attempt` (a bitmask), and return the new pending
 * mask. `attempt` is what keeps a retry from being a rescan: the first call, from
 * init, passes every channel; a retry passes only the bits that were left pending.
 *
 * WITHOUT that restriction a retry re-attempts channels that failed
 * PERMANENTLY -- a name collision, a missing category -- because their slot is
 * also empty. Every drain would then re-allocate an object, re-charge and
 * re-return the creating task's quota, and re-log an error for a failure whose
 * answer cannot change. One channel deferred for a real reason would turn every
 * other channel's permanent failure into a repeating cost.
 *
 * MUST run at PASSIVE_LEVEL: creation allocates an object and knf_reserve_payload
 * allocates a retention buffer, neither of which is legal above PASSIVE. Both
 * callers satisfy that (init is pre-scheduler; the drain is a THREADED DPC), and
 * the guard is cheap enough to keep -- a future non-threaded caller would
 * otherwise allocate at DISPATCH_LEVEL. */
static uint32_t pressure_create_channels(uint32_t attempt)
{
    uint32_t pending = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        /* Leave the mask exactly as it was: this attempt did not happen, so it
         * must neither clear a pending bit nor invent one. */
        return __atomic_load_n(&g_channels_pending, __ATOMIC_ACQUIRE);
    }

    for (uint32_t i = 0; i < QUOTA_PRESSURE_CHANNEL_COUNT; i++) {
        const pressure_channel_t *ch = &g_channel[i];
        KNF_STATE *state = pressure_channel_get(ch->slot);
        NTSTATUS   st;

        if ((attempt & (1u << i)) == 0) {
            /* Not ours to attempt. A channel excluded because it failed
             * permanently must NOT be re-marked pending either -- that would
             * resurrect it into the next retry and rebuild the rescan this
             * parameter exists to prevent. */
            continue;
        }
        if (state)
            continue;                       /* already published */

        st = pressure_create_state(ch->name, &state);
        if (st != STATUS_SUCCESS || !state) {
            if (quota_pressure_retry_status_retryable(st)) {
                /* TRANSIENT: say so, and say that it will be retried, so a log
                 * reader does not conclude the channel is dead.
                 *
                 * ONCE. The backoff bounds how often the retry RUNS; without
                 * this it would not bound how often the retry TALKS, and a
                 * channel stuck on resource exhaustion -- precisely the
                 * condition this subsystem exists to report -- would emit a
                 * warning per channel every 30 s for the life of the system,
                 * drowning the pressure reporting it is meant to enable. The
                 * threshold ERROR below still fires later, also once. */
                pending |= (1u << i);
                if (g_channels_retry.tries == 0)
                    klog(LOG_WARN, "quota",
                         "pressure: '%s' creation deferred (0x%x); retrying with backoff",
                         ch->name, (uint64_t)st);
            } else {
                klog(LOG_ERROR, "quota",
                     "pressure: '%s' creation failed (0x%x); %s",
                     ch->name, (uint64_t)st, ch->degraded);
            }
            continue;
        }

        /* Pre-size the retention buffer BEFORE publishing the pointer. A
         * publisher that saw the state first would find the buffer unsized and
         * have to grow it, which knf_publish refuses above PASSIVE_LEVEL -- so
         * the reservation is part of construction, not a follow-up step. A
         * failure here is not fatal: the channel works, its publishes may drop. */
        if (knf_reserve_payload(state, ch->payload_bytes) != STATUS_SUCCESS)
            klog(LOG_WARN, "quota",
                 "pressure: could not pre-size '%s'; publishes may drop", ch->name);

        __atomic_store_n(ch->slot, state, __ATOMIC_RELEASE);
    }

    /* Fold this attempt into the schedule, once per pass, so every trigger
     * inherits the same bounded cadence rather than inventing its own. */
    {
        int warn = 0;

        quota_pressure_retry_advance(&g_channels_retry, pending, pressure_now_ns(), &warn);
        if (warn)
            klog(LOG_ERROR, "quota",
                 "pressure: channel mask 0x%x still uncreated after %u attempts; "
                 "retrying every %llu ms (those notifications log only meanwhile)",
                 (uint64_t)pending, (uint64_t)g_channels_retry.tries,
                 QUOTA_PRESSURE_RETRY_MAX_NS / (1000ull * 1000ull));
    }

    __atomic_store_n(&g_channels_pending, pending, __ATOMIC_RELEASE);
    return pending;
}

/* Is a retry DUE? Asks the shared policy, so both triggers below use one answer
 * and neither can turn a pending channel into a busy loop: the drain re-arms
 * itself for queued records, and without this gate every re-arm would carry
 * another creation attempt with it. */
static int pressure_retry_due(void)
{
    return quota_pressure_retry_due(&g_channels_retry,
                                    __atomic_load_n(&g_channels_pending,
                                                    __ATOMIC_ACQUIRE),
                                    pressure_now_ns());
}

static void quota_pressure_drain(KDPC *dpc, void *context, void *arg1, void *arg2)
{
    pressure_ring_entry_t entry;
    uint32_t published;

    (void)dpc; (void)context; (void)arg1; (void)arg2;

#ifdef KERNEL_TESTS
    /* A drain queued before the hold was taken can still be dispatched after
     * it. Checking here as well as in drain_arm is what makes the hold actually
     * hold: otherwise an in-flight callback would consume the records the test
     * is about to inspect.
     *
     * FIRST, ahead of the channel-retry block below. The hold used to be checked
     * after it, which meant a held drain still ran the LIVE create path -- so a
     * test that installed a clock or creator seam could have its state consumed,
     * and its counted creator calls inflated, by the very callback the hold was
     * supposed to have stopped. A hold now means the drain does nothing at all. */
    if (__atomic_load_n(&g_test_hold, __ATOMIC_ACQUIRE)) {
        /* Disarm on the way out. Returning with the flag still set would mean
         * no producer could ever re-arm (drain_arm's exchange would always see
         * 1) and publication would stay dead for the rest of the boot once the
         * hold is released. */
        __atomic_store_n(&g_drain_armed, 0u, __ATOMIC_RELEASE);
        return;
    }
#endif

    /* Retry any channel whose creation was DEFERRED, BEFORE publishing: this is
     * the only context that recurs, runs at PASSIVE_LEVEL, and can afford to
     * wait. Only the pending bits are attempted, so a permanently-failed channel
     * is not re-attempted here. Costs one relaxed load when nothing is pending,
     * which is the normal case for the whole life of the system. */
    if (pressure_retry_due()) {
        uint32_t pending = __atomic_load_n(&g_channels_pending, __ATOMIC_ACQUIRE);

        if (pending != 0)
            (void)pressure_create_channels(pending);
    }

    __atomic_store_n(&g_drain_armed, 0u, __ATOMIC_RELEASE);

    /* Ring FIRST, then the overflow FIFO. A record only reaches the FIFO when
     * the ring was full, so everything already in the ring at that moment is
     * OLDER than it -- draining the FIFO first would hand a consumer a newer
     * level ahead of an older one.
     *
     * That ordering is not a guarantee across the two paths in general: once a
     * drain frees ring space, a later transition can enter the ring while an
     * older one still sits in a FIFO. This is why every record carries
     * transition_seq and why the contract asks consumers to order and
     * de-duplicate by it rather than by arrival. Recording is exact; ARRIVAL
     * order is best-effort, like delivery. */
    /* Reserve a slice of the budget for the overflow FIFO FIRST. A ring kept
     * permanently non-empty by failure-event traffic would otherwise consume
     * the whole budget every invocation and the FIFO -- which holds LEVEL
     * records, the ones a consumer cannot reconstruct -- would never drain at
     * all. Liveness for those outranks arrival order, which the contract
     * already declares best-effort. */
    published = drain_pending_transitions(QUOTA_PRESSURE_PENDING_RESERVE);

    while (published < QUOTA_PRESSURE_DRAIN_BUDGET && ring_pop(&entry)) {
        if (entry.kind == QUOTA_PRESSURE_KIND_TRANSITION) {
            publish_transition(&entry.u.transition);
            escalate_if_critical(&entry.u.transition);
        } else if (entry.kind == QUOTA_PRESSURE_KIND_FAILURE) {
            publish_failure(&entry.u.failure);
        }
        published++;
    }

    published += drain_pending_transitions(
        (published < QUOTA_PRESSURE_DRAIN_BUDGET)
            ? (QUOTA_PRESSURE_DRAIN_BUDGET - published) : 0u);

    /* BOUNDED, then re-arm and return. The threaded-DPC worker is a single
     * system-wide thread, so a callback that drained until empty would starve
     * every other threaded DPC for as long as quota traffic kept arriving --
     * and under exactly the resource pressure this subsystem exists to report.
     * Re-arming leaves the remaining work queued behind the other callbacks
     * rather than in front of them. */
    if (ring_used() > 0u || pending_transitions_exist())
        drain_arm();
}

/* ==========================================================================
 * Sampling
 * ========================================================================== */

/* Run one sample through `d`'s hysteresis. Caller holds the domain lock.
 * Returns 1 and fills `rec` when the sample completed a debounce and moved the
 * level; 0 otherwise.
 *
 * A sample moves the level by at most ONE step. A resource that jumps straight
 * from idle to saturated therefore walks up through watch and warning rather
 * than teleporting to critical, which is what makes each intermediate level
 * observable to a consumer that wanted to act early. */
static int domain_step_locked(pressure_domain_t *d, quota_resource_type_t type,
                              uint16_t permille, uint16_t window_ms,
                              quota_pressure_source_t source, uint64_t now_ns,
                              QUOTA_PRESSURE_RECORD *rec)
{
    uint8_t level = d->level;
    uint8_t next  = level;

    d->source_kind   = (uint8_t)source;
    d->valid         = 1;
    d->invalid_run   = 0;
    d->last_permille = permille;
    d->window_ms     = window_ms;

    if (level < (uint8_t)QUOTA_PRESSURE_CRITICAL &&
        permille >= quota_pressure_rise_threshold((uint32_t)level + 1u)) {
        d->fall_count = 0;
        if (++d->rise_count >= (uint16_t)QUOTA_PRESSURE_RISE_SAMPLES) {
            d->rise_count = 0;
            next = (uint8_t)(level + 1);
        }
    } else if (level > (uint8_t)QUOTA_PRESSURE_NORMAL &&
               permille < quota_pressure_fall_threshold((uint32_t)level)) {
        d->rise_count = 0;
        if (++d->fall_count >= (uint16_t)QUOTA_PRESSURE_FALL_SAMPLES) {
            d->fall_count = 0;
            next = (uint8_t)(level - 1);
        }
    } else {
        /* Inside the band for the current level: neither direction is being
         * sustained, so both counters reset. Letting them persist would let
         * unrelated excursions minutes apart add up to a transition. */
        d->rise_count = 0;
        d->fall_count = 0;
        /* Cleared with the rest: a leftover run from an earlier test can push the
         * NEXT test's first UNKNOWN sample past the reset threshold and fabricate
         * a transition record it never provoked. */
        d->invalid_run = 0;
    }

    if (next == level)
        return 0;

    d->level = next;

    memset(rec, 0, sizeof(*rec));
    rec->layout_version  = QUOTA_PRESSURE_RECORD_VERSION;
    rec->resource        = (uint32_t)type;
    rec->from_level      = level;
    rec->to_level        = next;
    rec->source_kind     = (uint8_t)source;
    rec->source_valid    = 1;
    rec->sample_permille = permille;
    rec->window_ms       = window_ms;
    rec->transition_seq  = __atomic_add_fetch(&g_transition_seq, 1ull, __ATOMIC_RELAXED);
    rec->timestamp_ns    = now_ns;
    return 1;
}

/* Common tail for both sampling seams. */
static void pressure_sample(quota_resource_type_t type, uint16_t permille,
                            uint16_t window_ms, quota_pressure_source_t source)
{
    pressure_domain_t    *d;
    QUOTA_PRESSURE_RECORD rec;
    pressure_ring_entry_t entry;
    uint64_t              flags;
    uint64_t              now_ns;
    int                   moved;
    int                   reset_moved = 0;

    if (!domain_valid(type))
        return;

    if (permille == QUOTA_PRESSURE_INVALID_PERMILLE) {
        /* No meaningful ratio (an unlimited cap, or an unimplemented seam).
         * The domain is marked UNKNOWN and the debounce counters are left
         * exactly as they were: an unknown must not de-escalate a level, and
         * must not count toward escalating one either. */
        d = &g_domain[type];
        spin_lock_irqsave(&d->lock, &flags);
        d->valid       = 0;
        d->source_kind = (uint8_t)source;
        /* An unknown must not de-escalate a level -- but it must not LATCH one
         * either. A capped principal that logs off or has its cap removed makes
         * this domain permanently unmeasurable, and without an exit the level
         * would stay at its peak for the rest of the boot (and keep the sampler
         * walking the registry every tick for a domain nobody can measure).
         * After a bounded run of consecutive unknowns the domain resets to
         * normal-and-unknown: the condition it was reporting is not merely
         * unobserved, its subject is gone. */
        if (d->level != (uint8_t)QUOTA_PRESSURE_NORMAL &&
            ++d->invalid_run >= QUOTA_PRESSURE_INVALID_RESET_SAMPLES) {
            uint8_t from = d->level;
            d->level      = (uint8_t)QUOTA_PRESSURE_NORMAL;
            d->rise_count = 0;
            d->fall_count = 0;
            d->invalid_run = 0;

            /* RECORD it like any other transition. Resetting silently would
             * leave a consumer that acted on the old level -- possibly a
             * critical nomination -- with no event telling it the condition
             * ended, which is the same stale-state hazard the explicit clear
             * exists to prevent. source_valid is 0: this move was driven by the
             * ABSENCE of a measurement, and the record says so. */
            memset(&rec, 0, sizeof(rec));
            rec.layout_version  = QUOTA_PRESSURE_RECORD_VERSION;
            rec.resource        = (uint32_t)type;
            rec.from_level      = from;
            rec.to_level        = (uint8_t)QUOTA_PRESSURE_NORMAL;
            rec.source_kind     = (uint8_t)source;
            rec.source_valid    = 0;
            rec.sample_permille = 0;
            rec.window_ms       = window_ms;
            rec.transition_seq  = __atomic_add_fetch(&g_transition_seq, 1ull,
                                                     __ATOMIC_RELAXED);
            rec.timestamp_ns    = uptime_ns();
            reset_moved = 1;

            entry.kind         = QUOTA_PRESSURE_KIND_TRANSITION;
            entry.u.transition = rec;
            if (!ring_push(&entry) &&
                d->pending_used < QUOTA_PRESSURE_PENDING_SLOTS) {
                d->pending[d->pending_head] = rec;
                d->pending_head = (uint8_t)((d->pending_head + 1u) %
                                            QUOTA_PRESSURE_PENDING_SLOTS);
                d->pending_used++;
            }
            __atomic_fetch_add(&g_transitions, 1ull, __ATOMIC_RELAXED);
        }
        spin_unlock_irqrestore(&d->lock, flags);
        if (reset_moved)
            drain_arm();
        return;
    }

    if (permille > (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX)
        permille = (uint16_t)QUOTA_PRESSURE_PERMILLE_MAX;

    /* Stamped BEFORE the lock: the timer read is not needed under the domain
     * lock, and keeping it outside holds the lock to arithmetic plus one
     * struct copy. */
    now_ns = uptime_ns();

    d = &g_domain[type];
    spin_lock_irqsave(&d->lock, &flags);
    moved = domain_step_locked(d, type, permille, window_ms, source, now_ns, &rec);
    if (moved) {
        /* Recorded while the transition is still owned, so a CPU that stalls
         * after deciding cannot lose it. The ring lock is a leaf under the
         * domain lock; nothing is called while either is held. */
        pressure_ring_entry_t entry;
        entry.kind         = QUOTA_PRESSURE_KIND_TRANSITION;
        entry.u.transition = rec;
        if (!ring_push(&entry)) {
            /* The ring is full -- almost certainly of failure events, which
             * share it. A transition must NOT be dropped here: the level is the
             * one fact a recovery consumer cannot reconstruct from anything
             * else. Queue it on the domain's own FIFO instead, preserving
             * order, and let the next drain publish it ahead of anything the
             * ring took later. */
            if (d->pending_used < QUOTA_PRESSURE_PENDING_SLOTS) {
                d->pending[d->pending_head] = rec;
                d->pending_head = (uint8_t)((d->pending_head + 1u) %
                                            QUOTA_PRESSURE_PENDING_SLOTS);
                d->pending_used++;
            }
            /* No second increment here when the FIFO is also full: ring_push
             * already counted this record's loss, and counting it twice would
             * make the drop total disagree with the sequence gaps a consumer
             * can actually observe. */
        }
        __atomic_fetch_add(&g_transitions, 1ull, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&d->lock, flags);

    /* Arming happens OUTSIDE the domain lock: KeInsertQueueDpc takes the DPC
     * queue's own lock, and pulling that under a quota-side lock would invent
     * an ordering between two subsystems that nothing else establishes. */
    if (moved)
        drain_arm();
}

void quota_pressure_note_activity(quota_resource_type_t type)
{
    if (!domain_valid(type))
        return;
    /* A MARK, nothing more: no lock, no clock, no walk. Read first and store
     * only when it would change the value -- an unconditional store would take
     * the line exclusive on EVERY quota mutation from EVERY CPU, and the common
     * case under load is that the domain is already marked. */
    if (__atomic_load_n(&g_domain[type].dirty, __ATOMIC_RELAXED) == 0u)
        __atomic_store_n(&g_domain[type].dirty, 1u, __ATOMIC_RELAXED);
}

void quota_pressure_sample_all(void)
{
    uint16_t worst[QUOTA_PRESSURE_DOMAIN_COUNT];
    uint8_t  needed[QUOTA_PRESSURE_DOMAIN_COUNT];
    uint32_t needed_count = 0;

    /* PASS 1 -- consume every mark BEFORE looking at the registry.
     *
     * Order matters: if the walk ran first, a mutation landing between the walk
     * and a domain's mark-clear would have its mark consumed while the sample
     * applied to it predates the mutation. For an otherwise-idle domain that is
     * a permanently lost observation. Clearing first inverts the race into a
     * harmless one -- a mark arriving after its exchange simply survives to the
     * next tick and costs one redundant sample. */
    for (uint32_t i = 0; i < QUOTA_PRESSURE_DOMAIN_COUNT; i++) {
        pressure_domain_t *d = &g_domain[i];
        uint64_t flags;
        uint8_t  was_dirty = __atomic_exchange_n(&d->dirty, 0u, __ATOMIC_ACQ_REL);

        spin_lock_irqsave(&d->lock, &flags);
        /* Sample when something moved, when the level is not resting, or when a
         * debounce is part-way through. The last clause is what lets STABLE
         * pressure settle: a principal that charges to its cap once and then
         * goes quiet marks the domain exactly once, and the rise debounce needs
         * two more samples after that to complete. Without it the level would
         * never move for the most ordinary case there is. */
        needed[i] = (was_dirty || d->level != (uint8_t)QUOTA_PRESSURE_NORMAL ||
                     d->rise_count != 0 || d->fall_count != 0) ? 1u : 0u;
        spin_unlock_irqrestore(&d->lock, flags);

        if (needed[i])
            needed_count++;
    }

    /* A fully idle system does no walk at all -- not a cheap walk, none. */
    if (needed_count == 0)
        return;

    /* PASS 2 -- ONE registry walk answering every needed domain at once, and
     * reading counters ONLY for those. The walk holds the registry lock with
     * interrupts disabled, so both the number of walks and the per-block work
     * inside one walk are kept to what is actually being measured. */
    quota_registry_worst_all(worst, QUOTA_PRESSURE_DOMAIN_COUNT, needed);

    /* PASS 3 -- step each needed domain. */
    for (uint32_t i = 0; i < QUOTA_PRESSURE_DOMAIN_COUNT; i++) {
        if (needed[i])
            pressure_sample((quota_resource_type_t)i, worst[i],
                            (uint16_t)QUOTA_PRESSURE_SAMPLE_WINDOW_MS,
                            QUOTA_PRESSURE_SRC_BUDGET);
    }
}

/* Periodic sampler DPC. Runs at DISPATCH_LEVEL on the timer service CPU, which
 * is where the registry walk belongs: bounded, off the charge path, and single
 * threaded so sample order is total. */
static void quota_pressure_tick(KDPC *dpc, void *context, void *arg1, void *arg2)
{
    (void)dpc; (void)context; (void)arg1; (void)arg2;

#ifdef KERNEL_TESTS
    /* The periodic sampler is live in a booted test kernel, so without this it
     * is a SECOND producer interposing derived samples into whatever domain a
     * test is driving by hand -- flipping a level or a validity flag
     * mid-assertion. Held here rather than inside quota_pressure_sample_all so
     * a test that drives the sampler DELIBERATELY still works; only the
     * unsolicited timer-driven pass is suppressed. */
    if (__atomic_load_n(&g_test_hold, __ATOMIC_ACQUIRE))
        return;
#endif

    quota_pressure_sample_all();

    /* The stall lane rides this timer rather than owning one: it aggregates
     * every two seconds and this tick is every 50 ms, so the call is a cheap
     * deadline check forty times out of forty-one. A second ktimer for a 2 s
     * period would cost a timer slot and another DPC to save nothing. */
    quota_stall_aggregate();

    /* THE INDEPENDENT RETRY TRIGGER for a deferred notification channel. The
     * drain is where the retry actually happens (it is the threaded, PASSIVE
     * context that may allocate), but the drain only re-arms for QUEUED RECORDS
     * -- so on an idle system a channel deferred during init would never be
     * retried at all, and a transient race at boot would cost the channel for
     * the life of the system. This tick recurs unconditionally, so arming from
     * here gives the retry a schedule of its own.
     *
     * Arming, not creating: this callback is a plain DPC at DISPATCH_LEVEL, where
     * allocation is illegal. drain_arm is idempotent (a single exchange on
     * g_drain_armed), so a pending channel across many ticks queues one drain at
     * a time rather than a flood. */
    if (pressure_retry_due())
        drain_arm();
}

/* NOT a production path, and deliberately left without source arbitration.
 *
 * This seam feeds a stall observation into a domain of the 16-type BUDGET space,
 * where it shares that domain's single set of rise/fall counters with the 50 ms
 * budget sampler. The two cadences differ by about forty to one, so whichever
 * producer runs more often effectively owns the level.
 *
 * An earlier revision of section 12 tried to fix that by letting a stall
 * submission HOLD the domain against budget sampling. That was worse than the
 * problem: a caller submitting a low or unknown stall reading every couple of
 * seconds would suppress budget sampling indefinitely, so a domain sitting at
 * 1000 permille of real quota saturation would keep reporting normal. A
 * mechanism that can HIDE pressure is not an acceptable fix for one that can
 * merely mistime it, so the hold was removed rather than tuned.
 *
 * The real stall telemetry does not come through here at all: cpu / mem / io
 * live in their own lane (quota_stall.h) with their own validity, debounce, and
 * level, and meet the budget lane only in quota_pressure_system_level() as a
 * max. Giving the 16-type space genuine per-source lanes is tracked as a
 * follow-up; until then this entry point has no production caller.
 * -> XREF: the quota charge-path cost and lifetime follow-ups roadmap. */
void quota_pressure_submit_stall(quota_resource_type_t type,
                                 uint16_t permille, uint16_t window_ms)
{
    pressure_sample(type, permille, window_ms, QUOTA_PRESSURE_SRC_STALL);
}

/* ==========================================================================
 * Failure events
 * ========================================================================== */

/* Token bucket for one resource. Refills by whole windows only, so a burst of
 * refusals inside one window cannot each grant themselves a partial refill. */
static int bucket_take(quota_resource_type_t type)
{
    uint64_t now = uptime_ns();
    uint64_t window_ns = (uint64_t)QUOTA_FAILURE_EVENT_WINDOW_MS * QUOTA_NSEC_PER_MSEC;
    uint64_t flags;
    int      admitted = 0;

    spin_lock_irqsave(&g_bucket_lock, &flags);

    /* Seeded on first use rather than at init: the buckets must work before
     * quota_pressure_init runs (a refusal during early boot is still an
     * event), and a static initializer cannot fill an array with a non-zero
     * value without repeating the burst constant per entry. */
    if (!g_buckets_seeded) {
        for (uint32_t i = 0; i < QUOTA_PRESSURE_DOMAIN_COUNT; i++)
            g_bucket[i].tokens = QUOTA_FAILURE_EVENT_BURST;
        g_buckets_seeded = 1;
    }

    failure_bucket_t *b = &g_bucket[type];

    if (b->last_refill_ns == 0ull) {
        b->last_refill_ns = now;
    } else if (now > b->last_refill_ns && window_ns != 0ull) {
        uint64_t windows = (now - b->last_refill_ns) / window_ns;
        if (windows > 0ull) {
            /* Cap the window count before multiplying: a long idle period
             * would otherwise form a product large enough to wrap before the
             * clamp below ever sees it. */
            /* Cap the window count before multiplying so a long idle cannot
             * form a product large enough to wrap. Capping at the burst size is
             * enough: the sum is clamped to the burst anyway, so more windows
             * could never yield more tokens. */
            if (windows > (uint64_t)QUOTA_FAILURE_EVENT_BURST)
                windows = (uint64_t)QUOTA_FAILURE_EVENT_BURST;
            uint64_t tok = (uint64_t)b->tokens +
                           windows * (uint64_t)QUOTA_FAILURE_EVENT_REFILL;
            b->tokens = (tok > (uint64_t)QUOTA_FAILURE_EVENT_BURST)
                      ? QUOTA_FAILURE_EVENT_BURST : (uint32_t)tok;
            /* Advance by WHOLE windows only, preserving the remainder. Setting
             * this to `now` would discard the part-window already elapsed, so a
             * caller arriving just after each refill would keep resetting the
             * clock and slow the effective refill rate below the configured
             * one. */
            b->last_refill_ns += windows * window_ns;
        }
    }
    if (b->tokens > 0u) {
        b->tokens--;
        admitted = 1;
    }
    spin_unlock_irqrestore(&g_bucket_lock, flags);
    return admitted;
}

void quota_pressure_note_failure(const quota_failure_source_t *src)
{
    pressure_ring_entry_t entry;
    QUOTA_FAILURE_RECORD *rec;
    uint64_t              seq;

    if (!src || !domain_valid(src->type))
        return;

    /* The sequence advances for every refusal, admitted or not, so a consumer
     * seeing 41 then 44 knows two were dropped rather than guessing. */
    seq = __atomic_add_fetch(&g_event_seq, 1ull, __ATOMIC_RELAXED);

    if (!bucket_take(src->type)) {
        __atomic_fetch_add(&g_drop_ratelimit, 1ull, __ATOMIC_RELAXED);
        return;
    }

    entry.kind = QUOTA_PRESSURE_KIND_FAILURE;
    rec = &entry.u.failure;
    memset(rec, 0, sizeof(*rec));

    rec->layout_version = QUOTA_FAILURE_RECORD_VERSION;
    rec->resource       = (uint32_t)src->type;
    rec->block_id       = src->block_id;
    rec->requested      = src->requested;
    rec->current        = src->current;
    rec->limit          = src->limit;
    rec->owner_sid_hash = src->owner_sid_hash;
    rec->event_seq      = seq;
    rec->timestamp_ns   = uptime_ns();
    rec->result         = (int32_t)src->result;
    rec->owner_rid      = src->owner_rid;
    rec->principal      = (uint8_t)src->principal;

    /* Identity is taken from the CALLER's capture, made at the instant of
     * refusal. Re-reading it here would be worse than useless: this function
     * has already taken the bucket lock, and a reschedule across that window
     * could make the record name a process that never touched this quota. The
     * accompanying attribution flags say how far the fields may be trusted. */
    rec->pid         = src->pid;
    rec->tid         = src->tid;
    rec->attribution = src->attribution;

    if (ring_push(&entry)) {
        __atomic_fetch_add(&g_events_recorded, 1ull, __ATOMIC_RELAXED);
        drain_arm();
    }
}

/* ==========================================================================
 * Queries
 * ========================================================================== */

quota_pressure_level_t quota_pressure_level(quota_resource_type_t type)
{
    pressure_domain_t *d;
    uint64_t           flags;
    uint8_t            level;

    if (!domain_valid(type))
        return QUOTA_PRESSURE_NORMAL;

    d = &g_domain[type];
    spin_lock_irqsave(&d->lock, &flags);
    level = d->level;
    spin_unlock_irqrestore(&d->lock, flags);
    return (quota_pressure_level_t)level;
}

quota_pressure_source_t quota_pressure_source_kind(quota_resource_type_t type)
{
    pressure_domain_t *d;
    uint64_t           flags;
    uint8_t            kind;

    if (!domain_valid(type))
        return QUOTA_PRESSURE_SRC_NONE;

    d = &g_domain[type];
    spin_lock_irqsave(&d->lock, &flags);
    kind = d->source_kind;
    spin_unlock_irqrestore(&d->lock, flags);
    return (quota_pressure_source_t)kind;
}

int quota_pressure_source_valid(quota_resource_type_t type)
{
    pressure_domain_t *d;
    uint64_t           flags;
    uint8_t            valid;

    if (!domain_valid(type))
        return 0;

    d = &g_domain[type];
    spin_lock_irqsave(&d->lock, &flags);
    valid = d->valid;
    spin_unlock_irqrestore(&d->lock, flags);
    return valid ? 1 : 0;
}

quota_pressure_level_t quota_pressure_system_level(void)
{
    /* Two independent lanes, one answer. The budget lane below derives levels
     * from quota saturation across the 16 resource types; the stall lane
     * (quota_stall.c) derives them from PSI-shaped stall time across cpu / mem /
     * io. They are debounced separately on purpose -- see
     * QUOTA_PRESSURE_STALL_HOLD_WINDOWS for why sharing a debounce corrupts both
     * -- and meet only here, as the worse of the two. Taking the max is the only
     * safe combination: a system stalling badly on I/O while every quota block
     * sits idle is under pressure, and averaging the two would hide it. */
    quota_pressure_level_t worst = quota_stall_system_level();

    for (uint32_t i = 0; i < QUOTA_PRESSURE_DOMAIN_COUNT; i++) {
        pressure_domain_t *d = &g_domain[i];
        uint64_t flags;
        uint8_t  level, valid;

        spin_lock_irqsave(&d->lock, &flags);
        level = d->level;
        valid = d->valid;
        spin_unlock_irqrestore(&d->lock, flags);

        /* An invalid domain is UNKNOWN, not calm. Skipping it keeps the system
         * level honest: a domain whose instrument does not exist cannot lower
         * the answer, and it cannot raise it either. */
        if (!valid)
            continue;
        /* FIRST measurement wins outright, later ones only if worse. The two
         * cases cannot be collapsed into the comparison alone: `worst` may be
         * UNKNOWN (the stall lane had nothing either), and UNKNOWN sorts ABOVE
         * every real level, so a bare `level > worst` would discard the first
         * genuine measurement and keep reporting "unmeasured" forever. */
        if (!quota_pressure_level_measured(worst) ||
            (quota_pressure_level_t)level > worst)
            worst = (quota_pressure_level_t)level;
    }

    /* Still UNKNOWN means neither lane has a single valid domain: no budget
     * instrument has sampled and no stall seam is wired. Report that rather than
     * NORMAL -- see the contract in quota_pressure.h. */
    return worst;
}

uint64_t quota_pressure_transition_count(void)
{
    return __atomic_load_n(&g_transitions, __ATOMIC_RELAXED);
}

uint64_t quota_pressure_events_recorded(void)
{
    return __atomic_load_n(&g_events_recorded, __ATOMIC_RELAXED);
}

uint64_t quota_pressure_dropped_ratelimit(void)
{
    return __atomic_load_n(&g_drop_ratelimit, __ATOMIC_RELAXED);
}

uint64_t quota_pressure_dropped_ring(void)
{
    return __atomic_load_n(&g_drop_ring, __ATOMIC_RELAXED);
}

uint64_t quota_pressure_dropped_transport(void)
{
    return __atomic_load_n(&g_drop_transport, __ATOMIC_RELAXED);
}

void quota_pressure_dump(void)
{
    klog(LOG_INFO, "quota", "pressure: system %s (%llu transitions, %llu events)",
         quota_pressure_level_name(quota_pressure_system_level()),
         quota_pressure_transition_count(), quota_pressure_events_recorded());

    for (uint32_t i = 0; i < QUOTA_PRESSURE_DOMAIN_COUNT; i++) {
        pressure_domain_t *d = &g_domain[i];
        uint64_t flags;
        uint8_t  level, valid, kind;
        uint16_t permille;

        spin_lock_irqsave(&d->lock, &flags);
        level    = d->level;
        valid    = d->valid;
        kind     = d->source_kind;
        permille = d->last_permille;
        spin_unlock_irqrestore(&d->lock, flags);

        if (!valid && kind == (uint8_t)QUOTA_PRESSURE_SRC_NONE)
            continue;   /* never sampled: nothing to report */

        klog(LOG_INFO, "quota", "  %s %s src %u %s (%u permille)",
             quota_resource_type_name((quota_resource_type_t)i),
             quota_pressure_level_name((quota_pressure_level_t)level),
             (uint32_t)kind, valid ? "valid" : "UNKNOWN", (uint32_t)permille);
    }

    klog(LOG_INFO, "quota", "  drops: %llu rate-limited, %llu ring, %llu transport",
         quota_pressure_dropped_ratelimit(), quota_pressure_dropped_ring(),
         quota_pressure_dropped_transport());
    /* The stall lane is the other half of the pressure picture; a dump that
     * showed only budget-sourced levels would read as "no pressure" for a
     * system stalling on I/O. */
    quota_stall_dump();
}

/* ==========================================================================
 * Escalation (cooperative only -- nominate, never terminate)
 * ========================================================================== */

int quota_pressure_nominate(quota_resource_type_t type,
                            QUOTA_NOMINATION_RECORD *out)
{
    uint64_t block_id = 0, sid_hash = 0;
    uint32_t rid = 0;
    uint16_t permille = 0;
    uint64_t now;
    int      found;

    if (!out)
        return 0;

    memset(out, 0, sizeof(*out));
    out->layout_version = QUOTA_NOMINATION_RECORD_VERSION;
    out->resource       = (uint32_t)type;
    out->principal      = (uint8_t)QUOTA_PRINCIPAL_USER;

    if (!domain_valid(type))
        return 0;

    found = quota_registry_worst_user(type,
                                      (uint16_t)QUOTA_PRESSURE_RISE_WATCH_PERMILLE,
                                      &block_id, &rid, &sid_hash, &permille);
    now = uptime_ns();
    out->nominated_at_ns = now;

    if (!found) {
        /* An explicit CLEAR rather than a silent zero: a consumer holding an
         * older nomination needs to be told the condition has passed, not left
         * to time it out. */
        out->valid           = 0;
        out->nomination_seq  = __atomic_add_fetch(&g_nomination_seq, 1ull, __ATOMIC_RELAXED);
        out->expires_at_ns   = now;
        return 0;
    }

    out->block_id       = block_id;
    out->owner_sid_hash = sid_hash;
    out->owner_rid      = rid;
    out->over_permille  = permille;
    out->nomination_seq = __atomic_add_fetch(&g_nomination_seq, 1ull, __ATOMIC_RELAXED);
    out->expires_at_ns  = now + ((uint64_t)QUOTA_NOMINATION_TTL_MS * QUOTA_NSEC_PER_MSEC);
    out->valid          = 1;
    return 1;
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

int quota_pressure_ready(void)
{
    return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) ? 1 : 0;
}

void quota_pressure_init(void)
{
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
        return;

    /* The domain locks need no explicit init: SPINLOCK_INIT is all-zero and
     * g_domain has static storage, so every lock is already in its released
     * state. That is what lets the state machine run before this function is
     * ever called (a refusal during early boot is still recorded). */
    /* Publication depends on a worker that may not exist yet: dpc_start_threads
     * can fail to create its thread. That is reported, but it is NOT fatal and
     * not permanent -- drain_arm re-checks on every arm, so publication starts
     * working if the worker appears later instead of staying dead for the boot. */
    if (!dpc_worker_started())
        klog(LOG_WARN, "quota",
             "pressure: threaded-DPC worker not running yet; records queue until it is");

    KeInitializeThreadedDpc(&g_drain_dpc, quota_pressure_drain, (void *)0);

    /* Anchor the stall lane's aggregation clock here rather than letting it
     * self-seed on its first tick: the interval from boot to this point is not
     * a measurement of anything, and dividing a window's growth by it would
     * report a fraction of an interval nobody observed. */
    quota_stall_init();

    /* Create all three channels and pre-size their retention buffers here, at
     * PASSIVE_LEVEL. Without the reservation a publish from the drain would have
     * to grow the buffer, and knf_publish refuses to allocate above PASSIVE --
     * the drain runs at PASSIVE today, but the reservation costs one call and
     * removes the dependency on that staying true.
     *
     * A channel refused for a TRANSIENT reason is left PENDING rather than
     * written off: the drain retries it later. This is the one-shot-caller defect
     * section 16 names -- a charge gate briefly closed by a job-membership
     * transition used to disable a notification channel permanently, because a
     * pointer-returning creation could not tell "not yet" from "no". */
    if (pressure_create_channels((1u << QUOTA_PRESSURE_CHANNEL_COUNT) - 1u) != 0)
        klog(LOG_WARN, "quota",
             "pressure: some channels deferred; the periodic sampler will retry them");

    /* Arm the periodic sampler. Without it a debounce could never complete for
     * pressure that is steady rather than churning: rising takes three samples
     * and falling five, and a quota mutation only ever marks the domain once. */
    {
        uint32_t hz = system_get_freq();
        uint64_t period = (hz != 0u)
            ? (((uint64_t)hz * QUOTA_PRESSURE_SAMPLE_WINDOW_MS) / 1000ull)
            : 0ull;
        if (period == 0ull)
            period = 1ull;      /* never 0: that would mean single-shot */
        KeInitializeDpc(&g_sample_dpc, quota_pressure_tick, (void *)0);
        KeInitializeTimer(&g_sample_timer);
        KeSetTimerEx(&g_sample_timer, system_get_ticks() + period, period,
                     &g_sample_dpc);
        g_sample_period_ticks = period;
    }

    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
    klog(LOG_INFO, "quota",
         "pressure: %u domains armed, ring %u slots, sampler every %llu ticks",
         (uint32_t)QUOTA_PRESSURE_DOMAIN_COUNT, (uint32_t)QUOTA_PRESSURE_RING_SLOTS,
         (uint64_t)g_sample_period_ticks);

    /* Anything recorded before the transport existed is published now. */
    if (ring_used() > 0u)
        drain_arm();
}

#ifdef KERNEL_TESTS
void quota_pressure_test_reset(void)
{
    uint64_t flags;

    /* Hold publication first: the live drain must not consume the records the
     * caller is about to produce and inspect. */
    __atomic_store_n(&g_test_hold, 1u, __ATOMIC_RELEASE);

    for (uint32_t i = 0; i < QUOTA_PRESSURE_DOMAIN_COUNT; i++) {
        pressure_domain_t *d = &g_domain[i];
        spin_lock_irqsave(&d->lock, &flags);
        d->level         = (uint8_t)QUOTA_PRESSURE_NORMAL;
        d->source_kind   = (uint8_t)QUOTA_PRESSURE_SRC_NONE;
        d->valid         = 0;
        d->last_permille = 0;
        d->window_ms     = 0;
        d->rise_count      = 0;
        d->fall_count      = 0;
        d->dirty           = 0;
        d->pending_head    = 0;
        d->pending_tail    = 0;
        d->pending_used    = 0;
        spin_unlock_irqrestore(&d->lock, flags);
    }

    spin_lock_irqsave(&g_ring_lock, &flags);
    g_ring_head = 0;
    g_ring_tail = 0;
    g_ring_used = 0;
    spin_unlock_irqrestore(&g_ring_lock, flags);

    spin_lock_irqsave(&g_bucket_lock, &flags);
    for (uint32_t i = 0; i < QUOTA_PRESSURE_DOMAIN_COUNT; i++) {
        g_bucket[i].tokens         = QUOTA_FAILURE_EVENT_BURST;
        g_bucket[i].last_refill_ns = 0;
    }
    g_buckets_seeded = 1;
    spin_unlock_irqrestore(&g_bucket_lock, flags);

    __atomic_store_n(&g_transitions, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&g_events_recorded, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drop_ratelimit, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drop_ring, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drop_transport, 0ull, __ATOMIC_RELAXED);
}

uint32_t quota_pressure_test_pop(QUOTA_PRESSURE_RECORD *out_transition,
                                 QUOTA_FAILURE_RECORD *out_failure)
{
    pressure_ring_entry_t entry;

    if (!ring_pop(&entry))
        return 0;

    if (entry.kind == QUOTA_PRESSURE_KIND_TRANSITION) {
        if (out_transition)
            *out_transition = entry.u.transition;
    } else if (entry.kind == QUOTA_PRESSURE_KIND_FAILURE) {
        if (out_failure)
            *out_failure = entry.u.failure;
    }
    return entry.kind;
}

uint32_t quota_pressure_test_pending(void)
{
    return ring_used();
}

void quota_pressure_test_refill_tokens(void)
{
    uint64_t flags;

    spin_lock_irqsave(&g_bucket_lock, &flags);
    for (uint32_t i = 0; i < QUOTA_PRESSURE_DOMAIN_COUNT; i++) {
        g_bucket[i].tokens         = QUOTA_FAILURE_EVENT_BURST;
        g_bucket[i].last_refill_ns = 0;
    }
    g_buckets_seeded = 1;
    spin_unlock_irqrestore(&g_bucket_lock, flags);
}

void quota_pressure_test_release(void)
{
    __atomic_store_n(&g_test_hold, 0u, __ATOMIC_RELEASE);
    /* Anything the suite left behind belongs to the real publisher again. */
    drain_arm();
}

/* TEST-SIDE-EFFECT-ALLOWED: this runs the real publish path, so it emits real
 * klog lines and real ETW/KNF records for records the test fabricated. That is
 * the point -- the publish path is what is under test -- but a serial-log
 * auditor reading a test boot will see escalation WARN lines for conditions
 * that never existed, and the notification states' change stamps advance. */
void quota_pressure_test_drain(void)
{
    pressure_ring_entry_t entry;

    /* Deliberately bypasses the hold and the DPC: this IS the publish path,
     * run synchronously so its branches (including the transport-drop
     * accounting when no notification state exists) are deterministically
     * covered without depending on the worker thread. */
    (void)drain_pending_transitions(QUOTA_PRESSURE_DOMAIN_COUNT *
                                    QUOTA_PRESSURE_PENDING_SLOTS);
    while (ring_pop(&entry)) {
        if (entry.kind == QUOTA_PRESSURE_KIND_TRANSITION) {
            publish_transition(&entry.u.transition);
            escalate_if_critical(&entry.u.transition);
        } else if (entry.kind == QUOTA_PRESSURE_KIND_FAILURE) {
            publish_failure(&entry.u.failure);
        }
    }
}

void quota_pressure_test_sample(quota_resource_type_t type, uint16_t permille)
{
    pressure_sample(type, permille, (uint16_t)QUOTA_PRESSURE_SAMPLE_WINDOW_MS,
                    QUOTA_PRESSURE_SRC_BUDGET);
}

/* --- LIVE create-path seam: test side ------------------------------------- */

/* The injected clock, and the status the injected creator reports. Both are read
 * by the seam functions above, which the drain can call from another CPU until the
 * quiesce below has flushed it, so both are atomic. */
static uint64_t g_test_clock_ns;
static NTSTATUS g_test_create_status;
static uint32_t g_test_create_calls;

static uint64_t pressure_test_clock(void)
{
    return __atomic_load_n(&g_test_clock_ns, __ATOMIC_ACQUIRE);
}

/* Counts every call and NEVER produces a state, so the caller takes the failure
 * branch. It cannot damage a live channel: it does not touch any slot, and the
 * isolation helper below is what decides which slot the create loop may reach. */
static NTSTATUS pressure_test_create(const char *name, KNF_STATE **out)
{
    (void)name;
    __atomic_fetch_add(&g_test_create_calls, 1u, __ATOMIC_RELAXED);
    if (out)
        *out = (KNF_STATE *)0;
    return __atomic_load_n(&g_test_create_status, __ATOMIC_ACQUIRE);
}

static const pressure_seam_t g_seam_test = { pressure_test_clock,
                                             pressure_test_create };

void quota_pressure_test_quiesce(void)
{
    /* Order is the whole contract. The hold stops the drain from being ARMED
     * again and stops an already-dispatched callback from doing any work; the
     * flush then waits out the callback that was already RUNNING. Without the
     * flush, `g_drain_armed == 0` proves nothing -- the drain clears that flag
     * before its body finishes -- so a seam installed on the strength of it could
     * be observed half-way by a callback still in flight. */
    __atomic_store_n(&g_test_hold, 1u, __ATOMIC_RELEASE);
    /* NOTE on the flush contract: dpc.h asks a teardown to stop its producers
     * first, and this does NOT cancel the 50 ms periodic sampler timer -- it only
     * hold-gates what that timer's DPC does. That is deliberate (the suite wants
     * the timer intact afterwards) and safe because the tick's duty cycle is
     * microseconds and the flush breaks on the first quiet sample; it is a hold,
     * not a subsystem teardown. */
    KeFlushQueuedDpcs();
}

void quota_pressure_test_seam_install(uint64_t now_ns, NTSTATUS create_status)
{
    __atomic_store_n(&g_test_clock_ns, now_ns, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_create_status, create_status, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_create_calls, 0u, __ATOMIC_RELEASE);
    /* One release store publishes BOTH function pointers, because they live in a
     * single immutable descriptor. The values above are published first, so a
     * reader that sees the new seam necessarily sees the clock and status it was
     * installed with. */
    __atomic_store_n(&g_seam, &g_seam_test, __ATOMIC_RELEASE);
}

void quota_pressure_test_seam_set_now(uint64_t now_ns)
{
    __atomic_store_n(&g_test_clock_ns, now_ns, __ATOMIC_RELEASE);
}

void quota_pressure_test_seam_remove(void)
{
    __atomic_store_n(&g_seam, &g_seam_live, __ATOMIC_RELEASE);
}

uint32_t quota_pressure_test_create_calls(void)
{
    return __atomic_load_n(&g_test_create_calls, __ATOMIC_RELAXED);
}

/* Make exactly ONE channel appear uncreated so the live create loop actually
 * reaches the creator. Without this the seam is unreachable after boot: every
 * slot is populated, and the loop skips a populated slot by design -- a test
 * could install a failing creator, drive the whole retry schedule, and never
 * invoke it once, "proving" a code path it never entered.
 *
 * The live KNF_STATE is SAVED, not destroyed: the slot is cleared so the loop
 * treats the channel as absent, and the restore below puts the identical pointer
 * back. Nothing is created, freed, or replaced, so the notification state a real
 * consumer may already hold a reference to is untouched throughout.
 *
 * REQUIRES the quiesce above (hold set AND drain flushed); a live drain would
 * otherwise see the cleared slot and try to create the channel for real. */
int quota_pressure_test_channel_isolate(uint32_t index,
                                        quota_pressure_channel_save_t *save)
{
    const pressure_channel_t *ch;

    if (!save || index >= QUOTA_PRESSURE_CHANNEL_COUNT)
        return -1;

    ch = &g_channel[index];
    save->index   = index;
    save->state   = (void *)__atomic_load_n(ch->slot, __ATOMIC_ACQUIRE);
    save->pending = __atomic_load_n(&g_channels_pending, __ATOMIC_ACQUIRE);
    save->retry   = g_channels_retry;
    /* at_ns is the ONE field of the schedule that crosses CPUs, and its contract
     * (see quota_pressure_retry_due) is that it is only ever touched atomically --
     * a whole-struct copy would read it plainly. Re-read it properly rather than
     * resting on the quiesce, which is a comment, not a barrier. */
    save->retry.at_ns = __atomic_load_n(&g_channels_retry.at_ns, __ATOMIC_RELAXED);

    /* Clear ONLY this slot, and seed ONLY this bit: the create loop attempts the
     * intersection of the attempt mask and the empty slots, so both halves are
     * needed to make exactly one channel reachable. */
    __atomic_store_n(ch->slot, (KNF_STATE *)0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_channels_pending, 1u << index, __ATOMIC_RELEASE);
    __atomic_store_n(&g_channels_retry.at_ns, 0ull, __ATOMIC_RELAXED);
    g_channels_retry.gap_ns = 0;
    g_channels_retry.tries  = 0;
    g_channels_retry.warned = 0;
    return 0;
}

void quota_pressure_test_channel_restore(const quota_pressure_channel_save_t *save)
{
    const pressure_channel_t *ch;

    if (!save || save->index >= QUOTA_PRESSURE_CHANNEL_COUNT)
        return;

    ch = &g_channel[save->index];
    /* The saved pointer, restored verbatim. The retry schedule and pending mask
     * go back too, so the suite cannot leave the live subsystem believing a
     * channel is missing or owing a backoff it never earned. */
    __atomic_store_n(ch->slot, (KNF_STATE *)save->state, __ATOMIC_RELEASE);
    g_channels_retry = save->retry;
    __atomic_store_n(&g_channels_retry.at_ns, save->retry.at_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&g_channels_pending, save->pending, __ATOMIC_RELEASE);
}

void *quota_pressure_test_channel_state(uint32_t index)
{
    if (index >= QUOTA_PRESSURE_CHANNEL_COUNT)
        return (void *)0;
    return (void *)__atomic_load_n(g_channel[index].slot, __ATOMIC_ACQUIRE);
}

uint32_t quota_pressure_test_create_attempt(uint32_t mask)
{
    return pressure_create_channels(mask);
}

int quota_pressure_test_retry_due(void)
{
    return pressure_retry_due();
}

uint64_t quota_pressure_test_retry_deadline(void)
{
    return __atomic_load_n(&g_channels_retry.at_ns, __ATOMIC_RELAXED);
}

uint32_t quota_pressure_test_retry_tries(void)
{
    return g_channels_retry.tries;
}
#endif /* KERNEL_TESTS */
