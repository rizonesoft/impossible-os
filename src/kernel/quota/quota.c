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
};

#undef Q_PRIV
#undef Q_UNL

/* Layer 1 defense: the parallel array must have exactly one entry per type, so
 * a new enum member without a matching row is a compile error, not a runtime
 * gap (a designated-initializer table would otherwise leave holes zero-filled
 * with a NULL name). This only bites because the array above is UNSIZED. */
_Static_assert(sizeof(g_quota_desc) / sizeof(g_quota_desc[0]) == QUOTA_RESOURCE_TYPE_COUNT,
               "quota descriptor table must have one entry per resource type");

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
struct quota_block {
    atomic_t   refcount;                            /* live references; 0 frees */
    atomic64_t usage[QUOTA_RESOURCE_TYPE_COUNT];    /* current charge, 0..MAX   */
    atomic64_t peak[QUOTA_RESOURCE_TYPE_COUNT];     /* high-water usage         */
    atomic64_t failures[QUOTA_RESOURCE_TYPE_COUNT]; /* refused charges, saturating */
    atomic64_t limit[QUOTA_RESOURCE_TYPE_COUNT];    /* QUOTA_LIMIT_UNLIMITED = no cap */
    spinlock_t lock;                                /* guards ALL mutations    */
    uint32_t   owner_sid_buf[SID_MAX_SIZE / 4];     /* SID capture, 4-aligned  */
    uint8_t    has_owner_sid;                       /* 0 = no owner recorded    */
};

/* A block must fit the kmalloc size rule (<= 4 KB). 14 types x 4 counters
 * plus the SID buffer is far under that; assert so a future type-count growth
 * cannot silently push allocation into pmm_alloc_contiguous territory. */
_Static_assert(sizeof(quota_block_t) <= 4096,
    "quota_block_t must stay within the kmalloc size rule (<= 4 KB)");

/* Every counter array is indexed by quota_resource_type_t, so EACH must have
 * one slot per type. Asserting only one of them would let a hand-edited
 * smaller bound on any other array compile while the loops and accessors keep
 * indexing across the full enum, writing into adjacent struct fields. */
#define QUOTA_ASSERT_PER_TYPE_ARRAY(field)                          \
    _Static_assert(sizeof(((quota_block_t *)0)->field) ==           \
                   sizeof(atomic64_t) * QUOTA_RESOURCE_TYPE_COUNT,  \
                   "quota_block_t." #field " must have one slot per resource type")

QUOTA_ASSERT_PER_TYPE_ARRAY(usage);
QUOTA_ASSERT_PER_TYPE_ARRAY(peak);
QUOTA_ASSERT_PER_TYPE_ARRAY(failures);
QUOTA_ASSERT_PER_TYPE_ARRAY(limit);

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
/* Test-only raw counter access (see quota.h). Compiled out in production. */
void quota_test_poke_usage(quota_block_t *block, quota_resource_type_t type, int64_t value)
{
    if (block && quota_type_valid(type))
        atomic64_set(&block->usage[type], value);
}

void quota_test_poke_failures(quota_block_t *block, quota_resource_type_t type, int64_t value)
{
    if (block && quota_type_valid(type))
        atomic64_set(&block->failures[type], value);
}

int64_t quota_test_raw_usage(const quota_block_t *block, quota_resource_type_t type)
{
    if (!block || !quota_type_valid(type))
        return 0;
    return atomic64_read(&block->usage[type]);
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
    int64_t cur = atomic64_read(&block->usage[type]);

    if (cur < 0) {
        /* Corrupted counter: fail closed rather than account against it. */
        *diag = QUOTA_DIAG_NEGATIVE_USAGE;
        return STATUS_INTEGER_OVERFLOW;
    }
    if (add > QUOTA_AMOUNT_MAX - cur)
        return STATUS_INTEGER_OVERFLOW;   /* checked BEFORE the add (UB guard) */

    int64_t next  = cur + add;
    int64_t limit = atomic64_read(&block->limit[type]);
    if (limit != (int64_t)QUOTA_LIMIT_UNLIMITED && next > limit)
        return STATUS_QUOTA_EXCEEDED;

    *out_total = next;
    return STATUS_SUCCESS;
}

/* Commit a checked total: store usage and lift the peak. Lock held. */
static void quota_commit_locked(quota_block_t *block, quota_resource_type_t type,
                                int64_t total)
{
    atomic64_set(&block->usage[type], total);
    quota_lift_peak(&block->peak[type], total);
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
    int64_t cur = atomic64_read(&block->usage[type]);
    if (cur < 0) {
        *diag = QUOTA_DIAG_NEGATIVE_USAGE;
        quota_bump_failures(&block->failures[type]);
        return STATUS_INTEGER_OVERFLOW;
    }
    if (cur < sub) {
        *diag = QUOTA_DIAG_RETURN_UNDERFLOW;
        quota_bump_failures(&block->failures[type]);
        return STATUS_INTEGER_OVERFLOW;
    }
    atomic64_set(&block->usage[type], cur - sub);
    return STATUS_SUCCESS;
}

quota_block_t *quota_block_create(const SID *owner, uint32_t owner_len)
{
    uint32_t sid_len = 0;

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

    /* Seed limits from the type registry so a fresh block already carries the
     * taxonomy's defaults; callers override per type with quota_set_limit. */
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        uint64_t def = g_quota_desc[i].default_limit;
        if (def > (uint64_t)QUOTA_AMOUNT_MAX)
            def = (uint64_t)QUOTA_AMOUNT_MAX;   /* clamp into the counter domain */
        atomic64_set(&block->limit[i], (int64_t)def);
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

void quota_block_ref(quota_block_t *block)
{
    if (!block)
        return;
    atomic_inc(&block->refcount);
}

void quota_block_deref(quota_block_t *block)
{
    if (!block)
        return;
    if (atomic_dec_and_test(&block->refcount))
        kfree(block);
}

const SID *quota_block_owner(const quota_block_t *block)
{
    if (!block || !block->has_owner_sid)
        return NULL;
    return (const SID *)block->owner_sid_buf;
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

    spin_lock_irqsave(&block->lock, &flags);
    NTSTATUS status = quota_check_locked(block, type, (int64_t)amount, &total, &diag);
    if (status == STATUS_SUCCESS)
        quota_commit_locked(block, type, total);
    else
        quota_bump_failures(&block->failures[type]);
    spin_unlock_irqrestore(&block->lock, flags);

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

    spin_lock_irqsave(&block->lock, &flags);
    NTSTATUS status = quota_release_locked(block, type, (int64_t)amount, &diag);
    spin_unlock_irqrestore(&block->lock, flags);

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

    spin_lock_irqsave(&first->lock, &flags_first);
    spin_lock_irqsave(&second->lock, &flags_second);

    NTSTATUS status;
    int64_t src_cur = atomic64_read(&src->usage[type]);
    if (src_cur < 0) {
        /* Corrupted source: distinct from an ordinary short balance. Reporting
         * this as QUOTA_EXCEEDED would hide an accounting-integrity failure
         * behind a routine-looking refusal, so it fails closed and is counted
         * and logged exactly like a corrupted destination. */
        diag = QUOTA_DIAG_NEGATIVE_USAGE_SOURCE;
        quota_bump_failures(&src->failures[type]);
        status = STATUS_INTEGER_OVERFLOW;
    } else if (src_cur < move) {
        /* Counted like every other refusal so the per-type failure telemetry
         * sees the most common transfer failure mode, not just the rare ones. */
        quota_bump_failures(&src->failures[type]);
        status = STATUS_QUOTA_EXCEEDED;      /* src does not hold that much */
    } else {
        int64_t dst_total = 0;
        status = quota_check_locked(dst, type, move, &dst_total, &diag);
        if (status == STATUS_SUCCESS) {
            quota_commit_locked(dst, type, dst_total);
            atomic64_set(&src->usage[type], src_cur - move);
        } else {
            quota_bump_failures(&dst->failures[type]);
            /* Re-label: quota_check_locked cannot know it was called for a
             * transfer destination, so a corrupted dst would otherwise be
             * logged as an ordinary charge and be indistinguishable from one. */
            if (diag == QUOTA_DIAG_NEGATIVE_USAGE)
                diag = QUOTA_DIAG_NEGATIVE_USAGE_DEST;
        }
    }

    spin_unlock_irqrestore(&second->lock, flags_second);
    spin_unlock_irqrestore(&first->lock, flags_first);

    if (diag != QUOTA_DIAG_NONE)
        quota_emit_diag(diag, type);     /* outside BOTH locks, only if needed */
    return status;
}

uint64_t quota_usage(const quota_block_t *block, quota_resource_type_t type)
{
    if (!block || !quota_type_valid(type))
        return 0;
    int64_t v = atomic64_read(&block->usage[type]);
    return (v > 0) ? (uint64_t)v : 0;
}

uint64_t quota_peak(const quota_block_t *block, quota_resource_type_t type)
{
    if (!block || !quota_type_valid(type))
        return 0;
    int64_t v = atomic64_read(&block->peak[type]);
    return (v > 0) ? (uint64_t)v : 0;
}

uint64_t quota_failures(const quota_block_t *block, quota_resource_type_t type)
{
    if (!block || !quota_type_valid(type))
        return 0;
    int64_t v = atomic64_read(&block->failures[type]);
    return (v > 0) ? (uint64_t)v : 0;
}

uint64_t quota_limit(const quota_block_t *block, quota_resource_type_t type)
{
    if (!block || !quota_type_valid(type))
        return 0;
    int64_t v = atomic64_read(&block->limit[type]);
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
    spin_lock_irqsave(&block->lock, &flags);
    atomic64_set(&block->limit[type], (int64_t)limit);
    spin_unlock_irqrestore(&block->lock, flags);
    return STATUS_SUCCESS;
}
