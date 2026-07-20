/* ============================================================================
 * nt_job.c -- Job Object syscalls (SSDT 0x0160-0x0167)
 *
 * The ring-3 ABI marshalling layer over the ob_job.c subsystem: resolves
 * handles, validates user buffers + lengths, builds results in kernel-local
 * storage, and copies them out. All object lifetime, membership, accounting,
 * and limit logic lives in ob_job.c.
 *
 * Handle model: process handles are resolved by the codebase-wide PID-as-handle
 * scheme (proc_from_handle), which carries no granted-access check -- the same
 * limitation every nt_process.c syscall has. Upgrading to typed PROCESS_OBJECT
 * handles with access masks is owned by the process-object handle model
 * (see the deferred XREFs in the Job Object section of TODO-21). Job handles
 * ARE typed (job_lookup validates ObpJobType).
 *
 * User-buffer copies are raw (matching the existing NT info-class handlers);
 * the probe + bounce-buffer usercopy campaign is owned elsewhere. Results are
 * always assembled in kernel-local storage FIRST, so the job spinlock is never
 * held across a user-memory access.
 * ============================================================================ */

#include "kernel/ob/ob_job.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/handle_table.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/nt_types.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"
#include "kernel/nt/zw.h"                  /* ProbeForReadIfUser / ssdt_previous_mode */
#include "kernel/cpu_security.h"           /* copy_from_user */
#include "kernel/security/privileges.h"    /* SeSinglePrivilegeCheck / SeIncreaseQuotaPrivilege */

/* --- Small helpers ------------------------------------------------------- */

static const char *job_oa_name(OBJECT_ATTRIBUTES *oa)
{
    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer)
        return (const char *)0;
    return (const char *)oa->ObjectName->Buffer;
}

/* Write a ReturnLength out-parameter safely. The surrounding classes store
 * through the caller's uint32_t* directly, which is a ring-3-controlled kernel
 * write; the quota class must not extend that, and a NULL pointer is legal
 * (the caller simply does not want the length). */
static NTSTATUS job_write_ret_len(uint32_t *ret_len, uint32_t value)
{
    NTSTATUS st;

    if (!ret_len)
        return STATUS_SUCCESS;
    st = ProbeForWriteIfUser(ret_len, (uint32_t)sizeof(value), 4);
    if (st != STATUS_SUCCESS)
        return st;
    if (copy_to_user(ret_len, &value, (uint32_t)sizeof(value)) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* Resolve a job HANDLE to its JOB_OBJECT body (type-checked). Returns the body
 * WITHOUT taking a reference -- the caller's open handle keeps the object alive
 * for the duration of the syscall (the existing per-object handler contract). */
static JOB_OBJECT *job_lookup(HANDLE handle)
{
    HANDLE_TABLE_ENTRY *entry =
        ObpLookupHandle(&task_current()->handle_table, handle);
    OBJECT_HEADER *hdr;

    if (!entry || !entry->object)
        return (JOB_OBJECT *)0;
    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpJobType)
        return (JOB_OBJECT *)0;
    return (JOB_OBJECT *)entry->object;
}

/* Resolve a process HANDLE to its task (PID-as-handle model; see file header). */
static struct task *proc_from_handle(HANDLE h)
{
    uint32_t pid;
    if (h == CURRENT_PROCESS || h == 0)
        return task_current();
    pid = (uint32_t)(uint64_t)(int32_t)h;
    if (pid >= task_count())
        return (struct task *)0;
    return task_get_by_pid(pid);
}

static void job_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

/* --- NtCreateJobObject (0x0160) -----------------------------------------
 * a1 = HANDLE* JobHandle (out), a2 = ACCESS_MASK, a3 = OBJECT_ATTRIBUTES*.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateJobObject_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *name;
    HANDLE h;

    (void)a2; (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    name = job_oa_name(oa);
    h = ob_job_create(&task_current()->handle_table, name);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;

    *out = h;
    klog(LOG_DEBUG, "job", "created job handle 0x%x", (uint64_t)(int32_t)h);
    return STATUS_SUCCESS;
}

/* --- NtOpenJobObject (0x0161) -------------------------------------------- */
static NTSTATUS NtOpenJobObject_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *name;
    void *body;

    (void)a2; (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    name = job_oa_name(oa);
    if (!name)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    body = ob_job_open(name);
    if (!body)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    *out = ObpAllocateHandle(&task_current()->handle_table, body, 0, 0);
    ObDereferenceObject(body);
    if (*out == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;
    return STATUS_SUCCESS;
}

/* --- NtAssignProcessToJobObject (0x0162) ---------------------------------
 * a1 = JobHandle, a2 = ProcessHandle.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtAssignProcessToJobObject_handler(uint64_t a1, uint64_t a2,
                                                   uint64_t a3, uint64_t a4,
                                                   uint64_t a5, uint64_t a6)
{
    JOB_OBJECT *job = job_lookup((HANDLE)(int32_t)a1);
    struct task *t = proc_from_handle((HANDLE)(int32_t)a2);
    NTSTATUS st;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!job)
        return STATUS_INVALID_HANDLE;
    if (!t)
        return STATUS_INVALID_HANDLE;
    if (t->state == TASK_DEAD)
        return STATUS_PROCESS_IS_TERMINATING;

    st = ob_job_assign(job, t);
    if (st == STATUS_SUCCESS)
        klog(LOG_DEBUG, "job", "assigned pid %u to job", (uint64_t)t->pid);
    return st;
}

/* --- NtTerminateJobObject (0x0163) ---------------------------------------
 * a1 = JobHandle, a2 = ExitStatus.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtTerminateJobObject_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    JOB_OBJECT *job = job_lookup((HANDLE)(int32_t)a1);
    int32_t exit_code = (int32_t)a2;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!job)
        return STATUS_INVALID_HANDLE;

    return ob_job_terminate(job, exit_code);
}

/* --- NtQueryInformationJobObject (0x0164) --------------------------------
 * a1 = JobHandle, a2 = InfoClass, a3 = Buffer, a4 = Length, a5 = RetLen* (out).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtQueryInformationJobObject_handler(uint64_t a1, uint64_t a2,
                                                    uint64_t a3, uint64_t a4,
                                                    uint64_t a5, uint64_t a6)
{
    JOB_OBJECT *job = job_lookup((HANDLE)(int32_t)a1);
    uint32_t info_class = (uint32_t)a2;
    void *buffer = (void *)a3;
    uint32_t length = (uint32_t)a4;
    uint32_t *ret_len = (uint32_t *)a5;

    (void)a6;

    if (!job)
        return STATUS_INVALID_HANDLE;
    if (!buffer)
        return STATUS_INVALID_PARAMETER;

    switch (info_class) {
    case JobObjectBasicAccountingInformation: {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
        if (length < sizeof(acct))
            return STATUS_BUFFER_TOO_SMALL;
        ob_job_collect_accounting(job, &acct, (IO_COUNTERS *)0);
        job_memcpy(buffer, &acct, sizeof(acct));
        if (ret_len) *ret_len = sizeof(acct);
        return STATUS_SUCCESS;
    }
    case JobObjectBasicAndIoAccountingInformation: {
        JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION both;
        if (length < sizeof(both))
            return STATUS_BUFFER_TOO_SMALL;
        ob_job_collect_accounting(job, &both.BasicInfo, &both.IoInfo);
        job_memcpy(buffer, &both, sizeof(both));
        if (ret_len) *ret_len = sizeof(both);
        return STATUS_SUCCESS;
    }
    case JobObjectBasicLimitInformation: {
        JOBOBJECT_BASIC_LIMIT_INFORMATION lim;
        if (length < sizeof(lim))
            return STATUS_BUFFER_TOO_SMALL;
        ob_job_collect_limits(job, &lim);
        job_memcpy(buffer, &lim, sizeof(lim));
        if (ret_len) *ret_len = sizeof(lim);
        return STATUS_SUCCESS;
    }
    case JobObjectExtendedLimitInformation: {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext;
        uint32_t i;
        if (length < sizeof(ext))
            return STATUS_BUFFER_TOO_SMALL;
        for (i = 0; i < sizeof(ext); i++)
            ((uint8_t *)&ext)[i] = 0;
        ob_job_collect_limits(job, &ext.BasicLimitInformation);
        job_memcpy(buffer, &ext, sizeof(ext));
        if (ret_len) *ret_len = sizeof(ext);
        return STATUS_SUCCESS;
    }
    case JobObjectQuotaLimitInformation: {
        /* Probe + bounce-copy, NOT the sibling classes' job_memcpy straight
         * into the caller's pointer: that pattern writes kernel data to
         * whatever address ring 3 supplied, which is a kernel-write primitive.
         * This class is new, so it does not inherit that boundary; the older
         * classes are repaired by their own owner.
         * -> XREF: TODO-12-native-api-ssdt.md section 6 (usercopy hardening). */
        JOBOBJECT_QUOTA_LIMIT_INFORMATION q;
        NTSTATUS st;

        if (length < sizeof(q)) {
            /* Hand back the size a caller needs, like the sibling process
             * quota class does -- a probe-then-size caller has no other way
             * to learn the V1 length. */
            (void)job_write_ret_len(ret_len, (uint32_t)sizeof(q));
            return STATUS_BUFFER_TOO_SMALL;
        }
        ob_job_collect_quota_limits(job, &q);
        st = ProbeForWriteIfUser(buffer, (uint32_t)sizeof(q), 8);
        if (st != STATUS_SUCCESS)
            return st;
        if (copy_to_user(buffer, &q, (uint32_t)sizeof(q)) != 0)
            return STATUS_ACCESS_VIOLATION;
        return job_write_ret_len(ret_len, (uint32_t)sizeof(q));
    }
    case JobObjectBasicProcessIdList: {
        JOBOBJECT_BASIC_PROCESS_ID_LIST *list =
            (JOBOBJECT_BASIC_PROCESS_ID_LIST *)buffer;
        uint64_t pids[JOB_MAX_MEMBERS];
        uint32_t header = (uint32_t)((uint8_t *)&list->ProcessIdList[0]
                                     - (uint8_t *)list);
        uint32_t capacity, written = 0, assigned = 0, i;

        if (length < header)
            return STATUS_BUFFER_TOO_SMALL;
        capacity = (length - header) / sizeof(uint64_t);
        if (capacity > JOB_MAX_MEMBERS)
            capacity = JOB_MAX_MEMBERS;

        ob_job_collect_pid_list(job, pids, capacity, &written, &assigned);

        list->NumberOfAssignedProcesses = assigned;
        list->NumberOfProcessIdsInList  = written;
        for (i = 0; i < written; i++)
            list->ProcessIdList[i] = pids[i];
        if (ret_len)
            *ret_len = header + written * (uint32_t)sizeof(uint64_t);
        /* Partial result if the caller's buffer could not hold every pid. */
        return (written < assigned) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* --- NtSetInformationJobObject (0x0165) ----------------------------------
 * a1 = JobHandle, a2 = InfoClass, a3 = Buffer, a4 = Length.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtSetInformationJobObject_handler(uint64_t a1, uint64_t a2,
                                                  uint64_t a3, uint64_t a4,
                                                  uint64_t a5, uint64_t a6)
{
    JOB_OBJECT *job = job_lookup((HANDLE)(int32_t)a1);
    uint32_t info_class = (uint32_t)a2;
    void *buffer = (void *)a3;
    uint32_t length = (uint32_t)a4;

    (void)a5; (void)a6;

    if (!job)
        return STATUS_INVALID_HANDLE;
    if (!buffer)
        return STATUS_INVALID_PARAMETER;

    switch (info_class) {
    case JobObjectBasicLimitInformation: {
        JOBOBJECT_BASIC_LIMIT_INFORMATION lim;
        if (length < sizeof(lim))
            return STATUS_BUFFER_TOO_SMALL;
        job_memcpy(&lim, buffer, sizeof(lim));   /* snapshot before validation */
        return ob_job_set_basic_limits(job, &lim);
    }
    case JobObjectExtendedLimitInformation: {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext;
        if (length < sizeof(ext))
            return STATUS_BUFFER_TOO_SMALL;
        job_memcpy(&ext, buffer, sizeof(ext));
        /* Only the BasicLimitInformation flags are honored; the memory/IO
         * fields require enforcement infrastructure that does not exist, so
         * ob_job_set_basic_limits rejects any unsupported LimitFlags bit
         * (STATUS_NOT_SUPPORTED) rather than silently accepting them. */
        return ob_job_set_basic_limits(job, &ext.BasicLimitInformation);
    }
    case JobObjectQuotaLimitInformation: {
        /* Probe + bounce-copy rather than the sibling classes' direct
         * job_memcpy from the caller pointer: this class RAISES limits behind a
         * privilege check, so it is a security boundary and does not inherit
         * the older classes' raw-deref pattern. */
        JOBOBJECT_QUOTA_LIMIT_INFORMATION q;
        NTSTATUS st;
        int privileged;

        if (length < sizeof(q))
            return STATUS_BUFFER_TOO_SMALL;
        st = ProbeForReadIfUser(buffer, (uint32_t)sizeof(q), 8);
        if (st != STATUS_SUCCESS)
            return st;
        if (copy_from_user(&q, buffer, (uint32_t)sizeof(q)) != 0)
            return STATUS_ACCESS_VIOLATION;
        privileged = SeSinglePrivilegeCheck(&SeIncreaseQuotaPrivilege,
                                            ssdt_previous_mode());
        return ob_job_set_quota_limits(job, &q, privileged);
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* --- NtIsProcessInJob (0x0166) -------------------------------------------
 * a1 = ProcessHandle, a2 = JobHandle.
 * If JobHandle is NULL, tests membership in ANY job (Win32 semantics).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtIsProcessInJob_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct task *t = proc_from_handle((HANDLE)(int32_t)a1);
    HANDLE job_handle = (HANDLE)(int32_t)a2;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!t)
        return STATUS_INVALID_HANDLE;

    if (job_handle == 0) {
        /* "Is the process in ANY job?" */
        return (t->job != NULL) ? STATUS_PROCESS_IN_JOB
                                : STATUS_PROCESS_NOT_IN_JOB;
    } else {
        JOB_OBJECT *job = job_lookup(job_handle);
        if (!job)
            return STATUS_INVALID_HANDLE;
        return ob_job_is_member(job, t->pid) ? STATUS_PROCESS_IN_JOB
                                             : STATUS_PROCESS_NOT_IN_JOB;
    }
}

/* --- NtCreateJobSet (0x0167) ---------------------------------------------
 * a1 = NumJob, a2 = UserJobSet*, a3 = Flags.
 * Job sets group jobs for job-set scheduling. This is a DEPRECATED Win32
 * surface (MSDN: "not supported and may be altered or unavailable"), and real
 * job-set scheduling requires scheduler infrastructure that does not exist.
 * We validate the arguments and accept the degenerate empty request; a
 * non-empty set is not implemented and is tracked for a scheduler owner. The
 * registration itself is a permanent promise (never removed).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateJobSet_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint32_t num_job = (uint32_t)a1;
    void *user_job_set = (void *)a2;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (num_job == 0)
        return STATUS_SUCCESS;          /* nothing to associate */
    if (!user_job_set)
        return STATUS_INVALID_PARAMETER;
    /* SCOPE-GAP-ALLOWED: real job-set scheduling needs scheduler infrastructure;
     * deferred to a concrete owner item filed in the Job Object section of
     * todo/02-kernel-core/TODO-21-process-model-extensions.md (deprecated Win32
     * surface -- validated + registered here, permanent SSDT promise kept). */
    return STATUS_NOT_IMPLEMENTED;
}

/* --- SSDT registration --------------------------------------------------- */

void nt_job_register_ssdt(void)
{
    ssdt_register(SSDT_NtCreateJobObject,          (SSDT_HANDLER)NtCreateJobObject_handler);
    ssdt_register(SSDT_NtOpenJobObject,            (SSDT_HANDLER)NtOpenJobObject_handler);
    ssdt_register(SSDT_NtAssignProcessToJobObject, (SSDT_HANDLER)NtAssignProcessToJobObject_handler);
    ssdt_register(SSDT_NtTerminateJobObject,       (SSDT_HANDLER)NtTerminateJobObject_handler);
    ssdt_register(SSDT_NtQueryInformationJobObject,(SSDT_HANDLER)NtQueryInformationJobObject_handler);
    ssdt_register(SSDT_NtSetInformationJobObject,  (SSDT_HANDLER)NtSetInformationJobObject_handler);
    ssdt_register(SSDT_NtIsProcessInJob,           (SSDT_HANDLER)NtIsProcessInJob_handler);
    ssdt_register(SSDT_NtCreateJobSet,             (SSDT_HANDLER)NtCreateJobSet_handler);
}
