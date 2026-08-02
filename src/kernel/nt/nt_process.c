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
#include "kernel/nt/syscall_filter.h"
#include "kernel/nt/pledge.h"
#include "kernel/nt/mitigation_policy.h"
#include "kernel/nt/zw.h"           /* ProbeForReadIfUser / ProbeForWrite / ssdt_previous_mode */
#include "kernel/cpu_security.h"    /* copy_from_user / copy_to_user */
#include "kernel/mm/heap.h"         /* kfree (inherited-filter cleanup) */
#include "kernel/nt/nt_unicode.h"   /* nt_decode_unicode_string / nt_unicode_to_ascii */
#include "kernel/fs/vfs.h"          /* vfs_open / vfs_close / VFS_DIRECTORY (cwd validation) */
#include "kernel/env.h"             /* env_set_drive_cwd (hidden =X: per-drive cwd, TODO-22 s12) */
#include "kernel/quota/quota_policy.h"     /* ProcessQuotaLimits projection + commit txn */
#include "kernel/security/privileges.h"    /* SeSinglePrivilegeCheck / SeIncreaseQuotaPrivilege */

/* Write a ReturnLength out-parameter safely (probe + bounce), for the quota
 * class. The older classes in this file store through the caller's pointer
 * directly, which is a ring-3-controlled kernel write; the quota class does not
 * extend that boundary. A NULL pointer is legal and succeeds silently.
 * -> XREF: TODO-12-native-api-ssdt.md section 6 (usercopy hardening). */
static NTSTATUS quota_write_ret_length(uint32_t *ret_length, uint32_t value)
{
    NTSTATUS st;

    if (!ret_length)
        return STATUS_SUCCESS;
    st = ProbeForWriteIfUser(ret_length, (uint32_t)sizeof(value), 4);
    if (st != STATUS_SUCCESS)
        return st;
    if (copy_to_user(ret_length, &value, (uint32_t)sizeof(value)) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

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
    struct syscall_filter *inherited_filter = (struct syscall_filter *)0;
    int pid;

    (void)a2; (void)a3; (void)a4; (void)a6;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;

    /* MIT_NO_CHILD_PROCESS: a process that set the child-process mitigation
     * policy cannot spawn children. Checked against the CALLER (task_current),
     * before any allocation, so a blocked create leaks nothing. */
    if (task_mitigation_get(task_current()) & MIT_NO_CHILD_PROCESS)
        return STATUS_CHILD_PROCESS_BLOCKED;

    /* Per-process syscall filter inheritance. A spawned process is a fresh
     * image (not a copy of the parent), so inheritance is OPT-IN via the
     * parent filter's SYSCALL_FILTER_INHERIT flag -- distinct from fork, where
     * the filter always carries. Pre-clone BEFORE creating the child so an
     * inheritable sandbox fails CLOSED under memory pressure (return an error)
     * rather than spawning an unfiltered child. Reading the parent snapshot
     * locklessly is safe: it is immutable and never freed while the parent
     * lives. */
    {
        struct syscall_filter *pf = __atomic_load_n(
            &task_current()->syscall_filter, __ATOMIC_ACQUIRE);
        if (pf && (pf->flags & SYSCALL_FILTER_INHERIT)) {
            inherited_filter = syscall_filter_clone(pf);
            if (!inherited_filter)
                return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /* Create a kernel task (no entry point -- caller must exec into it) */
    pid = task_create((task_entry_t)0, "NtProcess");
    if (pid < 0) {
        if (inherited_filter)
            kfree(inherited_filter);
        return STATUS_NO_MEMORY;
    }

    /* pledge/unveil ALWAYS carry to a spawned child (unlike the opt-in syscall
     * filter) -- a pledged parent must not escape by spawning an unrestricted
     * child. A pledged-without-proc parent is already terminated at the
     * dispatcher before reaching here. An unpledged parent clones nothing. */
    {
        struct task *child = task_get_by_pid((uint32_t)pid);
        if (child && pledge_unveil_inherit(child, task_current()) != 0) {
            /* OOM cloning the unveil set: refuse to run an under-restricted
             * child. Run the FULL remote-death teardown (marks DEAD so
             * find_next_task skips it + it is reaped, AND detaches the Job
             * membership task_create inherited -- omitting the detach leaked the
             * membership reference + active-process quota). task_terminate_remote
             * does NOT free the task, so the mid-task_wrapper stack-free UAF the
             * old direct-write guarded against still cannot happen. Closing the
             * pre-exec entry==0 window is the unpublished-child-construction work
             * in the process-exit-cleanup TODO. */
            task_terminate_remote(child, (int32_t)STATUS_INSUFFICIENT_RESOURCES);
            if (inherited_filter)
                kfree(inherited_filter);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /* Inherit handles from parent if requested. Best-effort: ob_handle_table_inherit
     * grows the child to cover all inheritable handles and warns if memory
     * pressure forced it to drop some. We do NOT fail here -- the child task is
     * already created with no teardown path, so failing would leak it, whereas a
     * partially-inherited process runs and frees its task on exit. Atomic
     * all-or-fail (which needs a child-task teardown helper) is a tracked
     * Handle-Inheritance follow-up item. */
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

        if (!child) {
            if (inherited_filter)
                kfree(inherited_filter);
            return STATUS_UNSUCCESSFUL;
        }

        /* Find the PROCESS_OBJECT via OB lookup -- for now, return PID as handle */
        h = ObpAllocateHandle(&task_current()->handle_table,
                              child, GENERIC_ALL, 0);
        if (h == INVALID_HANDLE_VALUE) {
            /* Free the un-attached clone: attaching earlier would leak the
             * filter + inflate g_syscall_filter_count when this last fallible
             * step fails (e.g. caller handle-table exhausted). The child task
             * itself still leaks here -- a pre-existing gap for the handle-
             * inheritance rollback follow-up, not introduced by the filter. */
            if (inherited_filter)
                kfree(inherited_filter);
            return STATUS_NO_MEMORY;
        }

        *out_handle = h;
        (void)entry;

        /* Publish the pre-cloned filter only AFTER every fallible step has
         * succeeded, so no error path leaves an attached-but-leaked filter. */
        if (inherited_filter)
            syscall_filter_attach(child, inherited_filter);
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

    /* Verify user thread got a TEB (uthread_create allocates one; if it
     * failed, uthread_create already returned -1, but guard defensively). */
    {
        struct task *cur = task_current();
        if (cur->peb && (uint32_t)tid < cur->num_threads &&
            !cur->threads[tid].teb) {
            klog(LOG_ERROR, "nt",
                 "NtCreateThread: TID %u has no TEB", (uint64_t)tid);
            return STATUS_NO_MEMORY;
        }

        /* Handle CreateSuspended */
        if (create_suspended && (uint32_t)tid < cur->num_threads) {
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
 * Upgraded: proper handle lookup.
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

    /* pledge: self-termination is survival-core, but terminating ANOTHER process
     * requires the `proc` category (matching the legacy SYS_KILL gate). The
     * coarse dispatcher classifies NtTerminateProcess as core because the target
     * is argument-dependent; enforce the distinction here. Snapshot the caller
     * ONCE (the current-task cursor is a global read; per-CPU tracking is a
     * separate hardening item) and use it for both the identity and the check.
     * On violation the dispatcher terminates the caller after this returns. */
    {
        struct task *self = task_current();
        if (t != self && pledge_user_mode()) {
            uint64_t pm = self ? __atomic_load_n(&self->pledge_mask,
                                                 __ATOMIC_ACQUIRE) : 0;
            if ((pm & PLEDGE_PLEDGED) && !(pm & PLEDGE_PROC)) {
                struct thread *th = thread_current();
                if (th)
                    th->pledge_pending = 1;
                return STATUS_PLEDGE_VIOLATION;
            }
        }
        if (t == self) {
            task_exit(exit_code);
            return STATUS_SUCCESS;
        }
    }

    if (t->state == TASK_DEAD)
        return STATUS_PROCESS_IS_TERMINATING;

    /* Centralized remote-death transition: marks TASK_DEAD + exit status, drops
     * the target's syscall-filter count contribution (so a killed-but-unreaped
     * filtered process stops arming the global dispatch fast path), marks the OB
     * process object dead, AND detaches any Job Object membership -- the last of
     * which this inline sequence previously missed, leaving stale job state on a
     * remote kill. */
    task_terminate_remote(t, (int32_t)exit_code);
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

    /* pledge: terminating a thread in ANOTHER process requires `proc`, like
     * NtTerminateProcess and SYS_KILL. Own-thread termination is core. Snapshot
     * the caller once (global cursor read; see NtTerminateProcess). */
    {
        struct task *self = task_current();
        if (owner && owner != self && pledge_user_mode()) {
            uint64_t pm = self ? __atomic_load_n(&self->pledge_mask,
                                                 __ATOMIC_ACQUIRE) : 0;
            if ((pm & PLEDGE_PLEDGED) && !(pm & PLEDGE_PROC)) {
                struct thread *th = thread_current();
                if (th)
                    th->pledge_pending = 1;
                return STATUS_PLEDGE_VIOLATION;
            }
        }
    }

    if (thr == thread_current()) {
        thread_exit(exit_code);
        return STATUS_SUCCESS;
    }

#ifdef KERNEL_TESTS
    /* This path publishes THREAD_DEAD directly rather than routing through
     * thread_exit or task_terminate_remote, so it is its own death transition
     * and owes the same capture snapshot they take. Without it a sibling could
     * terminate a thread parked inside a sub-chunk write and the abandoned
     * payload would carry no reason at reap. */
    if (owner)
        task_utest_cap_note_thread_death(owner, thr);
#endif
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
        info->TebBaseAddress = thr->teb ? thr->teb :
                                (owner ? owner->teb : (void *)0);
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

    /* pledge: self-query is survival-core, but querying ANOTHER process's image
     * name / PEB / parent / priority requires `proc` (task_from_handle resolves
     * any numeric handle as a PID, bypassing NtOpenProcess). Snapshot self once. */
    {
        struct task *self = task_current();
        if (t != self && pledge_user_mode()) {
            uint64_t pm = self ? __atomic_load_n(&self->pledge_mask,
                                                 __ATOMIC_ACQUIRE) : 0;
            if ((pm & PLEDGE_PLEDGED) && !(pm & PLEDGE_PROC)) {
                struct thread *th = thread_current();
                if (th)
                    th->pledge_pending = 1;
                return STATUS_PLEDGE_VIOLATION;
            }
        }
    }

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
    case ProcessTimes: {
        /* KERNEL_USER_TIMES: creation, exit, kernel, user times */
        struct {
            uint64_t CreateTime;
            uint64_t ExitTime;
            uint64_t KernelTime;
            uint64_t UserTime;
        } *times = buffer;
        if (length < 32)
            return STATUS_BUFFER_TOO_SMALL;
        times->CreateTime = 0;  /* not tracked yet */
        times->ExitTime = 0;
        times->KernelTime = 0;
        times->UserTime = 0;
        if (ret_length) *ret_length = 32;
        return STATUS_SUCCESS;
    }
    case ProcessDebugPort: {
        uint64_t *port = (uint64_t *)buffer;
        if (length < sizeof(uint64_t))
            return STATUS_BUFFER_TOO_SMALL;
        *port = 0;  /* not being debugged */
        if (ret_length) *ret_length = sizeof(uint64_t);
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
    case ProcessHandleCount: {
        uint32_t *hcount = (uint32_t *)buffer;
        if (length < sizeof(uint32_t))
            return STATUS_BUFFER_TOO_SMALL;
        *hcount = t->handle_table.count;
        if (ret_length) *ret_length = sizeof(uint32_t);
        return STATUS_SUCCESS;
    }
    case ProcessSessionInformation: {
        uint32_t *session = (uint32_t *)buffer;
        if (length < sizeof(uint32_t))
            return STATUS_BUFFER_TOO_SMALL;
        *session = 0;  /* session 0 (console) */
        if (ret_length) *ret_length = sizeof(uint32_t);
        return STATUS_SUCCESS;
    }
    case ProcessWow64Information: {
        uint64_t *wow64_peb = (uint64_t *)buffer;
        if (length < sizeof(uint64_t))
            return STATUS_BUFFER_TOO_SMALL;
        *wow64_peb = 0;  /* native 64-bit, no WoW64 */
        if (ret_length) *ret_length = sizeof(uint64_t);
        return STATUS_SUCCESS;
    }
    case ProcessImageFileName: {
        /* Return process name as a simple string */
        char *out = (char *)buffer;
        uint32_t i;
        if (!t->name) {
            if (length >= 1) out[0] = '\0';
            if (ret_length) *ret_length = 1;
            return STATUS_SUCCESS;
        }
        for (i = 0; i < length - 1 && t->name[i]; i++)
            out[i] = t->name[i];
        out[i] = '\0';
        if (ret_length) *ret_length = i + 1;
        return STATUS_SUCCESS;
    }
    case ProcessQuotaLimits: {
        /* Windows exposes process quotas here, as PROCESSINFOCLASS 1 -- there
         * is deliberately no separate quota syscall pair (one surface, one
         * authorization path). Self-only for the same reason the syscall-filter
         * class below is: task_from_handle resolves any integer handle as a raw
         * PID with no granted-access check, so a cross-process read here would
         * disclose another process's limits without PROCESS_QUERY_INFORMATION.
         * Probe + bounce-copy rather than writing the caller's pointer
         * directly (the sibling classes' raw-deref pattern is the known kernel-
         * write boundary; this class does not extend it). */
        QUOTA_LIMITS_EX limits;
        uint32_t out_len;
        NTSTATUS st;

        if (t != task_current())
            return STATUS_ACCESS_DENIED;
        /* Size-based dispatch: the caller's LENGTH selects the structure. A
         * short buffer is never reinterpreted by peeking at a suffix field. */
        if (length >= QUOTA_LIMITS_EX_SIZE)
            out_len = QUOTA_LIMITS_EX_SIZE;
        else if (length >= QUOTA_LIMITS_SIZE)
            out_len = QUOTA_LIMITS_SIZE;
        else {
            /* Even the failure path's length hint goes through the checked
             * write: *ret_length = X on a ring-3 pointer is a kernel write to
             * a caller-chosen address. A probe failure does not mask the
             * BUFFER_TOO_SMALL verdict -- the caller still learns the buffer
             * was short, just without the hint. */
            (void)quota_write_ret_length(ret_length, QUOTA_LIMITS_SIZE);
            return STATUS_BUFFER_TOO_SMALL;
        }
        st = quota_policy_query(t, &limits);
        if (st != STATUS_SUCCESS)
            return st;
        st = ProbeForWriteIfUser(buffer, out_len, 8);
        if (st != STATUS_SUCCESS)
            return st;
        if (copy_to_user(buffer, &limits, out_len) != 0)
            return STATUS_ACCESS_VIOLATION;
        return quota_write_ret_length(ret_length, out_len);
    }
    case ProcessMitigationPolicy:
        /* Ring-3 query DEFERRED (the set path above defers for the mirror
         * reason): writing the result to a caller-supplied buffer (or
         * ReturnLength) needs PTE-aware, fault-recoverable usercopy.
         * ProbeForWrite is range-only (address < MM_USER_PROBE_ADDRESS) and the
         * kernel heap is identity-mapped inside that window, so copy_to_user to
         * an aligned kernel address would corrupt kernel state -- the systemic
         * usercopy gap. The MIT_NO_CHILD_PROCESS policy is enforced
         * kernel-internally (task_mitigation_apply); both ring-3 handlers land
         * with the fault-recoverable usercopy. */
        return STATUS_NOT_SUPPORTED;
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
    case ProcessSystemCallFilterPolicy: {
        /* Install/tighten the target's per-process SSDT filter. The policy
         * buffer comes from a ring-3 caller, so probe + bounce-copy it into a
         * kernel-owned struct before touching it (this is a security boundary;
         * do not follow the sibling cases' raw-deref pattern here). */
        PROCESS_SYSCALL_FILTER_POLICY pol;
        NTSTATUS st;
        /* Restrict to the CALLER'S OWN process. task_from_handle still
         * resolves a handle as a raw PID with no granted-access check (the
         * systemic NT handle-rights enforcement gap, owned by the token
         * lifecycle / SRM access-check work), so a cross-process install here
         * would be an unprivileged syscall-DoS on an arbitrary PID -- including
         * an irreversible LOCKED filter. The secure model is self +
         * SYSCALL_FILTER_INHERIT (children inherit at creation); authorized
         * cross-process install waits on real handle rights. t ==
         * task_current() also implies t is live (not DEAD). */
        if (t != task_current())
            return STATUS_ACCESS_DENIED;
        if (length < sizeof(pol))
            return STATUS_BUFFER_TOO_SMALL;
        st = ProbeForReadIfUser(buffer, sizeof(pol), 8);
        if (st != STATUS_SUCCESS)
            return st;
        if (copy_from_user(&pol, buffer, (uint32_t)sizeof(pol)) != 0)
            return STATUS_ACCESS_VIOLATION;
        return syscall_filter_set_policy(t, &pol);
    }
    case ProcessQuotaLimits: {
        /* Self-only, for a stronger reason than the query: SeIncreaseQuota-
         * Privilege answers whether an increase is privileged, NOT which
         * process may be modified. Without the self check an unprivileged
         * caller could lower an arbitrary PID's limits, and a task slot reused
         * between the lookup and the commit would redirect the transaction into
         * a different process entirely. Cross-process quota set waits on real
         * handle rights (PROCESS_SET_QUOTA) over reference-counted process
         * handles. t == task_current() also implies t is live. */
        QUOTA_LIMITS_EX limits;
        uint32_t in_len;
        int is_ex;
        int privileged;
        NTSTATUS st;

        if (t != task_current())
            return STATUS_ACCESS_DENIED;
        if (length >= QUOTA_LIMITS_EX_SIZE) {
            in_len = QUOTA_LIMITS_EX_SIZE;
            is_ex = 1;
        } else if (length >= QUOTA_LIMITS_SIZE) {
            in_len = QUOTA_LIMITS_SIZE;
            is_ex = 0;
        } else {
            return STATUS_BUFFER_TOO_SMALL;
        }
        /* Zero the whole record first: a 48-byte caller supplies no suffix, and
         * the unwritten tail must read as "unset", never as stack residue. */
        {
            uint8_t *p = (uint8_t *)&limits;
            uint32_t i;
            for (i = 0; i < (uint32_t)sizeof(limits); i++)
                p[i] = 0;
        }
        st = ProbeForReadIfUser(buffer, in_len, 8);
        if (st != STATUS_SUCCESS)
            return st;
        if (copy_from_user(&limits, buffer, in_len) != 0)
            return STATUS_ACCESS_VIOLATION;
        /* The privilege verdict is computed HERE because this is the only layer
         * that knows the request's previous mode; quota_policy_set consumes it
         * as a boolean, exactly like task_rlimit_set's caller_privileged. */
        privileged = SeSinglePrivilegeCheck(&SeIncreaseQuotaPrivilege,
                                            ssdt_previous_mode());
        return quota_policy_set(t, &limits, is_ex, privileged);
    }
    case ProcessMitigationPolicy:
        /* Ring-3 SET DEFERRED (same reason as the query above): reading the
         * caller's policy buffer needs PTE-aware, fault-recoverable usercopy.
         * ProbeForRead is range-only and copy_from_user is not fault-safe, so
         * an aligned, in-range but UNMAPPED buffer would take a kernel-mode
         * page fault (an unprivileged ring-3 crash/DoS) instead of returning an
         * error -- the systemic usercopy gap. The mitigation_flags field +
         * MIT_NO_CHILD_PROCESS enforcement + fork inheritance ship and are set
         * via the kernel-internal task_mitigation_apply; the ring-3 setter
         * lands with the fault-recoverable usercopy. */
        return STATUS_NOT_SUPPORTED;
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ---- NtAlertThread (0x0040) --------------------------------------------- */
static NTSTATUS NtAlertThread_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Alert system requires APC infrastructure */
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
    /* Check pending APC -- requires APC infrastructure */
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

/* NtQueueApcThread (0x0043) -- needs APC infrastructure */
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

/* ---- NtSetCurrentDirectory (0x03D9) -------------------------------------
 * a1 = UNICODE_STRING *Path. Resolves against the caller's cwd, requires the
 * target to exist and be a directory, then commits it. task->cwd is the single
 * source of truth; the user PEB CurrentDirectory is a creation-time mirror that
 * ntdll keeps in sync in user mode (kernel does not write the user PEB here --
 * that avoids maintaining a second kernel-authoritative copy, per the design
 * review). Leaves cwd unchanged on any failure. */
static NTSTATUS NtSetCurrentDirectory_handler(uint64_t a1, uint64_t a2,
                                              uint64_t a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6)
{
    const UNICODE_STRING *path = (const UNICODE_STRING *)a1;
    uint32_t prev_mode = ssdt_previous_mode();
    uint16_t wbuf[TASK_CWD_MAX];
    char in[TASK_CWD_MAX];
    char resolved[TASK_CWD_MAX];
    uint32_t wchars = 0;
    struct task *t = task_current();
    struct vfs_node *node;
    NTSTATUS st;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!path)
        return STATUS_INVALID_PARAMETER;

    /* Decode the (untrusted) UNICODE_STRING and narrow to an ANSI path. */
    st = nt_decode_unicode_string(path, wbuf, TASK_CWD_MAX, &wchars, prev_mode);
    if (st != STATUS_SUCCESS)
        return st;
    st = nt_unicode_to_ascii(wbuf, wchars, in, sizeof(in), (uint32_t *)0);
    if (st != STATUS_SUCCESS)
        return st;

    /* Canonicalize against the current cwd (handles ".", "..", relative). */
    if (task_resolve_path(in, resolved, sizeof(resolved)) != 0)
        return STATUS_NAME_TOO_LONG;

    /* Entering a directory reads path metadata: rpath pledge + unveil 'r'. */
    if (pledge_user_mode()) {
        if (pledge_check_file(t, VFS_O_READ) != STATUS_SUCCESS)
            return STATUS_PLEDGE_VIOLATION;   /* pending flag set; tail terminates */
        if (unveil_check(t, resolved, UNVEIL_R) != STATUS_SUCCESS)
            return STATUS_ACCESS_DENIED;
    }

    /* Must exist AND be a directory. Release the probe ref on every path. */
    node = vfs_open(resolved, VFS_O_READ);
    if (!node)
        return STATUS_OBJECT_PATH_NOT_FOUND;
    if (node->type != VFS_DIRECTORY) {
        vfs_close(node);
        return STATUS_NOT_A_DIRECTORY;
    }
    vfs_close(node);

    /* Commit the hidden "=X:" per-drive current-directory variable AND the task
     * cwd as ONE serialized transaction under t->chdir_lock (TODO-22 s12). This
     * closes the adversarial-review race: two threads changing to the SAME drive
     * concurrently could otherwise interleave the two separate-lock writes and
     * commit different winners to cwd vs "=X:" (both calls succeeding), leaving a
     * permanent divergence that a later cross-drive "X:relative" open resolves
     * against the stale value. chdir_lock is a sleeping mutex (the env update
     * allocates); it is the OUTER lock, never held with environ_lock/cwd_lock
     * simultaneously (env_set and task_set_cwd take + release their own locks).
     *
     * Within the transaction, do the fallible allocating env update FIRST so an
     * OOM / block-cap returns the failure with cwd unchanged (never "success with
     * a stale =X:"); task_set_cwd then cannot fail (resolved already fit
     * TASK_CWD_MAX in task_resolve_path), so the two writes cannot diverge.
     * resolved[0] is the drive letter of the absolute "X:\..." path. */
    mutex_lock(&t->chdir_lock);
    {
        int erc = env_set_drive_cwd(t, resolved[0], resolved);
        if (erc == ENV_ERR_NOMEM || erc == ENV_ERR_NOSPACE) {
            mutex_unlock(&t->chdir_lock);
            return STATUS_NO_MEMORY;        /* leave cwd unchanged -- no divergence */
        }
        /* Any other negative (e.g. a non-letter drive, which resolved absolute
         * paths never carry) is non-fatal; the cwd change proceeds. */
    }
    if (task_set_cwd(t, resolved) != 0) {
        mutex_unlock(&t->chdir_lock);
        return STATUS_NAME_TOO_LONG;
    }
    mutex_unlock(&t->chdir_lock);

    klog(LOG_DEBUG, "task", "cwd set to %s (pid %u)", resolved, (uint64_t)t->pid);
    return STATUS_SUCCESS;
}

/* ---- NtQueryCurrentDirectory (0x03DA) -----------------------------------
 * a1 = WCHAR *Buffer (out), a2 = ULONG BufferLength (bytes). Writes the cwd as
 * a NUL-terminated UTF-16 string. STATUS_BUFFER_TOO_SMALL if it does not fit. */
static NTSTATUS NtQueryCurrentDirectory_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    uint16_t *ubuf = (uint16_t *)a1;
    uint32_t buf_bytes = (uint32_t)a2;
    uint32_t prev_mode = ssdt_previous_mode();
    char cwd[TASK_CWD_MAX];
    uint16_t wbuf[TASK_CWD_MAX];
    uint32_t clen = 0, need_bytes;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!ubuf || buf_bytes < sizeof(uint16_t))
        return STATUS_INVALID_PARAMETER;

    task_get_cwd(task_current(), cwd, sizeof(cwd));
    while (cwd[clen] && clen < TASK_CWD_MAX - 1) {
        wbuf[clen] = (uint16_t)(uint8_t)cwd[clen];
        clen++;
    }
    wbuf[clen] = 0;
    need_bytes = (clen + 1) * (uint32_t)sizeof(uint16_t);
    if (buf_bytes < need_bytes)
        return STATUS_BUFFER_TOO_SMALL;

    if (prev_mode == SSDT_USER_MODE) {
        NTSTATUS pw = ProbeForWrite(ubuf, need_bytes, 2);
        if (pw != STATUS_SUCCESS)
            return pw;
        if (copy_to_user(ubuf, wbuf, need_bytes) != 0)
            return STATUS_ACCESS_VIOLATION;
    } else {
        uint32_t i;
        for (i = 0; i < clen + 1; i++)
            ubuf[i] = wbuf[i];
    }
    return STATUS_SUCCESS;
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

    /* Working directory (TODO-21 process model extensions) */
    ssdt_register(SSDT_NtSetCurrentDirectory,   (SSDT_HANDLER)NtSetCurrentDirectory_handler);
    ssdt_register(SSDT_NtQueryCurrentDirectory, (SSDT_HANDLER)NtQueryCurrentDirectory_handler);

    /* Execution control */
    ssdt_register(SSDT_NtDelayExecution,    (SSDT_HANDLER)NtDelayExecution_handler);

    klog(LOG_INFO, "nt", "NT process/thread: 23 handlers registered (SSDT 0x0030-0x0047)");
}
