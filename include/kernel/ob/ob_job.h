/* ============================================================================
 * ob_job.h -- Job Object type for the Object Manager (Win32 Job Objects)
 *
 * A JOB_OBJECT is a process-group container: processes are assigned to a job,
 * accounting is aggregated across members, an active-process count limit is
 * enforced at assign time, and NtTerminateJobObject / KILL_ON_JOB_CLOSE kill
 * every member. Membership is a fixed-cap dense array (a job can hold at most
 * TASK_MAX processes) guarded by one per-job spinlock; each membership holds
 * ONE Ob reference on the job body, so the body outlives every member. The
 * KILL_ON_JOB_CLOSE action fires from on_close (handle_count -> 0), NOT
 * on_delete (ref_count -> 0), because membership refs keep the body alive past
 * the last handle close.
 *
 * Design review (2026-07-12, needs-attention: 4 High + 1 Medium, each verified
 * at file:line via superpowers:receiving-code-review) reshaped this: typed
 * task->job with a per-membership Ob ref, centralized detach used by every
 * process-death path, fork-time inheritance before child publication, and
 * fail-closed rejection of unenforced resource limits.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"         /* NTSTATUS */
#include "kernel/sched/spinlock.h"
#include "kernel/sched/task.h"          /* TASK_MAX, struct task */
#include "kernel/ob/ob_type.h"          /* OBJECT_TYPE */
#include "kernel/ob/handle_table.h"     /* HANDLE, HANDLE_TABLE */

/* --- Info classes (JOBOBJECTINFOCLASS subset) ---------------------------- */
#define JobObjectBasicAccountingInformation      1
#define JobObjectBasicLimitInformation           2
#define JobObjectBasicProcessIdList              3
#define JobObjectBasicAndIoAccountingInformation 8
#define JobObjectExtendedLimitInformation        9

/* --- Limit flags (JOBOBJECT_BASIC_LIMIT_INFORMATION.LimitFlags) ---------- */
#define JOB_OBJECT_LIMIT_WORKINGSET              0x00000001u
#define JOB_OBJECT_LIMIT_PROCESS_TIME            0x00000002u
#define JOB_OBJECT_LIMIT_JOB_TIME                0x00000004u
#define JOB_OBJECT_LIMIT_ACTIVE_PROCESS          0x00000008u
#define JOB_OBJECT_LIMIT_AFFINITY                0x00000010u
#define JOB_OBJECT_LIMIT_PRIORITY_CLASS          0x00000020u
#define JOB_OBJECT_LIMIT_PRESERVE_JOB_TIME       0x00000040u
#define JOB_OBJECT_LIMIT_SCHEDULING_CLASS        0x00000080u
#define JOB_OBJECT_LIMIT_PROCESS_MEMORY          0x00000100u
#define JOB_OBJECT_LIMIT_JOB_MEMORY              0x00000200u
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE       0x00002000u

/* Limit flags this implementation actually honors. ACTIVE_PROCESS is enforced
 * at assign time; KILL_ON_JOB_CLOSE is enforced at on_close. Every other flag
 * would require scheduler / MM / I/O enforcement infrastructure that does not
 * exist yet, so NtSetInformationJobObject rejects it (STATUS_NOT_SUPPORTED)
 * rather than accepting a limit it cannot enforce (a false containment
 * boundary). See the deferred resource-limit items in the Job Object section
 * of todo/02-kernel-core/TODO-21-process-model-extensions.md. */
#define JOB_SUPPORTED_LIMIT_FLAGS \
    (JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)

/* Maximum members: a job can hold at most every task in the system. */
#define JOB_MAX_MEMBERS   TASK_MAX

/* --- Windows accounting / limit ABI structures --------------------------- */
/* Times are 100ns units (Windows LARGE_INTEGER convention); sizes are bytes. */

typedef struct io_counters {
    uint64_t ReadOperationCount;
    uint64_t WriteOperationCount;
    uint64_t OtherOperationCount;
    uint64_t ReadTransferCount;
    uint64_t WriteTransferCount;
    uint64_t OtherTransferCount;
} IO_COUNTERS;

typedef struct jobobject_basic_accounting_information {
    int64_t  TotalUserTime;
    int64_t  TotalKernelTime;
    int64_t  ThisPeriodTotalUserTime;
    int64_t  ThisPeriodTotalKernelTime;
    uint32_t TotalPageFaultCount;
    uint32_t TotalProcesses;
    uint32_t ActiveProcesses;
    uint32_t TotalTerminatedProcesses;
} JOBOBJECT_BASIC_ACCOUNTING_INFORMATION;

typedef struct jobobject_basic_limit_information {
    int64_t  PerProcessUserTimeLimit;
    int64_t  PerJobUserTimeLimit;
    uint32_t LimitFlags;
    uint64_t MinimumWorkingSetSize;
    uint64_t MaximumWorkingSetSize;
    uint32_t ActiveProcessLimit;
    uint64_t Affinity;
    uint32_t PriorityClass;
    uint32_t SchedulingClass;
} JOBOBJECT_BASIC_LIMIT_INFORMATION;

typedef struct jobobject_extended_limit_information {
    JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
    IO_COUNTERS IoInfo;
    uint64_t ProcessMemoryLimit;
    uint64_t JobMemoryLimit;
    uint64_t PeakProcessMemoryUsed;
    uint64_t PeakJobMemoryUsed;
} JOBOBJECT_EXTENDED_LIMIT_INFORMATION;

typedef struct jobobject_basic_process_id_list {
    uint32_t NumberOfAssignedProcesses;
    uint32_t NumberOfProcessIdsInList;
    uint64_t ProcessIdList[1];   /* flexible; caller sizes the buffer */
} JOBOBJECT_BASIC_PROCESS_ID_LIST;

typedef struct jobobject_basic_and_io_accounting_information {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION BasicInfo;
    IO_COUNTERS IoInfo;
} JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION;

/* --- JOB_OBJECT body ----------------------------------------------------- */

typedef struct job_object {
    spinlock_t lock;                       /* protects members + counts + flags */
    uint32_t   member_pids[JOB_MAX_MEMBERS];
    uint32_t   num_members;                /* current live members (== active) */
    uint32_t   total_processes;            /* ever assigned (monotonic) */
    uint32_t   total_terminated;           /* members that died while in the job */
    uint8_t    terminated;                 /* set by NtTerminateJobObject: no new joins */
    /* Persistent accounting for DEPARTED members: a member's CPU/I/O totals are
     * folded in here the moment it detaches, so a job's aggregate does not
     * shrink when a member exits (Windows keeps departed usage in the job).
     * Live members are summed on top of these at query time. */
    uint64_t   acc_user_ns;
    uint64_t   acc_kernel_ns;
    uint64_t   acc_read_ops;
    uint64_t   acc_write_ops;
    uint64_t   acc_read_bytes;
    uint64_t   acc_write_bytes;
    /* Stored limits. Only ACTIVE_PROCESS + KILL_ON_JOB_CLOSE are honored;
     * limit_flags never contains an unsupported bit (set path rejects them). */
    uint32_t   limit_flags;
    uint32_t   active_process_limit;       /* valid iff LIMIT_ACTIVE_PROCESS set */
} JOB_OBJECT;

/* The registered Job Object type (for ObLookupObjectByName type checks). */
extern const OBJECT_TYPE *ObpJobType;

/* --- Type registration --------------------------------------------------- */
void ob_job_type_init(void);

/* --- Object create / open (handle-returning) ----------------------------- */
HANDLE ob_job_create(HANDLE_TABLE *ht, const char *name);
void  *ob_job_open(const char *name);   /* referenced body or NULL */

/* --- Membership operations (called from the syscall layer) --------------- */
NTSTATUS ob_job_assign(JOB_OBJECT *job, struct task *t);
NTSTATUS ob_job_terminate(JOB_OBJECT *job, int32_t exit_code);
int      ob_job_is_member(JOB_OBJECT *job, uint32_t pid);

/* --- Query / set helpers (fill kernel-local structs; caller copies to user) */
void ob_job_collect_accounting(JOB_OBJECT *job,
                               JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *acct,
                               IO_COUNTERS *io);
void ob_job_collect_limits(JOB_OBJECT *job, JOBOBJECT_BASIC_LIMIT_INFORMATION *lim);
NTSTATUS ob_job_set_basic_limits(JOB_OBJECT *job,
                                 const JOBOBJECT_BASIC_LIMIT_INFORMATION *lim);
void ob_job_collect_pid_list(JOB_OBJECT *job, uint64_t *out, uint32_t capacity,
                             uint32_t *written, uint32_t *assigned);

/* --- Membership lifecycle (called from the scheduler) -------------------- */

/* Detach a dying/terminating task from its job, if any. Idempotent and
 * single-shot per (task,job): clears task->job, removes the pid, drops the
 * membership Ob reference. Called from EVERY process-death path (task_exit,
 * NtTerminateProcess remote kill, NtTerminateJobObject). Takes the job lock
 * internally; releases it before ObDereferenceObject. Safe to call on a task
 * with no job. */
void ob_job_detach_task(struct task *t);

/* Inherit the parent's job into a forking child BEFORE the child is published
 * (num_tasks++). Reserves a membership slot + takes an Ob reference under the
 * parent job lock, fails CLOSED (returns -1) if the job is terminated or at
 * capacity so a child can never escape active-process limits or job-wide
 * termination. Returns 0 if the parent has no job (nothing to inherit) or on
 * success. On any later fork failure the caller rolls back via
 * ob_job_detach_task(child). */
int ob_job_fork_inherit(struct task *child, struct task *parent);

/* --- Syscall surface (implemented in nt_job.c) --------------------------- */
void nt_job_register_ssdt(void);
