/* test_ci.c -- Code Integrity policy object.
 *
 * Exercises the pure ci_policy_build_defaults() builder (ordered enforcement
 * scalar, fail-closed Secure-Boot + safe-mode gating of the permissive flags,
 * flag/enforcement orthogonality) and the CONSERVATIVE pre-publication behavior
 * of the lockless accessors (before ci_init, s_ready is 0, so every accessor
 * fails closed). ci_init itself is never called from a test -- it reads live
 * boot state, which the test-side-effect policy forbids.
 */
#include "kernel/test/test.h"
#include "kernel/ci/ci.h"

/* Active Secure Boot pins the locked SECUREBOOT level AND forces every
 * permissive flag off, even when safe mode would otherwise allow a relax. */
static void test_ci_secureboot_pins_and_forces_off(void)
{
    ci_policy_t p;
    ci_policy_build_defaults(&p, CI_SB_ACTIVE, /*ts=*/1, /*noci=*/1, /*relax=*/true);
    TEST_ASSERT_EQ(p.enforcement, (uint32_t)CI_ENFORCE_SECUREBOOT,
        "active Secure Boot pins SECUREBOOT enforcement");
    TEST_ASSERT_EQ(p.test_signing, 0u,
        "test-signing forced off under active Secure Boot (no relax)");
    TEST_ASSERT_EQ(p.secure_boot, 1u, "secure_boot flag recorded");
    TEST_ASSERT_EQ(p.sealed, 0u, "builder does not seal -- ci_init seals");
}

/* Unreadable/unknown Secure Boot must fail CLOSED: relaxations are refused and
 * enforcement stays ENFORCE, exactly as for an active state. */
static void test_ci_sb_unknown_failclosed(void)
{
    ci_policy_t p;
    ci_policy_build_defaults(&p, CI_SB_UNKNOWN, /*ts=*/1, /*noci=*/1, /*relax=*/true);
    TEST_ASSERT_EQ(p.test_signing, 0u, "unknown SB refuses test-signing");
    TEST_ASSERT_EQ(p.enforcement, (uint32_t)CI_ENFORCE_ENFORCE,
        "unknown SB refuses nointegritychecks -> stays ENFORCE");
    TEST_ASSERT_EQ(p.secure_boot, 0u, "unknown SB not recorded as active");
}

/* Relaxations are honored ONLY when safe mode allows AND SB is known-off. */
static void test_ci_relax_gating(void)
{
    ci_policy_t denied;
    ci_policy_build_defaults(&denied, CI_SB_KNOWN_OFF, /*ts=*/1, /*noci=*/1, /*relax=*/false);
    TEST_ASSERT_EQ(denied.test_signing, 0u, "safe mode denies relax -> test-signing off");
    TEST_ASSERT_EQ(denied.enforcement, (uint32_t)CI_ENFORCE_ENFORCE,
        "safe mode denies relax -> stays ENFORCE");

    ci_policy_t allowed;
    ci_policy_build_defaults(&allowed, CI_SB_KNOWN_OFF, /*ts=*/1, /*noci=*/1, /*relax=*/true);
    TEST_ASSERT_EQ(allowed.test_signing, 1u, "known-off + relax allowed -> test-signing honored");
    TEST_ASSERT_EQ(allowed.enforcement, (uint32_t)CI_ENFORCE_AUDIT,
        "authorized nointegritychecks drops to AUDIT (observe, allow)");
}

/* The enforcement scalar is ORDERED (the ratchet depends on this). */
static void test_ci_enforcement_ordered(void)
{
    TEST_ASSERT(CI_ENFORCE_DISABLED < CI_ENFORCE_AUDIT, "DISABLED < AUDIT");
    TEST_ASSERT(CI_ENFORCE_AUDIT < CI_ENFORCE_ENFORCE, "AUDIT < ENFORCE");
    TEST_ASSERT(CI_ENFORCE_ENFORCE < CI_ENFORCE_SECUREBOOT, "ENFORCE < SECUREBOOT");
}

/* The permissive flags are ORTHOGONAL to the enforcement scalar -- flipping one
 * (when it is even allowed) must not move enforcement. */
static void test_ci_flags_orthogonal(void)
{
    ci_policy_t off, on;
    ci_policy_build_defaults(&off, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/0, /*relax=*/true);
    ci_policy_build_defaults(&on,  CI_SB_KNOWN_OFF, /*ts=*/1, /*noci=*/0, /*relax=*/true);
    TEST_ASSERT_NEQ(off.test_signing, on.test_signing, "test_signing flag flipped");
    TEST_ASSERT_EQ(off.enforcement, on.enforcement,
        "test_signing flip does not change the enforcement scalar");
    TEST_ASSERT_EQ(on.enforcement, (uint32_t)CI_ENFORCE_ENFORCE,
        "no nointegritychecks relaxation -> ENFORCE regardless of test-signing");
}

/* A fresh policy starts with empty collections and all decisions audited. */
static void test_ci_collections_empty(void)
{
    ci_policy_t p;
    ci_policy_build_defaults(&p, CI_SB_ACTIVE, /*ts=*/0, /*noci=*/0, /*relax=*/false);
    TEST_ASSERT_EQ(p.anchor_count, 0u, "no anchors until the policy-root key is compiled in");
    TEST_ASSERT_EQ(p.revoked_count, 0u, "no revoked hashes by default");
    TEST_ASSERT_EQ(p.audit_flags, (uint32_t)CI_AUDIT_ALL, "all decision classes audited");
    TEST_ASSERT_EQ(p.measurement, 0u, "default is a verdict mode, not measure-only");
}

/* Before ci_init publishes (s_ready == 0), every accessor fails CLOSED:
 * enforcement is ENFORCE (never DISABLED), sealed is false, flags are off, and
 * an unknown hash is not reported revoked. (ci_init is not wired into a test.) */
static void test_ci_preinit_failclosed(void)
{
    TEST_ASSERT_EQ(ci_policy_sealed(), false, "not sealed before ci_init");
    TEST_ASSERT_EQ(ci_get_enforcement(), (uint32_t)CI_ENFORCE_ENFORCE,
        "pre-init enforcement fails closed to ENFORCE, not DISABLED");
    TEST_ASSERT_EQ(ci_is_test_signing(), false, "pre-init test-signing off");
    TEST_ASSERT_NULL(ci_policy_get(), "pre-init ci_policy_get returns NULL");
    uint8_t zero[CI_HASH_LEN] = {0};
    TEST_ASSERT_EQ(ci_hash_revoked(zero), true,
        "pre-init treats every hash as revoked (fail closed, deny until sealed)");
}

void test_register_ci(void)
{
    test_suite_register_cat("CI: Secure Boot pins + forces flags off",
        test_ci_secureboot_pins_and_forces_off, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: unknown Secure Boot fails closed",
        test_ci_sb_unknown_failclosed, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: relax safe-mode gating",
        test_ci_relax_gating, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: enforcement scalar ordered",
        test_ci_enforcement_ordered, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: permissive flags orthogonal",
        test_ci_flags_orthogonal, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: collections empty by default",
        test_ci_collections_empty, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: pre-init accessors fail closed",
        test_ci_preinit_failclosed, TEST_CAT_SECURITY);
}
