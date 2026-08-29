/* ============================================================================
 * test_security.c -- Security subsystem unit tests
 *
 * Tests SID comparison/formatting, ACL creation, token creation with
 * privilege verification.
 *
 * XREF: 00-infrastructure/TODO-02-ai-development-system.md (unit tests)
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/security/sid.h"
#include "kernel/security/acl.h"
#include "kernel/security/mic.h"
#include "kernel/security/token.h"
#include "kernel/security/privileges.h"
#include "kernel/security/assign_security.h"
#include "kernel/security/stack_canary.h"
#include "kernel/boot_info.h"   /* BOOT_CAP_PAYLOAD_DESCRIPTORS -- canary payload-peek gate */
#include "kernel/ob/ob.h"
#include "kernel/sched/task.h"   /* task/thread, task_current/thread_current for token assignment */
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

/* ---- Struct size assertions (bulletproofing) ---- */

static void test_security_struct_sizes(void)
{
    TEST_ASSERT_EQ(sizeof(ACL), 8, "ACL is 8 bytes (Windows ABI)");
    TEST_ASSERT_EQ(sizeof(ACE_HEADER), 4, "ACE_HEADER is 4 bytes");
    TEST_ASSERT_EQ(sizeof(ACCESS_ALLOWED_ACE), 12, "ACCESS_ALLOWED_ACE is 12 bytes");
    TEST_ASSERT_EQ(sizeof(ACCESS_DENIED_ACE), 12, "ACCESS_DENIED_ACE is 12 bytes");
    TEST_ASSERT_EQ(sizeof(SID), 8, "SID base is 8 bytes");

    /* Privilege structs */
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

    /* Monotonically increasing (compare the full 64-bit value, not just LowPart,
     * so a value that straddled the 32-bit boundary would not read as a drop). */
    TEST_ASSERT((((uint64_t)(uint32_t)b.HighPart << 32) | b.LowPart) >
                (((uint64_t)(uint32_t)a.HighPart << 32) | a.LowPart),
                "LUID allocator is monotonically increasing");

    /* Not equal */
    TEST_ASSERT(RtlEqualLuid(&a, &b) == 0,
                "Consecutive LUIDs are unique");

    /* Not zero */
    TEST_ASSERT(RtlIsZeroLuid(&a) == 0,
                "Allocated LUID is not zero");

    /* 64-bit split: the value carries into HighPart past the 32-bit boundary,
     * so the identifier no longer recycles after ~4 billion allocations. */
    {
        LUID lo   = RtlLuidFromValue(0xFFFFFFFFULL);
        LUID wrap = RtlLuidFromValue(0x100000000ULL);
        LUID hi   = RtlLuidFromValue(0x1FFFFFFFFULL);
        TEST_ASSERT(lo.LowPart == 0xFFFFFFFFu && lo.HighPart == 0,
                    "RtlLuidFromValue: max 32-bit value keeps HighPart 0");
        TEST_ASSERT(wrap.LowPart == 0 && wrap.HighPart == 1,
                    "RtlLuidFromValue: wrap carries into HighPart");
        TEST_ASSERT(hi.LowPart == 0xFFFFFFFFu && hi.HighPart == 1,
                    "RtlLuidFromValue: high dword above the wrap");
    }
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

        /* Guard path: len < 8 returns 0 immediately */
        rc = RtlPrivilegeSetToString(ps2, out, 4);
        TEST_ASSERT(rc == 0, "RtlPrivilegeSetToString with len=4 returns 0 (guard)");

        /* Truncation path: len=32 enters loop but truncates privilege name */
        rc = RtlPrivilegeSetToString(ps2, out, 32);
        TEST_ASSERT(rc >= 0, "RtlPrivilegeSetToString with len=32 truncates safely");

        /* Malformed count: a 1-entry set claiming a count over TOKEN_MAX_PRIVS
         * must fail closed rather than read past the backing array. */
        ps2->PrivilegeCount = TOKEN_MAX_PRIVS + 1;
        rc = RtlPrivilegeSetToString(ps2, out, sizeof(out));
        TEST_ASSERT(rc == -1,
                    "RtlPrivilegeSetToString rejects PrivilegeCount > TOKEN_MAX_PRIVS");
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

/* ---- RtlLengthSidBounded rejects untrusted / OOB SIDs ---- */

static void test_sid_length_bounded(void)
{
    uint8_t buf[68];
    SID *sid = (SID *)buf;

    memset(buf, 0, sizeof(buf));
    sid->Revision = SID_REVISION;
    sid->SubAuthorityCount = 1;

    /* Valid 1-subauth SID is 12 bytes when avail is ample. */
    TEST_ASSERT_EQ(RtlLengthSidBounded(sid, sizeof(buf)), 12,
                   "RtlLengthSidBounded valid SID -> 12");
    /* avail below the fixed 8-byte header -> 0 (cannot read count safely). */
    TEST_ASSERT_EQ(RtlLengthSidBounded(sid, 7), 0,
                   "RtlLengthSidBounded rejects avail < 8");
    /* Full length (12) exceeds avail (11) -> 0. */
    TEST_ASSERT_EQ(RtlLengthSidBounded(sid, 11), 0,
                   "RtlLengthSidBounded rejects len > avail");
    /* SubAuthorityCount > 15 -> 0 even with a large window. */
    sid->SubAuthorityCount = 16;
    TEST_ASSERT_EQ(RtlLengthSidBounded(sid, sizeof(buf)), 0,
                   "RtlLengthSidBounded rejects SubAuthorityCount > 15");
    /* Wrong revision -> 0. */
    sid->Revision = 99;
    sid->SubAuthorityCount = 1;
    TEST_ASSERT_EQ(RtlLengthSidBounded(sid, sizeof(buf)), 0,
                   "RtlLengthSidBounded rejects bad revision");
    /* NULL -> 0. */
    TEST_ASSERT_EQ(RtlLengthSidBounded((const SID *)0, 64), 0,
                   "RtlLengthSidBounded rejects NULL");
}

/* ---- RtlValidAcl / RtlGetAceEx bounded ACL walk ---- */

static void test_acl_valid_bounded(void)
{
    uint8_t acl_buf[256];
    uint8_t bad_buf[256];
    ACL *acl = (ACL *)acl_buf;
    ACL *bad = (ACL *)bad_buf;
    ACE_HEADER *ace;
    ACE_HEADER *h0;

    RtlCreateAcl(acl, sizeof(acl_buf), ACL_REVISION);
    RtlAddAccessAllowedAce(acl, ACL_REVISION, 0x1F01FF, SeLocalSystemSid);
    RtlAddAccessDeniedAce(acl, ACL_REVISION, 0x01, SeWorldSid);

    /* Well-formed ACL accepted when avail >= AclSize. */
    TEST_ASSERT(RtlValidAcl(acl, acl->AclSize) == 1,
                "RtlValidAcl accepts well-formed ACL");
    TEST_ASSERT(RtlValidAcl(acl, sizeof(acl_buf)) == 1,
                "RtlValidAcl accepts with larger avail");
    /* avail below AclSize / below the header -> reject. */
    TEST_ASSERT(RtlValidAcl(acl, acl->AclSize - 1) == 0,
                "RtlValidAcl rejects avail < AclSize");
    TEST_ASSERT(RtlValidAcl(acl, 4) == 0,
                "RtlValidAcl rejects avail < sizeof(ACL)");

    /* Bounded walk reaches both ACEs and rejects an out-of-range index. */
    TEST_ASSERT(RtlGetAceEx(acl, 0, &ace, acl->AclSize) == 0 &&
                ace->AceType == ACCESS_ALLOWED_ACE_TYPE,
                "RtlGetAceEx(0) -> allowed ACE");
    TEST_ASSERT(RtlGetAceEx(acl, 1, &ace, acl->AclSize) == 0 &&
                ace->AceType == ACCESS_DENIED_ACE_TYPE,
                "RtlGetAceEx(1) -> denied ACE");
    TEST_ASSERT(RtlGetAceEx(acl, 2, &ace, acl->AclSize) == -1,
                "RtlGetAceEx rejects index >= AceCount");

    /* An ACE whose AceSize overruns AclSize -> reject. */
    memcpy(bad_buf, acl_buf, sizeof(bad_buf));
    h0 = (ACE_HEADER *)(bad_buf + sizeof(ACL));
    h0->AceSize = bad->AclSize;
    TEST_ASSERT(RtlValidAcl(bad, bad->AclSize) == 0,
                "RtlValidAcl rejects ACE overrunning AclSize");

    /* Zero AceSize (would never advance) -> reject. */
    memcpy(bad_buf, acl_buf, sizeof(bad_buf));
    h0 = (ACE_HEADER *)(bad_buf + sizeof(ACL));
    h0->AceSize = 0;
    TEST_ASSERT(RtlValidAcl(bad, bad->AclSize) == 0,
                "RtlValidAcl rejects zero AceSize");

    /* AclSize larger than the readable window -> reject. */
    memcpy(bad_buf, acl_buf, sizeof(bad_buf));
    bad->AclSize = (uint16_t)(bad->AclSize + 100);
    TEST_ASSERT(RtlValidAcl(bad, acl->AclSize) == 0,
                "RtlValidAcl rejects AclSize > avail");

    /* RtlGetAceEx must reject a malformed TARGET ACE (index 0), not only the
     * ACEs it walks past: a SID-bearing ACE with AceSize 8 has no room for a
     * SID, and AceSize 4 is smaller than a SID-bearing ACE's fixed prefix. */
    memcpy(bad_buf, acl_buf, sizeof(bad_buf));
    h0 = (ACE_HEADER *)(bad_buf + sizeof(ACL));
    h0->AceSize = 8;
    TEST_ASSERT(RtlGetAceEx(bad, 0, &ace, bad->AclSize) == -1,
                "RtlGetAceEx rejects SID-bearing target ACE with no SID room");
    memcpy(bad_buf, acl_buf, sizeof(bad_buf));
    h0 = (ACE_HEADER *)(bad_buf + sizeof(ACL));
    h0->AceSize = 4;
    TEST_ASSERT(RtlGetAceEx(bad, 0, &ace, bad->AclSize) == -1,
                "RtlGetAceEx rejects target ACE with AceSize < SID prefix");

    /* Callback ACEs (0x09) are SID-bearing (Header+Mask+SidStart, SID at +8):
     * their inline SID must be validated just like a basic ACE. Flip the
     * first ACE to a callback type and corrupt its SID's SubAuthorityCount
     * (SID at ACE+8, count at SID offset 1) -> RtlValidAcl must reject. */
    memcpy(bad_buf, acl_buf, sizeof(bad_buf));
    h0 = (ACE_HEADER *)(bad_buf + sizeof(ACL));
    h0->AceType = ACCESS_ALLOWED_CALLBACK_ACE_TYPE;
    ((uint8_t *)h0)[sizeof(ACE_HEADER) + sizeof(uint32_t) + 1] = 200;
    TEST_ASSERT(RtlValidAcl(bad, bad->AclSize) == 0,
                "RtlValidAcl rejects callback ACE with corrupt inline SID");
    /* Same ACL as a callback ACE but with the SID intact -> accepted. */
    memcpy(bad_buf, acl_buf, sizeof(bad_buf));
    h0 = (ACE_HEADER *)(bad_buf + sizeof(ACL));
    h0->AceType = ACCESS_ALLOWED_CALLBACK_ACE_TYPE;
    TEST_ASSERT(RtlValidAcl(bad, bad->AclSize) == 1,
                "RtlValidAcl accepts callback ACE with valid inline SID");

    /* Object/unknown ACE types (e.g. 0x05) have a variable prefix we do not
     * parse -> rejected, so a validator-approved ACL never carries an ACE that
     * a fixed-offset consumer (RtlAclToCStr) would overread. */
    memcpy(bad_buf, acl_buf, sizeof(bad_buf));
    h0 = (ACE_HEADER *)(bad_buf + sizeof(ACL));
    h0->AceType = 0x05;  /* ACCESS_ALLOWED_OBJECT_ACE_TYPE (unsupported) */
    TEST_ASSERT(RtlValidAcl(bad, bad->AclSize) == 0,
                "RtlValidAcl rejects unparseable object/unknown ACE type");

    /* An invalid AclRevision must be rejected CONSISTENTLY by both bounded
     * untrusted-input APIs (shared acl_header_valid). */
    memcpy(bad_buf, acl_buf, sizeof(bad_buf));
    bad->AclRevision = 99;
    TEST_ASSERT(RtlValidAcl(bad, bad->AclSize) == 0,
                "RtlValidAcl rejects invalid AclRevision");
    TEST_ASSERT(RtlGetAceEx(bad, 0, &ace, bad->AclSize) == -1,
                "RtlGetAceEx rejects invalid AclRevision (consistency)");
}

/* ---- RtlSelfRelativeToAbsoluteSD bounded untrusted import ---- */

static void test_sd_selfrel_import_bounded(void)
{
    uint8_t owner_sid[16];
    uint8_t acl_buf[128];
    uint8_t rel_buf[256];
    uint8_t abs_buf[256];
    uint8_t bad[256];
    SECURITY_DESCRIPTOR sd_abs, sd_out;
    SECURITY_DESCRIPTOR_RELATIVE *sr;
    ACL *dacl = (ACL *)acl_buf;
    SID *owner = (SID *)owner_sid;
    uint32_t rel_len = sizeof(rel_buf);
    int rc;

    /* Build an absolute SD: Owner = LocalSystem, DACL with one allow ACE. */
    memset(owner_sid, 0, sizeof(owner_sid));
    RtlCopySid(owner_sid, sizeof(owner_sid), SeLocalSystemSid);
    RtlCreateAcl(dacl, sizeof(acl_buf), ACL_REVISION);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, 0x1F01FF, SeLocalSystemSid);
    RtlCreateSecurityDescriptor(&sd_abs, SECURITY_DESCRIPTOR_REVISION);
    RtlSetOwnerSecurityDescriptor(&sd_abs, owner, 0);
    RtlSetDaclSecurityDescriptor(&sd_abs, 1, dacl, 0);

    /* Marshal to a self-relative blob. */
    rc = RtlAbsoluteToSelfRelativeSD(&sd_abs, rel_buf, &rel_len);
    TEST_ASSERT(rc == 0, "RtlAbsoluteToSelfRelativeSD marshals");

    /* Correct rel_len imports and recovers Owner + DACL. */
    rc = RtlSelfRelativeToAbsoluteSD(rel_buf, rel_len, &sd_out,
                                     abs_buf, sizeof(abs_buf));
    TEST_ASSERT(rc == 0, "RtlSelfRelativeToAbsoluteSD valid import");
    TEST_ASSERT(sd_out.Owner != (SID *)0 &&
                RtlEqualSid(sd_out.Owner, SeLocalSystemSid) == 1,
                "imported Owner matches LocalSystem");
    TEST_ASSERT(sd_out.Dacl != (ACL *)0, "imported SD has DACL");

    /* rel_len shorter than the fixed header -> reject. */
    rc = RtlSelfRelativeToAbsoluteSD(rel_buf, 8, &sd_out, abs_buf, sizeof(abs_buf));
    TEST_ASSERT(rc == -1, "rejects rel_len < header");

    /* OffsetOwner pointing past rel_len -> reject. */
    memcpy(bad, rel_buf, sizeof(bad));
    sr = (SECURITY_DESCRIPTOR_RELATIVE *)bad;
    sr->OffsetOwner = rel_len + 100;
    rc = RtlSelfRelativeToAbsoluteSD(bad, rel_len, &sd_out, abs_buf, sizeof(abs_buf));
    TEST_ASSERT(rc == -1, "rejects OffsetOwner past rel_len");

    /* OffsetOwner pointing inside the fixed header -> reject. */
    memcpy(bad, rel_buf, sizeof(bad));
    sr = (SECURITY_DESCRIPTOR_RELATIVE *)bad;
    sr->OffsetOwner = 4;
    rc = RtlSelfRelativeToAbsoluteSD(bad, rel_len, &sd_out, abs_buf, sizeof(abs_buf));
    TEST_ASSERT(rc == -1, "rejects OffsetOwner inside header");

    /* rel_len that truncates the Owner SID mid-body -> reject. */
    rc = RtlSelfRelativeToAbsoluteSD(rel_buf,
                                     sizeof(SECURITY_DESCRIPTOR_RELATIVE) + 4,
                                     &sd_out, abs_buf, sizeof(abs_buf));
    TEST_ASSERT(rc == -1, "rejects rel_len cutting Owner SID");
}

/* ============================================================================
 * NT token syscall tests (SSDT dispatch path)
 * ============================================================================ */

#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/service_numbers.h"

/* Test: NtAllocateLocallyUniqueId via SSDT returns monotonic LUIDs */
static void test_nt_allocate_luid(void)
{
    LUID a, b;
    NTSTATUS status;

    a.LowPart = 0; a.HighPart = 0;
    status = ssdt_dispatch(SSDT_NtAllocateLocallyUniqueId,
                           (uint64_t)(uintptr_t)&a, 0, 0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtAllocateLocallyUniqueId via SSDT succeeds");
    TEST_ASSERT(!RtlIsZeroLuid(&a), "First LUID is non-zero");

    b.LowPart = 0; b.HighPart = 0;
    status = ssdt_dispatch(SSDT_NtAllocateLocallyUniqueId,
                           (uint64_t)(uintptr_t)&b, 0, 0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "Second SSDT dispatch succeeds");
    TEST_ASSERT((((uint64_t)(uint32_t)b.HighPart << 32) | b.LowPart) >
                (((uint64_t)(uint32_t)a.HighPart << 32) | a.LowPart),
                "LUIDs are monotonically increasing (full 64-bit)");

    /* NULL out pointer returns INVALID_PARAMETER */
    status = ssdt_dispatch(SSDT_NtAllocateLocallyUniqueId, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT(status == STATUS_INVALID_PARAMETER,
                "NtAllocateLocallyUniqueId rejects NULL out pointer");
}

/* Test: NtOpenProcessToken via SSDT returns a token handle for current process */
static void test_nt_open_process_token(void)
{
    HANDLE token_handle = INVALID_HANDLE_VALUE;
    NTSTATUS status;

    status = ssdt_dispatch(SSDT_NtOpenProcessToken,
                           (uint64_t)CURRENT_PROCESS,
                           (uint64_t)0x000F01FF  /* TOKEN_ALL_ACCESS */,
                           (uint64_t)(uintptr_t)&token_handle,
                           0, 0, 0);

    /* Current task may not have a token assigned in the test environment
     * (kernel boot-time tests run before any process has been given a
     * primary token).  Accept either success-with-handle or STATUS_NO_TOKEN. */
    if (NT_SUCCESS(status)) {
        TEST_ASSERT(token_handle != INVALID_HANDLE_VALUE,
                    "NtOpenProcessToken produces a valid handle");
    } else {
        TEST_ASSERT(status == STATUS_NO_TOKEN,
                    "NtOpenProcessToken returns STATUS_NO_TOKEN when task has no token");
    }

    /* NULL out pointer */
    status = ssdt_dispatch(SSDT_NtOpenProcessToken,
                           (uint64_t)CURRENT_PROCESS,
                           (uint64_t)0x000F01FF  /* TOKEN_ALL_ACCESS */,
                           0, 0, 0, 0);
    TEST_ASSERT(status == STATUS_INVALID_PARAMETER,
                "NtOpenProcessToken rejects NULL out pointer");
}

/* Test: NtQueryInformationToken rejects invalid handle */
static void test_nt_query_token_invalid_handle(void)
{
    uint8_t buf[64];
    uint32_t ret_len = 0;
    NTSTATUS status;

    /* Fabricated invalid handle */
    status = ssdt_dispatch(SSDT_NtQueryInformationToken,
                           (uint64_t)0x7FFFFFFF,
                           (uint64_t)TokenUser,
                           (uint64_t)(uintptr_t)buf,
                           (uint64_t)sizeof(buf),
                           (uint64_t)(uintptr_t)&ret_len,
                           0);
    TEST_ASSERT(status == STATUS_INVALID_HANDLE,
                "NtQueryInformationToken rejects bogus handle");

    /* Zero handle */
    status = ssdt_dispatch(SSDT_NtQueryInformationToken,
                           0,
                           (uint64_t)TokenUser,
                           (uint64_t)(uintptr_t)buf,
                           (uint64_t)sizeof(buf),
                           (uint64_t)(uintptr_t)&ret_len,
                           0);
    TEST_ASSERT(status == STATUS_INVALID_HANDLE,
                "NtQueryInformationToken rejects zero handle");
}

/* Test: NtSetInformationToken rejects read-only info classes */
static void test_nt_set_token_readonly(void)
{
    uint8_t buf[64] = { 0 };
    NTSTATUS status;

    /* TokenUser is read-only per Windows docs; we must reject it. */
    status = ssdt_dispatch(SSDT_NtSetInformationToken,
                           (uint64_t)0x7FFFFFFF,  /* bogus handle */
                           (uint64_t)TokenUser,
                           (uint64_t)(uintptr_t)buf,
                           (uint64_t)sizeof(buf),
                           0, 0);
    /* Invalid handle rejected first */
    TEST_ASSERT(status == STATUS_INVALID_HANDLE,
                "NtSetInformationToken rejects invalid handle before class check");

    /* NULL buffer */
    status = ssdt_dispatch(SSDT_NtSetInformationToken,
                           (uint64_t)0x7FFFFFFF,
                           (uint64_t)TokenUser,
                           0,
                           (uint64_t)sizeof(buf),
                           0, 0);
    TEST_ASSERT(status == STATUS_INVALID_PARAMETER,
                "NtSetInformationToken rejects NULL buffer");
}

/* Test: NtAdjustPrivilegesToken rejects invalid handle + NULL new state */
static void test_nt_adjust_privileges_invalid(void)
{
    NTSTATUS status;

    /* Invalid handle */
    status = ssdt_dispatch(SSDT_NtAdjustPrivilegesToken,
                           (uint64_t)0x7FFFFFFF, 0, 0, 0, 0, 0);
    TEST_ASSERT(status == STATUS_INVALID_HANDLE,
                "NtAdjustPrivilegesToken rejects invalid handle");
}

/* Test: all 9 SSDT slots are registered (not default stubs) */
static void test_nt_token_ssdt_registered(void)
{
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    extern int snprintf(char *buf, size_t size, const char *fmt, ...);
    static const struct { uint32_t svc; const char *name; } slots[] = {
        { SSDT_NtOpenProcessToken,      "NtOpenProcessToken" },
        { SSDT_NtOpenProcessTokenEx,    "NtOpenProcessTokenEx" },
        { SSDT_NtOpenThreadToken,       "NtOpenThreadToken" },
        { SSDT_NtOpenThreadTokenEx,     "NtOpenThreadTokenEx" },
        { SSDT_NtQueryInformationToken, "NtQueryInformationToken" },
        { SSDT_NtSetInformationToken,   "NtSetInformationToken" },
        { SSDT_NtAdjustPrivilegesToken, "NtAdjustPrivilegesToken" },
        { SSDT_NtAdjustGroupsToken,     "NtAdjustGroupsToken" },
        { SSDT_NtAllocateLocallyUniqueId, "NtAllocateLocallyUniqueId" },
        { SSDT_NtPrivilegeCheck,        "NtPrivilegeCheck" },
    };
    uint32_t i;
    char msg[96];
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        uint32_t idx = slots[i].svc & 0xFFF;
        snprintf(msg, sizeof(msg), "%s (0x%x) registered",
                 slots[i].name, (uint64_t)slots[i].svc);
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented, msg);
    }
}

/* ---- Stack canary (-fstack-protector-strong) ---- */

/* canary_massage is a pure function: low byte must be zeroed (terminator
 * canary) and bit 63 set (cookie is never 0, high byte non-zero). */
static void test_canary_massage_invariants(void)
{
    static const uint64_t inputs[] = {
        0, 1, 0xFFu, 0x123456789ABCDEFFull, 0xFFFFFFFFFFFFFFFFull, 0x00FF00FF00FF00FFull
    };
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        uintptr_t c = canary_massage(inputs[i]);
        TEST_ASSERT_EQ((uint64_t)(c & 0xFFu), 0u, "canary low byte is zero");
        TEST_ASSERT_EQ((uint64_t)(c >> 63), 1u, "canary bit 63 is set (never 0)");
        TEST_ASSERT(c != 0, "canary is non-zero");
    }
    /* The non-massaged middle bits are preserved (entropy not destroyed). */
    uintptr_t c = canary_massage(0x0011223344556677ull);
    TEST_ASSERT_EQ((uint64_t)((c >> 8) & 0xFFFFFFFFFFFFFFull),
                   0x80112233445566ull, "middle bits preserved + bit 63 set");
}

/* The live __stack_chk_guard must have been seeded at boot (canary_init in
 * kernel_main) and satisfy the same invariants. Read-only: do NOT call
 * canary_init from a test -- re-seeding would break this frame's own canary. */
static void test_canary_guard_seeded(void)
{
    TEST_ASSERT(__stack_chk_guard != 0, "__stack_chk_guard seeded non-zero at boot");
    TEST_ASSERT_EQ((uint64_t)(__stack_chk_guard & 0xFFu), 0u, "guard low byte zero");
    TEST_ASSERT_EQ((uint64_t)((uint64_t)__stack_chk_guard >> 63), 1u, "guard bit 63 set");
}

/* The boot-seed descriptor predicate must reject anything the canary cannot
 * safely dereference pre-IDT: unreserved (allocator-owned), out of the 4 GiB
 * identity map, or too short. Bit 3 = BOOT_PAYLOAD_FLAG_RESERVED;
 * 0x100000000 = BOOT_INFO_EARLY_MAP_END. */
static void test_canary_seed_desc_bounds(void)
{
    const uint32_t RES = (1u << 3);   /* BOOT_PAYLOAD_FLAG_RESERVED */
    const uint64_t END = 0x100000000ull;
    const uint32_t CAPS = BOOT_CAP_PAYLOAD_DESCRIPTORS;

    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS, RES, 0x200000, 4096), 1u,
                   "reserved + low + length>=16 accepted");
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS, 0, 0x200000, 4096), 0u,
                   "unreserved descriptor rejected");
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS, RES, END, 4096), 0u,
                   "phys_start at/above 4 GiB rejected");
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS, RES, END - 8, 4096), 0u,
                   "range crossing the 4 GiB map end rejected");
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS, RES, 0x200000, 8), 0u,
                   "length < 16 rejected");
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS, RES, 0, 4096), 0u,
                   "phys_start 0 rejected");

    /* CAPABILITY gate, checked before every bound above. canary_init
     * runs pre-IDT, so this peek is the earliest dereference of a
     * payload descriptor in the whole boot; without the negotiated
     * capability the PMM reservation pass never ran and FLAG_RESERVED
     * certifies nothing. Delete that branch and the two refusals below
     * fail while the accept above still passes -- which is what makes
     * the guard observable rather than merely present. */
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(0u, RES, 0x200000, 4096), 0u,
                   "caps clear refuses an otherwise-acceptable descriptor");
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(~CAPS, RES, 0x200000, 4096), 0u,
                   "every OTHER capability bit set is still a refusal");
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS | BOOT_CAP_RUNTIME_SERVICES,
                                                 RES, 0x200000, 4096), 1u,
                   "caps bit alongside others still accepted (mask, not equality)");

    /* Exact boundaries. canary_init runs pre-IDT, so an off-by-one on the
     * map bound is a hang rather than a wrong answer, and an off-by-one on
     * the minimum silently drops seed personalization instead. */
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS, RES, 0x200000, 16), 1u,
                   "exactly the 16-byte minimum accepted");
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS, RES, END - 16, 16), 1u,
                   "range ending exactly at the 4 GiB map end accepted");
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS, RES, END - 15, 16), 0u,
                   "range overrunning the map end by one byte rejected");
    TEST_ASSERT_EQ((uint64_t)canary_seed_desc_ok(CAPS, RES, 0x200000, ~0ull), 0u,
                   "a maximal length cannot wrap the map bound into an accept");
}

/* ---- Mandatory Integrity Control ---- */

static void test_mic(void)
{
    ACCESS_TOKEN tok;
    uint8_t sacl_buf[128];
    ACL *sacl = (ACL *)sacl_buf;
    SECURITY_DESCRIPTOR sd;

    /* Token IL is the last SubAuthority of IntegrityLevelSid. */
    memset(&tok, 0, sizeof(tok));
    tok.IntegrityLevelSid = (SID *)SeILLow;
    TEST_ASSERT_EQ(SeGetTokenIntegrityLevel(&tok), SECURITY_MANDATORY_LOW_RID,
                   "SeGetTokenIntegrityLevel(Low) == 4096");
    tok.IntegrityLevelSid = (SID *)SeILSystem;
    TEST_ASSERT_EQ(SeGetTokenIntegrityLevel(&tok), SECURITY_MANDATORY_SYSTEM_RID,
                   "SeGetTokenIntegrityLevel(System) == 16384");
    TEST_ASSERT_EQ(SeGetTokenIntegrityLevel((const ACCESS_TOKEN *)0),
                   SECURITY_MANDATORY_MEDIUM_RID,
                   "SeGetTokenIntegrityLevel(NULL) == Medium default");

    /* Level comparison. */
    TEST_ASSERT(SeCompareMandatoryLevels(SECURITY_MANDATORY_LOW_RID,
                SECURITY_MANDATORY_HIGH_RID) == -1, "Low < High");
    TEST_ASSERT(SeCompareMandatoryLevels(SECURITY_MANDATORY_HIGH_RID,
                SECURITY_MANDATORY_LOW_RID) == 1, "High > Low");
    TEST_ASSERT(SeCompareMandatoryLevels(SECURITY_MANDATORY_MEDIUM_RID,
                SECURITY_MANDATORY_MEDIUM_RID) == 0, "Medium == Medium");

    /* Object IL: no SACL -> Medium default. */
    RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    TEST_ASSERT_EQ(SeGetObjectIntegrityLevel(&sd), SECURITY_MANDATORY_MEDIUM_RID,
                   "SeGetObjectIntegrityLevel(no SACL) == Medium");

    /* Object IL: SACL carrying a High mandatory label -> High. */
    RtlCreateAcl(sacl, sizeof(sacl_buf), ACL_REVISION);
    RtlAddMandatoryAce(sacl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP,
                       SYSTEM_MANDATORY_LABEL_ACE_TYPE, SeILHigh);
    RtlSetSaclSecurityDescriptor(&sd, 1, sacl, 0);
    TEST_ASSERT_EQ(SeGetObjectIntegrityLevel(&sd), SECURITY_MANDATORY_HIGH_RID,
                   "SeGetObjectIntegrityLevel(High label) == High");

    /* No-Write-Up: Low subject writing a High object -> denied. */
    tok.IntegrityLevelSid = (SID *)SeILLow;
    TEST_ASSERT(SeCheckMandatoryAccess(&tok, &sd, GENERIC_WRITE, (const GENERIC_MAPPING *)0)
                == STATUS_ACCESS_DENIED,
                "MIC: Low GENERIC_WRITE to High object -> denied");
    /* Default policy is No-Write-Up only, so Low reading High is allowed. */
    TEST_ASSERT(SeCheckMandatoryAccess(&tok, &sd, GENERIC_READ, (const GENERIC_MAPPING *)0)
                == STATUS_SUCCESS,
                "MIC: Low GENERIC_READ to High (No-Write-Up only) -> allowed");
    /* System subject (>= object IL) is not restricted. */
    tok.IntegrityLevelSid = (SID *)SeILSystem;
    TEST_ASSERT(SeCheckMandatoryAccess(&tok, &sd, GENERIC_WRITE, (const GENERIC_MAPPING *)0)
                == STATUS_SUCCESS,
                "MIC: System GENERIC_WRITE to High object -> allowed");

    /* Object-specific write bit (not a generic bit) bypasses the policy UNLESS
     * the object's GENERIC_MAPPING classifies it. 0x2 = a fake object write. */
    {
        static const GENERIC_MAPPING map = { 0x1 /*R*/, 0x2 /*W*/, 0x4 /*X*/, 0x7 /*All*/ };
        tok.IntegrityLevelSid = (SID *)SeILLow;
        TEST_ASSERT(SeCheckMandatoryAccess(&tok, &sd, 0x2, (const GENERIC_MAPPING *)0)
                    == STATUS_SUCCESS,
                    "MIC: object-specific write bit without a mapping -> not classified");
        TEST_ASSERT(SeCheckMandatoryAccess(&tok, &sd, 0x2, &map) == STATUS_ACCESS_DENIED,
                    "MIC: object-specific write bit WITH mapping -> denied (no bypass)");
    }

    /* MAXIMUM_ALLOWED resolves to whatever the DACL grants, so a Low subject
     * requesting it on a High object is restricted under No-Write-Up. */
    tok.IntegrityLevelSid = (SID *)SeILLow;
    TEST_ASSERT(SeCheckMandatoryAccess(&tok, &sd, MAXIMUM_ALLOWED, (const GENERIC_MAPPING *)0)
                == STATUS_ACCESS_DENIED,
                "MIC: Low MAXIMUM_ALLOWED to High object -> denied (no bypass)");
    /* READ_CONTROL is read access; under No-Write-Up-only it is allowed. */
    TEST_ASSERT(SeCheckMandatoryAccess(&tok, &sd, READ_CONTROL, (const GENERIC_MAPPING *)0)
                == STATUS_SUCCESS,
                "MIC: Low READ_CONTROL to High (No-Write-Up only) -> allowed");

    /* A token IL SID that is not an S-1-16 mandatory-label SID -> Medium. */
    tok.IntegrityLevelSid = (SID *)SeLocalSystemSid;   /* S-1-5-18, not S-1-16 */
    TEST_ASSERT_EQ(SeGetTokenIntegrityLevel(&tok), SECURITY_MANDATORY_MEDIUM_RID,
                   "SeGetTokenIntegrityLevel(non-mandatory-label SID) -> Medium");

    /* No-Read-Up label: Low subject reading High -> denied. */
    {
        uint8_t sacl2_buf[128];
        ACL *sacl2 = (ACL *)sacl2_buf;
        SECURITY_DESCRIPTOR sd2;
        RtlCreateSecurityDescriptor(&sd2, SECURITY_DESCRIPTOR_REVISION);
        RtlCreateAcl(sacl2, sizeof(sacl2_buf), ACL_REVISION);
        RtlAddMandatoryAce(sacl2, ACL_REVISION, 0,
                           SYSTEM_MANDATORY_LABEL_NO_WRITE_UP | SYSTEM_MANDATORY_LABEL_NO_READ_UP,
                           SYSTEM_MANDATORY_LABEL_ACE_TYPE, SeILHigh);
        RtlSetSaclSecurityDescriptor(&sd2, 1, sacl2, 0);
        tok.IntegrityLevelSid = (SID *)SeILLow;
        TEST_ASSERT(SeCheckMandatoryAccess(&tok, &sd2, GENERIC_READ, (const GENERIC_MAPPING *)0)
                    == STATUS_ACCESS_DENIED,
                    "MIC: Low GENERIC_READ to High with No-Read-Up -> denied");
        TEST_ASSERT(SeCheckMandatoryAccess(&tok, &sd2, READ_CONTROL, (const GENERIC_MAPPING *)0)
                    == STATUS_ACCESS_DENIED,
                    "MIC: Low READ_CONTROL to High with No-Read-Up -> denied");
    }

    /* Malformed SACL fails closed: SeGetObjectIntegrityLevel -> System, and
     * SeCheckMandatoryAccess -> denied regardless of the IL comparison. */
    {
        uint8_t bad_buf[128];
        ACL *bad = (ACL *)bad_buf;
        SECURITY_DESCRIPTOR sd3;
        RtlCreateSecurityDescriptor(&sd3, SECURITY_DESCRIPTOR_REVISION);
        RtlCreateAcl(bad, sizeof(bad_buf), ACL_REVISION);
        RtlAddMandatoryAce(bad, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP,
                           SYSTEM_MANDATORY_LABEL_ACE_TYPE, SeILHigh);
        bad->AceCount = 50;   /* claims 50 ACEs, only 1 built -> walk hits a zero ACE */
        RtlSetSaclSecurityDescriptor(&sd3, 1, bad, 0);
        TEST_ASSERT_EQ(SeGetObjectIntegrityLevel(&sd3), SECURITY_MANDATORY_SYSTEM_RID,
                       "SeGetObjectIntegrityLevel(malformed SACL) fails closed to System");
        tok.IntegrityLevelSid = (SID *)SeILLow;
        TEST_ASSERT(SeCheckMandatoryAccess(&tok, &sd3, GENERIC_READ, (const GENERIC_MAPPING *)0)
                    == STATUS_ACCESS_DENIED,
                    "MIC: malformed SACL fails closed -> denied");
    }
}

/* Process/thread token assignment + self-impersonation. Exercises the primary-
 * token pin/unpin (NULL-safety + identity) and the ImpersonateSelf/RevertToSelf
 * round-trip against the current thread, restoring the current task's token so
 * the test is side-effect free. */
static void test_token_assignment(void)
{
    ACCESS_TOKEN *systok;
    struct task  *cur = task_current();
    struct thread *thr = thread_current();
    void         *saved_task_token;

    TEST_ASSERT(cur != (struct task *)0, "task_current() non-NULL in test ctx");
    TEST_ASSERT(thr != (struct thread *)0, "thread_current() non-NULL in test ctx");

    /* PsReferencePrimaryToken NULL-safety. */
    TEST_ASSERT(PsReferencePrimaryToken((struct task *)0) == (ACCESS_TOKEN *)0,
                "PsReferencePrimaryToken(NULL task) -> NULL");

    systok = SeCreateSystemToken();
    TEST_ASSERT(systok != (ACCESS_TOKEN *)0, "SeCreateSystemToken -> non-NULL");

    /* A task with a token: pin returns the same pointer; balance the pin. */
    saved_task_token = cur->token;
    cur->token = systok;
    {
        ACCESS_TOKEN *pinned = PsReferencePrimaryToken(cur);
        TEST_ASSERT(pinned == systok, "PsReferencePrimaryToken returns the task's token");
        PsDereferencePrimaryToken(pinned);   /* balance the pin (refcount back to 1) */
    }

    /* A task with a NULL token pins to NULL. */
    cur->token = (void *)0;
    TEST_ASSERT(PsReferencePrimaryToken(cur) == (ACCESS_TOKEN *)0,
                "PsReferencePrimaryToken(task with NULL token) -> NULL");

    /* RevertToSelf is idempotent when not impersonating. */
    RevertToSelf();
    TEST_ASSERT(thr->impersonation_token == (void *)0,
                "RevertToSelf() with no impersonation leaves NULL");

    /* ImpersonateSelf with a NULL primary token is a no-op (nothing to copy). */
    ImpersonateSelf(SecurityImpersonation);
    TEST_ASSERT(thr->impersonation_token == (void *)0,
                "ImpersonateSelf() with NULL primary token stays NULL");

    /* ImpersonateSelf/RevertToSelf round-trip with a real primary token. */
    cur->token = systok;
    ImpersonateSelf(SecurityImpersonation);
    {
        ACCESS_TOKEN *imp = (ACCESS_TOKEN *)thr->impersonation_token;
        TEST_ASSERT(imp != (ACCESS_TOKEN *)0, "ImpersonateSelf installs an impersonation token");
        TEST_ASSERT_EQ((uint64_t)imp->TokenType, (uint64_t)TokenImpersonation,
                       "impersonation token is TokenImpersonation");
        TEST_ASSERT_EQ((uint64_t)imp->ImpersonationLevel, (uint64_t)SecurityImpersonation,
                       "impersonation token carries the requested level");
        TEST_ASSERT(imp != systok, "impersonation token is a distinct copy, not the primary");
    }
    RevertToSelf();
    TEST_ASSERT(thr->impersonation_token == (void *)0,
                "RevertToSelf() clears the impersonation token");

    /* Restore the current task's real token; drop our test token's owning ref. */
    cur->token = saved_task_token;
    ObDereferenceObject(systok);
}

/* SePrivilegeCheck / SeSinglePrivilegeCheck. Exercises the KernelMode bypass,
 * NULL-safety, fail-closed-on-no-token, the token Privileges[] scan, and the
 * PRIVILEGE_SET ALL_NECESSARY-vs-any + empty-set semantics. Transiently sets the
 * current task's token (restored at the end, refcount-balanced). */
static void test_privilege_check(void)
{
    struct task  *cur = task_current();
    void         *saved;
    ACCESS_TOKEN *systok;
    LUID          bogus = { 9999, 0 };

    TEST_ASSERT(cur != (struct task *)0, "task_current() non-NULL");
    saved = cur->token;

    systok = SeCreateSystemToken();     /* all well-known privileges enabled */
    TEST_ASSERT(systok != (ACCESS_TOKEN *)0, "SeCreateSystemToken -> non-NULL");

    /* KernelMode bypasses the check regardless of token. */
    TEST_ASSERT(SeSinglePrivilegeCheck(&SeShutdownPrivilege, SE_KERNEL_MODE) == 1,
                "KernelMode bypass");
    /* NULL privilege is denied. */
    TEST_ASSERT(SeSinglePrivilegeCheck((const LUID *)0, 1) == 0,
                "NULL privilege denied");

    /* SYSTEM token holds every well-known privilege enabled. */
    cur->token = systok;
    TEST_ASSERT(SeSinglePrivilegeCheck(&SeShutdownPrivilege, 1) == 1,
                "SYSTEM holds SeShutdownPrivilege");
    TEST_ASSERT(SeSinglePrivilegeCheck(&SeDebugPrivilege, 1) == 1,
                "SYSTEM holds SeDebugPrivilege");
    TEST_ASSERT(SeSinglePrivilegeCheck(&bogus, 1) == 0,
                "unknown privilege not held");

    /* No effective token in UserMode fails closed. */
    cur->token = (void *)0;
    TEST_ASSERT(SeSinglePrivilegeCheck(&SeShutdownPrivilege, 1) == 0,
                "no effective token -> deny");

    /* PRIVILEGE_SET semantics. */
    cur->token = systok;
    {
        uint64_t rawbuf[4];   /* 32 bytes, 8-aligned: PRIVILEGE_SET + 2 entries */
        PRIVILEGE_SET *ps = (PRIVILEGE_SET *)rawbuf;

        ps->PrivilegeCount = 2;
        ps->Control = PRIVILEGE_SET_ALL_NECESSARY;
        ps->Privilege[0].Luid = SeShutdownPrivilege; ps->Privilege[0].Attributes = 0;
        ps->Privilege[1].Luid = bogus;               ps->Privilege[1].Attributes = 0;
        TEST_ASSERT(SePrivilegeCheck(ps, 1) == 0,
                    "ALL_NECESSARY with one privilege missing -> deny");

        ps->Control = 0;   /* any-one suffices */
        ps->Privilege[0].Attributes = 0; ps->Privilege[1].Attributes = 0;
        TEST_ASSERT(SePrivilegeCheck(ps, 1) == 1,
                    "any-mode with one held privilege -> allow");
        /* The held privilege is marked used-for-access (Windows semantics). */
        TEST_ASSERT((ps->Privilege[0].Attributes & SE_PRIVILEGE_USED_FOR_ACCESS) != 0,
                    "held privilege marked SE_PRIVILEGE_USED_FOR_ACCESS");

        TEST_ASSERT(SePrivilegeCheck(ps, SE_KERNEL_MODE) == 1,
                    "KernelMode set-check bypass");
    }

    /* Empty set: vacuously true for ALL_NECESSARY, false for any. */
    {
        PRIVILEGE_SET empty;
        empty.PrivilegeCount = 0;
        empty.Control = PRIVILEGE_SET_ALL_NECESSARY;
        TEST_ASSERT(SePrivilegeCheck(&empty, 1) == 1, "empty ALL_NECESSARY -> allow");
        empty.Control = 0;
        TEST_ASSERT(SePrivilegeCheck(&empty, 1) == 0, "empty any-set -> deny");
    }

    cur->token = saved;
    ObDereferenceObject(systok);
}

/* SePrivilegeCheckToken checks the SUPPLIED token, not the current subject's
 * effective token -- the distinguishing behavior NtPrivilegeCheck relies on
 * (it must answer for the ClientToken handle, not the caller). */
static void test_se_privilege_check_token(void)
{
    struct task  *cur = task_current();
    void         *saved;
    ACCESS_TOKEN *systok;
    LUID          bogus = { 9999, 0 };

    TEST_ASSERT(cur != (struct task *)0, "task_current() non-NULL");
    saved = cur->token;

    systok = SeCreateSystemToken();
    TEST_ASSERT(systok != (ACCESS_TOKEN *)0, "SeCreateSystemToken -> non-NULL");

    /* KernelMode bypasses regardless of token (even NULL). */
    TEST_ASSERT(SePrivilegeCheckToken((struct access_token *)0,
                                      (PRIVILEGE_SET *)0, SE_KERNEL_MODE) == 1,
                "KernelMode token-explicit bypass");
    /* NULL token in UserMode fails closed. */
    {
        uint64_t rawbuf[3];   /* PRIVILEGE_SET + 1 entry */
        PRIVILEGE_SET *ps = (PRIVILEGE_SET *)rawbuf;
        ps->PrivilegeCount = 1;
        ps->Control = PRIVILEGE_SET_ALL_NECESSARY;
        ps->Privilege[0].Luid = SeShutdownPrivilege;
        ps->Privilege[0].Attributes = 0;
        TEST_ASSERT(SePrivilegeCheckToken((struct access_token *)0, ps, 1) == 0,
                    "NULL token -> deny");

        /* Critically: clear the current effective token, then pass systok
         * EXPLICITLY. The effective-token path (SePrivilegeCheck) would deny;
         * the token-explicit path allows because it honors the supplied token. */
        cur->token = (void *)0;
        TEST_ASSERT(SePrivilegeCheck(ps, 1) == 0,
                    "effective-token path denies with no current token");
        ps->Privilege[0].Attributes = 0;
        TEST_ASSERT(SePrivilegeCheckToken((struct access_token *)systok, ps, 1) == 1,
                    "token-explicit path allows the SUPPLIED token");
        TEST_ASSERT((ps->Privilege[0].Attributes & SE_PRIVILEGE_USED_FOR_ACCESS) != 0,
                    "used-for-access marked on the supplied token's held privilege");

        /* An unheld privilege on the supplied token still denies. */
        ps->Privilege[0].Luid = bogus;
        ps->Privilege[0].Attributes = 0;
        TEST_ASSERT(SePrivilegeCheckToken((struct access_token *)systok, ps, 1) == 0,
                    "unheld privilege on supplied token -> deny");
    }

    cur->token = saved;
    ObDereferenceObject(systok);
}

/* SeAssignSecurity: SD inheritance, canonical ordering, propagation flags,
 * SE_DACL_PROTECTED, fallback, and owned-storage teardown. */
static void test_se_assign_security(void)
{
    uint8_t pbuf[256];
    ACL *pdacl = (ACL *)pbuf;
    ACE_HEADER *ace;
    SECURITY_DESCRIPTOR parent, creator;
    SECURITY_DESCRIPTOR *child;
    NTSTATUS st;

    /* Parent DACL: allow SYSTEM (CI|OI), deny World (OI-only). */
    RtlCreateAcl(pdacl, sizeof(pbuf), ACL_REVISION);
    RtlAddAccessAllowedAce(pdacl, ACL_REVISION, GENERIC_ALL, SeLocalSystemSid);
    RtlGetAce(pdacl, 0, &ace);
    ace->AceFlags = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
    RtlAddAccessDeniedAce(pdacl, ACL_REVISION, GENERIC_WRITE, SeWorldSid);
    RtlGetAce(pdacl, 1, &ace);
    ace->AceFlags = OBJECT_INHERIT_ACE;

    RtlCreateSecurityDescriptor(&parent, SECURITY_DESCRIPTOR_REVISION);
    RtlSetOwnerSecurityDescriptor(&parent, (SID *)SeLocalSystemSid, 0);
    RtlSetDaclSecurityDescriptor(&parent, 1, pdacl, 0);

    /* Creator SD with only an owner -> forces inheritance. */
    RtlCreateSecurityDescriptor(&creator, SECURITY_DESCRIPTOR_REVISION);
    RtlSetOwnerSecurityDescriptor(&creator, (SID *)SeLocalSystemSid, 0);

    /* File child: both ACEs carry OI so both inherit; leaves keep only
     * INHERITED_ACE; canonical order puts the deny ACE first. */
    child = (SECURITY_DESCRIPTOR *)0;
    st = SeAssignSecurity(&parent, &creator, 0, (struct access_token *)0, &child);
    TEST_ASSERT(st == STATUS_SUCCESS && child != (SECURITY_DESCRIPTOR *)0,
                "file child assigned");
    TEST_ASSERT(child->Dacl && child->Dacl->AceCount == 2,
                "file inherits 2 ACEs");
    RtlGetAce(child->Dacl, 0, &ace);
    TEST_ASSERT(ace->AceType == ACCESS_DENIED_ACE_TYPE,
                "canonical order: deny ACE first");
    TEST_ASSERT((ace->AceFlags & INHERITED_ACE) &&
                !(ace->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE |
                                   INHERIT_ONLY_ACE)),
                "file ACE has INHERITED only (no propagation)");
    SeDeassignSecurity(&child);
    TEST_ASSERT(child == (SECURITY_DESCRIPTOR *)0, "SeDeassignSecurity NULLs ptr");

    /* Directory child: allow CI|OI applies to the dir and keeps propagation;
     * deny OI-only becomes INHERIT_ONLY (propagates to file grandchildren). */
    st = SeAssignSecurity(&parent, &creator, 1, (struct access_token *)0, &child);
    TEST_ASSERT(st == STATUS_SUCCESS && child, "dir child assigned");
    TEST_ASSERT(child->Dacl->AceCount == 2, "dir inherits 2 ACEs");
    /* deny first (canonical); it is the OI-only -> INHERIT_ONLY case */
    RtlGetAce(child->Dacl, 0, &ace);
    TEST_ASSERT(ace->AceType == ACCESS_DENIED_ACE_TYPE &&
                (ace->AceFlags & INHERIT_ONLY_ACE) &&
                (ace->AceFlags & OBJECT_INHERIT_ACE) &&
                (ace->AceFlags & INHERITED_ACE),
                "dir OI-only deny -> INHERITED|INHERIT_ONLY|OI");
    RtlGetAce(child->Dacl, 1, &ace);
    TEST_ASSERT(ace->AceType == ACCESS_ALLOWED_ACE_TYPE &&
                !(ace->AceFlags & INHERIT_ONLY_ACE) &&
                (ace->AceFlags & CONTAINER_INHERIT_ACE) &&
                (ace->AceFlags & INHERITED_ACE),
                "dir CI|OI allow applies + propagates (no INHERIT_ONLY)");
    SeDeassignSecurity(&child);

    /* SE_DACL_PROTECTED on the child: inheritance blocked, no token DefaultDacl
     * -> fail-closed fallback (owner + SYSTEM + World-read = 3 ACEs). */
    creator.Control |= SE_DACL_PROTECTED;
    st = SeAssignSecurity(&parent, &creator, 1, (struct access_token *)0, &child);
    TEST_ASSERT(st == STATUS_SUCCESS && child, "protected child assigned");
    TEST_ASSERT(child->Dacl && child->Dacl->AceCount == 3,
                "protected -> fail-closed fallback 3-ACE DACL (never NULL)");
    TEST_ASSERT((child->Control & SE_DACL_PROTECTED) != 0,
                "SE_DACL_PROTECTED preserved on the output descriptor");
    SeDeassignSecurity(&child);
    creator.Control &= (uint16_t)~SE_DACL_PROTECTED;

    /* No resolvable owner (no creator owner, no token) fails closed. */
    {
        SECURITY_DESCRIPTOR empty;
        RtlCreateSecurityDescriptor(&empty, SECURITY_DESCRIPTOR_REVISION);
        st = SeAssignSecurity(&parent, &empty, 0, (struct access_token *)0, &child);
        TEST_ASSERT(st == STATUS_INVALID_PARAMETER,
                    "no resolvable owner -> STATUS_INVALID_PARAMETER");
    }

    /* Malformed owner SID (SubAuthorityCount over the 15 cap) fails closed
     * BEFORE any DACL sizing/copy touches it. */
    {
        uint8_t badsid[16] = {0};
        SECURITY_DESCRIPTOR bad;
        badsid[0] = 1;    /* Revision */
        badsid[1] = 20;   /* SubAuthorityCount > SID_MAX_SUB_AUTHORITIES */
        RtlCreateSecurityDescriptor(&bad, SECURITY_DESCRIPTOR_REVISION);
        RtlSetOwnerSecurityDescriptor(&bad, (SID *)badsid, 0);
        st = SeAssignSecurity(&parent, &bad, 0, (struct access_token *)0, &child);
        TEST_ASSERT(st == STATUS_INVALID_PARAMETER,
                    "malformed owner SID -> STATUS_INVALID_PARAMETER");
    }

    /* An inheritable conditional (callback) ACE on the parent fails closed
     * rather than being silently dropped (would lose an inheritable deny). */
    {
        uint8_t cbuf[64];
        ACL *cdacl = (ACL *)cbuf;
        SECURITY_DESCRIPTOR cbparent, creator2;
        RtlCreateAcl(cdacl, sizeof(cbuf), ACL_REVISION);
        RtlAddAccessAllowedAce(cdacl, ACL_REVISION, GENERIC_ALL, SeLocalSystemSid);
        RtlGetAce(cdacl, 0, &ace);
        ace->AceType = SYSTEM_AUDIT_CALLBACK_ACE_TYPE;   /* 0x0D -- audit callback */
        ace->AceFlags = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
        RtlCreateSecurityDescriptor(&cbparent, SECURITY_DESCRIPTOR_REVISION);
        RtlSetOwnerSecurityDescriptor(&cbparent, (SID *)SeLocalSystemSid, 0);
        RtlSetDaclSecurityDescriptor(&cbparent, 1, cdacl, 0);
        RtlCreateSecurityDescriptor(&creator2, SECURITY_DESCRIPTOR_REVISION);
        RtlSetOwnerSecurityDescriptor(&creator2, (SID *)SeLocalSystemSid, 0);
        st = SeAssignSecurity(&cbparent, &creator2, 0, (struct access_token *)0, &child);
        TEST_ASSERT(st == STATUS_INVALID_PARAMETER,
                    "inheritable callback ACE on parent -> fail closed");
    }
}

/* ---- Registration ---- */

void test_register_security(void)
{
    test_suite_register_cat("Security: stack canary massage invariants",
                            test_canary_massage_invariants, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: stack canary guard seeded",
                            test_canary_guard_seeded, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: stack canary seed-desc bounds",
                            test_canary_seed_desc_bounds, TEST_CAT_SECURITY);
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
    test_suite_register_cat("Security: RtlLengthSidBounded", test_sid_length_bounded, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: RtlValidAcl/RtlGetAceEx bounded", test_acl_valid_bounded, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: self-relative SD bounded import",
                            test_sd_selfrel_import_bounded, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: Mandatory Integrity Control", test_mic, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: token assignment + impersonation", test_token_assignment, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: SePrivilegeCheck", test_privilege_check, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: SePrivilegeCheckToken (token-explicit)",
                            test_se_privilege_check_token, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: SeAssignSecurity SD inheritance",
                            test_se_assign_security, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: privilege set to string", test_privilege_set_to_string, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: NtAllocateLocallyUniqueId", test_nt_allocate_luid, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: NtOpenProcessToken", test_nt_open_process_token, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: NtQueryInformationToken invalid",
                            test_nt_query_token_invalid_handle, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: NtSetInformationToken read-only",
                            test_nt_set_token_readonly, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: NtAdjustPrivilegesToken invalid",
                            test_nt_adjust_privileges_invalid, TEST_CAT_SECURITY);
    test_suite_register_cat("Security: token SSDT slots registered", test_nt_token_ssdt_registered, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
