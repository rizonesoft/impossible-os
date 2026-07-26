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
#include "kernel/quota/quota.h"         /* per-job aggregate accounting block */
#include "kernel/quota/quota_ledger.h"  /* charge gate + obligation migration */
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

/* Invalidate every in-flight accounting snapshot. MUST be called with job->lock
 * held, in the SAME critical section as the mutation it describes -- bumping
 * separately would open a window in which a collector sees the new membership
 * with the old generation and publishes a sum that mixes both. A plain increment
 * is correct because the spinlock supplies the ordering for the collector's two
 * reads, which are taken under the same lock. See member_gen in ob_job.h. */
static void job_member_gen_bump(JOB_OBJECT *job)
{
    job->member_gen++;
}

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
    /* A terminate is a membership event, not an accounting one: it forecloses
     * joins and reclassifies every later departure as a termination, so a
     * collector that straddles it would report counts from two different
     * regimes. */
    job_member_gen_bump(job);
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

    /* Release the aggregate block. CLEAR the pointer under the lock FIRST and
     * deref outside it: job_quota_ref() try-refs job->quota under this same
     * lock, so a deref-then-clear would let that helper CAS on memory this
     * call already freed. Same ordering as quota_task_teardown's owner path. */
    {
        struct quota_block *doomed;
        uint64_t flags;

        spin_lock_irqsave(&job->lock, &flags);
        doomed = job->quota;
        job->quota = (struct quota_block *)0;
        spin_unlock_irqrestore(&job->lock, flags);

        quota_block_deref(doomed);
    }
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
    /* One bump for the whole departure, in the critical section that performed
     * it. UNCONDITIONAL, including the !found mismatch path: the fold above
     * already moved acc_*, so a collector's persistent-sum snapshot is stale
     * whether or not the pid was still in the array. */
    job_member_gen_bump(job);
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
/* Undo everything the pre-lock quota preparation did, in reverse order, and end
 * the quiesce. Called on EVERY path that refuses the assignment after the absorb
 * has run -- five of them, which is why it is a helper and not five copies.
 *
 * Order matters and is the reverse of preparation: the obligations that adopted
 * part of the absorb record must give it back BEFORE the record is withdrawn,
 * or unabsorb would return only the un-adopted remainder and leave the job
 * billed for a process that never joined. The gate reopens last, so no charger
 * can observe a half-unwound state.
 *
 * A job with no quota block was never prepared and never quiesced, so there is
 * nothing to unwind and -- critically -- no close to reopen. */
static void ob_job_assign_unwind(JOB_OBJECT *job, struct task *t,
                                 quota_absorb_record_t *absorbed)
{
    if (!job->quota)
        return;
    (void)quota_ledger_unmigrate_from_job(t, job->quota, absorbed);
    quota_job_unabsorb(job->quota, absorbed);
    quota_gate_reopen(t);
}

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

        /* QUIESCE the joiner's chargers for the whole absorb-to-publish window.
         * Without this the window is real: a charge landing between the absorb
         * and the membership publication misses the job entirely, and the older
         * idea of revalidating a membership generation AFTER charging makes it
         * worse rather than better -- the absorb has already folded that charge
         * in, so the retry bills the job a second time. Draining first is the
         * only ordering that is right in both directions.
         *
         * A refusal here is not fatal to correctness, only to this attempt: the
         * assignment is refused and nothing has been folded in yet. */
        NTSTATUS qz = quota_gate_quiesce(t);
        if (qz != STATUS_SUCCESS)
            return qz;

        NTSTATUS qst = quota_job_absorb_task(job->quota, t, &absorbed);
        if (qst != STATUS_SUCCESS) {
            quota_gate_reopen(t);
            return qst;
        }

        /* ADOPT the joiner's outstanding obligations into the job rather than
         * charging it twice: the absorb above already billed the job for the
         * joiner's whole current usage, which includes what these obligations
         * hold, so the migration moves only WHO must give it back. Runs inside
         * the quiesce so no charger can add an obligation while the set is being
         * walked. */
        (void)quota_ledger_migrate_to_job(t, job->quota, &absorbed);
    }

    spin_lock_irqsave(&t->job_lock, &tflags);

    if (t->state == TASK_DEAD) {
        spin_unlock_irqrestore(&t->job_lock, tflags);
        ob_job_assign_unwind(job, t, &absorbed);
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
        ob_job_assign_unwind(job, t, &absorbed);
        return r;
    }

    spin_lock_irqsave(&job->lock, &jflags);
    if (job->terminated) {
        spin_unlock_irqrestore(&job->lock, jflags);
        spin_unlock_irqrestore(&t->job_lock, tflags);
        ob_job_assign_unwind(job, t, &absorbed);
        return STATUS_INVALID_PARAMETER;   /* job is dead: no new members */
    }
    if ((job->limit_flags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) &&
        job->num_members >= job->active_process_limit) {
        spin_unlock_irqrestore(&job->lock, jflags);
        spin_unlock_irqrestore(&t->job_lock, tflags);
        ob_job_assign_unwind(job, t, &absorbed);
        return STATUS_QUOTA_EXCEEDED;
    }
    if (job->num_members >= JOB_MAX_MEMBERS) {
        spin_unlock_irqrestore(&job->lock, jflags);
        spin_unlock_irqrestore(&t->job_lock, tflags);
        ob_job_assign_unwind(job, t, &absorbed);
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
    /* Same critical section as the baseline capture and the pid publication, so a
     * collector can never pair this member's pid with a generation that predates
     * its baseline. ob_job_fork_inherit routes through here, so this one bump
     * covers both ways a member can join. */
    job_member_gen_bump(job);
    /* Publish the absorb record WITH the membership, under the same lock that
     * the detach consumes it under: only this recorded amount is owned by the
     * membership, and exactly one detacher may withdraw it. */
    t->job_absorb = absorbed;
    t->job = job;
    spin_unlock_irqrestore(&job->lock, jflags);
    spin_unlock_irqrestore(&t->job_lock, tflags);

    /* End the quiesce only now that the membership is PUBLISHED. That ordering is
     * the contract: charges were drained, the absorb folded the joiner's usage
     * in, its outstanding obligations were adopted, membership became visible,
     * and only then is charging reopened -- so no charge can be admitted against
     * a chain that is mid-transition. */
    if (job->quota)
        quota_gate_reopen(t);

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
 * The IRQ-off hold used to scale with member count: every member's live counters
 * were read INSIDE job->lock. The generation counter splits that walk in two --
 * the part that must be locked (the membership list and the join-time baselines,
 * both plain memory written under this lock) and the part that must not be (the
 * live counter reads, which are the expensive half) -- and validates the join
 * with member_gen. Contract and both paths: ob_job.h.
 * ----------------------------------------------------------------------- */

/* Diagnostics for the two non-ideal outcomes. RELAXED: they are evidence, never
 * control flow, and the collector runs from a syscall path where a per-event
 * klog would be a flood. */
static uint64_t g_job_collect_retries;
static uint64_t g_job_collect_fallbacks;

uint64_t ob_job_collect_retry_count(void)
{
    return __atomic_load_n(&g_job_collect_retries, __ATOMIC_RELAXED);
}

uint64_t ob_job_collect_fallback_count(void)
{
    return __atomic_load_n(&g_job_collect_fallbacks, __ATOMIC_RELAXED);
}

uint64_t ob_job_member_gen(JOB_OBJECT *job)
{
    uint64_t flags, gen;

    if (!job)
        return 0;
    spin_lock_irqsave(&job->lock, &flags);
    gen = job->member_gen;
    spin_unlock_irqrestore(&job->lock, flags);
    return gen;
}

#ifdef KERNEL_TESTS
/* Remaining injected churn events; see ob_job_test_inject_gen_churn. Only ever
 * touched under job->lock (consumer) or between test phases (setter), so the
 * plain accesses below are serialized by the same lock as member_gen itself. */
static uint32_t g_job_gen_churn_inject;

void ob_job_test_inject_gen_churn(uint32_t attempts)
{
    __atomic_store_n(&g_job_gen_churn_inject, attempts, __ATOMIC_RELEASE);
}

/* Consume one injected churn event, if any. Called with job->lock held, in the
 * re-check window -- the same place a real concurrent join or departure would
 * have bumped the generation. */
static void job_gen_churn_maybe(JOB_OBJECT *job)
{
    uint32_t left = __atomic_load_n(&g_job_gen_churn_inject, __ATOMIC_ACQUIRE);

    if (!left)
        return;
    __atomic_store_n(&g_job_gen_churn_inject, left - 1u, __ATOMIC_RELEASE);
    job->member_gen++;
}
#endif

/* The eight aggregate columns a job reports. A named struct rather than eight
 * locals so the fast path and the locked fallback share ONE accumulation routine
 * and cannot drift into two subtly different formulas. */
struct job_acct_sums {
    uint64_t user_ns;
    uint64_t kernel_ns;
    uint64_t read_ops;
    uint64_t write_ops;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t other_ops;
    uint64_t other_bytes;
};

static void job_sums_zero(struct job_acct_sums *s)
{
    s->user_ns    = 0; s->kernel_ns   = 0;
    s->read_ops   = 0; s->write_ops   = 0;
    s->read_bytes = 0; s->write_bytes = 0;
    s->other_ops  = 0; s->other_bytes = 0;
}

static void job_sums_add(struct job_acct_sums *s, const struct task_acct_base *b)
{
    s->user_ns    += b->user_time_ns;
    s->kernel_ns  += b->kernel_time_ns;
    s->read_ops   += b->io_read_count;
    s->write_ops  += b->io_write_count;
    s->read_bytes += b->io_read_bytes;
    s->write_bytes += b->io_write_bytes;
    s->other_ops  += b->io_other_count;
    s->other_bytes += b->io_other_bytes;
}

/* Accumulate the membership-interval delta of pids[first, first+count) with the
 * lock taken only for the BASELINE COPY.
 *
 * Two constraints shape this, and only a per-member delta satisfies both:
 *
 *  1. The baseline cannot be read unlocked. task.job_acct_base is a plain
 *     ten-word struct whose only writer (ob_job_assign) stores into it under
 *     job->lock, so an unlocked read is a data race -- and re-checking a
 *     generation afterwards cannot repair that; it only discards the value the
 *     race produced.
 *  2. The subtraction must SATURATE PER MEMBER. task_acct_sub_sat exists because
 *     an inversion means a counter was reset under a live baseline, and its own
 *     contract states the consequence of the alternative: "wrapping would add
 *     ~2^64 to a job's aggregate and make every derived rate meaningless." A
 *     reset does not bump member_gen, so the generation cannot stand in for that
 *     defence. Summing the two sides and subtracting ONCE at the aggregate is
 *     therefore wrong, however tempting the arithmetic: it turns one member's
 *     reset into a near-UINT64_MAX total exposed through
 *     NtQueryInformationJobObject.
 *
 * So each member's delta is computed individually, through the SAME
 * task_acct_delta_since the fully-locked walk used -- saturation included, and
 * independently tested there. What moves outside the lock is the expensive half:
 * the live counter reads (RELAXED atomic loads over the static never-freed
 * tasks[] array) and the arithmetic.
 *
 * Baselines are copied in BATCHES rather than all at once because a full
 * JOB_MAX_MEMBERS array of them is 2560 bytes against an 8 KiB kernel task stack.
 * A batch bounds both the stack cost and each IRQ-off hold to a constant, which is
 * the property the cost work wanted -- a hold that does not scale with member
 * count -- at the price of ceil(n / JOB_COLLECT_BATCH) short acquisitions instead
 * of one long one. Membership moving between batches bumps member_gen, so the
 * caller's re-check discards the whole attempt. */
static void job_sum_members_batched(JOB_OBJECT *job, const uint32_t *pids,
                                    uint32_t first, uint32_t count,
                                    struct job_acct_sums *sums)
{
    /* Zeroed at declaration so an entry whose slot did not resolve is never a
     * stale read; the second loop skips those members anyway, which makes this
     * belt-and-braces rather than load-bearing. */
    struct task_acct_base base[JOB_COLLECT_BATCH] = { { 0 } };
    uint64_t flags;
    uint32_t i;

    spin_lock_irqsave(&job->lock, &flags);
    for (i = 0; i < count; i++) {
        struct task *t = task_get_by_pid(pids[first + i]);

        if (t)
            base[i] = t->job_acct_base;
    }
    spin_unlock_irqrestore(&job->lock, flags);

    /* Lock released: live reads and arithmetic only. */
    for (i = 0; i < count; i++) {
        struct task *t = task_get_by_pid(pids[first + i]);
        struct task_acct_base delta;

        if (!t)
            continue;
        task_acct_delta_since(t, &base[i], &delta);
        job_sums_add(sums, &delta);
    }
}

/* The same accumulation with job->lock ALREADY HELD -- the fallback walk. Reads
 * each baseline in place, since nothing can move underneath it. */
static void job_sum_members_locked(const uint32_t *pids, uint32_t n,
                                   struct job_acct_sums *sums)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        struct task *t = task_get_by_pid(pids[i]);
        struct task_acct_base delta;

        if (!t)
            continue;
        task_acct_delta_since(t, &t->job_acct_base, &delta);
        job_sums_add(sums, &delta);
    }
}

/* Snapshot the membership list plus everything else the report needs from the
 * body. Call with job->lock held. `pids` is COMPACTED to members whose task slot
 * resolves, so the baseline walk and the later live walk cover exactly the same
 * set by construction -- if they could disagree on membership the difference
 * between them would be meaningless. ActiveProcesses still reports the body's
 * own num_members, unchanged, so a member with an unresolvable slot is visible as
 * a count rather than silently dropped from both. */
static void job_snapshot_locked(JOB_OBJECT *job, uint32_t *pids, uint32_t *n_out,
                                struct job_acct_sums *persistent,
                                uint32_t *total_processes, uint32_t *active,
                                uint32_t *total_terminated)
{
    uint32_t i, n = 0;

    for (i = 0; i < job->num_members; i++) {
        if (task_get_by_pid(job->member_pids[i]))
            pids[n++] = job->member_pids[i];
    }
    *n_out = n;

    persistent->user_ns     = job->acc_user_ns;
    persistent->kernel_ns   = job->acc_kernel_ns;
    persistent->read_ops    = job->acc_read_ops;
    persistent->write_ops   = job->acc_write_ops;
    persistent->read_bytes  = job->acc_read_bytes;
    persistent->write_bytes = job->acc_write_bytes;
    persistent->other_ops   = job->acc_other_ops;
    persistent->other_bytes = job->acc_other_bytes;

    *total_processes  = job->total_processes;   /* ever associated (monotonic) */
    *active           = job->num_members;      /* currently live */
    *total_terminated = job->total_terminated;
}

void ob_job_collect_accounting(JOB_OBJECT *job,
                               JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *acct,
                               IO_COUNTERS *io)
{
    uint64_t flags;
    uint32_t i, attempt;
    uint32_t pids[JOB_MAX_MEMBERS];
    uint32_t n = 0;
    uint32_t total_processes = 0, active = 0, total_terminated = 0;
    struct job_acct_sums persistent, delta;
    int settled = 0;

    /* Zero first so partial fills never leak stack. */
    for (i = 0; i < sizeof(*acct); i++)
        ((uint8_t *)acct)[i] = 0;
    if (io)
        for (i = 0; i < sizeof(*io); i++)
            ((uint8_t *)io)[i] = 0;

    if (!job)
        return;

    job_sums_zero(&delta);
    job_sums_zero(&persistent);

    for (attempt = 0; attempt < JOB_COLLECT_MAX_RETRIES; attempt++) {
        uint64_t gen0, gen1;
        uint32_t off, pub0, pub1;

        spin_lock_irqsave(&job->lock, &flags);
        gen0 = job->member_gen;
        /* Publication sequence, sampled inside the same critical section as the
         * snapshot so it cannot be reordered against the compaction below. */
        pub0 = task_count();
        job_snapshot_locked(job, pids, &n, &persistent,
                            &total_processes, &active, &total_terminated);
        spin_unlock_irqrestore(&job->lock, flags);

        /* THE UNLOCKED HALF. Each batch takes the lock only long enough to copy
         * that batch's baselines; every live counter read and every subtraction
         * happens with the lock free and interrupts enabled. */
        job_sums_zero(&delta);
        for (off = 0; off < n; off += JOB_COLLECT_BATCH) {
            uint32_t count = n - off;

            if (count > JOB_COLLECT_BATCH)
                count = JOB_COLLECT_BATCH;
            job_sum_members_batched(job, pids, off, count, &delta);
        }

        spin_lock_irqsave(&job->lock, &flags);
#ifdef KERNEL_TESTS
        job_gen_churn_maybe(job);
#endif
        gen1 = job->member_gen;
        pub1 = task_count();
        spin_unlock_irqrestore(&job->lock, flags);

        /* TWO conditions, because they catch different events.
         *
         * An equal GENERATION means no join, departure or terminate intervened.
         *
         * An equal PUBLICATION SEQUENCE means no task became resolvable during the
         * window, and that is a separate hazard the generation cannot see:
         * ob_job_fork_inherit adds a child to member_pids[] BEFORE task publication
         * makes its slot resolvable, and publication happens outside job->lock and
         * bumps no generation. Without this the collector could compact the
         * unpublished child out, the child could then be published and accrue real
         * usage while the walk ran, and the generation would still match -- yielding
         * a total that sampled every other member at the END of the interval while
         * excluding that one. ("An unpublished child contributes zero" is true only
         * at the snapshot, not across the window.)
         *
         * task_count() is the right shape of check and a per-member resolvability
         * re-walk is NOT: a walk has a cursor, so a publication landing behind it
         * leaves the count unchanged while the child runs. num_tasks only ever
         * increments, on exactly the event that matters, and is bounded by TASK_MAX
         * so it cannot wrap. The cost is that an unrelated task creation also forces
         * a retry; task creation is rare and off every hot path, and the failure
         * direction is conservative (retry, then the locked fallback).
         *
         * HOW STRONG THIS IS, precisely: it NARROWS the window, it does not prove it
         * shut. num_tasks is a plain uint32_t incremented outside job->lock, so these
         * samples establish no happens-before against the publisher -- equal samples
         * are strong evidence, not a formal guarantee -- and the fully-locked
         * fallback below does not validate publication at all. Closing the class
         * properly means making task publication generation-visible, or its counter
         * atomic end-to-end, which is a change to the scheduler task-publication
         * protocol and is owned by the SMP phase-2 work; the accounting-quotas TODO
         * carries the accepted-finding stamp naming it. The residue is bounded and
         * self-correcting: a child published mid-walk is counted in ActiveProcesses
         * while the microseconds of usage it accrued during the walk are not, and the
         * next query is right.
         *
         * Both equal means every baseline read above still belongs to the membership
         * the counts describe, so the accumulated delta is publishable AS FAR AS
         * MEMBERSHIP GOES -- which is the guarantee, and the only one: the accepted
         * publication window above is explicitly outside it. */
        if (gen1 == gen0 && pub1 == pub0) {
            settled = 1;
            break;
        }
        __atomic_fetch_add(&g_job_collect_retries, 1ull, __ATOMIC_RELAXED);
    }

    if (!settled) {
        /* Invalidation outlasted the retry bound -- from either cause, so possibly
         * from task publication elsewhere rather than anything this job did. Take the
         * whole walk under the lock: no join, departure or terminate can invalidate
         * it, so this terminates in one pass, and the price is one long IRQ-off hold
         * rather
         * than a sum split across two membership states -- which matters because
         * this function cannot report failure. It is not a stronger guarantee than
         * that: publication is not serialized by this lock either, so the accepted
         * window above applies here too, and here it is not even detected. */
        __atomic_fetch_add(&g_job_collect_fallbacks, 1ull, __ATOMIC_RELAXED);
        spin_lock_irqsave(&job->lock, &flags);
        job_snapshot_locked(job, pids, &n, &persistent,
                            &total_processes, &active, &total_terminated);
        job_sums_zero(&delta);
        job_sum_members_locked(pids, n, &delta);
        spin_unlock_irqrestore(&job->lock, flags);
    }

    /* Departed-member usage (persistent) + live-member membership-interval usage.
     * A live member contributes exactly what a departing one folds in: its usage
     * since it joined. Both halves of the membership lifetime use the same
     * baseline, so a member's contribution does not jump when it exits. */
    acct->TotalProcesses           = total_processes;
    acct->ActiveProcesses          = active;
    acct->TotalTerminatedProcesses = total_terminated;

    /* 100ns units (Windows LARGE_INTEGER convention). */
    acct->TotalUserTime            = (int64_t)((persistent.user_ns + delta.user_ns) / 100);
    acct->TotalKernelTime          = (int64_t)((persistent.kernel_ns + delta.kernel_ns) / 100);
    acct->ThisPeriodTotalUserTime  = acct->TotalUserTime;
    acct->ThisPeriodTotalKernelTime = acct->TotalKernelTime;

    if (io) {
        io->ReadOperationCount  = persistent.read_ops    + delta.read_ops;
        io->WriteOperationCount = persistent.write_ops   + delta.write_ops;
        io->ReadTransferCount   = persistent.read_bytes  + delta.read_bytes;
        io->WriteTransferCount  = persistent.write_bytes + delta.write_bytes;
        /* Control ("Other") I/O completes the Windows IO_COUNTERS ABI, whose
         * Other* fields were previously left zeroed. They report the device-
         * control traffic counted by task_acct_note_control_io(). */
        io->OtherOperationCount = persistent.other_ops   + delta.other_ops;
        io->OtherTransferCount  = persistent.other_bytes + delta.other_bytes;
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

/* --- Aggregate quota report (NtQueryInformationJobObject extension) ------- */

/* Take a counted reference on the job's quota block, or NULL if the job is on
 * its way out. quota.h makes a live reference the caller's obligation for every
 * dereference; job->quota alone is only a pointer, and job_on_delete can drop
 * the last reference while a syscall is still reading through it. */
static quota_block_t *job_quota_ref(JOB_OBJECT *job)
{
    quota_block_t *block;
    uint64_t flags;

    if (!job)
        return (quota_block_t *)0;
    spin_lock_irqsave(&job->lock, &flags);
    block = job->quota;
    if (block && !quota_block_try_ref(block))
        block = (quota_block_t *)0;
    spin_unlock_irqrestore(&job->lock, flags);
    return block;
}

void ob_job_collect_quota_limits(JOB_OBJECT *job,
                                 JOBOBJECT_QUOTA_LIMIT_INFORMATION *out)
{
    quota_block_t *block;
    uint32_t i, count;

    if (!out)
        return;

    for (i = 0; i < sizeof(*out); i++)
        ((uint8_t *)out)[i] = 0;

    count = quota_resource_type_count();
    if (count > QUOTA_RESOURCE_TYPE_COUNT)
        count = QUOTA_RESOURCE_TYPE_COUNT;   /* registry can never exceed the ABI */
    out->ResourceCount = count;

    block = job_quota_ref(job);
    if (!block)
        return;   /* rows stay zero: no block means nothing is charged yet */

    for (i = 0; i < count; i++) {
        quota_resource_type_t type = (quota_resource_type_t)i;
        out->Resources[i].Usage    = quota_usage(block, type);
        out->Resources[i].Peak     = quota_peak(block, type);
        out->Resources[i].Limit    = quota_limit(block, type);
        out->Resources[i].Failures = quota_failures(block, type);
    }

    quota_block_deref(block);
}

NTSTATUS ob_job_set_quota_limits(JOB_OBJECT *job,
                                 const JOBOBJECT_QUOTA_LIMIT_INFORMATION *in,
                                 int caller_privileged)
{
    uint64_t previous[QUOTA_RESOURCE_TYPE_COUNT];
    quota_block_t *block;
    uint32_t count, i;
    NTSTATUS st;

    if (!job || !in)
        return STATUS_INVALID_PARAMETER;
    count = quota_resource_type_count();
    if (count > QUOTA_RESOURCE_TYPE_COUNT)
        count = QUOTA_RESOURCE_TYPE_COUNT;
    if (in->Reserved != 0 || in->ResourceCount > count)
        return STATUS_INVALID_PARAMETER;

    /* A job handle carries no per-object access mask yet (job_lookup accepts
     * any handle of the right type, and NtOpenJobObject installs access 0), so
     * "lowering is unprivileged" -- correct for a process constraining ITSELF
     * -- would let any opener of a shared named job tighten the limits of every
     * member. Until job handles carry rights, EVERY aggregate limit write needs
     * SeIncreaseQuotaPrivilege, raise or lower. The per-process class keeps the
     * unprivileged-lowering rule because its principal is provably the caller.
     * -> XREF: TODO-05-object-manager.md section 3 (granted-access enforcement). */
    if (!caller_privileged)
        return STATUS_PRIVILEGE_NOT_HELD;

    /* Representation first: these checks are pure and independent of any state,
     * so a malformed request is refused as malformed rather than as whatever
     * the job's current block situation happens to be. */
    for (i = 0; i < in->ResourceCount; i++) {
        if (in->Resources[i].Usage || in->Resources[i].Peak ||
            in->Resources[i].Failures)
            return STATUS_INVALID_PARAMETER;   /* kernel-owned columns */
        if (in->Resources[i].Limit > (uint64_t)QUOTA_AMOUNT_MAX)
            return STATUS_INVALID_PARAMETER;
    }

    block = job_quota_ref(job);
    if (!block)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* No cross-row authorization pass and no transaction lock: because EVERY
     * write here is privileged, the verdict does not depend on any row's
     * current value, so there is no pre-image for a concurrent setter to make
     * stale -- the race that would otherwise need serialization cannot arise.
     * Two racing privileged administrators land last-writer-wins per row,
     * exactly as two sequential calls would. Holding a global lock across the
     * row loop instead would put up to 16 nested quota-block acquisitions
     * inside one IRQ-off window, far past spinlock.h's hold-time budget. When
     * job handles carry access rights and unprivileged lowering returns, the
     * verdict becomes value-dependent again and MUST regain a PASSIVE_LEVEL
     * per-job transaction lock. -> XREF: TODO-05-object-manager.md section 3.
     *
     * ---- Commit, remembering each pre-image so a late failure restores. */
    st = STATUS_SUCCESS;
    for (i = 0; i < in->ResourceCount; i++) {
        previous[i] = quota_limit(block, (quota_resource_type_t)i);
        st = quota_set_limit(block, (quota_resource_type_t)i,
                             in->Resources[i].Limit);
        if (st != STATUS_SUCCESS) {
            uint32_t back;
            for (back = 0; back < i; back++)
                (void)quota_set_limit(block, (quota_resource_type_t)back,
                                      previous[back]);
            break;
        }
    }

    quota_block_deref(block);
    return st;
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
