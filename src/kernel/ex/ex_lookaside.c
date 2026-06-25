/* ============================================================================
 * ex_lookaside.c -- Lookaside lists (TODO-06 S5): NPaged, Paged, unified Ex.
 *
 * A lookaside list is a fixed-size block cache over a backing allocator. The
 * hot paths (allocate-from-cache, free-to-cache) are the S2 interlocked SLIST
 * and are lock-free + DISPATCH/SMP-safe. The cold backing paths (grow on a
 * cache miss, drain an over-cap free) call the backing allocator -- kmalloc by
 * default -- and the kernel heap is unsynchronized today, so they run ONLY at
 * <= APC_LEVEL and under one global executive-pool spinlock. A nonpaged miss
 * above APC_LEVEL returns NULL (a legal empty-cache outcome); a paged op above
 * APC_LEVEL is rejected outright (paged memory may fault). The synchronized
 * tagged pool that will replace the kmalloc backing is owned by the
 * 03-memory-concurrency advanced-allocator (ExAllocatePoolWithTag).
 *
 * The embedded SLIST_HEADER needs 16-byte alignment for cmpxchg16b; kmalloc
 * does not honor type alignment, so the control block over-provisions a raw
 * buffer and init points free_list at the aligned slot inside it -- a
 * lookaside list allocated by kmalloc therefore never trips the SLIST alignment
 * bugcheck.
 * ============================================================================ */

#include "kernel/ex.h"
#include "kernel/mm/heap.h"          /* kmalloc / kfree */
#include "libc/string.h"             /* memset */
#include "kernel/sched/spinlock.h"
#include "kernel/sched/irql.h"       /* KeGetCurrentIrql, APC_LEVEL */
#include "kernel/klog.h"
#include "kernel/bugcheck.h"

/* One global lock serializes the kmalloc/kfree backing churn of ALL lookaside
 * lists. It does not protect against non-lookaside kmalloc callers (the heap is
 * globally unsynchronized -- owned by the advanced-allocator); it bounds the
 * executive's own pool traffic and is only taken on the cold backing path. */
static DEFINE_SPINLOCK(s_backing_lock);

/* Verifier state (S14 owns the broader verifier; S5 exposes the seam). */
static volatile bool s_verifier_on = false;
static uint64_t      s_uaf_count   = 0;

void ExpSetLookasideVerifier(bool on)
{
    __atomic_store_n(&s_verifier_on, on, __ATOMIC_RELEASE);
}

uint64_t ExpLookasideUafCount(void)
{
    return __atomic_load_n(&s_uaf_count, __ATOMIC_ACQUIRE);
}

static inline bool verifier_on(void)
{
    return __atomic_load_n(&s_verifier_on, __ATOMIC_ACQUIRE);
}

/* Place an aligned SLIST_HEADER inside the over-provisioned raw buffer. */
static inline SLIST_HEADER *aligned_slist(uint8_t *raw)
{
    uintptr_t a = ((uintptr_t)raw + 15u) & ~(uintptr_t)15u;
    return (SLIST_HEADER *)a;
}

/* ---- backing allocator (cold path, <= APC_LEVEL, serialized) ------------- */

static void *backing_alloc(EX_LOOKASIDE *L)
{
    if (L->alloc_fn)
        return L->alloc_fn(L->size, L->tag, L->ctx);   /* caller owns its locking */

    uint64_t flags;
    void *p;
    spin_lock_irqsave(&s_backing_lock, &flags);
    p = kmalloc(L->size);
    spin_unlock_irqrestore(&s_backing_lock, flags);
    return p;
}

static void backing_free(EX_LOOKASIDE *L, void *p)
{
    if (L->free_fn) {
        L->free_fn(p, L->ctx);
        return;
    }
    uint64_t flags;
    spin_lock_irqsave(&s_backing_lock, &flags);
    kfree(p);
    spin_unlock_irqrestore(&s_backing_lock, flags);
}

/* ---- verifier poison helpers -------------------------------------------- */

/* Poison the entry body on free, skipping the first sizeof(SLIST_ENTRY) bytes
 * (the SLIST link overwrites them while the entry is cached). Only meaningful
 * when the entry is larger than the link. */
static void poison_body(EX_LOOKASIDE *L, void *entry)
{
    if (L->size <= sizeof(SLIST_ENTRY))
        return;
    uint8_t *b = (uint8_t *)entry + sizeof(SLIST_ENTRY);
    size_t n = L->size - sizeof(SLIST_ENTRY);
    memset(b, EX_LOOKASIDE_POISON_BYTE, n);
}

/* Re-check the poison on alloc; a mismatch means the body was written after the
 * free (use-after-free). Skip the link bytes (legitimately clobbered). */
static void check_poison(EX_LOOKASIDE *L, void *entry)
{
    if (L->size <= sizeof(SLIST_ENTRY))
        return;
    const uint8_t *b = (const uint8_t *)entry + sizeof(SLIST_ENTRY);
    size_t n = L->size - sizeof(SLIST_ENTRY);
    for (size_t i = 0; i < n; i++) {
        if (b[i] != EX_LOOKASIDE_POISON_BYTE) {
            __atomic_fetch_add(&s_uaf_count, 1ull, __ATOMIC_ACQ_REL);
            klog(LOG_ERROR, "ex",
                 "lookaside use-after-free: tag=0x%lx entry=%p body byte %lu",
                 (uint64_t)L->tag, entry, (uint64_t)i);
            return;   /* report once per entry */
        }
    }
}

/* ---- core allocate / free / init / teardown ----------------------------- */

static void *la_allocate(EX_LOOKASIDE *L)
{
    if (!L || !L->initialized)
        return (void *)0;

    /* Paged lists are illegal above APC_LEVEL for ANY access -- even a cache hit
     * dereferences paged memory that may fault. */
    if (L->paged && KeGetCurrentIrql() > APC_LEVEL) {
        klog(LOG_ERROR, "ex",
             "paged lookaside alloc at IRQL %u (> APC_LEVEL)",
             (uint64_t)KeGetCurrentIrql());
        if (verifier_on())
            KeBugCheckEx(BUGCHECK_IRQL_NOT_LESS_OR_EQUAL,
                         (uint64_t)(uintptr_t)L, KeGetCurrentIrql(), APC_LEVEL, 5);
        return (void *)0;
    }

    __atomic_fetch_add(&L->total_allocs, 1ull, __ATOMIC_RELAXED);

    SLIST_ENTRY *e = ExInterlockedPopEntrySList(L->free_list);
    if (e) {
        __atomic_fetch_add(&L->alloc_hits, 1ull, __ATOMIC_RELAXED);
        if (verifier_on())
            check_poison(L, (void *)e);
        return (void *)e;
    }

    __atomic_fetch_add(&L->alloc_misses, 1ull, __ATOMIC_RELAXED);

    /* Cache empty: grow from backing, but only where the heap is legal to
     * touch. Above APC_LEVEL a nonpaged caller just gets NULL (lookaside
     * empty) -- never an unsynchronized kmalloc at DISPATCH_LEVEL. */
    if (KeGetCurrentIrql() > APC_LEVEL)
        return (void *)0;

    return backing_alloc(L);
}

static void la_free(EX_LOOKASIDE *L, void *entry)
{
    if (!L || !L->initialized || !entry)
        return;

    if (L->paged && KeGetCurrentIrql() > APC_LEVEL) {
        klog(LOG_ERROR, "ex",
             "paged lookaside free at IRQL %u (> APC_LEVEL)",
             (uint64_t)KeGetCurrentIrql());
        if (verifier_on())
            KeBugCheckEx(BUGCHECK_IRQL_NOT_LESS_OR_EQUAL,
                         (uint64_t)(uintptr_t)L, KeGetCurrentIrql(), APC_LEVEL, 5);
        return;   /* cannot safely touch paged storage here */
    }

    __atomic_fetch_add(&L->total_frees, 1ull, __ATOMIC_RELAXED);

    if (verifier_on())
        poison_body(L, entry);

    /* Always cache (SLIST push is DISPATCH/SMP-safe). */
    ExInterlockedPushEntrySList(L->free_list, (SLIST_ENTRY *)entry);

    /* Over cap: drain one excess entry to backing, but only where the heap is
     * legal. Above APC_LEVEL the over-cap entry stays cached transiently and is
     * drained by a later <= APC_LEVEL free. */
    if (ExQueryDepthSList(L->free_list) > L->max_depth &&
        KeGetCurrentIrql() <= APC_LEVEL) {
        SLIST_ENTRY *excess = ExInterlockedPopEntrySList(L->free_list);
        if (excess) {
            backing_free(L, (void *)excess);
            __atomic_fetch_add(&L->free_drains, 1ull, __ATOMIC_RELAXED);
            return;
        }
    }
    __atomic_fetch_add(&L->free_hits, 1ull, __ATOMIC_RELAXED);
}

static int la_init(EX_LOOKASIDE *L, EX_LOOKASIDE_ALLOC alloc_fn,
                   EX_LOOKASIDE_FREE free_fn, void *ctx, bool paged,
                   size_t size, uint32_t tag, uint16_t depth)
{
    if (!L)
        return 1;
    /* Zero FIRST so every failure path below leaves the caller's storage inert
     * (initialized=false, free_list=NULL) -- the void NPaged/Paged initializers
     * discard the return code, so a rejected init must not leave indeterminate
     * or stale state that a later allocate/free/delete could misread. */
    memset(L, 0, sizeof(*L));

    if (size == 0)
        return 1;
    /* Reject oversized entries: bounds the kmalloc backing (no (size + 15)
     * wrap to a tiny allocation while L->size stays huge -> OOB verifier
     * memset/scan) and bounds the verifier poison scan on the hot path. */
    if (size > EX_LOOKASIDE_MAX_ALLOC)
        return 1;
    /* A custom backing pair must be all-or-nothing. */
    if ((alloc_fn == (EX_LOOKASIDE_ALLOC)0) != (free_fn == (EX_LOOKASIDE_FREE)0))
        return 1;

    L->free_list = aligned_slist(L->_slist_raw);
    ExInitializeSListHead(L->free_list);

    if (size < EX_LOOKASIDE_MIN_ALLOC)
        size = EX_LOOKASIDE_MIN_ALLOC;   /* entry must hold the SLIST link while cached */
    L->size      = size;
    L->tag       = tag;
    L->max_depth = depth ? depth : (uint16_t)EX_LOOKASIDE_DEFAULT_DEPTH;
    L->paged     = paged;
    L->alloc_fn  = alloc_fn;
    L->free_fn   = free_fn;
    L->ctx       = ctx;
    L->initialized = true;
    return 0;
}

/* Drain all cached entries to backing. Teardown / trim only; legal at
 * <= APC_LEVEL (the backing free touches the heap). Returns true if the drain
 * ran; false if the IRQL precondition was violated and the cache is untouched
 * -- the caller must NOT invalidate the list in that case (the cached entries
 * are still reachable through a later, legal drain). */
static bool la_drain(EX_LOOKASIDE *L)
{
    if (!L || !L->initialized)
        return true;   /* nothing to drain; safe to invalidate */
    if (KeGetCurrentIrql() > APC_LEVEL) {
        klog(LOG_ERROR, "ex",
             "lookaside drain at IRQL %u (> APC_LEVEL); retry at <= APC_LEVEL",
             (uint64_t)KeGetCurrentIrql());
        return false;  /* cache untouched; do NOT invalidate */
    }
    SLIST_ENTRY *e;
    while ((e = ExInterlockedPopEntrySList(L->free_list)) != (SLIST_ENTRY *)0)
        backing_free(L, (void *)e);
    return true;
}

/* ---- public NPaged API -------------------------------------------------- */

void ExInitializeNPagedLookasideList(NPAGED_LOOKASIDE_LIST *l, size_t size,
                                     uint32_t tag, uint16_t depth)
{
    if (l)
        (void)la_init(&l->L, (EX_LOOKASIDE_ALLOC)0, (EX_LOOKASIDE_FREE)0,
                      (void *)0, false, size, tag, depth);
}

void *ExAllocateFromNPagedLookasideList(NPAGED_LOOKASIDE_LIST *l)
{
    return l ? la_allocate(&l->L) : (void *)0;
}

void ExFreeToNPagedLookasideList(NPAGED_LOOKASIDE_LIST *l, void *entry)
{
    if (l)
        la_free(&l->L, entry);
}

void ExDeleteNPagedLookasideList(NPAGED_LOOKASIDE_LIST *l)
{
    /* Only invalidate once the cache actually drained; a wrong-IRQL drain
     * leaves the list intact and recoverable instead of stranding entries. */
    if (l && la_drain(&l->L))
        l->L.initialized = false;
}

/* ---- public Paged API --------------------------------------------------- */

void ExInitializePagedLookasideList(PAGED_LOOKASIDE_LIST *l, size_t size,
                                    uint32_t tag, uint16_t depth)
{
    if (l)
        (void)la_init(&l->L, (EX_LOOKASIDE_ALLOC)0, (EX_LOOKASIDE_FREE)0,
                      (void *)0, true, size, tag, depth);
}

void *ExAllocateFromPagedLookasideList(PAGED_LOOKASIDE_LIST *l)
{
    return l ? la_allocate(&l->L) : (void *)0;
}

void ExFreeToPagedLookasideList(PAGED_LOOKASIDE_LIST *l, void *entry)
{
    if (l)
        la_free(&l->L, entry);
}

void ExDeletePagedLookasideList(PAGED_LOOKASIDE_LIST *l)
{
    /* Only invalidate once the cache actually drained; a wrong-IRQL drain
     * leaves the list intact and recoverable instead of stranding entries. */
    if (l && la_drain(&l->L))
        l->L.initialized = false;
}

/* ---- public unified Ex API ---------------------------------------------- */

int ExInitializeLookasideListEx(LOOKASIDE_LIST_EX *l, EX_LOOKASIDE_ALLOC alloc_fn,
                                EX_LOOKASIDE_FREE free_fn, void *ctx, bool paged,
                                size_t size, uint32_t tag, uint16_t depth)
{
    if (!l)
        return 1;
    return la_init(&l->L, alloc_fn, free_fn, ctx, paged, size, tag, depth);
}

void *ExAllocateFromLookasideListEx(LOOKASIDE_LIST_EX *l)
{
    return l ? la_allocate(&l->L) : (void *)0;
}

void ExFreeToLookasideListEx(LOOKASIDE_LIST_EX *l, void *entry)
{
    if (l)
        la_free(&l->L, entry);
}

void ExFlushLookasideListEx(LOOKASIDE_LIST_EX *l)
{
    if (l)
        la_drain(&l->L);   /* keeps initialized: list stays usable after flush */
}

void ExDeleteLookasideListEx(LOOKASIDE_LIST_EX *l)
{
    /* Only invalidate once the cache actually drained; a wrong-IRQL drain
     * leaves the list intact and recoverable instead of stranding entries. */
    if (l && la_drain(&l->L))
        l->L.initialized = false;
}
