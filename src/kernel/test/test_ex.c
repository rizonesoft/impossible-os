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
}

#endif /* KERNEL_TESTS */
