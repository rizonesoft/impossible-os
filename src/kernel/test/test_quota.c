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
    quota_block_t *b = quota_block_create(NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "quota_block_create(NULL, 0) returns a block");
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
    quota_block_t *b = quota_block_create(NULL, 0);
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
    quota_block_t *b = quota_block_create(NULL, 0);
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
    quota_block_t *b = quota_block_create(NULL, 0);
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
    quota_block_t *b = quota_block_create(NULL, 0);
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

/* Returning more than was charged clamps at zero instead of wrapping negative. */
static void test_quota_return_underflow_clamps(void)
{
    quota_block_t *b = quota_block_create(NULL, 0);
    TEST_ASSERT_NOT_NULL((void *)b, "block allocated");
    if (!b)
        return;

    TEST_ASSERT_EQ((uint64_t)quota_charge(b, QUOTA_RES_THREAD, 3),
                   (uint64_t)STATUS_SUCCESS, "charge 3 threads");
    TEST_ASSERT_EQ((uint64_t)quota_return(b, QUOTA_RES_THREAD, 10),
                   (uint64_t)STATUS_SUCCESS, "over-return reports success");
    TEST_ASSERT_EQ(quota_usage(b, QUOTA_RES_THREAD), 0ULL,
                   "over-return clamped to 0, never negative");
    quota_block_deref(b);
}

/* Lowering a limit below current usage keeps committed charges but refuses the
 * next charge (the documented limit-lowering contract). */
static void test_quota_lowering_limit_keeps_committed(void)
{
    quota_block_t *b = quota_block_create(NULL, 0);
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
    quota_block_t *src = quota_block_create(NULL, 0);
    quota_block_t *dst = quota_block_create(NULL, 0);
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
    quota_block_t *b = quota_block_create(NULL, 0);
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
    quota_block_t *b = quota_block_create(NULL, 0);
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
    quota_block_t *b = quota_block_create(NULL, 0);
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

    quota_block_t *b = quota_block_create(owner, sizeof(buf));
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

/* An owner SID whose declared readable length cannot hold it is refused rather
 * than read past the caller's buffer. */
static void test_quota_block_owner_sid_truncated(void)
{
    uint8_t buf[SID_MAX_SIZE] = { 0 };
    SID *owner = (SID *)buf;
    const uint8_t nt_authority[6] = { 0, 0, 0, 0, 0, 5 };

    RtlInitializeSid(owner, nt_authority, 4);   /* needs 8 + 4*4 = 24 bytes */

    quota_block_t *b = quota_block_create(owner, 16);
    TEST_ASSERT_NULL((void *)b, "SID longer than the declared readable length is refused");

    b = quota_block_create(owner, 8);
    TEST_ASSERT_NULL((void *)b, "header-only readable length refuses a 4-subauthority SID");

    b = quota_block_create(owner, 24);
    TEST_ASSERT_NOT_NULL((void *)b, "exact readable length is accepted");
    quota_block_deref(b);
}

/* Transfer debits the source atomically, so a second transfer cannot spend the
 * same usage again. This is the deterministic stand-in for the two-CPU race:
 * with a pre-check-then-return design both transfers would succeed and the
 * destinations would jointly hold more than the source ever had. */
static void test_quota_transfer_cannot_duplicate(void)
{
    quota_block_t *src  = quota_block_create(NULL, 0);
    quota_block_t *dst1 = quota_block_create(NULL, 0);
    quota_block_t *dst2 = quota_block_create(NULL, 0);
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

/* When the destination refuses, the source debit is compensated exactly -- the
 * failed transfer must not leave usage stranded in neither block. */
static void test_quota_transfer_refusal_compensates(void)
{
    quota_block_t *src = quota_block_create(NULL, 0);
    quota_block_t *dst = quota_block_create(NULL, 0);
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

/* A charge refused by the post-commit limit re-check must leave usage exactly
 * as it was and count exactly one failure -- the withdrawal is its own charge,
 * never someone else's. */
static void test_quota_charge_refusal_is_exact(void)
{
    quota_block_t *b = quota_block_create(NULL, 0);
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
 * and is owned by the section 10 test infrastructure work. Do not upgrade
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
 * eventually picked. A bounded join needs scheduler support that does not
 * exist yet (owned by the section 10 test infrastructure work). */
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
    s_race_block = quota_block_create(NULL, 0);
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
    s_race_block = quota_block_create(NULL, 0);
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
    s_race_block = quota_block_create(NULL, 0);
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
    quota_block_t *src = quota_block_create(NULL, 0);
    quota_block_t *dst = quota_block_create(NULL, 0);
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
    quota_block_t *b = quota_block_create(NULL, 0);
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
    quota_block_t *b = quota_block_create(NULL, 0);
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
    quota_block_t *b = quota_block_create(NULL, 0);
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
    quota_block_t *b = quota_block_create(NULL, 0);
    TEST_ASSERT_NULL((void *)b, "allocation failure returns NULL, not a partial block");

    uint8_t buf[SID_MAX_SIZE] = { 0 };
    SID *sid = (SID *)buf;
    const uint8_t nt_authority[6] = { 0, 0, 0, 0, 0, 5 };

    /* Bad revision. */
    RtlInitializeSid(sid, nt_authority, 1);
    sid->Revision = 7;
    TEST_ASSERT_NULL((void *)quota_block_create(sid, sizeof(buf)),
                     "malformed revision is refused");

    /* Sub-authority count beyond the architectural maximum. */
    RtlInitializeSid(sid, nt_authority, 1);
    sid->SubAuthorityCount = SID_MAX_SUB_AUTHORITIES + 1;
    TEST_ASSERT_NULL((void *)quota_block_create(sid, sizeof(buf)),
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

    b = quota_block_create(sid, SID_MAX_SIZE);
    TEST_ASSERT_NOT_NULL((void *)b, "a maximum-length SID is accepted");
    if (b) {
        const SID *got = quota_block_owner(b);
        TEST_ASSERT_NOT_NULL((void *)got, "owner reported back");
        if (got)
            TEST_ASSERT(RtlEqualSid(got, sid) != 0, "maximum-length SID copied intact");
        quota_block_deref(b);
    }
}

/* Remaining public boundary matrix: the argument contracts each entry point
 * owns separately from the shared bad-argument sweep. */
static void test_quota_boundary_matrix(void)
{
    quota_block_t *b   = quota_block_create(NULL, 0);
    quota_block_t *dst = quota_block_create(NULL, 0);
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
    test_suite_register_cat("Quota: return underflow clamps",
                            test_quota_return_underflow_clamps, TEST_CAT_QUOTA);
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
}

#endif /* KERNEL_TESTS */
