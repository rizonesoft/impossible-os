/* ============================================================================
 * quota_owner.c -- Who owns a charge: process, user, and job
 *
 * quota.c owns the accounting MECHANISM (blocks, counters, limits). This file
 * owns the OWNERSHIP MODEL layered on top: which blocks a process charges
 * against, how they are inherited at process creation, how they are released
 * at death, and how a single charge is admitted by all of them or by none.
 *
 * Split from quota.c on purpose. The block internals stay private to quota.c
 * (the opaque-struct rule that makes the no-nested-lock contract enforceable),
 * so everything here is written against the same public API any other
 * subsystem uses -- the ownership model gets no special access to counters.
 *
 * Three principals own every charge:
 *   PROCESS  one block per live task, created with the task.
 *   USER     the canonical per-SID block behind the task's primary token,
 *            shared by every token for that SID so a user's budget cannot be
 *            multiplied by opening another token lineage.
 *   JOB      the block on the Job Object the task belongs to, if any.
 *
 * Both the process and user block pointers live in the task and are published
 * and cleared under task->quota_lock. That lock is the whole lifetime
 * protocol: a reader takes it, loads the pointer, and acquires a reference
 * before releasing it, so teardown (which clears and dereferences under the
 * same lock) can never free a block out from under a charge in flight. A
 * release-store would NOT be sufficient here -- publication order does not
 * stop a reader that already loaded the pointer from referencing it after the
 * final dereference has freed it.
 * ========================================================================== */

#include "kernel/quota/quota.h"
#include "kernel/sched/task.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_job.h"
#include "kernel/security/token.h"
#include "kernel/klog.h"

/* --- Task block attach / detach ------------------------------------------ */

NTSTATUS quota_task_init(struct task *task, struct access_token *token)
{
    quota_block_t *proc = NULL;
    quota_block_t *user = NULL;
    ACCESS_TOKEN  *tok  = (ACCESS_TOKEN *)token;

    uint64_t       flags;

    if (!task)
        return STATUS_INVALID_PARAMETER;

    /* A task with no token is normal: early kernel threads run before the SRM
     * assigns one, and they get an ownerless process block (still fully
     * chargeable -- only the per-user aggregate needs a SID). The caller holds
     * the token's reference across this call, so UserSid is stable here. */
    if (tok && tok->UserSid) {
        uint32_t sid_len = RtlLengthSid(tok->UserSid);
        proc = quota_block_create(QUOTA_PRINCIPAL_PROCESS, tok->UserSid, sid_len);
        if (proc) {
            /* The user block is SHARED, not created per process: that is what
             * makes the per-user budget an aggregate rather than a fresh
             * allowance handed out once per token. */
            user = quota_user_block_acquire(tok->UserSid, sid_len);
            if (!user) {
                quota_block_deref(proc);
                proc = NULL;
            }
        }
    } else {
        proc = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    }

    if (!proc)
        return STATUS_INSUFFICIENT_RESOURCES;

    spin_lock_irqsave(&task->quota_lock, &flags);
    if (task->quota) {
        /* Already initialized (idempotent by contract -- a re-created task slot
         * or a doubled create path must not leak the first block). */
        spin_unlock_irqrestore(&task->quota_lock, flags);
        quota_block_deref(proc);
        if (user)
            quota_block_deref(user);
        return STATUS_SUCCESS;
    }
    task->quota      = proc;
    task->quota_user = user;
    spin_unlock_irqrestore(&task->quota_lock, flags);
    return STATUS_SUCCESS;
}

void quota_task_teardown(struct task *task)
{
    quota_block_t *proc;
    quota_block_t *user;
    uint64_t       flags;

    if (!task)
        return;

    /* Clear under the lock, dereference outside it. Dropping the last
     * reference can free the block and take the registry lock; neither belongs
     * inside a task lock, and holding one across the free would nest two lock
     * classes for no benefit. */
    spin_lock_irqsave(&task->quota_lock, &flags);
    proc = task->quota;
    user = task->quota_user;
    task->quota      = NULL;
    task->quota_user = NULL;
    spin_unlock_irqrestore(&task->quota_lock, flags);

    quota_block_deref(proc);
    quota_block_deref(user);
}

/* --- Chain snapshot ------------------------------------------------------ */

/* Append `block` to the receipt, taking a reference, unless it is NULL or
 * already present. Returns 0 when the chain is full.
 *
 * Deduplication is not cosmetic. If the same block appeared twice the charge
 * would be applied twice for one resource, and the limit would bite at half
 * the configured value. It is reachable in practice: a process whose job block
 * is its own owner block, or a future ancestor walk that revisits a shared
 * parent job. */
static int quota_chain_append(quota_charge_receipt_t *receipt, quota_block_t *block)
{
    if (!block)
        return 1;
    for (uint32_t i = 0; i < receipt->count; i++) {
        if (receipt->blocks[i] == block)
            return 1;
    }
    if (receipt->count >= QUOTA_CHAIN_MAX)
        return 0;
    quota_block_ref(block);
    receipt->blocks[receipt->count++] = block;
    return 1;
}

/* Release every reference the receipt holds and empty it. */
static void quota_chain_release(quota_charge_receipt_t *receipt)
{
    for (uint32_t i = 0; i < receipt->count; i++) {
        quota_block_deref(receipt->blocks[i]);
        receipt->blocks[i] = NULL;
    }
    receipt->count = 0;
}

/* Collect the process and user blocks a task charges against.
 *
 * Returns 0 if the chain is full, -1 if the task has no process block. */
static int quota_chain_add_task_blocks(quota_charge_receipt_t *receipt,
                                       struct task *task,
                                       quota_block_t **seen_process)
{
    uint64_t flags;
    int      ok;

    /* One critical section for both pointers: the pair is replaced together at
     * init and cleared together at teardown, so reading them apart could mix a
     * live block with a stale one. */
    spin_lock_irqsave(&task->quota_lock, &flags);
    *seen_process = task->quota;
    ok = quota_chain_append(receipt, task->quota);
    if (ok)
        ok = quota_chain_append(receipt, task->quota_user);
    spin_unlock_irqrestore(&task->quota_lock, flags);

    if (!*seen_process)
        return -1;
    return ok;
}

/* Confirm the task did not die while the chain was being collected.
 *
 * The process/user blocks and the job block are captured in SEPARATE critical
 * sections, and death teardown detaches the job BEFORE it clears the quota
 * pointers. Without this re-check a charge could capture the process and user
 * blocks, have the job detached underneath it, then observe no job and commit
 * -- an allocation admitted against a job limit that never saw it, with a
 * receipt that can never restore the missing accounting.
 *
 * Re-reading the process pointer is a sufficient witness: teardown clears it
 * after the detach, so a pointer that is still the one we captured means no
 * teardown completed in between. Returns 0 when the task died mid-snapshot. */
static int quota_chain_still_live(struct task *task, quota_block_t *seen_process)
{
    uint64_t flags;
    int      live;

    spin_lock_irqsave(&task->quota_lock, &flags);
    live = (task->quota == seen_process);
    spin_unlock_irqrestore(&task->quota_lock, flags);
    return live;
}

/* Collect the block of every job owning the task.
 *
 * Nested jobs do not exist yet: ob_job_assign refuses a second job and
 * JOB_OBJECT has no parent link, so the chain is at most one job deep. The
 * walk is written as a collection step rather than a single fetch so adding
 * ancestors later is a change here and nowhere else. */
static int quota_chain_add_job_blocks(quota_charge_receipt_t *receipt,
                                      struct task *task)
{
    JOB_OBJECT *job;
    uint64_t    flags;
    int         ok;

    /* Pin the job under job_lock before releasing it -- the same discipline
     * ob_job_fork_inherit uses -- so a concurrent detach cannot drop the last
     * membership reference and free the body while its block is read. The job
     * holds its block for its whole life, so the block is live while the job
     * is pinned. */
    spin_lock_irqsave(&task->job_lock, &flags);
    job = task->job;
    if (job)
        ObReferenceObject(job);
    spin_unlock_irqrestore(&task->job_lock, flags);

    if (!job)
        return 1;

    ok = quota_chain_append(receipt, (quota_block_t *)job->quota);
    ObDereferenceObject(job);
    return ok;
}

/* --- Live job assignment ------------------------------------------------- */

NTSTATUS quota_job_absorb_task(struct quota_block *job_block, struct task *task,
                               quota_absorb_record_t *rec)
{
    quota_block_t *proc;
    uint64_t       flags;
    uint32_t       i;
    NTSTATUS       st = STATUS_SUCCESS;

    if (!job_block || !task || !rec)
        return STATUS_INVALID_PARAMETER;

    for (i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++)
        rec->taken[i] = 0;
    rec->active = 0;

    /* Pin the process block: the absorb runs before membership is published,
     * so nothing else keeps it alive for us. */
    spin_lock_irqsave(&task->quota_lock, &flags);
    proc = task->quota;
    if (proc)
        quota_block_ref(proc);
    spin_unlock_irqrestore(&task->quota_lock, flags);

    if (!proc)
        return STATUS_SUCCESS;    /* nothing charged yet; nothing to absorb */

    /* Fold the joiner's CURRENT usage into the job so its aggregate limit
     * covers what the process already holds. Without this a process could
     * allocate freely, then join a capped job, and its existing usage would
     * count against nothing -- an escape hatch out of every job limit. */
    for (i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        uint64_t taken = quota_usage(proc, (quota_resource_type_t)i);
        if (taken == 0)
            continue;
        st = quota_charge(job_block, (quota_resource_type_t)i, taken);
        if (st != STATUS_SUCCESS)
            break;
        rec->taken[i] = taken;
        rec->active = 1;
    }

    if (st != STATUS_SUCCESS) {
        /* All-or-nothing: give back what this absorb already folded in, so a
         * refused assignment leaves the job's accounting untouched. */
        quota_job_unabsorb(job_block, rec);
    }

    quota_block_deref(proc);
    return st;
}

void quota_job_unabsorb(struct quota_block *job_block, quota_absorb_record_t *rec)
{
    if (!job_block || !rec || !rec->active)
        return;
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        if (rec->taken[i]) {
            NTSTATUS st = quota_return(job_block, (quota_resource_type_t)i,
                                       rec->taken[i]);
            if (st != STATUS_SUCCESS) {
                /* Refused only if the job's counter no longer holds what this
                 * record folded in -- the accounting has already drifted.
                 * Counted, NOT klog'd: this runs from ob_job_detach_task on
                 * the task_death_teardown path, which is log-free by contract
                 * and reachable at elevated IRQL, where klog's live-disk flush
                 * would turn a recoverable accounting diagnostic into a stall.
                 * quota_return has already recorded the underflow through the
                 * IRQL-gated diagnostic path. */
                quota_note_unabsorb_refused();
            }
            rec->taken[i] = 0;
        }
    }
    rec->active = 0;
}

/* --- Chain charge / return ----------------------------------------------- */

NTSTATUS quota_charge_chain(struct task *task, quota_resource_type_t type,
                            uint64_t amount, uint32_t flags,
                            quota_charge_receipt_t *receipt)
{
    NTSTATUS st;
    int      chain;

    if (!receipt || !task || (flags & ~QUOTA_CHARGE_CLIENT) != 0)
        return STATUS_INVALID_PARAMETER;

    /* Client charging is REFUSED, not approximated. Billing the impersonated
     * client requires reading the executing thread's impersonation token, and
     * neither piece of infrastructure that makes that safe on SMP exists yet:
     * thread_current() resolves through process-global scheduler cursors (so it
     * can sample a sibling thread of the same task), and the token slot has no
     * teardown-safe pin (so a concurrent RevertToSelf can free the token
     * between the load and the reference). Approximating it would silently bill
     * the wrong user, which is worse than refusing. The flag and its contract
     * stay so callers can be written against the final shape.
     * Owned by the security reference monitor's teardown-safe token pin. */
    if (flags & QUOTA_CHARGE_CLIENT)
        return STATUS_NOT_SUPPORTED;

    /* Claim the receipt EXCLUSIVELY before touching a single field. Losing
     * this CAS means the receipt already holds a live charge (or another CPU
     * is mid-operation on it); overwriting it would drop the references that
     * charge is holding and strand its usage permanently. */
    if (atomic_cmpxchg(&receipt->state, QUOTA_RECEIPT_IDLE, QUOTA_RECEIPT_BUSY)
        != QUOTA_RECEIPT_IDLE)
        return STATUS_INVALID_PARAMETER;

    receipt->count  = 0;
    receipt->type   = type;
    receipt->amount = amount;

    if (amount == 0) {
        /* No-op: nothing charged, nothing owed. Release the claim so the
         * receipt stays reusable. */
        atomic_set(&receipt->state, QUOTA_RECEIPT_IDLE);
        return STATUS_SUCCESS;
    }

    /* Snapshot the whole chain FIRST, holding a reference on every block, and
     * take no owner lock past this point. Charging while holding job_lock or a
     * job's lock would violate the no-nested-call contract in quota.h and put
     * two lock classes in a cycle. */
    quota_block_t *seen_process = NULL;
    chain = quota_chain_add_task_blocks(receipt, task, &seen_process);
    if (chain == 1) {
        chain = quota_chain_add_job_blocks(receipt, task) ? 1 : 0;
        /* Re-validate AFTER the job step: the task may have died between the
         * two snapshots, in which case its job was already detached and this
         * chain is missing it. Fail closed rather than admit a charge no job
         * limit ever saw. */
        if (chain == 1 && !quota_chain_still_live(task, seen_process))
            chain = -1;
    }

    if (chain != 1) {
        quota_chain_release(receipt);
        atomic_set(&receipt->state, QUOTA_RECEIPT_IDLE);
        if (chain < 0) {
            /* No process block: the task is dead (teardown cleared it) or not
             * yet fully built. Admitting an empty chain here would report
             * success having charged nothing, so a process racing its own
             * death could allocate completely unaccounted. Fail closed. */
            return STATUS_PROCESS_IS_TERMINATING;
        }
        /* Chain deeper than QUOTA_CHAIN_MAX. Refuse rather than charge a
         * truncated chain: a link left out is a limit left unenforced. */
        klog(LOG_ERROR, "quota", "charge chain exceeds %u blocks for pid=%u",
             (uint64_t)QUOTA_CHAIN_MAX, (uint64_t)task->pid);
        return STATUS_INVALID_PARAMETER;
    }

    /* Charge one block at a time, each under its own lock -- never two at
     * once, so no ordering hazard exists between chains that share a job. */
    for (uint32_t i = 0; i < receipt->count; i++) {
        st = quota_charge(receipt->blocks[i], type, amount);
        if (st != STATUS_SUCCESS) {
            /* Unwind exactly the prefix that was charged. quota_return does
             * not consult limits, so a limit lowered concurrently cannot block
             * the unwind and strand usage. */
            for (uint32_t j = 0; j < i; j++)
                (void)quota_return(receipt->blocks[j], type, amount);
            quota_chain_release(receipt);
            atomic_set(&receipt->state, QUOTA_RECEIPT_IDLE);
            return st;
        }
    }
    /* Publish the receipt as returnable only now that every block is charged:
     * a returner must never see a half-built block set. */
    atomic_set(&receipt->state, QUOTA_RECEIPT_ACTIVE);
    return STATUS_SUCCESS;
}

void quota_return_chain(quota_charge_receipt_t *receipt)
{
    if (!receipt)
        return;
    /* EXACTLY ONE caller may return a given charge. Without this claim two
     * cleanup paths sharing an embedded receipt would both credit the amount
     * back -- the second credit erasing charges made since -- and both would
     * drop the same block references, which is a double free. A losing caller
     * (IDLE: nothing to return, or BUSY: another CPU owns it) does nothing,
     * which is what makes the documented idempotence true under SMP. */
    if (atomic_cmpxchg(&receipt->state, QUOTA_RECEIPT_ACTIVE, QUOTA_RECEIPT_BUSY)
        != QUOTA_RECEIPT_ACTIVE)
        return;

    for (uint32_t i = 0; i < receipt->count; i++)
        (void)quota_return(receipt->blocks[i], receipt->type, receipt->amount);
    quota_chain_release(receipt);
    receipt->amount = 0;
    atomic_set(&receipt->state, QUOTA_RECEIPT_IDLE);
}
