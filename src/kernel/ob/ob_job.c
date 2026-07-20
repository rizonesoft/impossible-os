/* ============================================================================
 * ob_job.c -- Job Object type: process-group container + accounting + limits
 *
 * A JOB_OBJECT holds a dense array of member pids under one per-job spinlock.
 * Each membership owns ONE Ob reference on the job body (taken at assign /
 * fork-inherit, dropped when the membership ends), so the body outlives every
 * member and a raw task->job pointer can never dangle.
 *
 * Reference-ownership invariant (the single-shot rule that makes detach and
 * terminate race-safe): a task with `task->job == J` under J's lock owns
 * exactly one reference on J. Whoever clears `task->job` under the lock also
 * owns the matching ObDereferenceObject. task_exit / NtTerminateProcess /
 * SYS_KILL / signal-kill each clear-and-drop via ob_job_detach_task();
 * NtTerminateJobObject / KILL_ON_JOB_CLOSE clear-and-drop in bulk. A task dies
 * on exactly one path, and each path checks task->job under the lock, so the
 * reference is dropped exactly once with no double-free and no leak.
 *
 * Locking discipline: the job spinlock guards ONLY the member array + counts +
 * flags. Object-manager calls (ObReferenceObject is a bare atomic and is taken
 * under the lock; ObDereferenceObject may free and is ALWAYS done after the
 * lock is released) and the remote-kill transition (task_terminate_remote) run
 * OUTSIDE the lock, on a snapshot taken under it. Kill-on-close and terminate
 * therefore never hold the lock across a process-death transition or a
 * potentially-freeing dereference.
 * ============================================================================ */

#include "kernel/ob/ob_job.h"
#include "kernel/quota/quota.h"   /* per-job aggregate accounting block */
#include "kernel/sched/irql.h"    /* KeGetCurrentIrql for the detach diag gate */
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/ob/ob_type.h"
#include "kernel/ob/handle_table.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/klog.h"

extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* The single registered Job Object type. */
const OBJECT_TYPE *ObpJobType = NULL;

/* --- Internal: bulk kill of every member (terminate / kill-on-close) -------
 * Marks the job terminated and snapshots the live pids under the lock, then --
 * with the lock RELEASED -- routes each member through task_terminate_remote,
 * which runs that member's OWN ob_job_detach_task. Crucially, this path does
 * NOT clear task->job or drop any membership reference itself: each member's
 * reference is dropped by its own detach, so the reference invariant "a task's
 * membership reference keeps the job (and its lock) alive until that task's
 * detach completes" holds for every path. That is what prevents the
 * lock-a-freed-job use-after-free (a bulk kill dropping another task's ref out
 * from under that task's in-flight detach). Idempotent via job->terminated.
 * ----------------------------------------------------------------------- */
static void job_kill_all_members(JOB_OBJECT *job, int32_t exit_code)
{
    uint32_t snapshot[JOB_MAX_MEMBERS];
    uint32_t n = 0;
    uint64_t flags;
    uint32_t i;

    spin_lock_irqsave(&job->lock, &flags);
    if (job->terminated) {                 /* already terminated: idempotent */
        spin_unlock_irqrestore(&job->lock, flags);
        return;
    }
    job->terminated = 1;                   /* no further joins; detach counts as termination */
    n = job->num_members;
    for (i = 0; i < n; i++)
        snapshot[i] = job->member_pids[i];
    spin_unlock_irqrestore(&job->lock, flags);

    /* Lock released. Each member's task_terminate_remote -> ob_job_detach_task
     * removes it and drops ITS OWN reference; a member that already self-exited
     * is skipped by the TASK_DEAD early-out (its detach already ran). */
    for (i = 0; i < n; i++) {
        struct task *t = task_get_by_pid(snapshot[i]);
        if (t)
            task_terminate_remote(t, exit_code);
    }
}

/* --- Callbacks ----------------------------------------------------------- */

/* on_close fires when the LAST handle to the job is closed (handle_count -> 0).
 * Member references still keep the body alive, so this is the correct place for
 * KILL_ON_JOB_CLOSE (on_delete would never run while members hold references).
 * The closing handle's own reference is dropped by the caller AFTER this
 * returns, so the body is guaranteed live for the duration. */
static void job_on_close(void *body, uint32_t remaining_handles)
{
    JOB_OBJECT *job = (JOB_OBJECT *)body;
    uint64_t flags;
    uint32_t kill;

    if (remaining_handles != 0)
        return;

    spin_lock_irqsave(&job->lock, &flags);
    kill = (job->limit_flags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) ? 1 : 0;
    spin_unlock_irqrestore(&job->lock, flags);

    if (kill)
        job_kill_all_members(job, 1);   /* exit code 1, matching Windows */
}

/* on_delete fires at ref_count -> 0, i.e. no handles AND no members remain.
 * By the reference invariant every member drops its ref on detach, so an
 * empty member array is expected here; a non-empty one is a refcount bug. */
static void job_on_delete(void *body)
{
    JOB_OBJECT *job = (JOB_OBJECT *)body;

    if (job->num_members != 0)
        klog(LOG_ERROR, "ob",
             "job_on_delete: %u members still attached at free (refcount bug)",
             (uint64_t)job->num_members);

    /* Release the aggregate block. A member charging through the job pins the
     * job body first, so no charge can be in flight here. */
    quota_block_deref(job->quota);
    job->quota = (struct quota_block *)0;
}

/* --- Type registration --------------------------------------------------- */

void ob_job_type_init(void)
{
    ObpJobType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Job",
        .body_size = sizeof(JOB_OBJECT),
        .on_close  = job_on_close,
        .on_delete = job_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpJobType)
        klog(LOG_ERROR, "ob", "Failed to register ObpJobType");
}

/* Detach-time membership inconsistencies, counted so the anomaly survives even
 * when the message is suppressed above PASSIVE_LEVEL. Same discipline as the
 * quota subsystem's deferred diagnostics. */
static uint64_t g_job_detach_mismatch;

uint64_t ob_job_detach_mismatch_count(void)
{
    return __atomic_load_n(&g_job_detach_mismatch, __ATOMIC_RELAXED);
}

/* --- Membership: detach (every process-death path) ----------------------- */

void ob_job_detach_task(struct task *t)
{
    JOB_OBJECT *job;
    uint64_t tflags, jflags;
    uint32_t i;
    int found = 0;
    quota_absorb_record_t absorbed;
    struct task_acct_base member_delta;

    if (!t)
        return;

    /* Serialize the whole membership read + mutation under the per-task job_lock
     * (lock order job_lock -> job->lock). While we hold job_lock no assignment
     * can reassign or clear t->job, and this task's own membership reference
     * keeps the job body alive, so the raw pointer we lock below cannot be freed
     * out from under us. The reference is dropped only after t->job is cleared,
     * AFTER releasing both locks. */
    spin_lock_irqsave(&t->job_lock, &tflags);
    job = t->job;
    if (!job) {
        spin_unlock_irqrestore(&t->job_lock, tflags);
        return;
    }
    /* Temporary pin, independent of the membership reference: the quota
     * withdrawal below runs after t->job is cleared and the membership
     * reference is dropped, so without this the body could be freed first. */
    ObReferenceObject(job);
    /* Claim the absorb record here, under the same lock that published it: a
     * racing detach for this task finds it already empty and cannot withdraw
     * the same amount twice. */
    absorbed = t->job_absorb;
    t->job_absorb.active = 0;

    spin_lock_irqsave(&job->lock, &jflags);
    /* Fold what this member accumulated WHILE ASSOCIATED into the job's
     * persistent accumulators BEFORE removing it, so a job's aggregate usage
     * does not vanish when a member exits (Windows retains departed-member
     * usage).
     *
     * The MEMBERSHIP INTERVAL, not the member's lifetime: the join-time
     * baseline is subtracted, so a process that ran for an hour and was then
     * assigned to a job hands the job only what it spent as a member. Folding
     * raw totals here (as this did before CPU/I/O accounting was specified)
     * credited the job retroactively for work done before it existed, and made
     * the aggregate disagree with the same member's live contribution below. */
    task_acct_delta_since(t, &t->job_acct_base, &member_delta);
    job->acc_user_ns     += member_delta.user_time_ns;
    job->acc_kernel_ns   += member_delta.kernel_time_ns;
    job->acc_read_ops    += member_delta.io_read_count;
    job->acc_write_ops   += member_delta.io_write_count;
    job->acc_read_bytes  += member_delta.io_read_bytes;
    job->acc_write_bytes += member_delta.io_write_bytes;
    job->acc_other_ops   += member_delta.io_other_count;
    job->acc_other_bytes += member_delta.io_other_bytes;
    /* A member leaving a TERMINATED job counts as a termination; a member
     * exiting a live job does not (Windows: TotalTerminatedProcesses tracks
     * limit/TerminateJobObject kills, not normal exits). */
    if (job->terminated)
        job->total_terminated++;
    /* Swap-remove pid from the dense array. */
    for (i = 0; i < job->num_members; i++) {
        if (job->member_pids[i] == t->pid) {
            job->member_pids[i] = job->member_pids[job->num_members - 1];
            job->num_members--;
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&job->lock, jflags);
    t->job = NULL;
    spin_unlock_irqrestore(&t->job_lock, tflags);

    /* Drop the membership reference UNCONDITIONALLY. ob_job_assign takes it in
     * the same critical section that sets t->job, so "t->job was set" (checked
     * above) means the reference is held -- whether or not the pid is still in
     * the member array. Releasing it only on the found path leaked it on the
     * mismatch path, and an unreachable reference keeps the Job Object and its
     * quota block alive forever. */
    ObDereferenceObject(job);
    if (!found) {
        /* Membership inconsistency (job set, but not in the member array).
         * IRQL-GATED rather than logged unconditionally: this runs from
         * task_death_teardown, which is log-free by contract and reachable at
         * elevated IRQL, where klog's live-disk flush re-enters the VFS and can
         * stall or deadlock a dying process. The counter is always bumped, so
         * the anomaly is never lost even when the message is suppressed. */
        __atomic_fetch_add(&g_job_detach_mismatch, 1ull, __ATOMIC_RELAXED);
        if (KeGetCurrentIrql() == PASSIVE_LEVEL)
            klog(LOG_ERROR, "ob",
                 "ob_job_detach_task: PID %u had job set but was not a member",
                 (uint64_t)t->pid);
    }

    /* Withdraw ONLY what joining folded in -- the other half of the absorb in
     * ob_job_assign, without which the job's usage could only ever grow. Runs
     * with NO lock held (quota.h forbids charging under job_lock) and exactly
     * once (the record was claimed under job_lock above). The member's own
     * post-join charges are NOT withdrawn here: they reached the job through
     * chain receipts and are returned by those receipts, so returning them
     * again would subtract from whichever member's usage covers the difference.
     */
    quota_job_unabsorb(job->quota, &absorbed);

    ObDereferenceObject(job);       /* drop the temporary pin */
}

/* --- Membership: assign (join a live process) ----------------------------
 * Under the job lock: reject if terminated, if the active-process limit would
 * be exceeded, if the array is full, or if the task is already in a job.
 * Takes the membership reference under the lock (bare atomic). Returns an
 * NTSTATUS so the syscall layer can distinguish the failure modes.
 * ----------------------------------------------------------------------- */
NTSTATUS ob_job_assign(JOB_OBJECT *job, struct task *t)
{
    uint64_t tflags, jflags;

    if (!job || !t)
        return STATUS_INVALID_PARAMETER;

    /* Serialize with the task's own detach under the per-task job_lock (lock
     * order job_lock -> job->lock). Liveness is authoritative here: task_exit
     * sets TASK_DEAD before its detach (which also takes job_lock), so either we
     * observe TASK_DEAD and reject, or the dying task's later detach observes the
     * membership we set here and removes it -- a dead task can never remain a
     * permanent member, and no fence/rollback is needed. Holding job_lock across
     * the reference take + t->job write also means no assignment ever frees the
     * job out from under a concurrent detacher. */
    /* Fold the joiner's outstanding usage into the job BEFORE taking either
     * lock (charging under job_lock would violate quota.h's no-nested-call
     * contract). A process that allocated first and joined a capped job
     * afterwards would otherwise carry that usage past the cap uncounted.
     * Refuse the assignment outright when the job cannot absorb it; every
     * rejection BELOW must undo the absorb, or a refused assignment would
     * leave the joiner's usage permanently inflating the job. */
    quota_absorb_record_t absorbed = { .taken = { 0 }, .active = 0 };
    if (job->quota) {
        /* Settle membership FIRST. Absorbing before this check would fold an
         * existing member's usage in a SECOND time on the idempotent same-job
         * re-assign -- double-counting it, and turning that documented success
         * into STATUS_QUOTA_EXCEEDED whenever the job lacks headroom for the
         * duplicate. The authoritative re-check under the locks below still
         * decides the outcome; this pre-check only avoids a pointless absorb. */
        NTSTATUS pre       = STATUS_SUCCESS;
        int      may_join  = 0;
        spin_lock_irqsave(&t->job_lock, &tflags);
        if (t->state == TASK_DEAD)
            pre = STATUS_PROCESS_IS_TERMINATING;
        else if (t->job == job)
            pre = STATUS_SUCCESS;          /* already a member: nothing to fold */
        else if (t->job != NULL)
            pre = STATUS_ACCESS_DENIED;
        else
            may_join = 1;                  /* not a member yet: absorb below */
        spin_unlock_irqrestore(&t->job_lock, tflags);

        if (!may_join)
            return pre;

        NTSTATUS qst = quota_job_absorb_task(job->quota, t, &absorbed);
        if (qst != STATUS_SUCCESS)
            return qst;
    }

    spin_lock_irqsave(&t->job_lock, &tflags);

    if (t->state == TASK_DEAD) {
        spin_unlock_irqrestore(&t->job_lock, tflags);
        quota_job_unabsorb(job->quota, &absorbed);
        return STATUS_PROCESS_IS_TERMINATING;
    }
    if (t->job != NULL) {
        /* Already in a job. Idempotent success if it is THIS job; otherwise
         * reject (nested jobs are not supported -- pre-Win8 semantics). */
        NTSTATUS r = (t->job == job) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
        spin_unlock_irqrestore(&t->job_lock, tflags);
        /* Undo in BOTH cases: on rejection nothing joined, and on the
         * idempotent re-assign the usage is already folded in from the first
         * assignment, so keeping this second fold would double-count it. */
        quota_job_unabsorb(job->quota, &absorbed);
        return r;
    }

    spin_lock_irqsave(&job->lock, &jflags);
    if (job->terminated) {
        spin_unlock_irqrestore(&job->lock, jflags);
        spin_unlock_irqrestore(&t->job_lock, tflags);
        quota_job_unabsorb(job->quota, &absorbed);
        return STATUS_INVALID_PARAMETER;   /* job is dead: no new members */
    }
    if ((job->limit_flags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) &&
        job->num_members >= job->active_process_limit) {
        spin_unlock_irqrestore(&job->lock, jflags);
        spin_unlock_irqrestore(&t->job_lock, tflags);
        quota_job_unabsorb(job->quota, &absorbed);
        return STATUS_QUOTA_EXCEEDED;
    }
    if (job->num_members >= JOB_MAX_MEMBERS) {
        spin_unlock_irqrestore(&job->lock, jflags);
        spin_unlock_irqrestore(&t->job_lock, tflags);
        quota_job_unabsorb(job->quota, &absorbed);
        return STATUS_QUOTA_EXCEEDED;
    }

    ObReferenceObject(job);                       /* membership reference */
    /* Capture the CPU/I/O baseline in the SAME critical section that publishes
     * the membership, so a collector walking member_pids[] under this lock can
     * never see a member whose baseline is still unset (which would credit the
     * job with that member's entire prior lifetime). */
    task_acct_capture_base(t, &t->job_acct_base);
    job->member_pids[job->num_members++] = t->pid;
    job->total_processes++;
    /* Publish the absorb record WITH the membership, under the same lock that
     * the detach consumes it under: only this recorded amount is owned by the
     * membership, and exactly one detacher may withdraw it. */
    t->job_absorb = absorbed;
    t->job = job;
    spin_unlock_irqrestore(&job->lock, jflags);
    spin_unlock_irqrestore(&t->job_lock, tflags);

    return STATUS_SUCCESS;
}

/* --- Membership: fork inheritance (before child publication) ------------- */

int ob_job_fork_inherit(struct task *child, struct task *parent)
{
    JOB_OBJECT *job;
    uint64_t pflags;
    NTSTATUS st;

    if (!child)
        return -1;

    /* The child slot may carry a stale pointer from a prior tenant; the fork
     * path zeroes most fields, but make membership explicit regardless. */
    child->job = NULL;

    if (!parent)
        return 0;

    /* Read the parent's job UNDER parent->job_lock and pin it with a temporary
     * reference before releasing the lock -- otherwise a concurrent detach of
     * the parent could clear the pointer and drop the last membership reference,
     * freeing the body before ob_job_assign locks it. The temp reference keeps
     * the body alive across the assign; it is dropped afterward on every path. */
    spin_lock_irqsave(&parent->job_lock, &pflags);
    job = parent->job;
    if (job)
        ObReferenceObject(job);
    spin_unlock_irqrestore(&parent->job_lock, pflags);

    if (!job)
        return 0;                                 /* parent has no job */

    st = ob_job_assign(job, child);               /* fail-closed on terminate/limit/full */
    ObDereferenceObject(job);                     /* drop the temporary pin */
    return (st == STATUS_SUCCESS) ? 0 : -1;
}

/* --- Terminate (NtTerminateJobObject) ------------------------------------ */

NTSTATUS ob_job_terminate(JOB_OBJECT *job, int32_t exit_code)
{
    if (!job)
        return STATUS_INVALID_PARAMETER;
    job_kill_all_members(job, exit_code);
    return STATUS_SUCCESS;
}

/* --- Membership query (NtIsProcessInJob) --------------------------------- */

int ob_job_is_member(JOB_OBJECT *job, uint32_t pid)
{
    uint64_t flags;
    uint32_t i;
    int is_member = 0;

    if (!job)
        return 0;

    spin_lock_irqsave(&job->lock, &flags);
    for (i = 0; i < job->num_members; i++) {
        if (job->member_pids[i] == pid) {
            is_member = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&job->lock, flags);
    return is_member;
}

/* --- Accounting collection (into a caller-owned kernel-local struct) ------
 * Sums per-member CPU time (ns -> 100ns units) and I/O counters under the lock,
 * plus the process counts. The lock is NEVER held across a user-memory access:
 * the syscall layer copies this kernel-local result to the (validated) user
 * buffer after this returns.
 * ----------------------------------------------------------------------- */
void ob_job_collect_accounting(JOB_OBJECT *job,
                               JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *acct,
                               IO_COUNTERS *io)
{
    uint64_t flags;
    uint32_t i;
    uint64_t user_ns, kernel_ns;
    uint64_t rd_ops, wr_ops, rd_bytes, wr_bytes;
    uint64_t oth_ops, oth_bytes;
    struct task_acct_base live;

    /* Zero first so partial fills never leak stack. */
    for (i = 0; i < sizeof(*acct); i++)
        ((uint8_t *)acct)[i] = 0;
    if (io)
        for (i = 0; i < sizeof(*io); i++)
            ((uint8_t *)io)[i] = 0;

    spin_lock_irqsave(&job->lock, &flags);
    /* Departed-member usage (persistent) + live-member usage (summed now). */
    user_ns   = job->acc_user_ns;
    kernel_ns = job->acc_kernel_ns;
    rd_ops    = job->acc_read_ops;
    wr_ops    = job->acc_write_ops;
    rd_bytes  = job->acc_read_bytes;
    wr_bytes  = job->acc_write_bytes;
    oth_ops   = job->acc_other_ops;
    oth_bytes = job->acc_other_bytes;
    for (i = 0; i < job->num_members; i++) {
        struct task *t = task_get_by_pid(job->member_pids[i]);
        if (!t)
            continue;
        /* A live member contributes exactly what a departing one folds in: its
         * usage since it joined. Both halves of the membership lifetime use the
         * same baseline, so a member's contribution does not jump when it exits.
         * The baseline was published under THIS lock alongside the pid, so it is
         * necessarily set for any member reachable from member_pids[]. */
        task_acct_delta_since(t, &t->job_acct_base, &live);
        user_ns   += live.user_time_ns;
        kernel_ns += live.kernel_time_ns;
        rd_ops    += live.io_read_count;
        wr_ops    += live.io_write_count;
        rd_bytes  += live.io_read_bytes;
        wr_bytes  += live.io_write_bytes;
        oth_ops   += live.io_other_count;
        oth_bytes += live.io_other_bytes;
    }
    acct->TotalProcesses          = job->total_processes;   /* ever associated (monotonic) */
    acct->ActiveProcesses         = job->num_members;       /* currently live */
    acct->TotalTerminatedProcesses = job->total_terminated;
    spin_unlock_irqrestore(&job->lock, flags);

    /* 100ns units (Windows LARGE_INTEGER convention). */
    acct->TotalUserTime            = (int64_t)(user_ns / 100);
    acct->TotalKernelTime          = (int64_t)(kernel_ns / 100);
    acct->ThisPeriodTotalUserTime  = acct->TotalUserTime;
    acct->ThisPeriodTotalKernelTime = acct->TotalKernelTime;

    if (io) {
        io->ReadOperationCount  = rd_ops;
        io->WriteOperationCount = wr_ops;
        io->ReadTransferCount   = rd_bytes;
        io->WriteTransferCount  = wr_bytes;
        /* Control ("Other") I/O completes the Windows IO_COUNTERS ABI, whose
         * Other* fields were previously left zeroed. They report the device-
         * control traffic counted by task_acct_note_control_io(). */
        io->OtherOperationCount = oth_ops;
        io->OtherTransferCount  = oth_bytes;
    }
}

/* --- Limit read (into kernel-local BasicLimitInformation) ---------------- */

void ob_job_collect_limits(JOB_OBJECT *job, JOBOBJECT_BASIC_LIMIT_INFORMATION *lim)
{
    uint64_t flags;
    uint32_t i;

    for (i = 0; i < sizeof(*lim); i++)
        ((uint8_t *)lim)[i] = 0;

    spin_lock_irqsave(&job->lock, &flags);
    lim->LimitFlags         = job->limit_flags;
    lim->ActiveProcessLimit = job->active_process_limit;
    spin_unlock_irqrestore(&job->lock, flags);
}

/* --- Limit set (NtSetInformationJobObject BasicLimitInformation) ----------
 * Rejects any limit flag this kernel cannot enforce (STATUS_NOT_SUPPORTED,
 * state unchanged) so a caller can never configure a silent no-op limit. Only
 * the honored flags are stored.
 * ----------------------------------------------------------------------- */
NTSTATUS ob_job_set_basic_limits(JOB_OBJECT *job,
                                 const JOBOBJECT_BASIC_LIMIT_INFORMATION *lim)
{
    uint64_t flags;

    if (!job || !lim)
        return STATUS_INVALID_PARAMETER;

    if (lim->LimitFlags & ~JOB_SUPPORTED_LIMIT_FLAGS)
        return STATUS_NOT_SUPPORTED;   /* an unenforceable limit was requested */

    if ((lim->LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) &&
        lim->ActiveProcessLimit == 0)
        return STATUS_INVALID_PARAMETER;

    spin_lock_irqsave(&job->lock, &flags);
    job->limit_flags = lim->LimitFlags;
    job->active_process_limit =
        (lim->LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS)
        ? lim->ActiveProcessLimit : 0;
    spin_unlock_irqrestore(&job->lock, flags);

    return STATUS_SUCCESS;
}

/* --- Process-id-list snapshot (NtQueryInformationJobObject) ---------------
 * Copies up to `capacity` member pids into `out`, returns the number written in
 * *written and the total assigned in *assigned. Lock held only across the array
 * copy (out is a kernel-local buffer sized by the caller).
 * ----------------------------------------------------------------------- */
void ob_job_collect_pid_list(JOB_OBJECT *job, uint64_t *out, uint32_t capacity,
                             uint32_t *written, uint32_t *assigned)
{
    uint64_t flags;
    uint32_t i, w = 0;

    spin_lock_irqsave(&job->lock, &flags);
    for (i = 0; i < job->num_members && w < capacity; i++)
        out[w++] = (uint64_t)job->member_pids[i];
    if (assigned)
        *assigned = job->num_members;
    spin_unlock_irqrestore(&job->lock, flags);

    if (written)
        *written = w;
}

/* --- Create / open (handle-returning; mirrors NtCreateSemaphore) ----------
 * A NULL name creates an anonymous job. A non-NULL name opens the existing
 * named job if present, else creates it under \BaseNamedObjects with a
 * name-collision redirect so one name maps to one job.
 * ----------------------------------------------------------------------- */
HANDLE ob_job_create(HANDLE_TABLE *ht, const char *name)
{
    JOB_OBJECT *job;
    HANDLE h;

    if (!ht)
        return INVALID_HANDLE_VALUE;

    if (name) {
        void *existing = NULL;
        char path[128];
        snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);
        if (ObLookupObjectByName(path, ObpJobType, 0, &existing) == 0 && existing) {
            h = ObpAllocateHandle(ht, existing, 0, 0);
            ObDereferenceObject(existing);
            return h;
        }
    }

    job = (JOB_OBJECT *)ob_alloc_object(ObpJobType);
    if (!job)
        return INVALID_HANDLE_VALUE;
    /* Body is zero-filled by ob_alloc_object: lock unlocked, no members, no
     * limits, not terminated -- no explicit field init required. */

    /* Aggregate accounting block, created with the job so every member finds
     * it already present. Deliberately OWNERLESS: a job's creator is not its
     * members (a job can hold processes belonging to other users), so tagging
     * the block with the creator's SID would make it look like that user's
     * usage in a per-SID aggregate while describing someone else's. The
     * per-user rollup reads USER blocks only for exactly this reason. */
    job->quota = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);
    if (!job->quota) {
        klog(LOG_ERROR, "ob", "ob_job_create: quota block allocation failed");
        ObDereferenceObject(job);
        return INVALID_HANDLE_VALUE;
    }

    if (name) {
        void *bno_dir = NULL;
        if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0,
                                 &bno_dir) == 0 && bno_dir) {
            if (ObInsertObject(job, name, bno_dir) < 0) {
                void *winner = NULL;
                char wpath[128];
                ObDereferenceObject(bno_dir);
                snprintf(wpath, sizeof(wpath), "\\BaseNamedObjects\\%s", name);
                if (ObLookupObjectByName(wpath, ObpJobType, 0, &winner) == 0
                    && winner) {
                    h = ObpAllocateHandle(ht, winner, 0, 0);
                    ObDereferenceObject(winner);
                    ObDereferenceObject(job);
                    return h;
                }
                ObDereferenceObject(job);
                return INVALID_HANDLE_VALUE;
            }
            ObDereferenceObject(bno_dir);
        }
    }

    h = ObpAllocateHandle(ht, job, 0, 0);
    ObDereferenceObject(job);
    return h;
}

/* Open an existing named job. Returns a REFERENCED body (caller derefs) or
 * NULL if no such job exists. */
void *ob_job_open(const char *name)
{
    void *body = NULL;
    char path[128];

    if (!name)
        return NULL;
    if (name[0] == '\\')
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);

    if (ObLookupObjectByName(path, ObpJobType, 0, &body) != 0)
        return NULL;
    return body;
}
