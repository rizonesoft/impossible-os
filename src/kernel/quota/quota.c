/* ============================================================================
 * quota.c -- Kernel resource-accounting type registry
 *
 * Owns the static const descriptor table that names every chargeable kernel
 * resource type, its accounting unit, default limit, and override privilege.
 * The table is the single source of truth for the taxonomy; the charge API and
 * every integration section consume it through the accessors below.
 * ============================================================================ */

#include "kernel/quota/quota.h"
#include "kernel/security/privileges.h"  /* SE_INCREASE_QUOTA_PRIVILEGE */
#include "kernel/mm/heap.h"              /* kmalloc_zeroed / kfree */
#include "kernel/sched/irql.h"           /* KeGetCurrentIrql for the diag gate */
#include "kernel/klog.h"
#include "kernel/smp.h"               /* smp_cpu_id for test-only CPU scoping */

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
    uint32_t   owner_sid_buf[SID_MAX_SIZE / 4];     /* SID capture, 4-aligned  */
    uint8_t    has_owner_sid;                       /* 0 = no owner recorded    */
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
/* Critical sections COMPLETED while counting is armed, on the arming CPU only.
 * Diagnostic: it orders nothing and guards nothing, so relaxed accesses are
 * exactly right -- atomicity without a barrier.
 *
 * The CPU scoping is load-bearing, not defensive. The counted set is not just
 * charging: quota_task_init and quota_task_teardown take task->quota_lock on
 * EVERY task creation and every process death. A thread dying on another CPU
 * inside a test's counted window would otherwise add a section to the global
 * total, and every budget assertion is an exact equality -- so that is a hard
 * test failure, not a tolerated skew. Counting only sections completed on the
 * CPU that armed the window removes that whole class of interference. */
static uint64_t g_lock_sections;
static uint8_t  g_lock_count_on;
static uint32_t g_lock_count_cpu;

/* Shared with quota_owner.c so owner-side sections land in the same total.
 * Called AFTER the lock is released (see quota_block_unlock). */
void quota_test_count_lock_section(void)
{
    if (!__atomic_load_n(&g_lock_count_on, __ATOMIC_RELAXED))
        return;
    if (smp_cpu_id() != __atomic_load_n(&g_lock_count_cpu, __ATOMIC_RELAXED))
        return;
    __atomic_fetch_add(&g_lock_sections, 1, __ATOMIC_RELAXED);
}
#endif

/* THE one place a block's lock is taken. Every mutation path goes through this
 * pair, which is what lets the test-only cost instrumentation stay honest: a
 * future lock site either uses this helper and is counted, or takes the lock
 * itself and is visible as an obvious deviation in review.
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
    spin_lock_irqsave(&block->lock, flags);
}

static inline void quota_block_unlock(quota_block_t *block, uint64_t flags)
{
    spin_unlock_irqrestore(&block->lock, flags);
#ifdef KERNEL_TESTS
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
}

#ifdef KERNEL_TESTS
/* Test-only raw counter access (see quota.h). Compiled out in production. */
void quota_test_poke_usage(quota_block_t *block, quota_resource_type_t type, int64_t value)
{
    if (block && quota_type_valid(type))
        atomic64_set(&block->counter[type].usage, value);
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
    __atomic_store_n(&g_lock_count_cpu, smp_cpu_id(), __ATOMIC_RELAXED);
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

NTSTATUS quota_charge(quota_block_t *block, quota_resource_type_t type, uint64_t amount)
{
    if (!block || !quota_type_valid(type) || amount > (uint64_t)QUOTA_AMOUNT_MAX)
        return STATUS_INVALID_PARAMETER;
    if (amount == 0)
        return STATUS_SUCCESS;              /* no-op, and never lifts the peak */

    int64_t total = 0;
    uint64_t flags;
    quota_diag_t diag = QUOTA_DIAG_NONE;

    quota_block_lock(block, &flags);
    NTSTATUS status = quota_check_locked(block, type, (int64_t)amount, &total, &diag);
    if (status == STATUS_SUCCESS)
        quota_commit_locked(block, type, total);
    else
        quota_bump_failures(&block->counter[type].failures);
    quota_block_unlock(block, flags);

    /* Outside the lock, and only when there is something to say: the normal
     * charge path must not pay a call into the diagnostic helper. */
    if (diag != QUOTA_DIAG_NONE)
        quota_emit_diag(diag, type);
    return status;
}

NTSTATUS quota_return(quota_block_t *block, quota_resource_type_t type, uint64_t amount)
{
    if (!block || !quota_type_valid(type) || amount > (uint64_t)QUOTA_AMOUNT_MAX)
        return STATUS_INVALID_PARAMETER;
    if (amount == 0)
        return STATUS_SUCCESS;

    uint64_t flags;
    quota_diag_t diag = QUOTA_DIAG_NONE;

    quota_block_lock(block, &flags);
    NTSTATUS status = quota_release_locked(block, type, (int64_t)amount, &diag);
    quota_block_unlock(block, flags);

    /* Logging happens OUTSIDE the lock, and only when there is something to
     * say -- the normal path must not pay a call into the diagnostic helper. */
    if (diag != QUOTA_DIAG_NONE)
        quota_emit_diag(diag, type);
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
    } else if (src_cur < move) {
        /* Counted like every other refusal so the per-type failure telemetry
         * sees the most common transfer failure mode, not just the rare ones. */
        quota_bump_failures(&src->counter[type].failures);
        status = STATUS_QUOTA_EXCEEDED;      /* src does not hold that much */
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
        }
    }

    quota_block_unlock_quiet(second, flags_second);
    quota_block_unlock_quiet(first, flags_first);
#ifdef KERNEL_TESTS
    /* Both sections recorded here, with interrupts genuinely restored. */
    quota_test_count_lock_section();
    quota_test_count_lock_section();
#endif

    if (diag != QUOTA_DIAG_NONE)
        quota_emit_diag(diag, type);     /* outside BOTH locks, only if needed */
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
        while (b && n < QUOTA_RELIMIT_BATCH) {
            /* try_ref, not ref: a block whose count already reached zero is
             * committed to teardown and must not be lifted back to life. */
            if (b->principal == (uint8_t)QUOTA_PRINCIPAL_USER &&
                quota_block_try_ref(b))
                batch[n++] = b;
            b = b->reg_next;
        }
        spin_unlock_irqrestore(&g_registry_lock, flags);

        /* Release the previous cursor only after the new batch is pinned, so
         * the list position we resumed from stayed valid for the whole walk. */
        if (cursor)
            quota_block_deref(cursor);
        cursor = (quota_block_t *)0;

        if (n == 0)
            break;

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
         * rest. A short batch means the list ended, so stop after applying it. */
        for (uint32_t i = 0; i + 1 < n; i++)
            quota_block_deref(batch[i]);
        if (n < QUOTA_RELIMIT_BATCH) {
            quota_block_deref(batch[n - 1]);
            break;
        }
        cursor = batch[n - 1];
    }
}
