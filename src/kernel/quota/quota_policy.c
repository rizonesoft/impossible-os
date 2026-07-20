/* quota_policy.c -- ProcessQuotaLimits projection and commit transaction
 *
 * See include/kernel/quota/quota_policy.h for the contract. The shape of this
 * file is dictated by one rule: a ProcessQuotaLimits set touches three stores
 * with three different locks and three different failure modes, so it must be
 * PREVALIDATE -> COMMIT -> (restore on an unexpected failure), never a
 * field-by-field walk that can stop halfway.
 *
 * Owner: TODO-25-kernel-resource-accounting-quotas.md section 8.
 */

#include "kernel/quota/quota_policy.h"
#include "kernel/quota/quota.h"
#include "kernel/sched/task.h"
#include "kernel/task_limits.h"
#include "kernel/ob/handle_table.h"
#include "kernel/klog.h"

/* ---- Unit conversion ----------------------------------------------------- *
 * RLIMIT_CPU counts whole seconds; TimeLimit counts signed 100-ns ticks. Both
 * directions must be total functions: the syscall boundary cannot answer "what
 * does this mean" with undefined behaviour. */

int64_t quota_policy_seconds_to_time_limit(uint64_t seconds)
{
    /* RLIM_INFINITY is the rlimit spelling of "no limit"; QUOTA_TIME_LIMIT_NONE
     * is the wire spelling. They are different bit patterns for the same fact,
     * so this is a translation, not a clamp. */
    if (seconds == RLIM_INFINITY)
        return QUOTA_TIME_LIMIT_NONE;
    /* A ZERO-second rlimit is a real, enforced cap of no CPU time at all -- the
     * TIGHTEST limit expressible -- and the wire's 0 means the opposite
     * (unlimited). Projecting it as 0 would report a deny-immediately policy as
     * "no cap", the one direction of this mapping that is dangerous to get
     * wrong, so it is reported as the smallest representable positive limit.
     * The reverse conversion truncates sub-second limits back to 0 seconds, so
     * the pair round-trips. */
    if (seconds == 0)
        return QUOTA_TIME_LIMIT_MIN;
    /* Saturate rather than wrap: a wrapped product reads back NEGATIVE, and a
     * negative TimeLimit is rejected as invalid -- so a caller who set a huge
     * but legal CPU limit would find it unreadable. */
    if (seconds > (uint64_t)QUOTA_TIME_LIMIT_MAX_SEC)
        return (int64_t)0x7FFFFFFFFFFFFFFFLL;
    return (int64_t)(seconds * QUOTA_TIME_100NS_PER_SEC);
}

uint64_t quota_policy_time_limit_to_seconds(int64_t time_limit)
{
    uint64_t secs;

    if (time_limit <= QUOTA_TIME_LIMIT_NONE)
        return RLIM_INFINITY;   /* 0 = unlimited; negatives never reach here */
    /* TRUNCATE towards zero rather than rounding up. rlimit seconds encode
     * "unlimited" as RLIM_INFINITY, never as 0, so a sub-second wire limit
     * lands on a real zero-second cap -- tighter than the caller asked for,
     * which is the safe rounding direction for a limit, and the exact inverse
     * of the zero projection above. */
    secs = (uint64_t)time_limit / QUOTA_TIME_100NS_PER_SEC;
    return secs;
}

/* ---- ABI verification (5-layer defense, layer 2) ------------------------- */

int quota_policy_abi_verify(void)
{
    if (sizeof(QUOTA_LIMITS) != QUOTA_LIMITS_SIZE)
        return 0;
    if (sizeof(QUOTA_LIMITS_EX) != QUOTA_LIMITS_EX_SIZE)
        return 0;
    if (__builtin_offsetof(QUOTA_LIMITS, TimeLimit) != 40)
        return 0;
    if (__builtin_offsetof(QUOTA_LIMITS_EX, Flags) != 80)
        return 0;
    if (__builtin_offsetof(QUOTA_LIMITS_EX, CpuRateLimit) != 84)
        return 0;
    return 1;
}

/* ---- Record lifecycle ---------------------------------------------------- */

void quota_policy_reset(struct task *t)
{
    if (!t)
        return;
    t->quota_policy.min_working_set   = 0;
    t->quota_policy.max_working_set   = 0;
    t->quota_policy.working_set_limit = 0;
    t->quota_policy.pagefile_limit    = 0;
    t->quota_policy.flags             = 0;
    t->quota_policy.cpu_rate_limit    = 0;
    t->quota_policy.generation        = 0;
}

/* ---- Query --------------------------------------------------------------- */

NTSTATUS quota_policy_query(struct task *t, QUOTA_LIMITS_EX *out)
{
    rlimit_t cpu;
    quota_block_t *block;
    uint64_t flags, txn_flags;

    if (!t || !out)
        return STATUS_INVALID_PARAMETER;

    /* Zero first: every later store is an overwrite, so an early return can
     * never hand the caller uninitialised kernel stack. */
    out->PagedPoolLimit        = 0;
    out->NonPagedPoolLimit     = 0;
    out->MinimumWorkingSetSize = 0;
    out->MaximumWorkingSetSize = 0;
    out->PagefileLimit         = 0;
    out->TimeLimit             = QUOTA_TIME_LIMIT_NONE;
    out->WorkingSetLimit       = 0;
    out->Reserved2             = 0;
    out->Reserved3             = 0;
    out->Reserved4             = 0;
    out->Flags                 = 0;
    out->CpuRateLimit          = 0;

    /* The WHOLE projection runs under the transaction lock the setter holds,
     * in the setter's lock order (policy lock -> block/rlimit locks). Reading
     * the three stores unsynchronized let a commit land between them and hand
     * the caller old pool limits beside new CPU and policy values -- a torn
     * view of a transaction this section promises is atomic. */
    spin_lock_irqsave(&t->quota_policy_lock, &txn_flags);

    /* Pool limits: the task's OWN process block, which is the token-local
     * overlay. The shared user block behind it may be looser; the charge path
     * enforces both, so reporting the process-local value is what a caller can
     * act on. quota.h requires a live REFERENCE across every block dereference:
     * the pointer alone can be cleared and its last reference dropped by a
     * concurrent teardown while this projection is still reading it. */
    spin_lock_irqsave(&t->quota_lock, &flags);
    block = t->quota;
    if (block && !quota_block_try_ref(block))
        block = (quota_block_t *)0;   /* already on its way out; report zeros */
    spin_unlock_irqrestore(&t->quota_lock, flags);
    if (block) {
        out->PagedPoolLimit    = quota_limit(block, QUOTA_RES_PAGED_POOL);
        out->NonPagedPoolLimit = quota_limit(block, QUOTA_RES_NONPAGED_POOL);
    }

    if (task_rlimit_get(t, RLIMIT_CPU, &cpu) == RLIMIT_OK)
        out->TimeLimit = quota_policy_seconds_to_time_limit(cpu.rlim_cur);

    out->MinimumWorkingSetSize = t->quota_policy.min_working_set;
    out->MaximumWorkingSetSize = t->quota_policy.max_working_set;
    out->WorkingSetLimit       = t->quota_policy.working_set_limit;
    out->PagefileLimit         = t->quota_policy.pagefile_limit;
    out->Flags                 = t->quota_policy.flags;
    out->CpuRateLimit          = t->quota_policy.cpu_rate_limit;
    spin_unlock_irqrestore(&t->quota_policy_lock, txn_flags);

    /* Outside the lock: the last deref frees, and free must not run with
     * interrupts off behind a lock this path took for a read-only
     * projection. */
    if (block)
        quota_block_deref(block);

    return STATUS_SUCCESS;
}

/* ---- Set ----------------------------------------------------------------- *
 * A "raise" is any move that gives the process MORE headroom, including
 * removing a cap by writing 0. Both directions matter: only raises need the
 * privilege, and lowering must stay unprivileged or a process could not
 * voluntarily constrain itself. */

static int quota_is_raise(uint64_t current, uint64_t want)
{
    if (current == 0)
        return 0;            /* already unlimited: nothing is a raise */
    if (want == 0)
        return 1;            /* removing the cap */
    return want > current;
}

NTSTATUS quota_policy_set(struct task *t, const QUOTA_LIMITS_EX *in,
                          int is_ex, int caller_privileged)
{
    QUOTA_LIMITS_EX req;
    quota_block_t *block;
    rlimit_t cpu_now, cpu_want;
    quota_policy_t saved;
    uint64_t paged_now, nonpaged_now, want_cpu_secs;
    uint64_t flags, txn_flags;
    NTSTATUS st;
    int raises;

    if (!t || !in)
        return STATUS_INVALID_PARAMETER;

    /* Single copy-in, exactly like task_rlimit_set: validate, authorize and
     * commit the SAME snapshot, so a concurrent writer to the caller's buffer
     * cannot present an allowed value at the check and another at the commit. */
    req = *in;

    /* ---- Prevalidate: representation ---- */
    if (req.TimeLimit < 0)
        return STATUS_INVALID_PARAMETER;
    if (req.MinimumWorkingSetSize && req.MaximumWorkingSetSize &&
        req.MinimumWorkingSetSize > req.MaximumWorkingSetSize)
        return STATUS_INVALID_PARAMETER;
    if (req.PagedPoolLimit > (uint64_t)QUOTA_AMOUNT_MAX ||
        req.NonPagedPoolLimit > (uint64_t)QUOTA_AMOUNT_MAX)
        return STATUS_INVALID_PARAMETER;
    if (is_ex) {
        if (req.Reserved2 || req.Reserved3 || req.Reserved4)
            return STATUS_INVALID_PARAMETER;
        if (req.Flags & ~QUOTA_LIMITS_SUPPORTED_FLAGS)
            return STATUS_INVALID_PARAMETER;
        /* ENABLE and DISABLE of the same knob is a contradiction. */
        if ((req.Flags & QUOTA_LIMITS_HARDWS_MIN_PAIR) ==
            QUOTA_LIMITS_HARDWS_MIN_PAIR)
            return STATUS_INVALID_PARAMETER;
        if ((req.Flags & QUOTA_LIMITS_HARDWS_MAX_PAIR) ==
            QUOTA_LIMITS_HARDWS_MAX_PAIR)
            return STATUS_INVALID_PARAMETER;
        if (req.CpuRateLimit & RATE_QUOTA_RESERVED_MASK)
            return STATUS_INVALID_PARAMETER;
        if (RATE_QUOTA_PERCENT(req.CpuRateLimit) > RATE_QUOTA_PERCENT_MAX)
            return STATUS_INVALID_PARAMETER;
    }

    /* Hold a REFERENCE, not just the pointer, for the whole transaction:
     * quota.h makes that the caller's obligation, and this path dereferences
     * the block through authorization, commit and rollback. */
    spin_lock_irqsave(&t->quota_lock, &flags);
    block = t->quota;
    if (block && !quota_block_try_ref(block))
        block = (quota_block_t *)0;
    spin_unlock_irqrestore(&t->quota_lock, flags);
    if (!block)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Serialize the whole transaction: prevalidation reads the current values,
     * and a second setter landing between the read and the commit would make
     * the privilege decision stale. A mutex (not a spinlock) because the
     * commit below takes other locks and must be able to block. */
    spin_lock_irqsave(&t->quota_policy_lock, &txn_flags);

    if (!is_ex) {
        /* A 48-byte caller supplied no suffix, so the suffix does not
         * participate: carry the CURRENT values forward. Zeroing them instead
         * would make every legacy update silently DESTROY an established
         * working-set limit, hard-working-set flags and CPU-rate policy -- and
         * those removals would then read as widenings, so an unprivileged
         * request that only TIGHTENS a base field would be refused for a
         * suffix the caller never sent. Read under the transaction lock, like
         * every other pre-image here. */
        req.WorkingSetLimit = t->quota_policy.working_set_limit;
        req.Reserved2 = 0;
        req.Reserved3 = 0;
        req.Reserved4 = 0;
        req.Flags = t->quota_policy.flags;
        req.CpuRateLimit = t->quota_policy.cpu_rate_limit;
    }

    paged_now    = quota_limit(block, QUOTA_RES_PAGED_POOL);
    nonpaged_now = quota_limit(block, QUOTA_RES_NONPAGED_POOL);
    if (task_rlimit_get(t, RLIMIT_CPU, &cpu_now) != RLIMIT_OK) {
        st = STATUS_INVALID_PARAMETER;
        goto out;
    }
    want_cpu_secs = quota_policy_time_limit_to_seconds(req.TimeLimit);

    /* ---- Prevalidate: authorization ----
     * One verdict over the WHOLE request. Authorizing field-by-field would let
     * an unprivileged caller land the lowering half of a mixed request and be
     * refused only on the raising half -- a partial commit by another name. */
    raises = quota_is_raise(paged_now, req.PagedPoolLimit) ||
             quota_is_raise(nonpaged_now, req.NonPagedPoolLimit) ||
             quota_is_raise(t->quota_policy.max_working_set,
                            req.MaximumWorkingSetSize) ||
             quota_is_raise(t->quota_policy.working_set_limit,
                            req.WorkingSetLimit) ||
             quota_is_raise(t->quota_policy.pagefile_limit,
                            req.PagefileLimit);
    /* CPU time is inverted in spelling (RLIM_INFINITY is the loose end) but
     * identical in meaning, so it is compared in rlimit space. */
    if (cpu_now.rlim_cur != RLIM_INFINITY &&
        (want_cpu_secs == RLIM_INFINITY || want_cpu_secs > cpu_now.rlim_cur))
        raises = 1;
    /* The EX suffix carries more ways to widen the process's constraints, and
     * none of them is a numeric limit. First: a higher CPU-rate percentage
     * (0 = no rate cap, so clearing an active one is the same removal a 0 pool
     * limit is). A 48-byte request cannot reach any of these -- its suffix is
     * carried forward unchanged above, so every comparison here is a no-op for
     * that form. */
    if (quota_is_raise(RATE_QUOTA_PERCENT(t->quota_policy.cpu_rate_limit),
                       RATE_QUOTA_PERCENT(req.CpuRateLimit)))
        raises = 1;
    /* MinimumWorkingSetSize is a FLOOR, not a ceiling, so quota_is_raise's
     * "0 means unlimited" reading does not apply: 0 is "no floor", and any
     * increase reserves more resident memory for this process at everyone
     * else's expense. A plain greater-than is the whole rule. */
    if (req.MinimumWorkingSetSize > t->quota_policy.min_working_set)
        raises = 1;
    /* The two hard-working-set flags point in OPPOSITE directions, because the
     * bounds they enforce do. Dropping HARDWS_MAX_ENABLE turns an enforced
     * ceiling into an advisory one -- more headroom, a raise. ADDING
     * HARDWS_MIN_ENABLE turns an advisory floor into a reserved one -- more
     * memory held for this process, also a raise. Treating both as "clearing
     * is the raise" would let an unprivileged caller pin a reservation. */
    if ((t->quota_policy.flags & QUOTA_LIMITS_HARDWS_MAX_ENABLE) &&
        !(req.Flags & QUOTA_LIMITS_HARDWS_MAX_ENABLE))
        raises = 1;
    if (!(t->quota_policy.flags & QUOTA_LIMITS_HARDWS_MIN_ENABLE) &&
        (req.Flags & QUOTA_LIMITS_HARDWS_MIN_ENABLE))
        raises = 1;
    if (raises && !caller_privileged) {
        st = STATUS_PRIVILEGE_NOT_HELD;
        goto out;
    }

    /* ---- Commit ----
     * Order: RLIMIT_CPU (the only step with an independent authorization rule,
     * so it goes first while nothing is committed) -> pool limits (already
     * range-checked, so quota_set_limit cannot reject them) -> the record
     * (a plain store that cannot fail). Every step keeps its pre-image so an
     * unexpected failure restores rather than leaves a mixture. */
    cpu_want.rlim_cur = want_cpu_secs;
    /* Preserve the HARD limit: the wire struct has one scalar per resource, so
     * a TimeLimit write must not be read as also flattening rlim_max. Raising
     * the soft limit above the hard one is not representable here; clamp so a
     * caller cannot smuggle a hard-limit raise through the soft field. */
    cpu_want.rlim_max = cpu_now.rlim_max;
    if (cpu_want.rlim_max != RLIM_INFINITY &&
        (cpu_want.rlim_cur == RLIM_INFINITY ||
         cpu_want.rlim_cur > cpu_want.rlim_max))
        cpu_want.rlim_cur = cpu_want.rlim_max;
    if (task_rlimit_set(t, RLIMIT_CPU, &cpu_want, caller_privileged)
        != RLIMIT_OK) {
        st = STATUS_PRIVILEGE_NOT_HELD;
        goto out;
    }

    st = quota_set_limit(block, QUOTA_RES_PAGED_POOL, req.PagedPoolLimit);
    if (st != STATUS_SUCCESS) {
        (void)task_rlimit_set(t, RLIMIT_CPU, &cpu_now, 1);
        goto out;
    }
    st = quota_set_limit(block, QUOTA_RES_NONPAGED_POOL,
                         req.NonPagedPoolLimit);
    if (st != STATUS_SUCCESS) {
        (void)quota_set_limit(block, QUOTA_RES_PAGED_POOL, paged_now);
        (void)task_rlimit_set(t, RLIMIT_CPU, &cpu_now, 1);
        goto out;
    }

    saved = t->quota_policy;
    t->quota_policy.min_working_set   = req.MinimumWorkingSetSize;
    t->quota_policy.max_working_set   = req.MaximumWorkingSetSize;
    t->quota_policy.working_set_limit = req.WorkingSetLimit;
    t->quota_policy.pagefile_limit    = req.PagefileLimit;
    t->quota_policy.flags             = req.Flags;
    t->quota_policy.cpu_rate_limit    = req.CpuRateLimit;
    t->quota_policy.generation        = saved.generation + 1;
    st = STATUS_SUCCESS;

out:
    /* ONE exit for the whole transaction: the lock and the block reference are
     * released here on every path, so no early return can leak either. */
    spin_unlock_irqrestore(&t->quota_policy_lock, txn_flags);
    quota_block_deref(block);
    return st;
}

/* ---- Handle-limit reconciliation ----------------------------------------- */

uint64_t quota_policy_effective_handle_limit(struct task *t)
{
    rlimit_t nofile;
    uint64_t from_table;
    uint64_t effective;

    if (!t)
        return QUOTA_HANDLE_LIMIT_UNLIMITED;

    /* handle_limit: 0 means "no quota", everything else is a real cap. Read
     * once -- re-reading for the compare and the result could straddle a
     * concurrent write (the table still has no lock;
     * TODO-05-object-manager.md section 3). */
    from_table = (uint64_t)__atomic_load_n(&t->handle_table.handle_limit,
                                           __ATOMIC_RELAXED);
    if (from_table == HANDLE_TABLE_LIMIT_UNLIMITED)
        from_table = QUOTA_HANDLE_LIMIT_UNLIMITED;

    /* RLIMIT_NOFILE: RLIM_INFINITY means "no quota", and 0 is a REAL cap of
     * zero handles -- the exact inverse of the table's encoding. Translating
     * both into one space before the min() is the whole point of this helper. */
    if (task_rlimit_get(t, RLIMIT_NOFILE, &nofile) != RLIMIT_OK)
        return from_table;
    effective = (nofile.rlim_cur == RLIM_INFINITY)
                    ? QUOTA_HANDLE_LIMIT_UNLIMITED
                    : nofile.rlim_cur;

    return (from_table < effective) ? from_table : effective;
}
