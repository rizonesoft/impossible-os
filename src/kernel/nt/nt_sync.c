/* ============================================================================
 * nt_sync.c -- NT synchronization object SSDT handlers
 *
 * Event, mutex (mutant), semaphore SSDT wrappers routing through the
 * Object Manager types established in TODO-03 section 6.  Also provides
 * NtWaitForSingleObject (upgraded), NtWaitForMultipleObjects, and
 * NtSignalAndWaitForSingleObject.  Keyed events deferred to TODO-07.
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
#include "kernel/klog.h"
#include "kernel/timer.h"

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

    /* Check ownership -- mutex must be owned by caller */
    {
        struct task *cur = task_current();
        struct thread *thr = thread_current();
        if (mo->mutex.owner_task != cur->pid ||
            (thr && mo->mutex.owner_thread != thr->id))
            return STATUS_MUTANT_NOT_OWNED;
    }

    if (prev)
        *prev = mutex_is_locked(&mo->mutex) ? 0 : 1;

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
    info->CurrentCount = mutex_is_locked(&mo->mutex) ? 0 : 1;
    info->OwnedByCaller = (mo->mutex.owner_task == cur->pid) ? 1 : 0;
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
    int32_t i;

    (void)a4; (void)a5; (void)a6;

    if (release_count <= 0)
        return STATUS_INVALID_PARAMETER;

    so = (SEMAPHORE_OBJECT *)sync_lookup(handle, ObpSemaphoreType);
    if (!so)
        return STATUS_INVALID_HANDLE;

    if (prev)
        *prev = sem_value(&so->semaphore);

    /* Check if release would exceed maximum */
    if (sem_value(&so->semaphore) + release_count > so->max_count)
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;

    for (i = 0; i < release_count; i++)
        sem_signal(&so->semaphore);

    return STATUS_SUCCESS;
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

/* ---- Helper: wait on a single sync object by handle --------------------- */
static NTSTATUS wait_on_handle(HANDLE handle, uint32_t timeout_ms)
{
    HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(
        &task_current()->handle_table, handle);
    OBJECT_HEADER *hdr;

    if (!entry || !entry->object)
        return STATUS_INVALID_HANDLE;

    hdr = OB_HEADER_FROM_BODY(entry->object);

    /* Dispatch based on object type */
    if (hdr->type == ObpEventType) {
        EVENT_OBJECT *eo = (EVENT_OBJECT *)entry->object;
        if (timeout_ms == 0)
            return event_is_set(&eo->event) ? STATUS_SUCCESS : STATUS_TIMEOUT;
        if (timeout_ms == 0xFFFFFFFF) {
            event_wait(&eo->event);
            return STATUS_SUCCESS;
        }
        return event_wait_timeout(&eo->event, timeout_ms) == 0
               ? STATUS_SUCCESS : STATUS_TIMEOUT;
    }

    if (hdr->type == ObpMutexType) {
        MUTEX_OBJECT *mo = (MUTEX_OBJECT *)entry->object;
        if (timeout_ms == 0)
            return mutex_trylock(&mo->mutex) ? STATUS_SUCCESS : STATUS_TIMEOUT;
        mutex_lock(&mo->mutex);
        return mo->abandoned ? STATUS_ABANDONED : STATUS_SUCCESS;
    }

    if (hdr->type == ObpSemaphoreType) {
        SEMAPHORE_OBJECT *so = (SEMAPHORE_OBJECT *)entry->object;
        if (timeout_ms == 0)
            return sem_trywait(&so->semaphore) ? STATUS_SUCCESS : STATUS_TIMEOUT;
        sem_wait(&so->semaphore);
        return STATUS_SUCCESS;
    }

    if (hdr->type == ObpProcessType) {
        /* Wait for process termination */
        struct task *t = ((PROCESS_OBJECT *)entry->object)->task;
        if (!t)
            return STATUS_INVALID_HANDLE;
        if (t->state == TASK_DEAD)
            return STATUS_SUCCESS;
        task_waitpid(t->pid);
        return STATUS_SUCCESS;
    }

    if (hdr->type == ObpTimerType) {
        /* Waiting on a timer blocks until the next fire. The NT timer
         * tick ISR (src/kernel/nt/nt_timer.c) calls event_set() when
         * due_ns is reached; auto-reset timers self-clear on consumption. */
        TIMER_OBJECT *to = (TIMER_OBJECT *)entry->object;
        if (timeout_ms == 0)
            return event_is_set(&to->event) ? STATUS_SUCCESS : STATUS_TIMEOUT;
        if (timeout_ms == 0xFFFFFFFF) {
            event_wait(&to->event);
            return STATUS_SUCCESS;
        }
        return event_wait_timeout(&to->event, timeout_ms) == 0
               ? STATUS_SUCCESS : STATUS_TIMEOUT;
    }

    return STATUS_OBJECT_TYPE_MISMATCH;
}

/* ---- Helper: convert NT timeout (100-ns) to ms -------------------------- */
static uint32_t nt_timeout_to_ms(uint64_t timeout_ptr)
{
    int64_t *tp = (int64_t *)timeout_ptr;
    if (!tp)
        return 0xFFFFFFFF;  /* infinite */
    if (*tp == 0)
        return 0;           /* poll */
    if (*tp < 0)
        return (uint32_t)((uint64_t)(-*tp) / 10000);  /* relative 100-ns to ms */
    return 0xFFFFFFFF;  /* absolute not supported yet -- treat as infinite */
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
    uint32_t timeout_ms;
    uint32_t i;

    (void)a4; (void)a6;

    if (!handles || count == 0 || count > MAXIMUM_WAIT_OBJECTS)
        return STATUS_INVALID_PARAMETER;

    timeout_ms = nt_timeout_to_ms(a5);

    if (wait_type == WaitAny) {
        /* Poll all handles, yield, repeat until one is satisfied */
        uint64_t start = uptime();
        for (;;) {
            for (i = 0; i < count; i++) {
                NTSTATUS s = wait_on_handle(handles[i], 0);
                if (NT_SUCCESS(s))
                    return (NTSTATUS)(STATUS_WAIT_0 + i);
            }
            if (timeout_ms == 0)
                return STATUS_TIMEOUT;
            if (timeout_ms != 0xFFFFFFFF &&
                (uptime() - start) * 1000 >= timeout_ms)
                return STATUS_TIMEOUT;
            yield();
        }
    }

    /* WaitAll: wait for each handle sequentially */
    for (i = 0; i < count; i++) {
        NTSTATUS s = wait_on_handle(handles[i], timeout_ms);
        if (!NT_SUCCESS(s) && s != STATUS_ABANDONED)
            return s;
    }
    return STATUS_SUCCESS;
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
    HANDLE_TABLE_ENTRY *sig_entry;
    OBJECT_HEADER *sig_hdr;

    (void)a3; (void)a5; (void)a6;

    timeout_ms = nt_timeout_to_ms(a4);

    /* Signal the first object */
    sig_entry = ObpLookupHandle(&task_current()->handle_table, signal_h);
    if (!sig_entry || !sig_entry->object)
        return STATUS_INVALID_HANDLE;

    sig_hdr = OB_HEADER_FROM_BODY(sig_entry->object);

    if (sig_hdr->type == ObpEventType)
        event_set(&((EVENT_OBJECT *)sig_entry->object)->event);
    else if (sig_hdr->type == ObpMutexType)
        mutex_unlock(&((MUTEX_OBJECT *)sig_entry->object)->mutex);
    else if (sig_hdr->type == ObpSemaphoreType)
        sem_signal(&((SEMAPHORE_OBJECT *)sig_entry->object)->semaphore);
    else
        return STATUS_OBJECT_TYPE_MISMATCH;

    /* Then wait on the second object */
    return wait_on_handle(wait_h, timeout_ms);
}

/* ======================================================================== */
/* KEYED EVENT stubs (SSDT 0x0084-0x0087) -- deferred to TODO-07           */
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

    /* Wait (0x0006-0x0008) -- override §5 NtWaitForSingleObject */
    ssdt_register(SSDT_NtWaitForSingleObject,          (SSDT_HANDLER)NtWaitForSingleObject_handler);
    ssdt_register(SSDT_NtWaitForMultipleObjects,       (SSDT_HANDLER)NtWaitForMultipleObjects_handler);
    ssdt_register(SSDT_NtSignalAndWaitForSingleObject, (SSDT_HANDLER)NtSignalAndWait_handler);

    /* Keyed events (0x0084-0x0087) -- deferred to TODO-07 */
    ssdt_register(SSDT_NtCreateKeyedEvent,    (SSDT_HANDLER)NtKeyedEvent_stub);
    ssdt_register(SSDT_NtOpenKeyedEvent,      (SSDT_HANDLER)NtKeyedEvent_stub);
    ssdt_register(SSDT_NtWaitForKeyedEvent,   (SSDT_HANDLER)NtKeyedEvent_stub);
    ssdt_register(SSDT_NtReleaseKeyedEvent,   (SSDT_HANDLER)NtKeyedEvent_stub);

    klog(LOG_INFO, "nt", "NT sync: 21 handlers registered (SSDT 0x0070-0x0087 + 0x0006-0x0008)");
}
