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
}

#endif /* KERNEL_TESTS */
