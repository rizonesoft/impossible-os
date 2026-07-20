/* ============================================================================
 * test_quota_perf.c -- Charge-path cost characterization for the quota API
 *
 * Section 5 coverage: the structural cost budget every hot-path consumer has to
 * live within, plus the counter-record locality the pool work depends on.
 *
 * Two KINDS of check live here, and the distinction is deliberate:
 *
 *   ENFORCED (asserted): the number of critical sections an operation enters,
 *   counting BOTH the block locks in quota.c and the owner-side locks in
 *   quota_owner.c. That number is exact, identical on TCG, KVM, WHPX and bare
 *   metal, and is what silently regresses when someone adds a lock to the
 *   charge path. Budgets are asserted for EQUALITY, not as upper bounds, so a
 *   lock that DISAPPEARS is caught too -- a charge that stopped locking would
 *   otherwise read as an improvement.
 *
 *   ADVISORY (reported, never asserted): TSC cycle counts. A wall-clock
 *   threshold would have to hold across platforms that differ by more than an
 *   order of magnitude, so it would either fail on the slow one or be widened
 *   until it accepted every answer. CLAUDE.md forbids that widening, so the
 *   numbers are logged for a human to read and nothing here fails on them.
 *   The instrumentation is left DISARMED during timing so the loops measure the
 *   production path rather than the counter.
 *
 * Every balance assertion is BASELINE-RELATIVE, never "usage == 0": these
 * suites charge the live current task, whose blocks may legitimately already
 * carry usage once pool integration lands. Asserting zero would pass today and
 * fail later for a correct implementation.
 *
 * XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md section 5
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/quota/quota.h"
#include "kernel/sched/task.h"    /* task_current for the chain-depth budget */
#if defined(__x86_64__)
/* The ONLY arch dependency in this file, and it is confined to the two
 * advisory timing suites. The enforced structural budgets are arch-neutral, so
 * a future ARM64 port drops the timing legs rather than failing to compile. */
#include "kernel/cpuid.h"         /* cpu_has / rdtscp_read (advisory timing) */
#define QUOTA_PERF_HAVE_TSC 1
#endif
#include "kernel/klog.h"

/* Iterations behind one advisory measurement. Large enough that the timer's
 * granularity is not the dominant term, small enough that the suite stays
 * quick under TCG (where every instruction is emulated). */
#define QUOTA_PERF_ITERATIONS  1000u

/* Cache line this test assumes, pinned INDEPENDENTLY of the implementation.
 * The accessor is cross-checked against it below rather than trusted: reading
 * the implementation's own value and then validating the implementation with
 * it is a tautology -- if the production bound drifted from the real x86-64
 * line size, accessor and test would drift together and still pass. */
#define QUOTA_PERF_LINE_BYTES  64u

/* Byte offset of the counter array inside quota_block. Pinned INDEPENDENTLY of
 * the implementation so a struct-prefix change is an explicit decision: without
 * it the locality check reads the production offset and compares it to itself,
 * which cannot fail. Moving the array changes which records share a line, so a
 * reorder must re-derive the locality claim rather than silently inherit it. */
#define QUOTA_PERF_COUNTER_BASE  24u

/* Advisory numbers are logged under the TEST tag, NOT under a quota-specific
 * one. A `test=1` boot clamps every subsystem to LOG_WARN for the duration of
 * the sweep and grants a LOG_DEBUG override to exactly TEST, UTEST, and DTEST
 * (src/kernel/main/boot_tests.c). A measurement logged under any other tag is
 * therefore emitted into a void -- which is precisely the bug that once ate 17
 * DTEST assertion lines silently. */
#define QUOTA_PERF_LOG_TAG  "TEST"

/* Restores a quota limit that a suite temporarily changed on a SHARED block.
 * Registered with test_add_action so it runs even when a non-fatal assertion
 * fails partway through and the suite body falls through its own restore. */
static struct {
    quota_block_t         *block;
    quota_resource_type_t  type;
    uint64_t               limit;
} s_limit_restore;

static void quota_perf_restore_limit(void *ctx)
{
    typeof(s_limit_restore) *r = ctx;
    if (r && r->block)
        (void)quota_set_limit(r->block, r->type, r->limit);
}

/* --- Enforced: single-block lock budgets ---------------------------------- */

/* Charge, return, and their refusal paths each enter exactly one section, and
 * an argument rejection enters none. The refusal cases matter most: a refused
 * charge must record its failure counter inside the SAME section that judged
 * it, or the refusal stops being atomic with the check. */
static void test_quota_charge_lock_budget(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    quota_test_lock_count_begin();
    NTSTATUS st = quota_charge(b, QUOTA_RES_NONPAGED_POOL, 4096);
    uint64_t n = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "charge admitted");
    TEST_ASSERT_EQ(n, (uint64_t)QUOTA_BUDGET_CHARGE_LOCKS,
                   "an admitted charge enters exactly QUOTA_BUDGET_CHARGE_LOCKS sections");

    quota_test_lock_count_begin();
    st = quota_return(b, QUOTA_RES_NONPAGED_POOL, 4096);
    n  = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "return accepted");
    TEST_ASSERT_EQ(n, (uint64_t)QUOTA_BUDGET_RETURN_LOCKS,
                   "a return enters exactly QUOTA_BUDGET_RETURN_LOCKS sections");

    /* Over-return fails closed, and must do so within its one section. */
    quota_test_lock_count_begin();
    st = quota_return(b, QUOTA_RES_NONPAGED_POOL, 1);
    n  = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INTEGER_OVERFLOW,
                   "over-return refused");
    TEST_ASSERT_EQ(n, (uint64_t)QUOTA_BUDGET_RETURN_LOCKS,
                   "a refused over-return still costs exactly one section");

    /* Refusal path: cap the type, then charge past it. */
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(b, QUOTA_RES_PAGED_POOL, 100),
                   (uint64_t)STATUS_SUCCESS, "limit set");
    quota_test_lock_count_begin();
    st = quota_charge(b, QUOTA_RES_PAGED_POOL, 101);
    n  = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "over-limit charge refused");
    TEST_ASSERT_EQ(n, (uint64_t)QUOTA_BUDGET_CHARGE_LOCKS,
                   "a REFUSED charge costs the same one section, not two");

    /* set_limit shares the block lock with charge, which is what makes a
     * lowering atomic against an in-flight charge. */
    quota_test_lock_count_begin();
    st = quota_set_limit(b, QUOTA_RES_PAGED_POOL, QUOTA_LIMIT_UNLIMITED);
    n  = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "limit restored");
    TEST_ASSERT_EQ(n, (uint64_t)QUOTA_BUDGET_SET_LIMIT_LOCKS,
                   "set_limit enters exactly QUOTA_BUDGET_SET_LIMIT_LOCKS sections");

    /* Everything rejected on arguments alone must cost nothing: these are the
     * paths a hot caller hits most often when it passes a bad type or a
     * no-op amount, and they must not touch the lock at all. */
    quota_test_lock_count_begin();
    NTSTATUS bad_type   = quota_charge(b, QUOTA_RESOURCE_TYPE_COUNT, 1);
    NTSTATUS zero_chg   = quota_charge(b, QUOTA_RES_NONPAGED_POOL, 0);
    NTSTATUS zero_ret   = quota_return(b, QUOTA_RES_NONPAGED_POOL, 0);
    NTSTATUS null_block = quota_charge(NULL, QUOTA_RES_NONPAGED_POOL, 1);
    n = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)bad_type, (uint64_t)STATUS_INVALID_PARAMETER,
                   "bad type rejected");
    TEST_ASSERT_EQ((uint64_t)zero_chg, (uint64_t)STATUS_SUCCESS,
                   "zero-amount charge is a no-op success");
    TEST_ASSERT_EQ((uint64_t)zero_ret, (uint64_t)STATUS_SUCCESS,
                   "zero-amount return is a no-op success");
    TEST_ASSERT_EQ((uint64_t)null_block, (uint64_t)STATUS_INVALID_PARAMETER,
                   "NULL block rejected");
    TEST_ASSERT_EQ(n, 0ULL,
                   "argument rejections and no-ops take no lock at all");

    quota_block_deref(b);
}

/* A transfer holds BOTH blocks for the whole move, so it enters exactly two
 * sections -- never four (lock, release, lock, release would let an observer
 * see the amount in neither block or in both). A REFUSED transfer costs the
 * same two, while the shapes rejected on arguments cost none. */
static void test_quota_transfer_lock_budget(void)
{
    quota_block_t *src = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    quota_block_t *dst = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)src, "source block allocated");
    TEST_ASSERT_NOT_NULL((void *)dst, "destination block allocated");
    if (!src || !dst) {
        if (src) quota_block_deref(src);
        if (dst) quota_block_deref(dst);
        return;
    }

    TEST_ASSERT_EQ((uint64_t)quota_charge(src, QUOTA_RES_NONPAGED_POOL, 512),
                   (uint64_t)STATUS_SUCCESS, "source seeded");

    quota_test_lock_count_begin();
    NTSTATUS st = quota_try_transfer(src, dst, QUOTA_RES_NONPAGED_POOL, 512);
    uint64_t n  = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "transfer succeeds");
    TEST_ASSERT_EQ(n, (uint64_t)QUOTA_BUDGET_TRANSFER_LOCKS,
                   "a transfer enters exactly QUOTA_BUDGET_TRANSFER_LOCKS sections");

    /* Source is empty now, so this one is refused -- under both locks, because
     * the shortfall can only be judged with the pair held. */
    quota_test_lock_count_begin();
    st = quota_try_transfer(src, dst, QUOTA_RES_NONPAGED_POOL, 512);
    n  = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "short source refused");
    TEST_ASSERT_EQ(n, (uint64_t)QUOTA_BUDGET_TRANSFER_LOCKS,
                   "a refused transfer still costs exactly two sections");

    /* Self-transfer and zero-amount are resolved before any lock. */
    quota_test_lock_count_begin();
    NTSTATUS self = quota_try_transfer(src, src, QUOTA_RES_NONPAGED_POOL, 1);
    NTSTATUS zero = quota_try_transfer(src, dst, QUOTA_RES_NONPAGED_POOL, 0);
    n = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)self, (uint64_t)STATUS_SUCCESS,
                   "self-transfer is a no-op success");
    TEST_ASSERT_EQ((uint64_t)zero, (uint64_t)STATUS_SUCCESS,
                   "zero-amount transfer is a no-op success");
    TEST_ASSERT_EQ(n, 0ULL, "no-op transfers take no lock");

    /* Return what the transfer moved so the fixture leaves nothing charged. */
    TEST_ASSERT_EQ((uint64_t)quota_return(dst, QUOTA_RES_NONPAGED_POOL, 512),
                   (uint64_t)STATUS_SUCCESS, "destination drained");

    quota_block_deref(src);
    quota_block_deref(dst);
}

/* --- Enforced: end-to-end chain budget ------------------------------------ */

/* A chain charge pays the owner-side snapshot sections PLUS one block section
 * per charged layer; its return pays only the block sections, because the
 * receipt already holds a reference to every block it touches.
 *
 * The depth is read from the receipt rather than assumed, so the budget still
 * holds once nested jobs add layers. */
static void test_quota_chain_lock_budget(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0;
    uint64_t charge_sections, return_sections, depth;
    uint64_t proc_before, user_before;

    if (!t || !t->quota || !t->quota_user) {
        TEST_SKIP("current task carries no quota blocks");
        return;
    }

    proc_before = quota_usage(t->quota, QUOTA_RES_NONPAGED_POOL);
    user_before = quota_usage(t->quota_user, QUOTA_RES_NONPAGED_POOL);

    quota_test_lock_count_begin();
    NTSTATUS st = quota_charge_chain(t, QUOTA_RES_NONPAGED_POOL, 64, 0, &r, &tok);
    charge_sections = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "chain charge admitted");

    depth = (uint64_t)r.count;
    TEST_ASSERT(depth >= 2, "chain reaches at least the process and user layers");
    TEST_ASSERT_EQ(charge_sections,
                   (uint64_t)QUOTA_BUDGET_CHAIN_OWNER_LOCKS +
                   depth * (uint64_t)QUOTA_BUDGET_CHAIN_LOCKS_PER_LAYER,
                   "a chain charge costs the owner snapshot plus one section per layer");

    quota_test_lock_count_begin();
    quota_return_chain(&r, tok);
    return_sections = quota_test_lock_count_end();
    TEST_ASSERT_EQ(return_sections, depth * (uint64_t)QUOTA_BUDGET_CHAIN_LOCKS_PER_LAYER,
                   "a chain return costs one section per charged layer and no owner locks");
    TEST_ASSERT_EQ((uint64_t)r.count, 0ULL, "receipt emptied by the return");

    /* Both layers restored to where they started -- not to zero. */
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NONPAGED_POOL), proc_before,
                   "process layer restored to its baseline");
    TEST_ASSERT_EQ(quota_usage(t->quota_user, QUOTA_RES_NONPAGED_POOL), user_before,
                   "user layer restored to its baseline");

    /* A zero-amount chain charge changes no counter, but it is NOT free: it
     * still takes one owner section to confirm the task could have been
     * charged, so a charge against a dying task is refused rather than
     * silently succeeding. Asserted so that owner-side cost cannot be added
     * to (or removed from) the no-op path unnoticed. */
    quota_charge_receipt_t zr = { 0 };
    uint64_t ztok = 0;
    quota_test_lock_count_begin();
    NTSTATUS zst = quota_charge_chain(t, QUOTA_RES_NONPAGED_POOL, 0, 0, &zr, &ztok);
    uint64_t zero_sections = quota_test_lock_count_end();
    TEST_ASSERT_EQ((uint64_t)zst, (uint64_t)STATUS_SUCCESS,
                   "zero-amount chain charge succeeds");
    TEST_ASSERT_EQ(zero_sections, (uint64_t)QUOTA_BUDGET_CHAIN_ZERO_OWNER_LOCKS,
                   "a zero-amount chain charge costs exactly one owner section");
    TEST_ASSERT_EQ((uint64_t)zr.count, 0ULL, "zero-amount charge records no blocks");

    /* A stale token cannot re-enter any critical section. */
    quota_test_lock_count_begin();
    quota_return_chain(&r, tok);
    uint64_t stale_sections = quota_test_lock_count_end();
    TEST_ASSERT_EQ(stale_sections, 0ULL,
                   "a stale-token return takes no lock at all");

    klog(LOG_INFO, QUOTA_PERF_LOG_TAG,
         "chain depth %u: charge %u sections, return %u sections",
         depth, charge_sections, return_sections);
}

/* A chain refused partway through must roll the charged prefix back, and that
 * rollback is itself lock traffic the budget has to see. Capping the USER layer
 * makes the process charge succeed and the user charge fail. */
static void test_quota_chain_rollback_lock_budget(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0;
    uint64_t proc_before, user_before;

    if (!t || !t->quota || !t->quota_user) {
        TEST_SKIP("current task carries no quota blocks");
        return;
    }

    /* This mutates the SHARED canonical user block, so the prior limit must be
     * saved and put back EXACTLY -- restoring "unlimited" would silently delete
     * a real cap for the rest of the boot. The restore is registered as a
     * cleanup action FIRST, because TEST_ASSERT is non-fatal: a failing
     * assertion below falls through rather than unwinding, so a restore written
     * only as a trailing statement is not guaranteed to run. Same save/restore
     * discipline as the all-or-nothing suite in test_quota.c. */
    const quota_resource_type_t type = QUOTA_RES_CRASH_BUFFER;
    proc_before = quota_usage(t->quota, type);
    user_before = quota_usage(t->quota_user, type);
    s_limit_restore.block = t->quota_user;
    s_limit_restore.type  = type;
    s_limit_restore.limit = quota_limit(t->quota_user, type);
    TEST_ASSERT_EQ((uint64_t)(int64_t)test_add_action(quota_perf_restore_limit,
                                                      &s_limit_restore), 0ULL,
                   "limit restore registered before the shared block is mutated");

    /* Cap just ABOVE current usage, never AT it: QUOTA_LIMIT_UNLIMITED is 0, so
     * a cap computed as "exactly the current usage" degenerates into "no cap at
     * all" whenever that usage is 0 -- and the charge under test then succeeds,
     * silently testing nothing. +1 keeps the cap real at every baseline, and is
     * refused outright at the top of the domain, so skip rather than proceed
     * with a cap that was never installed. */
    if (user_before >= (uint64_t)QUOTA_AMOUNT_MAX) {
        TEST_SKIP("user layer usage leaves no headroom for a temporary cap");
        return;
    }
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(t->quota_user, type, user_before + 1),
                   (uint64_t)STATUS_SUCCESS, "user layer capped just above its usage");

    quota_test_lock_count_begin();
    NTSTATUS st = quota_charge_chain(t, type, 16, 0, &r, &tok);
    uint64_t n = quota_test_lock_count_end();

    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "chain refused by the capped user layer");
    /* Owner snapshot + one attempt per layer up to and including the refusal
     * + one return per already-charged layer being unwound. With the process
     * layer charged and the user layer refusing that is 2 attempts and 1
     * rollback, each one per-layer section. */
    TEST_ASSERT_EQ(n, (uint64_t)QUOTA_BUDGET_CHAIN_OWNER_LOCKS +
                      3ULL * (uint64_t)QUOTA_BUDGET_CHAIN_LOCKS_PER_LAYER,
                   "a mid-chain refusal costs the attempts plus the prefix rollback");
    TEST_ASSERT_EQ((uint64_t)r.count, 0ULL, "refused chain records no blocks");

    /* Nothing stranded on either layer. */
    TEST_ASSERT_EQ(quota_usage(t->quota, type), proc_before,
                   "process layer rolled back to its baseline");
    TEST_ASSERT_EQ(quota_usage(t->quota_user, type), user_before,
                   "user layer never charged");

}

/* --- Enforced: counter-record locality ------------------------------------ */

/* One resource type's four counters live in one contiguous record, so a charge
 * touches 32 contiguous bytes instead of four locations up to 112 bytes apart.
 *
 * The claim asserted here is the HONEST one: a record spans at most two cache
 * lines. It is deliberately not "exactly one" -- blocks come from kmalloc,
 * which guarantees only 16-byte alignment, so a no-straddle guarantee would be
 * asserting a property the allocator does not provide. The single-line count is
 * reported so the benefit of a future aligned pool allocation is measurable.
 */
static void test_quota_counter_record_locality(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    uint32_t rec  = quota_test_counter_record_bytes();
    uint32_t base = quota_test_counter_base_offset();
    uint32_t line = quota_test_counter_line_bytes();
    TEST_ASSERT_EQ((uint64_t)line, (uint64_t)QUOTA_PERF_LINE_BYTES,
                   "implementation cache-line bound matches this test's own pin");
    TEST_ASSERT_EQ((uint64_t)rec, 32ULL, "a per-type counter record is 32 bytes");
    TEST_ASSERT(rec <= line,
                "a record fits within one cache line, so it can never span three");

    /* Pinned independently of the implementation: moving the array changes
     * which records share a line, so a struct-prefix change must be a decision,
     * not a silent inheritance of a claim that no longer holds. */
    TEST_ASSERT_EQ((uint64_t)base, (uint64_t)QUOTA_PERF_COUNTER_BASE,
                   "counter array still starts at the pinned block offset");

    uint64_t single = 0;
    for (uint32_t i = 0; i < quota_resource_type_count(); i++) {
        uintptr_t start = (uintptr_t)b + base + (uintptr_t)i * rec;
        uintptr_t end   = start + rec - 1;
        uint64_t  lines = (uint64_t)(end / line)
                        - (uint64_t)(start / line) + 1;
        TEST_ASSERT(lines <= 2, "a counter record spans at most two cache lines");
        if (lines == 1)
            single++;
    }
    klog(LOG_INFO, QUOTA_PERF_LOG_TAG, "counter records on one line: %u of %u",
         single, (uint64_t)quota_resource_type_count());

    quota_block_deref(b);
}

/* --- Advisory: TSC cost, reported and never asserted ---------------------- */

#ifdef QUOTA_PERF_HAVE_TSC

/* Whether the CPU tag returned beside an RDTSCP sample can be believed.
 *
 * CPU_FEATURE_RDTSCP gates the INSTRUCTION, but it does not make ECX
 * meaningful: WHPX exposes RDTSCP in CPUID while trapping IA32_TSC_AUX, which
 * leaves the tag unpredictable and would make a migration check either accept
 * a migrated sample or discard a good one. boot_hw.c probes the MSR and records
 * the answer in g_tsc_aux_available (QEMU TCG reports it unavailable).
 *
 * The measurement is NOT skipped when the tag is untrustworthy -- that would
 * surrender the numbers on the platform this suite actually runs on. Instead
 * the sample is reported with the missing guarantee stated, so an untagged
 * number is never presented as a verified one. */
static int quota_perf_cpu_tag_trusted(void)
{
    extern int g_tsc_aux_available;
    return g_tsc_aux_available != 0;
}

/* Time QUOTA_PERF_ITERATIONS charge+return pairs and report cycles per pair.
 * The lock instrumentation is left disarmed, so this measures the production
 * path and not the counter. */
static void test_quota_charge_cycles_advisory(void)
{
    if (!cpu_has(CPU_FEATURE_RDTSCP)) {
        TEST_SKIP("RDTSCP unsupported; advisory charge timing unavailable");
        return;
    }

    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    uint64_t before = quota_usage(b, QUOTA_RES_NONPAGED_POOL);
    uint32_t cpu_start = 0, cpu_end = 0;
    uint32_t done = 0;
    int all_ok = 1;

    uint64_t t0 = rdtscp_read(&cpu_start);
    for (uint32_t i = 0; i < QUOTA_PERF_ITERATIONS; i++) {
        if (quota_charge(b, QUOTA_RES_NONPAGED_POOL, 64) != STATUS_SUCCESS ||
            quota_return(b, QUOTA_RES_NONPAGED_POOL, 64) != STATUS_SUCCESS) {
            all_ok = 0;
            break;
        }
        done++;
    }
    uint64_t t1 = rdtscp_read(&cpu_end);

    /* The measurement's own integrity IS asserted -- only the resulting number
     * is advisory. A run whose operations failed or left usage behind would
     * report a meaningless cost, and that is a real defect worth failing on.
     * Both assertions hold whether or not the sample is reported. */
    TEST_ASSERT(all_ok != 0, "every timed charge/return pair succeeded");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_NONPAGED_POOL), before,
                   "timed loop is balanced: usage back to its baseline");

    if (!all_ok || done != QUOTA_PERF_ITERATIONS) {
        klog(LOG_INFO, QUOTA_PERF_LOG_TAG,
             "charge+return timing discarded (run incomplete: %u of %u pairs)", (uint64_t)done,
             (uint64_t)QUOTA_PERF_ITERATIONS);
    } else if (t1 <= t0 || (quota_perf_cpu_tag_trusted() && cpu_start != cpu_end)) {
        klog(LOG_INFO, QUOTA_PERF_LOG_TAG,
             "charge+return timing discarded (migrated or non-monotonic TSC)");
    } else {
        klog(LOG_INFO, QUOTA_PERF_LOG_TAG, "charge+return: %u cycles over %u pairs%s",
             (t1 - t0) / QUOTA_PERF_ITERATIONS, (uint64_t)QUOTA_PERF_ITERATIONS,
             quota_perf_cpu_tag_trusted() ? "" : " (CPU tag unverified: no TSC_AUX)");
    }

    quota_block_deref(b);
}

/* Same measurement for the chain path, which is what a pool consumer would
 * actually pay: it walks the owner chain and charges each layer in turn. */
static void test_quota_chain_cycles_advisory(void)
{
    struct task *t = task_current();

    if (!cpu_has(CPU_FEATURE_RDTSCP)) {
        TEST_SKIP("RDTSCP unsupported; advisory chain timing unavailable");
        return;
    }
    if (!t || !t->quota || !t->quota_user) {
        TEST_SKIP("current task carries no quota blocks");
        return;
    }

    uint64_t proc_before = quota_usage(t->quota, QUOTA_RES_NONPAGED_POOL);
    uint64_t user_before = quota_usage(t->quota_user, QUOTA_RES_NONPAGED_POOL);
    uint32_t cpu_start = 0, cpu_end = 0;
    uint32_t done = 0;
    int all_ok = 1;

    uint64_t t0 = rdtscp_read(&cpu_start);
    for (uint32_t i = 0; i < QUOTA_PERF_ITERATIONS; i++) {
        quota_charge_receipt_t r = { 0 };
        uint64_t tok = 0;
        if (quota_charge_chain(t, QUOTA_RES_NONPAGED_POOL, 64, 0, &r, &tok) != STATUS_SUCCESS) {
            all_ok = 0;
            break;
        }
        quota_return_chain(&r, tok);
        done++;
    }
    uint64_t t1 = rdtscp_read(&cpu_end);

    /* EVERY layer must be back at its baseline, not just the process one:
     * quota_return_chain does not surface a per-block return failure, so a
     * user-layer return that failed would otherwise leave usage stranded here
     * with every assertion still green. */
    TEST_ASSERT(all_ok != 0, "every timed chain charge/return pair succeeded");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NONPAGED_POOL), proc_before,
                   "timed chain loop is balanced: process layer at its baseline");
    TEST_ASSERT_EQ(quota_usage(t->quota_user, QUOTA_RES_NONPAGED_POOL), user_before,
                   "timed chain loop is balanced: user layer at its baseline");

    if (!all_ok || done != QUOTA_PERF_ITERATIONS) {
        klog(LOG_INFO, QUOTA_PERF_LOG_TAG,
             "chain timing discarded (run incomplete: %u of %u pairs)", (uint64_t)done,
             (uint64_t)QUOTA_PERF_ITERATIONS);
    } else if (t1 <= t0 || (quota_perf_cpu_tag_trusted() && cpu_start != cpu_end)) {
        klog(LOG_INFO, QUOTA_PERF_LOG_TAG,
             "chain timing discarded (migrated or non-monotonic TSC)");
    } else {
        klog(LOG_INFO, QUOTA_PERF_LOG_TAG, "chain charge+return: %u cycles over %u pairs%s",
             (t1 - t0) / QUOTA_PERF_ITERATIONS, (uint64_t)QUOTA_PERF_ITERATIONS,
             quota_perf_cpu_tag_trusted() ? "" : " (CPU tag unverified: no TSC_AUX)");
    }
}

#endif /* QUOTA_PERF_HAVE_TSC */

void test_register_quota_perf(void)
{
    test_suite_register_cat("Quota: charge lock-section budget",
                            test_quota_charge_lock_budget, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: transfer lock-section budget",
                            test_quota_transfer_lock_budget, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: chain lock-section budget",
                            test_quota_chain_lock_budget, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: chain rollback lock budget",
                            test_quota_chain_rollback_lock_budget, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: counter record locality",
                            test_quota_counter_record_locality, TEST_CAT_QUOTA);
#ifdef QUOTA_PERF_HAVE_TSC
    test_suite_register_cat("Quota: charge cost (advisory)",
                            test_quota_charge_cycles_advisory, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: chain charge cost (advisory)",
                            test_quota_chain_cycles_advisory, TEST_CAT_QUOTA);
#endif
}

#endif /* KERNEL_TESTS */
