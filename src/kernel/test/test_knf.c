/* test_knf.c -- Kernel Notification Facility unit tests (TEST_CAT_KNF, TODO-16).
 *
 * Object-model layer: the "NotificationState" Ob type, the \Notifications
 * namespace tree, knf_create_state / knf_lookup_state, lifetime classes, and
 * the SeCreatePermanentPrivilege wiring. No live boot infrastructure (test
 * policy): the \Notifications tree is built by knf_init() at boot, so the
 * tests only exercise the runtime Ob + create/lookup paths against it.
 *
 * Publish/subscribe layer: knf_publish (sequence advance, payload cap,
 * conditional/CAS publish, typed-payload enforcement), knf_subscribe /
 * knf_unsubscribe / knf_subscription_poll (prev/new delivery, state-pin
 * lifetime), and knf_reserve_payload (non-notifying pre-size).
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/knf/knf.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/security/privileges.h"
#include "kernel/nt/ntstatus.h"

extern int   strcmp(const char *a, const char *b);
extern void *memset(void *s, int c, size_t n);

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

    /* Idempotent / stale-reference contract: deleting an already-gone state
     * reports not-found (-1) rather than double-freeing or succeeding twice. */
    TEST_ASSERT_EQ(knf_delete_state("Kernel", "UtestState"), -1,
                   "second delete of a gone state returns -1 (not found)");
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

    /* Category is a single component too: empty or separator-bearing category
     * is rejected before it can alias a directory (e.g. "Security\\"). */
    TEST_ASSERT_NULL((void *)knf_create_state("", "X",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "empty category rejected");
    TEST_ASSERT_NULL((void *)knf_create_state("Security\\", "X",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "category with backslash rejected (no aliasing)");

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

/* ---- Publish: sequence advance + payload cap ---------------------------- */

static void test_knf_publish_sequence(void)
{
    KNF_STATE *st;
    uint64_t   prev = 999, cur = 999;
    NTSTATUS   s;
    char       payload[16] = { 0 };

    st = knf_create_state("Kernel", "PubSeq", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create PubSeq");
    if (!st) return;

    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                    &prev, &cur);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "first publish OK");
    TEST_ASSERT_EQ(prev, 0ull, "first publish prev == 0");
    TEST_ASSERT_EQ(cur, 1ull, "first publish new == 1");

    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 8, (const uint64_t *)0,
                    &prev, &cur);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "second publish OK");
    TEST_ASSERT_EQ(prev, 1ull, "second publish prev == 1");
    TEST_ASSERT_EQ(cur, 2ull, "second publish new == 2");
    TEST_ASSERT_EQ((uint64_t)atomic64_read(&st->sequence), 2ull,
                   "sequence == 2 after two publishes");

    /* Payload actually retained: len/cap tracked + bytes preserved. */
    TEST_ASSERT_EQ((uint64_t)st->payload_len, 8ull,
                   "payload_len tracks the last publish length");
    TEST_ASSERT_NOT_NULL(st->payload, "payload buffer allocated");

    /* Exact 4096-byte boundary accepted; first/last bytes preserved. */
    {
        static char big[KNF_MAX_PAYLOAD];
        big[0]                  = (char)0xA5;
        big[KNF_MAX_PAYLOAD - 1] = (char)0x5A;
        s = knf_publish(st, (const KNF_TYPE_ID *)0, big, KNF_MAX_PAYLOAD,
                        (const uint64_t *)0, (uint64_t *)0, &cur);
        TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                       "exactly 4096-byte publish accepted");
        TEST_ASSERT_EQ((uint64_t)st->payload_len, (uint64_t)KNF_MAX_PAYLOAD,
                       "payload_len == 4096 after max publish");
        TEST_ASSERT_EQ((uint64_t)(st->payload_cap >= KNF_MAX_PAYLOAD), 1ull,
                       "payload_cap grew to >= 4096");
        TEST_ASSERT_EQ((uint64_t)(uint8_t)((char *)st->payload)[0], 0xA5ull,
                       "first payload byte preserved");
        TEST_ASSERT_EQ((uint64_t)(uint8_t)((char *)st->payload)[KNF_MAX_PAYLOAD - 1],
                       0x5Aull, "last payload byte preserved");
    }

    /* Over the 4096-byte cap: rejected before any payload copy. */
    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 5000,
                    (const uint64_t *)0, (uint64_t *)0, (uint64_t *)0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_INVALID_PARAMETER,
                   "5000-byte publish rejected (over 4096 cap)");
    TEST_ASSERT_EQ((uint64_t)atomic64_read(&st->sequence), 3ull,
                   "rejected publish did not advance the sequence");

    /* Sequence-only (empty) publish: advances the stamp, zero-length payload. */
    s = knf_publish(st, (const KNF_TYPE_ID *)0, (const void *)0, 0,
                    (const uint64_t *)0, &prev, &cur);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "empty (signal) publish accepted");
    TEST_ASSERT_EQ(prev, 3ull, "empty publish prev == 3");
    TEST_ASSERT_EQ(cur, 4ull, "empty publish new == 4");
    TEST_ASSERT_EQ((uint64_t)st->payload_len, 0ull,
                   "empty publish sets payload_len == 0");

    /* NULL data with len>0: rejected, sequence unchanged. */
    s = knf_publish(st, (const KNF_TYPE_ID *)0, (const void *)0, 1,
                    (const uint64_t *)0, (uint64_t *)0, (uint64_t *)0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_INVALID_PARAMETER,
                   "len>0 with NULL data rejected");
    TEST_ASSERT_EQ((uint64_t)atomic64_read(&st->sequence), 4ull,
                   "rejected NULL-data publish did not advance the sequence");

    ObDereferenceObject(st);
    knf_delete_state("Kernel", "PubSeq");
}

/* ---- Subscribe + poll: prev/new delivery -------------------------------- */

static void test_knf_subscribe_poll(void)
{
    KNF_STATE             *st;
    struct knf_subscriber *sub = (struct knf_subscriber *)0;
    uint64_t               prev = 999, cur = 999;
    NTSTATUS               s;
    char                   payload[8] = { 0 };

    st = knf_create_state("Kernel", "PubSub", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create PubSub");
    if (!st) return;

    /* Publish once BEFORE subscribing: the subscriber baselines at seq 1. */
    knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                (uint64_t *)0, (uint64_t *)0);

    s = knf_subscribe(st, &sub);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "subscribe OK");
    TEST_ASSERT_NOT_NULL(sub, "subscriber handle non-NULL");

    /* No publish yet since subscribing: nothing pending. */
    s = knf_subscription_poll(sub, &prev, &cur);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_NO_MORE_ENTRIES,
                   "poll before any publish -> NO_MORE_ENTRIES");

    /* Advance the sequence 1 -> 2; subscriber should see (prev=1, new=2). */
    knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                (uint64_t *)0, (uint64_t *)0);
    s = knf_subscription_poll(sub, &prev, &cur);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "poll after publish OK");
    TEST_ASSERT_EQ(prev, 1ull, "subscriber sees prev == 1");
    TEST_ASSERT_EQ(cur, 2ull, "subscriber sees new == 2");

    /* Second poll with no new publish: drained. */
    s = knf_subscription_poll(sub, &prev, &cur);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_NO_MORE_ENTRIES,
                   "second poll drains -> NO_MORE_ENTRIES");

    TEST_ASSERT_EQ((uint32_t)knf_unsubscribe(&sub),
                   (uint32_t)STATUS_SUCCESS, "unsubscribe OK");
    TEST_ASSERT_NULL((void *)sub,
                     "unsubscribe consumed the handle (pointer nulled)");
    /* Double unsubscribe is safe: *psub is NULL, no freed-pointer deref. */
    TEST_ASSERT_EQ((uint32_t)knf_unsubscribe(&sub),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "double unsubscribe -> INVALID_PARAMETER (handle consumed)");

    ObDereferenceObject(st);
    knf_delete_state("Kernel", "PubSub");
}

/* ---- Multi-subscriber fanout + coalescing + not-found unsubscribe ------- */

static void test_knf_multi_subscriber_coalesce(void)
{
    KNF_STATE             *st;
    struct knf_subscriber *a = (struct knf_subscriber *)0;
    struct knf_subscriber *b = (struct knf_subscriber *)0;
    struct knf_subscriber  stacknode;
    struct knf_subscriber *stackp = &stacknode;
    uint64_t               pa = 0, na = 0, pb = 0, nb = 0;
    NTSTATUS               s;
    char                   payload[4] = { 0 };

    st = knf_create_state("Kernel", "PubFan", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create PubFan");
    if (!st) return;

    knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                (uint64_t *)0, (uint64_t *)0);   /* seq -> 1; both baseline 1 */
    TEST_ASSERT_EQ((uint32_t)knf_subscribe(st, &a),
                   (uint32_t)STATUS_SUCCESS, "subscribe a");
    TEST_ASSERT_EQ((uint32_t)knf_subscribe(st, &b),
                   (uint32_t)STATUS_SUCCESS, "subscribe b");

    /* One publish, both subscribers armed with the same (prev,new). */
    knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                (uint64_t *)0, (uint64_t *)0);   /* seq -> 2 */
    s = knf_subscription_poll(a, &pa, &na);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "poll a delivered");
    s = knf_subscription_poll(b, &pb, &nb);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "poll b delivered");
    TEST_ASSERT_EQ(pa, 1ull, "a prev == 1");
    TEST_ASSERT_EQ(na, 2ull, "a new == 2");
    TEST_ASSERT_EQ(pb, 1ull, "b prev == 1 (fanout reaches non-head node)");
    TEST_ASSERT_EQ(nb, 2ull, "b new == 2");

    /* Coalescing: two publishes before a single poll collapse to one
     * notification spanning the whole advance (prev=baseline, new=latest). */
    knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                (uint64_t *)0, (uint64_t *)0);   /* seq -> 3 */
    knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                (uint64_t *)0, (uint64_t *)0);   /* seq -> 4 */
    s = knf_subscription_poll(a, &pa, &na);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "coalesced poll a");
    TEST_ASSERT_EQ(pa, 2ull, "coalesced prev == 2 (baseline before the burst)");
    TEST_ASSERT_EQ(na, 4ull, "coalesced new == 4 (latest sequence)");
    s = knf_subscription_poll(a, &pa, &na);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_NO_MORE_ENTRIES,
                   "coalesced burst drained in one poll");

    /* Not-found unsubscribe: a node that was never linked (state set, off-list)
     * returns NOT_FOUND without freeing (no Ob ref was taken). */
    memset(&stacknode, 0, sizeof(stacknode));
    stacknode.state = st;
    TEST_ASSERT_EQ((uint32_t)knf_unsubscribe(&stackp),
                   (uint32_t)STATUS_NOT_FOUND,
                   "unsubscribe of an unlinked node -> NOT_FOUND");

    TEST_ASSERT_EQ((uint32_t)knf_unsubscribe(&a),
                   (uint32_t)STATUS_SUCCESS, "unsubscribe a");
    TEST_ASSERT_EQ((uint32_t)knf_unsubscribe(&b),
                   (uint32_t)STATUS_SUCCESS, "unsubscribe b");

    ObDereferenceObject(st);
    knf_delete_state("Kernel", "PubFan");
}

/* ---- Open-state (resolve existing, no create) --------------------------- */

static void test_knf_open_state(void)
{
    KNF_STATE *st, *op;

    /* Absent state: open returns NULL and does NOT create it. */
    TEST_ASSERT_NULL((void *)knf_open_state("Kernel", "MissingOpen"),
                     "open of absent state -> NULL");
    TEST_ASSERT_NULL((void *)knf_lookup_state("Kernel", "MissingOpen"),
                     "open did not create the state");

    st = knf_create_state("Kernel", "OpenMe", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create OpenMe");
    if (!st) return;

    op = knf_open_state("Kernel", "OpenMe");
    TEST_ASSERT_NOT_NULL(op, "open of existing state -> referenced non-NULL");
    if (op) ObDereferenceObject(op);

    /* Argument validation. */
    TEST_ASSERT_NULL((void *)knf_open_state((const char *)0, "OpenMe"),
                     "open with NULL category -> NULL");
    TEST_ASSERT_NULL((void *)knf_open_state("Kernel", "bad/name"),
                     "open with separator in name -> NULL");

    ObDereferenceObject(st);
    knf_delete_state("Kernel", "OpenMe");
}

/* ---- Conditional (CAS) publish ------------------------------------------ */

static void test_knf_cas_publish(void)
{
    KNF_STATE *st;
    uint64_t   stamp;
    NTSTATUS   s;
    char       payload[4] = { 0 };

    st = knf_create_state("Kernel", "PubCas", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create PubCas");
    if (!st) return;

    /* Matching stamp 0 == current: publish proceeds (seq -> 1). */
    stamp = 0;
    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, &stamp,
                    (uint64_t *)0, (uint64_t *)0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "CAS publish with matching stamp 0 succeeds");
    TEST_ASSERT_EQ((uint64_t)atomic64_read(&st->sequence), 1ull, "seq -> 1");

    /* Stale stamp 0 (current is 1): publish rejected, sequence unchanged. */
    stamp = 0;
    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, &stamp,
                    (uint64_t *)0, (uint64_t *)0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_UNSUCCESSFUL,
                   "CAS publish with stale stamp 0 rejected");
    TEST_ASSERT_EQ((uint64_t)atomic64_read(&st->sequence), 1ull,
                   "rejected CAS publish left seq == 1");

    /* Correct stamp 1: publish proceeds (seq -> 2). */
    stamp = 1;
    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, &stamp,
                    (uint64_t *)0, (uint64_t *)0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "CAS publish with matching stamp 1 succeeds");
    TEST_ASSERT_EQ((uint64_t)atomic64_read(&st->sequence), 2ull, "seq -> 2");

    ObDereferenceObject(st);
    knf_delete_state("Kernel", "PubCas");
}

/* ---- Typed-payload enforcement ------------------------------------------ */

static void test_knf_type_enforcement(void)
{
    KNF_STATE  *st;
    KNF_TYPE_ID good = { { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 } };
    KNF_TYPE_ID bad  = { { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 } };
    NTSTATUS    s;
    char        payload[4] = { 0 };

    st = knf_create_state("Kernel", "PubTyped", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, &good, KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create typed PubTyped");
    if (!st) return;

    s = knf_publish(st, &good, payload, 4, (const uint64_t *)0,
                    (uint64_t *)0, (uint64_t *)0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "publish with matching type_id succeeds");

    s = knf_publish(st, &bad, payload, 4, (const uint64_t *)0,
                    (uint64_t *)0, (uint64_t *)0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_OBJECT_TYPE_MISMATCH,
                   "publish with wrong type_id rejected");

    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                    (uint64_t *)0, (uint64_t *)0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_OBJECT_TYPE_MISMATCH,
                   "publish with NULL type_id on typed state rejected");

    ObDereferenceObject(st);
    knf_delete_state("Kernel", "PubTyped");
}

/* ---- Reserve payload + live-subscriber delete lifetime ------------------ */

static void test_knf_reserve_and_live_delete(void)
{
    KNF_STATE             *st;
    struct knf_subscriber *sub = (struct knf_subscriber *)0;
    uint64_t               prev = 0, cur = 0;
    NTSTATUS               s;
    char                   payload[64] = { 0 };

    st = knf_create_state("Kernel", "PubReserve", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create PubReserve");
    if (!st) return;

    /* Non-notifying pre-size: sequence must NOT advance. */
    s = knf_reserve_payload(st, 256);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "reserve 256 OK");
    TEST_ASSERT_EQ((uint64_t)atomic64_read(&st->sequence), 0ull,
                   "reserve did not advance the sequence");

    /* Bad reserve args. */
    TEST_ASSERT_EQ((uint32_t)knf_reserve_payload(st, 0),
                   (uint32_t)STATUS_INVALID_PARAMETER, "reserve 0 rejected");
    TEST_ASSERT_EQ((uint32_t)knf_reserve_payload(st, KNF_MAX_PAYLOAD + 1),
                   (uint32_t)STATUS_INVALID_PARAMETER, "reserve over cap rejected");

    s = knf_subscribe(st, &sub);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "subscribe OK");

    /* Delete the state from the namespace while a subscription is live. The
     * subscriber pins the body, so create-ref + this pointer stay valid. */
    TEST_ASSERT_EQ(knf_delete_state("Kernel", "PubReserve"), 0,
                   "delete unlinks state while subscriber live");
    TEST_ASSERT_NULL((void *)knf_lookup_state("Kernel", "PubReserve"),
                     "state gone from namespace after delete");

    /* Still safe to publish through the held reference and see it via the pin. */
    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 32, (const uint64_t *)0,
                    (uint64_t *)0, (uint64_t *)0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "publish through pinned state after delete OK");
    s = knf_subscription_poll(sub, &prev, &cur);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS,
                   "subscriber still receives after namespace delete");
    TEST_ASSERT_EQ(cur, 1ull, "post-delete publish delivered new == 1");

    TEST_ASSERT_EQ((uint32_t)knf_unsubscribe(&sub),
                   (uint32_t)STATUS_SUCCESS, "unsubscribe after delete OK");
    ObDereferenceObject(st);   /* last reference -> body freed here */
}

/* ---- Sequence-wrap boundary (no wrap through 0) ------------------------- */

static void test_knf_sequence_no_wrap(void)
{
    KNF_STATE *st;
    NTSTATUS   s;
    char       payload[4] = { 0 };

    st = knf_create_state("Kernel", "PubWrap", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create PubWrap");
    if (!st) return;

    /* Force the change stamp to its maximum; the next publish must be rejected
     * rather than wrapping to 0 (which would alias a fresh state + drop arms). */
    atomic64_set(&st->sequence, (int64_t)0xFFFFFFFFFFFFFFFFull);
    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                    (uint64_t *)0, (uint64_t *)0);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_INVALID_PARAMETER,
                   "publish at max sequence rejected (no wrap through 0)");
    TEST_ASSERT_EQ((uint64_t)atomic64_read(&st->sequence),
                   0xFFFFFFFFFFFFFFFFull,
                   "rejected max-sequence publish left the stamp unchanged");

    ObDereferenceObject(st);
    knf_delete_state("Kernel", "PubWrap");
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
    test_suite_register_cat("knf: publish sequence advance + payload cap",
                            test_knf_publish_sequence, TEST_CAT_KNF);
    test_suite_register_cat("knf: subscribe + poll (prev/new delivery)",
                            test_knf_subscribe_poll, TEST_CAT_KNF);
    test_suite_register_cat("knf: multi-subscriber fanout + coalescing",
                            test_knf_multi_subscriber_coalesce, TEST_CAT_KNF);
    test_suite_register_cat("knf: open-state (resolve existing, no create)",
                            test_knf_open_state, TEST_CAT_KNF);
    test_suite_register_cat("knf: conditional (CAS) publish",
                            test_knf_cas_publish, TEST_CAT_KNF);
    test_suite_register_cat("knf: typed-payload enforcement",
                            test_knf_type_enforcement, TEST_CAT_KNF);
    test_suite_register_cat("knf: reserve payload + live-subscriber delete",
                            test_knf_reserve_and_live_delete, TEST_CAT_KNF);
    test_suite_register_cat("knf: sequence-wrap boundary (no wrap through 0)",
                            test_knf_sequence_no_wrap, TEST_CAT_KNF);
}

#endif /* KERNEL_TESTS */
