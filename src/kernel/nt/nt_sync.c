/* ============================================================================
 * nt_sync.c -- NT synchronization object SSDT handlers
 *
 * Event, mutex (mutant), semaphore SSDT wrappers routing through the
 * Object Manager sync types (event, mutex, semaphore).  Also provides
 * NtWaitForSingleObject (upgraded), NtWaitForMultipleObjects, and
 * NtSignalAndWaitForSingleObject.  Keyed events are registered as stubs;
 * the real implementation is owned by the futex/keyed-event work in
 * 03-memory-concurrency/TODO-08 (advanced sync).
 * ============================================================================ */

#include "kernel/nt/nt_sync.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/sched/task.h"
#include "kernel/sched/event.h"
#include "kernel/sched/mutex.h"
#include "kernel/sched/semaphore.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_event.h"
#include "kernel/ob/ob_timer.h"
#include "kernel/ob/ob_mutex.h"
#include "kernel/ob/ob_semaphore.h"
#include "kernel/ob/ob_process.h"
#include "kernel/ipc/alpc_port.h" /* ObpAlpcPortType + ALPC_PORT for waitable ports */
#include "kernel/klog.h"
#include "kernel/timer.h"
#include "kernel/nt/filetime.h"
#include "kernel/time/wall_clock.h"

/* ---- Helper: extract path from OBJECT_ATTRIBUTES ----------------------- */
static const char *sync_oa_name(OBJECT_ATTRIBUTES *oa)
{
    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer)
        return (const char *)0;
    return (const char *)oa->ObjectName->Buffer;
}

/* ---- Helper: look up sync object by handle and verify type -------------- */
static void *sync_lookup(HANDLE handle, const OBJECT_TYPE *expected)
{
    HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(
        &task_current()->handle_table, handle);
    OBJECT_HEADER *hdr;

    if (!entry || !entry->object)
        return (void *)0;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != expected)
        return (void *)0;

    return entry->object;
}

/* ---- Helper: mutant ownership + hold depth ------------------------------
 * NT mutants are recursively acquirable by their owner.  `recursion` counts
 * acquisitions beyond the first and is mutated only by the owning thread.
 * Hold depth = 0 when free, 1 + recursion when owned.  The NT mutant count
 * is 1 - depth (1 = signalled/free, 0 = owned once, negative = recursive).
 * ----------------------------------------------------------------------- */
static int mutant_owned_by_current(MUTEX_OBJECT *mo)
{
    struct task *cur = task_current();
    struct thread *thr = thread_current();

    return mutex_is_locked(&mo->mutex) &&
           mo->mutex.owner_task == cur->pid &&
           (!thr || mo->mutex.owner_thread == thr->id);
}

static int32_t mutant_count(MUTEX_OBJECT *mo)
{
    if (!mutex_is_locked(&mo->mutex))
        return 1;
    return -(int32_t)mo->recursion;  /* 1 - (1 + recursion) */
}

/* ======================================================================== */
/* EVENT handlers (SSDT 0x0070-0x0075)                                     */
/* ======================================================================== */

/* ---- NtCreateEvent (0x0070) ---------------------------------------------
 * a1 = HANDLE* out, a2 = ACCESS_MASK, a3 = OBJECT_ATTRIBUTES*,
 * a4 = EventType, a5 = InitialState.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateEvent_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    event_type_t type = (a4 == SynchronizationEvent)
                        ? EVENT_AUTO_RESET : EVENT_MANUAL_RESET;
    int initial = (int)a5;
    const char *name;
    HANDLE h;

    (void)a2; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    name = sync_oa_name(oa);
    h = NtCreateEvent(&task_current()->handle_table, name, type, initial);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;

    *out = h;
    return STATUS_SUCCESS;
}

/* ---- NtOpenEvent (0x0071) -----------------------------------------------
 * a1 = HANDLE* out, a2 = ACCESS_MASK, a3 = OBJECT_ATTRIBUTES*.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtOpenEvent_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *name;
    void *body;

    (void)a2; (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    name = sync_oa_name(oa);
    if (!name)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (ObLookupObjectByName(name, ObpEventType, 0, &body) != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    *out = ObpAllocateHandle(&task_current()->handle_table, body, 0, 0);
    ObDereferenceObject(body);
    if (*out == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;
    return STATUS_SUCCESS;
}

/* ---- NtSetEvent (0x0072) ------------------------------------------------
 * a1 = HANDLE, a2 = int32_t* PreviousState (out, may be NULL).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtSetEvent_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    int32_t *prev = (int32_t *)a2;
    EVENT_OBJECT *eo;

    (void)a3; (void)a4; (void)a5; (void)a6;

    eo = (EVENT_OBJECT *)sync_lookup(handle, ObpEventType);
    if (!eo)
        return STATUS_INVALID_HANDLE;

    if (prev)
        *prev = event_is_set(&eo->event);

    event_set(&eo->event);
    return STATUS_SUCCESS;
}

/* ---- NtResetEvent (0x0073) ---------------------------------------------- */
static NTSTATUS NtResetEvent_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    int32_t *prev = (int32_t *)a2;
    EVENT_OBJECT *eo;

    (void)a3; (void)a4; (void)a5; (void)a6;

    eo = (EVENT_OBJECT *)sync_lookup(handle, ObpEventType);
    if (!eo)
        return STATUS_INVALID_HANDLE;

    if (prev)
        *prev = event_is_set(&eo->event);

    event_reset(&eo->event);
    return STATUS_SUCCESS;
}

/* ---- NtPulseEvent (0x0074) ---------------------------------------------- */
static NTSTATUS NtPulseEvent_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    int32_t *prev = (int32_t *)a2;
    EVENT_OBJECT *eo;

    (void)a3; (void)a4; (void)a5; (void)a6;

    eo = (EVENT_OBJECT *)sync_lookup(handle, ObpEventType);
    if (!eo)
        return STATUS_INVALID_HANDLE;

    if (prev)
        *prev = event_is_set(&eo->event);

    /* Pulse: set then immediately reset -- wakes waiting threads */
    event_set(&eo->event);
    event_reset(&eo->event);
    return STATUS_SUCCESS;
}

/* ---- NtQueryEvent (0x0075) ---------------------------------------------- */
static NTSTATUS NtQueryEvent_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    EVENT_BASIC_INFORMATION *info = (EVENT_BASIC_INFORMATION *)a3;
    uint32_t length = (uint32_t)a4;
    EVENT_OBJECT *eo;

    (void)a2; (void)a5; (void)a6;

    if (!info || length < sizeof(EVENT_BASIC_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    eo = (EVENT_OBJECT *)sync_lookup(handle, ObpEventType);
    if (!eo)
        return STATUS_INVALID_HANDLE;

    info->EventType = (eo->event.type == EVENT_AUTO_RESET)
                      ? SynchronizationEvent : NotificationEvent;
    info->EventState = (uint32_t)event_is_set(&eo->event);
    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* MUTANT handlers (SSDT 0x0076-0x0079)                                    */
/* ======================================================================== */

/* ---- NtCreateMutant (0x0076) --------------------------------------------
 * a1 = HANDLE* out, a2 = ACCESS_MASK, a3 = OBJECT_ATTRIBUTES*,
 * a4 = InitialOwner (bool).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateMutant_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    int initial_owner = (int)a4;
    const char *name;
    HANDLE h;

    (void)a2; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    name = sync_oa_name(oa);
    h = NtCreateMutex(&task_current()->handle_table, name, initial_owner);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;

    *out = h;
    return STATUS_SUCCESS;
}

/* ---- NtOpenMutant (0x0077) ---------------------------------------------- */
static NTSTATUS NtOpenMutant_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *name;
    void *body;

    (void)a2; (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    name = sync_oa_name(oa);
    if (!name)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (ObLookupObjectByName(name, ObpMutexType, 0, &body) != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    *out = ObpAllocateHandle(&task_current()->handle_table, body, 0, 0);
    ObDereferenceObject(body);
    if (*out == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;
    return STATUS_SUCCESS;
}

/* ---- NtReleaseMutant (0x0078) -------------------------------------------
 * a1 = HANDLE, a2 = int32_t* PreviousCount (out, may be NULL).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtReleaseMutant_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    int32_t *prev = (int32_t *)a2;
    MUTEX_OBJECT *mo;

    (void)a3; (void)a4; (void)a5; (void)a6;

    mo = (MUTEX_OBJECT *)sync_lookup(handle, ObpMutexType);
    if (!mo)
        return STATUS_INVALID_HANDLE;

    if (!mutant_owned_by_current(mo))
        return STATUS_MUTANT_NOT_OWNED;

    if (prev)
        *prev = mutant_count(mo);

    /* Recursive acquisitions unwind before the mutex itself is released */
    if (mo->recursion > 0) {
        mo->recursion--;
        return STATUS_SUCCESS;
    }

    mutex_unlock(&mo->mutex);
    return STATUS_SUCCESS;
}

/* ---- NtQueryMutant (0x0079) --------------------------------------------- */
static NTSTATUS NtQueryMutant_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    MUTANT_BASIC_INFORMATION *info = (MUTANT_BASIC_INFORMATION *)a3;
    uint32_t length = (uint32_t)a4;
    MUTEX_OBJECT *mo;
    struct task *cur;

    (void)a2; (void)a5; (void)a6;

    if (!info || length < sizeof(MUTANT_BASIC_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    mo = (MUTEX_OBJECT *)sync_lookup(handle, ObpMutexType);
    if (!mo)
        return STATUS_INVALID_HANDLE;

    cur = task_current();
    info->CurrentCount = mutant_count(mo);
    info->OwnedByCaller = (mutex_is_locked(&mo->mutex) &&
                           mo->mutex.owner_task == cur->pid) ? 1 : 0;
    info->AbandonedState = mo->abandoned;
    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* SEMAPHORE handlers (SSDT 0x007A-0x007D)                                 */
/* ======================================================================== */

/* ---- NtCreateSemaphore (0x007A) -----------------------------------------
 * a1 = HANDLE* out, a2 = ACCESS_MASK, a3 = OBJECT_ATTRIBUTES*,
 * a4 = InitialCount, a5 = MaximumCount.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateSemaphore_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    int32_t initial = (int32_t)a4;
    int32_t maximum = (int32_t)a5;
    const char *name;
    HANDLE h;

    (void)a2; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;
    if (initial < 0 || maximum <= 0 || initial > maximum)
        return STATUS_INVALID_PARAMETER;

    name = sync_oa_name(oa);
    h = NtCreateSemaphore(&task_current()->handle_table, name, initial, maximum);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;

    *out = h;
    return STATUS_SUCCESS;
}

/* ---- NtOpenSemaphore (0x007B) ------------------------------------------- */
static NTSTATUS NtOpenSemaphore_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *name;
    void *body;

    (void)a2; (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    name = sync_oa_name(oa);
    if (!name)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (ObLookupObjectByName(name, ObpSemaphoreType, 0, &body) != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    *out = ObpAllocateHandle(&task_current()->handle_table, body, 0, 0);
    ObDereferenceObject(body);
    if (*out == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;
    return STATUS_SUCCESS;
}

/* ---- Helper: checked semaphore release -----------------------------------
 * Shared by NtReleaseSemaphore and NtSignalAndWait so every release path
 * enforces MaximumCount.  Validates in 64-bit arithmetic (the int32 sum
 * `value + release_count` wraps near INT32_MAX and would bypass the cap);
 * no output is written until validation passes.
 * ----------------------------------------------------------------------- */
static NTSTATUS sem_release_checked(SEMAPHORE_OBJECT *so,
                                    int32_t release_count, int32_t *prev)
{
    int32_t cur = sem_value(&so->semaphore);
    int32_t i;

    if ((int64_t)cur + (int64_t)release_count > (int64_t)so->max_count)
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    if (prev)
        *prev = cur;

    for (i = 0; i < release_count; i++)
        sem_signal(&so->semaphore);

    return STATUS_SUCCESS;
}

/* ---- NtReleaseSemaphore (0x007C) ----------------------------------------
 * a1 = HANDLE, a2 = ReleaseCount, a3 = int32_t* PreviousCount (out).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtReleaseSemaphore_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    int32_t release_count = (int32_t)a2;
    int32_t *prev = (int32_t *)a3;
    SEMAPHORE_OBJECT *so;

    (void)a4; (void)a5; (void)a6;

    if (release_count <= 0)
        return STATUS_INVALID_PARAMETER;

    so = (SEMAPHORE_OBJECT *)sync_lookup(handle, ObpSemaphoreType);
    if (!so)
        return STATUS_INVALID_HANDLE;

    return sem_release_checked(so, release_count, prev);
}

/* ---- NtQuerySemaphore (0x007D) ------------------------------------------ */
static NTSTATUS NtQuerySemaphore_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    SEMAPHORE_BASIC_INFORMATION *info = (SEMAPHORE_BASIC_INFORMATION *)a3;
    uint32_t length = (uint32_t)a4;
    SEMAPHORE_OBJECT *so;

    (void)a2; (void)a5; (void)a6;

    if (!info || length < sizeof(SEMAPHORE_BASIC_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    so = (SEMAPHORE_OBJECT *)sync_lookup(handle, ObpSemaphoreType);
    if (!so)
        return STATUS_INVALID_HANDLE;

    info->CurrentCount = sem_value(&so->semaphore);
    info->MaximumCount = so->max_count;
    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* WAIT handlers (SSDT 0x0006-0x0008)                                      */
/* ======================================================================== */

/* ---- Wait plumbing -------------------------------------------------------
 * All waits resolve the handle ONCE, pin the object body with a reference
 * (so a concurrent NtClose cannot free it mid-wait), then operate on the
 * pinned body.  Multi-object WaitAll is all-or-none: objects are acquired
 * with non-blocking try-acquires and rolled back in reverse on any partial
 * failure, so a failed or timed-out WaitAll consumes nothing.
 * ----------------------------------------------------------------------- */

/* Event backing a body for the event-carried types (event/timer/ALPC) */
static event_t *wait_body_event(void *body, const OBJECT_TYPE *type)
{
    if (type == ObpEventType)
        return &((EVENT_OBJECT *)body)->event;
    if (type == ObpTimerType)
        return &((TIMER_OBJECT *)body)->event;
    return &((ALPC_PORT *)body)->SignalledEvent;
}

/* Is this body a type NtWaitXxx can wait on?  ALPC ports qualify only when
 * created with ALPC_PORTFLG_WAITABLE_PORT; non-waitable ports report
 * OBJECT_TYPE_MISMATCH so the caller learns the port is not
 * wait-compatible. */
static int wait_body_waitable(void *body, const OBJECT_TYPE *type)
{
    if (type == ObpAlpcPortType)
        return ((ALPC_PORT *)body)->IsWaitable;
    return type == ObpEventType || type == ObpMutexType ||
           type == ObpSemaphoreType || type == ObpProcessType ||
           type == ObpTimerType;
}

/* Non-blocking consuming acquire.  Returns 1 on success; *abandoned is set
 * (not cleared) when a mutant was abandoned by a dead owner.  Abandonment
 * is only CLEARED by wait_commit_abandoned() after the overall wait
 * succeeds, so a rolled-back WaitAll preserves the abandoned flag. */
static int wait_try_acquire(void *body, const OBJECT_TYPE *type, int *abandoned)
{
    if (type == ObpEventType || type == ObpTimerType ||
        type == ObpAlpcPortType)
        return event_try_consume(wait_body_event(body, type));

    if (type == ObpMutexType) {
        MUTEX_OBJECT *mo = (MUTEX_OBJECT *)body;
        if (mutant_owned_by_current(mo)) {
            mo->recursion++;
            return 1;
        }
        if (!mutex_trylock(&mo->mutex))
            return 0;
        if (mo->abandoned && abandoned)
            *abandoned = 1;
        return 1;
    }

    if (type == ObpSemaphoreType)
        return sem_trywait(&((SEMAPHORE_OBJECT *)body)->semaphore);

    if (type == ObpProcessType) {
        struct task *t = ((PROCESS_OBJECT *)body)->task;
        return t && t->state == TASK_DEAD;  /* non-consuming */
    }

    return 0;
}

/* Undo one wait_try_acquire() so a partially-satisfied WaitAll consumes
 * nothing.  Auto-reset events get their signal restored (event_set on a
 * manual-reset event that was only peeked is skipped -- it never cleared).
 * The spurious wake a restore can hand a third-party waiter is benign:
 * every waiter re-checks its condition. */
static void wait_rollback(void *body, const OBJECT_TYPE *type)
{
    if (type == ObpEventType || type == ObpTimerType ||
        type == ObpAlpcPortType) {
        event_t *ev = wait_body_event(body, type);
        if (ev->type == EVENT_AUTO_RESET)
            event_set(ev);
        return;
    }

    if (type == ObpMutexType) {
        MUTEX_OBJECT *mo = (MUTEX_OBJECT *)body;
        if (mo->recursion > 0)
            mo->recursion--;
        else
            mutex_unlock(&mo->mutex);
        return;
    }

    if (type == ObpSemaphoreType)
        sem_signal(&((SEMAPHORE_OBJECT *)body)->semaphore);

    /* process: try-acquire is non-consuming, nothing to undo */
}

/* NT clears mutant abandonment once a wait successfully acquires it */
static void wait_commit_abandoned(void *body, const OBJECT_TYPE *type)
{
    if (type == ObpMutexType)
        ((MUTEX_OBJECT *)body)->abandoned = 0;
}

/* Drop the Phase-A pins on the first n bodies */
static void wait_unpin(void **bodies, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        ObDereferenceObject(bodies[i]);
}

/* ---- Blocking single-object wait on a pinned body ----------------------- */
static NTSTATUS wait_on_body(void *body, const OBJECT_TYPE *type,
                             uint32_t timeout_ms)
{
    int abandoned = 0;

    /* Poll: exactly one consuming try-acquire, never blocks */
    if (timeout_ms == 0) {
        if (!wait_try_acquire(body, type, &abandoned))
            return STATUS_TIMEOUT;
        wait_commit_abandoned(body, type);
        return abandoned ? STATUS_ABANDONED : STATUS_SUCCESS;
    }

    if (type == ObpEventType || type == ObpTimerType ||
        type == ObpAlpcPortType) {
        event_t *ev = wait_body_event(body, type);
        if (timeout_ms == 0xFFFFFFFF) {
            event_wait(ev);
            return STATUS_SUCCESS;
        }
        /* event_wait_timeout returns 1 on signalled, 0 on timeout */
        return event_wait_timeout(ev, timeout_ms)
               ? STATUS_SUCCESS : STATUS_TIMEOUT;
    }

    if (type == ObpMutexType) {
        MUTEX_OBJECT *mo = (MUTEX_OBJECT *)body;
        if (mutant_owned_by_current(mo)) {
            mo->recursion++;
            return STATUS_SUCCESS;
        }
        if (timeout_ms == 0xFFFFFFFF) {
            mutex_lock(&mo->mutex);
        } else {
            uint64_t deadline = uptime_ns()
                                + (uint64_t)timeout_ms * 1000000ULL;
            while (!mutex_trylock(&mo->mutex)) {
                if (uptime_ns() >= deadline)
                    return STATUS_TIMEOUT;
                yield();
            }
        }
        if (mo->abandoned) {
            mo->abandoned = 0;
            return STATUS_ABANDONED;
        }
        return STATUS_SUCCESS;
    }

    if (type == ObpSemaphoreType) {
        semaphore_t *s = &((SEMAPHORE_OBJECT *)body)->semaphore;
        if (timeout_ms == 0xFFFFFFFF) {
            sem_wait(s);
            return STATUS_SUCCESS;
        }
        {
            uint64_t deadline = uptime_ns()
                                + (uint64_t)timeout_ms * 1000000ULL;
            while (!sem_trywait(s)) {
                if (uptime_ns() >= deadline)
                    return STATUS_TIMEOUT;
                yield();
            }
        }
        return STATUS_SUCCESS;
    }

    if (type == ObpProcessType) {
        /* Wait for process termination.  task_waitpid() blocks until the
         * child dies and reaps it; it returns immediately when the task
         * is already TASK_DEAD. */
        struct task *t = ((PROCESS_OBJECT *)body)->task;
        if (!t)
            return STATUS_INVALID_HANDLE;
        if (timeout_ms != 0xFFFFFFFF && t->state != TASK_DEAD) {
            uint64_t deadline = uptime_ns()
                                + (uint64_t)timeout_ms * 1000000ULL;
            while (t->state != TASK_DEAD) {
                if (uptime_ns() >= deadline)
                    return STATUS_TIMEOUT;
                yield();
            }
        }
        task_waitpid(t->pid);
        return STATUS_SUCCESS;
    }

    return STATUS_OBJECT_TYPE_MISMATCH;
}

/* ---- Helper: wait on a single sync object by handle --------------------- */
static NTSTATUS wait_on_handle(HANDLE handle, uint32_t timeout_ms)
{
    HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(
        &task_current()->handle_table, handle);
    OBJECT_HEADER *hdr;
    void *body;
    NTSTATUS status;

    if (!entry || !entry->object)
        return STATUS_INVALID_HANDLE;

    body = entry->object;
    hdr = OB_HEADER_FROM_BODY(body);

    if (!wait_body_waitable(body, hdr->type))
        return STATUS_OBJECT_TYPE_MISMATCH;
    if (hdr->type == ObpProcessType && !((PROCESS_OBJECT *)body)->task)
        return STATUS_INVALID_HANDLE;

    /* Pin the body so a concurrent NtClose cannot free it mid-wait
     * (ObReferenceObjectSafe returns 0 on success, -1 if the object is
     * already dying) */
    if (ObReferenceObjectSafe(body) != 0)
        return STATUS_INVALID_HANDLE;

    status = wait_on_body(body, hdr->type, timeout_ms);

    ObDereferenceObject(body);
    return status;
}

/* ---- Helper: convert NT timeout (100-ns) to ms --------------------------
 * NULL = infinite (0xFFFFFFFF sentinel), 0 = poll, negative = relative
 * interval, positive = absolute FILETIME deadline.  Conversion runs
 * through ke_delay_interval_to_ms (INT64_MIN-safe magnitude; an expired
 * absolute deadline resolves to a poll).  Without a wall-clock source an
 * absolute deadline is treated as expired rather than hanging forever,
 * and a huge finite interval clamps BELOW the infinite sentinel. */
static uint32_t nt_timeout_to_ms(uint64_t timeout_ptr)
{
    int64_t *tp = (int64_t *)timeout_ptr;
    FILETIME now = 0;
    uint32_t ms;

    if (!tp)
        return 0xFFFFFFFF;  /* infinite */
    if (*tp == 0)
        return 0;           /* poll */
    if (*tp > 0) {
        /* Sourced, not merely initialized: an unsourced clock returns
         * placeholder-plus-monotonic time and a future deadline would
         * clamp to a ~49-day wait instead of polling. */
        if (!wall_clock_time_sourced())
            return 0;
        now = KeQuerySystemTime();
        if (now == FILETIME_NOW_PLACEHOLDER)
            return 0;
    }
    ms = ke_delay_interval_to_ms(*tp, now);
    if (ms == 0xFFFFFFFF)
        ms = 0xFFFFFFFE;
    return ms;
}

/* ---- NtWaitForSingleObject (0x0006) -- upgraded -------------------------
 * a1 = HANDLE, a2 = Alertable (bool), a3 = int64_t* Timeout.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtWaitForSingleObject_handler(uint64_t a1, uint64_t a2,
                                              uint64_t a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    uint32_t timeout_ms;

    (void)a2; (void)a4; (void)a5; (void)a6;

    timeout_ms = nt_timeout_to_ms(a3);
    return wait_on_handle(handle, timeout_ms);
}

/* ---- NtWaitForMultipleObjects (0x0007) ----------------------------------
 * a1 = Count, a2 = HANDLE* array, a3 = WaitType (WaitAll/WaitAny),
 * a4 = Alertable (bool), a5 = int64_t* Timeout.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtWaitForMultipleObjects_handler(uint64_t a1, uint64_t a2,
                                                 uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    uint32_t count = (uint32_t)a1;
    HANDLE *handles = (HANDLE *)a2;
    uint32_t wait_type = (uint32_t)a3;
    void *bodies[MAXIMUM_WAIT_OBJECTS];
    const OBJECT_TYPE *types[MAXIMUM_WAIT_OBJECTS];
    uint32_t timeout_ms;
    uint64_t deadline = 0;
    uint32_t i;

    (void)a4; (void)a6;

    if (!handles || count == 0 || count > MAXIMUM_WAIT_OBJECTS)
        return STATUS_INVALID_PARAMETER;
    if (wait_type != WaitAll && wait_type != WaitAny)
        return STATUS_INVALID_PARAMETER;

    timeout_ms = nt_timeout_to_ms(a5);

    /* Phase A: resolve every handle once, verify it is waitable, and pin
     * the body.  WaitAll additionally rejects duplicate objects (two
     * handles to the same object cannot both be acquired all-or-none). */
    for (i = 0; i < count; i++) {
        HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(
            &task_current()->handle_table, handles[i]);
        OBJECT_HEADER *hdr;
        NTSTATUS fail = 0;

        if (!entry || !entry->object) {
            fail = STATUS_INVALID_HANDLE;
        } else {
            hdr = OB_HEADER_FROM_BODY(entry->object);
            if (!wait_body_waitable(entry->object, hdr->type)) {
                fail = STATUS_OBJECT_TYPE_MISMATCH;
            } else if (hdr->type == ObpProcessType &&
                       !((PROCESS_OBJECT *)entry->object)->task) {
                fail = STATUS_INVALID_HANDLE;
            } else if (wait_type == WaitAll) {
                uint32_t j;
                for (j = 0; j < i; j++) {
                    if (bodies[j] == entry->object) {
                        fail = STATUS_INVALID_PARAMETER;
                        break;
                    }
                }
            }
        }
        if (!fail && ObReferenceObjectSafe(entry->object) != 0)
            fail = STATUS_INVALID_HANDLE;
        if (fail) {
            wait_unpin(bodies, i);
            return fail;
        }
        bodies[i] = entry->object;
        types[i] = OB_HEADER_FROM_BODY(entry->object)->type;
    }

    if (timeout_ms != 0 && timeout_ms != 0xFFFFFFFF)
        deadline = uptime_ns() + (uint64_t)timeout_ms * 1000000ULL;

    /* Phase B: poll-acquire loop on the pinned bodies */
    for (;;) {
        if (wait_type == WaitAny) {
            for (i = 0; i < count; i++) {
                int abandoned = 0;
                if (wait_try_acquire(bodies[i], types[i], &abandoned)) {
                    wait_commit_abandoned(bodies[i], types[i]);
                    wait_unpin(bodies, count);
                    return abandoned
                           ? (NTSTATUS)(STATUS_ABANDONED + i)
                           : (NTSTATUS)(STATUS_WAIT_0 + i);
                }
            }
        } else {
            /* WaitAll: acquire everything or roll back everything */
            int abandoned = 0;
            int32_t first_abandoned = -1;
            uint32_t got = 0;

            for (i = 0; i < count; i++) {
                int this_abandoned = 0;
                if (!wait_try_acquire(bodies[i], types[i], &this_abandoned))
                    break;
                if (this_abandoned && first_abandoned < 0) {
                    abandoned = 1;
                    first_abandoned = (int32_t)i;
                }
                got++;
            }
            if (got == count) {
                for (i = 0; i < count; i++)
                    wait_commit_abandoned(bodies[i], types[i]);
                wait_unpin(bodies, count);
                return abandoned
                       ? (NTSTATUS)(STATUS_ABANDONED + (uint32_t)first_abandoned)
                       : STATUS_SUCCESS;
            }
            while (got > 0) {
                got--;
                wait_rollback(bodies[got], types[got]);
            }
        }

        if (timeout_ms == 0 ||
            (timeout_ms != 0xFFFFFFFF && uptime_ns() >= deadline)) {
            wait_unpin(bodies, count);
            return STATUS_TIMEOUT;
        }
        yield();
    }
}

/* ---- NtSignalAndWaitForSingleObject (0x0008) ----------------------------
 * a1 = HANDLE ObjectToSignal, a2 = HANDLE WaitObject,
 * a3 = Alertable (bool), a4 = int64_t* Timeout.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtSignalAndWait_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE signal_h = (HANDLE)(int32_t)a1;
    HANDLE wait_h = (HANDLE)(int32_t)a2;
    uint32_t timeout_ms;
    HANDLE_TABLE_ENTRY *sig_entry, *wait_entry;
    OBJECT_HEADER *sig_hdr;
    const OBJECT_TYPE *wait_type;
    void *wait_body;
    NTSTATUS status;

    (void)a3; (void)a5; (void)a6;

    timeout_ms = nt_timeout_to_ms(a4);

    /* Resolve, validate, and pin the WAIT object BEFORE mutating the
     * signal object, so a failed call has no visible side effects. */
    wait_entry = ObpLookupHandle(&task_current()->handle_table, wait_h);
    if (!wait_entry || !wait_entry->object)
        return STATUS_INVALID_HANDLE;
    wait_body = wait_entry->object;
    wait_type = OB_HEADER_FROM_BODY(wait_body)->type;
    if (!wait_body_waitable(wait_body, wait_type))
        return STATUS_OBJECT_TYPE_MISMATCH;
    if (wait_type == ObpProcessType && !((PROCESS_OBJECT *)wait_body)->task)
        return STATUS_INVALID_HANDLE;
    if (ObReferenceObjectSafe(wait_body) != 0)
        return STATUS_INVALID_HANDLE;

    /* Signal the first object */
    sig_entry = ObpLookupHandle(&task_current()->handle_table, signal_h);
    if (!sig_entry || !sig_entry->object) {
        ObDereferenceObject(wait_body);
        return STATUS_INVALID_HANDLE;
    }

    sig_hdr = OB_HEADER_FROM_BODY(sig_entry->object);

    if (sig_hdr->type == ObpEventType) {
        event_set(&((EVENT_OBJECT *)sig_entry->object)->event);
    } else if (sig_hdr->type == ObpMutexType) {
        /* Releasing a mutant requires ownership, same as NtReleaseMutant */
        MUTEX_OBJECT *mo = (MUTEX_OBJECT *)sig_entry->object;
        if (!mutant_owned_by_current(mo)) {
            ObDereferenceObject(wait_body);
            return STATUS_MUTANT_NOT_OWNED;
        }
        if (mo->recursion > 0)
            mo->recursion--;
        else
            mutex_unlock(&mo->mutex);
    } else if (sig_hdr->type == ObpSemaphoreType) {
        /* Same MaximumCount enforcement as NtReleaseSemaphore */
        status = sem_release_checked(
            (SEMAPHORE_OBJECT *)sig_entry->object, 1, (int32_t *)0);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(wait_body);
            return status;
        }
    } else {
        ObDereferenceObject(wait_body);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    /* Then wait on the second (pinned) object */
    status = wait_on_body(wait_body, wait_type, timeout_ms);
    ObDereferenceObject(wait_body);
    return status;
}

/* ======================================================================== */
/* KEYED EVENT stubs (SSDT 0x0084-0x0087) -- real implementation owned by  */
/* the futex/keyed-event work in 03-memory-concurrency/TODO-08            */
/* ======================================================================== */

static NTSTATUS NtKeyedEvent_stub(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_NOT_IMPLEMENTED;
}

/* ======================================================================== */
/* Registration                                                             */
/* ======================================================================== */

void nt_sync_register_ssdt(void)
{
    /* Event (0x0070-0x0075) */
    ssdt_register(SSDT_NtCreateEvent,       (SSDT_HANDLER)NtCreateEvent_handler);
    ssdt_register(SSDT_NtOpenEvent,         (SSDT_HANDLER)NtOpenEvent_handler);
    ssdt_register(SSDT_NtSetEvent,          (SSDT_HANDLER)NtSetEvent_handler);
    ssdt_register(SSDT_NtResetEvent,        (SSDT_HANDLER)NtResetEvent_handler);
    ssdt_register(SSDT_NtPulseEvent,        (SSDT_HANDLER)NtPulseEvent_handler);
    ssdt_register(SSDT_NtQueryEvent,        (SSDT_HANDLER)NtQueryEvent_handler);

    /* Mutant (0x0076-0x0079) */
    ssdt_register(SSDT_NtCreateMutant,      (SSDT_HANDLER)NtCreateMutant_handler);
    ssdt_register(SSDT_NtOpenMutant,        (SSDT_HANDLER)NtOpenMutant_handler);
    ssdt_register(SSDT_NtReleaseMutant,     (SSDT_HANDLER)NtReleaseMutant_handler);
    ssdt_register(SSDT_NtQueryMutant,       (SSDT_HANDLER)NtQueryMutant_handler);

    /* Semaphore (0x007A-0x007D) */
    ssdt_register(SSDT_NtCreateSemaphore,   (SSDT_HANDLER)NtCreateSemaphore_handler);
    ssdt_register(SSDT_NtOpenSemaphore,     (SSDT_HANDLER)NtOpenSemaphore_handler);
    ssdt_register(SSDT_NtReleaseSemaphore,  (SSDT_HANDLER)NtReleaseSemaphore_handler);
    ssdt_register(SSDT_NtQuerySemaphore,    (SSDT_HANDLER)NtQuerySemaphore_handler);

    /* Wait (0x0006-0x0008) -- override NtWaitForSingleObject */
    ssdt_register(SSDT_NtWaitForSingleObject,          (SSDT_HANDLER)NtWaitForSingleObject_handler);
    ssdt_register(SSDT_NtWaitForMultipleObjects,       (SSDT_HANDLER)NtWaitForMultipleObjects_handler);
    ssdt_register(SSDT_NtSignalAndWaitForSingleObject, (SSDT_HANDLER)NtSignalAndWait_handler);

    /* Keyed events (0x0084-0x0087) -- stubs; owned by 03-memory-concurrency/TODO-08 */
    ssdt_register(SSDT_NtCreateKeyedEvent,    (SSDT_HANDLER)NtKeyedEvent_stub);
    ssdt_register(SSDT_NtOpenKeyedEvent,      (SSDT_HANDLER)NtKeyedEvent_stub);
    ssdt_register(SSDT_NtWaitForKeyedEvent,   (SSDT_HANDLER)NtKeyedEvent_stub);
    ssdt_register(SSDT_NtReleaseKeyedEvent,   (SSDT_HANDLER)NtKeyedEvent_stub);

    klog(LOG_INFO, "nt", "NT sync: 21 handlers registered (SSDT 0x0070-0x0087 + 0x0006-0x0008)");
}
