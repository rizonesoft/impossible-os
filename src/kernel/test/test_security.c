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
#include "libc/string.h"

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
    {
        const char *expected = "SeShutdownPrivilege";
        const char *a = name;
        const char *b = expected;
        while (*a && *b && *a == *b) { a++; b++; }
        TEST_ASSERT(*a == '\0' && *b == '\0',
                    "SeShutdownPrivilege LUID maps to \"SeShutdownPrivilege\"");
    }

    /* Verify all 24 privilege LUIDs have non-NULL name mappings.
     * Catches g_priv_names[] table drift from SE_*_PRIVILEGE macros. */
    {
        const LUID *all_privs[] = {
            &SeCreateTokenPrivilege, &SeAssignPrimaryTokenPrivilege,
            &SeLockMemoryPrivilege, &SeIncreaseQuotaPrivilege,
            &SeTcbPrivilege, &SeSecurityPrivilege,
            &SeTakeOwnershipPrivilege, &SeLoadDriverPrivilege,
            &SeSystemProfilePrivilege, &SeSystemtimePrivilege,
            &SeProfileSingleProcessPrivilege, &SeIncreaseBasePriorityPrivilege,
            &SeCreatePagefilePrivilege, &SeBackupPrivilege,
            &SeRestorePrivilege, &SeShutdownPrivilege,
            &SeDebugPrivilege, &SeAuditPrivilege,
            &SeChangeNotifyPrivilege, &SeUndockPrivilege,
            &SeManageVolumePrivilege, &SeImpersonatePrivilege,
            &SeCreateGlobalPrivilege, &SeCreateSymbolicLinkPrivilege
        };
        uint32_t i;
        uint32_t mapped = 0;
        for (i = 0; i < 24; i++) {
            if (RtlPrivilegeLuidToName(all_privs[i]) != (const char *)0)
                mapped++;
        }
        TEST_ASSERT_EQ(mapped, 24, "All 24 privilege LUIDs have name mappings");
    }

    /* Unknown LUID returns NULL */
    {
        LUID unknown = { 999, 0 };
        TEST_ASSERT(RtlPrivilegeLuidToName(&unknown) == (const char *)0,
                    "Unknown LUID returns NULL");
    }
}

/* ---- Struct size assertions (bulletproofing §12) ---- */

static void test_security_struct_sizes(void)
{
    TEST_ASSERT_EQ(sizeof(ACL), 8, "ACL is 8 bytes (Windows ABI)");
    TEST_ASSERT_EQ(sizeof(ACE_HEADER), 4, "ACE_HEADER is 4 bytes");
    TEST_ASSERT_EQ(sizeof(ACCESS_ALLOWED_ACE), 12, "ACCESS_ALLOWED_ACE is 12 bytes");
    TEST_ASSERT_EQ(sizeof(ACCESS_DENIED_ACE), 12, "ACCESS_DENIED_ACE is 12 bytes");
    TEST_ASSERT_EQ(sizeof(SID), 8, "SID base is 8 bytes");

    /* Privilege structs (§2) */
    TEST_ASSERT_EQ(sizeof(LUID_AND_ATTRIBUTES), 12, "LUID_AND_ATTRIBUTES is 12 bytes");
    TEST_ASSERT_EQ(sizeof(PRIVILEGE_SET), 8, "PRIVILEGE_SET base is 8 bytes");
    TEST_ASSERT_EQ(sizeof(TOKEN_PRIVILEGES), 4, "TOKEN_PRIVILEGES base is 4 bytes");
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

/* ---- Service SID generation ---- */

static void test_service_sid(void)
{
    uint8_t buf[68];  /* SID_MAX_SIZE */
    SID *sid = (SID *)buf;
    uint32_t sid_len = sizeof(buf);
    int rc;
    char str[128];

    rc = RtlCreateServiceSid("TestService", sid, &sid_len);
    TEST_ASSERT(rc == 0, "RtlCreateServiceSid succeeds");
    TEST_ASSERT_EQ(sid->Revision, 1, "Service SID revision is 1");
    TEST_ASSERT_EQ(sid->SubAuthorityCount, 6, "Service SID has 6 sub-authorities");
    TEST_ASSERT_EQ(sid->SubAuthority[0], 80, "Service SID RID base is 80");

    /* Deterministic: same name produces same SID */
    {
        uint8_t buf2[68];
        SID *sid2 = (SID *)buf2;
        uint32_t len2 = sizeof(buf2);
        RtlCreateServiceSid("TestService", sid2, &len2);
        TEST_ASSERT(RtlEqualSid(sid, sid2) != 0,
                    "Same service name produces identical SID");
    }

    /* Different name produces different SID */
    {
        uint8_t buf3[68];
        SID *sid3 = (SID *)buf3;
        uint32_t len3 = sizeof(buf3);
        RtlCreateServiceSid("OtherService", sid3, &len3);
        TEST_ASSERT(RtlEqualSid(sid, sid3) == 0,
                    "Different service name produces different SID");
    }

    /* String format starts with S-1-5-80- */
    rc = RtlConvertSidToString(str, sizeof(str), sid);
    TEST_ASSERT(rc > 0, "Service SID converts to string");
    TEST_ASSERT(str[0] == 'S' && str[2] == '1' && str[4] == '5',
                "Service SID string starts with S-1-5-");
}

/* ---- LUID allocator ---- */

#include "kernel/security/luid.h"

static void test_luid_allocator(void)
{
    LUID a = NtAllocateLocallyUniqueId();
    LUID b = NtAllocateLocallyUniqueId();

    /* Monotonically increasing */
    TEST_ASSERT(b.LowPart > a.LowPart,
                "LUID allocator is monotonically increasing");

    /* Not equal */
    TEST_ASSERT(RtlEqualLuid(&a, &b) == 0,
                "Consecutive LUIDs are unique");

    /* Not zero */
    TEST_ASSERT(RtlIsZeroLuid(&a) == 0,
                "Allocated LUID is not zero");
}

/* ---- RtlPrivilegeSetToString ---- */

static void test_privilege_set_to_string(void)
{
    /* Build a small PRIVILEGE_SET with 2 entries on the stack */
    uint8_t ps_buf[8 + 2 * 12];  /* base + 2 LUID_AND_ATTRIBUTES */
    PRIVILEGE_SET *ps = (PRIVILEGE_SET *)ps_buf;
    char out[256];
    int rc;

    ps->PrivilegeCount = 2;
    ps->Control = 0;
    ps->Privilege[0].Luid = SeShutdownPrivilege;
    ps->Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;
    ps->Privilege[1].Luid = SeDebugPrivilege;
    ps->Privilege[1].Attributes = 0;  /* disabled */

    rc = RtlPrivilegeSetToString(ps, out, sizeof(out));
    TEST_ASSERT(rc > 0, "RtlPrivilegeSetToString returns positive length");

    /* Should contain both privilege names */
    {
        int found_shutdown = 0, found_debug = 0;
        const char *p = out;
        while (*p) {
            if (*p == 'S' && p[1] == 'e' && p[2] == 'S' && p[3] == 'h')
                found_shutdown = 1;
            if (*p == 'S' && p[1] == 'e' && p[2] == 'D' && p[3] == 'e')
                found_debug = 1;
            p++;
        }
        TEST_ASSERT(found_shutdown, "Output contains SeShutdownPrivilege");
        TEST_ASSERT(found_debug, "Output contains SeDebugPrivilege");
    }

    /* NULL input returns -1 */
    TEST_ASSERT(RtlPrivilegeSetToString((const PRIVILEGE_SET *)0, out, sizeof(out)) == -1,
                "RtlPrivilegeSetToString rejects NULL");

    /* Tiny buffer: RtlPrivilegeSetToString must handle truncation safely.
     * Regression: WHPX freeze observed when calling with small len after
     * prior calls on the same PRIVILEGE_SET. Rebuild ps to rule out
     * stale pointer from compiler stack reuse. */
    {
        uint8_t ps2_buf[8 + 2 * 12];
        PRIVILEGE_SET *ps2 = (PRIVILEGE_SET *)ps2_buf;
        ps2->PrivilegeCount = 1;
        ps2->Control = 0;
        ps2->Privilege[0].Luid = SeShutdownPrivilege;
        ps2->Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

        rc = RtlPrivilegeSetToString(ps2, out, 8);
        TEST_ASSERT(rc >= 0, "RtlPrivilegeSetToString with len=8 does not crash");
    }
}

/* ---- RtlValidSid rejects malformed ---- */

static void test_sid_valid_reject(void)
{
    uint8_t bad_buf[68];
    SID *bad = (SID *)bad_buf;

    /* SubAuthorityCount > 15 should be rejected */
    bad->Revision = SID_REVISION;
    bad->SubAuthorityCount = 16;
    TEST_ASSERT(RtlValidSid(bad) == 0,
                "RtlValidSid rejects SubAuthorityCount > 15");

    /* Wrong revision */
    bad->Revision = 99;
    bad->SubAuthorityCount = 1;
    TEST_ASSERT(RtlValidSid(bad) == 0,
                "RtlValidSid rejects bad revision");

    /* RtlEqualSid should reject malformed SID */
    bad->Revision = SID_REVISION;
    bad->SubAuthorityCount = 200;
    TEST_ASSERT(RtlEqualSid(bad, SeLocalSystemSid) == 0,
                "RtlEqualSid rejects malformed SID");
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
    test_suite_register_cat("Security: service SID", test_service_sid, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: LUID allocator", test_luid_allocator, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: SID validation reject", test_sid_valid_reject, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: privilege set to string", test_privilege_set_to_string, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
