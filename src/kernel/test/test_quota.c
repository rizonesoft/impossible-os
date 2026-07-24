/* ============================================================================
 * test_quota.c -- Kernel resource-accounting & quota unit tests
 *
 * Section 1 coverage: the resource type registry (taxonomy table, accessors,
 * unit/limit/name metadata, out-of-range safety, boot-time validation).
 * Section 2 coverage: quota blocks and the charge API (round-trip accounting,
 * peak high-water, limit enforcement and lowering, the 0..QUOTA_AMOUNT_MAX
 * counter domain, underflow clamping, transfer, refcount lifetime, owner SID).
 *
 * XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/quota/quota.h"
#include "kernel/security/privileges.h"  /* SE_INCREASE_QUOTA_PRIVILEGE */
#include "kernel/security/sid.h"         /* SID helpers for the owner-SID test */
#include "kernel/mm/heap.h"              /* kmalloc_fail_next for the OOM path */
#include "kernel/sched/task.h"           /* kthread_create / thread_join       */
#include "kernel/ob/ob_job.h"            /* job membership generation test      */
#include "kernel/ob/handle_table.h"      /* scratch table behind the job handle */
#include "kernel/ob/ob.h"                /* ObDereferenceObject                 */

/* The registry is validated in Phase 2 (quota_register_types) before any
 * consumer runs; the test sweep runs in Phase 3, so it is already ready. */
static void test_quota_registry_ready(void)
{
    TEST_ASSERT(quota_registry_ready() != 0,
                "quota registry validated at boot before tests run");
}

/* The accessor must report the enum count (the table is a parallel array). */
static void test_quota_type_count(void)
{
    TEST_ASSERT_EQ((uint64_t)quota_resource_type_count(),
                   (uint64_t)QUOTA_RESOURCE_TYPE_COUNT,
                   "quota_resource_type_count == QUOTA_RESOURCE_TYPE_COUNT");
    TEST_ASSERT(quota_resource_type_count() > 0,
                "at least one resource type is registered");
}

/* Every type has a valid descriptor with a non-empty name and a valid unit. */
static void test_quota_all_types_valid(void)
{
    for (uint32_t i = 0; i < quota_resource_type_count(); i++) {
        const quota_resource_desc_t *d = quota_resource_desc((quota_resource_type_t)i);
        TEST_ASSERT_NOT_NULL((void *)d, "in-range descriptor is non-NULL");
        TEST_ASSERT_NOT_NULL((void *)d->name, "descriptor name is non-NULL");
        TEST_ASSERT(d->name[0] != '\0', "descriptor name is non-empty");
        TEST_ASSERT(d->unit == QUOTA_UNIT_COUNT || d->unit == QUOTA_UNIT_BYTES,
                    "descriptor unit is COUNT or BYTES");
    }
}

/* Out-of-range accessors are safe: NULL descriptor, "?" name. */
static void test_quota_out_of_range_safe(void)
{
    const quota_resource_desc_t *d =
        quota_resource_desc((quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT);
    TEST_ASSERT_NULL((void *)d, "out-of-range descriptor is NULL");

    const char *name =
        quota_resource_type_name((quota_resource_type_t)(QUOTA_RESOURCE_TYPE_COUNT + 5));
    TEST_ASSERT_NOT_NULL((void *)name, "out-of-range name is non-NULL");
    TEST_ASSERT(name[0] == '?', "out-of-range name is the '?' sentinel");
}

/* Table-driven expected unit for EVERY resource type: pool/registry/crash are
 * byte-denominated, discrete objects are counted. Catches unit drift on any
 * row, not just a sampled few. */
static void test_quota_units_all_rows(void)
{
    /* Positional (unsized) in enum order: an appended enum member without a
     * matching row here shrinks sizeof(expect) so the assert fails at compile
     * time -- a sized [COUNT] array would tautologically always pass. */
    static const quota_unit_t expect[] = {
        QUOTA_UNIT_COUNT,   /* HANDLE             */
        QUOTA_UNIT_COUNT,   /* OBJECT_BODY        */
        QUOTA_UNIT_COUNT,   /* NAMESPACE_ENTRY    */
        QUOTA_UNIT_BYTES,   /* PAGED_POOL         */
        QUOTA_UNIT_BYTES,   /* NONPAGED_POOL      */
        QUOTA_UNIT_BYTES,   /* REGISTRY_BYTES     */
        QUOTA_UNIT_COUNT,   /* ALPC_MESSAGE       */
        QUOTA_UNIT_COUNT,   /* NOTIFICATION_STATE */
        QUOTA_UNIT_COUNT,   /* TIMER              */
        QUOTA_UNIT_COUNT,   /* THREAD             */
        QUOTA_UNIT_COUNT,   /* PROCESS            */
        QUOTA_UNIT_COUNT,   /* SECTION            */
        QUOTA_UNIT_COUNT,   /* MAPPED_VIEW        */
        QUOTA_UNIT_BYTES,   /* CRASH_BUFFER       */
        QUOTA_UNIT_COUNT,   /* NOTIFICATION_SUB   */
        QUOTA_UNIT_BYTES,   /* NOTIFICATION_BYTES */
    };
    _Static_assert(sizeof(expect) / sizeof(expect[0]) == QUOTA_RESOURCE_TYPE_COUNT,
                   "expected-unit table must have one positional entry per resource type");
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++)
        TEST_ASSERT_EQ((uint64_t)quota_resource_desc((quota_resource_type_t)i)->unit,
                       (uint64_t)expect[i], "resource type unit matches expected");
}

/* At this layer every default limit is UNLIMITED (0); concrete caps are
 * kernel-config policy applied at charge time. */
static void test_quota_default_limits_unlimited(void)
{
    TEST_ASSERT_EQ((uint64_t)QUOTA_LIMIT_UNLIMITED, (uint64_t)0,
                   "unlimited sentinel is 0");
    for (uint32_t i = 0; i < quota_resource_type_count(); i++) {
        const quota_resource_desc_t *d = quota_resource_desc((quota_resource_type_t)i);
        TEST_ASSERT_EQ((uint64_t)d->default_limit, (uint64_t)QUOTA_LIMIT_UNLIMITED,
                       "base-layer default limit is unlimited (caps come from config)");
    }
}

/* EVERY row's override privilege is SeIncreaseQuotaPrivilege (LUID 5), and none
 * is a zero LUID -- security-sensitive metadata that must not drift on any row. */
static void test_quota_override_privilege_all_rows(void)
{
    LUID expected = SE_INCREASE_QUOTA_PRIVILEGE;
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        const quota_resource_desc_t *d = quota_resource_desc((quota_resource_type_t)i);
        TEST_ASSERT(!RtlIsZeroLuid(&d->override_privilege),
                    "override privilege is not a zero LUID");
        TEST_ASSERT(RtlEqualLuid(&d->override_privilege, &expected),
                    "override privilege is SeIncreaseQuota (LUID 5)");
    }
}

/* Names are unique across all rows (a dump / by-name query must be unambiguous). */
static void test_quota_names_unique(void)
{
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        const char *ni = quota_resource_type_name((quota_resource_type_t)i);
        for (uint32_t j = i + 1; j < QUOTA_RESOURCE_TYPE_COUNT; j++) {
            const char *nj = quota_resource_type_name((quota_resource_type_t)j);
            /* pointer inequality is not enough; compare content */
            int same = 1;
            for (uint32_t k = 0; ; k++) {
                if (ni[k] != nj[k]) { same = 0; break; }
                if (ni[k] == '\0') break;
            }
            TEST_ASSERT(!same, "no two resource types share a name");
        }
    }
}

/* quota_types_dump walks all rows and formats each; exercise it end to end and
 * confirm it does not disturb registry state (no crash / NULL deref). */
static void test_quota_dump_smoke(void)
{
    quota_types_dump();
    TEST_ASSERT(quota_registry_ready() != 0,
                "registry still ready after dump (dump is read-only)");
}

/* --- Section 2: quota blocks and the charge API -------------------------- */

/* A fresh block starts at zero usage/peak/failures and carries one reference. */
static void test_quota_block_create_zeroed(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0) returns a block");
    if (!b)
        return;

    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_HANDLE), 0ULL, "fresh block usage is 0");
    TEST_ASSERT_EQ(quota_peak(b, QUOTA_RES_HANDLE), 0ULL, "fresh block peak is 0");
    TEST_ASSERT_EQ(quota_failures(b, QUOTA_RES_HANDLE), 0ULL, "fresh block failures is 0");
    TEST_ASSERT_NULL((void *)quota_block_owner(b), "no owner SID when created with NULL");
    quota_block_deref(b);
}

/* Charge then return leaves usage back at 0, but peak retains the high water. */
static void test_quota_charge_return_roundtrip(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_HANDLE, 10),
                   (uint64_t)STATUS_SUCCESS, "charge 10 succeeds");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_HANDLE), 10ULL, "usage is 10 after charge");
    TEST_ASSERT_EQ(quota_peak(b, QUOTA_RES_HANDLE), 10ULL, "peak lifted to 10");

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_HANDLE, 5),
                   (uint64_t)STATUS_SUCCESS, "charge 5 more succeeds");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_HANDLE), 15ULL, "usage accumulates to 15");

    TEST_ASSERT_EQ((uint64_t)quota_return(b, QUOTA_RES_HANDLE, 15),
                   (uint64_t)STATUS_SUCCESS, "return 15 succeeds");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_HANDLE), 0ULL, "usage back to 0");
    TEST_ASSERT_EQ(quota_peak(b, QUOTA_RES_HANDLE), 15ULL, "peak retains high water 15");
    quota_block_deref(b);
}

/* An over-limit charge is refused, leaves usage untouched, and counts a failure. */
static void test_quota_charge_over_limit_refused(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(b, QUOTA_RES_TIMER, 100),
                   (uint64_t)STATUS_SUCCESS, "set limit 100");
    TEST_ASSERT_EQ(quota_limit(b, QUOTA_RES_TIMER), 100ULL, "limit reads back as 100");

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_TIMER, 100),
                   (uint64_t)STATUS_SUCCESS, "charge exactly to the limit succeeds");
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_TIMER, 1),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "one over the limit is refused");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_TIMER), 100ULL, "refused charge left usage at 100");
    TEST_ASSERT_EQ(quota_failures(b, QUOTA_RES_TIMER), 1ULL, "failure counter bumped once");
    quota_block_deref(b);
}

/* Unlimited (0) means no cap: a large charge under the domain max succeeds. */
static void test_quota_unlimited_allows_large_charge(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ(quota_limit(b, QUOTA_RES_PAGED_POOL), (uint64_t)QUOTA_LIMIT_UNLIMITED,
                   "default limit is unlimited");
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_PAGED_POOL, 1ULL << 40),
                   (uint64_t)STATUS_SUCCESS, "1 TiB charge succeeds when unlimited");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_PAGED_POOL), 1ULL << 40, "usage is 1 TiB");
    quota_block_deref(b);
}

/* The counter domain is enforced at the boundary: amounts above the max are
 * rejected as bad parameters, and an add that would leave the domain is an
 * overflow rather than a wrap. */
static void test_quota_overflow_domain_guarded(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_NONPAGED_POOL,
                                          (uint64_t)QUOTA_AMOUNT_MAX + 1),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "amount above the domain max is an invalid parameter");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_NONPAGED_POOL), 0ULL,
                   "rejected amount did not touch usage");

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_NONPAGED_POOL,
                                          (uint64_t)QUOTA_AMOUNT_MAX),
                   (uint64_t)STATUS_SUCCESS, "charging the domain max succeeds");
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_NONPAGED_POOL, 1),
                   (uint64_t)STATUS_INTEGER_OVERFLOW,
                   "one more would leave the domain: overflow, not wrap");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_NONPAGED_POOL), (uint64_t)QUOTA_AMOUNT_MAX,
                   "usage unchanged after the overflow refusal");
    quota_block_deref(b);
}

/* Returning more than was charged FAILS CLOSED and leaves usage untouched --
 * clamping to zero would erase other live charges (a duplicate cleanup for one
 * resource would wipe the accounting for everything allocated since). */
static void test_quota_return_underflow_refused(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_THREAD, 3),
                   (uint64_t)STATUS_SUCCESS, "charge 3 threads");
    TEST_ASSERT_EQ((uint64_t)quota_return(b, QUOTA_RES_THREAD, 10),
                   (uint64_t)STATUS_INTEGER_OVERFLOW, "over-return is refused");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_THREAD), 3ULL,
                   "refused over-return left usage exactly as it was");
    TEST_ASSERT_EQ(quota_failures(b, QUOTA_RES_THREAD), 1ULL,
                   "the refused over-return was counted");
    quota_block_deref(b);
}

/* The concrete hazard the fail-closed semantic exists for: a stale duplicate
 * return must not erase a newer, unrelated charge. */
static void test_quota_stale_return_cannot_erase_newer_charge(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    /* Resource A charges 5 and is cleaned up correctly. */
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_HANDLE, 5),
                   (uint64_t)STATUS_SUCCESS, "A charges 5");
    TEST_ASSERT_EQ((uint64_t)quota_return(b, QUOTA_RES_HANDLE, 5),
                   (uint64_t)STATUS_SUCCESS, "A returns its 5");

    /* Resource B then charges 4 and is still live. */
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_HANDLE, 4),
                   (uint64_t)STATUS_SUCCESS, "B charges 4");

    /* A retried/duplicate cleanup for A returns the stale 5. */
    TEST_ASSERT_EQ((uint64_t)quota_return(b, QUOTA_RES_HANDLE, 5),
                   (uint64_t)STATUS_INTEGER_OVERFLOW, "the stale duplicate return is refused");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_HANDLE), 4ULL,
                   "B's live charge survives the stale return (clamping would have zeroed it)");
    quota_block_deref(b);
}

/* Lowering a limit below current usage keeps committed charges but refuses the
 * next charge (the documented limit-lowering contract). */
static void test_quota_lowering_limit_keeps_committed(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_SECTION, 50),
                   (uint64_t)STATUS_SUCCESS, "charge 50 sections");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(b, QUOTA_RES_SECTION, 10),
                   (uint64_t)STATUS_SUCCESS, "lower the limit to 10 below usage");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_SECTION), 50ULL,
                   "committed usage survives the lowering");
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_SECTION, 1),
                   (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "an over-quota block refuses the next charge");
    quota_block_deref(b);
}

/* Transfer moves usage between blocks and refuses when the source is short or
 * the destination cannot accept the charge (both blocks left untouched). */
static void test_quota_transfer_moves_and_refuses(void)
{
    quota_block_t *src = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    quota_block_t *dst = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)src, "src block allocated");
    TEST_ASSERT_NOT_NULL((void *)dst, "dst block allocated");
    if (!src || !dst) {
        quota_block_deref(src);
        quota_block_deref(dst);
        return;
    }

    TEST_ASSERT_EQ((uint64_t)quota_charge(src, QUOTA_RES_ALPC_MESSAGE, 40),
                   (uint64_t)STATUS_SUCCESS, "src charged 40");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(src, dst, QUOTA_RES_ALPC_MESSAGE, 15),
                   (uint64_t)STATUS_SUCCESS, "transfer 15 succeeds");
    TEST_ASSERT_EQ(quota_usage(src, QUOTA_RES_ALPC_MESSAGE), 25ULL, "src down to 25");
    TEST_ASSERT_EQ(quota_usage(dst, QUOTA_RES_ALPC_MESSAGE), 15ULL, "dst up to 15");

    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(src, dst, QUOTA_RES_ALPC_MESSAGE, 999),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "transfer beyond src holdings refused");
    TEST_ASSERT_EQ(quota_usage(src, QUOTA_RES_ALPC_MESSAGE), 25ULL, "src untouched on refusal");
    TEST_ASSERT_EQ(quota_usage(dst, QUOTA_RES_ALPC_MESSAGE), 15ULL, "dst untouched on refusal");

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(dst, QUOTA_RES_ALPC_MESSAGE, 16),
                   (uint64_t)STATUS_SUCCESS, "cap dst just above its usage");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(src, dst, QUOTA_RES_ALPC_MESSAGE, 5),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "dst at cap refuses the transfer");
    TEST_ASSERT_EQ(quota_usage(src, QUOTA_RES_ALPC_MESSAGE), 25ULL,
                   "src untouched when dst refuses");
    TEST_ASSERT_EQ(quota_usage(dst, QUOTA_RES_ALPC_MESSAGE), 15ULL,
                   "dst unchanged when its own charge failed");

    quota_block_deref(src);
    quota_block_deref(dst);
}

/* Self-transfer is a no-op success, not a double count or a drain. */
static void test_quota_transfer_self_is_noop(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_PROCESS, 7),
                   (uint64_t)STATUS_SUCCESS, "charge 7 processes");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(b, b, QUOTA_RES_PROCESS, 7),
                   (uint64_t)STATUS_SUCCESS, "self-transfer succeeds");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_PROCESS), 7ULL, "self-transfer left usage alone");
    quota_block_deref(b);
}

/* Every entry point rejects NULL blocks and out-of-range types instead of
 * faulting, and the queries answer 0 for them. */
static void test_quota_charge_api_bad_args(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    quota_resource_type_t bad = (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT;

    TEST_ASSERT_EQ((uint64_t)quota_charge(NULL, QUOTA_RES_HANDLE, 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "charge NULL block rejected");
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, bad, 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "charge bad type rejected");
    TEST_ASSERT_EQ((uint64_t)quota_return(NULL, QUOTA_RES_HANDLE, 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "return NULL block rejected");
    TEST_ASSERT_EQ((uint64_t)quota_return(b, bad, 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "return bad type rejected");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(NULL, b, QUOTA_RES_HANDLE, 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "transfer NULL src rejected");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(b, NULL, QUOTA_RES_HANDLE, 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "transfer NULL dst rejected");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(b, QUOTA_RES_HANDLE,
                                             (uint64_t)QUOTA_AMOUNT_MAX + 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "limit above the domain rejected");

    TEST_ASSERT_EQ(quota_usage(NULL, QUOTA_RES_HANDLE), 0ULL, "usage of NULL is 0");
    TEST_ASSERT_EQ(quota_peak(b, bad), 0ULL, "peak of a bad type is 0");
    TEST_ASSERT_EQ(quota_failures(NULL, QUOTA_RES_HANDLE), 0ULL, "failures of NULL is 0");
    TEST_ASSERT_EQ(quota_limit(b, bad), 0ULL, "limit of a bad type is 0");

    /* A zero amount is an explicit no-op success, and must not lift the peak. */
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_HANDLE, 0),
                   (uint64_t)STATUS_SUCCESS, "zero charge is a no-op success");
    TEST_ASSERT_EQ(quota_peak(b, QUOTA_RES_HANDLE), 0ULL, "zero charge did not lift the peak");

    quota_block_deref(b);
}

/* An extra reference keeps the block alive across a deref; the counters stay
 * readable and correct until the last reference goes. */
static void test_quota_block_refcount(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    quota_block_ref(b);
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_CRASH_BUFFER, 4),
                   (uint64_t)STATUS_SUCCESS, "charge against a twice-referenced block");
    quota_block_deref(b);                 /* drops to one reference, must NOT free */
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_CRASH_BUFFER), 4ULL,
                   "block still live and accurate after one deref");
    quota_block_deref(b);                 /* last reference: frees */

    /* NULL is tolerated by both reference calls (defensive, no fault). */
    quota_block_ref(NULL);
    quota_block_deref(NULL);
}

/* A block created with an owner SID copies it in and reports it back. */
static void test_quota_block_owner_sid(void)
{
    uint8_t buf[SID_MAX_SIZE] = { 0 };
    SID *owner = (SID *)buf;
    const uint8_t nt_authority[6] = { 0, 0, 0, 0, 0, 5 };

    RtlInitializeSid(owner, nt_authority, 1);
    uint32_t *sub = RtlSubAuthoritySid(owner, 0);
    TEST_ASSERT_NOT_NULL((void *)sub, "sub-authority 0 is addressable");
    if (sub)
        *sub = 18;                        /* S-1-5-18, the local system SID */

    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, owner, sizeof(buf));
    TEST_ASSERT_NOT_NULL((void *)b, "block with an owner SID allocated");
    if (!b)
        return;

    const SID *got = quota_block_owner(b);
    TEST_ASSERT_NOT_NULL((void *)got, "owner SID reported back");
    if (got) {
        TEST_ASSERT(RtlEqualSid(got, owner) != 0, "reported owner equals the source SID");
        TEST_ASSERT(got != owner, "owner SID was copied, not aliased");
    }
    quota_block_deref(b);
}

/* ==========================================================================
 * Section 3: process/token/job ownership model
 * ========================================================================== */

/* Build S-1-5-<rid> into `buf` and return it. The section-3 tests need SIDs
 * that are NOT the system SID, so they can exercise the per-SID registry
 * without disturbing the live SYSTEM user block every process shares. */
static SID *test_quota_make_sid(uint8_t *buf, uint32_t rid)
{
    SID *sid = (SID *)buf;
    const uint8_t nt_authority[6] = { 0, 0, 0, 0, 0, 5 };

    RtlInitializeSid(sid, nt_authority, 1);
    uint32_t *sub = RtlSubAuthoritySid(sid, 0);
    if (sub)
        *sub = rid;
    return sid;
}

/* The principal is recorded at creation and reported back; a NULL block and an
 * out-of-range principal are both rejected rather than silently defaulted. */
static void test_quota_principal_recorded(void)
{
    quota_block_t *p = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    quota_block_t *j = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);

    TEST_ASSERT_NOT_NULL((void *)p, "process-principal block allocated");
    TEST_ASSERT_NOT_NULL((void *)j, "job-principal block allocated");
    if (p)
        TEST_ASSERT_EQ((uint64_t)quota_block_principal(p),
                       (uint64_t)QUOTA_PRINCIPAL_PROCESS,
                       "process block reports QUOTA_PRINCIPAL_PROCESS");
    if (j)
        TEST_ASSERT_EQ((uint64_t)quota_block_principal(j),
                       (uint64_t)QUOTA_PRINCIPAL_JOB,
                       "job block reports QUOTA_PRINCIPAL_JOB");
    TEST_ASSERT_EQ((uint64_t)quota_block_principal(NULL),
                   (uint64_t)QUOTA_PRINCIPAL_COUNT,
                   "NULL block reports QUOTA_PRINCIPAL_COUNT");
    TEST_ASSERT_NULL((void *)quota_block_create(QUOTA_PRINCIPAL_COUNT, NULL, 0),
                     "out-of-range principal refused");

    quota_block_deref(p);
    quota_block_deref(j);
}

/* The canonical-user rule: the same SID must resolve to the SAME block, or a
 * second token lineage would receive its own full budget and the per-user
 * limit would not be a limit at all. A different SID must NOT collide. */
static void test_quota_user_block_canonical(void)
{
    uint8_t buf_a[SID_MAX_SIZE] = { 0 };
    uint8_t buf_b[SID_MAX_SIZE] = { 0 };
    SID *sid_a = test_quota_make_sid(buf_a, 4101);
    SID *sid_b = test_quota_make_sid(buf_b, 4102);

    quota_block_t *first  = quota_user_block_acquire(sid_a, SID_MAX_SIZE);
    quota_block_t *second = quota_user_block_acquire(sid_a, SID_MAX_SIZE);
    quota_block_t *other  = quota_user_block_acquire(sid_b, SID_MAX_SIZE);

    TEST_ASSERT_NOT_NULL((void *)first, "user block acquired for SID A");
    TEST_ASSERT_NOT_NULL((void *)other, "user block acquired for SID B");
    TEST_ASSERT(first == second, "same SID resolves to the same canonical block");
    TEST_ASSERT(first != other, "a different SID gets a different block");
    if (first)
        TEST_ASSERT_EQ((uint64_t)quota_block_principal(first),
                       (uint64_t)QUOTA_PRINCIPAL_USER,
                       "acquired block is a USER-principal block");
    TEST_ASSERT_NULL((void *)quota_user_block_acquire(NULL, 0),
                     "ownerless user block refused (could never be found again)");

    /* Usage charged through one handle is visible through the other, which is
     * the whole point of the block being shared. */
    if (first && second) {
        TEST_ASSERT_EQ((uint64_t)quota_charge(first, QUOTA_RES_HANDLE, 7),
                       (uint64_t)STATUS_SUCCESS, "charge via first handle");
        TEST_ASSERT_EQ(quota_usage(second, QUOTA_RES_HANDLE), 7ULL,
                       "usage visible through the second handle");
        TEST_ASSERT_EQ((uint64_t)quota_return(second, QUOTA_RES_HANDLE, 7),
                       (uint64_t)STATUS_SUCCESS, "return via second handle");
    }

    quota_block_deref(first);
    quota_block_deref(second);
    quota_block_deref(other);
}

/* try_ref pins a block that is still live, and the extra reference keeps it
 * alive across the owner's deref. */
static void test_quota_try_ref_live_block(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_block_try_ref(b), 1ULL,
                   "try_ref pins a live block");
    TEST_ASSERT_EQ((uint64_t)quota_block_try_ref(NULL), 0ULL,
                   "try_ref on NULL reports failure rather than faulting");

    /* Drop the creation reference; the try_ref reference must still hold the
     * block, which the charge below proves by using it. */
    quota_block_deref(b);
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_TIMER, 3),
                   (uint64_t)STATUS_SUCCESS, "block still usable via try_ref reference");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_TIMER), 3ULL, "usage recorded");
    quota_block_deref(b);
}

/* The double-count regression: a chain charge hits the process, user, and job
 * blocks, so an aggregate that summed every block owned by a SID would report
 * the charge two or three times. The rollup must count the USER layer only. */
static void test_quota_rollup_counts_user_layer_once(void)
{
    uint8_t buf[SID_MAX_SIZE] = { 0 };
    SID *sid = test_quota_make_sid(buf, 4103);
    uint64_t usage = 0, peak = 0;

    quota_block_t *user = quota_user_block_acquire(sid, SID_MAX_SIZE);
    /* A process block carrying the SAME owner SID -- exactly what a process
     * running as this user has. It must not be added to the aggregate. */
    quota_block_t *proc = quota_block_create(QUOTA_PRINCIPAL_PROCESS, sid, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)user, "canonical user block acquired");
    TEST_ASSERT_NOT_NULL((void *)proc, "same-SID process block created");
    if (!user || !proc) {
        quota_block_deref(user);
        quota_block_deref(proc);
        return;
    }

    TEST_ASSERT_EQ((uint64_t)quota_charge(user, QUOTA_RES_SECTION, 40),
                   (uint64_t)STATUS_SUCCESS, "charge the user block");
    TEST_ASSERT_EQ((uint64_t)quota_charge(proc, QUOTA_RES_SECTION, 40),
                   (uint64_t)STATUS_SUCCESS, "same charge lands on the process block");

    TEST_ASSERT_EQ((uint64_t)quota_rollup_by_sid(sid, SID_MAX_SIZE,
                                                 QUOTA_RES_SECTION, &usage, &peak),
                   (uint64_t)STATUS_SUCCESS, "rollup succeeds");
    TEST_ASSERT_EQ(usage, 40ULL, "rollup counts the charge ONCE, not per layer");
    TEST_ASSERT_EQ(peak, 40ULL, "rollup peak counts the user layer only");

    (void)quota_return(user, QUOTA_RES_SECTION, 40);
    (void)quota_return(proc, QUOTA_RES_SECTION, 40);
    quota_block_deref(user);
    quota_block_deref(proc);
}

/* Rollup argument validation, including a SID the caller under-declares. */
static void test_quota_rollup_bad_args(void)
{
    uint8_t buf[SID_MAX_SIZE] = { 0 };
    SID *sid = test_quota_make_sid(buf, 4104);
    uint64_t usage = 0;

    TEST_ASSERT_EQ((uint64_t)quota_rollup_by_sid(NULL, 0, QUOTA_RES_HANDLE, &usage, NULL),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL owner refused");
    TEST_ASSERT_EQ((uint64_t)quota_rollup_by_sid(sid, SID_MAX_SIZE,
                                                 QUOTA_RESOURCE_TYPE_COUNT, &usage, NULL),
                   (uint64_t)STATUS_INVALID_PARAMETER, "out-of-range type refused");
    TEST_ASSERT_EQ((uint64_t)quota_rollup_by_sid(sid, 4, QUOTA_RES_HANDLE, &usage, NULL),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "SID truncated by owner_len refused");
    /* Both outputs optional: a caller wanting neither still gets a verdict. */
    TEST_ASSERT_EQ((uint64_t)quota_rollup_by_sid(sid, SID_MAX_SIZE,
                                                 QUOTA_RES_HANDLE, NULL, NULL),
                   (uint64_t)STATUS_SUCCESS, "NULL outputs accepted");
}

/* Every live task carries both a process block and a shared user block: the
 * creation paths and PID 0's own wiring must leave no task unaccounted. */
static void test_quota_task_has_blocks(void)
{
    struct task *t = task_current();

    TEST_ASSERT_NOT_NULL((void *)t, "current task resolves");
    if (!t)
        return;
    TEST_ASSERT_NOT_NULL((void *)t->quota, "current task has a process quota block");
    if (t->quota)
        TEST_ASSERT_EQ((uint64_t)quota_block_principal(t->quota),
                       (uint64_t)QUOTA_PRINCIPAL_PROCESS,
                       "task block is a PROCESS-principal block");
    TEST_ASSERT_NOT_NULL((void *)t->quota_user, "current task has a user quota block");
    if (t->quota_user)
        TEST_ASSERT_EQ((uint64_t)quota_block_principal(t->quota_user),
                       (uint64_t)QUOTA_PRINCIPAL_USER,
                       "task user block is a USER-principal block");
}

/* A chain charge lands on every principal owning the task and the receipt
 * returns exactly those blocks. */
static void test_quota_chain_charge_roundtrip(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0;
    uint64_t proc_before, user_before;

    if (!t || !t->quota || !t->quota_user)
        return;                      /* covered by the task-has-blocks test */

    proc_before = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);
    user_before = quota_usage(t->quota_user, QUOTA_RES_ALPC_MESSAGE);

    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_ALPC_MESSAGE, 11, 0, &r, &tok),
                   (uint64_t)STATUS_SUCCESS, "chain charge admitted");
    TEST_ASSERT(r.count >= 2, "receipt records at least the process and user blocks");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), proc_before + 11,
                   "process block charged");
    TEST_ASSERT_EQ(quota_usage(t->quota_user, QUOTA_RES_ALPC_MESSAGE), user_before + 11,
                   "user block charged the same amount");

    quota_return_chain(&r, tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), proc_before,
                   "process usage restored exactly");
    TEST_ASSERT_EQ(quota_usage(t->quota_user, QUOTA_RES_ALPC_MESSAGE), user_before,
                   "user usage restored exactly");
    TEST_ASSERT_EQ((uint64_t)r.count, 0ULL, "receipt emptied by the return");

    /* A zero-amount charge is a success that owes nothing -- but it still
     * validates the type, so a taxonomy mistake is never masked by amount 0. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_ALPC_MESSAGE, 0, 0, &r, &tok),
                   (uint64_t)STATUS_SUCCESS, "zero-amount chain charge succeeds");
    TEST_ASSERT_EQ((uint64_t)r.count, 0ULL, "zero-amount charge records no blocks");
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RESOURCE_TYPE_COUNT, 0, 0, &r, &tok),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "zero-amount charge still rejects an out-of-range type");
}

/* All-or-nothing: when a LATER block in the chain refuses, the prefix already
 * charged must be given back, leaving no usage stranded anywhere. The user
 * block sits after the process block, so capping the user block exercises a
 * genuine mid-chain refusal. */
static void test_quota_chain_all_or_nothing(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0;
    uint64_t proc_before, user_before, saved_limit;

    if (!t || !t->quota || !t->quota_user)
        return;

    proc_before = quota_usage(t->quota, QUOTA_RES_CRASH_BUFFER);
    user_before = quota_usage(t->quota_user, QUOTA_RES_CRASH_BUFFER);
    saved_limit = quota_limit(t->quota_user, QUOTA_RES_CRASH_BUFFER);

    /* Cap the user block one unit below what the charge needs. */
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(t->quota_user, QUOTA_RES_CRASH_BUFFER,
                                             user_before + 9),
                   (uint64_t)STATUS_SUCCESS, "user block capped for the test");

    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_CRASH_BUFFER, 10, 0, &r, &tok),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "chain charge refused at the cap");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_CRASH_BUFFER), proc_before,
                   "process prefix charge was rolled back");
    TEST_ASSERT_EQ(quota_usage(t->quota_user, QUOTA_RES_CRASH_BUFFER), user_before,
                   "user block took no usage");
    TEST_ASSERT_EQ((uint64_t)r.count, 0ULL, "failed charge leaves an empty receipt");

    (void)quota_set_limit(t->quota_user, QUOTA_RES_CRASH_BUFFER, saved_limit);
}

/* A repeated return must not credit back a second time: that is exactly how a
 * duplicate cleanup would erase charges made since. */
static void test_quota_chain_return_idempotent(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0;
    uint64_t proc_before;

    if (!t || !t->quota)
        return;

    proc_before = quota_usage(t->quota, QUOTA_RES_NOTIFICATION_STATE);
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_NOTIFICATION_STATE, 5, 0, &r, &tok),
                   (uint64_t)STATUS_SUCCESS, "chain charge admitted");
    quota_return_chain(&r, tok);
    quota_return_chain(&r, tok);      /* second return must be a no-op */
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_STATE), proc_before,
                   "double return does not push usage below the true value");

    /* A charge made after the first return must survive the duplicate. */
    TEST_ASSERT_EQ((uint64_t)quota_charge(t->quota, QUOTA_RES_NOTIFICATION_STATE, 2),
                   (uint64_t)STATUS_SUCCESS, "later charge admitted");
    quota_return_chain(&r, tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_STATE), proc_before + 2,
                   "duplicate return did not erase the newer charge");
    (void)quota_return(t->quota, QUOTA_RES_NOTIFICATION_STATE, 2);
}

/* A receipt that already holds a live charge must NOT be overwritten: doing so
 * would drop the references the first charge is holding and strand its usage
 * with nothing left that could ever return it. */
static void test_quota_chain_receipt_reuse_refused(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0;
    uint64_t proc_before;

    if (!t || !t->quota)
        return;

    proc_before = quota_usage(t->quota, QUOTA_RES_MAPPED_VIEW);
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_MAPPED_VIEW, 6, 0, &r, &tok),
                   (uint64_t)STATUS_SUCCESS, "first chain charge admitted");
    uint64_t live_tok = tok;
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_MAPPED_VIEW, 6, 0, &r, &tok),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "charging into a live receipt is refused, not silently overwritten");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_MAPPED_VIEW), proc_before + 6,
                   "the refused second charge added nothing");
    /* The refusal must not clobber the token of the charge it refused to
     * overwrite -- that charge is still the caller's, and its token is the only
     * key that can return it. */
    TEST_ASSERT_EQ(tok, live_tok,
                   "a refused charge leaves the live charge's token intact");

    quota_return_chain(&r, tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_MAPPED_VIEW), proc_before,
                   "the first charge was still returnable after the refusal");

    /* Once returned, the receipt is idle again and may be reused. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_MAPPED_VIEW, 6, 0, &r, &tok),
                   (uint64_t)STATUS_SUCCESS, "receipt reusable after return");
    quota_return_chain(&r, tok);
}

/* A task whose process block is gone (death teardown already ran) must NOT get
 * a successful empty charge -- that would let a dying process allocate with no
 * accounting at all. */
static void test_quota_chain_no_process_block_fails_closed(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0;
    struct quota_block *saved;
    uint64_t flags;

    if (!t || !t->quota)
        return;

    /* Simulate the post-teardown window by detaching the block, then restore
     * it: the pointer is swapped under the same lock the charge path uses. */
    spin_lock_irqsave(&t->quota_lock, &flags);
    saved = t->quota;
    t->quota = (struct quota_block *)0;
    spin_unlock_irqrestore(&t->quota_lock, flags);

    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_THREAD, 4, 0, &r, &tok),
                   (uint64_t)STATUS_PROCESS_IS_TERMINATING,
                   "charge without a process block fails closed");
    TEST_ASSERT_EQ((uint64_t)r.count, 0ULL, "failed charge holds no blocks");
    /* A ZERO charge must fail closed on a dead task too -- reporting success
     * there would let a caller read it as "the task is still chargeable". */
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_THREAD, 0, 0, &r, &tok),
                   (uint64_t)STATUS_PROCESS_IS_TERMINATING,
                   "zero-amount charge also fails closed without a process block");

    spin_lock_irqsave(&t->quota_lock, &flags);
    t->quota = saved;
    spin_unlock_irqrestore(&t->quota_lock, flags);
}




/* Joining a job folds the joiner's existing usage into the job block, and a
 * job that cannot absorb it refuses rather than letting the usage escape. */
static void test_quota_job_absorb_and_unabsorb(void)
{
    struct task *t = task_current();
    quota_absorb_record_t rec = { .taken = { 0 }, .active = 0 };
    quota_block_t *job = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);
    uint64_t proc_usage;

    if (!t || !t->quota || !job) {
        quota_block_deref(job);
        return;
    }

    TEST_ASSERT_EQ((uint64_t)quota_charge(t->quota, QUOTA_RES_TIMER, 12),
                   (uint64_t)STATUS_SUCCESS, "process holds usage before joining");
    /* Read the ACTUAL current usage rather than assuming 12: the absorb folds
     * in whatever the process block holds, and a future charging consumer
     * would otherwise turn this into a confusing off-by-N failure. */
    proc_usage = quota_usage(t->quota, QUOTA_RES_TIMER);

    TEST_ASSERT_EQ((uint64_t)quota_job_absorb_task(job, t, &rec),
                   (uint64_t)STATUS_SUCCESS, "job absorbs the joiner's usage");
    TEST_ASSERT_EQ(quota_usage(job, QUOTA_RES_TIMER), proc_usage,
                   "pre-existing usage now counts against the job");

    quota_job_unabsorb(job, &rec);
    TEST_ASSERT_EQ(quota_usage(job, QUOTA_RES_TIMER), 0ULL,
                   "a refused assignment leaves the job's accounting untouched");
    TEST_ASSERT_EQ((uint64_t)rec.active, 0ULL, "record emptied by the unwind");
    quota_job_unabsorb(job, &rec);       /* second unwind must be a no-op */
    TEST_ASSERT_EQ(quota_usage(job, QUOTA_RES_TIMER), 0ULL,
                   "double unabsorb does not push the job below zero");

    /* A job capped below the joiner's usage must refuse the absorb outright. */
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(job, QUOTA_RES_TIMER, proc_usage - 1),
                   (uint64_t)STATUS_SUCCESS, "job capped below the joiner's usage");
    TEST_ASSERT_EQ((uint64_t)quota_job_absorb_task(job, t, &rec),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "absorb refused at the cap");
    TEST_ASSERT_EQ(quota_usage(job, QUOTA_RES_TIMER), 0ULL,
                   "refused absorb folded nothing in");

    (void)quota_return(t->quota, QUOTA_RES_TIMER, 12);
    quota_block_deref(job);
}

/* Departure withdraws EXACTLY what joining folded in -- never the member's
 * current usage. Post-join charges reached the job through chain receipts and
 * are returned by those receipts; withdrawing them here too would return them
 * twice, and once another member's usage covered the difference the receipt's
 * own later return would subtract from THAT member's live charge. */
static void test_quota_job_departure_withdraws_only_absorbed(void)
{
    struct task *t = task_current();
    quota_absorb_record_t rec = { .taken = { 0 }, .active = 0 };
    quota_block_t *job = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);

    if (!t || !t->quota || !job) {
        quota_block_deref(job);
        return;
    }

    uint64_t base = quota_usage(t->quota, QUOTA_RES_SECTION);
    TEST_ASSERT_EQ((uint64_t)quota_charge(t->quota, QUOTA_RES_SECTION, 20),
                   (uint64_t)STATUS_SUCCESS, "process holds usage before joining");
    TEST_ASSERT_EQ((uint64_t)quota_job_absorb_task(job, t, &rec),
                   (uint64_t)STATUS_SUCCESS, "join folds the usage in");
    /* The absorb folds the process block's WHOLE current usage, not just the
     * 20 charged here -- assert against the measured baseline so a future
     * charging consumer does not turn this into an off-by-N mystery. */
    TEST_ASSERT_EQ(quota_usage(job, QUOTA_RES_SECTION), base + 20, "job holds it");

    /* A post-join charge reaching the job the way a chain charge would, and a
     * second member's live charge on the same job. */
    TEST_ASSERT_EQ((uint64_t)quota_charge(job, QUOTA_RES_SECTION, 5),
                   (uint64_t)STATUS_SUCCESS, "post-join receipt-backed charge");
    TEST_ASSERT_EQ((uint64_t)quota_charge(job, QUOTA_RES_SECTION, 7),
                   (uint64_t)STATUS_SUCCESS, "another member charges the job");

    quota_job_unabsorb(job, &rec);
    TEST_ASSERT_EQ(quota_usage(job, QUOTA_RES_SECTION), 12ULL,
                   "departure withdrew exactly what it absorbed, leaving 5+7 live");
    TEST_ASSERT_EQ((uint64_t)rec.active, 0ULL, "record consumed by the withdrawal");

    /* The post-join charge's own receipt return still finds its usage intact. */
    TEST_ASSERT_EQ((uint64_t)quota_return(job, QUOTA_RES_SECTION, 5),
                   (uint64_t)STATUS_SUCCESS, "receipt-backed return still valid");
    TEST_ASSERT_EQ(quota_usage(job, QUOTA_RES_SECTION), 7ULL,
                   "the other member's live charge was never touched");

    (void)quota_return(t->quota, QUOTA_RES_SECTION, 20);
    quota_block_deref(job);
}

/* Pins the DOCUMENTED limitation of absorbing an aggregate snapshot: a
 * pre-join resource released while still a member reduces the process block
 * but leaves the job's absorbed copy standing until detach. This test exists so
 * the behavior cannot change silently -- when section 4 lands receipt-obligation
 * migration, this assertion is the one that must be rewritten. */
static void test_quota_prejoin_release_holds_job_until_detach(void)
{
    struct task *t = task_current();
    quota_absorb_record_t rec = { .taken = { 0 }, .active = 0 };
    quota_block_t *job = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);

    if (!t || !t->quota || !job) {
        quota_block_deref(job);
        return;
    }

    uint64_t base = quota_usage(t->quota, QUOTA_RES_OBJECT_BODY);
    TEST_ASSERT_EQ((uint64_t)quota_charge(t->quota, QUOTA_RES_OBJECT_BODY, 9),
                   (uint64_t)STATUS_SUCCESS, "pre-join resource charged");
    TEST_ASSERT_EQ((uint64_t)quota_job_absorb_task(job, t, &rec),
                   (uint64_t)STATUS_SUCCESS, "join absorbs it");

    /* Release the pre-join resource while still a member. Its receipt named
     * only the process and user blocks, so the job keeps its copy. */
    TEST_ASSERT_EQ((uint64_t)quota_return(t->quota, QUOTA_RES_OBJECT_BODY, 9),
                   (uint64_t)STATUS_SUCCESS, "pre-join resource released");
    TEST_ASSERT_EQ(quota_usage(job, QUOTA_RES_OBJECT_BODY), base + 9,
                   "job still holds the absorbed copy until detach (documented gap)");

    quota_job_unabsorb(job, &rec);
    TEST_ASSERT_EQ(quota_usage(job, QUOTA_RES_OBJECT_BODY), 0ULL,
                   "detach releases it, so the gap is bounded by membership");
    quota_block_deref(job);
}

/* quota_task_init is idempotent (a task that already has a block keeps it) and
 * quota_task_teardown clears both pointers. Exercised on the CURRENT task with
 * save/restore rather than a synthetic one, because struct task is far larger
 * than the kmalloc size rule allows a test to allocate. */
static void test_quota_task_init_teardown_contract(void)
{
    struct task *t = task_current();
    struct quota_block *saved_proc, *saved_user;
    uint64_t flags;

    TEST_ASSERT_EQ((uint64_t)quota_task_init(NULL, NULL),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL task refused");
    quota_task_teardown(NULL);          /* must not fault */

    if (!t || !t->quota)
        return;

    /* Idempotence: a second init must NOT replace the live blocks (which would
     * strand every charge already recorded against them). */
    saved_proc = t->quota;
    saved_user = t->quota_user;
    TEST_ASSERT_EQ((uint64_t)quota_task_init(t, (struct access_token *)t->token),
                   (uint64_t)STATUS_SUCCESS, "re-init reports success");
    TEST_ASSERT(t->quota == saved_proc, "re-init kept the existing process block");
    TEST_ASSERT(t->quota_user == saved_user, "re-init kept the existing user block");

    /* Teardown clears both. Detach the pointers first so the real blocks are
     * not released underneath the rest of the suite, then restore them. */
    spin_lock_irqsave(&t->quota_lock, &flags);
    t->quota = (struct quota_block *)0;
    t->quota_user = (struct quota_block *)0;
    spin_unlock_irqrestore(&t->quota_lock, flags);

    quota_task_teardown(t);             /* already empty: must be a no-op */
    TEST_ASSERT_NULL((void *)t->quota, "teardown leaves the process block NULL");
    TEST_ASSERT_NULL((void *)t->quota_user, "teardown leaves the user block NULL");

    spin_lock_irqsave(&t->quota_lock, &flags);
    t->quota = saved_proc;
    t->quota_user = saved_user;
    spin_unlock_irqrestore(&t->quota_lock, flags);
}

/* A refused un-absorb must be COUNTED, not logged: it runs from the log-free,
 * elevated-IRQL death-teardown path. Forcing a refusal (a record claiming more
 * than the job holds) must advance the drift counter. */
static void test_quota_unabsorb_refusal_counted(void)
{
    quota_block_t *job = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);
    quota_absorb_record_t rec = { .taken = { 0 }, .active = 0 };
    uint64_t before;

    TEST_ASSERT_NOT_NULL((void *)job, "job block allocated");
    if (!job)
        return;

    before = quota_unabsorb_refused_count();

    /* The job holds nothing, so returning 5 is an over-return: refused. */
    rec.taken[QUOTA_RES_TIMER] = 5;
    rec.active = 1;
    quota_job_unabsorb(job, &rec);

    TEST_ASSERT_EQ(quota_unabsorb_refused_count(), before + 1,
                   "a refused un-absorb advances the drift counter");
    TEST_ASSERT_EQ(quota_usage(job, QUOTA_RES_TIMER), 0ULL,
                   "the refused return moved no usage");
    TEST_ASSERT_EQ((uint64_t)rec.active, 0ULL, "record consumed even when refused");
    quota_block_deref(job);
}

/* Argument validation for the chain API. */
static void test_quota_chain_bad_args(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0;

    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(NULL, QUOTA_RES_HANDLE, 1, 0, &r, &tok),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL task refused");
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_HANDLE, 1, 0xFFu, &r, &tok),
                   (uint64_t)STATUS_INVALID_PARAMETER, "unknown flag bits refused");
    /* Client charging fails CLOSED rather than approximating: reading the
     * executing thread's impersonation token is not SMP-safe yet, and billing
     * the wrong user is worse than refusing. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_HANDLE, 1,
                                                QUOTA_CHARGE_CLIENT, &r, &tok),
                   (uint64_t)STATUS_NOT_SUPPORTED,
                   "QUOTA_CHARGE_CLIENT refused pending a teardown-safe token pin");
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_HANDLE, 1, 0, NULL, &tok),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL receipt refused");
    /* A charge with nowhere to record its token would be unreturnable, so the
     * missing out-parameter is refused rather than silently charged. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_HANDLE, 1, 0, &r, NULL),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL out_token refused");
    /* Returning an untouched receipt is safe and does nothing. */
    quota_return_chain(&r, tok);
    quota_return_chain(NULL, tok);
}

/* An owner SID whose declared readable length cannot hold it is refused rather
 * than read past the caller's buffer. */
static void test_quota_block_owner_sid_truncated(void)
{
    uint8_t buf[SID_MAX_SIZE] = { 0 };
    SID *owner = (SID *)buf;
    const uint8_t nt_authority[6] = { 0, 0, 0, 0, 0, 5 };

    RtlInitializeSid(owner, nt_authority, 4);   /* needs 8 + 4*4 = 24 bytes */

    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, owner, 16);
    TEST_ASSERT_NULL((void *)b, "SID longer than the declared readable length is refused");

    b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, owner, 8);
    TEST_ASSERT_NULL((void *)b, "header-only readable length refuses a 4-subauthority SID");

    b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, owner, 24);
    TEST_ASSERT_NOT_NULL((void *)b, "exact readable length is accepted");
    quota_block_deref(b);
}

/* Transfer debits the source atomically, so a second transfer cannot spend the
 * same usage again. This is the deterministic stand-in for the two-CPU race:
 * with a pre-check-then-return design both transfers would succeed and the
 * destinations would jointly hold more than the source ever had. */
static void test_quota_transfer_cannot_duplicate(void)
{
    quota_block_t *src  = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    quota_block_t *dst1 = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    quota_block_t *dst2 = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)src, "src allocated");
    TEST_ASSERT_NOT_NULL((void *)dst1, "dst1 allocated");
    TEST_ASSERT_NOT_NULL((void *)dst2, "dst2 allocated");
    if (!src || !dst1 || !dst2) {
        quota_block_deref(src);
        quota_block_deref(dst1);
        quota_block_deref(dst2);
        return;
    }

    TEST_ASSERT_EQ((uint64_t)quota_charge(src, QUOTA_RES_TIMER, 10),
                   (uint64_t)STATUS_SUCCESS, "src charged 10");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(src, dst1, QUOTA_RES_TIMER, 10),
                   (uint64_t)STATUS_SUCCESS, "first transfer of the full amount succeeds");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(src, dst2, QUOTA_RES_TIMER, 10),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "second transfer finds src drained");

    /* Conservation: the total across all three blocks equals what was charged. */
    uint64_t total = quota_usage(src,  QUOTA_RES_TIMER)
                   + quota_usage(dst1, QUOTA_RES_TIMER)
                   + quota_usage(dst2, QUOTA_RES_TIMER);
    TEST_ASSERT_EQ(total, 10ULL, "transfers conserve total usage, never duplicate it");
    TEST_ASSERT_EQ(quota_usage(src, QUOTA_RES_TIMER), 0ULL, "src fully drained");
    TEST_ASSERT_EQ(quota_usage(dst2, QUOTA_RES_TIMER), 0ULL, "refused transfer credited nothing");

    quota_block_deref(src);
    quota_block_deref(dst1);
    quota_block_deref(dst2);
}

/* When the destination refuses, the source must be left exactly as it was.
 * (The shipped transfer never debits speculatively -- it commits to dst and
 * only then writes src, both under the two block locks -- so there is no
 * compensating write to verify, just the absence of any change.) */
static void test_quota_transfer_refusal_compensates(void)
{
    quota_block_t *src = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    quota_block_t *dst = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)src, "src allocated");
    TEST_ASSERT_NOT_NULL((void *)dst, "dst allocated");
    if (!src || !dst) {
        quota_block_deref(src);
        quota_block_deref(dst);
        return;
    }

    TEST_ASSERT_EQ((uint64_t)quota_charge(src, QUOTA_RES_THREAD, 20),
                   (uint64_t)STATUS_SUCCESS, "src charged 20");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(dst, QUOTA_RES_THREAD, 5),
                   (uint64_t)STATUS_SUCCESS, "dst capped at 5");

    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(src, dst, QUOTA_RES_THREAD, 20),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "dst cannot accept 20");
    TEST_ASSERT_EQ(quota_usage(src, QUOTA_RES_THREAD), 20ULL,
                   "src debit compensated exactly after the refusal");
    TEST_ASSERT_EQ(quota_usage(dst, QUOTA_RES_THREAD), 0ULL, "dst took nothing");

    quota_block_deref(src);
    quota_block_deref(dst);
}

/* A refused charge must leave usage exactly as it was and count exactly one
 * failure per refusal. (Check and commit share one critical section, so a
 * refusal never touches the counter in the first place.) */
static void test_quota_charge_refusal_is_exact(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(b, QUOTA_RES_MAPPED_VIEW, 30),
                   (uint64_t)STATUS_SUCCESS, "limit 30");
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_MAPPED_VIEW, 25),
                   (uint64_t)STATUS_SUCCESS, "charge 25 under the limit");

    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_MAPPED_VIEW, 10),
                       (uint64_t)STATUS_QUOTA_EXCEEDED, "over-limit charge refused");
    }
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_MAPPED_VIEW), 25ULL,
                   "three refusals left usage untouched at 25");
    TEST_ASSERT_EQ(quota_failures(b, QUOTA_RES_MAPPED_VIEW), 3ULL,
                   "exactly one failure counted per refusal");
    TEST_ASSERT_EQ(quota_peak(b, QUOTA_RES_MAPPED_VIEW), 25ULL,
                   "a refused charge never lifts the peak");
    quota_block_deref(b);
}

/* --- Interleaved multi-thread accounting invariants ---------------------- *
 * These suites run real worker threads against one shared block and assert
 * exact totals, so an implementation that loses updates across an interleaved
 * charge/return sequence fails them.
 *
 * SCOPE LIMIT, stated precisely because the difference matters: the scheduler
 * is single-CPU today (global task/thread cursor, no per-CPU run queues --
 * src/kernel/sched/task.c), so these workers INTERLEAVE on one CPU at yield
 * points; they do not execute a quota mutation simultaneously on two CPUs.
 * That means these tests do NOT prove the lock is what protects the
 * check-then-commit window -- a lock-free implementation could still pass them
 * whenever no preemption lands inside that short window. Genuine cross-CPU
 * contention proof needs per-CPU run queues plus an operation-level checkpoint
 * and is blocked on TODO-07 smp-phase2 per-CPU run queues. Do not upgrade
 * these comments to claim parallel coverage until that lands. */

#define QUOTA_RACE_WORKERS     3
#define QUOTA_RACE_ITERATIONS  200
#define QUOTA_RACE_SPIN_BUDGET 100000   /* bounded: never hang the sweep */

static quota_block_t *s_race_block;
static volatile int   s_race_workers_done;
static volatile int   s_race_started;      /* workers that reached the rendezvous */
static volatile int   s_race_inflight;     /* workers currently in the charge loop */
static volatile int   s_race_max_inflight; /* high-water of the above             */

static void quota_race_reset(void)
{
    s_race_workers_done = 0;
    s_race_started      = 0;
    s_race_inflight     = 0;
    s_race_max_inflight = 0;
}

/* Start rendezvous: block until every worker has arrived, so the charge loops
 * are all live at once instead of running back-to-back. Spawning threads alone
 * does not achieve that -- one worker can finish all its iterations before the
 * next is picked, which would make the totals vacuous.
 *
 * The spin budget bounds this WAIT only; the driver's later thread_join is
 * unbounded (thread_join has no timeout), so a worker that is created but
 * never scheduled would still hang the sweep. That is accepted here for the
 * same reason the existing kthread suites accept it: the scheduler is
 * flat-cyclic over all runnable threads, so a created thread is always
 * eventually picked. A bounded join needs a thread_join timeout that does not
 * exist yet; it stays open as the section 10 bounded-worker-join item, blocked
 * on TODO-07 smp-phase2. Where a test only needs to observe completion (rather
 * than reap the thread) the DONE-flag + spin-budget pattern in
 * test_quota_dashboard.c is the bounded alternative available today. */
static void quota_race_rendezvous(void)
{
    __atomic_fetch_add(&s_race_started, 1, __ATOMIC_ACQ_REL);
    for (uint32_t spin = 0; spin < QUOTA_RACE_SPIN_BUDGET; spin++) {
        if (__atomic_load_n(&s_race_started, __ATOMIC_ACQUIRE) >= QUOTA_RACE_WORKERS)
            break;
        thread_yield();
    }
}

/* Track how many workers are inside the charge loop at once. This is
 * worker-LIFETIME overlap, not per-operation contention: a worker is counted
 * for its whole loop, including while yielded between mutations. */
static void quota_race_enter(void)
{
    int cur = __atomic_add_fetch(&s_race_inflight, 1, __ATOMIC_ACQ_REL);
    int max = __atomic_load_n(&s_race_max_inflight, __ATOMIC_ACQUIRE);
    while (cur > max) {
        if (__atomic_compare_exchange_n(&s_race_max_inflight, &max, cur, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }
}

static void quota_race_exit(void)
{
    __atomic_fetch_sub(&s_race_inflight, 1, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&s_race_workers_done, 1, __ATOMIC_ACQ_REL);
}

/* Each worker charges 1 and returns 1, repeatedly. With correct serialization
 * the net effect is exactly zero and no update is ever lost. */
static void quota_race_charge_return_worker(void *arg)
{
    (void)arg;
    quota_race_rendezvous();
    quota_race_enter();
    for (uint32_t i = 0; i < QUOTA_RACE_ITERATIONS; i++) {
        if (quota_charge(s_race_block, QUOTA_RES_HANDLE, 1) == STATUS_SUCCESS)
            quota_return(s_race_block, QUOTA_RES_HANDLE, 1);
        thread_yield();          /* interleave inside the contested region */
    }
    quota_race_exit();
}

/* Each worker charges 1 and KEEPS it, so the final usage must be exactly the
 * number of successful charges -- the classic lost-update detector. */
static void quota_race_charge_only_worker(void *arg)
{
    (void)arg;
    quota_race_rendezvous();
    quota_race_enter();
    for (uint32_t i = 0; i < QUOTA_RACE_ITERATIONS; i++) {
        (void)quota_charge(s_race_block, QUOTA_RES_THREAD, 1);
        thread_yield();
    }
    quota_race_exit();
}

/* N interleaved charges from several threads must sum EXACTLY -- no lost
 * updates -- and the peak must equal the exact final total. */
static void test_quota_concurrent_charges_sum_exactly(void)
{
    s_race_block = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)s_race_block, "shared race block allocated");
    if (!s_race_block)
        return;

    quota_race_reset();
    int tids[QUOTA_RACE_WORKERS];
    for (int w = 0; w < QUOTA_RACE_WORKERS; w++) {
        tids[w] = kthread_create(quota_race_charge_only_worker, (void *)0, 0);
        TEST_ASSERT(tids[w] >= 0, "charge worker spawned");
    }
    for (int w = 0; w < QUOTA_RACE_WORKERS; w++) {
        if (tids[w] >= 0)
            thread_join((uint32_t)tids[w]);
    }

    uint64_t expected = (uint64_t)QUOTA_RACE_WORKERS * QUOTA_RACE_ITERATIONS;
    TEST_ASSERT_EQ((uint64_t)s_race_workers_done, (uint64_t)QUOTA_RACE_WORKERS,
                   "every worker ran to completion");
    TEST_ASSERT(s_race_max_inflight >= 2,
                "workers were live concurrently (interleaved, single-CPU scheduler)");
    TEST_ASSERT_EQ(quota_usage(s_race_block, QUOTA_RES_THREAD), expected,
                   "concurrent charges sum exactly (no lost updates)");
    TEST_ASSERT_EQ(quota_peak(s_race_block, QUOTA_RES_THREAD), expected,
                   "peak equals the exact final total");

    quota_block_deref(s_race_block);
    s_race_block = NULL;
}

/* Balanced concurrent charge/return traffic must settle back to exactly zero
 * with no residual, and the peak must never exceed the worker count. */
static void test_quota_concurrent_charge_return_balances(void)
{
    s_race_block = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)s_race_block, "shared race block allocated");
    if (!s_race_block)
        return;

    quota_race_reset();
    int tids[QUOTA_RACE_WORKERS];
    for (int w = 0; w < QUOTA_RACE_WORKERS; w++) {
        tids[w] = kthread_create(quota_race_charge_return_worker, (void *)0, 0);
        TEST_ASSERT(tids[w] >= 0, "charge/return worker spawned");
    }
    for (int w = 0; w < QUOTA_RACE_WORKERS; w++) {
        if (tids[w] >= 0)
            thread_join((uint32_t)tids[w]);
    }

    TEST_ASSERT_EQ((uint64_t)s_race_workers_done, (uint64_t)QUOTA_RACE_WORKERS,
                   "every worker ran to completion");
    TEST_ASSERT(s_race_max_inflight >= 2,
                "workers were live concurrently (interleaved, single-CPU scheduler)");
    TEST_ASSERT_EQ(quota_usage(s_race_block, QUOTA_RES_HANDLE), 0ULL,
                   "balanced concurrent traffic leaves no residual usage");
    TEST_ASSERT_EQ(quota_test_raw_usage(s_race_block, QUOTA_RES_HANDLE), (int64_t)0,
                   "the raw counter is exactly 0, never driven negative");
    TEST_ASSERT(quota_peak(s_race_block, QUOTA_RES_HANDLE) <= (uint64_t)QUOTA_RACE_WORKERS,
                "peak never exceeds the number of simultaneous holders");

    quota_block_deref(s_race_block);
    s_race_block = NULL;
}

/* A shared cap under concurrent contention admits EXACTLY the cap and refuses
 * the rest: successes + failures must account for every attempt. */
static void test_quota_concurrent_limit_is_exact(void)
{
    s_race_block = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)s_race_block, "shared race block allocated");
    if (!s_race_block)
        return;

    const uint64_t cap = 50;
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(s_race_block, QUOTA_RES_THREAD, cap),
                   (uint64_t)STATUS_SUCCESS, "cap the shared block");

    quota_race_reset();
    int tids[QUOTA_RACE_WORKERS];
    for (int w = 0; w < QUOTA_RACE_WORKERS; w++) {
        tids[w] = kthread_create(quota_race_charge_only_worker, (void *)0, 0);
        TEST_ASSERT(tids[w] >= 0, "contending worker spawned");
    }
    for (int w = 0; w < QUOTA_RACE_WORKERS; w++) {
        if (tids[w] >= 0)
            thread_join((uint32_t)tids[w]);
    }

    TEST_ASSERT(s_race_max_inflight >= 2,
                "workers were live concurrently while contending for the cap");

    uint64_t attempts  = (uint64_t)QUOTA_RACE_WORKERS * QUOTA_RACE_ITERATIONS;
    uint64_t usage     = quota_usage(s_race_block, QUOTA_RES_THREAD);
    uint64_t failures  = quota_failures(s_race_block, QUOTA_RES_THREAD);

    TEST_ASSERT_EQ(usage, cap, "contended cap admits exactly the limit, never more");
    TEST_ASSERT_EQ(usage + failures, attempts,
                   "every attempt is accounted for as either a charge or a failure");

    quota_block_deref(s_race_block);
    s_race_block = NULL;
}

/* --- Corrupted-state and saturation fixtures ----------------------------- */

/* A transfer from a corrupted (negative) source must be reported as an
 * integrity failure, not as an ordinary "insufficient balance" refusal --
 * otherwise a corrupted block hides behind a routine-looking status. */
static void test_quota_transfer_corrupt_source(void)
{
    quota_block_t *src = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    quota_block_t *dst = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)src, "src allocated");
    TEST_ASSERT_NOT_NULL((void *)dst, "dst allocated");
    if (!src || !dst) {
        quota_block_deref(src);
        quota_block_deref(dst);
        return;
    }

    quota_test_poke_usage(src, QUOTA_RES_TIMER, -3);
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(src, dst, QUOTA_RES_TIMER, 1),
                   (uint64_t)STATUS_INTEGER_OVERFLOW,
                   "corrupted source reports an integrity failure, not QUOTA_EXCEEDED");
    TEST_ASSERT_EQ(quota_failures(src, QUOTA_RES_TIMER), 1ULL,
                   "the corrupted source records the failure");
    TEST_ASSERT_EQ(quota_test_raw_usage(src, QUOTA_RES_TIMER), (int64_t)-3,
                   "the corrupted counter is left untouched");
    TEST_ASSERT_EQ(quota_usage(dst, QUOTA_RES_TIMER), 0ULL, "dst credited nothing");

    quota_block_deref(src);
    quota_block_deref(dst);
}

/* A corrupted (negative) usage counter fails closed instead of accounting
 * against nonsense, and the corrupted value is left exactly as found. */
static void test_quota_negative_usage_fails_closed(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    quota_test_poke_usage(b, QUOTA_RES_PROCESS, -5);
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_PROCESS, 1),
                   (uint64_t)STATUS_INTEGER_OVERFLOW, "charge on a negative counter fails closed");
    TEST_ASSERT_EQ(quota_test_raw_usage(b, QUOTA_RES_PROCESS), (int64_t)-5,
                   "the corrupted counter was not modified");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_PROCESS), 0ULL,
                   "the public query clamps a negative counter to 0");
    TEST_ASSERT_EQ(quota_failures(b, QUOTA_RES_PROCESS), 1ULL, "the refusal was counted");
    quota_block_deref(b);
}

/* The diagnostic failure counter saturates at the domain max rather than
 * wrapping into the negative half and reporting nonsense forever after. */
static void test_quota_failure_counter_saturates(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(b, QUOTA_RES_SECTION, 1),
                   (uint64_t)STATUS_SUCCESS, "limit 1 so charges are refused");
    quota_test_poke_failures(b, QUOTA_RES_SECTION, QUOTA_AMOUNT_MAX - 1);

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_SECTION, 5),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "first refusal");
    TEST_ASSERT_EQ(quota_failures(b, QUOTA_RES_SECTION), (uint64_t)QUOTA_AMOUNT_MAX,
                   "failure counter reached the domain max");
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_SECTION, 5),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "second refusal");
    TEST_ASSERT_EQ(quota_failures(b, QUOTA_RES_SECTION), (uint64_t)QUOTA_AMOUNT_MAX,
                   "counter saturates: stays at max, never wraps negative");
    quota_block_deref(b);
}

/* An arithmetic-overflow refusal is counted like any other failure. */
static void test_quota_overflow_counts_failure(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_REGISTRY_BYTES,
                                          (uint64_t)QUOTA_AMOUNT_MAX),
                   (uint64_t)STATUS_SUCCESS, "charge the domain max");
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_REGISTRY_BYTES, 1),
                   (uint64_t)STATUS_INTEGER_OVERFLOW, "one more overflows");
    TEST_ASSERT_EQ(quota_failures(b, QUOTA_RES_REGISTRY_BYTES), 1ULL,
                   "an overflow refusal is counted as a failure");
    quota_block_deref(b);
}

/* Creation fails closed on allocation failure and on a malformed SID, and
 * accepts a maximum-length (15 sub-authority) SID exactly. */
static void test_quota_block_create_failure_paths(void)
{
    kmalloc_fail_next();
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NULL((void *)b, "allocation failure returns NULL, not a partial block");
    quota_block_t *leaked_on_failure = b;   /* NULL when the injection fired */

    uint8_t buf[SID_MAX_SIZE] = { 0 };
    SID *sid = (SID *)buf;
    const uint8_t nt_authority[6] = { 0, 0, 0, 0, 0, 5 };

    /* Bad revision. */
    RtlInitializeSid(sid, nt_authority, 1);
    sid->Revision = 7;
    TEST_ASSERT_NULL((void *)quota_block_create(QUOTA_PRINCIPAL_PROCESS, sid, sizeof(buf)),
                     "malformed revision is refused");

    /* Sub-authority count beyond the architectural maximum. */
    RtlInitializeSid(sid, nt_authority, 1);
    sid->SubAuthorityCount = SID_MAX_SUB_AUTHORITIES + 1;
    TEST_ASSERT_NULL((void *)quota_block_create(QUOTA_PRINCIPAL_PROCESS, sid, sizeof(buf)),
                     "out-of-range sub-authority count is refused");

    /* Maximum valid SID: 15 sub-authorities, exactly SID_MAX_SIZE bytes. */
    RtlInitializeSid(sid, nt_authority, SID_MAX_SUB_AUTHORITIES);
    for (uint32_t i = 0; i < SID_MAX_SUB_AUTHORITIES; i++) {
        uint32_t *sub = RtlSubAuthoritySid(sid, i);
        if (sub)
            *sub = i + 1;
    }
    TEST_ASSERT_EQ((uint64_t)RtlLengthSid(sid), (uint64_t)SID_MAX_SIZE,
                   "the fixture really is a maximum-length SID");

    b = quota_block_create(QUOTA_PRINCIPAL_PROCESS, sid, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)b, "a maximum-length SID is accepted");
    if (b) {
        const SID *got = quota_block_owner(b);
        TEST_ASSERT_NOT_NULL((void *)got, "owner reported back");
        if (got)
            TEST_ASSERT(RtlEqualSid(got, sid) != 0, "maximum-length SID copied intact");
        quota_block_deref(b);
    }

    /* Any block a refusal-path assertion above unexpectedly produced must be
     * released, or a FAILING run also reports a leak and masks the real cause. */
    quota_block_deref(leaked_on_failure);
}

/* Remaining public boundary matrix: the argument contracts each entry point
 * owns separately from the shared bad-argument sweep. */
static void test_quota_boundary_matrix(void)
{
    quota_block_t *b   = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    quota_block_t *dst = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    TEST_ASSERT_NOT_NULL((void *)dst, "dst allocated");
    if (!b || !dst) {
        quota_block_deref(b);
        quota_block_deref(dst);
        return;
    }
    quota_resource_type_t bad = (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT;

    /* quota_return boundaries. */
    TEST_ASSERT_EQ((uint64_t)quota_return(b, QUOTA_RES_HANDLE, 0),
                   (uint64_t)STATUS_SUCCESS, "zero return is a no-op success");
    TEST_ASSERT_EQ((uint64_t)quota_return(b, QUOTA_RES_HANDLE,
                                          (uint64_t)QUOTA_AMOUNT_MAX + 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "oversized return rejected");

    /* quota_try_transfer boundaries. */
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(b, dst, bad, 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "transfer with a bad type rejected");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(b, dst, QUOTA_RES_HANDLE, 0),
                   (uint64_t)STATUS_SUCCESS, "zero transfer is a no-op success");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(b, dst, QUOTA_RES_HANDLE,
                                                (uint64_t)QUOTA_AMOUNT_MAX + 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "oversized transfer rejected");

    /* Exact-depletion transfer drains the source and credits only the amount. */
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_HANDLE, 12),
                   (uint64_t)STATUS_SUCCESS, "src charged 12");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(b, dst, QUOTA_RES_HANDLE, 12),
                   (uint64_t)STATUS_SUCCESS, "exact-amount transfer succeeds");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_HANDLE), 0ULL, "src drained to exactly 0");
    TEST_ASSERT_EQ(quota_usage(dst, QUOTA_RES_HANDLE), 12ULL, "dst credited exactly 12");

    /* A transfer whose destination would overflow fails and restores the source. */
    quota_test_poke_usage(dst, QUOTA_RES_HANDLE, QUOTA_AMOUNT_MAX);
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_HANDLE, 4),
                   (uint64_t)STATUS_SUCCESS, "src charged 4 again");
    TEST_ASSERT_EQ((uint64_t)quota_try_transfer(b, dst, QUOTA_RES_HANDLE, 4),
                   (uint64_t)STATUS_INTEGER_OVERFLOW, "dst arithmetic overflow refuses");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_HANDLE), 4ULL,
                   "src restored after the destination overflow");

    /* quota_set_limit boundaries. */
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(NULL, QUOTA_RES_HANDLE, 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "set_limit on NULL rejected");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(b, bad, 1),
                   (uint64_t)STATUS_INVALID_PARAMETER, "set_limit with a bad type rejected");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(b, QUOTA_RES_THREAD,
                                             (uint64_t)QUOTA_AMOUNT_MAX),
                   (uint64_t)STATUS_SUCCESS, "the domain max is a valid limit");
    TEST_ASSERT_EQ(quota_limit(b, QUOTA_RES_THREAD), (uint64_t)QUOTA_AMOUNT_MAX,
                   "domain-max limit reads back");

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(b, QUOTA_RES_THREAD, 2),
                   (uint64_t)STATUS_SUCCESS, "narrow the limit to 2");
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_THREAD, 3),
                   (uint64_t)STATUS_QUOTA_EXCEEDED, "the finite cap is enforced");
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(b, QUOTA_RES_THREAD, QUOTA_LIMIT_UNLIMITED),
                   (uint64_t)STATUS_SUCCESS, "setting 0 removes the cap");
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_THREAD, 3),
                   (uint64_t)STATUS_SUCCESS, "the same charge now succeeds");

    quota_block_deref(b);
    quota_block_deref(dst);
}

/* ============================================================================
 * Section 7: CPU, I/O, and wakeup accounting + rate-limit records
 *
 * The metric counters live on `struct task` and are written by lock-free
 * RELAXED atomics from their canonical event sites. These tests drive the
 * PUBLIC counting seams (task_acct_note_*) against the CURRENT task and assert
 * the observable deltas, rather than reaching into scheduler-private state --
 * which is exactly the property section 7 promises power/health policy.
 * ========================================================================== */

/* CPU time is charged by the timer tick, which a test may not drive. What IS
 * assertable without live boot infrastructure: the split is exposed per task,
 * both halves are readable through the sampling API, and a sample never walks
 * backwards relative to an earlier one (monotonicity is the contract policy
 * depends on when it divides two samples). */
static void test_quota_cpu_split_monotonic(void)
{
    struct task *self = task_current();
    task_acct_sample_t a, b;

    TEST_ASSERT(self != (struct task *)0, "test runs with a current task");

    task_acct_sample(self, &a);
    task_acct_sample(self, &b);

    TEST_ASSERT(b.user_time_ns >= a.user_time_ns,
                "user CPU time never decreases between samples");
    TEST_ASSERT(b.kernel_time_ns >= a.kernel_time_ns,
                "kernel CPU time never decreases between samples");
    TEST_ASSERT(b.timestamp_ns >= a.timestamp_ns,
                "sample timestamps advance monotonically");
}

/* Read, write, and control are SEPARATE dimensions: counting a control op must
 * not disturb the read or write counters. */
static void test_quota_io_split_by_op(void)
{
    struct task *self = task_current();
    task_acct_sample_t before, after;

    task_acct_sample(self, &before);
    task_acct_note_control_io(self, 4096ull);
    task_acct_sample(self, &after);

    TEST_ASSERT_EQ(after.io_other_count, before.io_other_count + 1ull,
                   "one control op counted");
    TEST_ASSERT_EQ(after.io_other_bytes, before.io_other_bytes + 4096ull,
                   "control bytes accumulated");
    TEST_ASSERT_EQ(after.io_read_count, before.io_read_count,
                   "control op does not touch the read op counter");
    TEST_ASSERT_EQ(after.io_write_count, before.io_write_count,
                   "control op does not touch the write op counter");
    TEST_ASSERT_EQ(after.io_read_bytes, before.io_read_bytes,
                   "control op does not touch read bytes");
    TEST_ASSERT_EQ(after.io_write_bytes, before.io_write_bytes,
                   "control op does not touch write bytes");
}

/* A zero-byte control still occupied the device: it counts as an OPERATION and
 * contributes no bytes. This is the boundary the event contract calls out, and
 * getting it wrong silently changes every bytes-per-op figure derived later. */
static void test_quota_control_io_zero_byte_counts_op(void)
{
    struct task *self = task_current();
    task_acct_sample_t before, after;

    task_acct_sample(self, &before);
    task_acct_note_control_io(self, 0ull);
    task_acct_sample(self, &after);

    TEST_ASSERT_EQ(after.io_other_count, before.io_other_count + 1ull,
                   "zero-byte control counts as an operation");
    TEST_ASSERT_EQ(after.io_other_bytes, before.io_other_bytes,
                   "zero-byte control contributes no bytes");
}

/* ONLY a thread that was actually blocked counts as a wakeup. Every other
 * prior state reaches READY without a wait having been satisfied, and counting
 * those would inflate the wakeup rate battery policy reads. */
static void test_quota_wakeup_only_from_blocked(void)
{
    struct task *self = task_current();
    task_acct_sample_t before, after;

    task_acct_sample(self, &before);

    task_acct_note_wakeup(self, THREAD_BLOCKED);      /* the one counting case */
    task_acct_note_wakeup(self, THREAD_READY);        /* already runnable      */
    task_acct_note_wakeup(self, THREAD_RUNNING);      /* never waited          */
    task_acct_note_wakeup(self, THREAD_DEAD);         /* not a wait grant      */
    task_acct_note_wakeup(self, THREAD_FREE);         /* reaped slot           */
    task_acct_note_wakeup((struct task *)0, THREAD_BLOCKED);  /* NULL is a no-op */

    task_acct_sample(self, &after);

    TEST_ASSERT_EQ(after.wakeup_count, before.wakeup_count + 1ull,
                   "exactly one of six wake notifications counted");

    /* The wake seam itself rejects an out-of-range thread index rather than
     * writing past the thread array or counting a wakeup for a thread that
     * does not exist. Driven on the live task with an index above num_threads,
     * so nothing runnable is disturbed. */
    task_acct_sample(self, &before);
    task_wake_thread(self, self->num_threads);        /* one past the end */
    task_wake_thread(self, THREAD_MAX + 1u);          /* far out of range   */
    task_wake_thread((struct task *)0, 0);            /* NULL task          */
    task_acct_sample(self, &after);
    TEST_ASSERT_EQ(after.wakeup_count, before.wakeup_count,
                   "out-of-range and NULL wake targets count nothing");
}

/* The sample is fully written and carries its own timestamp; a NULL task
 * yields an all-zero sample rather than stack residue a caller would divide. */
static void test_quota_sample_stamped(void)
{
    task_acct_sample_t s;

    task_acct_sample(task_current(), &s);
    TEST_ASSERT(s.timestamp_ns != 0ull,
                "a real task's sample carries a nonzero timestamp");

    task_acct_sample((const struct task *)0, &s);
    TEST_ASSERT_EQ(s.timestamp_ns, 0ull, "NULL task samples to a zero stamp");
    TEST_ASSERT_EQ(s.user_time_ns, 0ull, "NULL task samples to zero CPU time");
    TEST_ASSERT_EQ(s.io_other_count, 0ull, "NULL task samples to zero control ops");
    TEST_ASSERT_EQ(s.wakeup_count, 0ull, "NULL task samples to zero wakeups");
}

/* THE membership-interval invariant: a job aggregate must cover what a member
 * did WHILE ASSOCIATED, so usage accumulated before the baseline was captured
 * is excluded. Driven through the same delta helper ob_job uses, against a
 * synthetic baseline -- no job assignment or boot infrastructure required. */
static void test_quota_membership_delta_excludes_prejoin(void)
{
    struct task *self = task_current();
    struct task_acct_base base, delta;

    /* Pre-join traffic: accumulated BEFORE the baseline is captured. */
    task_acct_note_control_io(self, 1000ull);

    task_acct_capture_base(self, &base);          /* "join" happens here */

    /* Post-join traffic: exactly this much must appear in the delta. */
    task_acct_note_control_io(self, 250ull);
    task_acct_note_control_io(self, 750ull);

    task_acct_delta_since(self, &base, &delta);

    TEST_ASSERT_EQ(delta.io_other_count, 2ull,
                   "delta counts the two post-baseline control ops only");
    TEST_ASSERT_EQ(delta.io_other_bytes, 1000ull,
                   "delta counts post-baseline bytes only, excluding pre-join");

    /* A baseline captured NOW yields a zero delta: nothing has happened since. */
    task_acct_capture_base(self, &base);
    task_acct_delta_since(self, &base, &delta);
    TEST_ASSERT_EQ(delta.io_other_count, 0ull,
                   "a fresh baseline yields no contribution");
    TEST_ASSERT_EQ(delta.user_time_ns, 0ull,
                   "a fresh baseline yields no CPU contribution");
}

/* A baseline above the live counter means the counter was reset underneath it.
 * Saturating at zero loses one member's contribution; wrapping would add ~2^64
 * to a job's aggregate and destroy every rate derived from it. */
static void test_quota_delta_saturates_on_reset(void)
{
    struct task *self = task_current();
    struct task_acct_base base, delta;

    task_acct_capture_base(self, &base);
    base.io_other_count = base.io_other_count + 1000ull;   /* impossible future */
    base.user_time_ns   = base.user_time_ns + 1000ull;

    task_acct_delta_since(self, &base, &delta);

    TEST_ASSERT_EQ(delta.io_other_count, 0ull,
                   "inverted control-op baseline saturates to zero, not 2^64");
    TEST_ASSERT_EQ(delta.user_time_ns, 0ull,
                   "inverted CPU baseline saturates to zero, not 2^64");

    /* A NULL baseline means "no baseline": the whole lifetime counts. */
    task_acct_delta_since(self, (const struct task_acct_base *)0, &delta);
    task_acct_capture_base(self, &base);
    TEST_ASSERT_EQ(delta.io_other_count, base.io_other_count,
                   "a NULL baseline reports the full lifetime");
}

/* Start every policy fixture from a defined state: the validator rejects a
 * non-zero `reserved0`, and an unflagged envelope's amounts are never read, so
 * a stack-garbage record would produce confusing refusals rather than testing
 * the rule under examination. */
static void quota_rate_zero(quota_rate_limit_t *r)
{
    uint32_t i;
    for (i = 0; i < sizeof(*r); i++)
        ((uint8_t *)r)[i] = 0;
}

/* A published policy comes back field-for-field, and the record is stored
 * DISTINCTLY from cumulative usage -- that separation is the section's claim. */
static void test_quota_rate_record_roundtrip(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS,
                                          (const SID *)0, 0);
    quota_rate_limit_t rec, got;

    TEST_ASSERT(b != (quota_block_t *)0, "block created for rate policy test");

    quota_rate_zero(&rec);
    rec.version             = QUOTA_RATE_VERSION;
    rec.period_ns           = 100000000ull;       /* 100 ms window */
    rec.primary.flags       = QUOTA_RATE_F_WEIGHT | QUOTA_RATE_F_RESERVATION |
                              QUOTA_RATE_F_MAX | QUOTA_RATE_F_HARD_CAP;
    rec.primary.weight      = 1024ull;
    rec.primary.reservation = 10000000ull;        /* 10 ms floor  */
    rec.primary.max         = 40000000ull;        /* 40 ms soft   */
    rec.primary.hard_cap    = 50000000ull;        /* 50 ms hard   */
    rec.generation          = 999ull;             /* ignored on input */

    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_CPU, &rec),
                   (uint64_t)STATUS_SUCCESS, "valid CPU rate policy accepted");

    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_CPU, &got),
                   (uint64_t)STATUS_SUCCESS, "policy read back");
    TEST_ASSERT_EQ((uint64_t)got.version, (uint64_t)QUOTA_RATE_VERSION,
                   "version preserved");
    TEST_ASSERT_EQ(got.period_ns, 100000000ull, "period preserved");
    TEST_ASSERT_EQ(got.primary.weight, 1024ull, "weight preserved");
    TEST_ASSERT_EQ(got.primary.reservation, 10000000ull, "reservation preserved");
    TEST_ASSERT_EQ(got.primary.max, 40000000ull, "max preserved");
    TEST_ASSERT_EQ(got.primary.hard_cap, 50000000ull, "hard cap preserved");
    TEST_ASSERT(got.generation != 999ull,
                "generation is assigned by the publisher, not the caller");

    /* The unit is DERIVED from class + envelope, never stored, so it cannot
     * disagree with the class it was published against. */
    TEST_ASSERT_EQ((uint64_t)quota_rate_envelope_unit(QUOTA_RATE_CLASS_CPU, 0),
                   (uint64_t)QUOTA_RATE_UNIT_NS, "CPU primary is runtime ns");
    TEST_ASSERT_EQ((uint64_t)quota_rate_envelope_unit(QUOTA_RATE_CLASS_IO_READ, 0),
                   (uint64_t)QUOTA_RATE_UNIT_OPS, "I/O primary is operations");
    TEST_ASSERT_EQ((uint64_t)quota_rate_envelope_unit(QUOTA_RATE_CLASS_IO_READ, 1),
                   (uint64_t)QUOTA_RATE_UNIT_BYTES, "I/O bytes envelope is bytes");

    /* Each class is independent: setting CPU leaves I/O unset. */
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_IO_WRITE, &got),
                   (uint64_t)STATUS_SUCCESS, "unset class still reads");
    TEST_ASSERT_EQ((uint64_t)got.primary.flags, 0ull,
                   "a CPU policy does not leak into the write-I/O class");

    quota_block_deref(b);
}

/* Every one of the eight baseline fields must be captured and subtracted
 * independently. The production code hand-writes eight separate subtractions,
 * so a single copy/paste field swap would corrupt per-job CPU or I/O totals
 * while leaving a test that only checks one or two fields perfectly green.
 * Distinct sentinel values per field make a swap impossible to miss. */
static void test_quota_delta_covers_every_field(void)
{
    struct task_acct_base base, now, delta;

    /* Synthetic fixture: base and "current" are plain structs, so this
     * exercises the arithmetic without perturbing the live task. */
    base.user_time_ns   = 100ull;  base.kernel_time_ns = 200ull;
    base.io_read_count  = 300ull;  base.io_read_bytes  = 400ull;
    base.io_write_count = 500ull;  base.io_write_bytes = 600ull;
    base.io_other_count = 700ull;  base.io_other_bytes = 800ull;
    base.wakeup_count   = 900ull;  base.timer_create_count = 1000ull;

    /* A distinct increment per field: if two fields were transposed, the
     * expected values below would not match. */
    now.user_time_ns   = base.user_time_ns   + 1ull;
    now.kernel_time_ns = base.kernel_time_ns + 2ull;
    now.io_read_count  = base.io_read_count  + 3ull;
    now.io_read_bytes  = base.io_read_bytes  + 4ull;
    now.io_write_count = base.io_write_count + 5ull;
    now.io_write_bytes = base.io_write_bytes + 6ull;
    now.io_other_count = base.io_other_count + 7ull;
    now.io_other_bytes = base.io_other_bytes + 8ull;
    now.wakeup_count   = base.wakeup_count   + 9ull;
    now.timer_create_count = base.timer_create_count + 10ull;

    task_acct_delta_fields(&now, &base, &delta);
    TEST_ASSERT_EQ(delta.user_time_ns,   1ull, "user_time_ns delta is its own field");
    TEST_ASSERT_EQ(delta.kernel_time_ns, 2ull, "kernel_time_ns delta is its own field");
    TEST_ASSERT_EQ(delta.io_read_count,  3ull, "io_read_count delta is its own field");
    TEST_ASSERT_EQ(delta.io_read_bytes,  4ull, "io_read_bytes delta is its own field");
    TEST_ASSERT_EQ(delta.io_write_count, 5ull, "io_write_count delta is its own field");
    TEST_ASSERT_EQ(delta.io_write_bytes, 6ull, "io_write_bytes delta is its own field");
    TEST_ASSERT_EQ(delta.io_other_count, 7ull, "io_other_count delta is its own field");
    TEST_ASSERT_EQ(delta.io_other_bytes, 8ull, "io_other_bytes delta is its own field");
    TEST_ASSERT_EQ(delta.wakeup_count,   9ull, "wakeup_count delta is its own field");
    TEST_ASSERT_EQ(delta.timer_create_count, 10ull,
                   "timer_create_count delta is its own field");

    /* Equal baseline: every field contributes nothing. */
    task_acct_delta_fields(&base, &base, &delta);
    TEST_ASSERT_EQ(delta.user_time_ns,   0ull, "equal baseline yields no CPU user delta");
    TEST_ASSERT_EQ(delta.kernel_time_ns, 0ull, "equal baseline yields no CPU kernel delta");
    TEST_ASSERT_EQ(delta.io_read_count,  0ull, "equal baseline yields no read-op delta");
    TEST_ASSERT_EQ(delta.io_read_bytes,  0ull, "equal baseline yields no read-byte delta");
    TEST_ASSERT_EQ(delta.io_write_count, 0ull, "equal baseline yields no write-op delta");
    TEST_ASSERT_EQ(delta.io_write_bytes, 0ull, "equal baseline yields no write-byte delta");
    TEST_ASSERT_EQ(delta.io_other_count, 0ull, "equal baseline yields no control-op delta");
    TEST_ASSERT_EQ(delta.io_other_bytes, 0ull, "equal baseline yields no control-byte delta");
    TEST_ASSERT_EQ(delta.wakeup_count,   0ull, "equal baseline yields no wakeup delta");
    TEST_ASSERT_EQ(delta.timer_create_count, 0ull, "equal baseline yields no timer delta");

    /* Baseline ABOVE current in EVERY field: all saturate to 0, none wrap. */
    task_acct_delta_fields(&base, &now, &delta);
    TEST_ASSERT_EQ(delta.user_time_ns,   0ull, "inverted user_time_ns saturates");
    TEST_ASSERT_EQ(delta.kernel_time_ns, 0ull, "inverted kernel_time_ns saturates");
    TEST_ASSERT_EQ(delta.io_read_count,  0ull, "inverted io_read_count saturates");
    TEST_ASSERT_EQ(delta.io_read_bytes,  0ull, "inverted io_read_bytes saturates");
    TEST_ASSERT_EQ(delta.io_write_count, 0ull, "inverted io_write_count saturates");
    TEST_ASSERT_EQ(delta.io_write_bytes, 0ull, "inverted io_write_bytes saturates");
    TEST_ASSERT_EQ(delta.io_other_count, 0ull, "inverted io_other_count saturates");
    TEST_ASSERT_EQ(delta.io_other_bytes, 0ull, "inverted io_other_bytes saturates");
    TEST_ASSERT_EQ(delta.wakeup_count,   0ull, "inverted wakeup_count saturates");
    TEST_ASSERT_EQ(delta.timer_create_count, 0ull, "inverted timer_create_count saturates");

    /* A NULL task captures a fully zeroed baseline, not stack residue. */
    task_acct_capture_base((const struct task *)0, &now);
    TEST_ASSERT_EQ(now.user_time_ns,   0ull, "NULL task captures zero CPU time");
    TEST_ASSERT_EQ(now.io_read_bytes,  0ull, "NULL task captures zero read bytes");
    TEST_ASSERT_EQ(now.io_write_bytes, 0ull, "NULL task captures zero write bytes");
    TEST_ASSERT_EQ(now.io_other_bytes, 0ull, "NULL task captures zero control bytes");
}

/* quota_rate_limit_get promises a BOUNDED retry and, on exhaustion,
 * STATUS_RETRY with the caller's buffer byte-for-byte untouched. That branch
 * needs a publisher stalled mid-update, which a single-threaded test cannot
 * produce -- so the sequence is forced odd through the test-only seam. */
static void test_quota_rate_get_retry_preserves_output(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS,
                                          (const SID *)0, 0);
    quota_rate_limit_t rec, got;

    TEST_ASSERT(b != (quota_block_t *)0, "block created for retry test");

    quota_rate_zero(&rec);
    rec.version          = QUOTA_RATE_VERSION;
    rec.period_ns        = 1000000ull;
    rec.primary.flags    = QUOTA_RATE_F_HARD_CAP;
    rec.primary.hard_cap = 77ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_WRITE, &rec),
                   (uint64_t)STATUS_SUCCESS, "policy published for retry test");

    /* Seed the caller's buffer with a policy it must still hold afterwards. */
    quota_rate_zero(&got);
    got.version          = QUOTA_RATE_VERSION;
    got.period_ns        = 4242ull;
    got.primary.hard_cap = 999ull;
    got.bytes.hard_cap   = 555ull;

    quota_test_poke_rate_seq(b, QUOTA_RATE_CLASS_IO_WRITE, 1);   /* write in flight */
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_IO_WRITE, &got),
                   (uint64_t)STATUS_RETRY,
                   "a permanently in-flux class reports STATUS_RETRY, not a torn read");
    TEST_ASSERT_EQ(got.period_ns, 4242ull,
                   "STATUS_RETRY leaves the caller's period untouched");
    TEST_ASSERT_EQ(got.primary.hard_cap, 999ull,
                   "STATUS_RETRY leaves the caller's primary envelope untouched");
    TEST_ASSERT_EQ(got.bytes.hard_cap, 555ull,
                   "STATUS_RETRY leaves the caller's byte envelope untouched");

    /* Restored to stable: the real policy reads back. */
    quota_test_poke_rate_seq(b, QUOTA_RATE_CLASS_IO_WRITE, 0);
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_IO_WRITE, &got),
                   (uint64_t)STATUS_SUCCESS, "a stable class reads successfully again");
    TEST_ASSERT_EQ(got.primary.hard_cap, 77ull, "the published policy is returned");

    /* A stalled class must not block a DIFFERENT class: that is why the
     * sequence counter is per class rather than shared. */
    quota_test_poke_rate_seq(b, QUOTA_RATE_CLASS_IO_WRITE, 1);
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_CPU, &got),
                   (uint64_t)STATUS_SUCCESS,
                   "an in-flux class does not stall readers of another class");
    quota_test_poke_rate_seq(b, QUOTA_RATE_CLASS_IO_WRITE, 0);

    quota_block_deref(b);
}

/* Boundary matrix the earlier rejection test only sampled: exact-MAX
 * acceptance, equal-bound acceptance, each +1 inversion, both reserved fields,
 * the BYTES envelope's own range/ordering rules, and per-class isolation. */
static void test_quota_rate_boundary_matrix(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_JOB, (const SID *)0, 0);
    quota_rate_limit_t rec, got;
    uint32_t cls;

    TEST_ASSERT(b != (quota_block_t *)0, "block created for boundary matrix");

    /* Exact QUOTA_RATE_AMOUNT_MAX is IN the domain and must be accepted. */
    quota_rate_zero(&rec);
    rec.version          = QUOTA_RATE_VERSION;
    rec.period_ns        = 1000ull;
    rec.primary.flags    = QUOTA_RATE_F_HARD_CAP;
    rec.primary.hard_cap = (uint64_t)QUOTA_RATE_AMOUNT_MAX;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &rec),
                   (uint64_t)STATUS_SUCCESS, "exact QUOTA_RATE_AMOUNT_MAX accepted");

    /* Equal reservation == max == hard_cap is satisfiable, not an inversion. */
    quota_rate_zero(&rec);
    rec.version             = QUOTA_RATE_VERSION;
    rec.period_ns           = 1000ull;
    rec.primary.flags       = QUOTA_RATE_F_RESERVATION | QUOTA_RATE_F_MAX |
                              QUOTA_RATE_F_HARD_CAP;
    rec.primary.reservation = 50ull;
    rec.primary.max         = 50ull;
    rec.primary.hard_cap    = 50ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &rec),
                   (uint64_t)STATUS_SUCCESS, "equal reservation/max/hard-cap accepted");

    /* The BYTES envelope carries the same range and ordering rules. */
    quota_rate_zero(&rec);
    rec.version           = QUOTA_RATE_VERSION;
    rec.period_ns         = 1000ull;
    rec.bytes.flags       = QUOTA_RATE_F_HARD_CAP;
    rec.bytes.hard_cap    = (uint64_t)QUOTA_RATE_AMOUNT_MAX + 1ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &rec),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "out-of-domain amount in the BYTES envelope refused");

    quota_rate_zero(&rec);
    rec.version             = QUOTA_RATE_VERSION;
    rec.period_ns           = 1000ull;
    rec.bytes.flags         = QUOTA_RATE_F_RESERVATION | QUOTA_RATE_F_MAX;
    rec.bytes.reservation   = 900ull;
    rec.bytes.max           = 500ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &rec),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "reservation above max in the BYTES envelope refused");

    /* A per-period amount in the BYTES envelope alone still needs a period. */
    quota_rate_zero(&rec);
    rec.version        = QUOTA_RATE_VERSION;
    rec.period_ns      = 0ull;
    rec.bytes.flags    = QUOTA_RATE_F_HARD_CAP;
    rec.bytes.hard_cap = 10ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &rec),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "BYTES-only per-period amount without a period refused");

    /* Both reserved fields must be zero. */
    quota_rate_zero(&rec);
    rec.version          = QUOTA_RATE_VERSION;
    rec.period_ns        = 1000ull;
    rec.primary.flags    = QUOTA_RATE_F_HARD_CAP;
    rec.primary.hard_cap = 10ull;
    rec.primary.reserved = 1u;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &rec),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "non-zero primary-envelope reserved field refused");
    rec.primary.reserved = 0u;
    rec.bytes.reserved   = 1u;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &rec),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "non-zero bytes-envelope reserved field refused");

    /* get argument validation. */
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get((const quota_block_t *)0,
                                                  QUOTA_RATE_CLASS_CPU, &got),
                   (uint64_t)STATUS_INVALID_PARAMETER, "get refuses a NULL block");
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b,
                       (quota_rate_class_t)QUOTA_RATE_CLASS_COUNT, &got),
                   (uint64_t)STATUS_INVALID_PARAMETER, "get refuses a bad class");

    /* Per-class isolation: publish a distinct policy to every class, then
     * rewrite ONE and prove the other three are byte-for-byte unchanged. */
    for (cls = 0; cls < (uint32_t)QUOTA_RATE_CLASS_COUNT; cls++) {
        quota_rate_zero(&rec);
        rec.version          = QUOTA_RATE_VERSION;
        rec.period_ns        = 1000ull;
        rec.primary.flags    = QUOTA_RATE_F_HARD_CAP;
        rec.primary.hard_cap = 1000ull + cls;      /* distinct per class */
        TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, (quota_rate_class_t)cls, &rec),
                       (uint64_t)STATUS_SUCCESS, "per-class sentinel published");
    }

    quota_rate_zero(&rec);
    rec.version          = QUOTA_RATE_VERSION;
    rec.period_ns        = 2000ull;
    rec.primary.flags    = QUOTA_RATE_F_HARD_CAP;
    rec.primary.hard_cap = 4242ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_WRITE, &rec),
                   (uint64_t)STATUS_SUCCESS, "one class rewritten");

    for (cls = 0; cls < (uint32_t)QUOTA_RATE_CLASS_COUNT; cls++) {
        TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, (quota_rate_class_t)cls, &got),
                       (uint64_t)STATUS_SUCCESS, "class readable after the rewrite");
        if (cls == (uint32_t)QUOTA_RATE_CLASS_IO_WRITE)
            TEST_ASSERT_EQ(got.primary.hard_cap, 4242ull,
                           "the rewritten class holds the new policy");
        else
            TEST_ASSERT_EQ(got.primary.hard_cap, 1000ull + cls,
                           "an untouched class keeps its own policy");
    }

    quota_block_deref(b);
}

/* A published record is CANONICAL: amounts whose flags are clear come back as
 * zero rather than as whatever the caller happened to leave in the struct.
 * Without this, two logically identical policies differ byte-for-byte and a
 * future query syscall would copy uninitialized caller stack out of the
 * kernel. Driven with loud sentinels in every unflagged slot. */
static void test_quota_rate_unflagged_fields_canonicalized(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS,
                                          (const SID *)0, 0);
    quota_rate_limit_t rec, got;

    TEST_ASSERT(b != (quota_block_t *)0, "block created for canonicalization test");

    quota_rate_zero(&rec);
    rec.version       = QUOTA_RATE_VERSION;
    rec.period_ns     = 1000000ull;
    /* Only the hard cap is declared meaningful... */
    rec.primary.flags = QUOTA_RATE_F_HARD_CAP;
    rec.primary.hard_cap = 500ull;
    /* ...but every other amount carries a sentinel the publisher must drop. */
    rec.primary.weight      = 0xDEADBEEFull;
    rec.primary.reservation = 0xCAFEBABEull;
    rec.primary.max         = 0xFEEDFACEull;

    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &rec),
                   (uint64_t)STATUS_SUCCESS, "policy with unflagged sentinels accepted");
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_IO_READ, &got),
                   (uint64_t)STATUS_SUCCESS, "policy read back");

    TEST_ASSERT_EQ(got.primary.hard_cap, 500ull, "the flagged amount survives");
    TEST_ASSERT_EQ(got.primary.weight, 0ull, "unflagged weight canonicalized to 0");
    TEST_ASSERT_EQ(got.primary.reservation, 0ull,
                   "unflagged reservation canonicalized to 0");
    TEST_ASSERT_EQ(got.primary.max, 0ull, "unflagged max canonicalized to 0");
    TEST_ASSERT_EQ((uint64_t)got.primary.reserved, 0ull,
                   "the pinned ABI hole always reads back as zero");

    /* A CPU policy blanks the byte envelope entirely, flags and amounts both,
     * even though an unflagged byte envelope is accepted. */
    quota_rate_zero(&rec);
    rec.version          = QUOTA_RATE_VERSION;
    rec.period_ns        = 1000000ull;
    rec.primary.flags    = QUOTA_RATE_F_HARD_CAP;
    rec.primary.hard_cap = 42ull;
    rec.bytes.hard_cap   = 0x1234567ull;      /* unflagged sentinel */
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_CPU, &rec),
                   (uint64_t)STATUS_SUCCESS, "CPU policy with unflagged bytes accepted");
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_CPU, &got),
                   (uint64_t)STATUS_SUCCESS, "CPU policy read back");
    TEST_ASSERT_EQ((uint64_t)got.bytes.flags, 0ull, "CPU byte envelope has no flags");
    TEST_ASSERT_EQ(got.bytes.hard_cap, 0ull,
                   "CPU byte envelope is blanked, not carried through");

    quota_block_deref(b);
}

/* THE reason the record carries two envelopes: a storage QoS layer caps a
 * stream by IOPS *and* bytes/sec at once. A pure-IOPS cap lets one huge request
 * saturate the device; a pure-bandwidth cap lets a flood of tiny requests
 * exhaust the queue. Publishing one must not erase the other. */
static void test_quota_rate_ops_and_bytes_coexist(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_USER, (const SID *)0, 0);
    quota_rate_limit_t rec, got;

    TEST_ASSERT(b != (quota_block_t *)0, "block created for dual-cap test");

    quota_rate_zero(&rec);
    rec.version           = QUOTA_RATE_VERSION;
    rec.period_ns         = 1000000000ull;        /* 1 s window */
    rec.primary.flags     = QUOTA_RATE_F_HARD_CAP;
    rec.primary.hard_cap  = 500ull;               /* 500 IOPS      */
    rec.bytes.flags       = QUOTA_RATE_F_HARD_CAP;
    rec.bytes.hard_cap    = 4194304ull;           /* 4 MiB/s       */

    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &rec),
                   (uint64_t)STATUS_SUCCESS, "IOPS + bandwidth policy accepted");
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_IO_READ, &got),
                   (uint64_t)STATUS_SUCCESS, "dual-cap policy read back");
    TEST_ASSERT_EQ(got.primary.hard_cap, 500ull,
                   "the operations cap survived publication of the byte cap");
    TEST_ASSERT_EQ(got.bytes.hard_cap, 4194304ull,
                   "the byte cap survived publication of the operations cap");

    /* The two dimensions are independent: 500 ops and 4 MiB is coherent even
     * though the byte amount dwarfs the op amount. No cross-envelope ordering. */
    TEST_ASSERT(got.bytes.hard_cap > got.primary.hard_cap,
                "envelopes are independent dimensions, not an ordered pair");

    /* CPU has no byte dimension: the same record shape is refused there. */
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_CPU, &rec),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "a byte envelope on a CPU policy is refused");

    quota_block_deref(b);
}

/* Every validation rule refuses publication rather than storing a policy no
 * consumer could act on. */
static void test_quota_rate_record_rejects_bad(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS,
                                          (const SID *)0, 0);
    quota_rate_limit_t ok, bad, got;

    TEST_ASSERT(b != (quota_block_t *)0, "block created for rejection test");

    quota_rate_zero(&ok);
    ok.version          = QUOTA_RATE_VERSION;
    ok.period_ns        = 1000000ull;
    ok.primary.flags    = QUOTA_RATE_F_HARD_CAP;
    ok.primary.hard_cap = 500ull;

    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &ok),
                   (uint64_t)STATUS_SUCCESS, "baseline valid policy accepted");

    bad = ok; bad.version = QUOTA_RATE_VERSION + 1u;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &bad),
                   (uint64_t)STATUS_INVALID_PARAMETER, "unknown version refused");

    bad = ok; bad.reserved0 = 1u;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &bad),
                   (uint64_t)STATUS_INVALID_PARAMETER, "non-zero reserved field refused");

    bad = ok; bad.primary.flags = 0x80000000u;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &bad),
                   (uint64_t)STATUS_INVALID_PARAMETER, "unknown flag bit refused");

    bad = ok; bad.bytes.flags = 0x40000000u;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &bad),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "unknown flag bit in the BYTES envelope refused too");

    bad = ok; bad.period_ns = 0ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &bad),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "per-period amount without a period refused");

    bad = ok; bad.primary.hard_cap = (uint64_t)QUOTA_RATE_AMOUNT_MAX + 1ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &bad),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "amount outside the counter domain refused");

    bad = ok;
    bad.primary.flags = QUOTA_RATE_F_RESERVATION | QUOTA_RATE_F_HARD_CAP;
    bad.primary.reservation = 900ull; bad.primary.hard_cap = 500ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &bad),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "reservation above hard cap refused as unsatisfiable");

    bad = ok; bad.primary.flags = QUOTA_RATE_F_MAX | QUOTA_RATE_F_HARD_CAP;
    bad.primary.max = 900ull; bad.primary.hard_cap = 500ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &bad),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "soft max above hard cap refused");

    /* Ordering is checked WITHIN an envelope, never across the two: an op
     * reservation is not comparable to a byte cap. */
    bad = ok;
    bad.primary.flags = QUOTA_RATE_F_RESERVATION;
    bad.primary.reservation = 400ull;
    bad.bytes.flags = QUOTA_RATE_F_HARD_CAP;
    bad.bytes.hard_cap = 8ull;          /* far below the op reservation */
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &bad),
                   (uint64_t)STATUS_SUCCESS,
                   "envelopes are independent: no cross-dimension ordering rule");
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_READ, &ok),
                   (uint64_t)STATUS_SUCCESS, "baseline policy restored");

    /* CPU has no byte dimension: a flagged byte envelope there is refused. */
    bad = ok; bad.bytes.flags = QUOTA_RATE_F_HARD_CAP; bad.bytes.hard_cap = 1ull;
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_CPU, &bad),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "byte envelope on a CPU policy refused");

    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, (quota_rate_class_t)QUOTA_RATE_CLASS_COUNT, &ok),
                   (uint64_t)STATUS_INVALID_PARAMETER, "out-of-range class refused");
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_CPU,
                                                  (const quota_rate_limit_t *)0),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL record refused");
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set((quota_block_t *)0,
                                                  QUOTA_RATE_CLASS_CPU, &ok),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL block refused");
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_CPU,
                                                  (quota_rate_limit_t *)0),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL output refused");

    /* Every rejection left the stored policy exactly as the valid set made it. */
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_IO_READ, &got),
                   (uint64_t)STATUS_SUCCESS, "policy still readable");
    TEST_ASSERT_EQ(got.primary.hard_cap, 500ull,
                   "a refused publication does not disturb the stored policy");
    TEST_ASSERT_EQ((uint64_t)got.primary.flags, (uint64_t)QUOTA_RATE_F_HARD_CAP,
                   "refused publications did not alter the stored flags");

    quota_block_deref(b);
}

/* The generation is what lets a consumer notice a policy changed without
 * diffing every field, so it must advance on every successful publication. */
static void test_quota_rate_generation_advances(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_JOB, (const SID *)0, 0);
    quota_rate_limit_t rec, first, second, rejected;

    TEST_ASSERT(b != (quota_block_t *)0, "block created for generation test");

    quota_rate_zero(&rec);
    rec.version        = QUOTA_RATE_VERSION;
    rec.period_ns      = 0ull;            /* a bare weight needs no window */
    rec.primary.flags  = QUOTA_RATE_F_WEIGHT;
    rec.primary.weight = 100ull;

    quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_CONTROL, &rec);
    quota_rate_limit_get(b, QUOTA_RATE_CLASS_IO_CONTROL, &first);

    rec.primary.weight = 200ull;
    quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_CONTROL, &rec);
    quota_rate_limit_get(b, QUOTA_RATE_CLASS_IO_CONTROL, &second);

    TEST_ASSERT(second.generation > first.generation,
                "generation advances on each successful publication");
    TEST_ASSERT_EQ(second.primary.weight, 200ull,
                   "the newer policy is the one stored");

    /* A REFUSED publication must not advance the generation: a consumer would
     * re-read a policy that never changed. */
    rec.version = QUOTA_RATE_VERSION + 1u;
    quota_rate_limit_set(b, QUOTA_RATE_CLASS_IO_CONTROL, &rec);
    quota_rate_limit_get(b, QUOTA_RATE_CLASS_IO_CONTROL, &rejected);
    TEST_ASSERT_EQ(rejected.generation, second.generation,
                   "a refused publication does not advance the generation");

    quota_block_deref(b);
}

/* "No policy" is distinct from "a policy whose amounts are zero": the former
 * carries no flags, so a consumer can tell it must not enforce anything. */
static void test_quota_rate_unset_reports_no_policy(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_USER, (const SID *)0, 0);
    quota_rate_limit_t got;
    uint32_t cls;

    TEST_ASSERT(b != (quota_block_t *)0, "block created for unset-policy test");

    for (cls = 0; cls < (uint32_t)QUOTA_RATE_CLASS_COUNT; cls++) {
        TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, (quota_rate_class_t)cls, &got),
                       (uint64_t)STATUS_SUCCESS, "unset class reads successfully");
        TEST_ASSERT_EQ((uint64_t)got.primary.flags, 0ull,
                       "a fresh block carries no rate policy");
        TEST_ASSERT_EQ((uint64_t)got.bytes.flags, 0ull,
                       "a fresh block carries no byte-dimension policy either");
        TEST_ASSERT_EQ(got.generation, 0ull,
                       "an unset policy has generation 0");
    }

    quota_block_deref(b);
}

/* Rate policy and cumulative usage are separate state on the same block:
 * publishing a policy must not move a counter, and charging must not move the
 * policy. Conflating the two is exactly what the section separates. */
static void test_quota_rate_separate_from_usage(void)
{
    quota_block_t *b = quota_block_create(QUOTA_PRINCIPAL_PROCESS,
                                          (const SID *)0, 0);
    quota_rate_limit_t rec, got;
    uint64_t usage_before, usage_after;

    TEST_ASSERT(b != (quota_block_t *)0, "block created for separation test");

    usage_before = quota_usage(b, QUOTA_RES_HANDLE);

    quota_rate_zero(&rec);
    rec.version          = QUOTA_RATE_VERSION;
    rec.period_ns        = 1000000ull;
    rec.primary.flags    = QUOTA_RATE_F_HARD_CAP;
    rec.primary.hard_cap = 42ull;      /* CPU budgets are runtime ns */
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_set(b, QUOTA_RATE_CLASS_CPU, &rec),
                   (uint64_t)STATUS_SUCCESS, "policy published");

    usage_after = quota_usage(b, QUOTA_RES_HANDLE);
    TEST_ASSERT_EQ(usage_after, usage_before,
                   "publishing a rate policy charges no usage");

    /* And a charge leaves the policy untouched. */
    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_HANDLE, 5ull),
                   (uint64_t)STATUS_SUCCESS, "charge admitted");
    TEST_ASSERT_EQ((uint64_t)quota_rate_limit_get(b, QUOTA_RATE_CLASS_CPU, &got),
                   (uint64_t)STATUS_SUCCESS, "policy still readable after a charge");
    TEST_ASSERT_EQ(got.primary.hard_cap, 42ull,
                   "a charge does not alter the rate policy");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_HANDLE), usage_before + 5ull,
                   "the charge landed in usage, not in policy");

    TEST_ASSERT_EQ((uint64_t)quota_return(b, QUOTA_RES_HANDLE, 5ull),
                   (uint64_t)STATUS_SUCCESS, "charge returned");
    quota_block_deref(b);
}

void test_register_quota(void)
{
    test_suite_register_cat("Quota: registry ready at boot",
                            test_quota_registry_ready, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: type count matches enum",
                            test_quota_type_count, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: all types valid",
                            test_quota_all_types_valid, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: out-of-range accessors safe",
                            test_quota_out_of_range_safe, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: units all rows",
                            test_quota_units_all_rows, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: default limits unlimited",
                            test_quota_default_limits_unlimited, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: override privilege all rows",
                            test_quota_override_privilege_all_rows, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: names unique",
                            test_quota_names_unique, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: dump smoke",
                            test_quota_dump_smoke, TEST_CAT_QUOTA);

    /* Section 2: quota blocks and the charge API */
    test_suite_register_cat("Quota: block create zeroed",
                            test_quota_block_create_zeroed, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: charge/return round-trip",
                            test_quota_charge_return_roundtrip, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: over-limit charge refused",
                            test_quota_charge_over_limit_refused, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: unlimited allows large charge",
                            test_quota_unlimited_allows_large_charge, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: overflow domain guarded",
                            test_quota_overflow_domain_guarded, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: return underflow refused",
                            test_quota_return_underflow_refused, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: stale return cannot erase newer charge",
                            test_quota_stale_return_cannot_erase_newer_charge, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: lowering limit keeps committed",
                            test_quota_lowering_limit_keeps_committed, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: transfer moves and refuses",
                            test_quota_transfer_moves_and_refuses, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: self-transfer is a no-op",
                            test_quota_transfer_self_is_noop, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: charge API bad args",
                            test_quota_charge_api_bad_args, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: block refcount lifetime",
                            test_quota_block_refcount, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: block owner SID copied",
                            test_quota_block_owner_sid, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: owner SID truncation refused",
                            test_quota_block_owner_sid_truncated, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: transfer cannot duplicate usage",
                            test_quota_transfer_cannot_duplicate, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: transfer refusal compensates src",
                            test_quota_transfer_refusal_compensates, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: charge refusal is exact",
                            test_quota_charge_refusal_is_exact, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: concurrent charges sum exactly",
                            test_quota_concurrent_charges_sum_exactly, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: concurrent charge/return balances",
                            test_quota_concurrent_charge_return_balances, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: concurrent limit is exact",
                            test_quota_concurrent_limit_is_exact, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: transfer from corrupt source",
                            test_quota_transfer_corrupt_source, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: negative usage fails closed",
                            test_quota_negative_usage_fails_closed, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: failure counter saturates",
                            test_quota_failure_counter_saturates, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: overflow counts a failure",
                            test_quota_overflow_counts_failure, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: block create failure paths",
                            test_quota_block_create_failure_paths, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: public boundary matrix",
                            test_quota_boundary_matrix, TEST_CAT_QUOTA);

    /* Section 3: process/token/job ownership model */
    test_suite_register_cat("Quota: principal recorded per block",
                            test_quota_principal_recorded, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: user block canonical per SID",
                            test_quota_user_block_canonical, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: try_ref pins a live block",
                            test_quota_try_ref_live_block, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: rollup counts user layer once",
                            test_quota_rollup_counts_user_layer_once, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: rollup bad args",
                            test_quota_rollup_bad_args, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: task carries process+user blocks",
                            test_quota_task_has_blocks, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: chain charge round-trip",
                            test_quota_chain_charge_roundtrip, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: chain charge all-or-nothing",
                            test_quota_chain_all_or_nothing, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: chain return is idempotent",
                            test_quota_chain_return_idempotent, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: chain charge bad args",
                            test_quota_chain_bad_args, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: live receipt reuse refused",
                            test_quota_chain_receipt_reuse_refused, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: no process block fails closed",
                            test_quota_chain_no_process_block_fails_closed, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: job absorbs joiner usage",
                            test_quota_job_absorb_and_unabsorb, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: departure withdraws only absorbed",
                            test_quota_job_departure_withdraws_only_absorbed, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: pre-join release holds job until detach",
                            test_quota_prejoin_release_holds_job_until_detach, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: task init/teardown contract",
                            test_quota_task_init_teardown_contract, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: unabsorb refusal counted",
                            test_quota_unabsorb_refusal_counted, TEST_CAT_QUOTA);

    /* Section 7: CPU, I/O, and wakeup accounting + rate-limit records */
    test_suite_register_cat("Quota: cpu split advances monotonically",
                            test_quota_cpu_split_monotonic, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: io counters split read/write/control",
                            test_quota_io_split_by_op, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: control io counts zero-byte op",
                            test_quota_control_io_zero_byte_counts_op, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: wakeup counts only blocked grants",
                            test_quota_wakeup_only_from_blocked, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: sample is coherent and stamped",
                            test_quota_sample_stamped, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: membership delta excludes pre-join usage",
                            test_quota_membership_delta_excludes_prejoin, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: delta saturates on reset counter",
                            test_quota_delta_saturates_on_reset, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: rate record round-trip",
                            test_quota_rate_record_roundtrip, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: IOPS and bandwidth caps coexist",
                            test_quota_rate_ops_and_bytes_coexist, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: unflagged rate fields canonicalized",
                            test_quota_rate_unflagged_fields_canonicalized, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: delta covers every metric field",
                            test_quota_delta_covers_every_field, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: rate get retry preserves output",
                            test_quota_rate_get_retry_preserves_output, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: rate boundary + isolation matrix",
                            test_quota_rate_boundary_matrix, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: rate record rejects bad policy",
                            test_quota_rate_record_rejects_bad, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: rate generation advances per set",
                            test_quota_rate_generation_advances, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: unset rate class reports no policy",
                            test_quota_rate_unset_reports_no_policy, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: rate policy separate from usage",
                            test_quota_rate_separate_from_usage, TEST_CAT_QUOTA);
}

#endif /* KERNEL_TESTS */
