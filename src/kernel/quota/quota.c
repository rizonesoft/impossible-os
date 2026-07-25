/* ============================================================================
 * quota.c -- Kernel resource-accounting type registry
 *
 * Owns the static const descriptor table that names every chargeable kernel
 * resource type, its accounting unit, default limit, and override privilege.
 * The table is the single source of truth for the taxonomy; the charge API and
 * every integration section consume it through the accessors below.
 * ============================================================================ */

#include "kernel/quota/quota.h"
#include "kernel/quota/quota_pressure.h" /* saturation sampling + refusal events */
#include "kernel/security/privileges.h"  /* SE_INCREASE_QUOTA_PRIVILEGE */
#include "kernel/mm/heap.h"              /* kmalloc_zeroed / kfree */
#include "kernel/sched/irql.h"           /* KeGetCurrentIrql for the diag gate */
#include "kernel/klog.h"
#include "kernel/smp.h"               /* smp_cpu_id for test-only CPU scoping */
#include "kernel/sched/task.h"           /* task_current / thread_current for refusal identity */

/* Same house pattern as src/kernel/security/sid.c: the freestanding build has
 * no string.h, so the compiler-provided memcpy is declared where it is used. */
extern void *memcpy(void *dst, const void *src, size_t n);

/* --- Descriptor table ---------------------------------------------------- *
 * static const so every field is available at link time (before any charge)
 * and reads are lock-free/SMP-safe. All default limits are UNLIMITED at this
 * layer; concrete numeric caps are policy owned by the kernel configuration
 * layer, applied at charge time per the precedence documented in quota.h. */
/* Local shorthands: every row shares the same override privilege and (at this
 * layer) an unlimited default; concrete caps come from kernel config. */
#define Q_PRIV  SE_INCREASE_QUOTA_PRIVILEGE
#define Q_UNL   QUOTA_LIMIT_UNLIMITED

/* UNSIZED on purpose: sizing this [QUOTA_RESOURCE_TYPE_COUNT] would make the
 * Layer 1 assert below tautological (sizeof/sizeof would equal the bound by
 * definition, whether or not every row is initialized). Leaving it unsized
 * makes the array exactly as long as the rows present, so a new enum member
 * without a matching row shrinks it and trips the assert at compile time. */
static const quota_resource_desc_t g_quota_desc[] = {
    [QUOTA_RES_HANDLE]             = { "handles",             Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_OBJECT_BODY]        = { "object-bodies",       Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_NAMESPACE_ENTRY]    = { "namespace-entries",   Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_PAGED_POOL]         = { "paged-pool",          Q_PRIV, Q_UNL, QUOTA_UNIT_BYTES },
    [QUOTA_RES_NONPAGED_POOL]      = { "nonpaged-pool",       Q_PRIV, Q_UNL, QUOTA_UNIT_BYTES },
    [QUOTA_RES_REGISTRY_BYTES]     = { "registry-bytes",      Q_PRIV, Q_UNL, QUOTA_UNIT_BYTES },
    [QUOTA_RES_ALPC_MESSAGE]       = { "alpc-messages",       Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_NOTIFICATION_STATE] = { "notification-states", Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_TIMER]              = { "timers",              Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_THREAD]             = { "threads",             Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_PROCESS]            = { "processes",           Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_SECTION]            = { "sections",            Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_MAPPED_VIEW]        = { "mapped-views",        Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_CRASH_BUFFER]       = { "crash-buffers",       Q_PRIV, Q_UNL, QUOTA_UNIT_BYTES },
    /* Appended by section 6 -- rows follow enum order, which is ABI. */
    [QUOTA_RES_NOTIFICATION_SUB]   = { "notification-subs",   Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_NOTIFICATION_BYTES] = { "notification-bytes",  Q_PRIV, Q_UNL, QUOTA_UNIT_BYTES },
};

#undef Q_PRIV
#undef Q_UNL

/* Layer 1 defense: the parallel array must have exactly one entry per type, so
 * a new enum member without a matching row is a compile error, not a runtime
 * gap (a designated-initializer table would otherwise leave holes zero-filled
 * with a NULL name). This only bites because the array above is UNSIZED. */
_Static_assert(sizeof(g_quota_desc) / sizeof(g_quota_desc[0]) == QUOTA_RESOURCE_TYPE_COUNT,
               "quota descriptor table must have one entry per resource type");

/* Defined with the USER-block registry below; declared here because the
 * taxonomy validation seeds the effective defaults as its last step. */
void quota_user_default_publish(quota_resource_type_t type, uint64_t limit);

/* Registry-ready flag. Written once by quota_register_types() on the BSP in
 * Phase 2 before any consumer charges; read-only thereafter. Release/acquire
 * pairs the store with the reads so an AP that sees readiness also sees a fully
 * validated table. */
static volatile int g_quota_ready = 0;

/* Content-compare two descriptor names (string literals are not guaranteed to
 * be pooled to one pointer, so a pointer compare is not sufficient). */
static int quota_name_eq(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return *a == *b;
}

boot_result_t quota_register_types(void)
{
    /* Layer 2 defense: validate the static table. A malformed taxonomy (a hole
     * left by a missing initializer, a bad unit, a missing override privilege,
     * or a duplicate name) must halt boot rather than let consumers charge
     * against an inconsistent registry. */
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        const quota_resource_desc_t *d = &g_quota_desc[i];
        if (d->name == (const char *)0 || d->name[0] == '\0') {
            klog(LOG_ERROR, "quota", "resource type %u has no name", (uint64_t)i);
            return BOOT_FATAL;
        }
        if (d->unit != QUOTA_UNIT_COUNT && d->unit != QUOTA_UNIT_BYTES) {
            klog(LOG_ERROR, "quota", "resource type %u (%s) has bad unit %d",
                 (uint64_t)i, d->name, (int64_t)d->unit);
            return BOOT_FATAL;
        }
        /* Security-sensitive: every row must name a real override privilege; a
         * zero LUID would mean nothing gates raising this resource's cap. */
        if (RtlIsZeroLuid(&d->override_privilege)) {
            klog(LOG_ERROR, "quota", "resource type %u (%s) has a zero override privilege",
                 (uint64_t)i, d->name);
            return BOOT_FATAL;
        }
        /* Names must be unique so a dump or by-name query never maps two types
         * to one label. */
        for (uint32_t j = 0; j < i; j++) {
            if (quota_name_eq(d->name, g_quota_desc[j].name)) {
                klog(LOG_ERROR, "quota", "resource type %u (%s) duplicates type %u",
                     (uint64_t)i, d->name, (uint64_t)j);
                return BOOT_FATAL;
            }
        }
    }

    /* Seed the effective USER defaults from the validated taxonomy. Every USER
     * block created before the config layer registers (Phase 3) uses these;
     * quota_config_register_tunables then re-publishes them from the tunables,
     * and any later change flows through quota_user_default_relimit. */
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++)
        quota_user_default_publish((quota_resource_type_t)i,
                                   g_quota_desc[i].default_limit);

    __atomic_store_n(&g_quota_ready, 1, __ATOMIC_RELEASE);
    klog(LOG_INFO, "quota", "resource type registry: %u types validated",
         (uint64_t)QUOTA_RESOURCE_TYPE_COUNT);
    return BOOT_OK;
}

const quota_resource_desc_t *quota_resource_desc(quota_resource_type_t type)
{
    if ((uint32_t)type >= QUOTA_RESOURCE_TYPE_COUNT)
        return (const quota_resource_desc_t *)0;
    return &g_quota_desc[type];
}

const char *quota_resource_type_name(quota_resource_type_t type)
{
    const quota_resource_desc_t *d = quota_resource_desc(type);
    return d ? d->name : "?";
}

uint32_t quota_resource_type_count(void)
{
    return QUOTA_RESOURCE_TYPE_COUNT;
}

int quota_registry_ready(void)
{
    return __atomic_load_n(&g_quota_ready, __ATOMIC_ACQUIRE);
}

void quota_types_dump(void)
{
    klog(LOG_INFO, "quota", "resource type registry (%u types):",
         (uint64_t)QUOTA_RESOURCE_TYPE_COUNT);
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        const quota_resource_desc_t *d = &g_quota_desc[i];
        const char *unit = (d->unit == QUOTA_UNIT_BYTES) ? "bytes" : "count";
        if (d->default_limit == QUOTA_LIMIT_UNLIMITED)
            klog(LOG_INFO, "quota", "  [%u] %s unit=%s limit=unlimited",
                 (uint64_t)i, d->name, unit);
        else
            klog(LOG_INFO, "quota", "  [%u] %s unit=%s limit=%u",
                 (uint64_t)i, d->name, unit, d->default_limit);
    }
}

/* ==========================================================================
 * Quota blocks and the charge API
 *
 * Counter invariant: every stored counter stays in 0 .. QUOTA_AMOUNT_MAX.
 * atomic64_t is signed, so every add is bounds-checked BEFORE it happens --
 * a signed overflow would be undefined behavior and could wrap a counter
 * below its limit, turning the cap into a no-op. See quota.h for the
 * lifetime and limit-lowering contracts this code implements.
 * ========================================================================== */

/* The block definition lives HERE, not in the header, so no other translation
 * unit can touch a counter directly or acquire `lock`. Only the functions
 * below take that lock, which is what makes the address-ordered two-block
 * acquisition in quota_try_transfer the ONLY multi-lock path in the system
 * and therefore deadlock-free by construction. */
/* The four counters for ONE resource type, stored together.
 *
 * The original layout was four parallel arrays -- usage[14], peak[14],
 * failures[14], limit[14] -- so charging type T read or wrote four locations
 * up to 112 bytes apart, which is as many as four cache lines for a single
 * charge. Grouping one type's counters into a 32-byte record makes a charge
 * touch 32 CONTIGUOUS bytes: two lines at worst, one in the common case.
 *
 * How many lines a record actually spans is decided by the block's runtime
 * address AND by this array's offset within the block. Records are 32 bytes and
 * the array starts 24 bytes in, so record i begins at block + 24 + 32i: with
 * kmalloc's 16-byte guarantee the start alternates between a line-fitting and a
 * line-straddling offset, and EXACTLY HALF the records straddle -- at every
 * legal block address, including a 64-byte-aligned one. The measured 7-of-14 is
 * that structural result, not luck. Fixing it needs the array's offset to be a
 * multiple of 32 as well as an aligned allocation; a 64-byte-aligned pool
 * allocation alone would NOT change the ratio. Owned by the advanced-allocator
 * work that introduces the pool classes. The static asserts below pin the
 * record's SIZE and contiguity only -- never a runtime address property.
 *
 * Grouping does not introduce WRITER-vs-writer false sharing: one lock guards
 * every mutation of a block, so two CPUs never write two different types'
 * counters in the same block concurrently. It DOES change reader-vs-writer
 * sharing, and not for the better: the old parallel arrays kept limit and
 * failures on different lines from the usage/peak a charge writes, whereas this
 * record co-locates all four, so a lock-free quota_limit/quota_failures reader
 * can now have its line invalidated by an unrelated charge on the same block
 * and type. That cost is unmeasured on real SMP; the writer-locality win is the
 * reason it is accepted for now, not a proof that it dominates. */
typedef struct quota_counters {
    atomic64_t usage;      /* current charge, 0..QUOTA_AMOUNT_MAX     */
    atomic64_t peak;       /* high-water usage                        */
    atomic64_t failures;   /* refused charges, saturating             */
    atomic64_t limit;      /* QUOTA_LIMIT_UNLIMITED = no cap          */
} quota_counters_t;

/* Bytes in one per-type record, and the smallest cache line on any target this
 * kernel builds for. The record must not exceed a line, or the locality claim
 * above ("two lines at worst") stops holding. */
#define QUOTA_COUNTER_RECORD_BYTES  32u
#define QUOTA_COUNTER_LINE_BYTES    64u

struct quota_block {
    /* Registry linkage, guarded by g_registry_lock (NOT by `lock`). Kept first
     * so a walk touches the same cache line it needs to advance. */
    struct quota_block *reg_next;
    struct quota_block *reg_prev;
    uint8_t    principal;                           /* quota_principal_t, immutable */
    uint8_t    registered;                          /* on the USER registry list */
    atomic_t   refcount;                            /* live references; 0 frees */
    /* One record per resource type: see quota_counters_t above. */
    quota_counters_t counter[QUOTA_RESOURCE_TYPE_COUNT];
    spinlock_t lock;                                /* guards ALL mutations    */
    /* Per-type limit PROVENANCE, guarded by `lock` alongside the limit it
     * describes. 0 = the limit is still whatever the block was seeded with at
     * create time (a policy default); 1 = quota_set_limit set it explicitly.
     *
     * This is what makes the runtime config default safe to re-apply: a live
     * USER block whose limit was never set explicitly is still expressing the
     * administrator's default and must follow it when that default changes,
     * while a block someone deliberately capped must NOT be silently raised or
     * lowered back to the default. Without the distinction the config layer
     * would have to choose between never reaching live blocks (a default no
     * administrator can actually change) or trampling explicit limits. */
    uint8_t    limit_explicit[QUOTA_RESOURCE_TYPE_COUNT];
    /* Rate policy per class, published as a WHOLE under a sequence counter.
     *
     * `rate_seq[cls]` is odd while that class is being written and even when
     * its record is stable; a reader samples it before and after copying and
     * retries on a change. That is what makes a policy update atomic to a
     * consumer without making the READ take the block lock -- a scheduler
     * consulting a policy runs in exactly the contexts (its own lock held,
     * interrupts disabled) where quota.h forbids entering this API's critical
     * sections. Writers are serialized by `lock`, so a class never has two
     * concurrent writers.
     *
     * PER CLASS, not one shared counter. A single counter would let a write to
     * any class restart a reader of a DIFFERENT class, so a busy publisher
     * could keep a reader retrying indefinitely -- in a context where that
     * reader may be running with interrupts disabled, which turns a livelock
     * into a hang. Per-class sequencing bounds the interference to the one
     * class actually being updated, and the reader is bounded on top of that
     * (see QUOTA_RATE_READ_TRIES). */
    uint32_t   rate_seq[QUOTA_RATE_CLASS_COUNT];
    quota_rate_limit_t rate[QUOTA_RATE_CLASS_COUNT];
    uint32_t   owner_sid_buf[SID_MAX_SIZE / 4];     /* SID capture, 4-aligned  */
    uint8_t    has_owner_sid;                       /* 0 = no owner recorded    */
    /* Stable identity for diagnostics, assigned once at create and never
     * reused. A refusal event has to name WHICH principal was refused, and the
     * block ADDRESS cannot serve: it is recycled by the allocator, so a
     * consumer correlating two events by address could attribute a later
     * block's refusal to an earlier block's owner. Immutable, so it is readable
     * without the block lock.
     *
     * Placed AFTER the counter array on purpose. Sitting in the prefix, it
     * would push the counters 8 bytes along and change which records share a
     * cache line -- the locality claim the perf suite pins. Its own readers
     * (the nomination walk and the refusal path) are both cold, so nothing
     * pays for the extra line. */
    uint64_t   id;
};

/* A block must fit the kmalloc size rule (<= 4 KB). 16 types x 4 counters
 * plus the SID buffer is far under that; assert so a future type-count growth
 * cannot silently push allocation into pmm_alloc_contiguous territory. */
_Static_assert(sizeof(quota_block_t) <= 4096,
    "quota_block_t must stay within the kmalloc size rule (<= 4 KB)");

/* A record must be exactly its four counters with no padding. If the compiler
 * ever inserted any, the record would grow past 32 bytes and the locality
 * claim (a charge touches at most two lines) would quietly stop holding while
 * everything still compiled and passed. */
_Static_assert(sizeof(quota_counters_t) == 4 * sizeof(atomic64_t),
    "quota_counters_t must be exactly its four counters, with no padding");
_Static_assert(sizeof(quota_counters_t) == QUOTA_COUNTER_RECORD_BYTES,
    "quota_counters_t must be QUOTA_COUNTER_RECORD_BYTES wide");

/* A record wider than one cache line could span three lines at an unlucky
 * address, which is worse than the layout this replaced for the peak+limit
 * pair. This is the assert that keeps a future fifth counter honest: adding
 * one means re-deriving the locality claim, not silently widening the record. */
_Static_assert(QUOTA_COUNTER_RECORD_BYTES <= QUOTA_COUNTER_LINE_BYTES,
    "a per-type counter record must fit within one cache line");

/* The counter array is indexed by quota_resource_type_t, so it must have one
 * record per type. Without this, a hand-edited smaller bound would compile
 * while the loops and accessors kept indexing across the full enum, writing
 * into adjacent struct fields. */
_Static_assert(sizeof(((quota_block_t *)0)->counter) ==
               sizeof(quota_counters_t) * QUOTA_RESOURCE_TYPE_COUNT,
    "quota_block_t.counter must have one record per resource type");

/* The SID capture buffer is uint32_t-typed (SubAuthority alignment) and sized
 * by division, which truncates DOWN. If SID_MAX_SIZE ever stops being a
 * multiple of 4 the buffer would be short and the bounded memcpy in
 * quota_block_create would run past it into the following fields. */
_Static_assert(sizeof(((quota_block_t *)0)->owner_sid_buf) >= SID_MAX_SIZE,
    "owner_sid_buf must hold a maximum-length SID (SID_MAX_SIZE must be 4-aligned)");

static int quota_type_valid(quota_resource_type_t type)
{
    return (uint32_t)type < QUOTA_RESOURCE_TYPE_COUNT;
}

#ifdef KERNEL_TESTS
/* Critical sections COMPLETED while counting is armed, attributed to the
 * ARMING THREAD at PASSIVE_LEVEL. Diagnostic: it orders nothing and guards
 * nothing, so relaxed accesses are exactly right -- atomicity without a
 * barrier.
 *
 * The scoping is load-bearing, not defensive, and it is deliberately by
 * thread rather than by CPU. The counted set is not just charging:
 * quota_task_init and quota_task_teardown take task->quota_lock on EVERY task
 * creation and every process death. Under the earlier CPU-scoped rule a
 * sibling task completing a section on the armed CPU ADDED to the total while
 * a measured thread that migrated mid-window DROPPED sections from it, and
 * every budget assertion is an exact equality -- so either direction is a hard
 * test failure rather than tolerated skew. Matching the arming thread makes
 * the count follow the operation under test wherever it runs.
 *
 * The PASSIVE_LEVEL gate closes the other half: an interrupt or DPC landing on
 * the measured thread's stack runs with that thread current, so thread
 * identity alone would let a timer interrupt's quota work count as the
 * invocation's. Sections released above PASSIVE are not this invocation's
 * work and are skipped. */
static uint64_t      g_lock_sections;
static uint8_t       g_lock_count_on;
static struct thread *g_lock_count_thread;

/* COUNTER-mutation epoch, distinct from the registry's membership generation.
 *
 * It exists because membership and totals are independent: a transfer moves
 * usage between two blocks without linking or unlinking anything, so the
 * generation sits perfectly still while the aggregate the leak sweep is
 * computing changes underneath it. Two walks that merely AGREE do not close
 * that hole either -- reading the source before the transfer and the
 * destination after it yields the same wrong total both times.
 *
 * Bumped on every block-lock release, which over-counts (a release need not
 * have mutated anything) and never under-counts: every counter write happens
 * under that lock. Over-counting costs the sweep a retry; under-counting would
 * cost it correctness.
 *
 * KERNEL_TESTS only. The sweep it serves is test infrastructure, and putting a
 * shared RMW on the production charge path to serve a diagnostic is exactly
 * the trade this module refuses elsewhere -- which is why the storage below is
 * PER-CPU rather than a pair of globals. */

/* Writers currently INSIDE a block critical section. The epoch alone is not
 * enough, because it advances on COMPLETION: quota_try_transfer credits the
 * destination and debits the source before either unlock, so an unlocked
 * walker can observe the amount in BOTH blocks while the epoch sits still on
 * either side of the walk. Two agreeing walks then certify a total that never
 * existed. Requiring this count to be zero before AND after the walks is what
 * rejects a mutation that is in progress rather than merely finished. */
/* PER-CPU, cache-line isolated. A single global pair would put three locked
 * RMWs on ONE shared line in every quota critical section -- and KERNEL_TESTS
 * is ON in the shipped image, so charges to completely unrelated blocks would
 * contend globally even though their block locks are independent. That is a
 * production hot-path tax for a test-time diagnostic, which is precisely the
 * trade this module refuses. Per-CPU slots keep every RMW in the issuing CPU's
 * own line; the sweep pays the aggregation instead, once per sample.
 *
 * Padded to a full line so two CPUs never share one, and so these never share
 * a line with the arm-gate byte the lock-section counter reads. */
typedef struct {
    uint64_t epoch;
    uint32_t active;
    uint8_t  pad[QUOTA_COUNTER_LINE_BYTES - sizeof(uint64_t) - sizeof(uint32_t)];
} quota_writer_slot_t;

_Static_assert(sizeof(quota_writer_slot_t) == QUOTA_COUNTER_LINE_BYTES,
    "quota writer slots must be exactly one cache line, or CPUs false-share");

static quota_writer_slot_t g_writer_slot[MAX_CPUS]
    __attribute__((aligned(QUOTA_COUNTER_LINE_BYTES)));

/* Size alone is NOT isolation. A 64-byte stride still lets one slot's `active`
 * share a line with the next slot's `epoch` if the ARRAY BASE is not itself
 * line-aligned -- which the natural alignment of the type does not guarantee,
 * and which a harmless BSS or linker-layout change could silently introduce.
 * Both facts are asserted so neither can drift. */
_Static_assert(__alignof__(g_writer_slot) >= QUOTA_COUNTER_LINE_BYTES,
    "g_writer_slot must be cache-line ALIGNED, not merely cache-line sized: "
    "without the aligned attribute one slot's active shares a line with the "
    "next slot's epoch and adjacent CPUs false-share on every charge");

/* Entered BEFORE the lock and exited AFTER it, deliberately: running them
 * inside the critical section would lengthen an IRQ-off quota hold. */
static inline void quota_writers_enter(void)
{
    uint32_t cpu = smp_cpu_id();
    if (cpu >= MAX_CPUS)
        return;
    __atomic_fetch_add(&g_writer_slot[cpu].active, 1, __ATOMIC_ACQ_REL);
}

static inline void quota_writers_exit(void)
{
    uint32_t cpu = smp_cpu_id();
    if (cpu >= MAX_CPUS)
        return;
    __atomic_fetch_add(&g_writer_slot[cpu].epoch, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&g_writer_slot[cpu].active, 1, __ATOMIC_ACQ_REL);
}

/* --- Split enter/exit, for a path that must mark itself in-flight BEFORE it
 * masks interrupts ---------------------------------------------------------
 *
 * The N-lock adjust path cannot use the pair above. It masks interrupts around
 * its entire claim-to-republish window, and the number of locks it will take is
 * not known until after the mask is in place, so entering once per lock inside
 * the loop puts every one of those atomic RMWs inside the IRQ-off window -- the
 * cost the enter-before-lock ordering exists to avoid, reintroduced by a path
 * that acquires N locks instead of one.
 *
 * The split lets that path mark itself in-flight with ONE RMW before masking, and
 * settle the epoch and section accounting after unmasking. The RECORDED TOTALS ARE
 * IDENTICAL to entering N times (epoch advances N, active nets to zero); the only
 * observable difference is that peak `active` reads 1 instead of N, which the gate
 * cannot distinguish because it only ever asks whether anything is in flight (see
 * quota_test_writers_active). Marking in-flight EARLIER is strictly safer: the
 * window now covers the tag claim as well as the block sections. */
static inline void quota_writers_enter_active(void)
{
    uint32_t cpu = smp_cpu_id();
    if (cpu >= MAX_CPUS)
        return;
    __atomic_fetch_add(&g_writer_slot[cpu].active, 1, __ATOMIC_ACQ_REL);
}

static inline void quota_writers_exit_active(void)
{
    uint32_t cpu = smp_cpu_id();
    if (cpu >= MAX_CPUS)
        return;
    __atomic_fetch_sub(&g_writer_slot[cpu].active, 1, __ATOMIC_ACQ_REL);
}

/* Advance the mutation epoch by the number of block sections a transaction
 * completed, without touching the in-flight count. One RMW rather than n: the
 * epoch is a monotonic total that only ever gets summed and differenced, so
 * adding n at once is indistinguishable from n increments. */
static inline void quota_writers_epoch_add(uint32_t n)
{
    uint32_t cpu = smp_cpu_id();
    if (cpu >= MAX_CPUS)
        return;
    if (n == 0)
        return;
    __atomic_fetch_add(&g_writer_slot[cpu].epoch, (uint64_t)n, __ATOMIC_RELAXED);
}

/* Summed across CPUs. A writer that MIGRATES between its enter and its exit
 * would decrement a different slot than it incremented, so an individual slot
 * can go momentarily negative-as-unsigned -- the SUM is still correct, which is
 * all the gate needs, because it only ever asks "is anything in flight". */
uint64_t quota_test_mutation_epoch(void)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        sum += __atomic_load_n(&g_writer_slot[i].epoch, __ATOMIC_RELAXED);
    return sum;
}

uint32_t quota_test_writers_active(void)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        sum += __atomic_load_n(&g_writer_slot[i].active, __ATOMIC_ACQUIRE);
    return sum;
}

/* Shared with quota_owner.c so owner-side sections land in the same total.
 * Called AFTER the lock is released (see quota_block_unlock). */
void quota_test_count_lock_section(void)
{
    if (!__atomic_load_n(&g_lock_count_on, __ATOMIC_RELAXED))
        return;
    /* Interrupt/DPC context is never the measured invocation. Checked first:
     * it is a plain per-CPU read, cheaper than resolving the thread. */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    /* thread_current() bounds-checks the scheduler cursor and returns NULL
     * before the scheduler is up; the arming thread is by construction alive
     * for the whole window, so a NULL here is simply "not us". */
    if (thread_current() !=
        __atomic_load_n(&g_lock_count_thread, __ATOMIC_RELAXED))
        return;
    __atomic_fetch_add(&g_lock_sections, 1, __ATOMIC_RELAXED);
}
#endif

/* The one place a block's lock is taken FOR A COUNTER MUTATION. Every charge,
 * return, transfer, and set-limit path goes through this pair, which is what
 * lets the test-only instrumentation stay honest: a future lock site either
 * uses this helper and is counted, or takes the lock itself and is visible as
 * an obvious deviation in review.
 *
 * ONE deliberate exception exists today: quota_rate_limit_set takes the lock
 * directly, because it publishes block->rate[] policy under a sequence counter
 * and touches no counter[] field. It is therefore invisible to both the writer
 * count and the section budget, which is correct -- the leak snapshot reads
 * only counter[].usage, so a rate publication cannot perturb it. A future
 * direct-lock site that DOES touch counter[] would break that reasoning.
 *
 * The count is bumped on the UNLOCK side, after the lock is released, so it
 * counts sections COMPLETED and no instrumentation runs inside a quota
 * critical section. That ordering matters beyond tidiness: KERNEL_TESTS is ON
 * by default (Makefile), so this code is in the image that boots on real
 * hardware -- an armed counter's locked increment inside the critical section
 * would lengthen every quota hold time and serialize CPUs on the counter's
 * cache line. Every section entered is also left, so the total matches what an
 * entry-side counter would produce.
 *
 * It does NOT guarantee interrupts are enabled at that point: unlock restores
 * the CALLER's saved IF, so an interrupt-context caller (supported) or one
 * beneath an outer irqsave lock still has them masked. See quota.h for the
 * full contract and the attribution limits. */
static inline void quota_block_lock(quota_block_t *block, uint64_t *flags)
{
#ifdef KERNEL_TESTS
    /* Before the lock, so the common single-lock path never pays this inside
     * an IRQ-off window. The nested transfer path is the exception: its SECOND
     * acquire necessarily runs with the first block's lock already held, so
     * that one enter does land inside IRQ-off. Bounded and per-CPU, so it is a
     * couple of uncontended cycles on a line this CPU already owns. */
    quota_writers_enter();
#endif
    spin_lock_irqsave(&block->lock, flags);
}

/* Acquire WITHOUT the writer-count enter, for the N-lock adjust path.
 *
 * That path has already marked itself in-flight with a single
 * quota_writers_enter_active() before masking interrupts, so entering here would
 * both double-count the in-flight marker and put N atomic RMWs back inside the
 * IRQ-off window. It pairs with quota_block_unlock_quiet plus the epoch/section
 * settlement below the mask restore. */
static inline void quota_block_lock_quiet(quota_block_t *block, uint64_t *flags)
{
    spin_lock_irqsave(&block->lock, flags);
}

/* Release the adjust path's in-flight marker on a return that completed NO block
 * section. Deliberately does not advance the epoch: an adjust that never took a
 * lock never counted a mutation before this change either, and the totals must
 * stay identical. Expands to nothing without KERNEL_TESTS so the early-return
 * paths carry no #ifdef clutter. */
#ifdef KERNEL_TESTS
#define QUOTA_ADJUST_WRITERS_ABANDON()  quota_writers_exit_active()
#else
#define QUOTA_ADJUST_WRITERS_ABANDON()  ((void)0)
#endif

static inline void quota_block_unlock(quota_block_t *block, uint64_t flags)
{
    spin_unlock_irqrestore(&block->lock, flags);
#ifdef KERNEL_TESTS
    quota_writers_exit();
    quota_test_count_lock_section();
#endif
}

/* Unlock WITHOUT accounting, for the nested two-lock path.
 *
 * Releasing the inner lock does not re-enable interrupts: its saved flags were
 * captured after the outer acquisition had already cleared IF, so restoring
 * them leaves IF=0. Accounting there would run with the outer lock still held
 * and interrupts still masked -- exactly what moving the count to the unlock
 * side exists to avoid. The transfer path therefore releases both locks with
 * this helper and records its two sections afterwards. */
static inline void quota_block_unlock_quiet(quota_block_t *block, uint64_t flags)
{
    spin_unlock_irqrestore(&block->lock, flags);
    /* Quiet about EVERYTHING, including the writer count and epoch. The inner
     * release of a nested pair still runs with the OUTER lock held and
     * interrupts masked, so an atomic RMW here would sit inside a quota
     * critical section. The transfer path records both exits after releasing
     * both locks -- see quota_writers_exit_pair. */
}

#ifdef KERNEL_TESTS
/* Completion record for the nested two-lock path, called once both locks are
 * genuinely released and interrupts restored. */
static inline void quota_writers_exit_pair(void)
{
    quota_writers_exit();
    quota_writers_exit();
}
#endif

/* ==========================================================================
 * Rate-limit policy records (see the contract in quota.h)
 *
 * Policy is NOT usage: nothing here charges, returns, or consults a counter.
 * These functions only store and reproduce a record coherently.
 * ========================================================================== */

/* Bound on the lock-free reader's retries before it falls back to the lock.
 * A seqlock reader is only livelock-free if something stops it retrying: this
 * API is documented as callable from a scheduler with interrupts disabled,
 * where an unbounded retry is a hang rather than a slowdown. Writers hold the
 * block lock and are rare (an administrative action), so exhausting this many
 * attempts means real contention, not the common case. */
#define QUOTA_RATE_READ_TRIES  8u

/* Field accessors used by BOTH sides of the publication, so every shared field
 * is touched through an atomic access rather than a plain struct copy racing a
 * plain struct write. */
static void quota_rate_store_field(uint32_t *dst, uint32_t v)
{
    __atomic_store_n(dst, v, __ATOMIC_RELAXED);
}

static void quota_rate_store_u64(uint64_t *dst, uint64_t v)
{
    __atomic_store_n(dst, v, __ATOMIC_RELAXED);
}

/* Publish one envelope in CANONICAL form: an amount whose flag is clear stores
 * as 0. The contract says an unflagged amount is meaningless, so copying the
 * caller's value through would persist an indeterminate number, make two
 * logically identical policies differ byte-for-byte, and (once a query syscall
 * copies records outward) export uninitialized caller stack. `keep` lets the
 * CPU case blank the entire byte envelope. */
static void quota_rate_store_envelope(quota_rate_envelope_t *dst,
                                      const quota_rate_envelope_t *src,
                                      int keep)
{
    uint32_t f = keep ? src->flags : 0u;

    quota_rate_store_field(&dst->flags,    f);
    quota_rate_store_field(&dst->reserved, 0u);   /* pinned ABI hole: always 0 */
    quota_rate_store_u64(&dst->weight,
                         (f & QUOTA_RATE_F_WEIGHT)      ? src->weight      : 0ull);
    quota_rate_store_u64(&dst->reservation,
                         (f & QUOTA_RATE_F_RESERVATION) ? src->reservation : 0ull);
    quota_rate_store_u64(&dst->max,
                         (f & QUOTA_RATE_F_MAX)         ? src->max         : 0ull);
    quota_rate_store_u64(&dst->hard_cap,
                         (f & QUOTA_RATE_F_HARD_CAP)    ? src->hard_cap    : 0ull);
}

static void quota_rate_load_envelope(const quota_rate_envelope_t *src,
                                     quota_rate_envelope_t *dst)
{
    dst->flags       = __atomic_load_n(&src->flags, __ATOMIC_RELAXED);
    dst->reserved    = __atomic_load_n(&src->reserved, __ATOMIC_RELAXED);
    dst->weight      = __atomic_load_n(&src->weight, __ATOMIC_RELAXED);
    dst->reservation = __atomic_load_n(&src->reservation, __ATOMIC_RELAXED);
    dst->max         = __atomic_load_n(&src->max, __ATOMIC_RELAXED);
    dst->hard_cap    = __atomic_load_n(&src->hard_cap, __ATOMIC_RELAXED);
}

static void quota_rate_copy_out(const quota_rate_limit_t *src,
                                quota_rate_limit_t *dst)
{
    dst->version    = __atomic_load_n(&src->version, __ATOMIC_RELAXED);
    dst->reserved0  = __atomic_load_n(&src->reserved0, __ATOMIC_RELAXED);
    dst->period_ns  = __atomic_load_n(&src->period_ns, __ATOMIC_RELAXED);
    dst->generation = __atomic_load_n(&src->generation, __ATOMIC_RELAXED);
    quota_rate_load_envelope(&src->primary, &dst->primary);
    quota_rate_load_envelope(&src->bytes,   &dst->bytes);
}

/* The class->unit mapping below is an else-everything dispatch, so appending a
 * rate class that is NOT byte/op denominated (a memory-bandwidth or network
 * class, say) would silently inherit the I/O denominations with no diagnostic.
 * Pinning the count makes that a BUILD failure instead: adding a class forces
 * whoever adds it to revisit this mapping. Same rule the resource-type enum
 * uses, applied to a derived dispatch rather than to a parallel array. */
_Static_assert(QUOTA_RATE_CLASS_COUNT == 4,
    "a new rate class must revisit quota_rate_envelope_unit's denomination map");

quota_rate_unit_t quota_rate_envelope_unit(quota_rate_class_t cls, int want_bytes)
{
    if (cls == QUOTA_RATE_CLASS_CPU)
        return QUOTA_RATE_UNIT_NS;          /* bytes envelope is never published */
    return want_bytes ? QUOTA_RATE_UNIT_BYTES : QUOTA_RATE_UNIT_OPS;
}

static int quota_rate_class_valid(quota_rate_class_t cls)
{
    return (uint32_t)cls < (uint32_t)QUOTA_RATE_CLASS_COUNT;
}

/* Validate ONE envelope in isolation. The two envelopes of a record are
 * independent dimensions (operations and bytes), so their amounts are never
 * compared against each other -- 500 ops and 4 MiB per period is a perfectly
 * coherent policy, and requiring an ordering between them would be nonsense. */
static int quota_rate_envelope_valid(const quota_rate_envelope_t *e)
{
    if (e->flags & ~(uint32_t)QUOTA_RATE_F_ALL)
        return 0;
    if (e->reserved != 0u)          /* pinned ABI hole must be zero */
        return 0;

    /* Only FLAGGED amounts are meaningful, so only they are range-checked; an
     * unset field may hold anything and is never read by a consumer. */
    if ((e->flags & QUOTA_RATE_F_WEIGHT) && e->weight > QUOTA_RATE_AMOUNT_MAX)
        return 0;
    if ((e->flags & QUOTA_RATE_F_RESERVATION) && e->reservation > QUOTA_RATE_AMOUNT_MAX)
        return 0;
    if ((e->flags & QUOTA_RATE_F_MAX) && e->max > QUOTA_RATE_AMOUNT_MAX)
        return 0;
    if ((e->flags & QUOTA_RATE_F_HARD_CAP) && e->hard_cap > QUOTA_RATE_AMOUNT_MAX)
        return 0;

    /* Ordering: a floor above a ceiling can never be honored simultaneously.
     * Each pair is checked only when BOTH ends are flagged. */
    if ((e->flags & QUOTA_RATE_F_RESERVATION) && (e->flags & QUOTA_RATE_F_MAX) &&
        e->reservation > e->max)
        return 0;
    if ((e->flags & QUOTA_RATE_F_RESERVATION) && (e->flags & QUOTA_RATE_F_HARD_CAP) &&
        e->reservation > e->hard_cap)
        return 0;
    if ((e->flags & QUOTA_RATE_F_MAX) && (e->flags & QUOTA_RATE_F_HARD_CAP) &&
        e->max > e->hard_cap)
        return 0;

    return 1;
}

/* Does this envelope carry an amount that only means something per period? */
static int quota_rate_envelope_needs_period(const quota_rate_envelope_t *e)
{
    return (e->flags & (QUOTA_RATE_F_RESERVATION |
                        QUOTA_RATE_F_MAX |
                        QUOTA_RATE_F_HARD_CAP)) != 0u;
}

/* Reject any record a consumer could not act on. Validation lives here, at the
 * single publication point, so no consumer has to defend against a policy that
 * contradicts itself -- and so an unsatisfiable policy is reported to whoever
 * set it rather than silently reinterpreted by a scheduler at runtime. */
static int quota_rate_record_valid(quota_rate_class_t cls,
                                   const quota_rate_limit_t *rec)
{
    if (rec->version != QUOTA_RATE_VERSION)
        return 0;
    if (rec->reserved0 != 0u)
        return 0;

    if (!quota_rate_envelope_valid(&rec->primary))
        return 0;
    if (!quota_rate_envelope_valid(&rec->bytes))
        return 0;

    /* CPU time has no byte dimension. A byte envelope on a CPU policy is a
     * caller error, not something to store and hope the consumer ignores. */
    if (cls == QUOTA_RATE_CLASS_CPU && rec->bytes.flags != 0u)
        return 0;

    /* Every amount except a bare weight is expressed PER PERIOD, so a policy
     * carrying one without a window is meaningless: a consumer would have to
     * invent the window, and two consumers would invent different ones. */
    if ((quota_rate_envelope_needs_period(&rec->primary) ||
         quota_rate_envelope_needs_period(&rec->bytes)) &&
        rec->period_ns == 0ull)
        return 0;

    return 1;
}

NTSTATUS quota_rate_limit_set(quota_block_t *block, quota_rate_class_t cls,
                              const quota_rate_limit_t *rec)
{
    uint64_t flags;
    uint64_t next_gen;

    if (!block || !rec || !quota_rate_class_valid(cls))
        return STATUS_INVALID_PARAMETER;
    if (!quota_rate_record_valid(cls, rec))
        return STATUS_INVALID_PARAMETER;

    /* The block lock serializes WRITERS (so a class's sequence counter never
     * has two concurrent publishers); the sequence counter is what makes the
     * update atomic to lock-free READERS. */
    spin_lock_irqsave(&block->lock, &flags);

    next_gen = block->rate[cls].generation + 1ull;

    /* Odd sequence = "record in flux". The release fence keeps the counter
     * bump ahead of the field writes, so a reader that saw the even value
     * cannot also observe a half-written record. */
    __atomic_store_n(&block->rate_seq[cls], block->rate_seq[cls] + 1u,
                     __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    /* Field-wise stores through the same helper the reader loads with, so the
     * publication is expressed in atomic accesses end to end rather than as a
     * plain struct assignment racing a plain struct read. */
    quota_rate_store_field(&block->rate[cls].version,   rec->version);
    quota_rate_store_field(&block->rate[cls].reserved0, rec->reserved0);
    quota_rate_store_u64(&block->rate[cls].period_ns,   rec->period_ns);
    quota_rate_store_envelope(&block->rate[cls].primary, &rec->primary, 1);
    /* CPU has no byte dimension: blank that envelope rather than storing a
     * zero-flagged copy of whatever the caller left there. */
    quota_rate_store_envelope(&block->rate[cls].bytes,   &rec->bytes,
                              cls != QUOTA_RATE_CLASS_CPU);
    /* The generation is assigned here, never taken from the caller: it must be
     * monotonic per block so a consumer can use it to detect change. */
    quota_rate_store_u64(&block->rate[cls].generation,  next_gen);

    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&block->rate_seq[cls], block->rate_seq[cls] + 1u,
                     __ATOMIC_RELAXED);

    spin_unlock_irqrestore(&block->lock, flags);
    return STATUS_SUCCESS;
}

NTSTATUS quota_rate_limit_get(const quota_block_t *block, quota_rate_class_t cls,
                              quota_rate_limit_t *out)
{
    uint32_t before, after, tries;
    quota_rate_limit_t staged;

    if (!block || !out || !quota_rate_class_valid(cls))
        return STATUS_INVALID_PARAMETER;

    /* Seqlock read: copy, then confirm the sequence neither changed nor was
     * odd. BOUNDED -- see QUOTA_RATE_READ_TRIES. */
    for (tries = 0; tries < QUOTA_RATE_READ_TRIES; tries++) {
        before = __atomic_load_n(&block->rate_seq[cls], __ATOMIC_RELAXED);
        if (before & 1u)
            continue;                       /* write in flight; resample */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        /* Stage into a LOCAL, never straight into *out. A failed attempt reads
         * a possibly torn record, so writing it to the caller's buffer would
         * destroy the very snapshot the STATUS_RETRY contract promises to
         * leave intact -- and would hand a consumer a mixed policy on exactly
         * the path that exists to prevent one. */
        quota_rate_copy_out(&block->rate[cls], &staged);

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        after = __atomic_load_n(&block->rate_seq[cls], __ATOMIC_RELAXED);
        if (before == after) {
            *out = staged;                  /* publish only a stable snapshot */
            return STATUS_SUCCESS;
        }
    }

    /* Sustained contention with a publisher. Report it rather than retrying
     * forever OR falling back to the block lock: this function is documented as
     * callable from a scheduler holding its own lock with interrupts disabled,
     * where spinning is a hang and taking the block lock would violate the
     * no-nested-call contract in quota.h. `*out` is left UNTOUCHED, so the
     * caller simply keeps using the snapshot it already had -- a policy that is
     * one update stale is always safe; a torn or absent one is not. */
    return STATUS_RETRY;
}

#ifdef KERNEL_TESTS
/* Test-only raw counter access (see quota.h). Compiled out in production. */
void quota_test_poke_usage(quota_block_t *block, quota_resource_type_t type, int64_t value)
{
    if (block && quota_type_valid(type))
        atomic64_set(&block->counter[type].usage, value);
}

void quota_test_poke_rate_seq(quota_block_t *block, quota_rate_class_t cls, int odd)
{
    uint32_t v;

    if (!block || !quota_rate_class_valid(cls))
        return;
    v = __atomic_load_n(&block->rate_seq[cls], __ATOMIC_RELAXED);
    /* Move to the requested parity without going backwards, so a restored
     * even value never collides with a sequence a real publish already used. */
    if (((v & 1u) != 0u) != (odd != 0))
        v++;
    __atomic_store_n(&block->rate_seq[cls], v, __ATOMIC_RELAXED);
}

void quota_test_poke_failures(quota_block_t *block, quota_resource_type_t type, int64_t value)
{
    if (block && quota_type_valid(type))
        atomic64_set(&block->counter[type].failures, value);
}

int64_t quota_test_raw_usage(const quota_block_t *block, quota_resource_type_t type)
{
    if (!block || !quota_type_valid(type))
        return 0;
    return atomic64_read(&block->counter[type].usage);
}

void quota_test_lock_count_begin(void)
{
    __atomic_store_n(&g_lock_sections, 0, __ATOMIC_RELAXED);
    /* Record the caller's thread, then arm. Arming LAST means the counter can
     * never admit a section against a stale identity from a prior window. */
    __atomic_store_n(&g_lock_count_thread, thread_current(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_lock_count_on, 1, __ATOMIC_RELAXED);
}

uint64_t quota_test_lock_count_end(void)
{
    __atomic_store_n(&g_lock_count_on, 0, __ATOMIC_RELAXED);
    return __atomic_load_n(&g_lock_sections, __ATOMIC_RELAXED);
}

uint32_t quota_test_counter_record_bytes(void)
{
    return QUOTA_COUNTER_RECORD_BYTES;
}

uint32_t quota_test_counter_line_bytes(void)
{
    return QUOTA_COUNTER_LINE_BYTES;
}

uint32_t quota_test_counter_base_offset(void)
{
    return (uint32_t)__builtin_offsetof(struct quota_block, counter);
}
#endif /* KERNEL_TESTS */

/* --- Helpers, all called with the owning block's lock held --------------- */

/* Lift a high-water mark to `value` if it is higher. */
static void quota_lift_peak(atomic64_t *peak, int64_t value)
{
    if (value > atomic64_read(peak))
        atomic64_set(peak, value);
}

/* Bump a saturating diagnostic counter: it must never wrap into the negative
 * half and start reporting nonsense. */
static void quota_bump_failures(atomic64_t *failures)
{
    int64_t cur = atomic64_read(failures);
    if (cur < QUOTA_AMOUNT_MAX)
        atomic64_set(failures, cur + 1);
}

/* Diagnostics observed inside a critical section, emitted after the unlock.
 * klog takes its own lock and can reach serial, framebuffer, and disk output;
 * doing that under a quota spinlock would hold the lock across unbounded I/O
 * at DISPATCH_LEVEL, and would self-deadlock once the logging path itself
 * becomes quota-charged. So the locked helpers only RECORD what happened. */
typedef enum {
    QUOTA_DIAG_NONE = 0,
    QUOTA_DIAG_NEGATIVE_USAGE,          /* corrupted counter, failed closed   */
    QUOTA_DIAG_NEGATIVE_USAGE_SOURCE,   /* ... on a transfer's SOURCE block   */
    QUOTA_DIAG_NEGATIVE_USAGE_DEST,     /* ... on a transfer's DESTINATION    */
    QUOTA_DIAG_RETURN_UNDERFLOW         /* returned more than was charged     */
} quota_diag_t;

/* Diagnostics refused because the caller was above PASSIVE_LEVEL. Counted
 * rather than dropped silently, so a corruption storm arriving from interrupt
 * context is still visible to the dashboard. */
static uint64_t g_quota_diag_deferred;

/* Job un-absorb refusals (quota_owner.c). Counted rather than logged for the
 * same reason as the deferred diagnostics above, plus one more: the un-absorb
 * runs from the process-death teardown path, which is log-free by contract. */
static uint64_t g_quota_unabsorb_refused;

void quota_note_unabsorb_refused(void)
{
    __atomic_fetch_add(&g_quota_unabsorb_refused, 1ull, __ATOMIC_RELAXED);
}

uint64_t quota_unabsorb_refused_count(void)
{
    return __atomic_load_n(&g_quota_unabsorb_refused, __ATOMIC_RELAXED);
}

/* Returns that exhausted their bounded wait on a receipt still BUSY at the
 * returner's OWN generation (quota_owner.c). Counted, not logged, because the
 * retry runs with interrupts masked. Stale and duplicate returns are NOT counted
 * -- those are documented no-ops that leak nothing. */
static uint64_t g_quota_return_wait_exhausted;

void quota_note_return_wait_exhausted(void)
{
    __atomic_fetch_add(&g_quota_return_wait_exhausted, 1ull, __ATOMIC_RELAXED);
}

uint64_t quota_return_wait_exhausted_count(void)
{
    return __atomic_load_n(&g_quota_return_wait_exhausted, __ATOMIC_RELAXED);
}

/* Emit a recorded diagnostic. MUST be called with no quota lock held.
 *
 * Gated on PASSIVE_LEVEL: klog's live-debug path appends and FLUSHES through
 * the VFS to disk (klog_disk_append + klog_disk_flush in klog.c), which is
 * unbounded blocking I/O. This API is documented as callable at
 * DISPATCH_LEVEL and from interrupt context, so emitting there would turn a
 * recoverable accounting diagnostic into a stall -- or a deadlock if the
 * caller holds an outer lock that the storage path also needs. */
static void quota_emit_diag(quota_diag_t diag, quota_resource_type_t type)
{
    if (diag == QUOTA_DIAG_NONE)
        return;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        __atomic_fetch_add(&g_quota_diag_deferred, 1ull, __ATOMIC_RELAXED);
        return;
    }

    switch (diag) {
    case QUOTA_DIAG_NEGATIVE_USAGE:
        klog(LOG_ERROR, "quota", "charge %s: negative usage, failing closed",
             quota_resource_type_name(type));
        break;
    case QUOTA_DIAG_NEGATIVE_USAGE_SOURCE:
        klog(LOG_ERROR, "quota",
             "transfer %s: negative usage on SOURCE block, failing closed",
             quota_resource_type_name(type));
        break;
    case QUOTA_DIAG_NEGATIVE_USAGE_DEST:
        klog(LOG_ERROR, "quota",
             "transfer %s: negative usage on DESTINATION block, failing closed",
             quota_resource_type_name(type));
        break;
    case QUOTA_DIAG_RETURN_UNDERFLOW:
        klog(LOG_ERROR, "quota",
             "return %s: returning more than charged, refusing (stale bookkeeping)",
             quota_resource_type_name(type));
        break;
    case QUOTA_DIAG_NONE:
    default:
        break;
    }
}

uint64_t quota_diag_deferred_count(void)
{
    return __atomic_load_n(&g_quota_diag_deferred, __ATOMIC_RELAXED);
}

uint64_t quota_block_id(const quota_block_t *block)
{
    return block ? block->id : 0ull;
}

/* Digest a block's captured owner SID into the two fields a diagnostic record
 * carries: the RID (last sub-authority, which is what a human recognizes) and a
 * 64-bit hash of the whole SID (which distinguishes two principals that share a
 * RID under different authorities).
 *
 * A digest rather than the SID bytes: the record is a fixed-size ABI struct
 * copied into a bounded ring, and a variable-length 68-byte SID would either
 * quadruple every entry or force a truncation whose failure mode is silently
 * mis-attributing a refusal. Both fields read the block's OWN immutable capture,
 * so no lock is needed and no caller SID is touched. */
static void quota_owner_digest(const quota_block_t *block,
                               uint32_t *out_rid, uint64_t *out_hash)
{
    *out_rid  = 0;
    *out_hash = 0;

    if (!block->has_owner_sid)
        return;

    const SID *sid = (const SID *)block->owner_sid_buf;
    uint32_t   len = RtlLengthSid(sid);
    const uint8_t *bytes = (const uint8_t *)block->owner_sid_buf;

    /* FNV-1a over the captured SID. Not a security primitive -- it labels a
     * principal in a diagnostic stream, and a collision costs a consumer one
     * ambiguous label, never an authorization decision. */
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < len && i < SID_MAX_SIZE; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= 0x100000001B3ull;
    }
    *out_hash = hash;

    if (sid->SubAuthorityCount > 0 && sid->SubAuthorityCount <= SID_MAX_SUB_AUTHORITIES)
        *out_rid = sid->SubAuthority[sid->SubAuthorityCount - 1];
}

/* Report one refused charge to the pressure/event plane. MUST be called with no
 * block lock held: the event path takes its own locks and reads the current
 * task, neither of which is legal under the charge critical section.
 *
 * `current` and `limit` are SNAPSHOTS taken while the lock was still held. They
 * are not re-read here on purpose: a consumer needs the numbers that produced
 * the refusal, and by now another CPU may already have changed them. */
static void quota_report_refusal(const quota_block_t *block,
                                 quota_resource_type_t type, uint64_t requested,
                                 uint64_t current, uint64_t limit, NTSTATUS result)
{
    quota_failure_source_t src;
    struct task           *t;
    struct thread         *thr;

    src.type      = type;
    src.principal = (quota_principal_t)block->principal;
    src.block_id  = block->id;
    src.requested = requested;
    src.current   = current;
    src.limit     = limit;
    src.result    = result;
    /* The digest is computed here rather than inside the event path so it
     * describes the block at the moment of refusal; it is a bounded loop over
     * at most SID_MAX_SIZE bytes with no lock held. */
    quota_owner_digest(block, &src.owner_rid, &src.owner_sid_hash);

    /* Identity captured HERE, at the refusal, not inside the event path: that
     * path takes a lock on the way in and a reschedule across it could make the
     * record name a process that never touched this quota.
     *
     * Even here it is BEST EFFORT and says so in the record. task_current and
     * thread_current resolve through global scheduler cursors rather than
     * per-CPU state, so another CPU scheduling concurrently can still make them
     * disagree with reality. block_id and the SID digest above are the
     * authoritative identity; pid/tid are the convenience. */
    /* thread_current() bounds-checks the scheduler cursor and can return NULL
     * before the scheduler is up; task_current() does NOT -- it returns the
     * address of a table slot unconditionally, so a NULL test on it is always
     * false and would set PID_VALID even for a refusal charged against a slot
     * no thread was running. Tie the pid's validity to the THREAD's, which is
     * the one that actually knows whether a task context exists. */
    thr = thread_current();
    t   = thr ? task_current() : (struct task *)0;
    src.pid         = t ? t->pid : 0u;
    src.tid         = thr ? thr->id : 0u;
    src.attribution = QUOTA_ATTRIB_BEST_EFFORT
                    | (t ? QUOTA_ATTRIB_PID_VALID : 0u)
                    | (thr ? QUOTA_ATTRIB_TID_VALID : 0u);

    quota_pressure_note_failure(&src);
}

/* Read a type's usage and limit coherently. Caller holds the block lock, so the
 * pair cannot straddle a mutation the way two lock-free queries would. */
static void quota_snapshot_locked(const quota_block_t *block, quota_resource_type_t type,
                                  uint64_t *out_usage, uint64_t *out_limit)
{
    int64_t u = atomic64_read(&block->counter[type].usage);
    int64_t l = atomic64_read(&block->counter[type].limit);
    *out_usage = (u > 0) ? (uint64_t)u : 0ull;
    *out_limit = (l > 0) ? (uint64_t)l : 0ull;
}

/* Decide whether `add` of `type` fits, WITHOUT committing. Lock held.
 * Returns STATUS_SUCCESS and the resulting total, or the refusal status.
 * Never logs; sets *diag for the caller to emit after unlocking. */
static NTSTATUS quota_check_locked(quota_block_t *block, quota_resource_type_t type,
                                   int64_t add, int64_t *out_total, quota_diag_t *diag)
{
    int64_t cur = atomic64_read(&block->counter[type].usage);

    if (cur < 0) {
        /* Corrupted counter: fail closed rather than account against it. */
        *diag = QUOTA_DIAG_NEGATIVE_USAGE;
        return STATUS_INTEGER_OVERFLOW;
    }
    if (add > QUOTA_AMOUNT_MAX - cur)
        return STATUS_INTEGER_OVERFLOW;   /* checked BEFORE the add (UB guard) */

    int64_t next  = cur + add;
    int64_t limit = atomic64_read(&block->counter[type].limit);
    if (limit != (int64_t)QUOTA_LIMIT_UNLIMITED && next > limit)
        return STATUS_QUOTA_EXCEEDED;

    *out_total = next;
    return STATUS_SUCCESS;
}

/* Commit a checked total: store usage and lift the peak. Lock held. */
static void quota_commit_locked(quota_block_t *block, quota_resource_type_t type,
                                int64_t total)
{
    atomic64_set(&block->counter[type].usage, total);
    quota_lift_peak(&block->counter[type].peak, total);
}

/* Subtract `sub`. Lock held. Never logs; sets *diag on refusal.
 *
 * An over-return FAILS CLOSED and leaves usage untouched. Clamping to 0 (the
 * obvious alternative) is unsafe: returning more than is charged means the
 * caller is working from stale bookkeeping, and zeroing would erase OTHER
 * live charges. Concretely -- A charges 5 and returns 5, B then charges 4, and
 * a duplicate/retried cleanup for A returns 5 again: clamping resets usage
 * from 4 to 0 while B is still holding its 4, so every later charge is
 * measured against a counter that has silently lost B. Refusing keeps the
 * accounting honest and surfaces the double-return as a failure. */
static NTSTATUS quota_release_locked(quota_block_t *block, quota_resource_type_t type,
                                     int64_t sub, quota_diag_t *diag)
{
    int64_t cur = atomic64_read(&block->counter[type].usage);
    if (cur < 0) {
        *diag = QUOTA_DIAG_NEGATIVE_USAGE;
        quota_bump_failures(&block->counter[type].failures);
        return STATUS_INTEGER_OVERFLOW;
    }
    if (cur < sub) {
        *diag = QUOTA_DIAG_RETURN_UNDERFLOW;
        quota_bump_failures(&block->counter[type].failures);
        return STATUS_INTEGER_OVERFLOW;
    }
    atomic64_set(&block->counter[type].usage, cur - sub);
    return STATUS_SUCCESS;
}

/* --- Canonical USER-block registry --------------------------------------- *
 * ONLY QUOTA_PRINCIPAL_USER blocks are listed here, because only they must be
 * FINDABLE by SID: a process or job block is reachable solely through the
 * owner that holds it, and nothing ever looks one up.
 *
 * That restriction is a security property, not tidiness. Listing every block
 * would make this list grow with attacker-controlled objects -- NtCreateJobObject
 * needs no privilege and the handle ceiling allows thousands of live jobs -- and
 * every lookup walks it with interrupts disabled. Bounding the list to one entry
 * per distinct owner SID makes the walk independent of how many objects a caller
 * creates. The lock protects ONLY the list linkage.
 *
 * Lock order is absolute: g_registry_lock may be taken while holding nothing,
 * and NO block's `lock` may be acquired while it is held. A registry walk
 * therefore reads counters through their lock-free atomic loads only. Taking a
 * block lock under the registry lock would put the registry INSIDE the charge
 * path's lock ordering and deadlock a CPU charging a block another CPU is
 * enumerating. */
static struct quota_block *g_registry_head;
static spinlock_t          g_registry_lock = SPINLOCK_INIT;

/* Monotonic LINK/UNLINK counter, mutated only under g_registry_lock. It exists
 * so a walker can tell "the list did not move under me" from "it did": the
 * leak sweep compares two readings taken across a walk and marks the snapshot
 * indeterminate when they differ, rather than reporting a membership change it
 * never observed coherently as a leak. Read through an atomic load because
 * readers sample it OUTSIDE the lock, by design -- taking the lock to learn
 * whether the lock was taken would defeat the purpose. */
static uint64_t            g_registry_gen;

/* Source of the never-reused block identity carried in diagnostic records. */
static uint64_t            g_block_id_next;

/* EFFECTIVE per-type default for USER blocks: the value a new USER block is
 * seeded with and the value the re-limit walk applies. Cached as plain atomics
 * rather than read from the tunable registry at each use for one specific
 * reason -- seeding must be doable while holding g_registry_lock, and reaching
 * into another subsystem's lock from under this one would invent a lock order
 * that does not exist today.
 *
 * Seeded from the taxonomy by quota_register_types and re-published by the
 * config layer. The publish-then-walk ordering in quota_user_default_relimit
 * is what closes the seed/publish race: see the comment there. */
static atomic64_t g_user_default[QUOTA_RESOURCE_TYPE_COUNT];

/* Apply the current effective defaults to a block that is NOT YET PUBLISHED.
 *
 * Deliberately takes no block lock, and must not: the caller holds
 * g_registry_lock, under which acquiring a block lock is forbidden. That is
 * sound only because the block is unreachable -- it is not on the registry
 * list and no other CPU has ever seen its address -- so there is no concurrent
 * reader or writer to exclude. Calling this on a PUBLISHED block would be a
 * bug, not merely a style violation. */
static void quota_seed_user_limits_unpublished(quota_block_t *block)
{
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        if (block->limit_explicit[i])
            continue;   /* an explicit limit outranks the config default */
        atomic64_set(&block->counter[i].limit,
                     atomic64_read(&g_user_default[i]));
    }
}

void quota_user_default_publish(quota_resource_type_t type, uint64_t limit)
{
    if ((uint32_t)type >= QUOTA_RESOURCE_TYPE_COUNT)
        return;
    if (limit > (uint64_t)QUOTA_AMOUNT_MAX)
        limit = (uint64_t)QUOTA_AMOUNT_MAX;
    atomic64_set(&g_user_default[type], (int64_t)limit);
}

uint64_t quota_user_default_current(quota_resource_type_t type)
{
    if ((uint32_t)type >= QUOTA_RESOURCE_TYPE_COUNT)
        return QUOTA_LIMIT_UNLIMITED;
    int64_t v = atomic64_read(&g_user_default[type]);
    return (v > 0) ? (uint64_t)v : 0;
}

/* Link a fully constructed block. Caller HOLDS the registry lock: the
 * canonical-user-block path must scan and link in one critical section, or two
 * CPUs racing on a first-ever SID could each publish a block and split that
 * user's budget in half. */
static void quota_registry_link_locked(quota_block_t *block)
{
    block->reg_prev = (struct quota_block *)0;
    block->reg_next = g_registry_head;
    if (g_registry_head)
        g_registry_head->reg_prev = block;
    g_registry_head = block;
    block->registered = 1;
    __atomic_fetch_add(&g_registry_gen, 1, __ATOMIC_RELAXED);
}

uint64_t quota_registry_generation(void)
{
    return __atomic_load_n(&g_registry_gen, __ATOMIC_RELAXED);
}

/* Unlink a block whose refcount has already reached zero. Splitting this from
 * the kfree that follows is what makes teardown safe against a concurrent
 * lookup: the finder holds the registry lock, so it either completes its
 * try-ref (which fails, the count being zero) before this unlink, or never
 * sees the block at all. The block is therefore never freed while a walker
 * still holds a pointer to it. */
static void quota_registry_remove(quota_block_t *block)
{
    uint64_t flags;
    spin_lock_irqsave(&g_registry_lock, &flags);
    if (!block->registered) {
        spin_unlock_irqrestore(&g_registry_lock, flags);
        return;                     /* process/job block: never listed */
    }
    block->registered = 0;
    if (block->reg_prev)
        block->reg_prev->reg_next = block->reg_next;
    else if (g_registry_head == block)
        g_registry_head = block->reg_next;
    if (block->reg_next)
        block->reg_next->reg_prev = block->reg_prev;
    block->reg_next = (struct quota_block *)0;
    block->reg_prev = (struct quota_block *)0;
    __atomic_fetch_add(&g_registry_gen, 1, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&g_registry_lock, flags);
}

/* Compare a block's captured owner against a caller SID of known length. The
 * block's copy was validated at capture, so only the caller side needs the
 * bounded length check. */
static int quota_block_owner_is(const quota_block_t *block,
                                const SID *owner, uint32_t owner_len)
{
    if (!block->has_owner_sid || !owner)
        return 0;
    const SID *mine = (const SID *)block->owner_sid_buf;
    uint32_t   len  = RtlLengthSidBounded(owner, owner_len);
    if (len == 0 || len != RtlLengthSid(mine))
        return 0;
    return RtlEqualSid(mine, owner) ? 1 : 0;
}

/* Build a block WITHOUT publishing it to the registry. Split out so the
 * canonical-user path can allocate outside the registry lock (allocation and
 * SID validation are far too much work to hold it for) and then decide, inside
 * one critical section, whether to publish this block or adopt the one a
 * racing CPU published first. An unpublished block is unreachable by any other
 * CPU, so it is discarded with a plain kfree rather than a deref. */
static quota_block_t *quota_block_alloc(quota_principal_t principal,
                                        const SID *owner, uint32_t owner_len)
{
    uint32_t sid_len = 0;

    if ((unsigned)principal >= (unsigned)QUOTA_PRINCIPAL_COUNT) {
        klog(LOG_WARN, "quota", "block create: invalid principal %u",
             (uint64_t)principal);
        return NULL;
    }

    if (owner) {
        /* Validate against the length the CALLER guarantees is readable, never
         * against SID_MAX_SIZE: a well-formed-looking header (Revision 1,
         * SubAuthorityCount 15) at the end of a mapping would otherwise be
         * accepted as 68 bytes and copied straight across the boundary. */
        if (owner_len > SID_MAX_SIZE)
            owner_len = SID_MAX_SIZE;
        sid_len = RtlLengthSidBounded(owner, owner_len);
        if (sid_len == 0) {
            klog(LOG_WARN, "quota", "block create: malformed or truncated owner SID");
            return NULL;
        }
    }

    quota_block_t *block = (quota_block_t *)kmalloc_zeroed(sizeof(quota_block_t));
    if (!block) {
        klog(LOG_ERROR, "quota", "block create: out of memory");
        return NULL;
    }

    atomic_set(&block->refcount, 1);
    block->lock = (spinlock_t)SPINLOCK_INIT;
    block->principal = (uint8_t)principal;
    /* Never-reused identity. Starts at 1 so a zero id in a diagnostic record
     * reads unambiguously as "no block", not as the first one ever made. */
    block->id = __atomic_add_fetch(&g_block_id_next, 1ull, __ATOMIC_RELAXED);

    /* Seed limits so a fresh block already carries the effective default;
     * callers override per type with quota_set_limit.
     *
     * Precedence (ENFORCED here and in quota_set_limit, no longer "intended"):
     * per-block explicit > kernel-config override > taxonomy default_limit.
     * The config override is consulted for USER blocks ONLY -- it expresses a
     * PER-USER cap, and the same allocator builds PROCESS and JOB blocks, so
     * applying it to those would cap a single process at the whole user's
     * budget. Before the config layer registers (early boot) the lookup falls
     * back to the taxonomy default, so ordering imposes no constraint. */
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        uint64_t def = (principal == QUOTA_PRINCIPAL_USER)
                     ? quota_user_default_current((quota_resource_type_t)i)
                     : g_quota_desc[i].default_limit;
        if (def > (uint64_t)QUOTA_AMOUNT_MAX)
            def = (uint64_t)QUOTA_AMOUNT_MAX;   /* clamp into the counter domain */
        atomic64_set(&block->counter[i].limit, (int64_t)def);
    }

    if (owner) {
        /* Copy EXACTLY the bounded length, with memcpy rather than RtlCopySid.
         * RtlCopySid re-derives the length from the source with the unbounded
         * RtlLengthSid, so a concurrent bump of SubAuthorityCount between the
         * check above and the copy would make it read 68 bytes from a source
         * only `owner_len` of which is guaranteed mapped. Capturing sid_len
         * bytes fixes the read extent at the value we already validated. */
        memcpy(block->owner_sid_buf, owner, sid_len);

        /* Validate the immutable CAPTURE before publishing it: if the source
         * was mutated mid-copy, the snapshot may be internally inconsistent.
         * From here on the block only ever reads its own buffer. */
        const SID *copy = (const SID *)block->owner_sid_buf;
        if (RtlLengthSidBounded(copy, sid_len) != sid_len) {
            klog(LOG_WARN, "quota", "block create: owner SID changed during copy");
            kfree(block);
            return NULL;
        }
        block->has_owner_sid = 1;
    }

    return block;
}

quota_block_t *quota_block_create(quota_principal_t principal,
                                  const SID *owner, uint32_t owner_len)
{
    /* Deliberately NOT registered. Process and job blocks are reached only
     * through the owner holding them, so listing them would grow the lookup
     * walk with attacker-creatable objects for no benefit. USER blocks are
     * published by quota_user_block_acquire, which is the only path that must
     * be able to find one again. */
    return quota_block_alloc(principal, owner, owner_len);
}

void quota_block_ref(quota_block_t *block)
{
    if (!block)
        return;
    atomic_inc(&block->refcount);
}

int quota_block_try_ref(quota_block_t *block)
{
    if (!block)
        return 0;
    /* CAS loop rather than a bare increment: a block whose count has already
     * reached zero is committed to teardown, and lifting it back to one would
     * hand out a reference to memory the deref path is about to free. Losing
     * the CAS only means another CPU changed the count; retry against the
     * value we just observed. */
    for (;;) {
        int32_t cur = atomic_read(&block->refcount);
        if (cur <= 0)
            return 0;
        if (atomic_cmpxchg(&block->refcount, cur, cur + 1) == cur)
            return 1;
    }
}

void quota_block_deref(quota_block_t *block)
{
    if (!block)
        return;
    if (!atomic_dec_and_test(&block->refcount))
        return;
    /* Unlink BEFORE freeing. See quota_registry_remove: this ordering, plus
     * try-ref's refusal at zero, is what keeps a concurrent registry walk from
     * ever holding a pointer to freed memory. */
    quota_registry_remove(block);
    kfree(block);
}

const SID *quota_block_owner(const quota_block_t *block)
{
    if (!block || !block->has_owner_sid)
        return NULL;
    return (const SID *)block->owner_sid_buf;
}

quota_principal_t quota_block_principal(const quota_block_t *block)
{
    if (!block)
        return QUOTA_PRINCIPAL_COUNT;
    return (quota_principal_t)block->principal;
}

quota_block_t *quota_user_block_acquire(const SID *owner, uint32_t owner_len)
{
    uint64_t       flags;
    quota_block_t *found = NULL;

    if (!owner)
        return NULL;

    /* Fast path: the SID almost always already has its canonical block. */
    spin_lock_irqsave(&g_registry_lock, &flags);
    for (quota_block_t *b = g_registry_head; b; b = b->reg_next) {
        if (b->principal == (uint8_t)QUOTA_PRINCIPAL_USER &&
            quota_block_owner_is(b, owner, owner_len) &&
            quota_block_try_ref(b)) {
            found = b;
            break;
        }
    }
    spin_unlock_irqrestore(&g_registry_lock, flags);
    if (found)
        return found;

    /* First use of this SID. Build the candidate OUTSIDE the registry lock --
     * allocation plus SID validation is far more work than belongs in a lock
     * every rollup contends -- but do NOT publish it yet. */
    quota_block_t *fresh = quota_block_alloc(QUOTA_PRINCIPAL_USER, owner, owner_len);
    if (!fresh)
        return NULL;

    /* Re-scan and publish in ONE critical section. Publishing before this
     * re-scan would be a bug with teeth: a third CPU could adopt `fresh` while
     * the loser here discards it, leaving two live canonical blocks for one
     * SID and splitting that user's budget into two independently enforced
     * halves -- the exact failure this function exists to prevent. */
    spin_lock_irqsave(&g_registry_lock, &flags);
    for (quota_block_t *b = g_registry_head; b; b = b->reg_next) {
        if (b->principal == (uint8_t)QUOTA_PRINCIPAL_USER &&
            quota_block_owner_is(b, owner, owner_len) &&
            quota_block_try_ref(b)) {
            found = b;
            break;
        }
    }
    if (!found) {
        /* RE-SEED under the registry lock, immediately before publishing.
         *
         * This closes the seed/publish race: quota_block_alloc read the
         * effective defaults outside any lock, so a concurrent re-limit could
         * have changed them since. The walk deliberately misses blocks linked
         * at the head after it passes, on the premise that a new block already
         * carries the new value -- a premise this re-seed is what makes true.
         *
         * The interleaving argument, given that a re-limit PUBLISHES the new
         * default before it starts walking: either this link completes before
         * the walk's first lock acquisition, in which case the block is at the
         * head and the walk sees it; or the link happens after, in which case
         * this re-seed ran after that publish and reads the new value. Without
         * it, a user whose block was born in that window would keep an
         * unlimited budget until the next configuration change. */
        quota_seed_user_limits_unpublished(fresh);
        quota_registry_link_locked(fresh);
    }
    spin_unlock_irqrestore(&g_registry_lock, flags);

    if (found) {
        /* Lost the race. `fresh` was never published, so no other CPU can hold
         * a pointer to it: free it directly rather than going through deref
         * (which would try to unlink a block that was never linked). */
        kfree(fresh);
        return found;
    }
    return fresh;
}

NTSTATUS quota_rollup_by_sid(const SID *owner, uint32_t owner_len,
                             quota_resource_type_t type,
                             uint64_t *usage_out, uint64_t *peak_out)
{
    uint64_t flags;
    uint64_t usage = 0;
    uint64_t peak  = 0;

    if (!owner || !quota_type_valid(type))
        return STATUS_INVALID_PARAMETER;
    if (RtlLengthSidBounded(owner, owner_len) == 0)
        return STATUS_INVALID_PARAMETER;

    spin_lock_irqsave(&g_registry_lock, &flags);
    for (quota_block_t *b = g_registry_head; b; b = b->reg_next) {
        /* USER blocks only. Summing every layer would count each chain charge
         * two or three times; and a job's owner is its creator, not its
         * members, so job blocks do not describe this SID's usage at all.
         * Counters are read with the lock-free accessors -- no block lock is
         * taken here, by the ordering rule above. */
        if (b->principal != (uint8_t)QUOTA_PRINCIPAL_USER)
            continue;
        if (!quota_block_owner_is(b, owner, owner_len))
            continue;
        uint64_t u = quota_usage(b, type);
        uint64_t p = quota_peak(b, type);
        /* Saturate rather than wrap: an aggregate that silently rolled over
         * would read as a healthy small number. */
        usage = (usage > (uint64_t)QUOTA_AMOUNT_MAX - u) ? (uint64_t)QUOTA_AMOUNT_MAX : usage + u;
        peak  = (peak  > (uint64_t)QUOTA_AMOUNT_MAX - p) ? (uint64_t)QUOTA_AMOUNT_MAX : peak  + p;
    }
    spin_unlock_irqrestore(&g_registry_lock, flags);

    if (usage_out)
        *usage_out = usage;
    if (peak_out)
        *peak_out = peak;
    return STATUS_SUCCESS;
}

void quota_registry_worst_all(uint16_t *out_permille, uint32_t count,
                              const uint8_t *wanted)
{
    uint64_t flags;

    if (!out_permille || count == 0)
        return;
    if (count > QUOTA_RESOURCE_TYPE_COUNT)
        count = QUOTA_RESOURCE_TYPE_COUNT;

    for (uint32_t t = 0; t < count; t++)
        out_permille[t] = QUOTA_PRESSURE_INVALID_PERMILLE;

    /* ONE walk answering every resource's question at once. The per-type
     * variant below walks the same list for a single type; calling it in a loop
     * would hold this lock -- with interrupts disabled -- once per type, for the
     * same set of blocks. */
    spin_lock_irqsave(&g_registry_lock, &flags);
    for (struct quota_block *b = g_registry_head; b; b = b->reg_next) {
        if (b->principal != (uint8_t)QUOTA_PRINCIPAL_USER)
            continue;

        for (uint32_t t = 0; t < count; t++) {
            /* Only the types the caller actually needs. The walk runs with the
             * registry lock held and interrupts disabled, so reading all 16
             * counters per block when one is wanted would multiply the IRQ-off
             * window for answers nobody asked for. */
            if (wanted && !wanted[t])
                continue;
            int64_t  l = atomic64_read(&b->counter[t].limit);
            int64_t  u = atomic64_read(&b->counter[t].usage);
            if (atomic64_read(&b->counter[t].limit) != l) {
                l = atomic64_read(&b->counter[t].limit);
                u = atomic64_read(&b->counter[t].usage);
            }
            uint64_t usage = (u > 0) ? (uint64_t)u : 0ull;
            uint64_t limit = (l > 0) ? (uint64_t)l : 0ull;
            uint16_t p = quota_pressure_permille(usage, limit);

            if (p == QUOTA_PRESSURE_INVALID_PERMILLE)
                continue;   /* uncapped here: no ratio, no candidacy */
            /* An UNKNOWN slot is replaced by any real reading, including 0 --
             * "the worst capped principal holds nothing" is zero pressure, not
             * absence of measurement. */
            if (out_permille[t] == QUOTA_PRESSURE_INVALID_PERMILLE ||
                p > out_permille[t])
                out_permille[t] = p;
        }
    }
    spin_unlock_irqrestore(&g_registry_lock, flags);
}

int quota_registry_worst_user(quota_resource_type_t type, uint16_t min_permille,
                              uint64_t *out_block_id, uint32_t *out_rid,
                              uint64_t *out_sid_hash, uint16_t *out_permille)
{
    uint64_t flags;
    uint16_t best = 0;
    int      found = 0;

    if (!quota_type_valid(type) || !out_block_id || !out_rid || !out_sid_hash ||
        !out_permille)
        return 0;

    *out_block_id = 0;
    *out_rid      = 0;
    *out_sid_hash = 0;
    *out_permille = 0;

    /* The registry lock covers the LINKAGE only, and the counters are read
     * through their lock-free atomic loads -- taking a block lock under this
     * one is forbidden (it would put the registry inside the charge path's
     * ordering). That restriction is exactly what makes this walk usable as an
     * escalation input: it cannot deadlock against a concurrent charge, and a
     * block cannot be freed mid-walk because the deref path unlinks under this
     * same lock before freeing.
     *
     * The list holds one entry per distinct owner SID, so the walk is bounded
     * by the number of users on the system, not by how many objects anyone
     * created. */
    spin_lock_irqsave(&g_registry_lock, &flags);
    for (struct quota_block *b = g_registry_head; b; b = b->reg_next) {
        if (b->principal != (uint8_t)QUOTA_PRINCIPAL_USER)
            continue;

        /* usage and limit are two independent lock-free loads, so a concurrent
         * set_limit between them would pair a NEW limit with an OLD usage and
         * yield a ratio that never existed. Re-read the limit and retry once if
         * it moved; a limit that changes twice inside this window is a caller
         * racing itself, and the next sample covers it. */
        int64_t  l = atomic64_read(&b->counter[type].limit);
        int64_t  u = atomic64_read(&b->counter[type].usage);
        if (atomic64_read(&b->counter[type].limit) != l) {
            l = atomic64_read(&b->counter[type].limit);
            u = atomic64_read(&b->counter[type].usage);
        }
        uint64_t usage = (u > 0) ? (uint64_t)u : 0ull;
        uint64_t limit = (l > 0) ? (uint64_t)l : 0ull;
        uint16_t p = quota_pressure_permille(usage, limit);

        /* An uncapped principal has no ratio, so it can never be "furthest
         * over" anything -- skipping it is the same honesty the VALID flag
         * expresses for a domain. */
        if (p == QUOTA_PRESSURE_INVALID_PERMILLE)
            continue;
        if (p < min_permille)
            continue;
        /* `found` gates the tie-break, not `best`. Comparing against best alone
         * would skip a capped principal sitting at exactly 0 permille, so a
         * domain whose users had all returned their charges would report NO
         * CAPPED PRINCIPAL -- i.e. UNKNOWN -- instead of "zero pressure". An
         * unknown never de-escalates a level, so the level would stay latched
         * at whatever its peak was for the rest of the boot. */
        if (found && p <= best)
            continue;

        best          = p;
        found         = 1;
        *out_block_id = b->id;
        *out_permille = p;
        quota_owner_digest(b, out_rid, out_sid_hash);
    }
    spin_unlock_irqrestore(&g_registry_lock, flags);

    return found;
}

/* --- System-wide charge attribution --------------------------------------- *
 *
 * Two tables indexed [source][type], plus one honesty counter. Both are sized
 * from the two taxonomies rather than by hand, so growing either one grows the
 * storage with it instead of silently truncating an index.
 *
 * This is the ONLY state the feature adds, and it is STATIC: 2 KiB for the whole
 * system, nothing per block. The header carries the reasoning at length; the
 * short form is that every job object owns a quota block, creating one needs no
 * privilege, and the heap is a single 2 MiB run -- so a per-block matrix would be
 * an unprivileged out-of-memory amplifier built out of telemetry.
 *
 * Updated ONCE PER OBLIGATION by the chain entry points, never once per block.
 * A chain charge bills three principals for the SAME resource, so a per-block
 * update would both count it three times and put a globally shared cache line
 * inside the hottest lock section in the kernel -- the exact cost section 15
 * spent its budget removing. One update per obligation also makes the credit
 * symmetric by construction, because the receipt already carries the source.
 *
 * NO LOCK, deliberately. A charger may hold a block lock at raised IRQL with
 * interrupts masked, so a global lock here would thread a new order through the
 * charge path; and the panic-path dashboard has to read these with no lock at
 * all. What that costs is coherence across cells, which is precisely what
 * quota.h declines to promise. */
static const char *const g_source_name[QUOTA_SOURCE_COUNT] = {
    "unattributed", "object", "ipc", "notify",
    "registry", "memory", "process", "diag",
};

_Static_assert(sizeof(g_source_name) / sizeof(g_source_name[0]) == QUOTA_SOURCE_COUNT,
    "one name per charge source: a grown taxonomy must grow this table too");

static atomic64_t g_source_usage[QUOTA_SOURCE_COUNT][QUOTA_RESOURCE_TYPE_COUNT];
static atomic64_t g_source_charges[QUOTA_SOURCE_COUNT][QUOTA_RESOURCE_TYPE_COUNT];

/* The honesty counter is a PLAIN fetch-add, not the bounded CAS the cells use,
 * and the difference is the whole point: this counter is what says the cells can
 * no longer be trusted, so an update it could DROP would let the table degrade
 * while still reporting itself healthy. A fetch-add cannot fail. Saturation is
 * not needed on 64 bits -- reaching 2^64 mismatches is unreachable, whereas
 * losing the first one to contention is not. */
static uint64_t   g_source_mismatch;

/* Pin the cost. The whole justification for a system-wide table is that its
 * size does not scale with anything a caller can create, so the size is the
 * invariant worth asserting -- a future per-principal dimension would multiply
 * this and must confront the heap arithmetic in quota.h first. */
_Static_assert(sizeof(g_source_usage) ==
                   QUOTA_SOURCE_COUNT * QUOTA_RESOURCE_TYPE_COUNT * sizeof(atomic64_t),
    "the attribution table must be exactly source x type cells, with no padding");
_Static_assert(sizeof(g_source_usage) + sizeof(g_source_charges) <= 4096,
    "system-wide attribution must stay a fixed small cost; a growth that pushes "
    "it past 4 KiB means re-deriving the storage decision, not widening this");

/* Retry ceiling for one attribution cell. Bounded because this runs on the
 * charge path, where a CPU may have interrupts masked: an unbounded retry there
 * is a hang, and the table is a diagnostic whose worst failure is one dropped
 * update that the mismatch counter then reports. */
#define QUOTA_SOURCE_CAS_TRIES  8u

/* Move `cell` by `delta`, keeping it inside the documented 0..QUOTA_AMOUNT_MAX
 * domain. Returns how much was ACTUALLY applied, so the caller can tell an exact
 * update from a clamped one; 0 when the cell would not move or the bounded retry
 * was exhausted.
 *
 * The clamp is what makes the table safe rather than merely useful. Letting a
 * cell absorb an unmatched credit and go negative would turn it into an
 * unbounded residual accumulator, and since a single charge may be as large as
 * QUOTA_AMOUNT_MAX, the very next charge on that source would then overflow a
 * signed 64-bit counter. A clamp plus a counted mismatch cannot overflow and
 * cannot lie about which direction the books are off. */
static int64_t quota_source_apply_cell(atomic64_t *cell, int64_t delta)
{
    for (uint32_t i = 0; i < QUOTA_SOURCE_CAS_TRIES; i++) {
        int64_t raw  = atomic64_read(cell);
        /* A negative cell is outside the domain (only a memory error can
         * produce one). Re-seat the arithmetic at 0 rather than propagating it,
         * but CAS against the value actually stored so the exchange is honest. */
        int64_t base = (raw < 0) ? 0 : raw;
        int64_t want;

        if (delta >= 0)
            want = (delta > QUOTA_AMOUNT_MAX - base) ? QUOTA_AMOUNT_MAX : base + delta;
        else
            want = ((-delta) > base) ? 0 : base + delta;

        if (want == raw)
            return 0;
        if (atomic64_cmpxchg(cell, raw, want) == raw)
            return want - base;
    }
    return 0;
}

/* Mark the table degraded. Never droppable -- see g_source_mismatch. */
static void quota_source_note_mismatch(void)
{
    __atomic_fetch_add(&g_source_mismatch, 1ull, __ATOMIC_RELAXED);
}

void quota_source_note(quota_charge_source_t source, quota_resource_type_t type,
                       int64_t delta)
{
    if (!quota_source_valid(source) || !quota_type_valid(type) || delta == 0)
        return;
    /* INT64_MIN has no negation, and no legitimate amount reaches it (every
     * charge is validated against QUOTA_AMOUNT_MAX before it gets here), so this
     * is a corrupted delta rather than a large one: count it and touch nothing. */
    if (delta < -QUOTA_AMOUNT_MAX) {
        quota_source_note_mismatch();
        return;
    }

    if (quota_source_apply_cell(&g_source_usage[source][type], delta) != delta)
        quota_source_note_mismatch();
}

void quota_source_count_charge(quota_charge_source_t source,
                               quota_resource_type_t type)
{
    atomic64_t *cell;

    if (!quota_source_valid(source) || !quota_type_valid(type))
        return;

    /* EXHAUSTION COUNTS AS A MISMATCH HERE TOO: a count that silently skipped a
     * charge is the same quiet drift the counter exists to disclose. Saturation at
     * the ceiling is NOT a mismatch -- the counter is documented as saturating, so
     * stopping there is the contract rather than a lost update. Read first so the
     * two are told apart; a cell that reaches the ceiling between the read and the
     * update is reported as a mismatch, which is the truthful direction (something
     * did not apply exactly) rather than the convenient one. */
    cell = &g_source_charges[source][type];
    if (atomic64_read(cell) < QUOTA_AMOUNT_MAX &&
        quota_source_apply_cell(cell, 1) != 1)
        quota_source_note_mismatch();
}

const char *quota_source_name(quota_charge_source_t source)
{
    return quota_source_valid(source) ? g_source_name[source] : "?";
}

int64_t quota_source_usage(quota_charge_source_t source, quota_resource_type_t type)
{
    if (!quota_source_valid(source) || !quota_type_valid(type))
        return 0;
    int64_t v = atomic64_read(&g_source_usage[source][type]);
    return (v > 0) ? v : 0;
}

int64_t quota_source_charges(quota_charge_source_t source, quota_resource_type_t type)
{
    if (!quota_source_valid(source) || !quota_type_valid(type))
        return 0;
    int64_t v = atomic64_read(&g_source_charges[source][type]);
    return (v > 0) ? v : 0;
}

uint64_t quota_source_mismatch(void)
{
    return __atomic_load_n(&g_source_mismatch, __ATOMIC_RELAXED);
}

#ifdef KERNEL_TESTS
/* Test-only withdrawal (see quota.h). Compiled out in production. Deliberately the
 * ONLY way the counter can move backwards: a test that provokes a mismatch has to
 * be able to put the image back, but nothing in the live path may.
 *
 * A bounded CAS on a DELTA rather than a store, so an increment another CPU makes
 * during cleanup survives -- a store would erase it, and the caller's own
 * post-cleanup assertion would then pass because the evidence was destroyed. */
void quota_test_withdraw_source_mismatch(uint64_t count)
{
    for (uint32_t i = 0; i < QUOTA_SOURCE_CAS_TRIES; i++) {
        uint64_t cur  = __atomic_load_n(&g_source_mismatch, __ATOMIC_RELAXED);
        uint64_t want = (count > cur) ? 0ull : (cur - count);

        if (__atomic_compare_exchange_n(&g_source_mismatch, &cur, want, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}
#endif

void quota_source_snapshot(int64_t out[QUOTA_SOURCE_COUNT][QUOTA_RESOURCE_TYPE_COUNT])
{
    if (!out)
        return;
    for (uint32_t s = 0; s < QUOTA_SOURCE_COUNT; s++) {
        for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++) {
            int64_t v = atomic64_read(&g_source_usage[s][t]);
            out[s][t] = (v > 0) ? v : 0;
        }
    }
}

/* Print the attribution breakdown: for each type any subsystem holds, one row
 * per contributing source. Lock-free throughout, so it is legal from both the
 * live dashboard and the panic-path dump. Types nobody holds are skipped, and a
 * nonzero mismatch is reported FIRST -- it is the flag that says the rows below
 * no longer add up, and a reader who missed that would trust a broken split. */
static void quota_dump_sources(void)
{
    uint64_t mismatch = quota_source_mismatch();

    if (mismatch != 0)
        klog_unrated(LOG_WARN, "quota",
             "  attribution: %llu unmatched update(s) -- the split below is approximate",
             mismatch);

    for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++) {
        int nonempty = 0;

        /* A PREDICATE, not a sum. Every cell may legally hold QUOTA_AMOUNT_MAX
         * (== INT64_MAX), so adding two of them overflows a signed 64-bit
         * accumulator -- undefined behavior, in the live dashboard and the
         * crash-time dump both. Nothing here needs the total: the only question
         * is whether this type has anything worth printing. */
        for (uint32_t s = 0; s < QUOTA_SOURCE_COUNT && !nonempty; s++) {
            if (quota_source_usage((quota_charge_source_t)s,
                                   (quota_resource_type_t)t) != 0 ||
                quota_source_charges((quota_charge_source_t)s,
                                     (quota_resource_type_t)t) != 0)
                nonempty = 1;
        }
        if (!nonempty)
            continue;

        for (uint32_t s = 0; s < QUOTA_SOURCE_COUNT; s++) {
            int64_t held = quota_source_usage((quota_charge_source_t)s,
                                              (quota_resource_type_t)t);
            int64_t made = quota_source_charges((quota_charge_source_t)s,
                                                (quota_resource_type_t)t);

            if (held == 0 && made == 0)
                continue;
            klog_unrated(LOG_INFO, "quota", "  %s by %s: held %llu  charges %llu",
                 quota_resource_type_name((quota_resource_type_t)t),
                 quota_source_name((quota_charge_source_t)s),
                 (uint64_t)held, (uint64_t)made);
        }
    }
}

NTSTATUS quota_charge(quota_block_t *block, quota_resource_type_t type, uint64_t amount)
{
    if (!block || !quota_type_valid(type) || amount > (uint64_t)QUOTA_AMOUNT_MAX)
        return STATUS_INVALID_PARAMETER;
    if (amount == 0)
        return STATUS_SUCCESS;              /* no-op, and never lifts the peak */

    int64_t total = 0;
    uint64_t flags;
    uint64_t usage_now = 0, limit_now = 0;
    quota_diag_t diag = QUOTA_DIAG_NONE;

    quota_block_lock(block, &flags);
    NTSTATUS status = quota_check_locked(block, type, (int64_t)amount, &total, &diag);
    if (status == STATUS_SUCCESS)
        quota_commit_locked(block, type, total);
    else
        quota_bump_failures(&block->counter[type].failures);
    /* Snapshot ONLY on the path that consumes it. The pressure machine samples
     * from the registry on its own cadence now, so a SUCCESSFUL charge has no
     * reader for these two atomic loads -- and a charge is the hottest path in
     * the kernel. Only a refusal needs the numbers as they stood. */
    if (status != STATUS_SUCCESS)
        quota_snapshot_locked(block, type, &usage_now, &limit_now);
    quota_block_unlock(block, flags);

    /* Outside the lock, and only when there is something to say: the normal
     * charge path must not pay a call into the diagnostic helper. */
    if (diag != QUOTA_DIAG_NONE)
        quota_emit_diag(diag, type);

    /* Every refusal of a well-formed call produces a record, not just the
     * quota one: the contract carries a result field precisely so a corrupted
     * counter or an arithmetic overflow is reported as itself rather than
     * vanishing into an anonymous deferred count. INVALID_PARAMETER is excluded
     * -- it means the CALL was malformed, so there is no principal state worth
     * reporting. */
    if (status == STATUS_QUOTA_EXCEEDED || status == STATUS_INTEGER_OVERFLOW)
        quota_report_refusal(block, type, amount, usage_now, limit_now, status);

    quota_pressure_note_activity(type);
    return status;
}

NTSTATUS quota_return(quota_block_t *block, quota_resource_type_t type, uint64_t amount)
{
    if (!block || !quota_type_valid(type) || amount > (uint64_t)QUOTA_AMOUNT_MAX)
        return STATUS_INVALID_PARAMETER;
    if (amount == 0)
        return STATUS_SUCCESS;

    uint64_t flags;
    uint64_t usage_now = 0, limit_now = 0;
    quota_diag_t diag = QUOTA_DIAG_NONE;

    quota_block_lock(block, &flags);
    NTSTATUS status = quota_release_locked(block, type, (int64_t)amount, &diag);
    quota_block_unlock(block, flags);
    (void)usage_now; (void)limit_now;

    /* Logging happens OUTSIDE the lock, and only when there is something to
     * say -- the normal path must not pay a call into the diagnostic helper. */
    if (diag != QUOTA_DIAG_NONE)
        quota_emit_diag(diag, type);

    /* A RETURN is the operation that lowers saturation, so this is the trigger
     * that lets a level fall back down. Sampling only on charges would leave a
     * level latched at whatever the peak was. */
    quota_pressure_note_activity(type);
    return status;
}

NTSTATUS quota_try_transfer(quota_block_t *src, quota_block_t *dst,
                            quota_resource_type_t type, uint64_t amount)
{
    if (!src || !dst || !quota_type_valid(type) || amount > (uint64_t)QUOTA_AMOUNT_MAX)
        return STATUS_INVALID_PARAMETER;
    if (amount == 0 || src == dst)
        return STATUS_SUCCESS;

    /* Hold BOTH blocks for the whole move, so the debit and the credit are one
     * indivisible step: no observer can see the amount in both blocks or in
     * neither, and no concurrent charge/return can consume the interim state.
     *
     * Lock rank is the block's integer ADDRESS, low first, so two transfers in
     * opposite directions between the same pair acquire in the same order and
     * cannot deadlock. The addresses are converted to uintptr_t before being
     * compared: a relational comparison of pointers into two SEPARATE objects
     * is undefined behavior in C, so the ordering must be established on the
     * integer values, not on the pointers. src == dst was handled above, so
     * the two addresses are always distinct here. */
    int64_t move = (int64_t)amount;
    uintptr_t src_addr = (uintptr_t)src;
    uintptr_t dst_addr = (uintptr_t)dst;
    quota_block_t *first  = (src_addr < dst_addr) ? src : dst;
    quota_block_t *second = (src_addr < dst_addr) ? dst : src;
    uint64_t flags_first, flags_second;
    uint64_t src_usage = 0, src_limit = 0, dst_usage = 0, dst_limit = 0;
    quota_block_t *refused_block = (quota_block_t *)0;
    quota_diag_t diag = QUOTA_DIAG_NONE;

    quota_block_lock(first, &flags_first);
    quota_block_lock(second, &flags_second);

    NTSTATUS status;
    int64_t src_cur = atomic64_read(&src->counter[type].usage);
    if (src_cur < 0) {
        /* Corrupted source: distinct from an ordinary short balance. Reporting
         * this as QUOTA_EXCEEDED would hide an accounting-integrity failure
         * behind a routine-looking refusal, so it fails closed and is counted
         * and logged exactly like a corrupted destination. */
        diag = QUOTA_DIAG_NEGATIVE_USAGE_SOURCE;
        quota_bump_failures(&src->counter[type].failures);
        status = STATUS_INTEGER_OVERFLOW;
        refused_block = src;
    } else if (src_cur < move) {
        /* Counted like every other refusal so the per-type failure telemetry
         * sees the most common transfer failure mode, not just the rare ones.
         * The refusal is attributed to SRC: it is the block that came up short,
         * and reporting dst would name a principal that was never asked for
         * anything it could not give. */
        quota_bump_failures(&src->counter[type].failures);
        status = STATUS_QUOTA_EXCEEDED;      /* src does not hold that much */
        refused_block = src;
    } else {
        int64_t dst_total = 0;
        status = quota_check_locked(dst, type, move, &dst_total, &diag);
        if (status == STATUS_SUCCESS) {
            quota_commit_locked(dst, type, dst_total);
            atomic64_set(&src->counter[type].usage, src_cur - move);
        } else {
            quota_bump_failures(&dst->counter[type].failures);
            /* Re-label: quota_check_locked cannot know it was called for a
             * transfer destination, so a corrupted dst would otherwise be
             * logged as an ordinary charge and be indistinguishable from one. */
            if (diag == QUOTA_DIAG_NEGATIVE_USAGE)
                diag = QUOTA_DIAG_NEGATIVE_USAGE_DEST;
            if (status == STATUS_QUOTA_EXCEEDED || status == STATUS_INTEGER_OVERFLOW)
                refused_block = dst;
        }
    }

    /* Only the refusing side's numbers are consumed (by the failure event);
     * the pressure sample comes from the registry on its own cadence. */
    if (refused_block)
        quota_snapshot_locked(refused_block, type,
                              (refused_block == dst) ? &dst_usage : &src_usage,
                              (refused_block == dst) ? &dst_limit : &src_limit);

    quota_block_unlock_quiet(second, flags_second);
    quota_block_unlock_quiet(first, flags_first);
#ifdef KERNEL_TESTS
    /* Both sections recorded here, with interrupts genuinely restored. The
     * writer-count exits land here too, so the transfer counts as IN PROGRESS
     * for its whole two-block window -- which is the window a leak snapshot
     * must refuse to read, since the amount is briefly in both blocks. */
    quota_writers_exit_pair();
    quota_test_count_lock_section();
    quota_test_count_lock_section();
#endif

    if (diag != QUOTA_DIAG_NONE)
        quota_emit_diag(diag, type);     /* outside BOTH locks, only if needed */

    if (refused_block) {
        int to_dst = (refused_block == dst);
        quota_report_refusal(refused_block, type, amount,
                             to_dst ? dst_usage : src_usage,
                             to_dst ? dst_limit : src_limit, status);
    }

    quota_pressure_note_activity(type);
    return status;
}

/* --- Transactional multi-block adjust (see the contract in quota.h) ------- */

/* Collect the receipt's blocks into `sorted`, deduplicated and in ascending
 * address order. Returns the count.
 *
 * The ORDER is what prevents two adjusts over overlapping chains from
 * deadlocking, and is established on uintptr_t values rather than on the
 * pointers, because a relational comparison of pointers into two separate
 * objects is undefined behavior in C -- the same reasoning quota_try_transfer
 * records for its pair.
 *
 * The DEDUPLICATION is not defensive tidiness: acquiring the same non-recursive
 * spinlock twice is an instant self-deadlock, and it would also apply the delta
 * to that block twice. Nothing constructs such a receipt today; a receipt that
 * ever names one block twice must not be able to hang the CPU that adjusts it. */
static uint32_t quota_adjust_sort_blocks(const quota_charge_receipt_t *receipt,
                                         quota_block_t **sorted)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < receipt->count && i < QUOTA_CHAIN_MAX; i++) {
        quota_block_t *b = receipt->blocks[i];
        if (!b)
            continue;

        /* Insertion sort with a duplicate check: the array is at most
         * QUOTA_CHAIN_MAX entries, so this is cheaper than anything cleverer and
         * has no allocation. */
        uint32_t pos = 0;
        int      dup = 0;
        while (pos < n) {
            if (sorted[pos] == b) {
                dup = 1;
                break;
            }
            if ((uintptr_t)sorted[pos] > (uintptr_t)b)
                break;
            pos++;
        }
        if (dup)
            continue;

        for (uint32_t j = n; j > pos; j--)
            sorted[j] = sorted[j - 1];
        sorted[pos] = b;
        n++;
    }
    return n;
}

NTSTATUS quota_charge_adjust(quota_charge_receipt_t *receipt, uint64_t token,
                             uint64_t new_amount)
{
    /* Reject a non-canonical token for the same reason quota_return_chain does:
     * QUOTA_RECEIPT_TAG shifts it past the state bits, so a fabricated token
     * with bits above the generation width would build the SAME tag as the live
     * one and win the claim. */
    if (!receipt || token == 0 || token > QUOTA_RECEIPT_GEN_MAX)
        return STATUS_INVALID_PARAMETER;
    if (new_amount > (uint64_t)QUOTA_AMOUNT_MAX)
        return STATUS_INVALID_PARAMETER;

    const int64_t active_tag = QUOTA_RECEIPT_TAG(token, QUOTA_RECEIPT_ACTIVE);
    const int64_t busy_tag   = QUOTA_RECEIPT_TAG(token, QUOTA_RECEIPT_BUSY);

    /* MASK INTERRUPTS ACROSS THE WHOLE BUSY WINDOW, from the claim below to the
     * republish at the end. Not for mutual exclusion -- the tag CAS and the block
     * locks provide that -- but to make the window invisible to this CPU's own
     * interrupt handlers.
     *
     * Without it the window is preemptible on its own CPU, and a returner running
     * in that interrupt would find the receipt BUSY at its own generation with no
     * way to make progress: the adjust it is waiting for cannot resume until the
     * interrupt returns. That is the one starvation this design cannot recover
     * from, because it deadlocks a single CPU against itself rather than merely
     * waiting on another. Masking removes it outright, leaving only cross-CPU
     * collisions, where the adjust is provably progressing under the block locks.
     *
     * The cost is honest and bounded: the window adds the block-pointer sort and
     * the tag CAS to an IRQ-off region that already spans every block critical
     * section this transaction enters. */
#ifdef KERNEL_TESTS
    /* Mark this CPU in-flight BEFORE the mask, so no instrumentation RMW lands
     * inside the window at all. The lock loop below therefore uses the quiet
     * acquire, and the epoch and section counts are settled after the restore.
     * Every early return between here and there pays the matching active exit --
     * that is the whole cost of moving the marker out. */
    quota_writers_enter_active();
#endif
    uint64_t adjust_irq = local_irq_save();

    /* Claim the charge exclusively. Losing this means the token names no live
     * charge on this receipt (wrong token, already returned, or another operation
     * owns it), and there is nothing to resize. */
    if (atomic64_cmpxchg(&receipt->tag, active_tag, busy_tag) != active_tag) {
        local_irq_restore(adjust_irq);
        QUOTA_ADJUST_WRITERS_ABANDON();
        return STATUS_INVALID_PARAMETER;
    }

    quota_resource_type_t type = receipt->type;
    uint64_t              old  = receipt->amount;
    /* Captured INSIDE the BUSY window, by value. The source is immutable for a
     * charge's life, but the receipt STORAGE is not: once this republishes,
     * another CPU may return the charge and recharge the same receipt under a
     * different source, so a read taken after the republish could attribute this
     * resize to whoever came next. */
    quota_charge_source_t source = (quota_charge_source_t)receipt->source;

    if (!quota_type_valid(type)) {
        /* A live receipt should never carry an invalid type; refuse rather than
         * index the counter array with it. */
        atomic64_set(&receipt->tag, active_tag);
        local_irq_restore(adjust_irq);
        QUOTA_ADJUST_WRITERS_ABANDON();
        return STATUS_INVALID_PARAMETER;
    }

    if (new_amount == old) {
        atomic64_set(&receipt->tag, active_tag);
        local_irq_restore(adjust_irq);
        QUOTA_ADJUST_WRITERS_ABANDON();
        return STATUS_SUCCESS;
    }

    quota_block_t *sorted[QUOTA_CHAIN_MAX];
    uint32_t       n = quota_adjust_sort_blocks(receipt, sorted);

    if (n == 0) {
        /* Nothing is charged anywhere, so the amount is bookkeeping only. */
        receipt->amount = new_amount;
        atomic64_set(&receipt->tag, active_tag);
        local_irq_restore(adjust_irq);
        QUOTA_ADJUST_WRITERS_ABANDON();
        return STATUS_SUCCESS;
    }

    const int increase = (new_amount > old);
    const int64_t delta = increase ? (int64_t)(new_amount - old)
                                   : (int64_t)(old - new_amount);

    int64_t        totals[QUOTA_CHAIN_MAX];
    uint64_t       lock_flags[QUOTA_CHAIN_MAX];
    NTSTATUS       status  = STATUS_SUCCESS;
    quota_block_t *refused = (quota_block_t *)0;
    quota_diag_t   diag    = QUOTA_DIAG_NONE;
    uint64_t       refused_usage = 0, refused_limit = 0;

    /* Every block, held simultaneously, ascending address order. Quiet acquires:
     * this path marked itself in-flight once before masking interrupts, so no
     * instrumentation RMW belongs inside the window. */
    for (uint32_t i = 0; i < n; i++)
        quota_block_lock_quiet(sorted[i], &lock_flags[i]);

    /* PREVALIDATE the whole transaction before committing any part of it. This
     * is the entire difference from a per-block walk: no usage moves and no peak
     * lifts until every block has agreed. */
    for (uint32_t i = 0; i < n; i++) {
        if (increase) {
            status = quota_check_locked(sorted[i], type, delta, &totals[i], &diag);
            if (status != STATUS_SUCCESS) {
                refused = sorted[i];
                break;
            }
        } else {
            int64_t cur = atomic64_read(&sorted[i]->counter[type].usage);
            if (cur < 0) {
                diag    = QUOTA_DIAG_NEGATIVE_USAGE;
                status  = STATUS_INTEGER_OVERFLOW;
                refused = sorted[i];
                break;
            }
            if (cur < delta) {
                /* The block does not hold what this receipt claims it charged:
                 * an accounting-integrity failure, not a routine refusal. Fail
                 * closed -- reducing to a clamped value would silently erase
                 * some other live charge's usage. */
                diag    = QUOTA_DIAG_RETURN_UNDERFLOW;
                status  = STATUS_INTEGER_OVERFLOW;
                refused = sorted[i];
                break;
            }
            totals[i] = cur - delta;
        }
    }

    if (status == STATUS_SUCCESS) {
        /* COMMIT. quota_commit_locked stores the usage and lifts the peak only
         * when the new total exceeds it, so it is correct for both directions:
         * a decrease cannot lift a peak. */
        for (uint32_t i = 0; i < n; i++)
            quota_commit_locked(sorted[i], type, totals[i]);
        receipt->amount = new_amount;
    } else if (refused) {
        /* Counted like every other refusal in this module, so per-type failure
         * telemetry sees a refused resize. This is the ONLY state a refused
         * adjust changes -- usage, peak, and the receipt are untouched. */
        quota_bump_failures(&refused->counter[type].failures);
        quota_snapshot_locked(refused, type, &refused_usage, &refused_limit);
    }

    for (uint32_t i = n; i > 0; i--)
        quota_block_unlock_quiet(sorted[i - 1], lock_flags[i - 1]);

    /* Move the attributed amount by the same signed delta the counters moved by,
     * and ONLY on the committed path -- a refused adjust moved no usage, so
     * recording one would make the breakdown disagree with the counters it exists
     * to explain.
     *
     * INSIDE THE BUSY CLAIM, BEFORE THE REPUBLISH BELOW. Republishing first makes
     * the charge returnable again, so a token-holding returner on another CPU can
     * credit back the NEW amount before this line has added the delta that
     * produced it: the credit meets a row still holding the OLD amount, the
     * clamp at 0 absorbs the difference, and this line then adds a delta nothing
     * will ever credit back -- a permanent phantom in the row, from an
     * interleaving that is entirely legal. Attribution is therefore part of the
     * publication boundary, not a step after it.
     *
     * The cost is honest and it is why the recording is SPLIT: every block lock is
     * already released, and quota_source_note touches exactly ONE cell, so this
     * adds at most QUOTA_SOURCE_CAS_TRIES compare-exchanges plus one
     * non-blocking fetch-add to an IRQ-off window that already spans the
     * block-pointer sort, every block critical section, and the tag CAS. The
     * cumulative charge-count cell is NOT touched here -- both because a resize is
     * not a new charge and because a second contended cache line with interrupts
     * off would double this bound for a statistic with no ordering requirement. */
    if (status == STATUS_SUCCESS)
        quota_source_note(source, type, increase ? delta : -delta);

    /* Republish at the SAME generation: the holder's token must keep naming this
     * charge, whether the resize was applied or refused. Interrupts are restored
     * only AFTER the republish, which is what closes the same-CPU starvation
     * window described at the claim above. */
    atomic64_set(&receipt->tag, active_tag);
    local_irq_restore(adjust_irq);

#ifdef KERNEL_TESTS
    /* Recorded once every lock is genuinely released AND interrupts are
     * genuinely restored -- which is why this sits below the restore rather than
     * beside the unlocks. The transfer path defers its pair for exactly this
     * reason: instrumentation must never run inside a quota critical section or
     * an IRQ-off window, because KERNEL_TESTS is on in the image that boots real
     * hardware and these are locked per-CPU RMWs.
     *
     * The epoch advances by n and the in-flight marker taken before the mask is
     * released, which reproduces exactly what n enter/exit pairs recorded -- the
     * difference is only WHERE the RMWs happen, never how many mutations the gate
     * and the section budget observe.
     *
     * THE ORDER OF THESE TWO CALLS IS LOAD-BEARING, not stylistic. The counters
     * were committed above and interrupts are already restored, so `active` is the
     * only thing still telling a concurrent quota_leak_snapshot that this window is
     * untrustworthy. Advancing the epoch FIRST and dropping the marker second means
     * no instant exists in which the mutations are visible, the epoch is stale, and
     * `writers_active` reads zero -- which is exactly the combination that would let
     * the sweep certify a total predating this adjust. Do not swap them.
     *
     * Consequence worth knowing: because the marker is now taken before the mask
     * and dropped after the restore, this thread may MIGRATE between the two and
     * decrement a different CPU's slot. That is benign by construction -- both
     * readers SUM every slot and the sum is correct under unsigned wrap (see the
     * migration note on quota_test_mutation_epoch). The early-return paths also
     * hold the marker briefly while mutating nothing, so a snapshot racing them
     * reports "incoherent" rather than a wrong total: the safe direction. */
    quota_writers_epoch_add(n);
    quota_writers_exit_active();
    for (uint32_t i = 0; i < n; i++)
        quota_test_count_lock_section();
#endif

    if (diag != QUOTA_DIAG_NONE)
        quota_emit_diag(diag, type);        /* outside every lock */
    if (refused)
        quota_report_refusal(refused, type,
                             increase ? new_amount : (uint64_t)delta,
                             refused_usage, refused_limit, status);

    quota_pressure_note_activity(type);
    return status;
}

uint64_t quota_usage(const quota_block_t *block, quota_resource_type_t type)
{
    if (!block || !quota_type_valid(type))
        return 0;
    int64_t v = atomic64_read(&block->counter[type].usage);
    return (v > 0) ? (uint64_t)v : 0;
}

uint64_t quota_peak(const quota_block_t *block, quota_resource_type_t type)
{
    if (!block || !quota_type_valid(type))
        return 0;
    int64_t v = atomic64_read(&block->counter[type].peak);
    return (v > 0) ? (uint64_t)v : 0;
}

uint64_t quota_failures(const quota_block_t *block, quota_resource_type_t type)
{
    if (!block || !quota_type_valid(type))
        return 0;
    int64_t v = atomic64_read(&block->counter[type].failures);
    return (v > 0) ? (uint64_t)v : 0;
}

uint64_t quota_limit(const quota_block_t *block, quota_resource_type_t type)
{
    if (!block || !quota_type_valid(type))
        return 0;
    int64_t v = atomic64_read(&block->counter[type].limit);
    return (v > 0) ? (uint64_t)v : 0;
}

NTSTATUS quota_set_limit(quota_block_t *block, quota_resource_type_t type, uint64_t limit)
{
    if (!block || !quota_type_valid(type) || limit > (uint64_t)QUOTA_AMOUNT_MAX)
        return STATUS_INVALID_PARAMETER;

    /* Taking the block lock makes the limit change atomic with respect to any
     * in-flight charge: a charge either sees the old limit and completes, or
     * sees the new one and is judged against it. There is no window where a
     * charge is admitted against a limit that no longer applies. */
    uint64_t flags;
    quota_block_lock(block, &flags);
    atomic64_set(&block->counter[type].limit, (int64_t)limit);
    /* An explicit set pins this limit against the config default: from here on
     * a change to quota.user.<type>_default leaves this block alone. Recorded
     * under the same lock as the limit so the two can never disagree. */
    block->limit_explicit[type] = 1;
    quota_block_unlock(block, flags);

    /* A limit change moves saturation without moving usage, and can move it
     * arbitrarily far in one step: raising a cap is the other way (besides a
     * return) that a latched level must be able to fall, and lowering one can
     * put a principal over its budget instantly. */
    quota_pressure_note_activity(type);
    return STATUS_SUCCESS;
}

/* ==========================================================================
 * Runtime configurable default limits (section 6)
 *
 * The config layer registers one tunable per resource type and calls in here
 * when an administrator changes one. Two properties make that a CREDIBLE cap
 * rather than a create-time cosmetic:
 *
 *   - It is scoped to USER blocks. The same allocator builds PROCESS and JOB
 *     blocks, so applying a "per-user default" to all three would silently cap
 *     every process at the user's aggregate budget.
 *   - It reaches blocks that ALREADY EXIST. The canonical USER block for a
 *     logged-in SID is created once and lives for the session, so a default
 *     that only seeded new blocks would never affect the users actually
 *     running -- an administrative control that changes nothing.
 *
 * The walk obeys the absolute lock order above (no block lock under the
 * registry lock) by PINNING a bounded batch under the registry lock, dropping
 * it, and only then calling quota_set_limit. Batching bounds the interrupts-off
 * hold time regardless of how many users exist.
 *
 * The cursor keeps its reference across the gap between batches. That is what
 * makes resuming safe: a block we hold a reference to cannot reach refcount 0,
 * so it cannot be unlinked, so its reg_next is still meaningful when we retake
 * the lock. Blocks LINKED during the walk are missed by design -- they are
 * created at the head and already carry the new default from their seed.
 * ========================================================================== */

/* Blocks pinned per registry-lock acquisition. Small enough that the
 * interrupts-off walk stays short, large enough that a system with many live
 * users does not pay a lock round-trip per block. */
#define QUOTA_RELIMIT_BATCH  16u

/* Nodes VISITED per registry-lock acquisition. Distinct from the pin budget
 * above because a block that fails try_ref (already torn down to refcount 0,
 * awaiting unlink) consumes a visit without consuming a pin -- so bounding
 * pins alone does not bound the interrupts-off scan. Set above the pin budget
 * so ordinary churn never truncates a pass. */
#define QUOTA_RELIMIT_VISIT_MAX  64u

void quota_user_default_relimit(quota_resource_type_t type, uint64_t limit)
{
    quota_block_t *cursor = (quota_block_t *)0;   /* pinned resume point */

    if (!quota_type_valid(type) || limit > (uint64_t)QUOTA_AMOUNT_MAX)
        return;

    /* PUBLISH the new default BEFORE walking. The order is load-bearing, not
     * incidental: a block being created concurrently re-seeds from this value
     * under the registry lock just before it links, so publishing first means
     * a block that the walk misses has necessarily already read the new value.
     * Walking first and publishing after would leave exactly the window this
     * ordering exists to close. */
    quota_user_default_publish(type, limit);

    for (;;) {
        quota_block_t *batch[QUOTA_RELIMIT_BATCH];
        uint32_t n = 0;
        uint64_t flags;

        spin_lock_irqsave(&g_registry_lock, &flags);
        quota_block_t *b = cursor ? cursor->reg_next : g_registry_head;
        /* Bound VISITED nodes, not just successful pins. A block whose refcount
         * already hit zero fails try_ref and does not advance `n`, so counting
         * pins alone would let a burst of concurrent teardowns stretch this
         * interrupts-off critical section arbitrarily. `last` carries the
         * resume point is batch[n-1] whenever this pass pinned anything. */
        uint32_t visited = 0;
        while (b && n < QUOTA_RELIMIT_BATCH && visited < QUOTA_RELIMIT_VISIT_MAX) {
            visited++;
            /* try_ref, not ref: a block whose count already reached zero is
             * committed to teardown and must not be lifted back to life. */
            if (b->principal == (uint8_t)QUOTA_PRINCIPAL_USER &&
                quota_block_try_ref(b))
                batch[n++] = b;
            b = b->reg_next;
        }
        /* Did the visit budget (rather than the list end) stop this pass? If so
         * the walk is NOT finished, even when this pass pinned a short batch or
         * nothing at all -- resume from the last block we could pin. */
        int visit_capped = (b != (quota_block_t *)0);
        spin_unlock_irqrestore(&g_registry_lock, flags);

        /* Release the previous cursor only after the new batch is pinned, so
         * the list position we resumed from stayed valid for the whole walk. */
        if (cursor)
            quota_block_deref(cursor);
        cursor = (quota_block_t *)0;

        if (n == 0) {
            /* Nothing pinnable. If the visit budget stopped us there are more
             * nodes we never reached, but with no pinned block there is no safe
             * resume point -- restarting from the head would re-walk forever.
             * Every skipped block failed try_ref, i.e. is already committed to
             * teardown and about to be unlinked, so stopping loses no live
             * limit; log it because a persistent occurrence is pathological. */
            if (visit_capped)
                klog(LOG_WARN, "quota",
                     "re-limit stopped early: %u nodes visited, none pinnable",
                     (uint64_t)visited);
            break;
        }

        for (uint32_t i = 0; i < n; i++) {
            uint64_t bflags;
            quota_block_lock(batch[i], &bflags);
            /* Re-test provenance UNDER the block lock. Between the pin and
             * here another CPU may have set an explicit limit on this block;
             * overwriting it would be exactly the trampling the provenance
             * flag exists to prevent. Lowering below current usage is allowed
             * and leaves usage untouched -- subsequent charges are refused,
             * which is the documented contract, not a counter rewrite. */
            if (!batch[i]->limit_explicit[type])
                atomic64_set(&batch[i]->counter[type].limit, (int64_t)limit);
            quota_block_unlock(batch[i], bflags);
        }

        /* Keep the LAST entry referenced as the next resume point; release the
         * rest. A short batch means the list ENDED -- but only when the visit
         * budget was not what stopped us; otherwise there is more list to walk
         * and this pass just could not pin a full batch of it. */
        for (uint32_t i = 0; i + 1 < n; i++)
            quota_block_deref(batch[i]);
        if (!visit_capped && n < QUOTA_RELIMIT_BATCH) {
            quota_block_deref(batch[n - 1]);
            break;
        }
        cursor = batch[n - 1];
    }

    /* An administrator lowering a default can put every live user over budget
     * at once without a single charge being made. Nothing else on this path
     * touches the charge seams, so without this the new saturation would go
     * unobserved until the next unrelated mutation. Forced, for the same reason
     * quota_set_limit forces: a limit change can move saturation arbitrarily
     * far in one step. */
    quota_pressure_note_activity(type);
}

/* ========================================================================== *
 * Dashboards and the leak sweep
 *
 * Everything here walks the USER registry with the same batched pin pattern as
 * the re-limit walk above -- pin a bounded batch under the registry lock, drop
 * the lock, act, resume from the last pinned block -- because it obeys the same
 * absolute rule: no block lock, and no serial output, while the registry lock
 * is held. The only exception is quota_dump_crash, which does not walk at all.
 *
 * Output goes through klog_unrated, NOT klog. The per-subsystem rate limiter
 * would drop a dashboard's rows precisely when the quota subsystem is busy --
 * which is exactly when an operator runs it, and always in the crash case. A
 * dashboard that silently renders nothing under load is not a dashboard. This
 * is the documented use for the unrated path (klog.h): the caller applies its
 * own bound, and here the output is structurally bounded by QUOTA_DUMP_BATCH,
 * QUOTA_DUMP_VISIT_MAX, and QUOTA_DUMP_CRASH_ROWS rather than by a limiter.
 * ========================================================================== */

/* Blocks pinned per registry-lock acquisition, and nodes VISITED per
 * acquisition. Separate budgets for the same reason the re-limit walk keeps
 * them separate: a block already at refcount 0 consumes a visit without
 * consuming a pin, so bounding pins alone does not bound the interrupts-off
 * scan. */
#define QUOTA_DUMP_BATCH      8u
#define QUOTA_DUMP_VISIT_MAX  64u

/* Blocks rendered by ONE quota_dump() invocation, across all batches.
 *
 * QUOTA_DUMP_VISIT_MAX bounds a single registry-lock acquisition and resets
 * every iteration, so it does NOT bound the dump: the walk resumes until the
 * registry ends. Without a whole-invocation cap a system with many live users
 * could emit a header plus up to one row per resource type per user, overrun
 * the 1000-entry klog ring, and destroy the very diagnostics the dashboard
 * exists to preserve. This cap is also what makes bypassing the shared rate
 * limiter legitimate for this function -- the output is bounded by
 * construction rather than by the limiter. A truncation is reported. */
#define QUOTA_DUMP_MAX_BLOCKS 32u

/* Rows the panic-path dump copies out. Bounded and statically allocated: the
 * crash path may not allocate, and a large automatic array would be a stack
 * hazard in a context whose stack is already suspect. 8 principals is enough
 * to characterize which user exhausted what; a truncation is reported rather
 * than silently dropped. */
#define QUOTA_DUMP_CRASH_ROWS 8u

typedef struct {
    uint64_t id;
    uint32_t owner_rid;
    uint64_t owner_hash;
    uint8_t  has_owner;
    int64_t  usage[QUOTA_RESOURCE_TYPE_COUNT];
    int64_t  limit[QUOTA_RESOURCE_TYPE_COUNT];
} quota_crash_row_t;

/* Written ONLY by quota_dump_crash, which has exactly one call site and runs
 * only after panic_try_claim_owner() succeeded (panic.c) -- that claim, not any
 * broadcast parking of healthy CPUs, is what makes this single-writer. panic.c
 * has no halt-IPI or NMI broadcast; a second CPU that panics parks itself.
 * Do NOT add unsynchronized panic-path state beside this on the strength of
 * 'every other CPU has parked' -- that is not what the code does.
 * BSS rather than stack for the reason above. */
static quota_crash_row_t g_crash_rows[QUOTA_DUMP_CRASH_ROWS];

/* Times the try-lock failed and the dump took its header-only fallback. Both
 * the panic path and a contention test are otherwise unable to distinguish a
 * dump that reported rows from one that skipped them -- both just return. */
static uint64_t g_crash_fallbacks;

/* Emit one block's non-idle types. Called with NO lock held: counters are
 * lock-free atomic loads and the header is explicit that two counters read in
 * succession may straddle an update, which is the correct trade for a
 * diagnostic that must not serialize the charge path.
 *
 * A negative counter cannot occur in the documented domain (0..AMOUNT_MAX), so
 * one is reported as CORRUPT rather than rendered as a plausible number -- a
 * dashboard that prints 18446744073709551615 for a corrupted counter hides
 * exactly the failure an operator opened it to find. */
static void quota_dump_block(const quota_block_t *block)
{
    uint32_t rid  = 0;
    uint64_t hash = 0;
    quota_owner_digest(block, &rid, &hash);

    klog_unrated(LOG_INFO, "quota", "  block #%llu  user rid %u  sid %llx",
         block->id, (uint64_t)rid, hash);

    for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++) {
        int64_t usage = atomic64_read(&block->counter[t].usage);
        int64_t peak  = atomic64_read(&block->counter[t].peak);
        int64_t fails = atomic64_read(&block->counter[t].failures);
        int64_t limit = atomic64_read(&block->counter[t].limit);

        if (usage < 0 || peak < 0 || fails < 0 || limit < 0) {
            klog_unrated(LOG_ERROR, "quota", "    %s CORRUPT (negative counter)",
                 quota_resource_type_name((quota_resource_type_t)t));
            continue;
        }
        if (usage == 0 && peak == 0 && fails == 0)
            continue;               /* idle type: not worth a dashboard row */

        if (limit == (int64_t)QUOTA_LIMIT_UNLIMITED)
            klog_unrated(LOG_INFO, "quota",
                 "    %s usage %llu  peak %llu  limit -  fail %llu",
                 quota_resource_type_name((quota_resource_type_t)t),
                 (uint64_t)usage, (uint64_t)peak, (uint64_t)fails);
        else
            klog_unrated(LOG_INFO, "quota",
                 "    %s usage %llu  peak %llu  limit %llu  fail %llu",
                 quota_resource_type_name((quota_resource_type_t)t),
                 (uint64_t)usage, (uint64_t)peak, (uint64_t)limit,
                 (uint64_t)fails);
    }
}

void quota_dump(void)
{
    quota_block_t *cursor = (quota_block_t *)0;
    uint32_t       blocks = 0;
    int            truncated = 0;

    if (!quota_registry_ready()) {
        klog_unrated(LOG_INFO, "quota", "dashboard: registry not validated yet");
        return;
    }

    klog_unrated(LOG_INFO, "quota", "--- quota dashboard (USER blocks, gen %llu) ---",
         quota_registry_generation());

    for (;;) {
        quota_block_t *batch[QUOTA_DUMP_BATCH];
        uint32_t n = 0, visited = 0;
        uint64_t flags;

        spin_lock_irqsave(&g_registry_lock, &flags);
        quota_block_t *b = cursor ? cursor->reg_next : g_registry_head;
        while (b && n < QUOTA_DUMP_BATCH && visited < QUOTA_DUMP_VISIT_MAX) {
            visited++;
            /* try_ref, never ref: a block already at zero is committed to
             * teardown and must not be lifted back to life by a dashboard. */
            if (b->principal == (uint8_t)QUOTA_PRINCIPAL_USER &&
                quota_block_try_ref(b))
                batch[n++] = b;
            b = b->reg_next;
        }
        int visit_capped = (b != (quota_block_t *)0);
        spin_unlock_irqrestore(&g_registry_lock, flags);

        /* Release the old cursor only after the new batch is pinned, so the
         * resume position stayed valid across the gap. */
        if (cursor)
            quota_block_deref(cursor);
        cursor = (quota_block_t *)0;

        if (n == 0) {
            if (visit_capped)
                klog_unrated(LOG_WARN, "quota",
                     "dashboard truncated: %u nodes visited, none pinnable",
                     (uint64_t)visited);
            break;
        }

        for (uint32_t i = 0; i < n; i++) {
            if (blocks >= QUOTA_DUMP_MAX_BLOCKS) {
                truncated = 1;
                break;
            }
            quota_dump_block(batch[i]);
            blocks++;
        }

        /* Release the whole batch and stop: the cap is a whole-invocation
         * bound, so there is no resume point to keep pinned. */
        if (truncated) {
            for (uint32_t i = 0; i < n; i++)
                quota_block_deref(batch[i]);
            break;
        }

        for (uint32_t i = 0; i + 1 < n; i++)
            quota_block_deref(batch[i]);
        if (!visit_capped && n < QUOTA_DUMP_BATCH) {
            quota_block_deref(batch[n - 1]);
            break;
        }
        cursor = batch[n - 1];
    }

    if (truncated)
        klog_unrated(LOG_WARN, "quota",
             "dashboard truncated at %u blocks (whole-invocation cap)",
             (uint64_t)blocks);

    klog_unrated(LOG_INFO, "quota", "--- %u USER block(s); PROCESS/JOB blocks are not "
         "enumerable (no lifetime-safe task iterator) ---", (uint64_t)blocks);

    /* The per-block rows above answer "which principal holds it"; this answers
     * "which subsystem asked for it". It is printed even though PROCESS and JOB
     * blocks are not enumerable, and that is the point: attribution is
     * system-wide, so it covers the charges those unreachable blocks hold. */
    klog_unrated(LOG_INFO, "quota", "--- charge attribution (system-wide) ---");
    quota_dump_sources();
}

void quota_dump_crash(void)
{
    uint32_t rows = 0;
    uint32_t seen = 0;
    uint32_t omitted = 0;       /* USER rows the row budget could not hold   */
    int      tail_unknown = 0;  /* visit budget stopped the walk early       */
    uint64_t flags;
    int      locked;

    if (!quota_registry_ready()) {
        klog_unrated(LOG_INFO, "quota", "crash dump: registry not validated");
        return;
    }

    /* Non-blocking acquisition, with interrupts masked across the attempt so
     * this CPU cannot take one while holding the lock. spin_trylock cannot
     * wait, so a CPU that died holding the registry lock costs this dump its
     * rows -- never the crash report itself.
     *
     * local_irq_save rather than spin_lock_irqsave's machinery: trylock raises
     * no IRQL, so the release side must lower none (see spin_tryunlock). */
    flags  = local_irq_save();
    locked = spin_trylock(&g_registry_lock);
    if (!locked) {
        local_irq_restore(flags);
        __atomic_fetch_add(&g_crash_fallbacks, 1, __ATOMIC_RELAXED);
        klog_unrated(LOG_ERROR, "quota",
             "crash dump: registry unavailable (lock held elsewhere)");
        /* The per-principal rows are gone, but attribution needs neither the
         * registry nor its lock, so it is still reportable -- and this is
         * exactly the crash where it is the only thing left to report. */
        klog_unrated(LOG_INFO, "quota", "--- charge attribution (system-wide) ---");
        quota_dump_sources();
        return;
    }

    /* Copy, do not print, under the lock. The rows are a fixed-size snapshot
     * so the locked window is bounded arithmetic with no output in it. */
    quota_block_t *b = g_registry_head;
    for (; b && seen < QUOTA_DUMP_VISIT_MAX; b = b->reg_next) {
        seen++;
        if (b->principal != (uint8_t)QUOTA_PRINCIPAL_USER)
            continue;
        if (rows >= QUOTA_DUMP_CRASH_ROWS) {
            omitted++;              /* exact: a USER row we chose not to store */
            continue;
        }

        quota_crash_row_t *r = &g_crash_rows[rows++];
        r->id        = b->id;
        r->has_owner = b->has_owner_sid;
        quota_owner_digest(b, &r->owner_rid, &r->owner_hash);
        for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++) {
            r->usage[t] = atomic64_read(&b->counter[t].usage);
            r->limit[t] = atomic64_read(&b->counter[t].limit);
        }
    }

    /* A non-NULL cursor means the visit budget stopped the walk with an
     * arbitrarily long tail unexamined. That is a LOWER BOUND, not a count --
     * folding it into `omitted` would let the warning state an exact number of
     * unwalked nodes it cannot possibly know. */
    tail_unknown = (b != (quota_block_t *)0);

    spin_tryunlock(&g_registry_lock);
    local_irq_restore(flags);

    klog_unrated(LOG_INFO, "quota", "--- quota crash dump (%u USER block(s)) ---",
         (uint64_t)rows);

    for (uint32_t i = 0; i < rows; i++) {
        const quota_crash_row_t *r = &g_crash_rows[i];
        if (r->has_owner)
            klog_unrated(LOG_INFO, "quota", "  block #%llu  rid %u  sid %llx",
                 r->id, (uint64_t)r->owner_rid, r->owner_hash);
        else
            /* Distinct from a real owner whose RID and hash are both 0 --
             * quota_owner_digest returns exactly that for an ownerless block,
             * so printing the digest unconditionally would conflate them. */
            klog_unrated(LOG_INFO, "quota", "  block #%llu  (no owner SID)",
                 r->id);
        for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++) {
            /* Corruption is checked BEFORE the idle fast path, exactly as the
             * live dashboard does. A type with zero usage but a corrupted
             * NEGATIVE limit is still corrupt, and skipping it as "idle" would
             * hide it from the one report written when something already went
             * wrong. Casting it to uint64_t would be worse still: it would
             * print 18446744073709551615, and rendering the unlimited sentinel
             * as "limit 0" would invert its meaning on a post-mortem read. */
            if (r->usage[t] < 0 || r->limit[t] < 0) {
                klog_unrated(LOG_ERROR, "quota",
                     "    %s CORRUPT (negative counter)",
                     quota_resource_type_name((quota_resource_type_t)t));
                continue;
            }
            if (r->usage[t] == 0)
                continue;               /* idle and sane: no row worth a line */
            if (r->limit[t] == (int64_t)QUOTA_LIMIT_UNLIMITED)
                klog_unrated(LOG_INFO, "quota", "    %s usage %llu  limit -",
                     quota_resource_type_name((quota_resource_type_t)t),
                     (uint64_t)r->usage[t]);
            else
                klog_unrated(LOG_INFO, "quota", "    %s usage %llu  limit %llu",
                     quota_resource_type_name((quota_resource_type_t)t),
                     (uint64_t)r->usage[t], (uint64_t)r->limit[t]);
        }
    }

    /* Report each fact separately and only when true. A registry holding
     * exactly QUOTA_DUMP_CRASH_ROWS blocks, or exactly QUOTA_DUMP_VISIT_MAX
     * nodes, is reported in FULL -- claiming truncation there sends an operator
     * hunting for data that is not missing, in the one output that has to be
     * trustworthy. And the unwalked tail is unbounded, so it is never given a
     * number. */
    if (omitted)
        klog_unrated(LOG_WARN, "quota",
             "crash dump: %u row(s) shown, %u USER block(s) omitted by the row budget",
             (uint64_t)rows, (uint64_t)omitted);
    if (tail_unknown)
        klog_unrated(LOG_WARN, "quota",
             "crash dump: visit budget reached after %u nodes; tail not walked",
             (uint64_t)seen);

    /* Attribution is the one part of this report that survives BOTH budgets
     * above and a registry lock this dump could not take: it is system-wide
     * static storage read with plain atomic loads, so it needs no walk, no lock,
     * and no allocation. On a crash where the rows were lost entirely, this may
     * be the only surviving answer to "which subsystem was holding it". */
    klog_unrated(LOG_INFO, "quota", "--- charge attribution (system-wide) ---");
    quota_dump_sources();
}

/* The counter-mutation epoch and the in-progress writer count, or constants
 * when they are not compiled in. Constants make those checks vacuously true
 * rather than wrong: the remaining checks still apply, and the header says so. */
static inline uint64_t quota_mutation_epoch_sample(void)
{
#ifdef KERNEL_TESTS
    return quota_test_mutation_epoch();
#else
    return 0;
#endif
}

static inline uint32_t quota_writers_active_sample(void)
{
#ifdef KERNEL_TESTS
    return quota_test_writers_active();
#else
    return 0;
#endif
}

/* ONE accumulating pass over the USER registry.
 *
 * Returns 1 when the pass covered every node and every addition stayed inside
 * the counter domain; 0 when a node could not be pinned (its contribution is
 * missing, so the totals are not a whole) or an addition would overflow.
 *
 * The overflow check is not defensive padding: a single block may legally hold
 * QUOTA_AMOUNT_MAX, which IS INT64_MAX, so two saturated blocks overflow the
 * aggregate. Signed overflow is undefined behavior, and the plausible outcome
 * -- a negative total -- would make the sweep read a real leak as "no positive
 * delta" and pass. Refusing to classify is the only honest answer. */
static int quota_leak_walk(uint32_t *out_blocks, int64_t *out_usage)
{
    quota_block_t *cursor = (quota_block_t *)0;
    int            ok     = 1;

    *out_blocks = 0;
    for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++)
        out_usage[t] = 0;

    for (;;) {
        quota_block_t *batch[QUOTA_DUMP_BATCH];
        uint32_t n = 0, visited = 0;
        uint64_t flags;

        spin_lock_irqsave(&g_registry_lock, &flags);
        quota_block_t *b = cursor ? cursor->reg_next : g_registry_head;
        while (b && n < QUOTA_DUMP_BATCH && visited < QUOTA_DUMP_VISIT_MAX) {
            visited++;
            if (b->principal == (uint8_t)QUOTA_PRINCIPAL_USER &&
                quota_block_try_ref(b))
                batch[n++] = b;
            b = b->reg_next;
        }
        int visit_capped = (b != (quota_block_t *)0);
        spin_unlock_irqrestore(&g_registry_lock, flags);

        /* Release the previous cursor only after this batch is pinned, so the
         * resume position stayed valid across the unlocked gap. */
        if (cursor)
            quota_block_deref(cursor);
        cursor = (quota_block_t *)0;

        if (n == 0) {
            if (visit_capped)
                ok = 0;         /* unreachable nodes: totals are incomplete */
            break;
        }

        for (uint32_t i = 0; i < n; i++) {
            (*out_blocks)++;
            for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++) {
                int64_t u = atomic64_read(&batch[i]->counter[t].usage);
                /* NEGATIVE is corruption, not idleness. The documented domain
                 * is 0..QUOTA_AMOUNT_MAX, so a value below zero means a torn
                 * mutation or a return-underflow got through. Skipping it the
                 * way we skip an idle zero would let both walks agree on a
                 * total that omits it and report the category CLEAN -- the
                 * opposite of the fail-closed handling the overflow path and
                 * the dashboard's CORRUPT row already use. */
                if (u < 0) {
                    ok = 0;
                    continue;
                }
                if (u == 0)
                    continue;
                if (out_usage[t] > QUOTA_AMOUNT_MAX - u) {
                    ok = 0;     /* checked BEFORE the add: never wraps */
                    continue;
                }
                out_usage[t] += u;
            }
        }

        /* Keep the LAST entry pinned as the resume point; release the rest. */
        for (uint32_t i = 0; i + 1 < n; i++)
            quota_block_deref(batch[i]);
        if (!visit_capped && n < QUOTA_DUMP_BATCH) {
            quota_block_deref(batch[n - 1]);
            break;
        }
        cursor = batch[n - 1];
    }

    return ok;
}

int quota_leak_snapshot(quota_leak_snapshot_t *out)
{
    uint32_t blocks_b;
    int64_t  usage_b[QUOTA_RESOURCE_TYPE_COUNT];
    uint64_t gen_before, gen_after;
    uint64_t epoch_before, epoch_after;

    if (!out)
        return 0;

    for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++)
        out->usage[t] = 0;
    out->user_blocks = 0;
    out->coherent    = 0;
    out->generation  = 0;

    if (!quota_registry_ready())
        return 0;

    /* THREE independent checks, because each catches what the others cannot.
     *
     *   - The membership generation catches a link or unlink.
     *   - The counter-mutation epoch catches a charge, return, or transfer.
     *     This one is load-bearing and cannot be replaced by the walks: a
     *     transfer read source-before/destination-after produces the SAME
     *     wrong total on both walks, so two agreeing walks would certify it.
     *   - Two agreeing walks catch a mutation the epoch could theoretically
     *     miss on a non-KERNEL_TESTS build, where the epoch does not move.
     *
     * A reading is coherent only if all three hold. */
    gen_before   = quota_registry_generation();
    epoch_before = quota_mutation_epoch_sample();
    /* No mutation may be IN PROGRESS at either end. The epoch only witnesses
     * completed ones, and a transfer stalled between crediting the destination
     * and debiting the source is exactly the state whose total never existed. */
    if (quota_writers_active_sample() != 0)
        goto indeterminate;

    if (!quota_leak_walk(&out->user_blocks, out->usage))
        goto indeterminate;
    if (!quota_leak_walk(&blocks_b, usage_b))
        goto indeterminate;

    /* SAMPLE ORDER AT THIS EDGE IS LOAD-BEARING, and it is the mirror image of
     * the opening edge (epoch, then writers). Reject a live writer FIRST, then
     * read the epoch.
     *
     * Reading the epoch first loses the race it exists to win: a transfer
     * parked after crediting the destination can span both walks, so both see
     * the same double-counted total; if it then completes AFTER the epoch is
     * sampled but BEFORE the writer count is read, the epoch still matches and
     * the writer count reads zero, and an aggregate that never existed is
     * certified coherent. Rejecting on the writer count before the epoch
     * sample closes that window: any writer that had not yet finished at this
     * point is still counted, and any that finished earlier moved the epoch. */
    if (quota_writers_active_sample() != 0)
        goto indeterminate;
    epoch_after = quota_mutation_epoch_sample();
    gen_after   = quota_registry_generation();
    if (gen_before != gen_after || epoch_before != epoch_after ||
        blocks_b != out->user_blocks)
        goto indeterminate;

    for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++)
        if (usage_b[t] != out->usage[t])
            goto indeterminate;

    out->generation = gen_after;
    out->coherent   = 1;
    return 1;

indeterminate:
    /* Report the generation the caller can actually act on, and leave the
     * totals visible for diagnosis -- but never claim they are comparable. */
    out->generation = quota_registry_generation();
    out->coherent   = 0;
    return 0;
}

#ifdef KERNEL_TESTS
uint64_t quota_test_crash_fallbacks(void)
{
    return __atomic_load_n(&g_crash_fallbacks, __ATOMIC_RELAXED);
}

uint32_t quota_test_dump_batch(void)
{
    return QUOTA_DUMP_BATCH;
}

/* The registry lock is file-private, so making it genuinely unavailable to
 * quota_dump_crash from a single-CPU test requires a seam here. Taking the
 * real lock (rather than faking a busy flag) is what makes the test prove the
 * actual acquire path: quota_dump_crash's try-lock must FAIL and return, and a
 * blocking regression would hang here rather than pass silently. */
void quota_test_registry_hold(uint64_t *flags)
{
    spin_lock_irqsave(&g_registry_lock, flags);
}

void quota_test_registry_release(uint64_t flags)
{
    spin_unlock_irqrestore(&g_registry_lock, flags);
}
#endif
