/* ============================================================================
 * test_nt_sync.c -- NT synchronization object SSDT handler tests
 *
 * Behavior tests for src/kernel/nt/nt_sync.c (NT synchronisation object
 * syscalls): SSDT
 * registration, wait constants, semaphore release overflow guard, WaitAll
 * all-or-none rollback, duplicate/aliased handle rejection, consuming
 * WaitAny polls, mutant recursion, and NtSignalAndWaitForSingleObject.
 * All dispatches go through ssdt_dispatch with timeout=0 polls only (no
 * blocking waits in the single-threaded test context).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/nt_sync.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/ob/ob_event.h"
#include "kernel/ob/ob_mutex.h"
#include "kernel/ob/handle_table.h"
#include "kernel/nt/filetime.h"
#include "kernel/time/wall_clock.h"

/* ---- NT sync objects tests ---- */

/* Build OBJECT_ATTRIBUTES + UNICODE_STRING for an NT namespace path
 * (ASCII buffer via uint16_t* cast, matching sync_oa_name's narrow read) */
static void nt_test_build_oa(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us,
                             const char *path)
{
    uint32_t len = 0;
    while (path[len]) len++;
    us->Buffer = (uint16_t *)(uintptr_t)path;
    us->Length = (uint16_t)len;
    us->MaximumLength = (uint16_t)(len + 1);
    InitializeObjectAttributes(oa, us, OBJ_CASE_INSENSITIVE,
                               INVALID_HANDLE_VALUE, (void *)0);
}

/* Unlink a named object from \BaseNamedObjects so repeated suite runs and
 * the leak checker see no residue (mirrors test_ob.c's cleanup helper). */
static void nt_test_cleanup_named(const char *leaf, const OBJECT_TYPE *type)
{
    extern int snprintf(char *buf, size_t size, const char *fmt, ...);
    char path[128];
    void *body = (void *)0;
    void *bno = (void *)0;

    snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", leaf);
    if (ObLookupObjectByName(path, type, 0, &body) != 0 || !body)
        return;
    ObMakeTemporaryObject(body);
    if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0,
                             &bno) == 0 && bno) {
        ObpRemoveFromDirectory(bno, body);
        ObDereferenceObject(bno);
    }
    ObDereferenceObject(body);
}

static void test_nt_sync_ssdt_registered(void)
{
    NTSTATUS s;

    /* NtCreateEvent (0x0070) with NULL out -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateEvent, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateEvent SSDT registered (NULL out)");

    /* NtCreateMutant (0x0076) with NULL out -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateMutant, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateMutant SSDT registered (NULL out)");

    /* NtCreateSemaphore (0x007A) with NULL out -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateSemaphore, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateSemaphore SSDT registered (NULL out)");

    /* NtWaitForMultipleObjects (0x0007) with NULL handles -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtWaitForMultipleObjects, 1, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtWaitForMultipleObjects SSDT registered (NULL handles)");

    /* NtSetEvent (0x0072) with invalid handle -- INVALID_HANDLE */
    s = ssdt_dispatch(SSDT_NtSetEvent, 999, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE,
                   "NtSetEvent SSDT registered (bad handle)");

    /* Keyed event (0x0084) -- pending keyed-event subsystem */
    s = ssdt_dispatch(SSDT_NtCreateKeyedEvent, 0, 0, 0, 0, 0, 0);
    TEST_PENDING(s == STATUS_NOT_IMPLEMENTED,
                 "NtCreateKeyedEvent (0x84): no keyed-event subsystem yet");
}

static void test_nt_sync_constants(void)
{
    TEST_ASSERT_EQ(NotificationEvent,     0, "NotificationEvent == 0");
    TEST_ASSERT_EQ(SynchronizationEvent,  1, "SynchronizationEvent == 1");
    TEST_ASSERT_EQ(WaitAll,               0, "WaitAll == 0");
    TEST_ASSERT_EQ(WaitAny,               1, "WaitAny == 1");
    TEST_ASSERT_EQ(STATUS_WAIT_0,         0, "STATUS_WAIT_0 == 0");
    TEST_ASSERT_EQ(STATUS_ABANDONED,   0x80, "STATUS_ABANDONED == 0x80");
}

/* NtReleaseSemaphore: the max-count guard must use 64-bit arithmetic and
 * must not write PreviousCount on the failure path. */
static void test_nt_sem_release_overflow(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    int32_t prev = -777;
    NTSTATUS s;

    s = (NTSTATUS)ssdt_dispatch(SSDT_NtCreateSemaphore,
                                (uint64_t)(uintptr_t)&h, 0, 0,
                                1, 0x7FFFFFFF, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "create semaphore(initial=1, max=INT32_MAX)");

    /* 1 + INT32_MAX wraps in int32 -- the guard must still reject */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtReleaseSemaphore,
                                (uint64_t)(uint32_t)h, 0x7FFFFFFF,
                                (uint64_t)(uintptr_t)&prev, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SEMAPHORE_LIMIT_EXCEEDED,
                   "overflowing release rejected");
    TEST_ASSERT_EQ(prev, -777, "PreviousCount untouched on failed release");

    s = (NTSTATUS)ssdt_dispatch(SSDT_NtReleaseSemaphore,
                                (uint64_t)(uint32_t)h, 1,
                                (uint64_t)(uintptr_t)&prev, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "valid release");
    TEST_ASSERT_EQ(prev, 1, "PreviousCount == 1 after valid release");

    /* Large release count must be O(waiters), not O(count): a semaphore
     * with a huge maximum released by a huge count completes promptly and
     * lands the exact count -- the batched sem_signal_n path, not a
     * 2.1-billion-iteration sem_signal loop. */
    {
        HANDLE big = INVALID_HANDLE_VALUE;
        SEMAPHORE_BASIC_INFORMATION si;
        ssdt_dispatch(SSDT_NtCreateSemaphore, (uint64_t)(uintptr_t)&big, 0, 0,
                      0, 0x7FFFFFFF, 0);  /* initial 0, max INT32_MAX */
        s = (NTSTATUS)ssdt_dispatch(SSDT_NtReleaseSemaphore,
                                    (uint64_t)(uint32_t)big, 0x40000000,
                                    (uint64_t)(uintptr_t)&prev, 0, 0, 0);
        TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                       "large release count succeeds (batched, not O(n))");
        TEST_ASSERT_EQ(prev, 0, "PreviousCount == 0 before large release");
        ssdt_dispatch(SSDT_NtQuerySemaphore, (uint64_t)(uint32_t)big, 0,
                      (uint64_t)(uintptr_t)&si, sizeof(si), 0, 0);
        TEST_ASSERT_EQ(si.CurrentCount, 0x40000000,
                       "count landed exactly after large batched release");
        ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)big, 0, 0, 0, 0, 0);
    }

    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)h, 0, 0, 0, 0, 0);
}

/* WaitAll all-or-none: a partially-satisfiable WaitAll must consume
 * NOTHING; a successful zero-timeout poll on an auto-reset event must
 * consume the signal. */
static void test_nt_waitall_rollback(void)
{
    HANDLE e1 = INVALID_HANDLE_VALUE, e2 = INVALID_HANDLE_VALUE;
    HANDLE arr[2];
    int64_t zero_timeout = 0;
    NTSTATUS s;

    /* two auto-reset events: e1 signalled, e2 not */
    ssdt_dispatch(SSDT_NtCreateEvent, (uint64_t)(uintptr_t)&e1, 0, 0,
                  SynchronizationEvent, 1, 0);
    ssdt_dispatch(SSDT_NtCreateEvent, (uint64_t)(uintptr_t)&e2, 0, 0,
                  SynchronizationEvent, 0, 0);
    arr[0] = e1;
    arr[1] = e2;

    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                2, (uint64_t)(uintptr_t)arr, WaitAll, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_TIMEOUT,
                   "partial WaitAll times out");

    /* e1's signal must have been preserved (rolled back) ... */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                (uint64_t)(uint32_t)e1, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "e1 signal preserved after failed WaitAll");

    /* ... and that successful poll consumed it (auto-reset) */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                (uint64_t)(uint32_t)e1, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_TIMEOUT,
                   "auto-reset signal consumed by successful poll");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)e1, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)e2, 0, 0, 0, 0, 0);
}

/* WaitAll rejects duplicate objects; WaitAny allows them and CONSUMES the
 * satisfying auto-reset signal (manual-reset signals persist). */
static void test_nt_waitall_duplicates(void)
{
    HANDLE e1 = INVALID_HANDLE_VALUE;
    HANDLE m1 = INVALID_HANDLE_VALUE;
    HANDLE arr[2];
    int64_t zero_timeout = 0;
    NTSTATUS s;

    ssdt_dispatch(SSDT_NtCreateEvent, (uint64_t)(uintptr_t)&e1, 0, 0,
                  SynchronizationEvent, 1, 0);
    arr[0] = e1;
    arr[1] = e1;

    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                2, (uint64_t)(uintptr_t)arr, WaitAll, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_INVALID_PARAMETER,
                   "WaitAll duplicate handles rejected");

    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                2, (uint64_t)(uintptr_t)arr, WaitAny, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_WAIT_0,
                   "WaitAny duplicate handles allowed, returns index 0");

    /* that WaitAny consumed the auto-reset signal */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                2, (uint64_t)(uintptr_t)arr, WaitAny, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_TIMEOUT,
                   "WaitAny consumed the auto-reset signal");

    /* manual-reset signals survive a successful WaitAny */
    ssdt_dispatch(SSDT_NtCreateEvent, (uint64_t)(uintptr_t)&m1, 0, 0,
                  NotificationEvent, 1, 0);
    arr[0] = m1;
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                1, (uint64_t)(uintptr_t)arr, WaitAny, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_WAIT_0,
                   "WaitAny on set manual-reset event succeeds");
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                (uint64_t)(uint32_t)m1, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "manual-reset signal persists after WaitAny");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)e1, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)m1, 0, 0, 0, 0, 0);
}

/* WaitAll duplicate rejection compares object BODIES: two distinct handles
 * opened to the same named event must also be rejected. */
static void test_nt_waitall_alias_duplicates(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE h1 = INVALID_HANDLE_VALUE, h2 = INVALID_HANDLE_VALUE;
    HANDLE arr[2];
    int64_t zero_timeout = 0;
    NTSTATUS s;

    /* create takes the leaf name; open takes the full NT path */
    nt_test_build_oa(&oa, &us, "T12S8AliasEvt");
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtCreateEvent,
                                (uint64_t)(uintptr_t)&h1, 0,
                                (uint64_t)(uintptr_t)&oa,
                                SynchronizationEvent, 1, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "create named event");

    nt_test_build_oa(&oa, &us, "\\BaseNamedObjects\\T12S8AliasEvt");
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtOpenEvent,
                                (uint64_t)(uintptr_t)&h2, 0,
                                (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "open second handle to named event");

    arr[0] = h1;
    arr[1] = h2;
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                2, (uint64_t)(uintptr_t)arr, WaitAll, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_INVALID_PARAMETER,
                   "WaitAll rejects two handles to the same object");

    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                2, (uint64_t)(uintptr_t)arr, WaitAny, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_WAIT_0,
                   "WaitAny allows aliased handles");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)h1, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)h2, 0, 0, 0, 0, 0);
    nt_test_cleanup_named("T12S8AliasEvt", ObpEventType);
}

/* WaitAll rollback must restore semaphore tokens and mutant state, not
 * just event signals. */
static void test_nt_waitall_rollback_sem_mutant(void)
{
    HANDLE sem = INVALID_HANDLE_VALUE, ev = INVALID_HANDLE_VALUE;
    HANDLE mo = INVALID_HANDLE_VALUE, mf = INVALID_HANDLE_VALUE;
    HANDLE arr[2];
    SEMAPHORE_BASIC_INFORMATION si;
    MUTANT_BASIC_INFORMATION mi;
    int32_t prev = -777;
    int64_t zero_timeout = 0;
    NTSTATUS s;

    /* the never-signalled second object that forces the rollback */
    ssdt_dispatch(SSDT_NtCreateEvent, (uint64_t)(uintptr_t)&ev, 0, 0,
                  SynchronizationEvent, 0, 0);

    /* semaphore token restored on rollback */
    ssdt_dispatch(SSDT_NtCreateSemaphore, (uint64_t)(uintptr_t)&sem, 0, 0,
                  1, 10, 0);
    arr[0] = sem;
    arr[1] = ev;
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                2, (uint64_t)(uintptr_t)arr, WaitAll, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_TIMEOUT,
                   "sem+event WaitAll times out");
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtQuerySemaphore,
                                (uint64_t)(uint32_t)sem, 0,
                                (uint64_t)(uintptr_t)&si, sizeof(si), 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "query semaphore");
    TEST_ASSERT_EQ(si.CurrentCount, 1,
                   "semaphore token restored after rolled-back WaitAll");

    /* ... and a successful WaitAny consumes the token */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                1, (uint64_t)(uintptr_t)arr, WaitAny, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_WAIT_0,
                   "WaitAny acquires the semaphore");
    ssdt_dispatch(SSDT_NtQuerySemaphore, (uint64_t)(uint32_t)sem, 0,
                  (uint64_t)(uintptr_t)&si, sizeof(si), 0, 0);
    TEST_ASSERT_EQ(si.CurrentCount, 0,
                   "WaitAny consumed the semaphore token");

    /* owned mutant: recursion depth unchanged by rolled-back WaitAll */
    ssdt_dispatch(SSDT_NtCreateMutant, (uint64_t)(uintptr_t)&mo, 0, 0,
                  1, 0, 0);
    arr[0] = mo;
    arr[1] = ev;
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                2, (uint64_t)(uintptr_t)arr, WaitAll, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_TIMEOUT,
                   "owned-mutant WaitAll times out");
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtReleaseMutant,
                                (uint64_t)(uint32_t)mo,
                                (uint64_t)(uintptr_t)&prev, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "owned mutant still held exactly once");
    TEST_ASSERT_EQ(prev, 0, "depth 1 preserved (PreviousCount == 0)");
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtReleaseMutant,
                                (uint64_t)(uint32_t)mo,
                                (uint64_t)(uintptr_t)&prev, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_MUTANT_NOT_OWNED,
                   "no phantom recursion left behind");

    /* free mutant: ownership rolled back to free */
    ssdt_dispatch(SSDT_NtCreateMutant, (uint64_t)(uintptr_t)&mf, 0, 0,
                  0, 0, 0);
    arr[0] = mf;
    arr[1] = ev;
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                2, (uint64_t)(uintptr_t)arr, WaitAll, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_TIMEOUT,
                   "free-mutant WaitAll times out");
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtQueryMutant,
                                (uint64_t)(uint32_t)mf, 0,
                                (uint64_t)(uintptr_t)&mi, sizeof(mi), 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "query mutant");
    TEST_ASSERT_EQ(mi.CurrentCount, 1,
                   "free mutant released again by rollback");
    TEST_ASSERT_EQ(mi.OwnedByCaller, 0,
                   "free mutant not owned after rollback");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)sem, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)ev, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)mo, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)mf, 0, 0, 0, 0, 0);
}

/* NT mutants are recursively acquirable by the owner; releases unwind
 * the recursion before the mutex itself is released. */
static void test_nt_mutant_recursion(void)
{
    HANDLE m = INVALID_HANDLE_VALUE;
    int32_t prev = -777;
    int64_t zero_timeout = 0;
    NTSTATUS s;

    /* InitialOwner=TRUE: created owned by this thread (depth 1) */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtCreateMutant,
                                (uint64_t)(uintptr_t)&m, 0, 0, 1, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "create mutant(InitialOwner=TRUE)");

    /* Recursive acquire by owner succeeds without blocking (depth 2) */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                (uint64_t)(uint32_t)m, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "owner re-acquires mutant recursively");

    /* NtQueryMutant reports the recursion depth: count == 1 - depth */
    {
        MUTANT_BASIC_INFORMATION mi;
        s = (NTSTATUS)ssdt_dispatch(SSDT_NtQueryMutant,
                                    (uint64_t)(uint32_t)m, 0,
                                    (uint64_t)(uintptr_t)&mi, sizeof(mi),
                                    0, 0);
        TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                       "query mutant at depth 2");
        TEST_ASSERT_EQ(mi.CurrentCount, -1, "CurrentCount == -1 at depth 2");
        TEST_ASSERT_EQ(mi.OwnedByCaller, 1, "OwnedByCaller at depth 2");
    }

    /* First release unwinds recursion: PreviousCount == -1 (depth 2) */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtReleaseMutant,
                                (uint64_t)(uint32_t)m,
                                (uint64_t)(uintptr_t)&prev, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "first release succeeds");
    TEST_ASSERT_EQ(prev, -1, "PreviousCount == -1 at depth 2");

    /* Second release frees the mutant: PreviousCount == 0 (depth 1) */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtReleaseMutant,
                                (uint64_t)(uint32_t)m,
                                (uint64_t)(uintptr_t)&prev, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "second release succeeds");
    TEST_ASSERT_EQ(prev, 0, "PreviousCount == 0 at depth 1");

    /* Third release: no longer owned */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtReleaseMutant,
                                (uint64_t)(uint32_t)m,
                                (uint64_t)(uintptr_t)&prev, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_MUTANT_NOT_OWNED,
                   "release of unowned mutant rejected");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)m, 0, 0, 0, 0, 0);
}

/* NtSignalAndWaitForSingleObject releases exactly one mutant level and
 * enforces ownership on the signal object. */
static void test_nt_signal_and_wait_mutant(void)
{
    HANDLE m = INVALID_HANDLE_VALUE, ev = INVALID_HANDLE_VALUE;
    HANDLE mu = INVALID_HANDLE_VALUE;
    int32_t prev = -777;
    int64_t zero_timeout = 0;
    NTSTATUS s;

    /* owned mutant at depth 2 + pre-signalled auto-reset event */
    ssdt_dispatch(SSDT_NtCreateMutant, (uint64_t)(uintptr_t)&m, 0, 0,
                  1, 0, 0);
    ssdt_dispatch(SSDT_NtWaitForSingleObject, (uint64_t)(uint32_t)m, 0,
                  (uint64_t)(uintptr_t)&zero_timeout, 0, 0, 0);
    ssdt_dispatch(SSDT_NtCreateEvent, (uint64_t)(uintptr_t)&ev, 0, 0,
                  SynchronizationEvent, 1, 0);

    /* signal (release one level) + wait (pre-signalled) */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtSignalAndWaitForSingleObject,
                                (uint64_t)(uint32_t)m,
                                (uint64_t)(uint32_t)ev, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "SignalAndWait on recursively owned mutant");

    /* exactly ONE level was released: depth 1 remains */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtReleaseMutant,
                                (uint64_t)(uint32_t)m,
                                (uint64_t)(uintptr_t)&prev, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "mutant still held once after SignalAndWait");
    TEST_ASSERT_EQ(prev, 0, "SignalAndWait released exactly one level");

    /* signalling an unowned mutant fails and must NOT consume the wait
     * object's signal */
    ssdt_dispatch(SSDT_NtCreateMutant, (uint64_t)(uintptr_t)&mu, 0, 0,
                  0, 0, 0);
    ssdt_dispatch(SSDT_NtSetEvent, (uint64_t)(uint32_t)ev, 0, 0, 0, 0, 0);
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtSignalAndWaitForSingleObject,
                                (uint64_t)(uint32_t)mu,
                                (uint64_t)(uint32_t)ev, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_MUTANT_NOT_OWNED,
                   "SignalAndWait on unowned mutant rejected");
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                (uint64_t)(uint32_t)ev, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "wait object signal preserved on failed SignalAndWait");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)m, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)ev, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)mu, 0, 0, 0, 0, 0);
}

/* SignalAndWait must enforce the semaphore MaximumCount like
 * NtReleaseSemaphore, and a failed signal must not consume the wait
 * object's signal. */
static void test_nt_signal_and_wait_sem_limit(void)
{
    HANDLE sem = INVALID_HANDLE_VALUE, ev = INVALID_HANDLE_VALUE;
    int64_t zero_timeout = 0;
    NTSTATUS s;

    /* semaphore already at MaximumCount (initial == max == 1) */
    ssdt_dispatch(SSDT_NtCreateSemaphore, (uint64_t)(uintptr_t)&sem, 0, 0,
                  1, 1, 0);
    ssdt_dispatch(SSDT_NtCreateEvent, (uint64_t)(uintptr_t)&ev, 0, 0,
                  SynchronizationEvent, 1, 0);

    s = (NTSTATUS)ssdt_dispatch(SSDT_NtSignalAndWaitForSingleObject,
                                (uint64_t)(uint32_t)sem,
                                (uint64_t)(uint32_t)ev, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SEMAPHORE_LIMIT_EXCEEDED,
                   "SignalAndWait rejects maxed semaphore");

    /* wait object signal untouched by the failed signal */
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                (uint64_t)(uint32_t)ev, 0,
                                (uint64_t)(uintptr_t)&zero_timeout, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "wait signal preserved on rejected semaphore signal");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)sem, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)ev, 0, 0, 0, 0, 0);
}

/* An expired absolute FILETIME deadline (positive Timeout) must behave
 * as a poll, never as an infinite wait; a near-future absolute deadline
 * must time out promptly (bounded), never hang. */
static void test_nt_wait_absolute_deadline_expired(void)
{
    HANDLE ev = INVALID_HANDLE_VALUE;
    int64_t past_deadline = 1;  /* 100ns after 1601 -- long expired */
    NTSTATUS s;

    ssdt_dispatch(SSDT_NtCreateEvent, (uint64_t)(uintptr_t)&ev, 0, 0,
                  SynchronizationEvent, 0, 0);

    s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                (uint64_t)(uint32_t)ev, 0,
                                (uint64_t)(uintptr_t)&past_deadline, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_TIMEOUT,
                   "expired absolute deadline polls instead of hanging");

    /* Near-future absolute deadline: times out in ~50 ms, never hangs.
     * Only meaningful with a real wall-clock source (read-only query). */
    if (wall_clock_time_sourced()) {
        int64_t future = (int64_t)KeQuerySystemTime()
                         + 50 * 10000;  /* now + 50 ms in 100ns units */
        s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                    (uint64_t)(uint32_t)ev, 0,
                                    (uint64_t)(uintptr_t)&future, 0, 0, 0);
        TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_TIMEOUT,
                       "future absolute deadline times out (bounded)");
    }

    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)ev, 0, 0, 0, 0, 0);
}

/* Mutant recursion ceiling: an owner re-acquire at the cap must fail with
 * STATUS_MUTANT_LIMIT_EXCEEDED instead of wrapping the counter (a wrap
 * would unlock the mutex while the owner still holds recursive acquires).
 * The counter is seeded directly on the named object's body -- looping
 * 2^31 acquisitions is not viable in a test. */
static void test_nt_mutant_recursion_ceiling(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE m = INVALID_HANDLE_VALUE;
    MUTEX_OBJECT *mo = (MUTEX_OBJECT *)0;
    void *body = (void *)0;
    int64_t zero_timeout = 0;
    NTSTATUS s;

    nt_test_build_oa(&oa, &us, "T12S8RecCap");
    s = (NTSTATUS)ssdt_dispatch(SSDT_NtCreateMutant,
                                (uint64_t)(uintptr_t)&m, 0,
                                (uint64_t)(uintptr_t)&oa, 1, 0, 0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "create owned named mutant");

    if (ObLookupObjectByName("\\BaseNamedObjects\\T12S8RecCap",
                             ObpMutexType, 0, &body) == 0 && body) {
        mo = (MUTEX_OBJECT *)body;
        mo->recursion = 0x7FFFFFFD;  /* one below the cap */

        /* one more re-acquire reaches the cap ... */
        s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                    (uint64_t)(uint32_t)m, 0,
                                    (uint64_t)(uintptr_t)&zero_timeout,
                                    0, 0, 0);
        TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                       "re-acquire below the ceiling succeeds");

        /* ... and the next one must be refused with the defined limit
         * status (poll path), not wrapped and not a bare timeout */
        s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                    (uint64_t)(uint32_t)m, 0,
                                    (uint64_t)(uintptr_t)&zero_timeout,
                                    0, 0, 0);
        TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_MUTANT_LIMIT_EXCEEDED,
                       "poll at the ceiling returns limit status (no wrap)");

        /* a blocking wait at the ceiling fails fast with the same status
         * (owner short-circuit runs before any block) */
        s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                    (uint64_t)(uint32_t)m, 0, 0, 0, 0, 0);
        TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_MUTANT_LIMIT_EXCEEDED,
                       "blocking wait at the ceiling returns limit status");

        /* WaitAll containing the capped mutant also surfaces the limit,
         * not a spin-to-timeout */
        {
            HANDLE arr2[1];
            int64_t zt = 0;
            arr2[0] = m;
            s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                        1, (uint64_t)(uintptr_t)arr2, WaitAll,
                                        0, (uint64_t)(uintptr_t)&zt, 0);
            TEST_ASSERT_EQ((uint32_t)s,
                           (uint32_t)STATUS_MUTANT_LIMIT_EXCEEDED,
                           "WaitAll at the ceiling surfaces limit status");

            /* WaitAny with an INFINITE timeout (NULL pointer) must surface
             * the limit immediately, not hang -- the capped mutant is the
             * only object and can never be acquired. */
            s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                        1, (uint64_t)(uintptr_t)arr2, WaitAny,
                                        0, 0, 0);
            TEST_ASSERT_EQ((uint32_t)s,
                           (uint32_t)STATUS_MUTANT_LIMIT_EXCEEDED,
                           "WaitAny(infinite) at ceiling surfaces limit, no hang");
        }

        /* WaitAll where the capped mutant is ordered AFTER an unsignalled
         * object (infinite timeout): the prepass must still detect the
         * limit (scan-all, not break-on-first-miss) and not hang. */
        {
            HANDLE ev2 = INVALID_HANDLE_VALUE;
            HANDLE arr3[2];
            ssdt_dispatch(SSDT_NtCreateEvent, (uint64_t)(uintptr_t)&ev2, 0, 0,
                          SynchronizationEvent, 0, 0);  /* unsignalled */
            arr3[0] = ev2;  /* not-ready, ordered first */
            arr3[1] = m;    /* capped mutant, ordered second */
            s = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForMultipleObjects,
                                        2, (uint64_t)(uintptr_t)arr3, WaitAll,
                                        0, 0, 0);
            TEST_ASSERT_EQ((uint32_t)s,
                           (uint32_t)STATUS_MUTANT_LIMIT_EXCEEDED,
                           "WaitAll(infinite) detects ceiling behind not-ready object");
            ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)ev2, 0, 0, 0, 0, 0);
        }

        /* restore sane state for cleanup: depth 1, no recursion */
        mo->recursion = 0;
        ObDereferenceObject(body);
    } else {
        TEST_ASSERT(0, "named mutant body lookup failed");
    }

    ssdt_dispatch(SSDT_NtReleaseMutant, (uint64_t)(uint32_t)m, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)(uint32_t)m, 0, 0, 0, 0, 0);
    nt_test_cleanup_named("T12S8RecCap", ObpMutexType);
}

void test_register_nt_sync(void)
{
    test_suite_register_cat("NT: sync SSDT registered", test_nt_sync_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: sync constants", test_nt_sync_constants, TEST_CAT_ABI);
    test_suite_register_cat("NT: semaphore release overflow guard", test_nt_sem_release_overflow, TEST_CAT_ABI);
    test_suite_register_cat("NT: WaitAll all-or-none rollback", test_nt_waitall_rollback, TEST_CAT_ABI);
    test_suite_register_cat("NT: WaitAll sem/mutant rollback", test_nt_waitall_rollback_sem_mutant, TEST_CAT_ABI);
    test_suite_register_cat("NT: WaitAll duplicate handles", test_nt_waitall_duplicates, TEST_CAT_ABI);
    test_suite_register_cat("NT: WaitAll aliased duplicates", test_nt_waitall_alias_duplicates, TEST_CAT_ABI);
    test_suite_register_cat("NT: mutant recursive acquire", test_nt_mutant_recursion, TEST_CAT_ABI);
    test_suite_register_cat("NT: SignalAndWait mutant release", test_nt_signal_and_wait_mutant, TEST_CAT_ABI);
    test_suite_register_cat("NT: SignalAndWait semaphore limit", test_nt_signal_and_wait_sem_limit, TEST_CAT_ABI);
    test_suite_register_cat("NT: expired absolute wait deadline", test_nt_wait_absolute_deadline_expired, TEST_CAT_ABI);
    test_suite_register_cat("NT: mutant recursion ceiling", test_nt_mutant_recursion_ceiling, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
