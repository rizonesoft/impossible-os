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
#include "kernel/test/scratch.h"  /* TEST_SCRATCH_KBUF: subscriber-cap array */

extern void *memset(void *dst, int c, size_t n);  /* freestanding: no <string.h> */
#include "kernel/knf/knf.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/security/privileges.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/knf_syscall_info.h"
#include "kernel/quota/quota.h"         /* attribution queries */
#include "kernel/quota/quota_ledger.h"  /* the charge gate, to force STATUS_RETRY */
#include "kernel/sched/task.h"
#include "kernel/boot_init.h"   /* SUBSYS_KNF + kernel_subsystem_ready/_set_ready */

extern int   strcmp(const char *a, const char *b);
extern void *memset(void *s, int c, size_t n);
extern int   snprintf(char *buf, size_t size, const char *fmt, ...);
extern NTSTATUS nt_query_notification_information(void *buffer, uint32_t buf_size,
                                                 uint32_t *return_length);

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

/* ---- Poll argument validation ------------------------------------------- */

static void test_knf_poll_invalid(void)
{
    struct knf_subscriber node;
    uint64_t              p = 0, n = 0;

    TEST_ASSERT_EQ((uint32_t)knf_subscription_poll((struct knf_subscriber *)0,
                                                   &p, &n),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "poll of NULL subscriber -> INVALID_PARAMETER");

    memset(&node, 0, sizeof(node));   /* node.state == NULL */
    TEST_ASSERT_EQ((uint32_t)knf_subscription_poll(&node, &p, &n),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "poll of subscriber with NULL state -> INVALID_PARAMETER");
}

/* ---- Subscriber cap (bounds the under-lock fanout walk) ----------------- */

/* The cap array is one pointer per subscriber slot (~32 KB). It was a
 * file-scope static only to stay under the BSS-collision gate; it is a
 * per-case TEST_SCRATCH_KBUF allocation now, so the bytes leave the kernel
 * image. It MUST be zeroed: the allocator does not, and the unsubscribe loop
 * below walks only the slots that subscribed successfully, so a stale
 * non-NULL slot would otherwise be indistinguishable from a live handle. The
 * array is freed by the action drain AFTER this function returns, which is
 * after the unsubscribe loop has released every handle it owns. */
#define KNF_CAP_SUBS_BYTES \
    ((uint64_t)sizeof(struct knf_subscriber *) * (uint64_t)KNF_MAX_SUBSCRIBERS_PER_STATE)

static void test_knf_subscriber_cap(void)
{
    KNF_STATE             *st;
    struct knf_subscriber *extra = (struct knf_subscriber *)0;
    NTSTATUS               s;
    uint32_t               i, n = 0;
    TEST_SCRATCH_KBUF(subsbuf, KNF_CAP_SUBS_BYTES);
    struct knf_subscriber **const g_cap_subs = (struct knf_subscriber **)subsbuf;

    memset(g_cap_subs, 0, KNF_CAP_SUBS_BYTES);

    st = knf_create_state("Kernel", "PubCap", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create PubCap");
    if (!st) return;

    for (i = 0; i < KNF_MAX_SUBSCRIBERS_PER_STATE; i++) {
        if (knf_subscribe(st, &g_cap_subs[i]) != STATUS_SUCCESS)
            break;
        n++;
    }
    TEST_ASSERT_EQ((uint64_t)n, (uint64_t)KNF_MAX_SUBSCRIBERS_PER_STATE,
                   "all subscribers up to the cap accepted");
    TEST_ASSERT_EQ((uint64_t)st->subscriber_count,
                   (uint64_t)KNF_MAX_SUBSCRIBERS_PER_STATE,
                   "subscriber_count sits at the cap");

    s = knf_subscribe(st, &extra);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_INSUFFICIENT_RESOURCES,
                   "subscribe past the cap rejected");
    TEST_ASSERT_NULL((void *)extra, "over-cap subscribe left the handle NULL");

    /* A failing TEST_ASSERT records and continues, so a fill loop that stopped
     * short leaves the probe above BELOW the cap, where it SUCCEEDS and hands
     * back a live handle. Release it before the array or its quota charge and
     * its state reference leak: knf_delete_state only unlinks the state and
     * cannot drop a subscriber reference it does not own. */
    if (extra)
        knf_unsubscribe(&extra);

    for (i = 0; i < n; i++)
        knf_unsubscribe(&g_cap_subs[i]);
    TEST_ASSERT_EQ((uint64_t)st->subscriber_count, 0ull,
                   "subscriber_count back to 0 after unsubscribe all");

    ObDereferenceObject(st);
    knf_delete_state("Kernel", "PubCap");
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

/* ---- ETW/klog trace bridge ---------------------------------------------- */

static void test_knf_trace_flags(void)
{
    KNF_STATE *st;
    uint64_t   prev = 0, cur = 0, drops, skips;
    NTSTATUS   s;
    char       payload[4] = { 0 };

    st = knf_create_state("Kernel", "PubTrace", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                          KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create PubTrace");
    if (!st) return;

    /* Set the opt-ins; they are stored verbatim under the lock. */
    s = knf_set_trace_flags(st, KNF_TRACE_KLOG | KNF_TRACE_ETW);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "set trace flags OK");
    TEST_ASSERT_EQ((uint64_t)st->trace_flags,
                   (uint64_t)(KNF_TRACE_KLOG | KNF_TRACE_ETW),
                   "trace_flags stored");

    /* Unknown bits are rejected and leave the flags unchanged. */
    s = knf_set_trace_flags(st, 0x100u);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_INVALID_PARAMETER,
                   "unknown trace bits rejected");
    TEST_ASSERT_EQ((uint64_t)st->trace_flags,
                   (uint64_t)(KNF_TRACE_KLOG | KNF_TRACE_ETW),
                   "rejected set left trace_flags unchanged");
    TEST_ASSERT_EQ((uint32_t)knf_set_trace_flags((KNF_STATE *)0, KNF_TRACE_KLOG),
                   (uint32_t)STATUS_INVALID_PARAMETER, "NULL state rejected");

    /* Publish with the bridge enabled runs klog + ETW off the lock and still
     * advances the sequence normally (bridge is side-band, never blocks it).
     * At PASSIVE_LEVEL (the test context) the bridge emits and must NOT record
     * a DISPATCH-level drop -- the drop counter only ticks when tracing is
     * skipped at >= DISPATCH_LEVEL. */
    drops = knf_trace_drops_at_dispatch_count();
    skips = knf_trace_skips_guard_count();
    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                    &prev, &cur);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "traced publish OK");
    TEST_ASSERT_EQ(cur, 1ull, "traced publish advanced the sequence");
    TEST_ASSERT_EQ(knf_trace_drops_at_dispatch_count(), drops,
                   "PASSIVE traced publish did not count as a DISPATCH drop");
    TEST_ASSERT_EQ(knf_trace_skips_guard_count(), skips,
                   "un-nested traced publish did not count as a guard skip");

    /* Clearing the flags disables the bridge (publish still succeeds). */
    TEST_ASSERT_EQ((uint32_t)knf_set_trace_flags(st, 0),
                   (uint32_t)STATUS_SUCCESS, "clear trace flags OK");
    s = knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                    &prev, &cur);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "untraced publish OK");
    TEST_ASSERT_EQ(cur, 2ull, "untraced publish advanced the sequence");

    ObDereferenceObject(st);
    knf_delete_state("Kernel", "PubTrace");
}

/* KNF leaf names are only unique within a category directory, so the trace
 * record's identity is category+leaf. Prove two same-leaf states in different
 * categories carry distinct categories (the field the ETW/klog record emits),
 * so records for Kernel\Dup and Power\Dup are disambiguable. */
static void test_knf_trace_category_identity(void)
{
    KNF_STATE *k, *p;

    k = knf_create_state("Kernel", "Dup", KNF_LIFETIME_TEMPORARY,
                         KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE);
    p = knf_create_state("Power", "Dup", KNF_LIFETIME_TEMPORARY,
                         KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(k, "create Kernel\\Dup");
    TEST_ASSERT_NOT_NULL(p, "create Power\\Dup");
    if (!k || !p) goto out;

    /* Same leaf, different objects, different category identity. */
    TEST_ASSERT(k != p, "same leaf in two categories are distinct states");
    TEST_ASSERT_EQ((uint32_t)(strcmp(k->name, p->name) == 0), 1u,
                   "leaf names match");
    TEST_ASSERT_EQ((uint32_t)(strcmp(k->category, "Kernel") == 0), 1u,
                   "Kernel\\Dup category stored");
    TEST_ASSERT_EQ((uint32_t)(strcmp(p->category, "Power") == 0), 1u,
                   "Power\\Dup category stored");
    TEST_ASSERT_EQ((uint32_t)(strcmp(k->category, p->category) != 0), 1u,
                   "categories disambiguate the two records");
out:
    if (k) { ObDereferenceObject(k); knf_delete_state("Kernel", "Dup"); }
    if (p) { ObDereferenceObject(p); knf_delete_state("Power", "Dup"); }
}

/* Coalescing/retention layer: level-triggered coalescing missed-update counter,
 * kernel-private retention query (query-after-miss), + secret-mode policy flag. */
static void test_knf_coalesce_retention(void)
{
    KNF_STATE             *st;
    struct knf_subscriber *sub = (struct knf_subscriber *)0;
    uint64_t               prev = 0, cur = 0, seq = 0;
    uint32_t               qlen = 0;
    NTSTATUS               s;
    int                    i;
    char                   payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    char                   qbuf[8]    = { 0 };

    st = knf_create_state("Kernel", "PubCoal", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create PubCoal");
    if (!st) return;

    TEST_ASSERT_EQ((uint32_t)knf_subscribe(st, &sub),
                   (uint32_t)STATUS_SUCCESS, "subscribe");

    /* 5 rapid publishes; the slow subscriber sees only the latest and reports 4
     * coalesced (missed) updates -- the first arms the pending, the next 4
     * coalesce over it. */
    for (i = 0; i < 5; i++)
        knf_publish(st, (const KNF_TYPE_ID *)0, payload, 8, (const uint64_t *)0,
                    (uint64_t *)0, (uint64_t *)0);   /* seq 1..5 */
    TEST_ASSERT_EQ(knf_subscription_missed_count(sub), 4ull,
                   "5 rapid publishes coalesced 4 missed updates");
    TEST_ASSERT_EQ(knf_subscription_missed_count(sub), 0ull,
                   "missed counter read-and-reset");

    s = knf_subscription_poll(sub, &prev, &cur);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "poll delivered latest");
    TEST_ASSERT_EQ(cur, 5ull, "poll delivered the latest sequence");

    /* Retention: query-after-miss returns the retained last payload + stamp. */
    s = knf_query_last_kernel(st, qbuf, sizeof(qbuf), &qlen, &seq);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "retention query OK");
    TEST_ASSERT_EQ((uint64_t)qlen, 8ull, "retained payload length");
    TEST_ASSERT_EQ(seq, 5ull, "retention query current stamp");
    TEST_ASSERT_EQ((uint32_t)(qbuf[7] == 8), 1u, "retained payload bytes intact");

    /* Buffer-too-small: partial copy + full length reported (WNF query shape). */
    qlen = 0;
    s = knf_query_last_kernel(st, qbuf, 4, &qlen, &seq);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_BUFFER_TOO_SMALL,
                   "retention query reports too-small buffer");
    TEST_ASSERT_EQ((uint64_t)qlen, 8ull, "too-small query still reports full length");

    /* NULL output buffer is a length-only query: a non-empty payload must NOT
     * report false success -- it returns BUFFER_TOO_SMALL with the full length. */
    qlen = 0;
    s = knf_query_last_kernel(st, (void *)0, 64, &qlen, &seq);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_BUFFER_TOO_SMALL,
                   "NULL buffer with non-empty payload is not false success");
    TEST_ASSERT_EQ((uint64_t)qlen, 8ull, "NULL-buffer length query reports full length");

    /* Mode flags: secret opt-in stored; unknown bits + NULL rejected. */
    TEST_ASSERT_EQ((uint32_t)knf_set_mode(st, KNF_MODE_SECRET),
                   (uint32_t)STATUS_SUCCESS, "set secret mode");
    TEST_ASSERT_EQ((uint64_t)st->mode_flags, (uint64_t)KNF_MODE_SECRET,
                   "mode flags stored");
    TEST_ASSERT_EQ((uint32_t)knf_set_mode(st, 0x80u),
                   (uint32_t)STATUS_INVALID_PARAMETER, "unknown mode bits rejected");
    TEST_ASSERT_EQ((uint32_t)knf_set_mode((KNF_STATE *)0, KNF_MODE_SECRET),
                   (uint32_t)STATUS_INVALID_PARAMETER, "NULL state rejected");

    knf_unsubscribe(&sub);
    ObDereferenceObject(st);
    knf_delete_state("Kernel", "PubCoal");
}

/* Diagnostics browser: NtQuerySystemInformation(SystemNotificationInformation)
 * reports live/cumulative KNF counters + init readiness. Asserts deltas after
 * known operations (create, subscribe, publish burst, denied user-mode permanent
 * create) so the test does not depend on other suites having run first. */
static void test_knf_diag_query(void)
{
    SYSTEM_NOTIFICATION_INFORMATION a, b;
    uint32_t                        rl = 0;
    NTSTATUS                        s;
    KNF_STATE                      *st;
    struct knf_subscriber          *sub = (struct knf_subscriber *)0;
    uint64_t                        prev = 0, cur = 0;
    int                             i;
    char                            payload[4] = { 0 };

    /* Baseline snapshot: ABI header fields + readiness flag. */
    s = nt_query_notification_information(&a, sizeof(a), &rl);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "diag query OK");
    TEST_ASSERT_EQ((uint64_t)rl,
                   (uint64_t)sizeof(SYSTEM_NOTIFICATION_INFORMATION),
                   "diag return length == struct size");
    TEST_ASSERT_EQ((uint64_t)a.Version,
                   (uint64_t)SYSTEM_NOTIFICATION_INFORMATION_VERSION, "diag version");
    TEST_ASSERT_EQ((uint64_t)a.Size, 96ull, "diag Size field == 96");
    TEST_ASSERT((a.Flags & SYSTEM_NOTIFICATION_FLAG_READY) != 0,
                "KNF reports ready (namespace + type initialized)");

    /* Known operations: +1 live state, +1 subscriber, +3 publishes (+2 coalesced
     * over the unread pending), +1 security denial (user-mode permanent create). */
    st = knf_create_state("Kernel", "PubDiag", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL(st, "create PubDiag");
    if (!st) return;
    knf_subscribe(st, &sub);
    for (i = 0; i < 3; i++)
        knf_publish(st, (const KNF_TYPE_ID *)0, payload, 4, (const uint64_t *)0,
                    &prev, &cur);
    (void)knf_create_state("Kernel", "PubDiagPerm", KNF_LIFETIME_PERMANENT,
                           KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0, KNF_USER_MODE);

    s = nt_query_notification_information(&b, sizeof(b), &rl);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "diag re-query OK");
    TEST_ASSERT(b.LiveStateCount  >= a.LiveStateCount  + 1, "live state count rose");
    TEST_ASSERT(b.SubscriberCount >= a.SubscriberCount + 1, "subscriber count rose");
    TEST_ASSERT(b.PublishCount    >= a.PublishCount    + 3, "publish count rose by >=3");
    TEST_ASSERT(b.CoalescedCount  >= a.CoalescedCount  + 2, "coalesced count rose by >=2");
    TEST_ASSERT(b.SecurityDenials >= a.SecurityDenials + 1, "security denial counted");

    /* Length-only / too-small buffer: full size reported, INFO_LENGTH_MISMATCH. */
    rl = 0;
    s = nt_query_notification_information(&b, 8, &rl);
    TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_INFO_LENGTH_MISMATCH,
                   "too-small diag buffer rejected");
    TEST_ASSERT_EQ((uint64_t)rl, 96ull, "too-small diag query reports full length");

    /* Readiness mapping: force KNF not-ready and confirm the marshaller reports
     * UNAVAILABLE (fatal) distinctly from READY, then restore the oracle slot so
     * no other suite observes a spurious KNF-down state. */
    {
        bool saved = kernel_subsystem_ready(SUBSYS_KNF);
        kernel_subsystem_set_ready(SUBSYS_KNF, false);
        s = nt_query_notification_information(&b, sizeof(b), &rl);
        TEST_ASSERT_EQ((uint32_t)s, (uint32_t)STATUS_SUCCESS, "diag query (not ready)");
        TEST_ASSERT((b.Flags & SYSTEM_NOTIFICATION_FLAG_UNAVAILABLE) != 0,
                    "not-ready KNF reports UNAVAILABLE");
        TEST_ASSERT((b.Flags & SYSTEM_NOTIFICATION_FLAG_READY) == 0,
                    "not-ready KNF does not report READY");
        kernel_subsystem_set_ready(SUBSYS_KNF, saved);
    }

    knf_unsubscribe(&sub);
    ObDereferenceObject(st);
    knf_delete_state("Kernel", "PubDiag");
}

/* Stress leak-check: 400 states + 100 subscribers, torn fully down, asserting
 * the KNF diagnostics live-state + subscriber counters return to baseline (no
 * leak). A single OB directory caps at OB_DIR_MAX_ENTRIES (128), so the load is
 * spread across the 4 category directories (PER_CAT each). States beyond the
 * first NSUBS drop their create reference immediately (kept alive by the
 * namespace reference, deleted by name); the first NSUBS keep the pointer to
 * subscribe and to drop the create ref at teardown. The exact-count assertions
 * make a silent directory-cap hit a failure, not reduced-coverage pass. Also
 * exercises the idempotent delete path (a second delete is a no-op). */
static void test_knf_stress_no_leak(void)
{
    static const char *const cats[] = { "Kernel", "Power", "Session", "Security" };
    enum { NCATS = 4, PER_CAT = 100, NSTATES = NCATS * PER_CAT, NSUBS = 100 };
    KNF_STATE             *keep[NSUBS] = { 0 };   /* first NSUBS states, kept for teardown */
    struct knf_subscriber *sub[NSUBS]  = { 0 };
    uint64_t               base_states, base_subs;
    char                   name[24];
    int                    c, i, idx, created = 0, subbed = 0;

    base_states = knf_diag_live_state_count();
    base_subs   = knf_diag_subscriber_count();

    for (c = 0; c < NCATS; c++) {
        for (i = 0; i < PER_CAT; i++) {
            KNF_STATE *st;
            idx = c * PER_CAT + i;
            snprintf(name, sizeof(name), "Stress%d", i);
            st = knf_create_state(cats[c], name, KNF_LIFETIME_TEMPORARY,
                                  KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE);
            if (!st)
                continue;
            created++;
            if (idx < NSUBS) {
                keep[idx] = st;
                if (knf_subscribe(st, &sub[idx]) == STATUS_SUCCESS)
                    subbed++;
            } else {
                ObDereferenceObject(st);   /* keep only namespace ref; delete by name later */
            }
        }
    }
    /* Exact cardinality: a silent OB-directory-cap hit (create returning NULL)
     * must FAIL the test rather than pass with reduced coverage. */
    TEST_ASSERT_EQ((uint64_t)created, (uint64_t)NSTATES, "stress created all NSTATES");
    TEST_ASSERT_EQ((uint64_t)subbed,  (uint64_t)NSUBS,   "stress subscribed all NSUBS");
    TEST_ASSERT_EQ(knf_diag_live_state_count(), base_states + (uint64_t)NSTATES,
                   "stress: live-state counter rose by NSTATES");
    TEST_ASSERT_EQ(knf_diag_subscriber_count(), base_subs + (uint64_t)NSUBS,
                   "stress: subscriber counter rose by NSUBS");

    /* Teardown everything; the counters must return to baseline (no leak). */
    for (c = 0; c < NCATS; c++) {
        for (i = 0; i < PER_CAT; i++) {
            idx = c * PER_CAT + i;
            snprintf(name, sizeof(name), "Stress%d", i);
            if (idx < NSUBS) {
                if (sub[idx])
                    knf_unsubscribe(&sub[idx]);
                if (keep[idx])
                    ObDereferenceObject(keep[idx]);
            }
            knf_delete_state(cats[c], name);
        }
    }
    /* Idempotent delete: a second delete of an already-removed name is a no-op. */
    knf_delete_state("Kernel", "Stress0");

    TEST_ASSERT_EQ(knf_diag_live_state_count(), base_states,
                   "stress: no leaked states after full teardown");
    TEST_ASSERT_EQ(knf_diag_subscriber_count(), base_subs,
                   "stress: no leaked subscribers after full teardown");
}

/* ---- Registration ------------------------------------------------------- */

/* ==========================================================================
 * Section 16 (TODO-25): status-bearing creation
 * ========================================================================== */

/* knf_create_state_ex names WHY a creation failed, which knf_create_state
 * structurally cannot. The distinction that matters most is invisible in this
 * test and is the reason the function exists: a transient STATUS_RETRY from the
 * charge gate no longer arrives as NULL, indistinguishable from a permanent
 * quota refusal. What IS testable here is that every other outcome now has its
 * own status and that the pointer form still behaves exactly as before. */
static void test_knf_create_ex_reports_distinct_statuses(void)
{
    KNF_STATE *st = (KNF_STATE *)0;
    char longname[KNF_NAME_MAX + 8];
    uint32_t i;

    /* Success sets BOTH the status and the out-param. */
    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex("Kernel", "UtestExState",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE, &st),
                   (uint64_t)STATUS_SUCCESS, "a valid creation reports success");
    TEST_ASSERT_NOT_NULL((void *)st, "success yields a referenced state");
    ObDereferenceObject(st);
    TEST_ASSERT_EQ(knf_delete_state("Kernel", "UtestExState"), 0,
                   "the state created through _ex is a normal state");

    /* A bad argument is INVALID_PARAMETER and, critically, EMPTIES the out-param
     * so a caller may branch on the status alone. */
    st = (KNF_STATE *)0xdead;
    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex((const char *)0, "X",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE, &st),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL category is a bad argument");
    TEST_ASSERT_NULL((void *)st, "a failure clears the out-param it was handed");

    for (i = 0; i < sizeof(longname) - 1; i++)
        longname[i] = 'a';
    longname[sizeof(longname) - 1] = '\0';
    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex("Kernel", longname,
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE, &st),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "an over-length leaf name is a bad argument");

    /* An unknown category is NOT a bad argument: the arguments are well-formed
     * and the path does not exist. Collapsing the two would send a caller
     * hunting for a mistake in its own parameters. */
    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex("NoSuchCategory", "X",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE, &st),
                   (uint64_t)STATUS_OBJECT_PATH_NOT_FOUND,
                   "an unknown category is a path failure, not a bad argument");

    /* An out-of-range lifetime must be refused rather than reach the privilege
     * gate while still qualifying for OB_FLAG_PERMANENT. */
    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex("Kernel", "UtestExBadLife",
                        (KNF_LIFETIME)(KNF_LIFETIME_TEMPORARY + 1),
                        KNF_SCOPE_SYSTEM, (const KNF_TYPE_ID *)0,
                        KNF_KERNEL_MODE, &st),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "an undefined lifetime class is refused");

    /* A NULL out-param is itself a bad argument, not a crash. */
    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex("Kernel", "X",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE,
                        (KNF_STATE **)0),
                   (uint64_t)STATUS_INVALID_PARAMETER, "a NULL out-param is refused");
}

/* create-or-open through the status-bearing form: losing the insert race and
 * adopting the existing winner is a SUCCESS, because one name means one state.
 * Reporting a collision there would make every second caller handle a failure
 * that is really the documented behavior. */
static void test_knf_create_ex_create_or_open_is_success(void)
{
    KNF_STATE *a = (KNF_STATE *)0, *b = (KNF_STATE *)0;

    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex("Power", "UtestExDup",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE, &a),
                   (uint64_t)STATUS_SUCCESS, "first creation succeeds");
    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex("Power", "UtestExDup",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE, &b),
                   (uint64_t)STATUS_SUCCESS, "a second creation OPENS rather than fails");
    TEST_ASSERT_NOT_NULL((void *)a, "the first call yields a state");
    TEST_ASSERT(a == b, "one name resolves to exactly one state body");

    if (b)
        ObDereferenceObject(b);
    if (a)
        ObDereferenceObject(a);
    knf_delete_state("Power", "UtestExDup");
}

/* THE defining assertion for knf_create_state_ex, and the reason section 16 filed
 * the item: a transient charge-gate closure must arrive as STATUS_RETRY, NOT as
 * the NULL that a one-shot caller reads as permanent failure.
 *
 * Closing the gate with quota_gate_quiesce reproduces exactly the production
 * condition -- a job-membership transition holding this task's charge gate shut
 * -- without any fault injection, because the charge path treats both
 * identically. */
static void test_knf_create_ex_propagates_transient_retry(void)
{
    struct task *t = task_current();
    KNF_STATE   *st = (KNF_STATE *)0;
    int64_t      notify_before;

    if (!t || !t->quota)
        return;                  /* no principal to gate; covered elsewhere */

    notify_before = quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                       QUOTA_RES_NOTIFICATION_STATE);

    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_SUCCESS,
                   "the gate quiesces for the test");

    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex("Kernel", "UtestExRetry",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE, &st),
                   (uint64_t)STATUS_RETRY,
                   "a closed charge gate surfaces as STATUS_RETRY, not a refusal");
    TEST_ASSERT_NULL((void *)st, "a refused creation yields no state");
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                                QUOTA_RES_NOTIFICATION_STATE)
                              - notify_before),
                   0ull, "a refused creation leaves no attributed charge behind");
    /* And no state was published under that name -- the failure path unwound. */
    TEST_ASSERT_NULL((void *)knf_lookup_state("Kernel", "UtestExRetry"),
                     "a refused creation publishes nothing into the namespace");

    /* Reopening makes the SAME call succeed, which is what proves the status was
     * transient rather than a permanent condition wearing a retryable name. */
    quota_gate_reopen(t);
    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex("Kernel", "UtestExRetry",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE, &st),
                   (uint64_t)STATUS_SUCCESS,
                   "the retry succeeds once the gate reopens");
    TEST_ASSERT_NOT_NULL((void *)st, "the retry yields a state");
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                                QUOTA_RES_NOTIFICATION_STATE)
                              - notify_before),
                   1ull, "the successful creation charges NOTIFY exactly once");

    ObDereferenceObject(st);
    TEST_ASSERT_EQ(knf_delete_state("Kernel", "UtestExRetry"), 0,
                   "the state is removed again");
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                                QUOTA_RES_NOTIFICATION_STATE)
                              - notify_before),
                   0ull, "deleting the state credits the NOTIFY attribution back");

    /* The pointer-returning wrapper is where the distinction is LOST, and that is
     * by design -- assert it so nobody mistakes the wrapper for the fixed path. */
    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_SUCCESS,
                   "the gate quiesces again");
    TEST_ASSERT_NULL((void *)knf_create_state("Kernel", "UtestExRetry2",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE),
                     "the pointer form still collapses a transient refusal to NULL");
    quota_gate_reopen(t);
}

/* A privilege refusal must be its OWN status, distinct from every transient one:
 * a caller that retried it would loop forever. KNF_USER_MODE with a non-temporary
 * lifetime is the documented gate. */
static void test_knf_create_ex_privilege_refusal_is_distinct(void)
{
    KNF_STATE *st = (KNF_STATE *)0;
    NTSTATUS   status;

    status = knf_create_state_ex("Kernel", "UtestExPriv",
                                 KNF_LIFETIME_PERMANENT, KNF_SCOPE_SYSTEM,
                                 (const KNF_TYPE_ID *)0, KNF_USER_MODE, &st);

    /* A test kernel thread may legitimately hold SeCreatePermanentPrivilege, so
     * the assertion is on the DISTINCTION rather than on one outcome: either the
     * privilege check refused it with its own status, or it succeeded. What must
     * never happen is a transient status for a privilege decision, because that
     * is what a retry loop would spin on. */
    if (status == STATUS_SUCCESS) {
        TEST_ASSERT_NOT_NULL((void *)st, "a permitted creation yields a state");
        ObDereferenceObject(st);
        knf_delete_state("Kernel", "UtestExPriv");
    } else {
        TEST_ASSERT_EQ((uint64_t)status, (uint64_t)STATUS_PRIVILEGE_NOT_HELD,
                       "a privilege refusal reports PRIVILEGE_NOT_HELD");
        TEST_ASSERT_NULL((void *)st, "a refused creation yields no state");
    }
    TEST_ASSERT(status != STATUS_RETRY && status != STATUS_INSUFFICIENT_RESOURCES,
                "a privilege decision is never reported as transient");
}

/* The PRODUCTION tags, on the real consumers rather than through a test helper.
 * The attribution helpers are covered in test_quota.c; what is unproven without
 * this is the WIRING -- mis-tagging or reverting a real charge site would leave
 * those helper tests green while the operator dashboard blamed the wrong
 * subsystem. Asserts every one of KNF's three charge sites. */
static void test_knf_production_charges_are_tagged_notify(void)
{
    KNF_STATE             *st = (KNF_STATE *)0;
    struct knf_subscriber *sub = (struct knf_subscriber *)0;
    int64_t                state_before, bytes_before, sub_before;

    state_before = quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                      QUOTA_RES_NOTIFICATION_STATE);
    bytes_before = quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                      QUOTA_RES_NOTIFICATION_BYTES);
    sub_before   = quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                      QUOTA_RES_NOTIFICATION_SUB);

    TEST_ASSERT_EQ((uint64_t)knf_create_state_ex("Kernel", "UtestTagged",
                        KNF_LIFETIME_TEMPORARY, KNF_SCOPE_SYSTEM,
                        (const KNF_TYPE_ID *)0, KNF_KERNEL_MODE, &st),
                   (uint64_t)STATUS_SUCCESS, "the tagged state is created");
    if (!st)
        return;

    /* BOTH of creation's charges carry the tag, not just the state count: the
     * retention budget is the larger of the two and the one an operator chasing
     * notification memory would look for. */
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                                QUOTA_RES_NOTIFICATION_STATE)
                              - state_before),
                   1ull, "state creation is attributed to NOTIFY");
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                                QUOTA_RES_NOTIFICATION_BYTES)
                              - bytes_before),
                   (uint64_t)KNF_MAX_PAYLOAD,
                   "and so is its retention budget, by the exact payload ceiling");

    /* The subscription charge is a THIRD site with its own resource type. */
    TEST_ASSERT_EQ((uint64_t)knf_subscribe(st, &sub), (uint64_t)STATUS_SUCCESS,
                   "subscribing succeeds");
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                                QUOTA_RES_NOTIFICATION_SUB)
                              - sub_before),
                   1ull, "a subscription is attributed to NOTIFY");

    TEST_ASSERT_EQ((uint64_t)knf_unsubscribe(&sub), (uint64_t)STATUS_SUCCESS,
                   "unsubscribing succeeds");
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                                QUOTA_RES_NOTIFICATION_SUB)
                              - sub_before),
                   0ull, "and credits the subscription back to the same tag");

    ObDereferenceObject(st);
    TEST_ASSERT_EQ(knf_delete_state("Kernel", "UtestTagged"), 0,
                   "the state is deleted");
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                                QUOTA_RES_NOTIFICATION_STATE)
                              - state_before),
                   0ull, "deletion credits the state count back");
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_NOTIFY,
                                                QUOTA_RES_NOTIFICATION_BYTES)
                              - bytes_before),
                   0ull, "and the retention budget too, leaving no residue");
}

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
    test_suite_register_cat("knf: poll argument validation",
                            test_knf_poll_invalid, TEST_CAT_KNF);
    test_suite_register_cat("knf: subscriber cap bounds the fanout walk",
                            test_knf_subscriber_cap, TEST_CAT_KNF);
    test_suite_register_cat("knf: conditional (CAS) publish",
                            test_knf_cas_publish, TEST_CAT_KNF);
    test_suite_register_cat("knf: typed-payload enforcement",
                            test_knf_type_enforcement, TEST_CAT_KNF);
    test_suite_register_cat("knf: reserve payload + live-subscriber delete",
                            test_knf_reserve_and_live_delete, TEST_CAT_KNF);
    test_suite_register_cat("knf: sequence-wrap boundary (no wrap through 0)",
                            test_knf_sequence_no_wrap, TEST_CAT_KNF);
    test_suite_register_cat("knf: ETW/klog trace bridge flags",
                            test_knf_trace_flags, TEST_CAT_KNF);
    test_suite_register_cat("knf: trace record category+leaf identity",
                            test_knf_trace_category_identity, TEST_CAT_KNF);
    test_suite_register_cat("knf: coalescing missed counter + retention query",
                            test_knf_coalesce_retention, TEST_CAT_KNF);
    test_suite_register_cat("knf: SystemNotificationInformation diagnostics query",
                            test_knf_diag_query, TEST_CAT_KNF);
    test_suite_register_cat("knf: stress 400 states / 100 subscribers, no leak",
                            test_knf_stress_no_leak, TEST_CAT_KNF);

    /* Status-bearing creation (quota charge-attribution work) */
    test_suite_register_cat("knf: create_ex reports distinct statuses",
                            test_knf_create_ex_reports_distinct_statuses, TEST_CAT_KNF);
    test_suite_register_cat("knf: create_ex create-or-open is success",
                            test_knf_create_ex_create_or_open_is_success, TEST_CAT_KNF);
    test_suite_register_cat("knf: create_ex propagates transient RETRY",
                            test_knf_create_ex_propagates_transient_retry, TEST_CAT_KNF);
    test_suite_register_cat("knf: create_ex privilege refusal is distinct",
                            test_knf_create_ex_privilege_refusal_is_distinct, TEST_CAT_KNF);
    test_suite_register_cat("knf: production charges tagged NOTIFY",
                            test_knf_production_charges_are_tagged_notify, TEST_CAT_KNF);
}

#endif /* KERNEL_TESTS */
