/* test_ex.c -- Executive Support Runtime unit tests (TODO-06).
 *
 * S2 Interlocked SLIST: lock-free LIFO push/pop/flush/depth. Single-threaded
 * functional coverage (LIFO order, depth linearization, empty/flush edges) PLUS
 * a concurrent multi-kthread stress suite that exercises the failed-DCAS retry
 * path and checks for lost/duplicated nodes under SMP contention. No live boot
 * infrastructure (test policy); the shared head is 16-byte aligned for
 * cmpxchg16b. The depth-saturation boundary (>65535) is covered by the
 * saturating arithmetic (d < SLIST_DEPTH_MAX ? d+1 : MAX), not a live 64K-node
 * test (a 65536-entry pool would cost ~512 KB of test BSS).
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/ex.h"
#include "kernel/sched/task.h"
#include "kernel/sched/irql.h"   /* KeRaiseIrql/KeLowerIrql -- paged-lookaside IRQL gate test */
#include "kernel/mm/heap.h"      /* kmalloc/kfree -- LOOKASIDE_LIST_EX custom backing test */
#include "kernel/ob/ob.h"   /* ObDereferenceObject -- release caller-owned callback objects */

/* ---- S2: Interlocked SLIST ---------------------------------------------- */

static void test_ex_slist_init_empty(void)
{
    SLIST_HEADER h;
    ExInitializeSListHead(&h);
    TEST_ASSERT_EQ(ExQueryDepthSList(&h), 0u, "fresh list depth == 0");
    TEST_ASSERT(ExInterlockedPopEntrySList(&h) == (SLIST_ENTRY *)0,
                "pop on empty returns NULL");
    TEST_ASSERT(ExInterlockedFlushSList(&h) == (SLIST_ENTRY *)0,
                "flush on empty returns NULL");
    TEST_ASSERT_EQ(ExQueryDepthSList(&h), 0u, "depth still 0 after empty ops");
}

static void test_ex_slist_push_pop_lifo(void)
{
    SLIST_HEADER h;
    SLIST_ENTRY e0, e1, e2;
    ExInitializeSListHead(&h);

    TEST_ASSERT(ExInterlockedPushEntrySList(&h, &e0) == (SLIST_ENTRY *)0,
                "first push returns previous head NULL");
    TEST_ASSERT(ExInterlockedPushEntrySList(&h, &e1) == &e0,
                "second push returns prior head e0");
    TEST_ASSERT(ExInterlockedPushEntrySList(&h, &e2) == &e1,
                "third push returns prior head e1");
    TEST_ASSERT_EQ(ExQueryDepthSList(&h), 3u, "depth == 3 after 3 pushes");

    /* LIFO: last pushed pops first. */
    TEST_ASSERT(ExInterlockedPopEntrySList(&h) == &e2, "pop 1 == e2 (LIFO)");
    TEST_ASSERT(ExInterlockedPopEntrySList(&h) == &e1, "pop 2 == e1");
    TEST_ASSERT_EQ(ExQueryDepthSList(&h), 1u, "depth == 1 after 2 pops");
    TEST_ASSERT(ExInterlockedPopEntrySList(&h) == &e0, "pop 3 == e0");
    TEST_ASSERT_EQ(ExQueryDepthSList(&h), 0u, "depth == 0 when drained");
    TEST_ASSERT(ExInterlockedPopEntrySList(&h) == (SLIST_ENTRY *)0,
                "pop past empty returns NULL");
}

static void test_ex_slist_flush_detaches_chain(void)
{
    SLIST_HEADER h;
    SLIST_ENTRY e[4];
    uint32_t i;
    ExInitializeSListHead(&h);
    for (i = 0; i < 4; i++)
        ExInterlockedPushEntrySList(&h, &e[i]);
    TEST_ASSERT_EQ(ExQueryDepthSList(&h), 4u, "depth == 4 before flush");

    SLIST_ENTRY *chain = ExInterlockedFlushSList(&h);
    TEST_ASSERT(chain == &e[3], "flush returns the current head e[3]");
    TEST_ASSERT_EQ(ExQueryDepthSList(&h), 0u, "depth reset to 0 after flush");
    TEST_ASSERT(ExInterlockedPopEntrySList(&h) == (SLIST_ENTRY *)0,
                "list empty after flush");

    /* The detached chain is walkable e[3]->e[2]->e[1]->e[0]->NULL. */
    uint32_t walked = 0;
    SLIST_ENTRY *p = chain;
    while (p) { walked++; p = p->Next; }
    TEST_ASSERT_EQ(walked, 4u, "detached chain has all 4 entries");
    TEST_ASSERT(chain->Next == &e[2] && chain->Next->Next == &e[1],
                "chain order is reverse-of-push (LIFO)");
}

static void test_ex_slist_reuse_after_drain(void)
{
    /* Push/pop cycles must keep depth honest and the ABA sequence advancing
     * (re-pushing the same entry storage must not corrupt the list). */
    SLIST_HEADER h;
    SLIST_ENTRY a, b;
    ExInitializeSListHead(&h);
    uint32_t round;
    for (round = 0; round < 8; round++) {
        ExInterlockedPushEntrySList(&h, &a);
        ExInterlockedPushEntrySList(&h, &b);
        TEST_ASSERT_EQ(ExQueryDepthSList(&h), 2u, "depth == 2 mid-cycle");
        TEST_ASSERT(ExInterlockedPopEntrySList(&h) == &b, "pop b first");
        TEST_ASSERT(ExInterlockedPopEntrySList(&h) == &a, "pop a second");
        TEST_ASSERT_EQ(ExQueryDepthSList(&h), 0u, "depth == 0 end of cycle");
    }
}

/* ---- S2: concurrent SMP stress (loss/duplication detection) ------------- */

#define STRESS_THREADS  4u
#define STRESS_PER      64u
#define STRESS_TOTAL    (STRESS_THREADS * STRESS_PER)

/* 16-byte aligned shared head (cmpxchg16b requires it). */
static SLIST_HEADER g_stress_slist __attribute__((aligned(16)));
static SLIST_ENTRY  g_stress_nodes[STRESS_TOTAL];
static uint32_t     g_stress_seen[STRESS_TOTAL];   /* atomic per-index pop count */
static volatile uint32_t g_stress_started;          /* start-barrier counter */
static volatile uint32_t g_stress_failed;           /* set if a worker bails */

/* Bounded yield budget for a single pop. Total push == total pop, so a correct
 * DCAS never exhausts it; a LOST node makes one worker unable to reach its
 * quota -- bound + bail so the test FAILS deterministically instead of hanging
 * in thread_join (the bug this suite must catch). Generous vs real contention. */
#define STRESS_POP_BUDGET   1000000u
#define STRESS_START_BUDGET 100000u

/* Each worker waits at a start barrier (maximize contention), pushes its slice
 * of P nodes, then pops P nodes (any nodes, shared LIFO), atomically tallying
 * each popped node's index. Every node is popped exactly once if the DCAS is
 * correct; a lost node leaves seen==0, a duplicate leaves seen>1. */
static void stress_worker(void *arg)
{
    uint32_t t = (uint32_t)(uintptr_t)arg;
    uint32_t base = t * STRESS_PER, i, spins = 0;

    __atomic_fetch_add(&g_stress_started, 1, __ATOMIC_ACQ_REL);
    /* Start barrier (bounded so a never-launched sibling can't hang us). */
    while (__atomic_load_n(&g_stress_started, __ATOMIC_ACQUIRE) < STRESS_THREADS) {
        if (++spins > STRESS_START_BUDGET) break;
        yield();
    }

    for (i = 0; i < STRESS_PER; i++)
        ExInterlockedPushEntrySList(&g_stress_slist, &g_stress_nodes[base + i]);

    for (i = 0; i < STRESS_PER; i++) {
        SLIST_ENTRY *n;
        uint32_t tries = 0;
        while (!(n = ExInterlockedPopEntrySList(&g_stress_slist))) {
            if (++tries > STRESS_POP_BUDGET) {
                /* A node was lost -- bail so the test asserts instead of hangs. */
                __atomic_store_n(&g_stress_failed, 1, __ATOMIC_RELEASE);
                return;
            }
            yield();                                /* list transiently empty */
        }
        uint32_t idx = (uint32_t)(n - g_stress_nodes);
        __atomic_fetch_add(&g_stress_seen[idx], 1, __ATOMIC_ACQ_REL);
    }
}

static void test_ex_slist_smp_stress(void)
{
    uint32_t i;
    int tids[STRESS_THREADS];

    ExInitializeSListHead(&g_stress_slist);
    for (i = 0; i < STRESS_TOTAL; i++)
        g_stress_seen[i] = 0;
    g_stress_started = 0;
    g_stress_failed = 0;

    for (i = 0; i < STRESS_THREADS; i++)
        tids[i] = kthread_create(stress_worker, (void *)(uintptr_t)i, 0);
    for (i = 0; i < STRESS_THREADS; i++) {
        TEST_ASSERT(tids[i] >= 0, "stress kthread_create succeeds");
        if (tids[i] >= 0)
            thread_join((uint32_t)tids[i]);
    }

    TEST_ASSERT_EQ(g_stress_failed, 0u,
                   "no worker exhausted its pop budget (no lost node hang)");

    /* Every node popped exactly once: no loss, no duplication. */
    uint32_t lost = 0, dup = 0;
    for (i = 0; i < STRESS_TOTAL; i++) {
        if (g_stress_seen[i] == 0) lost++;
        else if (g_stress_seen[i] > 1) dup++;
    }
    TEST_ASSERT_EQ(lost, 0u, "no lost nodes across concurrent push/pop");
    TEST_ASSERT_EQ(dup, 0u, "no duplicated nodes across concurrent push/pop");
    TEST_ASSERT_EQ(ExQueryDepthSList(&g_stress_slist), 0u,
                   "list drained (depth 0) after balanced stress");
}

/* ---- S3: Rundown Protection --------------------------------------------- */

static void test_ex_rundown_acquire_release(void)
{
    EX_RUNDOWN_REF r;
    ExInitializeRundownProtection(&r);
    TEST_ASSERT(!ExIsRundownActive(&r), "fresh ref not active");
    TEST_ASSERT(ExAcquireRundownProtection(&r), "acquire 1 succeeds");
    TEST_ASSERT(ExAcquireRundownProtection(&r), "acquire 2 succeeds (nested)");
    ExReleaseRundownProtection(&r);
    ExReleaseRundownProtection(&r);
    TEST_ASSERT(!ExIsRundownActive(&r), "still not active after balanced rel");
}

static void test_ex_rundown_completed_rejects(void)
{
    EX_RUNDOWN_REF r;
    ExInitializeRundownProtection(&r);
    ExRundownCompleted(&r);
    TEST_ASSERT(ExIsRundownActive(&r), "rundown active after Completed");
    TEST_ASSERT(!ExAcquireRundownProtection(&r), "acquire fails once run down");
    ExReInitializeRundownProtection(&r);
    TEST_ASSERT(!ExIsRundownActive(&r), "reinit clears active");
    TEST_ASSERT(ExAcquireRundownProtection(&r), "acquire succeeds after reinit");
    ExReleaseRundownProtection(&r);
}

static void test_ex_rundown_wait_no_refs(void)
{
    EX_RUNDOWN_REF r;
    ExInitializeRundownProtection(&r);
    /* No outstanding refs: wait returns immediately and arms rundown. */
    ExWaitForRundownProtectionRelease(&r);
    TEST_ASSERT(ExIsRundownActive(&r), "wait with no refs arms rundown");
    TEST_ASSERT(!ExAcquireRundownProtection(&r), "acquire fails after wait begins");
}

/* Concurrent drain: a worker holds a reference across several yields, then
 * releases; the main thread's ExWaitForRundownProtectionRelease must block
 * until that release lands (proving the wait drains outstanding refs and that
 * acquire is rejected once the wait has begun). */
static EX_RUNDOWN_REF g_rd_ref;
static volatile uint32_t g_rd_acquired;
static volatile uint32_t g_rd_released;

static void rundown_worker(void *arg)
{
    (void)arg;
    if (!ExAcquireRundownProtection(&g_rd_ref))
        return;                              /* should not happen: armed empty */
    __atomic_store_n(&g_rd_acquired, 1, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < 50; i++)
        yield();                             /* hold the ref a while */
    /* Release THEN publish the marker, so a main-thread wait that returns
     * before the actual release cannot pass the post-wait assertion. */
    ExReleaseRundownProtection(&g_rd_ref);
    __atomic_store_n(&g_rd_released, 1, __ATOMIC_RELEASE);
}

static void test_ex_rundown_concurrent_drain(void)
{
    ExInitializeRundownProtection(&g_rd_ref);
    g_rd_acquired = 0;
    g_rd_released = 0;

    int tid = kthread_create(rundown_worker, (void *)0, 0);
    TEST_ASSERT(tid >= 0, "rundown worker kthread_create succeeds");
    if (tid < 0)
        return;

    /* Wait until the worker holds the reference, so the drain actually blocks. */
    uint32_t spins = 0;
    while (!__atomic_load_n(&g_rd_acquired, __ATOMIC_ACQUIRE)) {
        if (++spins > 1000000u) break;
        yield();
    }
    TEST_ASSERT(__atomic_load_n(&g_rd_acquired, __ATOMIC_ACQUIRE),
                "worker acquired the reference");

    ExWaitForRundownProtectionRelease(&g_rd_ref);
    /* The wait drains at refcount 0, i.e. inside the worker's release, which is
     * immediately before the worker publishes g_rd_released. Bounded-spin for
     * the marker so the assertion proves a real release happened (not just an
     * early return) without racing that publish window. */
    spins = 0;
    while (!__atomic_load_n(&g_rd_released, __ATOMIC_ACQUIRE)) {
        if (++spins > 1000000u) break;
        yield();
    }
    TEST_ASSERT(__atomic_load_n(&g_rd_released, __ATOMIC_ACQUIRE),
                "wait drained the worker's reference (release observed)");
    TEST_ASSERT(!ExAcquireRundownProtection(&g_rd_ref),
                "acquire rejected after rundown began");
    thread_join((uint32_t)tid);
}

/* ---- S4: Callback Objects ----------------------------------------------- */

static volatile uint32_t g_cb_calls;
static void *g_cb_last_ctx;
static void *g_cb_last_a1;
static void *g_cb_last_a2;

static void cb_test_routine(void *ctx, void *a1, void *a2)
{
    __atomic_fetch_add(&g_cb_calls, 1, __ATOMIC_ACQ_REL);
    g_cb_last_ctx = ctx;
    g_cb_last_a1 = a1;
    g_cb_last_a2 = a2;
}

static void test_ex_callback_register_notify(void)
{
    EX_CALLBACK_OBJECT *cb = ExCreateCallback((const char *)0, true, true);
    TEST_ASSERT(cb != (EX_CALLBACK_OBJECT *)0, "anonymous create returns object");
    if (!cb) return;

    g_cb_calls = 0;
    EX_CALLBACK_COOKIE c = ExRegisterCallback(cb, cb_test_routine, (void *)0x1234);
    TEST_ASSERT(c != 0, "register returns non-zero cookie");
    ExNotifyCallback(cb, (void *)0xAA, (void *)0xBB);
    TEST_ASSERT_EQ(g_cb_calls, 1u, "notify invoked the routine once");
    TEST_ASSERT(g_cb_last_ctx == (void *)0x1234, "routine got its context");
    TEST_ASSERT(g_cb_last_a1 == (void *)0xAA && g_cb_last_a2 == (void *)0xBB,
                "routine got both notify args");

    ExUnregisterCallback(cb, c);
    ExNotifyCallback(cb, 0, 0);
    TEST_ASSERT_EQ(g_cb_calls, 1u, "no invoke after unregister");
    ObDereferenceObject(cb);
}

static void test_ex_callback_allow_multiple_false(void)
{
    EX_CALLBACK_OBJECT *cb = ExCreateCallback((const char *)0, true, false);
    if (!cb) { TEST_ASSERT(0, "create"); return; }
    EX_CALLBACK_COOKIE c1 = ExRegisterCallback(cb, cb_test_routine, 0);
    TEST_ASSERT(c1 != 0, "first register succeeds");
    EX_CALLBACK_COOKIE c2 = ExRegisterCallback(cb, cb_test_routine, 0);
    TEST_ASSERT_EQ(c2, 0u, "second register capped (allow_multiple=false)");
    ExUnregisterCallback(cb, c1);
    EX_CALLBACK_COOKIE c3 = ExRegisterCallback(cb, cb_test_routine, 0);
    TEST_ASSERT(c3 != 0, "register succeeds again after unregister");
    ExUnregisterCallback(cb, c3);
    ObDereferenceObject(cb);
}

static void test_ex_callback_multiple_and_enumerate(void)
{
    EX_CALLBACK_OBJECT *cb = ExCreateCallback((const char *)0, true, true);
    if (!cb) { TEST_ASSERT(0, "create"); return; }
    EX_CALLBACK_COOKIE a = ExRegisterCallback(cb, cb_test_routine, (void *)0x1);
    EX_CALLBACK_COOKIE b = ExRegisterCallback(cb, cb_test_routine, (void *)0x2);
    EX_CALLBACK_COOKIE c = ExRegisterCallback(cb, cb_test_routine, (void *)0x3);
    TEST_ASSERT(a && b && c, "three registrations succeed");

    g_cb_calls = 0;
    ExNotifyCallback(cb, 0, 0);
    TEST_ASSERT_EQ(g_cb_calls, 3u, "notify fired all three");

    void *out[EX_CALLBACK_MAX_SLOTS];
    uint32_t n = ExpEnumerateCallback(cb, out, EX_CALLBACK_MAX_SLOTS);
    TEST_ASSERT_EQ(n, 3u, "enumerate returns 3 active");
    ExUnregisterCallback(cb, a);
    ExUnregisterCallback(cb, b);
    ExUnregisterCallback(cb, c);
    ObDereferenceObject(cb);
}

static void test_ex_callback_slot_reuse(void)
{
    /* design re-review F3-redux: a reused slot must re-arm its rundown or it
     * would never dispatch. */
    EX_CALLBACK_OBJECT *cb = ExCreateCallback((const char *)0, true, true);
    if (!cb) { TEST_ASSERT(0, "create"); return; }
    EX_CALLBACK_COOKIE c1 = ExRegisterCallback(cb, cb_test_routine, (void *)0x9);
    ExUnregisterCallback(cb, c1);
    EX_CALLBACK_COOKIE c2 = ExRegisterCallback(cb, cb_test_routine, (void *)0xA);
    TEST_ASSERT(c2 != 0 && c2 != c1, "reused slot gets a fresh cookie");
    g_cb_calls = 0;
    ExNotifyCallback(cb, 0, 0);
    TEST_ASSERT_EQ(g_cb_calls, 1u, "reused slot dispatches (rundown re-armed)");
    ExUnregisterCallback(cb, c2);
    ObDereferenceObject(cb);
}

static void test_ex_callback_stale_cookie_noop(void)
{
    EX_CALLBACK_OBJECT *cb = ExCreateCallback((const char *)0, true, true);
    if (!cb) { TEST_ASSERT(0, "create"); return; }
    EX_CALLBACK_COOKIE c1 = ExRegisterCallback(cb, cb_test_routine, 0);
    ExUnregisterCallback(cb, c1);
    ExUnregisterCallback(cb, c1);   /* stale: must be a safe no-op */
    g_cb_calls = 0;
    ExNotifyCallback(cb, 0, 0);
    TEST_ASSERT_EQ(g_cb_calls, 0u, "stale double-unregister left nothing live");
    ObDereferenceObject(cb);
}

static void test_ex_callback_builtin_named(void)
{
    /* The built-in well-known objects are created at ex_init. */
    EX_CALLBACK_OBJECT *cb = ExCreateCallback("ProcessCreate", false, true);
    TEST_ASSERT(cb != (EX_CALLBACK_OBJECT *)0,
                "built-in \\Callback\\ProcessCreate resolvable");
    if (cb) ObDereferenceObject(cb);
}

/* ---- S5: Lookaside lists ------------------------------------------------ */

static void test_ex_lookaside_cache_hit(void)
{
    NPAGED_LOOKASIDE_LIST la;
    ExInitializeNPagedLookasideList(&la, 64, 0x4C4B5453u /* 'STKL' */, 8);

    void *p1 = ExAllocateFromNPagedLookasideList(&la);
    TEST_ASSERT(p1 != (void *)0, "first alloc (cache miss) returns a block");
    TEST_ASSERT_EQ(la.L.alloc_misses, 1ull, "first alloc counted as a miss");
    TEST_ASSERT_EQ(la.L.alloc_hits, 0ull, "no cache hit yet");

    ExFreeToNPagedLookasideList(&la, p1);
    TEST_ASSERT_EQ(la.L.free_hits, 1ull, "free under cap cached (not drained)");

    void *p2 = ExAllocateFromNPagedLookasideList(&la);
    TEST_ASSERT(p2 == p1, "free-then-alloc returns the cached block (LIFO)");
    TEST_ASSERT_EQ(la.L.alloc_hits, 1ull, "second alloc served from cache");

    ExFreeToNPagedLookasideList(&la, p2);
    ExDeleteNPagedLookasideList(&la);
}

static void test_ex_lookaside_depth_cap(void)
{
    NPAGED_LOOKASIDE_LIST la;
    ExInitializeNPagedLookasideList(&la, 48, 0x50414331u /* 'CAP1' */, 2);

    void *p[5];
    for (int i = 0; i < 5; i++) {
        p[i] = ExAllocateFromNPagedLookasideList(&la);
        TEST_ASSERT(p[i] != (void *)0, "alloc returns a block");
    }
    for (int i = 0; i < 5; i++)
        ExFreeToNPagedLookasideList(&la, p[i]);

    /* depth cap 2: first two frees cache, the next three each push-then-drain. */
    TEST_ASSERT_EQ(la.L.free_drains, 3ull, "3 over-cap frees drained to backing");
    TEST_ASSERT_EQ(la.L.free_hits, 2ull, "2 frees retained in cache");
    TEST_ASSERT(ExQueryDepthSList(la.L.free_list) <= 2u, "cache depth honored at cap");

    ExDeleteNPagedLookasideList(&la);
}

static void test_ex_lookaside_paged_rejects_dispatch(void)
{
    PAGED_LOOKASIDE_LIST la;
    ExInitializePagedLookasideList(&la, 64, 0x47504431u /* '1DPG' */, 4);

    /* Verifier OFF: a DISPATCH_LEVEL caller must be refused with NULL, not a
     * bugcheck (paged memory may fault above APC_LEVEL). */
    KIRQL old;
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    void *p = ExAllocateFromPagedLookasideList(&la);
    KeLowerIrql(old);

    TEST_ASSERT(p == (void *)0, "paged alloc at DISPATCH_LEVEL returns NULL");

    /* At PASSIVE_LEVEL it works. */
    p = ExAllocateFromPagedLookasideList(&la);
    TEST_ASSERT(p != (void *)0, "paged alloc at PASSIVE_LEVEL succeeds");
    ExFreeToPagedLookasideList(&la, p);
    ExDeletePagedLookasideList(&la);
}

struct la_ctx { uint32_t allocs; uint32_t frees; };

static void *la_test_alloc(size_t size, uint32_t tag, void *ctx)
{
    (void)tag;
    struct la_ctx *c = (struct la_ctx *)ctx;
    c->allocs++;
    return kmalloc(size);
}

static void la_test_free(void *block, void *ctx)
{
    struct la_ctx *c = (struct la_ctx *)ctx;
    c->frees++;
    kfree(block);
}

static void test_ex_lookaside_ex_custom_backing(void)
{
    struct la_ctx ctx = { 0, 0 };
    LOOKASIDE_LIST_EX la;
    int rc = ExInitializeLookasideListEx(&la, la_test_alloc, la_test_free, &ctx,
                                         false, 80, 0x5453554Cu /* 'LUST' */, 4);
    TEST_ASSERT_EQ((uint64_t)rc, 0ull, "EX init with custom backing succeeds");

    void *p = ExAllocateFromLookasideListEx(&la);   /* miss -> custom alloc */
    TEST_ASSERT(p != (void *)0, "EX alloc returns a block via custom allocator");
    TEST_ASSERT_EQ((uint64_t)ctx.allocs, 1ull, "custom alloc invoked with private context");

    ExFreeToLookasideListEx(&la, p);                /* cached, custom free NOT called */
    TEST_ASSERT_EQ((uint64_t)ctx.frees, 0ull, "cached free does not hit custom free");

    ExDeleteLookasideListEx(&la);                   /* drains cache -> custom free */
    TEST_ASSERT_EQ((uint64_t)ctx.frees, 1ull, "delete drains cache via custom free");
}

static void test_ex_lookaside_nonpaged_dispatch(void)
{
    NPAGED_LOOKASIDE_LIST la;
    ExInitializeNPagedLookasideList(&la, 64, 0x50534944u /* 'DISP' */, 8);

    /* Prewarm one cached entry at PASSIVE_LEVEL. */
    void *p = ExAllocateFromNPagedLookasideList(&la);
    TEST_ASSERT(p != (void *)0, "prewarm alloc succeeds");
    ExFreeToNPagedLookasideList(&la, p);

    KIRQL old;
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    void *hit = ExAllocateFromNPagedLookasideList(&la);   /* cache hit: legal at DISPATCH */
    void *miss = ExAllocateFromNPagedLookasideList(&la);  /* empty: must NOT touch heap */
    KeLowerIrql(old);

    TEST_ASSERT(hit == p, "nonpaged cache hit succeeds at DISPATCH_LEVEL");
    TEST_ASSERT(miss == (void *)0, "nonpaged miss at DISPATCH_LEVEL returns NULL (no heap)");

    ExFreeToNPagedLookasideList(&la, hit);
    ExDeleteNPagedLookasideList(&la);
}

static void test_ex_lookaside_paged_free_rejects_dispatch(void)
{
    PAGED_LOOKASIDE_LIST la;
    ExInitializePagedLookasideList(&la, 64, 0x46474450u /* 'PDGF' */, 4);

    void *p = ExAllocateFromPagedLookasideList(&la);   /* PASSIVE: succeeds */
    TEST_ASSERT(p != (void *)0, "paged alloc at PASSIVE succeeds");

    KIRQL old;
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    ExFreeToPagedLookasideList(&la, p);                /* rejected: paged > APC */
    KIRQL depth_at_dispatch = (KIRQL)ExQueryDepthSList(la.L.free_list);
    KeLowerIrql(old);

    TEST_ASSERT_EQ(la.L.free_hits + la.L.free_drains, 0ull,
                   "paged free at DISPATCH_LEVEL did not touch the list");
    TEST_ASSERT_EQ((uint64_t)depth_at_dispatch, 0ull,
                   "paged free at DISPATCH_LEVEL did not cache the entry");

    ExFreeToPagedLookasideList(&la, p);               /* PASSIVE: now legal */
    ExDeletePagedLookasideList(&la);
}

static void test_ex_lookaside_ex_badargs_and_flush(void)
{
    struct la_ctx ctx = { 0, 0 };
    LOOKASIDE_LIST_EX la;

    TEST_ASSERT(ExInitializeLookasideListEx((LOOKASIDE_LIST_EX *)0, 0, 0, 0,
                                            false, 64, 0, 0) != 0,
                "EX init rejects NULL list");
    TEST_ASSERT(ExInitializeLookasideListEx(&la, 0, 0, 0, false, 0, 0, 0) != 0,
                "EX init rejects zero size");
    TEST_ASSERT(ExInitializeLookasideListEx(&la, la_test_alloc, 0, &ctx,
                                            false, 64, 0, 0) != 0,
                "EX init rejects alloc-only custom pair");
    TEST_ASSERT(ExInitializeLookasideListEx(&la, 0, la_test_free, &ctx,
                                            false, 64, 0, 0) != 0,
                "EX init rejects free-only custom pair");
    TEST_ASSERT(ExInitializeLookasideListEx(&la, 0, 0, 0, false,
                                            EX_LOOKASIDE_MAX_ALLOC + 1, 0, 0) != 0,
                "EX init rejects oversized entry (kmalloc overflow guard)");

    /* Valid init with default depth + min-size clamp (request below the link). */
    int rc = ExInitializeLookasideListEx(&la, la_test_alloc, la_test_free, &ctx,
                                         false, 4, 0x48534C46u /* 'FLSH' */, 0);
    TEST_ASSERT_EQ((uint64_t)rc, 0ull, "EX init with valid args succeeds");
    TEST_ASSERT(la.L.size >= sizeof(SLIST_ENTRY), "size clamped to >= SLIST link");
    TEST_ASSERT_EQ((uint64_t)la.L.max_depth, (uint64_t)EX_LOOKASIDE_DEFAULT_DEPTH,
                   "depth 0 -> default cap");

    void *p = ExAllocateFromLookasideListEx(&la);   /* miss -> custom alloc */
    ExFreeToLookasideListEx(&la, p);                /* cached */
    TEST_ASSERT(ExQueryDepthSList(la.L.free_list) == 1u, "one entry cached before flush");

    ExFlushLookasideListEx(&la);                    /* drains cache, keeps usable */
    TEST_ASSERT_EQ((uint64_t)ctx.frees, 1ull, "flush drained the cached entry via custom free");
    TEST_ASSERT(ExQueryDepthSList(la.L.free_list) == 0u, "cache empty after flush");
    TEST_ASSERT(la.L.initialized, "list still initialized after flush");

    void *p2 = ExAllocateFromLookasideListEx(&la);  /* still usable */
    TEST_ASSERT(p2 != (void *)0, "alloc succeeds after flush");
    ExFreeToLookasideListEx(&la, p2);
    ExDeleteLookasideListEx(&la);
}

static void test_ex_lookaside_depth_clamp(void)
{
    /* A depth at/above the SLIST saturation point would disable trimming
     * (depth field saturates at SLIST_DEPTH_MAX, so `depth > max_depth` could
     * never become true). Init must clamp it below saturation. */
    NPAGED_LOOKASIDE_LIST la;
    ExInitializeNPagedLookasideList(&la, 64, 0x504D4C43u /* 'CLMP' */, SLIST_DEPTH_MAX);
    TEST_ASSERT_EQ((uint64_t)la.L.max_depth, (uint64_t)EX_LOOKASIDE_MAX_DEPTH,
                   "oversized depth clamped below SLIST saturation");
    ExDeleteNPagedLookasideList(&la);
}

static void test_ex_lookaside_failed_init_inert(void)
{
    /* Pre-dirty the storage so a failed init that forgot to zero would leave a
     * non-zero `initialized` and a garbage free_list. */
    NPAGED_LOOKASIDE_LIST la;
    for (size_t i = 0; i < sizeof(la); i++)
        ((volatile uint8_t *)&la)[i] = 0xCCu;

    /* Oversized request: the void initializer discards the return, so the list
     * must be forced inert. */
    ExInitializeNPagedLookasideList(&la, EX_LOOKASIDE_MAX_ALLOC + 1, 0x44414231u, 8);
    TEST_ASSERT(!la.L.initialized, "failed oversized init leaves list inert");

    /* All API calls on the inert list must be safe no-ops (never touch a bogus
     * SLIST head). */
    TEST_ASSERT(ExAllocateFromNPagedLookasideList(&la) == (void *)0,
                "alloc on inert list returns NULL");
    ExFreeToNPagedLookasideList(&la, (void *)0x1000);   /* no-op: not initialized */
    ExDeleteNPagedLookasideList(&la);                   /* no-op: not initialized */
    TEST_ASSERT(!la.L.initialized, "inert list stays inert");
}

static void test_ex_lookaside_delete_at_dispatch_recovers(void)
{
    NPAGED_LOOKASIDE_LIST la;
    ExInitializeNPagedLookasideList(&la, 64, 0x4C454444u /* 'DDEL' */, 8);

    void *p = ExAllocateFromNPagedLookasideList(&la);
    ExFreeToNPagedLookasideList(&la, p);            /* cached */
    TEST_ASSERT(ExQueryDepthSList(la.L.free_list) == 1u, "one entry cached");

    /* Delete at DISPATCH_LEVEL cannot drain (backing free is <= APC_LEVEL); the
     * list must stay intact and recoverable, not strand the cached entry. */
    KIRQL old;
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    ExDeleteNPagedLookasideList(&la);
    KeLowerIrql(old);

    TEST_ASSERT(la.L.initialized, "delete at DISPATCH left list initialized");
    TEST_ASSERT(ExQueryDepthSList(la.L.free_list) == 1u, "cached entry still reachable");

    /* A legal delete at PASSIVE_LEVEL drains and invalidates. */
    ExDeleteNPagedLookasideList(&la);
    TEST_ASSERT(!la.L.initialized, "delete at PASSIVE invalidated the list");
}

static void test_ex_lookaside_verifier_uaf(void)
{
    ExpSetLookasideVerifier(true);

    NPAGED_LOOKASIDE_LIST la;
    ExInitializeNPagedLookasideList(&la, 64, 0x46414C56u /* 'VLAF' */, 8);

    void *p = ExAllocateFromNPagedLookasideList(&la);
    TEST_ASSERT(p != (void *)0, "alloc returns a block");
    ExFreeToNPagedLookasideList(&la, p);            /* body poisoned past the link */

    uint64_t before = ExpLookasideUafCount();
    /* Simulate a write-after-free into the cached entry body (past the SLIST
     * link in the first 8 bytes, inside the poisoned region). */
    ((volatile uint8_t *)p)[16] = 0xFFu;

    void *p2 = ExAllocateFromNPagedLookasideList(&la);  /* hit -> poison re-check */
    TEST_ASSERT(p2 == p, "verifier alloc still returns the cached block");
    TEST_ASSERT_EQ(ExpLookasideUafCount(), before + 1ull,
                   "verifier detected the use-after-free on the poisoned body");

    ExFreeToNPagedLookasideList(&la, p2);
    ExDeleteNPagedLookasideList(&la);
    ExpSetLookasideVerifier(false);
}

/* ---- S6: Fast References ------------------------------------------------- */

/* Two 8-byte-aligned dummy "objects" -- the fast-ref cache mechanic operates on
 * opaque aligned pointers (the Ob ref/deref is the caller's, exercised by the
 * boolean contract below, not by a live refcount here). uint64_t storage is
 * 8-byte aligned, satisfying EX_FAST_REF_MASK. The misalignment bugcheck path
 * is intentionally NOT unit-tested: ExpFastRefPack calls KeBugCheckEx, which
 * would halt the run (same reason ex_slist's misaligned-head bugcheck is
 * untested); the _Static_assert in ex.h pins the 3-bit count field instead. */
static uint64_t g_fr_obj_a;
static uint64_t g_fr_obj_b;

static void test_ex_fastref_pack_unpack_roundtrip(void)
{
    void *obj = &g_fr_obj_a;
    uintptr_t c;

    TEST_ASSERT((((uintptr_t)obj) & EX_FAST_REF_MASK) == 0,
                "dummy object is 8-byte aligned");

    for (c = 0; c <= EX_FAST_REF_MAX; c++) {
        uintptr_t packed = ExpFastRefPack(obj, c);
        TEST_ASSERT(ExpFastRefUnpackObject(packed) == obj,
                    "unpack recovers the exact object pointer");
        TEST_ASSERT_EQ(ExpFastRefUnpackCount(packed), c,
                       "unpack recovers the exact cached count");
    }
    /* NULL object packs to an empty (zero) value. */
    TEST_ASSERT_EQ(ExpFastRefPack((void *)0, 0), (uintptr_t)0,
                   "NULL object + count 0 packs to empty");
    TEST_ASSERT(ExpFastRefUnpackObject(0) == (void *)0,
                "empty value unpacks to NULL object");
}

static void test_ex_fastref_init_acquire_empty_slowpath(void)
{
    EX_FAST_REF ref;
    EX_FAST_REF_RESULT r;

    /* Init with an object, cache empty: acquire must signal the SLOW path
     * (object present, cached=false) so the caller does ObReferenceObjectSafe. */
    ExInitializeFastReference(&ref, &g_fr_obj_a);
    r = ExAcquireFastReference(&ref);
    TEST_ASSERT(r.object == &g_fr_obj_a, "acquire returns the stored object");
    TEST_ASSERT(r.cached == false, "empty cache -> slow path (cached=false)");

    /* Init empty (NULL): acquire returns the empty sentinel. */
    ExInitializeFastReference(&ref, (void *)0);
    r = ExAcquireFastReference(&ref);
    TEST_ASSERT(r.object == (void *)0, "empty ref acquire returns NULL object");
    TEST_ASSERT(r.cached == false, "empty ref acquire is not a cached hit");
    TEST_ASSERT(ExGetObjectFastReference(&ref) == (void *)0,
                "get-object on empty ref is NULL");
}

static void test_ex_fastref_release_saturation_then_drain(void)
{
    EX_FAST_REF ref;
    EX_FAST_REF_RESULT r;
    uintptr_t i;

    ExInitializeFastReference(&ref, &g_fr_obj_a);

    /* Fill the cache: MAX releases are absorbed (true); the next is rejected
     * (false) because the count saturated -- the "falls back to full Ob ref"
     * (caller must ObDereferenceObject) path the test checkpoint requires. */
    for (i = 0; i < EX_FAST_REF_MAX; i++)
        TEST_ASSERT(ExReleaseFastReference(&ref, &g_fr_obj_a) == true,
                    "release below cap is absorbed into the cache");
    TEST_ASSERT(ExReleaseFastReference(&ref, &g_fr_obj_a) == false,
                "release at saturation falls back to Ob deref (false)");

    /* get-object does not consume a cached reference. */
    TEST_ASSERT(ExGetObjectFastReference(&ref) == &g_fr_obj_a,
                "get-object snapshots without draining the cache");

    /* Drain: MAX cached hits (true), then the cache empties to the slow path. */
    for (i = 0; i < EX_FAST_REF_MAX; i++) {
        r = ExAcquireFastReference(&ref);
        TEST_ASSERT(r.object == &g_fr_obj_a && r.cached == true,
                    "acquire hands out a cached reference (fast path)");
    }
    r = ExAcquireFastReference(&ref);
    TEST_ASSERT(r.object == &g_fr_obj_a && r.cached == false,
                "cache drained -> back to slow path");
}

static void test_ex_fastref_release_object_mismatch(void)
{
    EX_FAST_REF ref;

    /* Releasing the WRONG object must not be absorbed (the ref was logically
     * swapped under the caller) -- caller must deref it itself. */
    ExInitializeFastReference(&ref, &g_fr_obj_a);
    TEST_ASSERT(ExReleaseFastReference(&ref, &g_fr_obj_b) == false,
                "release of a non-matching object returns false");
    /* The cache state for the real object is untouched. */
    TEST_ASSERT(ExReleaseFastReference(&ref, &g_fr_obj_a) == true,
                "release of the matching object still absorbs");
}

static void test_ex_fastref_release_null_rejected(void)
{
    EX_FAST_REF ref;

    /* Releasing NULL must never be absorbed: it would CAS an empty ref to a
     * phantom (NULL, count=1) state. Reject and leave the word untouched. */
    ExInitializeFastReference(&ref, (void *)0);
    TEST_ASSERT(ExReleaseFastReference(&ref, (void *)0) == false,
                "release of NULL on empty ref returns false");
    TEST_ASSERT(ExGetObjectFastReference(&ref) == (void *)0,
                "empty ref stays empty after a NULL release");

    /* Also rejected when the ref holds a real object. */
    ExInitializeFastReference(&ref, &g_fr_obj_a);
    TEST_ASSERT(ExReleaseFastReference(&ref, (void *)0) == false,
                "release of NULL on a populated ref returns false");
    TEST_ASSERT(ExGetObjectFastReference(&ref) == &g_fr_obj_a,
                "populated ref unchanged after a NULL release");
}

static void test_ex_fastref_compare_swap(void)
{
    EX_FAST_REF ref;
    uintptr_t old_count = 0xDEAD;

    ExInitializeFastReference(&ref, &g_fr_obj_a);
    /* Cache two references on A so the swap reports a non-zero old count. */
    TEST_ASSERT(ExReleaseFastReference(&ref, &g_fr_obj_a) == true, "cache A #1");
    TEST_ASSERT(ExReleaseFastReference(&ref, &g_fr_obj_a) == true, "cache A #2");

    /* Wrong expected object: no swap. */
    TEST_ASSERT(ExCompareSwapFastReference(&ref, &g_fr_obj_b, &g_fr_obj_b,
                                           &old_count) == false,
                "swap with mismatched old_object fails");
    TEST_ASSERT(ExGetObjectFastReference(&ref) == &g_fr_obj_a,
                "failed swap leaves the object unchanged");

    /* Correct expected object: swap A -> B, old count surfaced for balancing. */
    TEST_ASSERT(ExCompareSwapFastReference(&ref, &g_fr_obj_b, &g_fr_obj_a,
                                           &old_count) == true,
                "swap A -> B succeeds on matching old_object");
    TEST_ASSERT_EQ(old_count, 2u, "swapped-out cached count surfaced (=2)");
    TEST_ASSERT(ExGetObjectFastReference(&ref) == &g_fr_obj_b,
                "object is now B with a fresh (0) cache");

    /* Swap B -> NULL clears the ref. */
    TEST_ASSERT(ExCompareSwapFastReference(&ref, (void *)0, &g_fr_obj_b,
                                           &old_count) == true,
                "swap B -> NULL clears the ref");
    TEST_ASSERT_EQ(old_count, 0u, "fresh B cache had 0 references");
    TEST_ASSERT(ExGetObjectFastReference(&ref) == (void *)0,
                "ref is empty after swap to NULL");
}

/* SMP stress: hammer one EX_FAST_REF from FR_STRESS_THREADS kthreads doing
 * balanced acquire/release, exercising the lock-free CAS retry paths (which the
 * single-threaded tests above never reach) and proving reference conservation:
 * no torn count, no lost update, no duplicated/leaked cached reference. Using
 * MORE threads than EX_FAST_REF_MAX guarantees the cache occasionally empties,
 * so the cnt==0 slow path fires too. Conservation model: the cache starts full
 * (MAX cached refs); each cached acquire moves one ref into a worker's hand and
 * its release returns it, so (cache + refs-in-hands) == MAX always holds. */
#define FR_STRESS_THREADS   8u    /* > EX_FAST_REF_MAX (7) -> empties the cache */
#define FR_STRESS_ITERS     20000u
#define FR_STRESS_START_BUDGET 100000u

static uint64_t           g_fr_stress_obj;       /* 8-aligned shared object */
static EX_FAST_REF        g_fr_stress_ref;
static volatile uint32_t  g_fr_started;
static volatile uint32_t  g_fr_bad_object;       /* a torn/corrupt pointer seen */
static volatile uint32_t  g_fr_overflow;         /* refs-in-hands exceeded MAX */
static volatile uint32_t  g_fr_release_fail;     /* release saw saturation (bug) */
static volatile int32_t   g_fr_held;             /* cached refs in worker hands */

static void fr_stress_worker(void *arg)
{
    uint32_t i, spins = 0;
    (void)arg;

    __atomic_fetch_add(&g_fr_started, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&g_fr_started, __ATOMIC_ACQUIRE) < FR_STRESS_THREADS) {
        if (++spins > FR_STRESS_START_BUDGET) break;
        yield();
    }

    for (i = 0; i < FR_STRESS_ITERS; i++) {
        EX_FAST_REF_RESULT r = ExAcquireFastReference(&g_fr_stress_ref);

        /* The word must never expose anything but the one shared object (or
         * NULL when drained) -- a torn count must not bleed into the pointer. */
        if (r.object != (void *)0 && r.object != &g_fr_stress_obj) {
            __atomic_store_n(&g_fr_bad_object, 1, __ATOMIC_RELEASE);
            return;
        }
        if (r.cached) {
            int32_t h = __atomic_add_fetch(&g_fr_held, 1, __ATOMIC_ACQ_REL);
            if ((uintptr_t)h > EX_FAST_REF_MAX)
                __atomic_store_n(&g_fr_overflow, 1, __ATOMIC_RELEASE);
            /* Return the cached reference. Under conservation the cache can be
             * at most MAX-1 while we hold one, so release must absorb it. */
            if (!ExReleaseFastReference(&g_fr_stress_ref, &g_fr_stress_obj))
                __atomic_store_n(&g_fr_release_fail, 1, __ATOMIC_RELEASE);
            __atomic_sub_fetch(&g_fr_held, 1, __ATOMIC_ACQ_REL);
        }
        /* r.cached == false: the cache was momentarily empty (siblings hold all
         * MAX refs) -- the slow path; nothing to account, just keep hammering. */
    }
}

static void test_ex_fastref_smp_stress(void)
{
    uint32_t i, drained;
    int tids[FR_STRESS_THREADS];
    EX_FAST_REF_RESULT r;

    ExInitializeFastReference(&g_fr_stress_ref, &g_fr_stress_obj);
    /* Pre-fill the cache to MAX (the conserved reference pool). */
    for (i = 0; i < EX_FAST_REF_MAX; i++)
        TEST_ASSERT(ExReleaseFastReference(&g_fr_stress_ref, &g_fr_stress_obj),
                    "stress setup fills the cache to MAX");
    g_fr_started = 0;
    g_fr_bad_object = 0;
    g_fr_overflow = 0;
    g_fr_release_fail = 0;
    g_fr_held = 0;

    for (i = 0; i < FR_STRESS_THREADS; i++)
        tids[i] = kthread_create(fr_stress_worker, (void *)(uintptr_t)i, 0);
    for (i = 0; i < FR_STRESS_THREADS; i++) {
        TEST_ASSERT(tids[i] >= 0, "fastref stress kthread_create succeeds");
        if (tids[i] >= 0)
            thread_join((uint32_t)tids[i]);
    }

    TEST_ASSERT_EQ(g_fr_bad_object, 0u,
                   "no torn/corrupt object pointer observed under contention");
    TEST_ASSERT_EQ(g_fr_overflow, 0u,
                   "refs-in-hands never exceeded MAX (no double hand-out)");
    TEST_ASSERT_EQ(g_fr_release_fail, 0u,
                   "release always absorbed (conservation held, no lost update)");
    TEST_ASSERT_EQ((uint32_t)g_fr_held, 0u, "all cached refs returned to the ref");

    /* Drain the final cache: it must hold exactly MAX again -- proving no cached
     * reference was lost or duplicated across the whole concurrent run. */
    drained = 0;
    for (;;) {
        r = ExAcquireFastReference(&g_fr_stress_ref);
        if (!r.cached) break;
        TEST_ASSERT(r.object == &g_fr_stress_obj, "drained ref is the shared object");
        if (++drained > EX_FAST_REF_MAX + 4) break;   /* guard against a runaway */
    }
    TEST_ASSERT_EQ(drained, (uint32_t)EX_FAST_REF_MAX,
                   "cache restored to exactly MAX (no leaked/duplicated refs)");
}

/* ---- S7: RTL_BITMAP ----------------------------------------------------- */

static void test_ex_bitmap_basic_and_padding(void)
{
    /* Sizes spanning word boundaries + the padding edge (design-review axis). */
    static const uint32_t sizes[] = { 0u, 1u, 31u, 32u, 33u, 1000u };
    uint32_t buf[RTL_BITMAP_WORDS(1000)];
    RTL_BITMAP bm;
    uint32_t si;

    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        uint32_t size = sizes[si];
        RtlInitializeBitMap(&bm, buf, size);
        RtlClearAllBits(&bm);
        TEST_ASSERT_EQ(RtlNumberOfSetBits(&bm), 0u, "cleared bitmap has 0 set bits");
        TEST_ASSERT_EQ(RtlNumberOfClearBits(&bm), size, "all bits clear == size");

        RtlSetAllBits(&bm);
        /* Padding must NOT be counted: NumberOfSetBits == size exactly. */
        TEST_ASSERT_EQ(RtlNumberOfSetBits(&bm), size,
                       "set-all counts exactly SizeOfBitMap (no padding)");
        if (size > 0)
            TEST_ASSERT(RtlAreBitsSet(&bm, 0, size), "all valid bits read set");
        /* A bit at the padding index is out of range -> reads false, never set. */
        TEST_ASSERT(!RtlTestBit(&bm, size), "out-of-range bit reads false");
        TEST_ASSERT(!RtlAreBitsSet(&bm, size, 1), "out-of-range AreBitsSet false");
        TEST_ASSERT_EQ(RtlFindClearBits(&bm, 1, 0), RTL_BITMAP_NOT_FOUND,
                       "no clear bit in a fully-set bitmap");
    }
}

static void test_ex_bitmap_runs(void)
{
    uint32_t buf[RTL_BITMAP_WORDS(1000)];
    RTL_BITMAP bm;
    uint32_t r;

    RtlInitializeBitMap(&bm, buf, 1000);
    RtlClearAllBits(&bm);

    /* First run of 10 clear bits is at index 0. */
    TEST_ASSERT_EQ(RtlFindClearBits(&bm, 10, 0), 0u, "first clear run at 0");

    /* Carve out [0,40) set, [40,45) set, leave a clear gap, force the finder to
     * return a run that crosses the 32-bit word boundary. */
    RtlSetBits(&bm, 0, 40);
    TEST_ASSERT(RtlAreBitsSet(&bm, 0, 40), "set range [0,40) all set");
    TEST_ASSERT(RtlAreBitsClear(&bm, 40, 960), "rest still clear");
    /* The first 20-bit clear run now starts at 40 (spans words 1->2). */
    r = RtlFindClearBits(&bm, 20, 0);
    TEST_ASSERT_EQ(r, 40u, "clear run after the set prefix starts at 40");

    /* Find-and-set allocates the run and advances the next allocation. */
    r = RtlFindClearBitsAndSet(&bm, 8, 0);
    TEST_ASSERT_EQ(r, 40u, "find-and-set returns 40");
    TEST_ASSERT(RtlAreBitsSet(&bm, 40, 8), "the found run is now set");
    r = RtlFindClearBitsAndSet(&bm, 8, 0);
    TEST_ASSERT_EQ(r, 48u, "next find-and-set returns 48 (after the allocated run)");

    /* A run larger than the whole bitmap can never be found. */
    TEST_ASSERT_EQ(RtlFindClearBits(&bm, 2000, 0), RTL_BITMAP_NOT_FOUND,
                   "run larger than the bitmap is NOT_FOUND");

    /* RtlClearBits round-trips with RtlSetBits. */
    RtlClearBits(&bm, 40, 16);
    TEST_ASSERT(RtlAreBitsClear(&bm, 40, 16), "cleared range reads clear");
}

static void test_ex_bitmap_oob_and_large(void)
{
    uint32_t buf[RTL_BITMAP_WORDS(1000)];
    uint32_t big[RTL_BITMAP_WORDS(4096)];
    RTL_BITMAP bm;
    uint32_t r;

    /* Partial out-of-range range writes are a NO-OP (whole range rejected, never
     * clamped) -- a stale/oversized request must not mutate the valid tail. */
    RtlInitializeBitMap(&bm, buf, 1000);
    RtlClearAllBits(&bm);
    RtlSetBits(&bm, 999, 2);          /* [999, 1001) exceeds 1000 -> reject */
    TEST_ASSERT(!RtlTestBit(&bm, 999), "partial-OOB SetBits left bit 999 clear");
    TEST_ASSERT_EQ(RtlNumberOfSetBits(&bm), 0u, "partial-OOB SetBits set nothing");
    RtlSetAllBits(&bm);
    RtlClearBits(&bm, 995, 100);      /* exceeds 1000 -> reject */
    TEST_ASSERT(RtlTestBit(&bm, 999), "partial-OOB ClearBits left bit 999 set");
    TEST_ASSERT_EQ(RtlNumberOfSetBits(&bm), 1000u, "partial-OOB ClearBits cleared nothing");

    /* Large mostly-full bitmap with a non-zero hint: the only clear run is in the
     * wrap region [0, hint). The word-aware finder + bounded wrap must locate it. */
    RtlInitializeBitMap(&bm, big, 4096);
    RtlSetAllBits(&bm);
    RtlClearBits(&bm, 100, 8);        /* the single clear run, before the hint */
    r = RtlFindClearBits(&bm, 8, 2000);   /* hint past the run -> forces the wrap */
    TEST_ASSERT_EQ(r, 100u, "word-aware finder locates the wrap-region clear run");
    /* No 9-bit clear run exists anywhere. */
    TEST_ASSERT_EQ(RtlFindClearBits(&bm, 9, 0), RTL_BITMAP_NOT_FOUND,
                   "no run larger than the single 8-bit gap");
    /* A run spanning a word boundary in a mostly-set map. */
    RtlClearBits(&bm, 60, 10);        /* [60,70) crosses the 32-bit boundary */
    r = RtlFindClearBits(&bm, 10, 0);
    TEST_ASSERT_EQ(r, 60u, "finds the word-crossing clear run at 60");
}

/* ---- S7: RTL_AVL_TABLE --------------------------------------------------- */

typedef struct { uint64_t key; uint64_t payload; } avl_elem_t;

static RTL_GENERIC_COMPARE_RESULTS avl_cmp(RTL_AVL_TABLE *t, void *a, void *b)
{
    uint64_t ka = ((avl_elem_t *)a)->key, kb = ((avl_elem_t *)b)->key;
    (void)t;
    if (ka < kb) return RtlGenericLessThan;
    if (ka > kb) return RtlGenericGreaterThan;
    return RtlGenericEqual;
}
static void *avl_alloc(RTL_AVL_TABLE *t, uint32_t size) { (void)t; return kmalloc(size); }
static void  avl_free(RTL_AVL_TABLE *t, void *b) { (void)t; kfree(b); }

#define AVL_N 1000u

static void test_ex_avl_thousand_keys(void)
{
    RTL_AVL_TABLE tbl;
    uint32_t i, count;
    uint64_t prev;
    void *body;
    bool isnew;

    RtlInitializeGenericTableAvl(&tbl, avl_cmp, avl_alloc, avl_free, (void *)0);
    TEST_ASSERT(RtlIsGenericTableEmptyAvl(&tbl), "fresh table is empty");

    /* Insert 0..N-1 in ASCENDING order -- the worst case for an unbalanced BST,
     * so it directly proves the AVL rebalancing keeps the height logarithmic. */
    for (i = 0; i < AVL_N; i++) {
        avl_elem_t e; e.key = i; e.payload = i * 7u + 1u;
        body = RtlInsertElementGenericTableAvl(&tbl, &e, (uint32_t)sizeof(e), &isnew);
        TEST_ASSERT(body != (void *)0 && isnew, "insert returns a new element body");
    }
    TEST_ASSERT_EQ(RtlNumberGenericTableElementsAvl(&tbl), AVL_N, "1000 elements present");

    /* Height bound: AVL guarantees height <= ~1.44*log2(n+2) ~= 15 nodes for
     * n=1000; a degenerate (unbalanced) tree would be height 1000. */
    TEST_ASSERT(tbl.Root->Height <= 15,
                "AVL height stays logarithmic (<= 15 for 1000 keys)");

    /* Duplicate insert returns the existing element, does not grow the table. */
    {
        avl_elem_t dup; dup.key = 500; dup.payload = 0;
        body = RtlInsertElementGenericTableAvl(&tbl, &dup, (uint32_t)sizeof(dup), &isnew);
        TEST_ASSERT(body != (void *)0 && !isnew, "duplicate insert is not new");
        TEST_ASSERT_EQ(((avl_elem_t *)body)->payload, 500u * 7u + 1u,
                       "duplicate returns the ORIGINAL element (payload intact)");
        TEST_ASSERT_EQ(RtlNumberGenericTableElementsAvl(&tbl), AVL_N,
                       "duplicate insert did not grow the table");
    }

    /* Lookup every key. */
    for (i = 0; i < AVL_N; i++) {
        avl_elem_t k; k.key = i; k.payload = 0;
        body = RtlLookupElementGenericTableAvl(&tbl, &k);
        TEST_ASSERT(body != (void *)0 && ((avl_elem_t *)body)->key == i,
                    "lookup finds each inserted key");
    }

    /* Enumerate yields strictly ascending order. */
    count = 0; prev = 0;
    body = RtlEnumerateGenericTableAvl(&tbl, true);
    while (body) {
        uint64_t k = ((avl_elem_t *)body)->key;
        if (count > 0)
            TEST_ASSERT(k > prev, "enumerate is strictly ascending (sorted)");
        prev = k; count++;
        body = RtlEnumerateGenericTableAvl(&tbl, false);
    }
    TEST_ASSERT_EQ(count, AVL_N, "enumerate visited every element once");

    /* Delete the even keys; the odds must remain and stay balanced. */
    for (i = 0; i < AVL_N; i += 2) {
        avl_elem_t k; k.key = i; k.payload = 0;
        TEST_ASSERT(RtlDeleteElementGenericTableAvl(&tbl, &k), "delete even key");
    }
    TEST_ASSERT_EQ(RtlNumberGenericTableElementsAvl(&tbl), AVL_N / 2,
                   "half the elements remain after deleting evens");
    TEST_ASSERT(tbl.Root->Height <= 14, "height still logarithmic after deletes");
    for (i = 1; i < AVL_N; i += 2) {
        avl_elem_t k; k.key = i; k.payload = 0;
        body = RtlLookupElementGenericTableAvl(&tbl, &k);
        TEST_ASSERT(body != (void *)0, "odd keys survive the even deletions");
    }
    {
        avl_elem_t k; k.key = 4; k.payload = 0;
        TEST_ASSERT(RtlLookupElementGenericTableAvl(&tbl, &k) == (void *)0,
                    "a deleted key is gone");
        TEST_ASSERT(!RtlDeleteElementGenericTableAvl(&tbl, &k),
                    "deleting an absent key returns false");
    }

    /* Delete the rest; table empties cleanly (frees every node). */
    for (i = 1; i < AVL_N; i += 2) {
        avl_elem_t k; k.key = i; k.payload = 0;
        TEST_ASSERT(RtlDeleteElementGenericTableAvl(&tbl, &k), "delete remaining odd key");
    }
    TEST_ASSERT(RtlIsGenericTableEmptyAvl(&tbl), "table empty after deleting all");
    TEST_ASSERT(tbl.Root == (RTL_BALANCED_LINKS *)0, "root NULL when empty");
}

/* ---- S7: RTL_DYNAMIC_HASH_TABLE ----------------------------------------- */

typedef struct { RTL_DYNAMIC_HASH_TABLE_ENTRY link; uint64_t val; } htest_entry_t;

static void *h_alloc(RTL_DYNAMIC_HASH_TABLE *t, uint32_t size) { (void)t; return kmalloc(size); }
static void  h_free(RTL_DYNAMIC_HASH_TABLE *t, void *b) { (void)t; kfree(b); }

#define HASH_N 200u

static htest_entry_t g_h_entries[HASH_N];

static void test_ex_hashtable_resize_and_lookup(void)
{
    RTL_DYNAMIC_HASH_TABLE tbl;
    RTL_HASH_TABLE_CONTEXT ctx;
    uint32_t i, grown_buckets;
    RTL_DYNAMIC_HASH_TABLE_ENTRY *e;

    TEST_ASSERT_EQ(RtlInitializeDynamicHashTable(&tbl, h_alloc, h_free, (void *)0, 8), 0,
                   "hash table init succeeds");
    /* Mandatory allocator: NULL allocate/free must be rejected. */
    {
        RTL_DYNAMIC_HASH_TABLE bad;
        TEST_ASSERT(RtlInitializeDynamicHashTable(&bad, (RTL_HASH_ALLOCATE_ROUTINE)0,
                                                  h_free, (void *)0, 8) != 0,
                    "NULL allocate is rejected (no hidden allocation)");
    }

    /* Insert N entries under distinct signatures; the directory must grow. */
    for (i = 0; i < HASH_N; i++) {
        g_h_entries[i].val = i;
        TEST_ASSERT_EQ(RtlInsertEntryHashTable(&tbl, &g_h_entries[i].link, 1000u + i), 0,
                       "insert entry");
    }
    TEST_ASSERT_EQ(RtlNumberOfEntriesHashTable(&tbl), HASH_N, "all entries counted");
    TEST_ASSERT(tbl.BucketCount >= HASH_N,
                "directory grew at least to the entry count (resize works)");
    grown_buckets = tbl.BucketCount;

    /* Every signature is found and resolves to the right embedded entry. */
    for (i = 0; i < HASH_N; i++) {
        e = RtlLookupEntryHashTable(&tbl, 1000u + i, &ctx);
        TEST_ASSERT(e != (RTL_DYNAMIC_HASH_TABLE_ENTRY *)0, "lookup finds the signature");
        TEST_ASSERT_EQ(((htest_entry_t *)e)->val, i, "lookup returns the right entry");
    }
    /* A missing signature is not found. */
    TEST_ASSERT(RtlLookupEntryHashTable(&tbl, 999999u, &ctx) == (RTL_DYNAMIC_HASH_TABLE_ENTRY *)0,
                "absent signature is NULL");

    /* Collision chain: three entries share one signature -> Lookup + GetNext walk all. */
    {
        static htest_entry_t coll[3];
        uint32_t seen = 0;
        for (i = 0; i < 3; i++) { coll[i].val = 7000u + i;
            RtlInsertEntryHashTable(&tbl, &coll[i].link, 0xC0FFEEu); }
        e = RtlLookupEntryHashTable(&tbl, 0xC0FFEEu, &ctx);
        while (e) { seen++; e = RtlGetNextEntryHashTable(&tbl, &ctx); }
        TEST_ASSERT_EQ(seen, 3u, "Lookup+GetNext walk the full collision chain");
        /* Remove-the-just-returned-entry during a walk is the supported pattern:
         * the cursor caches the successor eagerly, so removing the current entry
         * leaves the walk valid (drains the whole chain safely). */
        seen = 0;
        e = RtlLookupEntryHashTable(&tbl, 0xC0FFEEu, &ctx);
        while (e) {
            RTL_DYNAMIC_HASH_TABLE_ENTRY *cur = e;
            seen++;
            e = RtlGetNextEntryHashTable(&tbl, &ctx);
            RtlRemoveEntryHashTable(&tbl, cur);   /* remove the one just returned */
        }
        TEST_ASSERT_EQ(seen, 3u, "remove-current-during-walk drains the whole chain");
        TEST_ASSERT(RtlLookupEntryHashTable(&tbl, 0xC0FFEEu, &ctx) == (RTL_DYNAMIC_HASH_TABLE_ENTRY *)0,
                    "collision signature gone after walk-remove");
    }

    /* Remove every entry. The table is GROW-ONLY: the directory does NOT shrink
     * (so a remove-current cursor walk can never be invalidated by a mid-walk
     * rehash); memory is reclaimed at RtlDeleteDynamicHashTable. */
    for (i = 0; i < HASH_N; i++)
        TEST_ASSERT(RtlRemoveEntryHashTable(&tbl, &g_h_entries[i].link), "remove entry");
    TEST_ASSERT_EQ(RtlNumberOfEntriesHashTable(&tbl), 0u, "table empty after removes");
    TEST_ASSERT_EQ(tbl.BucketCount, grown_buckets, "grow-only: directory does not shrink");
    /* Lookups after removal miss. */
    TEST_ASSERT(RtlLookupEntryHashTable(&tbl, 1000u, &ctx) == (RTL_DYNAMIC_HASH_TABLE_ENTRY *)0,
                "removed signature no longer found");
    /* Removing an absent entry is false. */
    TEST_ASSERT(!RtlRemoveEntryHashTable(&tbl, &g_h_entries[0].link),
                "removing an already-removed entry returns false");

    RtlDeleteDynamicHashTable(&tbl);
    TEST_ASSERT(tbl.Directory == (RTL_DYNAMIC_HASH_TABLE_ENTRY **)0,
                "delete releases the directory");
}

#define HASH_CHAIN_N 40u
static htest_entry_t g_h_chain[HASH_CHAIN_N];

/* Drain a long single-signature chain via the remove-current cursor pattern,
 * AFTER the table has grown (NumEntries > BucketCount). Grow-only removal means
 * no mid-walk rehash, so every entry is returned and removed exactly once. */
static void test_ex_hashtable_drain_chain(void)
{
    RTL_DYNAMIC_HASH_TABLE tbl;
    RTL_HASH_TABLE_CONTEXT ctx;
    RTL_DYNAMIC_HASH_TABLE_ENTRY *e;
    uint32_t i, drained = 0, grown;

    TEST_ASSERT_EQ(RtlInitializeDynamicHashTable(&tbl, h_alloc, h_free, (void *)0, 8), 0,
                   "init");
    /* All entries share one signature -> one long chain; the count also grows
     * the directory (HASH_CHAIN_N > 8). */
    for (i = 0; i < HASH_CHAIN_N; i++) {
        g_h_chain[i].val = i;
        RtlInsertEntryHashTable(&tbl, &g_h_chain[i].link, 0xABCDEFu);
    }
    grown = tbl.BucketCount;
    TEST_ASSERT(grown >= HASH_CHAIN_N, "directory grew past the entry count");

    e = RtlLookupEntryHashTable(&tbl, 0xABCDEFu, &ctx);
    while (e) {
        RTL_DYNAMIC_HASH_TABLE_ENTRY *cur = e;
        drained++;
        e = RtlGetNextEntryHashTable(&tbl, &ctx);
        TEST_ASSERT(RtlRemoveEntryHashTable(&tbl, cur), "remove the just-returned entry");
    }
    TEST_ASSERT_EQ(drained, HASH_CHAIN_N,
                   "every chain entry returned exactly once across the drain");
    TEST_ASSERT_EQ(RtlNumberOfEntriesHashTable(&tbl), 0u, "chain fully drained");
    TEST_ASSERT_EQ(tbl.BucketCount, grown, "grow-only: no shrink during the drain");
    RtlDeleteDynamicHashTable(&tbl);
}

/* Overflow-boundary guards -- exercised WITHOUT huge allocations: the guards
 * reject before any allocate/memcpy, so these never touch real memory. */
static void test_ex_s7_overflow_guards(void)
{
    /* RTL_BITMAP_WORDS is overflow-free near UINT32_MAX (naive (bits+31)/32
     * would wrap to 0 at UINT32_MAX). */
    TEST_ASSERT_EQ(RTL_BITMAP_WORDS(0xFFFFFFFFu), 0x8000000u,
                   "word count for UINT32_MAX bits does not wrap");
    TEST_ASSERT_EQ(RTL_BITMAP_WORDS(0xFFFFFFE0u), 0x7FFFFFFu,
                   "word count for UINT32_MAX-31 bits is exact");
    TEST_ASSERT_EQ(RTL_BITMAP_WORDS(0u), 0u, "word count for 0 bits is 0");
    TEST_ASSERT_EQ(RTL_BITMAP_WORDS(32u), 1u, "word count for 32 bits is 1");
    TEST_ASSERT_EQ(RTL_BITMAP_WORDS(33u), 2u, "word count for 33 bits is 2");

    /* AVL insert rejects a size that would wrap header + size, BEFORE allocating
     * (the allocate routine is never reached, so no heap overflow). */
    {
        RTL_AVL_TABLE tbl;
        bool isnew = true;
        void *body;
        avl_elem_t e; e.key = 1; e.payload = 1;
        RtlInitializeGenericTableAvl(&tbl, avl_cmp, avl_alloc, avl_free, (void *)0);
        body = RtlInsertElementGenericTableAvl(&tbl, &e, 0xFFFFFFFFu, &isnew);
        TEST_ASSERT(body == (void *)0 && !isnew,
                    "AVL insert rejects a size that would wrap the alloc arg");
        TEST_ASSERT(RtlIsGenericTableEmptyAvl(&tbl), "table untouched after rejected insert");
    }

    /* Hash init clamps an absurd bucket request to the cap (no 4 GiB memset). */
    {
        RTL_DYNAMIC_HASH_TABLE big;
        /* DEFAULT-sized request still succeeds; the cap only bounds the byte
         * size, it does not reject a normal init. */
        TEST_ASSERT_EQ(RtlInitializeDynamicHashTable(&big, h_alloc, h_free, (void *)0,
                                                     0u), 0,
                       "hash init with 0 initial buckets uses the default");
        TEST_ASSERT(big.BucketCount >= 8u && big.BucketCount <= (1u << 28),
                    "bucket count within [min, cap]");
        RtlDeleteDynamicHashTable(&big);
    }
}

/* ---- S12: Worker Items -------------------------------------------------- */

static volatile uint32_t g_wi_count;       /* target routine run count */
static volatile uint32_t g_wi_block_run;   /* blocker routine is running */
static volatile uint32_t g_wi_release;     /* tell the blocker to finish */
/* File-scope so a spin-budget timeout can never let sys_wq write a freed stack
 * frame: the storage outlives any test return. */
static EX_WORK_ITEM g_wi_item;
static EX_WORK_ITEM g_wi_blocker;
static EX_WORK_ITEM g_wi_target;

static void wi_target_fn(void *ctx) { (void)ctx; __atomic_fetch_add(&g_wi_count, 1, __ATOMIC_ACQ_REL); }

static void wi_block_fn(void *ctx)
{
    uint32_t spins = 0;
    (void)ctx;
    __atomic_store_n(&g_wi_block_run, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_wi_release, __ATOMIC_ACQUIRE)) {
        if (++spins > 2000000u) break;
        yield();
    }
}

static void test_ex_workitem_run_and_requeue(void)
{
    uint32_t spins = 0;
    EX_WORK_ITEM *wip = &g_wi_item;

    g_wi_count = 0;
    ExInitializeWorkItem(wip, wi_target_fn, (void *)0);
    TEST_ASSERT_EQ(ExpWorkItemState(wip), EX_WI_IDLE, "fresh item is IDLE");

    TEST_ASSERT_EQ(ExQueueWorkItem(wip), 0, "queue succeeds");
    /* Wait for the TERMINAL state, not just the count -- the routine bumps the
     * count BEFORE the trampoline stores DONE, so the stack item must reach DONE
     * before it goes out of scope / is reused (else the worker writes a freed
     * frame). */
    while (ExpWorkItemState(wip) != EX_WI_DONE) {
        if (++spins > 2000000u) break;
        yield();
    }
    TEST_ASSERT_EQ(g_wi_count, 1u, "worker ran the routine once");
    TEST_ASSERT_EQ(ExpWorkItemState(wip), EX_WI_DONE, "item is DONE after run");

    /* Re-queue a completed item (terminal state -> QUEUED). */
    TEST_ASSERT_EQ(ExQueueWorkItem(wip), 0, "re-queue a DONE item succeeds");
    spins = 0;
    while (ExpWorkItemState(wip) != EX_WI_DONE) {
        if (++spins > 2000000u) break;
        yield();
    }
    TEST_ASSERT_EQ(g_wi_count, 2u, "re-queued item ran again");

    /* Bad args. */
    TEST_ASSERT_EQ(ExQueueWorkItem((EX_WORK_ITEM *)0), -1, "queue NULL item fails");
    TEST_ASSERT(!ExCancelWorkItem((EX_WORK_ITEM *)0), "cancel NULL item is false");
}

static void test_ex_workitem_cancel_before_dispatch(void)
{
    uint32_t spins = 0;
    EX_WORK_ITEM *blocker = &g_wi_blocker, *target = &g_wi_target;

    g_wi_count = 0; g_wi_block_run = 0; g_wi_release = 0;

    /* Occupy the single sys_wq worker with a blocker so the next item stays
     * QUEUED (deterministic cancel-before-dispatch). */
    ExInitializeWorkItem(blocker, wi_block_fn, (void *)0);
    ExInitializeWorkItem(target, wi_target_fn, (void *)0);
    TEST_ASSERT_EQ(ExQueueWorkItem(blocker), 0, "queue blocker");
    while (!__atomic_load_n(&g_wi_block_run, __ATOMIC_ACQUIRE)) {
        if (++spins > 2000000u) break;
        yield();
    }
    TEST_ASSERT(__atomic_load_n(&g_wi_block_run, __ATOMIC_ACQUIRE), "blocker occupies the worker");

    /* The target queues behind the busy worker and must still be cancellable. */
    TEST_ASSERT_EQ(ExQueueWorkItem(target), 0, "queue target behind the blocker");
    TEST_ASSERT(ExCancelWorkItem(target), "cancel wins while still QUEUED");
    TEST_ASSERT_EQ(ExpWorkItemState(target), EX_WI_CANCELLED, "target is CANCELLED");
    /* A second cancel (already cancelled) loses. */
    TEST_ASSERT(!ExCancelWorkItem(target), "second cancel of a cancelled item fails");
    /* A CANCELLED item is NOT re-queueable yet -- its stale node has not drained,
     * so re-queueing (which would create a second node) must be rejected. */
    TEST_ASSERT_EQ(ExQueueWorkItem(target), -1, "cannot re-queue a not-yet-drained CANCELLED item");

    /* Release the blocker; the worker drains. The cancelled target must NOT run,
     * and its stale node must drain it from CANCELLED back to IDLE. */
    __atomic_store_n(&g_wi_release, 1, __ATOMIC_RELEASE);
    spins = 0;
    while (ExpWorkItemState(blocker) != EX_WI_DONE) {
        if (++spins > 2000000u) break;
        yield();
    }
    TEST_ASSERT_EQ(ExpWorkItemState(blocker), EX_WI_DONE, "blocker completed");
    /* The worker now drains the cancelled target's stale node: CANCELLED -> IDLE. */
    spins = 0;
    while (ExpWorkItemState(target) != EX_WI_IDLE) {
        if (++spins > 2000000u) break;
        yield();
    }
    TEST_ASSERT_EQ(ExpWorkItemState(target), EX_WI_IDLE,
                   "cancelled item drains to IDLE (free/re-queue-safe)");
    TEST_ASSERT_EQ(g_wi_count, 0u, "cancelled target never ran");
    /* After draining, the item is re-queueable again. Wait for the TERMINAL DONE
     * (not just the count) so the stack item is not reused while the worker is
     * still about to write its DONE state. */
    TEST_ASSERT_EQ(ExQueueWorkItem(target), 0, "drained item re-queues");
    spins = 0;
    while (ExpWorkItemState(target) != EX_WI_DONE) {
        if (++spins > 2000000u) break;
        yield();
    }
    TEST_ASSERT_EQ(g_wi_count, 1u, "re-queued drained item runs");
    TEST_ASSERT_EQ(ExpWorkItemState(target), EX_WI_DONE, "target DONE before test returns");
}

void test_register_ex(void)
{
    test_suite_register_cat("ex: SLIST init/empty edges",
                            test_ex_slist_init_empty, TEST_CAT_EX);
    test_suite_register_cat("ex: SLIST push/pop LIFO + depth",
                            test_ex_slist_push_pop_lifo, TEST_CAT_EX);
    test_suite_register_cat("ex: SLIST flush detaches chain",
                            test_ex_slist_flush_detaches_chain, TEST_CAT_EX);
    test_suite_register_cat("ex: SLIST reuse cycles keep depth honest",
                            test_ex_slist_reuse_after_drain, TEST_CAT_EX);
    test_suite_register_cat("ex: SLIST concurrent push/pop (no loss/dup)",
                            test_ex_slist_smp_stress, TEST_CAT_EX);
    test_suite_register_cat("ex: rundown acquire/release/nested",
                            test_ex_rundown_acquire_release, TEST_CAT_EX);
    test_suite_register_cat("ex: rundown completed rejects acquire",
                            test_ex_rundown_completed_rejects, TEST_CAT_EX);
    test_suite_register_cat("ex: rundown wait with no refs arms",
                            test_ex_rundown_wait_no_refs, TEST_CAT_EX);
    test_suite_register_cat("ex: rundown concurrent drain",
                            test_ex_rundown_concurrent_drain, TEST_CAT_EX);
    test_suite_register_cat("ex: callback register/notify/unregister",
                            test_ex_callback_register_notify, TEST_CAT_EX);
    test_suite_register_cat("ex: callback allow_multiple=false cap",
                            test_ex_callback_allow_multiple_false, TEST_CAT_EX);
    test_suite_register_cat("ex: callback multiple + enumerate",
                            test_ex_callback_multiple_and_enumerate, TEST_CAT_EX);
    test_suite_register_cat("ex: callback slot reuse re-arms rundown",
                            test_ex_callback_slot_reuse, TEST_CAT_EX);
    test_suite_register_cat("ex: callback stale cookie no-op",
                            test_ex_callback_stale_cookie_noop, TEST_CAT_EX);
    test_suite_register_cat("ex: callback built-in named resolvable",
                            test_ex_callback_builtin_named, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside free-then-alloc cache hit",
                            test_ex_lookaside_cache_hit, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside depth cap drains excess",
                            test_ex_lookaside_depth_cap, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside paged rejects DISPATCH_LEVEL",
                            test_ex_lookaside_paged_rejects_dispatch, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside EX custom backing + context",
                            test_ex_lookaside_ex_custom_backing, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside nonpaged hit/miss at DISPATCH",
                            test_ex_lookaside_nonpaged_dispatch, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside paged free rejects DISPATCH",
                            test_ex_lookaside_paged_free_rejects_dispatch, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside EX bad-args + flush keeps usable",
                            test_ex_lookaside_ex_badargs_and_flush, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside depth clamp below saturation",
                            test_ex_lookaside_depth_clamp, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside failed init leaves inert",
                            test_ex_lookaside_failed_init_inert, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside delete at DISPATCH recovers",
                            test_ex_lookaside_delete_at_dispatch_recovers, TEST_CAT_EX);
    test_suite_register_cat("ex: lookaside verifier detects UAF",
                            test_ex_lookaside_verifier_uaf, TEST_CAT_EX);
    test_suite_register_cat("ex: fastref pack/unpack round-trip",
                            test_ex_fastref_pack_unpack_roundtrip, TEST_CAT_EX);
    test_suite_register_cat("ex: fastref empty cache -> slow path",
                            test_ex_fastref_init_acquire_empty_slowpath, TEST_CAT_EX);
    test_suite_register_cat("ex: fastref saturation falls back, then drains",
                            test_ex_fastref_release_saturation_then_drain, TEST_CAT_EX);
    test_suite_register_cat("ex: fastref release object mismatch rejected",
                            test_ex_fastref_release_object_mismatch, TEST_CAT_EX);
    test_suite_register_cat("ex: fastref release NULL rejected",
                            test_ex_fastref_release_null_rejected, TEST_CAT_EX);
    test_suite_register_cat("ex: fastref compare-swap object identity",
                            test_ex_fastref_compare_swap, TEST_CAT_EX);
    test_suite_register_cat("ex: fastref concurrent acquire/release (conservation)",
                            test_ex_fastref_smp_stress, TEST_CAT_EX);
    test_suite_register_cat("ex: bitmap basic ops + padding safety",
                            test_ex_bitmap_basic_and_padding, TEST_CAT_EX);
    test_suite_register_cat("ex: bitmap run finding + find-and-set",
                            test_ex_bitmap_runs, TEST_CAT_EX);
    test_suite_register_cat("ex: bitmap partial-OOB no-op + large word-aware find",
                            test_ex_bitmap_oob_and_large, TEST_CAT_EX);
    test_suite_register_cat("ex: AVL 1000-key insert/lookup/delete/enumerate",
                            test_ex_avl_thousand_keys, TEST_CAT_EX);
    test_suite_register_cat("ex: dynamic hash resize + lookup + chains",
                            test_ex_hashtable_resize_and_lookup, TEST_CAT_EX);
    test_suite_register_cat("ex: dynamic hash drain long chain (grow-only cursor)",
                            test_ex_hashtable_drain_chain, TEST_CAT_EX);
    test_suite_register_cat("ex: S7 overflow-boundary guards",
                            test_ex_s7_overflow_guards, TEST_CAT_EX);
    test_suite_register_cat("ex: work item run + re-queue",
                            test_ex_workitem_run_and_requeue, TEST_CAT_EX);
    test_suite_register_cat("ex: work item cancel before dispatch",
                            test_ex_workitem_cancel_before_dispatch, TEST_CAT_EX);
}

#endif /* KERNEL_TESTS */
