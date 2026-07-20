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
#include "kernel/quota/quota.h"         /* QUOTA_RESOURCE_TYPE_COUNT, quota_* readers */

/* --- Info classes (JOBOBJECTINFOCLASS subset) ---------------------------- */
#define JobObjectBasicAccountingInformation      1
#define JobObjectBasicLimitInformation           2
#define JobObjectBasicProcessIdList              3
#define JobObjectBasicAndIoAccountingInformation 8
#define JobObjectExtendedLimitInformation        9

/* Impossible OS extension class. Numbered at 0x1000 like the extension system-
 * information classes, so it can never collide with a Windows JOBOBJECTINFO-
 * CLASS value this kernel has not implemented yet.
 *
 * Why an extension rather than filling JOBOBJECT_EXTENDED_LIMIT_INFORMATION's
 * ProcessMemoryLimit / JobMemoryLimit: those two are COMMITTED-memory limits,
 * and this kernel has no per-process commit accounting to project them from.
 * Reporting pool bytes in a field that means committed memory would be a
 * plausible-looking wrong answer. This class reports what the quota subsystem
 * genuinely knows -- per-resource usage, peak, limit and failure count for the
 * job's aggregate block -- and the Windows memory fields stay 0 (no limit set)
 * until commit accounting exists. */
#define JobObjectQuotaLimitInformation           0x1000

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

/* One row per registered quota resource type, in quota_resource_type_t order.
 * A row is self-describing: `Limit` 0 means unlimited (QUOTA_LIMIT_UNLIMITED),
 * matching the rest of the kernel. */
typedef struct jobobject_quota_resource {
    uint64_t Usage;
    uint64_t Peak;
    uint64_t Limit;
    uint64_t Failures;
} JOBOBJECT_QUOTA_RESOURCE;

/* FROZEN V1 row count. The array is deliberately NOT sized by
 * QUOTA_RESOURCE_TYPE_COUNT: that constant grows whenever the kernel registers
 * a new resource type, and a wire structure whose size tracks an internal enum
 * would silently change this information class's required buffer length --
 * every existing binary would start getting STATUS_BUFFER_TOO_SMALL, and a
 * newer caller would be rejected by an older kernel. The row count is part of
 * the ABI, so it is pinned here; ResourceCount reports how many rows the
 * running kernel actually populated. Adding resource types beyond V1 requires a
 * V2 class, not a wider array. */
#define JOB_QUOTA_V1_RESOURCE_COUNT   16U

typedef struct jobobject_quota_limit_information {
    uint32_t ResourceCount;    /* rows populated == quota_resource_type_count() */
    uint32_t Reserved;         /* must be zero */
    JOBOBJECT_QUOTA_RESOURCE Resources[JOB_QUOTA_V1_RESOURCE_COUNT];
} JOBOBJECT_QUOTA_LIMIT_INFORMATION;

/* Layer 1: the wire size is a constant, and the internal resource enum must fit
 * inside it. A new resource type that overflows V1 fails the build here rather
 * than truncating the report or moving the wire size under existing callers. */
_Static_assert(sizeof(JOBOBJECT_QUOTA_RESOURCE) == 32,
    "JOBOBJECT_QUOTA_RESOURCE must be 4 x uint64_t");
_Static_assert(__builtin_offsetof(JOBOBJECT_QUOTA_LIMIT_INFORMATION, Resources) == 8,
    "JOBOBJECT_QUOTA_LIMIT_INFORMATION.Resources offset");
_Static_assert(sizeof(JOBOBJECT_QUOTA_LIMIT_INFORMATION) ==
               8 + 32 * JOB_QUOTA_V1_RESOURCE_COUNT,
    "JOBOBJECT_QUOTA_LIMIT_INFORMATION V1 wire size is frozen at 520 bytes");
_Static_assert(QUOTA_RESOURCE_TYPE_COUNT <= JOB_QUOTA_V1_RESOURCE_COUNT,
    "a resource type past V1 needs a V2 information class, not a wider row array");

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
    /* Persistent accounting for DEPARTED members: what a member accumulated
     * WHILE ASSOCIATED is folded in here the moment it detaches, so a job's
     * aggregate does not shrink when a member exits (Windows keeps departed
     * usage in the job). Live members are summed on top of these at query time,
     * contributing the same membership-interval delta.
     *
     * MEMBERSHIP INTERVAL, not member lifetime: each member's join-time
     * baseline (task.job_acct_base) is subtracted, so the aggregate answers
     * "what was done in this job" rather than "what its members ever did".
     * Guarded by `lock` -- every read and write is inside the job spinlock. */
    uint64_t   acc_user_ns;
    uint64_t   acc_kernel_ns;
    uint64_t   acc_read_ops;
    uint64_t   acc_write_ops;
    uint64_t   acc_read_bytes;
    uint64_t   acc_write_bytes;
    uint64_t   acc_other_ops;      /* device-control operations */
    uint64_t   acc_other_bytes;    /* device-control bytes */
    /* Stored limits. Only ACTIVE_PROCESS + KILL_ON_JOB_CLOSE are honored;
     * limit_flags never contains an unsupported bit (set path rejects them). */
    uint32_t   limit_flags;
    uint32_t   active_process_limit;       /* valid iff LIMIT_ACTIVE_PROCESS set */
    /* Aggregate resource accounting for the whole job (kernel/quota/quota.h).
     * Every member's chain charge is admitted by this block as well as by the
     * member's own, which is what makes a job limit an aggregate rather than a
     * per-process one. Created with the job and held for its entire life (so a
     * member that pinned the job can read it without further synchronization),
     * released in job_on_delete. NULL only on allocation failure. */
    struct quota_block *quota;
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

/* Fill the job's aggregate per-resource quota report. Reads the job's quota
 * block with the lock-free quota_usage/peak/limit/failures accessors and takes
 * NO job lock: the counters are already atomic, and adding this walk under
 * job->lock would extend exactly the IRQ-off hold the CPU-accounting section
 * already flagged as too long. A row can therefore straddle a concurrent
 * charge, which is correct for a diagnostic report and is why enforcement
 * still happens inside the charge path, never off this snapshot.
 *
 * A job with no quota block (pre-quota boot) reports every row as zero. */
void ob_job_collect_quota_limits(JOB_OBJECT *job,
                                 JOBOBJECT_QUOTA_LIMIT_INFORMATION *out);

/* Apply the aggregate resource limits in `in` to the job's quota block, for
 * rows [0, in->ResourceCount). Only the `Limit` column is an input; Usage,
 * Peak and Failures are kernel-owned and MUST be supplied as zero (fail-closed:
 * accepting them would suggest a caller can write measured state).
 *
 * ALL-OR-NOTHING, for the same reason the per-process transaction is: the whole
 * request is validated and authorized first, and if a commit still fails every
 * already-written row is restored. The whole transaction is serialized against
 * other setters, so a raise verdict cannot be authorized against one pre-image
 * and committed over another.
 *
 * EVERY write -- raise or lower -- requires `caller_privileged`. Unlike the
 * per-process class, whose principal is provably the calling process itself, a
 * job handle carries no granted-access mask yet, so any opener of a shared
 * named job could otherwise tighten every member's limits. The unprivileged-
 * lowering rule returns once job handles enforce rights.
 * -> XREF: TODO-05-object-manager.md section 3.
 *
 * Returns STATUS_INVALID_PARAMETER (bad count/limit/non-zero reserved column),
 * STATUS_PRIVILEGE_NOT_HELD, or STATUS_INSUFFICIENT_RESOURCES when the job has
 * no block. */
NTSTATUS ob_job_set_quota_limits(JOB_OBJECT *job,
                                 const JOBOBJECT_QUOTA_LIMIT_INFORMATION *in,
                                 int caller_privileged);

/* --- Membership lifecycle (called from the scheduler) -------------------- */

/* Detach a dying/terminating task from its job, if any. Idempotent and
 * single-shot per (task,job): clears task->job, removes the pid, drops the
 * membership Ob reference. Called from EVERY process-death path (task_exit,
 * NtTerminateProcess remote kill, NtTerminateJobObject). Takes the job lock
 * internally; releases it before ObDereferenceObject. Safe to call on a task
 * with no job. */
void ob_job_detach_task(struct task *t);

/* Count of detaches that found `job` set but no matching member entry. Non-zero
 * means the membership bookkeeping has drifted. Counted (not just logged)
 * because detach runs from the log-free process-death teardown path, where the
 * message itself is suppressed above PASSIVE_LEVEL. */
uint64_t ob_job_detach_mismatch_count(void);

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
