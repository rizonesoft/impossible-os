/* ============================================================================
 * test_quota_syscall.c -- ProcessQuotaLimits ABI, projection, privilege rule,
 * and the job aggregate-limit class (native query/set quota syscalls).
 *
 * These tests exercise the KERNEL side of the syscall: quota_policy_query /
 * quota_policy_set / quota_policy_effective_handle_limit and
 * ob_job_set_quota_limits. The syscall handlers themselves are pure
 * marshalling (probe, bounce-copy, size dispatch) over these calls, and
 * invoking them would need a ring-3 buffer, so the behaviour that can actually
 * be wrong -- the projection, the transaction, the raise rule, the zero
 * inversions -- is asserted here directly.
 *
 * Every test restores whatever it changed on the CURRENT task, because the
 * suite runs inside a live process whose limits other suites also read.
 *
 * XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/quota/quota_policy.h"
#include "kernel/quota/quota.h"
#include "kernel/nt/quota_syscall_info.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/sched/task.h"
#include "kernel/task_limits.h"
#include "kernel/ob/ob_job.h"
#include "kernel/ob/handle_table.h"

/* --- Save/restore of everything a ProcessQuotaLimits set can touch -------- */

typedef struct tqs_saved {
    QUOTA_LIMITS_EX limits;
    rlimit_t        cpu;
    rlimit_t        nofile;
    uint32_t        handle_limit;
    int             valid;
} tqs_saved_t;

static void tqs_save(tqs_saved_t *s)
{
    struct task *t = task_current();
    s->valid = 0;
    if (!t)
        return;
    if (quota_policy_query(t, &s->limits) != STATUS_SUCCESS)
        return;
    if (task_rlimit_get(t, RLIMIT_CPU, &s->cpu) != RLIMIT_OK)
        return;
    if (task_rlimit_get(t, RLIMIT_NOFILE, &s->nofile) != RLIMIT_OK)
        return;
    s->handle_limit = t->handle_table.handle_limit;
    s->valid = 1;
}

static void tqs_restore(const tqs_saved_t *s)
{
    struct task *t = task_current();
    if (!t || !s->valid)
        return;
    /* Privileged restore: putting a limit BACK is a raise, and the tests run
     * as whatever the suite's token is. */
    (void)quota_policy_set(t, &s->limits, 1, 1);
    (void)task_rlimit_set(t, RLIMIT_CPU, &s->cpu, 1);
    (void)task_rlimit_set(t, RLIMIT_NOFILE, &s->nofile, 1);
    t->handle_table.handle_limit = s->handle_limit;
}

static void tqs_zero(QUOTA_LIMITS_EX *q)
{
    uint8_t *p = (uint8_t *)q;
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof(*q); i++)
        p[i] = 0;
}

/* --- ABI ----------------------------------------------------------------- *
 * Layer 3 of the 5-layer defense. The header's _Static_asserts already pin
 * these at compile time; asserting them again here is what catches a header
 * edited without a full rebuild of every consumer. */

static void test_quota_syscall_abi_pinned(void)
{
    TEST_ASSERT_EQ(sizeof(QUOTA_LIMITS), 48, "QUOTA_LIMITS is 48 bytes");
    TEST_ASSERT_EQ(sizeof(QUOTA_LIMITS_EX), 88, "QUOTA_LIMITS_EX is 88 bytes");
    TEST_ASSERT_EQ(__builtin_offsetof(QUOTA_LIMITS, TimeLimit), 40,
                   "QUOTA_LIMITS.TimeLimit at offset 40");
    TEST_ASSERT_EQ(__builtin_offsetof(QUOTA_LIMITS_EX, WorkingSetLimit), 48,
                   "QUOTA_LIMITS_EX.WorkingSetLimit at offset 48");
    TEST_ASSERT_EQ(__builtin_offsetof(QUOTA_LIMITS_EX, Flags), 80,
                   "QUOTA_LIMITS_EX.Flags at offset 80");
    TEST_ASSERT_EQ(__builtin_offsetof(QUOTA_LIMITS_EX, CpuRateLimit), 84,
                   "QUOTA_LIMITS_EX.CpuRateLimit at offset 84");
    TEST_ASSERT_EQ(ProcessQuotaLimits, 1,
                   "ProcessQuotaLimits is PROCESSINFOCLASS 1 (Windows value)");
    TEST_ASSERT_EQ(quota_policy_abi_verify(), 1,
                   "runtime ABI verification agrees with the static asserts");
}

/* --- TimeLimit conversion ------------------------------------------------ */

static void test_quota_syscall_time_conversion(void)
{
    /* Both spellings of "no limit" translate into each other. */
    TEST_ASSERT_EQ(quota_policy_seconds_to_time_limit(RLIM_INFINITY),
                   QUOTA_TIME_LIMIT_NONE, "RLIM_INFINITY becomes TimeLimit 0");
    TEST_ASSERT_EQ(quota_policy_time_limit_to_seconds(QUOTA_TIME_LIMIT_NONE),
                   RLIM_INFINITY, "TimeLimit 0 becomes RLIM_INFINITY");

    TEST_ASSERT_EQ(quota_policy_seconds_to_time_limit(1), 10000000,
                   "1 second is 10^7 100-ns ticks");
    TEST_ASSERT_EQ(quota_policy_time_limit_to_seconds(10000000), 1,
                   "10^7 ticks is 1 second");

    /* Zero seconds is a REAL cap (deny immediately), and the wire's 0 means the
     * opposite (unlimited), so the projection must not collapse them. It
     * reports the smallest representable positive limit instead, and the
     * reverse conversion truncates it back -- the pair round-trips. */
    TEST_ASSERT_EQ(quota_policy_seconds_to_time_limit(0), QUOTA_TIME_LIMIT_MIN,
                   "a 0-second rlimit projects as the tightest cap, not unlimited");
    TEST_ASSERT_EQ(quota_policy_time_limit_to_seconds(QUOTA_TIME_LIMIT_MIN), 0,
                   "the tightest wire cap converts back to a 0-second rlimit");

    /* Sub-second limits TRUNCATE towards zero: rlimit spells "unlimited" only
     * as RLIM_INFINITY, so 0 is a real cap and truncation is the tighter (safe)
     * rounding direction rather than an inversion of the caller's intent. */
    TEST_ASSERT_EQ(quota_policy_time_limit_to_seconds(9999999), 0,
                   "a sub-second limit truncates to a 0-second cap");
    TEST_ASSERT_EQ(quota_policy_time_limit_to_seconds(10000001), 1,
                   "a limit just past 1s truncates to 1 second");

    /* Saturate rather than wrap: a wrapped product reads back negative, and a
     * negative TimeLimit is rejected as invalid. */
    TEST_ASSERT_EQ(quota_policy_seconds_to_time_limit(0xFFFFFFFFFFFFFFFEULL),
                   0x7FFFFFFFFFFFFFFFLL,
                   "an unrepresentable second count saturates to INT64_MAX");
    TEST_ASSERT(quota_policy_seconds_to_time_limit(
                    (uint64_t)QUOTA_TIME_LIMIT_MAX_SEC + 1) > 0,
                "saturation stays positive (a negative would be rejected)");
}

/* --- Projection ---------------------------------------------------------- */

static void test_quota_syscall_projection_reads_real_state(void)
{
    struct task *t = task_current();
    QUOTA_LIMITS_EX q;
    tqs_saved_t saved;

    TEST_ASSERT_NOT_NULL((void *)t, "a current task exists");
    if (!t)
        return;
    tqs_save(&saved);

    /* Pool limits come from the task's OWN process block: set one there and it
     * must appear in the projection, proving nothing is fabricated. */
    if (t->quota) {
        TEST_ASSERT_EQ(quota_set_limit(t->quota, QUOTA_RES_PAGED_POOL, 0x4000),
                       STATUS_SUCCESS, "paged pool limit accepted");
        TEST_ASSERT_EQ(quota_policy_query(t, &q), STATUS_SUCCESS,
                       "query succeeds");
        TEST_ASSERT_EQ(q.PagedPoolLimit, 0x4000,
                       "PagedPoolLimit projects the process block's limit");
    }

    /* TimeLimit comes from RLIMIT_CPU, converted. */
    {
        rlimit_t cpu;
        cpu.rlim_cur = 5;
        cpu.rlim_max = RLIM_INFINITY;
        TEST_ASSERT_EQ(task_rlimit_set(t, RLIMIT_CPU, &cpu, 1), RLIMIT_OK,
                       "RLIMIT_CPU set to 5 seconds");
        TEST_ASSERT_EQ(quota_policy_query(t, &q), STATUS_SUCCESS,
                       "query succeeds after CPU limit");
        TEST_ASSERT_EQ(q.TimeLimit, 50000000,
                       "TimeLimit projects 5 seconds as 5x10^7 ticks");
    }

    /* A NULL output is refused rather than faulted on. */
    TEST_ASSERT_EQ(quota_policy_query(t, (QUOTA_LIMITS_EX *)0),
                   STATUS_INVALID_PARAMETER, "NULL output refused");
    TEST_ASSERT_EQ(quota_policy_query((struct task *)0, &q),
                   STATUS_INVALID_PARAMETER, "NULL task refused");

    tqs_restore(&saved);
}

/* --- Privilege rule ------------------------------------------------------ */

static void test_quota_syscall_raise_needs_privilege(void)
{
    struct task *t = task_current();
    QUOTA_LIMITS_EX q;
    tqs_saved_t saved;

    if (!t || !t->quota)
        return;
    tqs_save(&saved);

    /* Start from a finite cap so both directions are meaningful. */
    tqs_zero(&q);
    q.PagedPoolLimit = 0x8000;
    q.NonPagedPoolLimit = 0x8000;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_SUCCESS,
                   "privileged caller establishes a finite cap");

    /* Lowering the caller's own limit is unprivileged. */
    tqs_zero(&q);
    q.PagedPoolLimit = 0x1000;
    q.NonPagedPoolLimit = 0x8000;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_SUCCESS,
                   "lowering own limit needs no privilege");
    TEST_ASSERT_EQ(quota_limit(t->quota, QUOTA_RES_PAGED_POOL), 0x1000,
                   "the lowered limit committed");

    /* Raising it back is refused without privilege, and nothing moves. */
    tqs_zero(&q);
    q.PagedPoolLimit = 0x8000;
    q.NonPagedPoolLimit = 0x8000;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_PRIVILEGE_NOT_HELD,
                   "raising a limit without privilege is refused");
    TEST_ASSERT_EQ(quota_limit(t->quota, QUOTA_RES_PAGED_POOL), 0x1000,
                   "a refused raise leaves the limit unchanged");

    /* REMOVING a cap (writing 0 = unlimited) is a raise too -- the case a
     * naive "is the number bigger" check would wave straight through. */
    tqs_zero(&q);
    q.PagedPoolLimit = 0;
    q.NonPagedPoolLimit = 0x8000;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_PRIVILEGE_NOT_HELD,
                   "removing a cap by writing 0 counts as a raise");
    TEST_ASSERT_EQ(quota_limit(t->quota, QUOTA_RES_PAGED_POOL), 0x1000,
                   "the cap survives the refused removal");

    tqs_restore(&saved);
}

/* --- Transaction atomicity ----------------------------------------------- */

static void test_quota_syscall_set_is_all_or_nothing(void)
{
    struct task *t = task_current();
    QUOTA_LIMITS_EX q;
    rlimit_t cpu_before, cpu_after;
    tqs_saved_t saved;

    if (!t || !t->quota)
        return;
    tqs_save(&saved);

    tqs_zero(&q);
    q.PagedPoolLimit = 0x8000;
    q.NonPagedPoolLimit = 0x8000;
    q.MaximumWorkingSetSize = 0x20000;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_SUCCESS,
                   "baseline established");
    TEST_ASSERT_EQ(task_rlimit_get(t, RLIMIT_CPU, &cpu_before), RLIMIT_OK,
                   "CPU limit readable");

    /* A request that LOWERS the pool limits but RAISES the working set must be
     * refused WHOLE: the tightening half must not land. This is the partial-
     * commit shape the transaction exists to prevent. */
    tqs_zero(&q);
    q.PagedPoolLimit = 0x2000;          /* lower  */
    q.NonPagedPoolLimit = 0x2000;       /* lower  */
    q.MaximumWorkingSetSize = 0x99000;  /* RAISE  */
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_PRIVILEGE_NOT_HELD,
                   "a mixed lower+raise request is refused as one unit");
    TEST_ASSERT_EQ(quota_limit(t->quota, QUOTA_RES_PAGED_POOL), 0x8000,
                   "the lowering half did NOT land");
    TEST_ASSERT_EQ(quota_policy_query(t, &q), STATUS_SUCCESS, "query ok");
    TEST_ASSERT_EQ(q.MaximumWorkingSetSize, 0x20000,
                   "the raising half did not land either");
    TEST_ASSERT_EQ(task_rlimit_get(t, RLIMIT_CPU, &cpu_after), RLIMIT_OK,
                   "CPU limit still readable");
    TEST_ASSERT_EQ(cpu_after.rlim_cur, cpu_before.rlim_cur,
                   "the refused transaction did not touch RLIMIT_CPU");

    tqs_restore(&saved);
}

static void test_quota_syscall_set_preserves_hard_limit(void)
{
    struct task *t = task_current();
    QUOTA_LIMITS_EX q;
    rlimit_t cpu, after;
    tqs_saved_t saved;

    if (!t)
        return;
    tqs_save(&saved);

    /* The wire struct has ONE scalar per resource, so a TimeLimit write must
     * not be read as also flattening the hard limit. */
    cpu.rlim_cur = 100;
    cpu.rlim_max = 200;
    TEST_ASSERT_EQ(task_rlimit_set(t, RLIMIT_CPU, &cpu, 1), RLIMIT_OK,
                   "CPU soft 100 / hard 200 established");

    tqs_zero(&q);
    q.TimeLimit = quota_policy_seconds_to_time_limit(50);
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_SUCCESS,
                   "lowering the soft CPU limit is unprivileged");
    TEST_ASSERT_EQ(task_rlimit_get(t, RLIMIT_CPU, &after), RLIMIT_OK,
                   "CPU limit readable");
    TEST_ASSERT_EQ(after.rlim_cur, 50, "the soft limit moved");
    TEST_ASSERT_EQ(after.rlim_max, 200,
                   "the HARD limit is preserved, not flattened by the wire value");

    /* A soft limit above the hard cap is clamped, never smuggled through. */
    tqs_zero(&q);
    q.TimeLimit = quota_policy_seconds_to_time_limit(5000);
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_SUCCESS,
                   "privileged raise accepted");
    TEST_ASSERT_EQ(task_rlimit_get(t, RLIMIT_CPU, &after), RLIMIT_OK,
                   "CPU limit readable after raise");
    TEST_ASSERT_EQ(after.rlim_cur, 200,
                   "a soft value above the hard cap is clamped to the cap");
    TEST_ASSERT_EQ(after.rlim_max, 200, "the hard cap itself is untouched");

    tqs_restore(&saved);
}

/* --- Validation ---------------------------------------------------------- */

static void test_quota_syscall_set_rejects_malformed(void)
{
    struct task *t = task_current();
    QUOTA_LIMITS_EX q;
    tqs_saved_t saved;

    if (!t || !t->quota)
        return;
    tqs_save(&saved);

    tqs_zero(&q);
    q.TimeLimit = -1;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_INVALID_PARAMETER,
                   "a negative TimeLimit is refused");

    tqs_zero(&q);
    q.MinimumWorkingSetSize = 0x8000;
    q.MaximumWorkingSetSize = 0x1000;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_INVALID_PARAMETER,
                   "min working set above max is refused");

    tqs_zero(&q);
    q.Flags = QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MIN_DISABLE;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_INVALID_PARAMETER,
                   "ENABLE and DISABLE of one knob is a contradiction");

    tqs_zero(&q);
    q.Flags = 0x80000000u;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_INVALID_PARAMETER,
                   "an unknown flag bit is rejected, not swallowed");

    tqs_zero(&q);
    q.Reserved3 = 1;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_INVALID_PARAMETER,
                   "a non-zero Reserved field is rejected");

    tqs_zero(&q);
    q.CpuRateLimit = RATE_QUOTA_PERCENT_MAX + 1;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_INVALID_PARAMETER,
                   "a CPU rate above 100 percent is rejected");

    tqs_zero(&q);
    q.PagedPoolLimit = (uint64_t)QUOTA_AMOUNT_MAX + 1;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_INVALID_PARAMETER,
                   "a pool limit past the counter range is rejected");

    TEST_ASSERT_EQ(quota_policy_set(t, (const QUOTA_LIMITS_EX *)0, 1, 1),
                   STATUS_INVALID_PARAMETER, "a NULL request is refused");

    tqs_restore(&saved);
}

static void test_quota_syscall_non_ex_ignores_suffix(void)
{
    struct task *t = task_current();
    QUOTA_LIMITS_EX q;
    tqs_saved_t saved;

    if (!t || !t->quota)
        return;
    tqs_save(&saved);

    /* Establish a suffix policy through the EX form first, so the base-form
     * call below has something it could destroy. */
    tqs_zero(&q);
    q.CpuRateLimit = 25;
    q.Flags = QUOTA_LIMITS_HARDWS_MAX_ENABLE;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_SUCCESS,
                   "EX caller establishes the suffix policy");

    /* A 48-byte caller supplies no suffix. Junk in the suffix of the kernel's
     * copy must be IGNORED -- neither validated (a short call cannot be
     * rejected for bytes it never sent) nor stored, and the established suffix
     * policy must SURVIVE rather than being silently cleared. */
    tqs_zero(&q);
    q.Flags = 0x80000000u;          /* would be rejected under is_ex */
    q.Reserved2 = 0xDEADBEEF;       /* would be rejected under is_ex */
    q.CpuRateLimit = 0xFFFFFFFFu;   /* would be rejected under is_ex */
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 0, 1), STATUS_SUCCESS,
                   "a non-EX request ignores the suffix rather than failing on it");

    TEST_ASSERT_EQ(quota_policy_query(t, &q), STATUS_SUCCESS, "query ok");
    TEST_ASSERT_EQ(q.Flags, QUOTA_LIMITS_HARDWS_MAX_ENABLE,
                   "the base-form write preserved the suffix flags");
    TEST_ASSERT_EQ(q.CpuRateLimit, 25,
                   "the base-form write preserved the suffix rate");

    /* And because the suffix is carried forward rather than zeroed, a base-form
     * request that only TIGHTENS is not misread as a widening. */
    tqs_zero(&q);
    q.PagefileLimit = 0x2000;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 0, 0), STATUS_SUCCESS,
                   "an unprivileged base-form tightening is not blocked by the absent suffix");

    tqs_restore(&saved);
}

/* --- Handle-limit reconciliation ----------------------------------------- */

static void test_quota_syscall_handle_limit_reconcile(void)
{
    struct task *t = task_current();
    rlimit_t nofile;
    tqs_saved_t saved;

    if (!t)
        return;
    tqs_save(&saved);

    /* Both unlimited, spelled INVERSELY on the two sides. */
    nofile.rlim_cur = RLIM_INFINITY;
    nofile.rlim_max = RLIM_INFINITY;
    TEST_ASSERT_EQ(task_rlimit_set(t, RLIMIT_NOFILE, &nofile, 1), RLIMIT_OK,
                   "NOFILE set to infinity");
    t->handle_table.handle_limit = HANDLE_TABLE_LIMIT_UNLIMITED;
    TEST_ASSERT_EQ(quota_policy_effective_handle_limit(t),
                   QUOTA_HANDLE_LIMIT_UNLIMITED,
                   "both unlimited reconciles to unlimited");

    /* The table caps, the rlimit does not: the table's cap binds. */
    t->handle_table.handle_limit = 500;
    TEST_ASSERT_EQ(quota_policy_effective_handle_limit(t), 500,
                   "a table cap binds when the rlimit is infinite");

    /* The rlimit is tighter: it binds. */
    nofile.rlim_cur = 100;
    nofile.rlim_max = RLIM_INFINITY;
    TEST_ASSERT_EQ(task_rlimit_set(t, RLIMIT_NOFILE, &nofile, 1), RLIMIT_OK,
                   "NOFILE lowered to 100");
    TEST_ASSERT_EQ(quota_policy_effective_handle_limit(t), 100,
                   "the tighter of the two binds");

    /* The inversion itself: RLIMIT_NOFILE 0 is a REAL cap of zero handles,
     * while handle_limit 0 means no enforcement. A naive min() over the raw
     * words would report this deny-all rlimit as unlimited. */
    nofile.rlim_cur = 0;
    nofile.rlim_max = RLIM_INFINITY;
    TEST_ASSERT_EQ(task_rlimit_set(t, RLIMIT_NOFILE, &nofile, 1), RLIMIT_OK,
                   "NOFILE lowered to 0");
    t->handle_table.handle_limit = HANDLE_TABLE_LIMIT_UNLIMITED;
    TEST_ASSERT_EQ(quota_policy_effective_handle_limit(t), 0,
                   "a zero rlimit is a deny-all cap, not 'unlimited'");

    TEST_ASSERT_EQ(quota_policy_effective_handle_limit((struct task *)0),
                   QUOTA_HANDLE_LIMIT_UNLIMITED, "a NULL task reports unlimited");

    tqs_restore(&saved);
}

/* --- Slot reuse ---------------------------------------------------------- */

static void test_quota_syscall_reset_clears_policy(void)
{
    struct task *t = task_current();
    QUOTA_LIMITS_EX q;
    tqs_saved_t saved;

    if (!t)
        return;
    tqs_save(&saved);

    tqs_zero(&q);
    q.MaximumWorkingSetSize = 0x30000;
    q.PagefileLimit = 0x40000;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_SUCCESS,
                   "policy established");

    /* A recycled task slot must not present the dead tenant's limits. */
    quota_policy_reset(t);
    TEST_ASSERT_EQ(quota_policy_query(t, &q), STATUS_SUCCESS, "query ok");
    TEST_ASSERT_EQ(q.MaximumWorkingSetSize, 0,
                   "reset clears the working-set cap");
    TEST_ASSERT_EQ(q.PagefileLimit, 0, "reset clears the pagefile limit");
    TEST_ASSERT_EQ(t->quota_policy.generation, 0, "reset zeroes the generation");

    tqs_restore(&saved);
}

static void test_quota_syscall_generation_advances_on_commit(void)
{
    struct task *t = task_current();
    QUOTA_LIMITS_EX q;
    uint64_t gen0;
    tqs_saved_t saved;

    if (!t || !t->quota)
        return;
    tqs_save(&saved);

    tqs_zero(&q);
    q.PagefileLimit = 0x10000;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_SUCCESS, "first set");
    gen0 = t->quota_policy.generation;

    tqs_zero(&q);
    q.PagefileLimit = 0x8000;   /* a lowering: unprivileged, must commit */
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_SUCCESS, "second set");
    TEST_ASSERT_EQ(t->quota_policy.generation, gen0 + 1,
                   "the generation advances once per committed set");

    tqs_zero(&q);
    q.PagefileLimit = 0;        /* removing the cap: refused unprivileged */
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_PRIVILEGE_NOT_HELD,
                   "refused set");
    TEST_ASSERT_EQ(t->quota_policy.generation, gen0 + 1,
                   "a refused set does NOT advance the generation");

    tqs_restore(&saved);
}

/* --- Job aggregate limits ------------------------------------------------ */

static void test_quota_syscall_job_report_shape(void)
{
    JOBOBJECT_QUOTA_LIMIT_INFORMATION q;
    uint32_t i;

    /* A NULL job must still produce a well-formed, fully-zeroed report rather
     * than leaving the caller's buffer untouched. */
    for (i = 0; i < (uint32_t)sizeof(q); i++)
        ((uint8_t *)&q)[i] = 0xAA;
    ob_job_collect_quota_limits((JOB_OBJECT *)0, &q);
    TEST_ASSERT_EQ(q.ResourceCount, quota_resource_type_count(),
                   "the report names one row per registered resource type");
    TEST_ASSERT_EQ(q.Reserved, 0, "Reserved is zeroed");
    for (i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        TEST_ASSERT_EQ(q.Resources[i].Usage, 0, "no-job usage row is zero");
        TEST_ASSERT_EQ(q.Resources[i].Limit, 0, "no-job limit row is zero");
    }
}

static void test_quota_syscall_job_set_rejects_malformed(void)
{
    JOBOBJECT_QUOTA_LIMIT_INFORMATION q;
    uint32_t i;

    for (i = 0; i < (uint32_t)sizeof(q); i++)
        ((uint8_t *)&q)[i] = 0;

    TEST_ASSERT_EQ(ob_job_set_quota_limits((JOB_OBJECT *)0, &q, 1),
                   STATUS_INVALID_PARAMETER, "a NULL job is refused");

    /* The kernel-owned columns are inputs to nothing: supplying them is a
     * caller error, not a value to ignore -- and it is diagnosed as malformed
     * before the job's block state can produce a different answer. */
    {
        JOB_OBJECT job = {0};
        q.ResourceCount = 1;
        q.Resources[0].Usage = 1;
        TEST_ASSERT_EQ(ob_job_set_quota_limits(&job, &q, 1),
                       STATUS_INVALID_PARAMETER,
                       "a request writing the Usage column is refused");
    }
}

static void test_quota_syscall_job_set_always_privileged(void)
{
    JOBOBJECT_QUOTA_LIMIT_INFORMATION q;
    JOB_OBJECT job = {0};   /* no quota block: nothing can be committed */
    uint32_t i;

    for (i = 0; i < (uint32_t)sizeof(q); i++)
        ((uint8_t *)&q)[i] = 0;
    q.ResourceCount = 1;
    q.Resources[0].Limit = 4096;   /* a LOWERING from unlimited */

    /* Job handles carry no granted-access mask yet, so unprivileged lowering
     * would let any opener of a shared named job squeeze every member. The
     * authorization verdict therefore precedes the quota block entirely: the
     * unprivileged caller is refused for lack of privilege, while the same
     * request WITH privilege gets as far as the missing block. */
    TEST_ASSERT_EQ(ob_job_set_quota_limits(&job, &q, 0),
                   STATUS_PRIVILEGE_NOT_HELD,
                   "an unprivileged job quota write is refused even when lowering");
    TEST_ASSERT_EQ(ob_job_set_quota_limits(&job, &q, 1),
                   STATUS_INSUFFICIENT_RESOURCES,
                   "the privileged caller passes authorization and finds no block");
}

static void test_quota_syscall_job_wire_size_frozen(void)
{
    /* The wire size is FROZEN at V1, not derived from the internal resource
     * enum: a new resource type must not move this information class's
     * required buffer length under existing callers. */
    TEST_ASSERT_EQ(sizeof(JOBOBJECT_QUOTA_LIMIT_INFORMATION), 520,
                   "V1 wire size is frozen at 520 bytes");
    TEST_ASSERT(quota_resource_type_count() <= JOB_QUOTA_V1_RESOURCE_COUNT,
                "the registered resource types still fit inside V1");
}

static void test_quota_syscall_suffix_widening_needs_privilege(void)
{
    struct task *t = task_current();
    QUOTA_LIMITS_EX q;
    tqs_saved_t saved;

    if (!t || !t->quota)
        return;
    tqs_save(&saved);

    /* Establish an enforced 10% CPU rate cap with a hard working-set ceiling. */
    tqs_zero(&q);
    q.CpuRateLimit = 10;
    q.Flags = QUOTA_LIMITS_HARDWS_MAX_ENABLE;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_SUCCESS,
                   "privileged caller establishes the suffix caps");

    /* Raising the rate percentage is a raise even though no numeric limit
     * moved -- it hands the process more CPU. */
    tqs_zero(&q);
    q.CpuRateLimit = 100;
    q.Flags = QUOTA_LIMITS_HARDWS_MAX_ENABLE;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_PRIVILEGE_NOT_HELD,
                   "raising the CPU rate percentage without privilege is refused");
    TEST_ASSERT_EQ(t->quota_policy.cpu_rate_limit, 10,
                   "the refused raise left the rate cap in place");

    /* Clearing the rate word removes the cap entirely: the same removal that a
     * zero pool limit is. */
    tqs_zero(&q);
    q.Flags = QUOTA_LIMITS_HARDWS_MAX_ENABLE;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_PRIVILEGE_NOT_HELD,
                   "removing the CPU rate cap without privilege is refused");

    /* Dropping the MAX enforcement flag converts a hard ceiling into an
     * advisory one -- a widening with no number attached. */
    tqs_zero(&q);
    q.CpuRateLimit = 10;
    q.Flags = 0;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_PRIVILEGE_NOT_HELD,
                   "dropping the hard-max flag without privilege is refused");
    TEST_ASSERT_EQ(t->quota_policy.flags, QUOTA_LIMITS_HARDWS_MAX_ENABLE,
                   "the refused flag drop left enforcement on");

    /* The MIN flag runs the OTHER way: enabling it pins a reservation, so
     * ADDING it is the privileged move and dropping it is free. */
    tqs_zero(&q);
    q.CpuRateLimit = 10;
    q.Flags = QUOTA_LIMITS_HARDWS_MAX_ENABLE | QUOTA_LIMITS_HARDWS_MIN_ENABLE;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_PRIVILEGE_NOT_HELD,
                   "enabling the hard-min floor without privilege is refused");
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_SUCCESS,
                   "the privileged caller may enable the hard-min floor");
    tqs_zero(&q);
    q.CpuRateLimit = 10;
    q.Flags = QUOTA_LIMITS_HARDWS_MAX_ENABLE;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_SUCCESS,
                   "dropping the hard-min floor needs no privilege");

    /* Tightening the rate stays unprivileged. */
    tqs_zero(&q);
    q.CpuRateLimit = 5;
    q.Flags = QUOTA_LIMITS_HARDWS_MAX_ENABLE;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_SUCCESS,
                   "lowering the CPU rate percentage needs no privilege");

    /* MinimumWorkingSetSize is a FLOOR: raising it reserves more resident
     * memory, so it is a widening even though every ceiling stayed put. */
    tqs_zero(&q);
    q.MinimumWorkingSetSize = 0x100000;
    q.CpuRateLimit = 5;
    q.Flags = QUOTA_LIMITS_HARDWS_MAX_ENABLE;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_PRIVILEGE_NOT_HELD,
                   "raising the working-set floor without privilege is refused");
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 1), STATUS_SUCCESS,
                   "the privileged caller may raise the floor");
    tqs_zero(&q);
    q.CpuRateLimit = 5;
    q.Flags = QUOTA_LIMITS_HARDWS_MAX_ENABLE;
    TEST_ASSERT_EQ(quota_policy_set(t, &q, 1, 0), STATUS_SUCCESS,
                   "dropping the floor back to zero needs no privilege");

    tqs_restore(&saved);
}

/* --- Registration -------------------------------------------------------- */

void test_register_quota_syscall(void)
{
    test_suite_register_cat("Quota syscall: job quota writes need privilege",
                            test_quota_syscall_job_set_always_privileged,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: job report wire size frozen at V1",
                            test_quota_syscall_job_wire_size_frozen,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: suffix widening needs privilege",
                            test_quota_syscall_suffix_widening_needs_privilege,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: QUOTA_LIMITS ABI pinned",
                            test_quota_syscall_abi_pinned, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: TimeLimit conversion total",
                            test_quota_syscall_time_conversion, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: projection reads real state",
                            test_quota_syscall_projection_reads_real_state,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: raising a limit needs privilege",
                            test_quota_syscall_raise_needs_privilege, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: set is all-or-nothing",
                            test_quota_syscall_set_is_all_or_nothing, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: set preserves the hard limit",
                            test_quota_syscall_set_preserves_hard_limit, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: malformed requests refused",
                            test_quota_syscall_set_rejects_malformed, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: non-EX request ignores the suffix",
                            test_quota_syscall_non_ex_ignores_suffix, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: handle-limit inversion reconciled",
                            test_quota_syscall_handle_limit_reconcile, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: reset clears a reused slot's policy",
                            test_quota_syscall_reset_clears_policy, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: generation advances only on commit",
                            test_quota_syscall_generation_advances_on_commit,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: job aggregate report shape",
                            test_quota_syscall_job_report_shape, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota syscall: job aggregate set refuses malformed",
                            test_quota_syscall_job_set_rejects_malformed,
                            TEST_CAT_QUOTA);
}

#endif /* KERNEL_TESTS */
