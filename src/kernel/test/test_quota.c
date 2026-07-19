/* ============================================================================
 * test_quota.c -- Kernel resource-accounting & quota unit tests
 *
 * Section 1 coverage: the resource type registry (taxonomy table, accessors,
 * unit/limit/name metadata, out-of-range safety, boot-time validation).
 *
 * XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/quota/quota.h"
#include "kernel/security/privileges.h"  /* SE_INCREASE_QUOTA_PRIVILEGE */

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
}

#endif /* KERNEL_TESTS */
