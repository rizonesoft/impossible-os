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

/* How many times ob_job_collect_accounting re-attempts its mostly-unlocked walk
 * before falling back to the fully-locked one. What invalidates an attempt is
 * CHANGE, never lock contention, so this bounds reshaping rather than waiting --
 * and there are TWO causes, not one: this job's membership moving (`member_gen`)
 * and any task in the system being published (`task_count()`, needed because
 * publication bumps no generation; see the collector). Four consecutive
 * invalidations mean the state is moving faster than it is being read, and the
 * locked walk is then both cheaper and guaranteed to terminate. Never 0 -- that
 * would make the unlocked path dead code and hand every query the long
 * IRQ-off hold this whole mechanism exists to remove. */
#define JOB_COLLECT_MAX_RETRIES   4U
_Static_assert(JOB_COLLECT_MAX_RETRIES >= 1U,
    "a zero retry bound would skip the unlocked walk entirely");

/* Members whose join-time baselines are copied per lock acquisition. The baseline
 * MUST be copied under job->lock (plain multi-word memory), but a full
 * JOB_MAX_MEMBERS array of them is 2560 bytes against an 8 KiB kernel task stack,
 * so the collector copies them in batches: bounded stack, and each IRQ-off hold
 * bounded to a constant instead of scaling with member count. */
#define JOB_COLLECT_BATCH   8U
_Static_assert(JOB_COLLECT_BATCH >= 1U && JOB_COLLECT_BATCH <= JOB_MAX_MEMBERS,
    "the baseline batch must be non-empty and no larger than the member cap");

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
    /* Membership GENERATION, bumped under `lock` by every event that changes
     * what a membership snapshot MEANS: a join (ob_job_assign, which every
     * fork-inherit routes through), a departure (ob_job_detach_task, in the same
     * critical section that folds the departed usage and removes the pid), and a
     * job-wide terminate (which forecloses joins and reclassifies every later
     * departure as a termination). NOT bumped by a limit write and not by any
     * accounting read. It is not the collector's only invalidation cause, though:
     * task publication moves no generation, so the collector ALSO re-checks
     * task_count(), and an attempt can be invalidated with this job untouched.
     *
     * This is what lets ob_job_collect_accounting move the expensive half of its
     * walk out of the IRQ-off critical section: it snapshots the pid list AND
     * the join-time baselines under the lock, reads the live per-member counters
     * with the lock RELEASED, then re-reads this counter and retries if it moved.
     * The baselines cannot move out with the live reads -- task.job_acct_base is
     * a plain multi-word struct written under this lock, so reading it unlocked
     * would be a genuine data race that a generation re-check cannot legalize
     * (it discards a torn VALUE; it does not make the access defined).
     *
     * Guarded by `lock` -- every read and write is inside it, which is also why
     * the two reads need no explicit ordering: the spinlock supplies it. */
    uint64_t   member_gen;
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

/* Fill the job's aggregate CPU/IO accounting. Returns void BY CONTRACT: there is
 * no failure a caller could act on, so the implementation cannot say "this answer
 * is unreliable" and must instead bound how wrong it can be.
 *
 * WHAT IS GUARANTEED, exactly -- and it is narrower than "consistent": the CPU/IO
 * total covers the members that were RESOLVABLE AT THE SNAPSHOT, with no join,
 * departure or terminate applied halfway through it. A member is never counted
 * twice and never half-counted, and a departure never lands between the persistent
 * sums and the live walk.
 *
 * It is NOT a promise that the total covers every member the counts describe.
 * `ActiveProcesses` reports the body's whole member array, and a member that was
 * unresolvable at the snapshot is in that count while contributing nothing to the
 * total -- so the fields are NOT cross-consistent, and a caller must not derive
 * "per-member average" or "every member accounted" from them. The window below is
 * why that member can be a real, running one rather than only a not-yet-started
 * one.
 *
 * FAST PATH: snapshot the pid list, the persistent departed-member sums and the
 * counts under `job->lock` with `member_gen`; then walk the members in batches of
 * JOB_COLLECT_BATCH, taking the lock only to copy that batch's join-time baselines
 * and computing each member's delta with the lock RELEASED (through the same
 * saturating `task_acct_delta_since` the locked walk uses, so a counter reset
 * beneath a live baseline still clamps to zero rather than wrapping); finally
 * re-read `member_gen`. Equal generation means no join, departure or terminate
 * intervened, so every baseline still belongs to the member it was read for and the
 * accumulated delta is publishable on that axis.
 *
 * FALLBACK: after JOB_COLLECT_MAX_RETRIES invalidated attempts (from EITHER cause --
 * this job's membership moving, or any task in the system being published), do the
 * whole walk under the lock. No join, departure or terminate can make this loop or
 * split it -- the price of sustained invalidation is one long hold, and a busy fork
 * workload elsewhere can be what pays it.
 *
 * THE PUBLICATION WINDOW, which is what keeps the guarantee above narrow: a task
 * becomes resolvable via
 * `num_tasks`, which task creation increments OUTSIDE `job->lock`, while
 * fork-inheritance puts the child in `member_pids[]` beforehand. So a child can be
 * compacted out of a walk and then start running inside it. The fast path samples
 * `task_count()` across the window, which NARROWS this and detects the common
 * cases, but the sample carries no happens-before against the publisher and the
 * fallback does not sample at all. Consequence, bounded: such a child is counted in
 * `ActiveProcesses` while the microseconds of usage it accrued during that walk are
 * not; the next query is right. Do NOT build a caller that needs this closed --
 * closing it means making publication generation-visible in the scheduler, which
 * the accounting-quotas TODO tracks against the SMP phase-2 work.
 *
 * Never holds the lock across a user-memory access: the syscall layer copies this
 * kernel-local result out after the call returns. */
void ob_job_collect_accounting(JOB_OBJECT *job,
                               JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *acct,
                               IO_COUNTERS *io);

/* Current membership generation (takes `lock` internally). Diagnostic + test
 * surface: the invariant worth asserting is that it moves on a join and on a
 * departure and stands still across an accounting read. */
uint64_t ob_job_member_gen(JOB_OBJECT *job);

/* How many collector attempts were invalidated, and how many collections gave up
 * and took the locked walk. Counted rather than logged: the collector runs from a
 * syscall path where a per-event message would be a log flood, and a non-zero
 * fallback count is the signal that JOB_COLLECT_MAX_RETRIES is mistuned for a real
 * workload. Both are RELAXED atomics -- diagnostics, not control.
 *
 * NOT attributable to this job on their own. An attempt is invalidated by either
 * cause (see JOB_COLLECT_MAX_RETRIES), and one of them is GLOBAL: any task creation
 * anywhere bumps `task_count()`, so a busy fork workload can drive these counters --
 * and even the long locked fallback -- on a job whose own membership never moved.
 * Reading a rise here as "this job is being reshaped" is therefore wrong; splitting
 * the counters by cause is what a real tuning question would need. */
uint64_t ob_job_collect_retry_count(void);
uint64_t ob_job_collect_fallback_count(void);

#ifdef KERNEL_TESTS
/* Inject `attempts` membership-churn events into the collector: each collect
 * attempt consumes one and bumps `member_gen` after taking its snapshot, which is
 * precisely what a concurrent join or departure does to an in-flight walk. This is
 * the only way to prove the retry path and the bounded fallback on a harness that
 * runs the collector on ONE CPU; a genuine cross-CPU race needs the per-CPU run
 * queues owned by the SMP phase-2 roadmap, with the dependent two-CPU regression
 * owned by the accounting-quotas test section. Pass 0 to disable. Compiled out of
 * production builds entirely. */
void ob_job_test_inject_gen_churn(uint32_t attempts);
#endif
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
