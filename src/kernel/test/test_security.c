/* ============================================================================
 * test_security.c -- Security subsystem unit tests
 *
 * Tests SID comparison/formatting, ACL creation, token creation with
 * privilege verification.
 *
 * XREF: 00-infrastructure/TODO-02 §3
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/security/sid.h"
#include "kernel/security/acl.h"
#include "kernel/security/token.h"
#include "kernel/security/privileges.h"
#include "kernel/ob/ob.h"

/* ---- SID comparison ---- */

static void test_sid_equal(void)
{
    TEST_ASSERT(RtlEqualSid(SeLocalSystemSid, SeLocalSystemSid) != 0,
                "RtlEqualSid(SeLocalSystemSid, SeLocalSystemSid) is true");
}

/* ---- SID formatting ---- */

static void test_sid_to_string(void)
{
    char buf[64];
    int len = RtlConvertSidToString(buf, sizeof(buf), SeLocalSystemSid);
    TEST_ASSERT(len > 0, "RtlConvertSidToString returns positive length");

    /* Check the string is "S-1-5-18" */
    int match = (buf[0] == 'S' && buf[1] == '-' && buf[2] == '1' &&
                 buf[3] == '-' && buf[4] == '5' && buf[5] == '-' &&
                 buf[6] == '1' && buf[7] == '8' && buf[8] == '\0');
    TEST_ASSERT(match, "SeLocalSystemSid formats as \"S-1-5-18\"");
}

/* ---- ACL creation round-trip ---- */

static void test_acl_roundtrip(void)
{
    /* Stack buffer large enough for header + 1 ACE */
    uint8_t acl_buf[256];
    ACL *acl = (ACL *)acl_buf;
    int rc;

    rc = RtlCreateAcl(acl, sizeof(acl_buf), ACL_REVISION);
    TEST_ASSERT(rc == 0, "RtlCreateAcl succeeds");

    rc = RtlAddAccessAllowedAce(acl, ACL_REVISION, 0x1F01FF, SeLocalSystemSid);
    TEST_ASSERT(rc == 0, "RtlAddAccessAllowedAce succeeds");

    ACE_HEADER *ace = (ACE_HEADER *)0;
    rc = RtlGetAce(acl, 0, &ace);
    TEST_ASSERT(rc == 0, "RtlGetAce(0) succeeds");
    TEST_ASSERT(ace != (ACE_HEADER *)0, "ACE pointer is non-NULL");
    TEST_ASSERT(ace->AceType == ACCESS_ALLOWED_ACE_TYPE,
                "ACE type is ACCESS_ALLOWED_ACE_TYPE");
}

/* ---- Token creation ---- */

static void test_system_token(void)
{
    ACCESS_TOKEN *tok = SeCreateSystemToken();
    TEST_ASSERT(tok != (ACCESS_TOKEN *)0, "SeCreateSystemToken returns non-NULL");

    TEST_ASSERT(tok->PrivilegeCount == 24,
                "system token has 24 privileges");

    TEST_ASSERT(RtlEqualSid(tok->IntegrityLevelSid, SeILSystem) != 0,
                "system token IL is System");

    ObDereferenceObject(tok);  /* cleanup */
}

/* ---- Privilege LUID to name lookup ---- */

static void test_privilege_name(void)
{
    const char *name = RtlPrivilegeLuidToName(&SeShutdownPrivilege);
    TEST_ASSERT(name != (const char *)0, "RtlPrivilegeLuidToName returns non-NULL");

    /* Check it's "SeShutdownPrivilege" */
    const char *expected = "SeShutdownPrivilege";
    const char *a = name;
    const char *b = expected;
    while (*a && *b && *a == *b) { a++; b++; }
    TEST_ASSERT(*a == '\0' && *b == '\0',
                "SeShutdownPrivilege LUID maps to \"SeShutdownPrivilege\"");
}

/* ---- Struct size assertions (bulletproofing §12) ---- */

static void test_security_struct_sizes(void)
{
    TEST_ASSERT_EQ(sizeof(ACL), 8, "ACL is 8 bytes (Windows ABI)");
    TEST_ASSERT_EQ(sizeof(ACE_HEADER), 4, "ACE_HEADER is 4 bytes");
    TEST_ASSERT_EQ(sizeof(ACCESS_ALLOWED_ACE), 12, "ACCESS_ALLOWED_ACE is 12 bytes");
    TEST_ASSERT_EQ(sizeof(ACCESS_DENIED_ACE), 12, "ACCESS_DENIED_ACE is 12 bytes");
    TEST_ASSERT_EQ(sizeof(SID), 8, "SID base is 8 bytes");
}

/* ---- SID binary format verification ---- */

static void test_sid_binary_format(void)
{
    /* SeLocalSystemSid = S-1-5-18
     * Binary: Revision=1, SubAuthorityCount=1, Authority={0,0,0,0,0,5},
     *         SubAuthority[0]=18 */
    TEST_ASSERT_EQ(SeLocalSystemSid->Revision, 1, "S-1-5-18 Revision == 1");
    TEST_ASSERT_EQ(SeLocalSystemSid->SubAuthorityCount, 1, "S-1-5-18 SubAuthorityCount == 1");
    TEST_ASSERT_EQ(SeLocalSystemSid->IdentifierAuthority[5], 5,
                   "S-1-5-18 Authority == 5 (NT Authority)");
    TEST_ASSERT_EQ(SeLocalSystemSid->SubAuthority[0], 18,
                   "S-1-5-18 SubAuthority[0] == 18");
    TEST_ASSERT_EQ(RtlLengthSid(SeLocalSystemSid), 12,
                   "S-1-5-18 total length == 12 bytes (8 + 4*1)");
}

/* ---- ACL with 3 ACEs: walk and verify offsets ---- */

static void test_acl_3ace_walk(void)
{
    uint8_t buf[512];
    ACL *acl = (ACL *)buf;
    ACE_HEADER *ace;
    int rc;

    rc = RtlCreateAcl(acl, sizeof(buf), ACL_REVISION);
    TEST_ASSERT(rc == 0, "RtlCreateAcl for 3-ACE test");

    /* Add 3 ACEs with different SIDs */
    rc = RtlAddAccessAllowedAce(acl, ACL_REVISION, 0x1F01FF, SeLocalSystemSid);
    TEST_ASSERT(rc == 0, "ACE 0 (SYSTEM) added");
    rc = RtlAddAccessAllowedAce(acl, ACL_REVISION, 0x1F01FF, SeBuiltinAdministratorsSid);
    TEST_ASSERT(rc == 0, "ACE 1 (Administrators) added");
    rc = RtlAddAccessDeniedAce(acl, ACL_REVISION, GENERIC_ALL, SeWorldSid);
    TEST_ASSERT(rc == 0, "ACE 2 (Everyone deny) added");

    TEST_ASSERT_EQ(acl->AceCount, 3, "ACL has 3 ACEs");

    /* Walk all 3 ACEs and verify types */
    rc = RtlGetAce(acl, 0, &ace);
    TEST_ASSERT(rc == 0 && ace->AceType == ACCESS_ALLOWED_ACE_TYPE,
                "ACE 0 is ACCESS_ALLOWED");
    rc = RtlGetAce(acl, 1, &ace);
    TEST_ASSERT(rc == 0 && ace->AceType == ACCESS_ALLOWED_ACE_TYPE,
                "ACE 1 is ACCESS_ALLOWED");
    rc = RtlGetAce(acl, 2, &ace);
    TEST_ASSERT(rc == 0 && ace->AceType == ACCESS_DENIED_ACE_TYPE,
                "ACE 2 is ACCESS_DENIED");

    /* Out-of-bounds access should fail */
    rc = RtlGetAce(acl, 3, &ace);
    TEST_ASSERT(rc != 0, "ACE 3 (OOB) returns error");
}

/* ---- Registration ---- */

void test_register_security(void)
{
    test_suite_register_cat("Security: SID equal", test_sid_equal, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: SID to string", test_sid_to_string, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: ACL roundtrip", test_acl_roundtrip, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: system token", test_system_token, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: privilege name", test_privilege_name, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: struct sizes", test_security_struct_sizes, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: SID binary format", test_sid_binary_format, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: ACL 3-ACE walk", test_acl_3ace_walk, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
