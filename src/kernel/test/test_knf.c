/* test_knf.c -- Kernel Notification Facility unit tests (TEST_CAT_KNF, TODO-16).
 *
 * Object-model layer: the "NotificationState" Ob type, the \Notifications
 * namespace tree, knf_create_state / knf_lookup_state, lifetime classes, and
 * the SeCreatePermanentPrivilege wiring. No live boot infrastructure (test
 * policy): the \Notifications tree is built by knf_init() at boot, so the
 * tests only exercise the runtime Ob + create/lookup paths against it.
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/knf/knf.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/security/privileges.h"

extern int strcmp(const char *a, const char *b);

/* ---- Type registration -------------------------------------------------- */

static void test_knf_type_registered(void)
{
    TEST_ASSERT_NOT_NULL((void *)ObpNotificationStateType,
                         "NotificationState Ob type registered by knf_init");
}

/* ---- Namespace tree ----------------------------------------------------- */

static void test_knf_categories_exist(void)
{
    void *dir = NULL;

    TEST_ASSERT_EQ(ObLookupObjectByName("\\Notifications", ObpDirectoryType,
                                        0, &dir), 0,
                   "\\Notifications root directory resolves");
    TEST_ASSERT_NOT_NULL(dir, "\\Notifications non-NULL");
    if (dir) ObDereferenceObject(dir);

    dir = NULL;
    TEST_ASSERT_EQ(ObLookupObjectByName("\\Notifications\\Kernel",
                                        ObpDirectoryType, 0, &dir), 0,
                   "\\Notifications\\Kernel resolves");
    if (dir) ObDereferenceObject(dir);

    dir = NULL;
    TEST_ASSERT_EQ(ObLookupObjectByName("\\Notifications\\Power",
                                        ObpDirectoryType, 0, &dir), 0,
                   "\\Notifications\\Power resolves");
    if (dir) ObDereferenceObject(dir);

    dir = NULL;
    TEST_ASSERT_EQ(ObLookupObjectByName("\\Notifications\\Security",
                                        ObpDirectoryType, 0, &dir), 0,
                   "\\Notifications\\Security resolves");
    if (dir) ObDereferenceObject(dir);

    dir = NULL;
    TEST_ASSERT_EQ(ObLookupObjectByName("\\Notifications\\Session",
                                        ObpDirectoryType, 0, &dir), 0,
                   "\\Notifications\\Session resolves");
    if (dir) ObDereferenceObject(dir);
}

/* ---- Create + lookup ---------------------------------------------------- */

static void test_knf_create_and_lookup(void)
{
    KNF_STATE *st;

    /* PERMANENT so the state persists across test re-runs (create-or-open). */
    st = knf_create_state("Kernel", "UtestState", KNF_LIFETIME_PERMANENT,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "knf_create_state(Kernel\\UtestState) succeeds");
    if (st) {
        TEST_ASSERT_EQ((uint64_t)atomic64_read(&st->sequence), 0ull,
                       "fresh state sequence == 0");
        TEST_ASSERT_EQ(strcmp(st->name, "UtestState"), 0,
                       "state name stored");
        TEST_ASSERT_EQ((int)st->lifetime, (int)KNF_LIFETIME_PERMANENT,
                       "lifetime recorded");
        ObDereferenceObject(st);
    }

    st = knf_lookup_state("Kernel", "UtestState");
    TEST_ASSERT_NOT_NULL(st, "knf_lookup_state finds the created state");
    if (st) ObDereferenceObject(st);

    TEST_ASSERT_NULL((void *)knf_lookup_state("Kernel", "NoSuchState"),
                     "lookup of absent state returns NULL");

    /* Clean up: delete removes the state from the namespace and frees it. */
    TEST_ASSERT_EQ(knf_delete_state("Kernel", "UtestState"), 0,
                   "knf_delete_state removes the created state");
    TEST_ASSERT_NULL((void *)knf_lookup_state("Kernel", "UtestState"),
                     "state gone after delete");
}

/* ---- Create-or-open (one name == one state) ----------------------------- */

static void test_knf_create_or_open(void)
{
    KNF_STATE *a, *b;

    a = knf_create_state("Power", "UtestDup", KNF_LIFETIME_PERMANENT,
                         KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                         KNF_KERNEL_MODE);
    b = knf_create_state("Power", "UtestDup", KNF_LIFETIME_PERMANENT,
                         KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                         KNF_KERNEL_MODE);

    TEST_ASSERT_NOT_NULL(a, "first create succeeds");
    TEST_ASSERT_NOT_NULL(b, "second create opens existing");
    TEST_ASSERT(a == b, "same name resolves to the same state object");

    if (a) ObDereferenceObject(a);
    if (b) ObDereferenceObject(b);
    knf_delete_state("Power", "UtestDup");
}

/* ---- Argument validation ------------------------------------------------ */

static void test_knf_create_bad_args(void)
{
    char longname[KNF_NAME_MAX + 8];
    uint32_t i;

    TEST_ASSERT_NULL((void *)knf_create_state((const char *)0, "X",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "NULL category rejected");
    TEST_ASSERT_NULL((void *)knf_create_state("Kernel", "",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "empty name rejected");

    for (i = 0; i < sizeof(longname) - 1; i++)
        longname[i] = 'a';
    longname[sizeof(longname) - 1] = '\0';
    TEST_ASSERT_NULL((void *)knf_create_state("Kernel", longname,
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "over-length name rejected");

    TEST_ASSERT_NULL((void *)knf_create_state("NoSuchCategory", "X",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "unknown category rejected");

    /* Path separators in a leaf name are rejected (a name is one component). */
    TEST_ASSERT_NULL((void *)knf_create_state("Kernel", "foo\\bar",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "name with backslash rejected");
    TEST_ASSERT_NULL((void *)knf_create_state("Kernel", "foo/bar",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "name with forward slash rejected");

    /* An out-of-range lifetime is rejected (must not bypass the privilege
     * gate while still receiving OB_FLAG_PERMANENT). */
    TEST_ASSERT_NULL((void *)knf_create_state("Kernel", "UtestBadLife",
                        (KNF_LIFETIME)99, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "out-of-range lifetime rejected");
}

/* ---- Kernel-mode bypasses the permanent-create privilege ---------------- */

static void test_knf_kernel_mode_permanent_bypass(void)
{
    KNF_STATE *st;

    /* Kernel-mode callers are trusted: PERMANENT create must not be gated. */
    st = knf_create_state("Session", "UtestPerm", KNF_LIFETIME_PERMANENT,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st,
        "kernel-mode PERMANENT create bypasses SeCreatePermanentPrivilege");
    if (st) ObDereferenceObject(st);
    knf_delete_state("Session", "UtestPerm");
}

/* ---- Header flags per lifetime + category ------------------------------- */

static void test_knf_header_flags(void)
{
    KNF_STATE     *perm, *temp, *sec;
    OBJECT_HEADER *h;

    /* Permanent -> OB_FLAG_PERMANENT set. */
    perm = knf_create_state("Kernel", "UtestFlagPerm", KNF_LIFETIME_PERMANENT,
                            KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                            KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(perm, "permanent create succeeds");
    if (perm) {
        h = OB_HEADER_FROM_BODY(perm);
        TEST_ASSERT((h->flags & OB_FLAG_PERMANENT) != 0,
                    "permanent state has OB_FLAG_PERMANENT");
        ObDereferenceObject(perm);
    }
    knf_delete_state("Kernel", "UtestFlagPerm");

    /* Temporary -> OB_FLAG_PERMANENT clear. */
    temp = knf_create_state("Kernel", "UtestFlagTemp", KNF_LIFETIME_TEMPORARY,
                            KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                            KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(temp, "temporary create succeeds");
    if (temp) {
        h = OB_HEADER_FROM_BODY(temp);
        TEST_ASSERT((h->flags & OB_FLAG_PERMANENT) == 0,
                    "temporary state is not OB_FLAG_PERMANENT");
        ObDereferenceObject(temp);
    }
    knf_delete_state("Kernel", "UtestFlagTemp");

    /* Security category -> OB_FLAG_KERNEL_ONLY set. */
    sec = knf_create_state("Security", "UtestFlagSec", KNF_LIFETIME_PERMANENT,
                           KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                           KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(sec, "security-category create succeeds");
    if (sec) {
        h = OB_HEADER_FROM_BODY(sec);
        TEST_ASSERT((h->flags & OB_FLAG_KERNEL_ONLY) != 0,
                    "security-category state is OB_FLAG_KERNEL_ONLY");
        ObDereferenceObject(sec);
    }
    knf_delete_state("Security", "UtestFlagSec");
}

/* ---- Name-length boundary (leaf must fit a namespace component) ---------- */

static void test_knf_name_boundary(void)
{
    char maxname[KNF_NAME_MAX];      /* KNF_NAME_MAX-1 usable chars + NUL */
    char overname[KNF_NAME_MAX + 1]; /* exactly KNF_NAME_MAX chars: rejected */
    KNF_STATE *st;
    uint32_t i;

    for (i = 0; i < KNF_NAME_MAX - 1; i++)
        maxname[i] = 'm';
    maxname[KNF_NAME_MAX - 1] = '\0';

    st = knf_create_state("Kernel", maxname, KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st,
        "max-length (KNF_NAME_MAX-1) leaf name is accepted + inserts");
    if (st) ObDereferenceObject(st);
    knf_delete_state("Kernel", maxname);

    for (i = 0; i < KNF_NAME_MAX; i++)
        overname[i] = 'o';
    overname[KNF_NAME_MAX] = '\0';
    TEST_ASSERT_NULL((void *)knf_create_state("Kernel", overname,
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "name of exactly KNF_NAME_MAX chars is rejected");
}

/* ---- User-mode permanent/persistent create is privilege-gated ----------- *
 * The test subject's effective token does not hold SeCreatePermanentPrivilege,
 * so a user-mode Permanent or Persistent create must be DENIED (NULL), while a
 * Temporary create (no privilege needed) succeeds. This exercises the
 * access_mode != KERNEL_MODE deny branch of the privilege gate. Fine-grained
 * allow/deny with an explicit restricted-token fixture is owned by the KNF
 * security / namespace access-policy section. */
static void test_knf_user_mode_permanent_denied(void)
{
    KNF_STATE *st;

    TEST_ASSERT_NULL((void *)knf_create_state("Session", "UtestUserPerm",
                        KNF_LIFETIME_PERMANENT, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_USER_MODE),
                     "user-mode PERMANENT create denied without privilege");
    TEST_ASSERT_NULL((void *)knf_create_state("Session", "UtestUserPersist",
                        KNF_LIFETIME_PERSISTENT, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_USER_MODE),
                     "user-mode PERSISTENT create denied without privilege");
    TEST_ASSERT_NULL((void *)knf_create_state("Session", "UtestUserWellKnown",
                        KNF_LIFETIME_WELLKNOWN, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_USER_MODE),
                     "user-mode WELLKNOWN create denied without privilege");

    /* Temporary needs no privilege: a user-mode create still succeeds. */
    st = knf_create_state("Session", "UtestUserTemp", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_USER_MODE);
    TEST_ASSERT_NOT_NULL(st, "user-mode TEMPORARY create needs no privilege");
    if (st) ObDereferenceObject(st);
    knf_delete_state("Session", "UtestUserTemp");
}

/* ---- SeCreatePermanentPrivilege wiring ---------------------------------- */

static void test_knf_permanent_privilege_name(void)
{
    const char *name = RtlPrivilegeLuidToName(&SeCreatePermanentPrivilege);

    TEST_ASSERT_NOT_NULL((void *)name,
                         "SeCreatePermanentPrivilege LUID has a name");
    if (name)
        TEST_ASSERT_EQ(strcmp(name, "SeCreatePermanentPrivilege"), 0,
                       "LUID 16 maps to SeCreatePermanentPrivilege");
    TEST_ASSERT_EQ((uint32_t)SeCreatePermanentPrivilege.LowPart, 16u,
                   "SeCreatePermanentPrivilege LowPart == 16 (WNF/Windows)");
}

/* ---- Registration ------------------------------------------------------- */

void test_register_knf(void)
{
    test_suite_register_cat("knf: NotificationState type registered",
                            test_knf_type_registered, TEST_CAT_KNF);
    test_suite_register_cat("knf: \\Notifications category tree exists",
                            test_knf_categories_exist, TEST_CAT_KNF);
    test_suite_register_cat("knf: create + lookup state (seq 0)",
                            test_knf_create_and_lookup, TEST_CAT_KNF);
    test_suite_register_cat("knf: create-or-open (one name == one state)",
                            test_knf_create_or_open, TEST_CAT_KNF);
    test_suite_register_cat("knf: create arg validation",
                            test_knf_create_bad_args, TEST_CAT_KNF);
    test_suite_register_cat("knf: kernel-mode permanent-create bypass",
                            test_knf_kernel_mode_permanent_bypass, TEST_CAT_KNF);
    test_suite_register_cat("knf: header flags per lifetime + category",
                            test_knf_header_flags, TEST_CAT_KNF);
    test_suite_register_cat("knf: leaf name-length boundary",
                            test_knf_name_boundary, TEST_CAT_KNF);
    test_suite_register_cat("knf: user-mode permanent create denied (no priv)",
                            test_knf_user_mode_permanent_denied, TEST_CAT_KNF);
    test_suite_register_cat("knf: SeCreatePermanentPrivilege wiring",
                            test_knf_permanent_privilege_name, TEST_CAT_KNF);
}

#endif /* KERNEL_TESTS */
