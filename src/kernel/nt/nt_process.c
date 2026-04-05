/* ============================================================================
 * nt_process.c -- NT process and thread lifecycle SSDT handlers
 *
 * Wraps existing task/thread infrastructure with NT-compatible SSDT handlers.
 * SSDT indices 0x0030-0x0047 (Process and Thread range).
 * ============================================================================ */

#include "kernel/nt/nt_process.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/sched/task.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_process.h"
#include "kernel/ob/ob_thread.h"
#include "kernel/klog.h"
#include "kernel/timer.h"

/* ---- Helper: look up task by HANDLE (currently PID) --------------------- */
static struct task *task_from_handle(HANDLE h)
{
    uint32_t pid;
    if (h == CURRENT_PROCESS || h == 0)
        return task_current();
    pid = (uint32_t)(uint64_t)(int32_t)h;
    if (pid >= task_count())
        return (struct task *)0;
    return task_get_by_pid(pid);
}

/* ---- Helper: look up thread in a task ----------------------------------- */
static struct thread *thread_from_handle(HANDLE h, struct task **out_task)
{
    struct task *cur = task_current();
    uint32_t tid;

    if (h == CURRENT_THREAD || h == 0) {
        if (out_task) *out_task = cur;
        return thread_current();
    }

    /* Handle encodes TID for current process threads */
    tid = (uint32_t)(uint64_t)(int32_t)h;
    if (tid < cur->num_threads) {
        if (out_task) *out_task = cur;
        return &cur->threads[tid];
    }
    return (struct thread *)0;
}

/* ---- NtCreateProcess (0x0030) -------------------------------------------
 * a1 = HANDLE* ProcessHandle (out), a2 = ACCESS_MASK,
 * a3 = OBJECT_ATTRIBUTES* (name), a4 = HANDLE ParentProcess,
 * a5 = InheritObjectTable (bool), a6 = 0 (reserved).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateProcess_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    uint32_t inherit = (uint32_t)a5;
    int pid;

    (void)a2; (void)a3; (void)a4; (void)a6;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;

    /* Create a kernel task (no entry point -- caller must exec into it) */
    pid = task_create((task_entry_t)0, "NtProcess");
    if (pid < 0)
        return STATUS_NO_MEMORY;

    /* Inherit handles from parent if requested */
    if (inherit) {
        struct task *child = task_get_by_pid((uint32_t)pid);
        if (child)
            ob_handle_table_inherit(&task_current()->handle_table,
                                    &child->handle_table);
    }

    /* Allocate handle for the new process in caller's table */
    {
        HANDLE_TABLE_ENTRY *entry;
        struct task *child = task_get_by_pid((uint32_t)pid);
        HANDLE h;

        if (!child)
            return STATUS_UNSUCCESSFUL;

        /* Find the PROCESS_OBJECT via OB lookup -- for now, return PID as handle */
        h = ObpAllocateHandle(&task_current()->handle_table,
                              child, GENERIC_ALL, 0);
        if (h == INVALID_HANDLE_VALUE)
            return STATUS_NO_MEMORY;

        *out_handle = h;
        (void)entry;
    }

    klog(LOG_DEBUG, "nt", "NtCreateProcess: PID %u created", (uint64_t)pid);
    return STATUS_SUCCESS;
}

/* ---- NtOpenProcess (0x0032) ---------------------------------------------
 * a1 = HANDLE* out, a2 = ACCESS_MASK, a3 = OBJECT_ATTRIBUTES*,
 * a4 = CLIENT_ID* (UniqueProcess = PID).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtOpenProcess_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    ACCESS_MASK access = (ACCESS_MASK)a2;
    CLIENT_ID *cid = (CLIENT_ID *)a4;
    struct task *t;
    HANDLE h;

    (void)a3; (void)a5; (void)a6;

    if (!out_handle || !cid)
        return STATUS_INVALID_PARAMETER;

    t = task_get_by_pid(cid->UniqueProcess);
    if (!t || t->state == TASK_DEAD)
        return STATUS_INVALID_PARAMETER;

    h = ObpAllocateHandle(&task_current()->handle_table, t, access, 0);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;

    *out_handle = h;
    return STATUS_SUCCESS;
}

/* ---- NtCreateThread (0x0036) --------------------------------------------
 * a1 = HANDLE* ThreadHandle (out), a2 = ACCESS_MASK,
 * a3 = HANDLE ProcessHandle, a4 = thread_entry_t EntryPoint,
 * a5 = void* Argument, a6 = CreateSuspended (bool).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateThread_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    thread_entry_t entry = (thread_entry_t)a4;
    void *arg = (void *)a5;
    uint32_t create_suspended = (uint32_t)a6;
    int tid;

    (void)a2; (void)a3;

    if (!out_handle || !entry)
        return STATUS_INVALID_PARAMETER;

    tid = thread_create(entry, arg, THREAD_STACK_SIZE);
    if (tid < 0)
        return STATUS_NO_MEMORY;

    /* Handle CreateSuspended */
    if (create_suspended) {
        struct task *cur = task_current();
        if ((uint32_t)tid < cur->num_threads) {
            cur->threads[tid].suspend_count = 1;
            cur->threads[tid].state = TASK_BLOCKED;
        }
    }

    /* Return thread ID as handle (simplified; proper OB handle allocation later) */
    *out_handle = (HANDLE)tid;

    klog(LOG_DEBUG, "nt", "NtCreateThread: TID %u (suspended=%u)",
         (uint64_t)tid, (uint64_t)create_suspended);
    return STATUS_SUCCESS;
}

/* ---- NtOpenThread (0x0038) ----------------------------------------------
 * a1 = HANDLE* out, a2 = ACCESS_MASK, a3 = OBJECT_ATTRIBUTES*,
 * a4 = CLIENT_ID* (UniqueProcess + UniqueThread).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtOpenThread_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    CLIENT_ID *cid = (CLIENT_ID *)a4;
    struct task *t;

    (void)a2; (void)a3; (void)a5; (void)a6;

    if (!out_handle || !cid)
        return STATUS_INVALID_PARAMETER;

    t = task_get_by_pid(cid->UniqueProcess);
    if (!t || cid->UniqueThread >= t->num_threads)
        return STATUS_INVALID_PARAMETER;

    *out_handle = (HANDLE)cid->UniqueThread;
    return STATUS_SUCCESS;
}

/* ---- NtTerminateProcess (0x0033) ----------------------------------------
 * Upgraded from §5: proper handle lookup.
 * a1 = HANDLE, a2 = NTSTATUS ExitStatus.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtTerminateProcess_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    NTSTATUS exit_code = (NTSTATUS)(int32_t)a2;
    struct task *t;

    (void)a3; (void)a4; (void)a5; (void)a6;

    t = task_from_handle(handle);
    if (!t)
        return STATUS_INVALID_HANDLE;

    if (t == task_current()) {
        task_exit(exit_code);
        return STATUS_SUCCESS;
    }

    if (t->state == TASK_DEAD)
        return STATUS_PROCESS_IS_TERMINATING;

    t->state = TASK_DEAD;
    t->exit_status = (int32_t)exit_code;
    ob_process_mark_dead(t->pid);
    klog(LOG_DEBUG, "nt", "NtTerminateProcess: PID %u (0x%x)",
         (uint64_t)t->pid, (uint64_t)(uint32_t)exit_code);
    return STATUS_SUCCESS;
}

/* ---- NtTerminateThread (0x0039) -----------------------------------------
 * a1 = HANDLE ThreadHandle, a2 = NTSTATUS ExitStatus.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtTerminateThread_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    NTSTATUS exit_code = (NTSTATUS)(int32_t)a2;
    struct task *owner;
    struct thread *thr;

    (void)a3; (void)a4; (void)a5; (void)a6;

    thr = thread_from_handle(handle, &owner);
    if (!thr)
        return STATUS_INVALID_HANDLE;

    if (thr == thread_current()) {
        thread_exit(exit_code);
        return STATUS_SUCCESS;
    }

    thr->state = TASK_DEAD;
    thr->exit_status = (int32_t)exit_code;
    if (owner)
        ob_thread_mark_dead(owner->pid, thr->id);
    return STATUS_SUCCESS;
}

/* ---- NtSuspendThread (0x003B) -------------------------------------------
 * a1 = HANDLE ThreadHandle, a2 = uint32_t* PreviousSuspendCount (out).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtSuspendThread_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    uint32_t *prev_count = (uint32_t *)a2;
    struct task *owner;
    struct thread *thr;

    (void)a3; (void)a4; (void)a5; (void)a6;

    thr = thread_from_handle(handle, &owner);
    if (!thr)
        return STATUS_INVALID_HANDLE;

    if (prev_count)
        *prev_count = thr->suspend_count;

    if (thr->suspend_count >= 127)
        return STATUS_SUSPEND_COUNT_EXCEEDED;

    thr->suspend_count++;
    if (thr->state == TASK_READY || thr->state == TASK_RUNNING)
        thr->state = TASK_BLOCKED;

    return STATUS_SUCCESS;
}

/* ---- NtResumeThread (0x003A) --------------------------------------------
 * a1 = HANDLE ThreadHandle, a2 = uint32_t* PreviousSuspendCount (out).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtResumeThread_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    uint32_t *prev_count = (uint32_t *)a2;
    struct task *owner;
    struct thread *thr;

    (void)a3; (void)a4; (void)a5; (void)a6;

    thr = thread_from_handle(handle, &owner);
    if (!thr)
        return STATUS_INVALID_HANDLE;

    if (prev_count)
        *prev_count = thr->suspend_count;

    if (thr->suspend_count == 0)
        return STATUS_SUCCESS;

    thr->suspend_count--;
    if (thr->suspend_count == 0 && thr->state == TASK_BLOCKED)
        thr->state = TASK_READY;

    return STATUS_SUCCESS;
}

/* ---- NtQueryInformationThread (0x003E) ----------------------------------
 * a1 = HANDLE, a2 = info class, a3 = buffer, a4 = length,
 * a5 = uint32_t* ReturnLength.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtQueryInformationThread_handler(uint64_t a1, uint64_t a2,
                                                  uint64_t a3, uint64_t a4,
                                                  uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    uint32_t info_class = (uint32_t)a2;
    void *buffer = (void *)a3;
    uint32_t length = (uint32_t)a4;
    uint32_t *ret_length = (uint32_t *)a5;
    struct task *owner;
    struct thread *thr;

    (void)a6;

    thr = thread_from_handle(handle, &owner);
    if (!thr || !buffer)
        return STATUS_INVALID_PARAMETER;

    switch (info_class) {
    case ThreadBasicInformation: {
        THREAD_BASIC_INFORMATION *info = (THREAD_BASIC_INFORMATION *)buffer;
        if (length < sizeof(THREAD_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        info->ExitStatus = thr->exit_status;
        info->TebBaseAddress = owner ? owner->teb : (void *)0;
        info->UniqueProcessId = owner ? owner->pid : 0;
        info->_pad0 = 0;
        info->UniqueThreadId = thr->id;
        info->_pad1 = 0;
        info->BasePriority = thr->base_priority;
        info->Priority = thr->priority;
        if (ret_length)
            *ret_length = (uint32_t)sizeof(THREAD_BASIC_INFORMATION);
        return STATUS_SUCCESS;
    }
    case ThreadPriority:
    case ThreadBasePriority: {
        uint32_t *prio = (uint32_t *)buffer;
        if (length < sizeof(uint32_t))
            return STATUS_BUFFER_TOO_SMALL;
        *prio = (info_class == ThreadPriority)
                ? thr->priority : thr->base_priority;
        if (ret_length) *ret_length = sizeof(uint32_t);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ---- NtSetInformationThread (0x003F) ------------------------------------
 * a1 = HANDLE, a2 = info class, a3 = buffer, a4 = length.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtSetInformationThread_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    uint32_t info_class = (uint32_t)a2;
    void *buffer = (void *)a3;
    uint32_t length = (uint32_t)a4;
    struct task *owner;
    struct thread *thr;

    (void)a5; (void)a6;

    thr = thread_from_handle(handle, &owner);
    if (!thr || !buffer)
        return STATUS_INVALID_PARAMETER;

    switch (info_class) {
    case ThreadPriority:
    case ThreadBasePriority: {
        uint32_t prio;
        if (length < sizeof(uint32_t))
            return STATUS_BUFFER_TOO_SMALL;
        prio = *(uint32_t *)buffer;
        if (prio > THREAD_PRIO_REALTIME)
            return STATUS_INVALID_PARAMETER;
        if (owner)
            thread_set_priority(owner->pid, thr->id, prio);
        return STATUS_SUCCESS;
    }
    case ThreadAffinityMask: {
        /* Store but don't enforce yet (SMP affinity is future work) */
        return STATUS_SUCCESS;
    }
    case ThreadIdealProcessor: {
        /* Store but don't enforce yet */
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ---- NtQueryInformationProcess (0x0034) ---------------------------------
 * a1 = HANDLE, a2 = info class, a3 = buffer, a4 = length,
 * a5 = uint32_t* ReturnLength.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtQueryInformationProcess_handler(uint64_t a1, uint64_t a2,
                                                   uint64_t a3, uint64_t a4,
                                                   uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    uint32_t info_class = (uint32_t)a2;
    void *buffer = (void *)a3;
    uint32_t length = (uint32_t)a4;
    uint32_t *ret_length = (uint32_t *)a5;
    struct task *t;

    (void)a6;

    t = task_from_handle(handle);
    if (!t || !buffer)
        return STATUS_INVALID_PARAMETER;

    switch (info_class) {
    case ProcessBasicInformation: {
        PROCESS_BASIC_INFORMATION *info = (PROCESS_BASIC_INFORMATION *)buffer;
        if (length < sizeof(PROCESS_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        info->ExitStatus = t->exit_status;
        info->_pad0 = 0;
        info->PebBaseAddress = t->peb;
        info->AffinityMask = 0xFFFFFFFF;
        info->BasePriority = t->threads[0].base_priority;
        info->UniqueProcessId = t->pid;
        info->InheritedFromUniqueProcessId = t->parent_pid;
        info->_pad1 = 0;
        if (ret_length)
            *ret_length = (uint32_t)sizeof(PROCESS_BASIC_INFORMATION);
        return STATUS_SUCCESS;
    }
    case ProcessPriorityClass: {
        uint32_t *pclass = (uint32_t *)buffer;
        if (length < sizeof(uint32_t))
            return STATUS_BUFFER_TOO_SMALL;
        *pclass = t->threads[0].base_priority;
        if (ret_length) *ret_length = sizeof(uint32_t);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ---- NtSetInformationProcess (0x0035) -----------------------------------
 * a1 = HANDLE, a2 = info class, a3 = buffer, a4 = length.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtSetInformationProcess_handler(uint64_t a1, uint64_t a2,
                                                 uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    uint32_t info_class = (uint32_t)a2;
    void *buffer = (void *)a3;
    uint32_t length = (uint32_t)a4;
    struct task *t;

    (void)a5; (void)a6;

    t = task_from_handle(handle);
    if (!t || !buffer)
        return STATUS_INVALID_PARAMETER;

    switch (info_class) {
    case ProcessPriorityClass: {
        uint32_t prio;
        if (length < sizeof(uint32_t))
            return STATUS_BUFFER_TOO_SMALL;
        prio = *(uint32_t *)buffer;
        if (prio > THREAD_PRIO_REALTIME)
            return STATUS_INVALID_PARAMETER;
        thread_set_priority(t->pid, 0, prio);
        return STATUS_SUCCESS;
    }
    case ProcessDefaultHardErrorMode:
        return STATUS_SUCCESS;  /* accept but ignore */
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ---- NtAlertThread (0x0040) --------------------------------------------- */
static NTSTATUS NtAlertThread_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Alert system requires APC infrastructure (TODO-06 §11) */
    return STATUS_SUCCESS;
}

/* ---- NtAlertResumeThread (0x0041) --------------------------------------- */
static NTSTATUS NtAlertResumeThread_handler(uint64_t a1, uint64_t a2,
                                            uint64_t a3, uint64_t a4,
                                            uint64_t a5, uint64_t a6)
{
    /* Alert + resume: alert the thread, then resume */
    NtAlertThread_handler(a1, 0, 0, 0, 0, 0);
    return NtResumeThread_handler(a1, a2, a3, a4, a5, a6);
}

/* ---- NtTestAlert (0x0046) ----------------------------------------------- */
static NTSTATUS NtTestAlert_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Check pending APC -- requires APC infrastructure (TODO-06 §11) */
    return STATUS_SUCCESS;
}

/* ---- NtDelayExecution (0x0047) ------------------------------------------
 * a1 = Alertable (bool), a2 = int64_t* DelayInterval (100-ns units,
 * negative = relative, positive = absolute).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtDelayExecution_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    int64_t *interval = (int64_t *)a2;
    uint64_t delay_ms;
    uint64_t start;

    (void)a1; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!interval)
        return STATUS_INVALID_PARAMETER;

    /* Convert 100-ns interval to milliseconds.
     * Negative = relative delay; positive = absolute (not supported yet). */
    if (*interval >= 0)
        return STATUS_NOT_SUPPORTED;  /* absolute time not implemented */

    delay_ms = (uint64_t)(-*interval) / 10000;  /* 100ns -> ms */
    if (delay_ms == 0)
        delay_ms = 1;

    /* Yield-loop sleep using uptime tick counter */
    start = uptime();
    while ((uptime() - start) * 1000 < delay_ms)
        yield();

    return STATUS_SUCCESS;
}

/* ---- Stubs for blocked dependencies ------------------------------------- */

/* NtGetContextThread (0x003C) -- needs CONTEXT from TODO-10 */
static NTSTATUS NtGetContextThread_stub(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_NOT_IMPLEMENTED;
}

/* NtSetContextThread (0x003D) -- needs CONTEXT from TODO-10 */
static NTSTATUS NtSetContextThread_stub(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_NOT_IMPLEMENTED;
}

/* NtQueueApcThread (0x0043) -- needs APC from TODO-06 §11 */
static NTSTATUS NtQueueApcThread_stub(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_NOT_IMPLEMENTED;
}

/* NtImpersonateThread (0x0042) -- needs SRM from TODO-11 */
static NTSTATUS NtImpersonateThread_stub(uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_NOT_IMPLEMENTED;
}

/* NtCreateUserProcess (0x0045) -- needs full PE loader from TODO-09 */
static NTSTATUS NtCreateUserProcess_stub(uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_NOT_IMPLEMENTED;
}

/* ---- Registration ------------------------------------------------------- */

void nt_process_register_ssdt(void)
{
    /* Process creation and lookup */
    ssdt_register(SSDT_NtCreateProcess,     (SSDT_HANDLER)NtCreateProcess_handler);
    ssdt_register(SSDT_NtCreateProcessEx,   (SSDT_HANDLER)NtCreateProcess_handler);
    ssdt_register(SSDT_NtOpenProcess,       (SSDT_HANDLER)NtOpenProcess_handler);
    ssdt_register(SSDT_NtTerminateProcess,  (SSDT_HANDLER)NtTerminateProcess_handler);

    /* Thread creation, lookup, and control */
    ssdt_register(SSDT_NtCreateThread,      (SSDT_HANDLER)NtCreateThread_handler);
    ssdt_register(SSDT_NtCreateThreadEx,    (SSDT_HANDLER)NtCreateThread_handler);
    ssdt_register(SSDT_NtOpenThread,        (SSDT_HANDLER)NtOpenThread_handler);
    ssdt_register(SSDT_NtTerminateThread,   (SSDT_HANDLER)NtTerminateThread_handler);
    ssdt_register(SSDT_NtResumeThread,      (SSDT_HANDLER)NtResumeThread_handler);
    ssdt_register(SSDT_NtSuspendThread,     (SSDT_HANDLER)NtSuspendThread_handler);

    /* Thread context (stubs -- need CONTEXT from TODO-10) */
    ssdt_register(SSDT_NtGetContextThread,  (SSDT_HANDLER)NtGetContextThread_stub);
    ssdt_register(SSDT_NtSetContextThread,  (SSDT_HANDLER)NtSetContextThread_stub);

    /* Information queries */
    ssdt_register(SSDT_NtQueryInformationThread,  (SSDT_HANDLER)NtQueryInformationThread_handler);
    ssdt_register(SSDT_NtSetInformationThread,    (SSDT_HANDLER)NtSetInformationThread_handler);
    ssdt_register(SSDT_NtQueryInformationProcess, (SSDT_HANDLER)NtQueryInformationProcess_handler);
    ssdt_register(SSDT_NtSetInformationProcess,   (SSDT_HANDLER)NtSetInformationProcess_handler);

    /* Alert, APC, impersonation */
    ssdt_register(SSDT_NtAlertThread,       (SSDT_HANDLER)NtAlertThread_handler);
    ssdt_register(SSDT_NtAlertResumeThread, (SSDT_HANDLER)NtAlertResumeThread_handler);
    ssdt_register(SSDT_NtTestAlert,         (SSDT_HANDLER)NtTestAlert_handler);
    ssdt_register(SSDT_NtQueueApcThread,    (SSDT_HANDLER)NtQueueApcThread_stub);
    ssdt_register(SSDT_NtImpersonateThread, (SSDT_HANDLER)NtImpersonateThread_stub);

    /* Combined process + thread creation */
    ssdt_register(SSDT_NtCreateUserProcess, (SSDT_HANDLER)NtCreateUserProcess_stub);

    /* Execution control */
    ssdt_register(SSDT_NtDelayExecution,    (SSDT_HANDLER)NtDelayExecution_handler);

    klog(LOG_INFO, "nt", "NT process/thread: 23 handlers registered (SSDT 0x0030-0x0047)");
}
