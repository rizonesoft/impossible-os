/* ============================================================================
 * knf.c -- Kernel Notification Facility: object type + namespace + create
 *
 * Registers the "NotificationState" Object Manager type, builds the
 * \Notifications namespace tree, and provides the knf_create_state /
 * knf_lookup_state primitives. Publish/subscribe, waitable subscriptions,
 * access policy, coalescing and persistent registry backing land in the
 * later TODO-16 sections; the reserved KNF_STATE fields are inert here.
 * ============================================================================ */

#include "kernel/knf/knf.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/mm/heap.h"
#include "kernel/security/privileges.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"

extern void  *memset(void *s, int c, size_t n);
extern void  *memcpy(void *d, const void *s, size_t n);
extern size_t strlen(const char *s);
extern char  *strncpy(char *dst, const char *src, size_t n);
extern int    strcmp(const char *a, const char *b);
extern int    snprintf(char *buf, size_t size, const char *fmt, ...);

/* A state is published as a namespace component, so its leaf name can never
 * exceed the namespace component limit -- pin the relationship at compile time
 * so a future KNF_NAME_MAX bump cannot silently create names ObInsertObject
 * will reject after allocation. */
_Static_assert(KNF_NAME_MAX <= OB_NAME_MAX,
               "KNF_NAME_MAX must fit a namespace component (OB_NAME_MAX)");

/* The registered type singleton (declared extern in knf.h). */
const OBJECT_TYPE *ObpNotificationStateType = NULL;

/* Category directories provisioned at init (the object-model roots; the
 * built-in-catalog section adds more). */
static const char *const KNF_CATEGORIES[] = {
    "Kernel", "Power", "Security", "Session",
};
#define KNF_CATEGORY_COUNT \
    (sizeof(KNF_CATEGORIES) / sizeof(KNF_CATEGORIES[0]))

/* --- Type callbacks ------------------------------------------------------ */

static void knf_state_on_delete(void *body)
{
    KNF_STATE *st = (KNF_STATE *)body;

    /* Retention payload is a heap pointer once the publish path allocates it;
     * free it here so a state teardown never leaks its last payload. The
     * subscriber list is torn down by the subscription-teardown path before a
     * state can reach refcount 0, so it is NULL here in the object-model layer. */
    if (st->payload) {
        kfree(st->payload);
        st->payload = NULL;
    }
}

void ob_knf_type_init(void)
{
    if (ObpNotificationStateType)
        return;   /* idempotent */

    ObpNotificationStateType = ob_create_type(&(OBJECT_TYPE){
        .name      = "NotificationState",
        .body_size = sizeof(KNF_STATE),
        .on_close  = NULL,
        .on_delete = knf_state_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpNotificationStateType)
        klog(LOG_ERROR, "knf", "Failed to register ObpNotificationStateType");
}

/* --- Namespace helpers --------------------------------------------------- */

/* Create `name` as a child directory of `parent`; returns the (referenced-by-
 * parent) directory body, or NULL. Mirrors ob_ns.c's ns_mkdir. */
static void *knf_mkdir(void *parent, const char *name)
{
    void *dir = ob_ns_create_directory(parent);
    if (!dir) {
        klog(LOG_ERROR, "knf", "mkdir: alloc failed for '%s'", name);
        return NULL;
    }
    if (ObInsertObject(dir, name, parent) < 0) {
        klog(LOG_ERROR, "knf", "mkdir: insert failed for '%s'", name);
        ObDereferenceObject(dir);
        return NULL;
    }
    return dir;
}

/* Resolve \Notifications\<category> to a referenced directory body, or NULL.
 * Caller must ObDereferenceObject the result. */
static void *knf_category_dir(const char *category)
{
    char path[KNF_NAME_MAX + 32];
    void *dir = NULL;

    snprintf(path, sizeof(path), "\\Notifications\\%s", category);
    if (ObLookupObjectByName(path, ObpDirectoryType, 0, &dir) != 0)
        return NULL;
    return dir;
}

/* --- Public API ---------------------------------------------------------- */

KNF_STATE *knf_create_state(const char *category, const char *name,
                            KNF_LIFETIME lifetime, KNF_DATA_SCOPE scope,
                            const KNF_TYPE_ID *type_id, uint32_t access_mode)
{
    KNF_STATE     *st;
    OBJECT_HEADER *hdr;
    void          *dir;
    size_t         nlen;

    if (!ObpNotificationStateType || !category || !name)
        return NULL;

    /* Lifetime must be a defined class (an out-of-range value must not slip
     * past the privilege gate below while still getting OB_FLAG_PERMANENT). */
    if (lifetime < KNF_LIFETIME_WELLKNOWN || lifetime > KNF_LIFETIME_TEMPORARY)
        return NULL;

    nlen = strlen(name);
    if (nlen == 0 || nlen >= KNF_NAME_MAX)
        return NULL;

    /* A leaf name is a single namespace component: reject path separators so
     * the single-component insert and the path-walking lookup can never
     * disagree about which object a name refers to. */
    for (size_t k = 0; k < nlen; k++) {
        if (name[k] == '\\' || name[k] == '/')
            return NULL;
    }

    /* WellKnown / Permanent / Persistent states outlive a normal creator, so a
     * user-mode caller must hold SeCreatePermanentPrivilege (matches WNF: only
     * the system provisions non-temporary state names). Temporary needs none.
     * KernelMode bypasses (SeSinglePrivilegeCheck returns 1 for SE_KERNEL_MODE). */
    if (lifetime != KNF_LIFETIME_TEMPORARY && access_mode != KNF_KERNEL_MODE) {
        if (!SeSinglePrivilegeCheck(&SeCreatePermanentPrivilege, access_mode)) {
            klog(LOG_WARN, "knf",
                 "create '%s\\%s' denied: SeCreatePermanentPrivilege required",
                 category, name);
            return NULL;
        }
    }

    dir = knf_category_dir(category);
    if (!dir) {
        /* Unknown category is a caller argument error, not a kernel fault:
         * WARN (not FAIL) so it never looks like a subsystem error. */
        klog(LOG_WARN, "knf", "create: unknown category '%s'", category);
        return NULL;
    }

    st = (KNF_STATE *)ob_alloc_object(ObpNotificationStateType);
    if (!st) {
        ObDereferenceObject(dir);
        return NULL;
    }

    /* Body is zero-filled by ob_alloc_object: sequence=0, payload=NULL,
     * subscribers=NULL, spinlock flag=0 (its init state) all hold already. */
    strncpy(st->name, name, KNF_NAME_MAX - 1);
    st->name[KNF_NAME_MAX - 1] = '\0';
    atomic64_set(&st->sequence, 0);
    st->lifetime = lifetime;
    st->scope    = scope;
    if (type_id) {
        memcpy(&st->type_id, type_id, sizeof(st->type_id));
        st->has_type_id = 1;
    }

    /* Set the lifetime + category flags BEFORE publishing the object into the
     * namespace. ObInsertObject is the point at which a concurrent
     * knf_delete_state can first see the state; if PERMANENT were set only
     * afterwards, a delete landing in that window would unlink the state and
     * then this path would re-mark it PERMANENT, stranding an unreferenced
     * permanent object (a leak). Setting flags first closes that window; on
     * insert failure ObMakeTemporaryObject clears PERMANENT so the loser frees.
     * Temporary states are not PERMANENT; Security-category states are
     * kernel-only publishers by default (the access-policy section refines
     * per-state DACLs). */
    hdr = OB_HEADER_FROM_BODY(st);
    if (lifetime != KNF_LIFETIME_TEMPORARY)
        hdr->flags |= OB_FLAG_PERMANENT;
    if (strcmp(category, "Security") == 0)
        hdr->flags |= OB_FLAG_KERNEL_ONLY;

    /* Publish under the category directory. The directory takes its own
     * reference on success; on a name collision, open the existing state
     * (WNF create-or-open) so one name maps to one state. */
    if (ObInsertObject(st, st->name, dir) < 0) {
        char wpath[KNF_NAME_MAX + 32];
        void *winner = NULL;

        ObMakeTemporaryObject(st);  /* clear PERMANENT so this loser can free */
        ObDereferenceObject(st);
        ObDereferenceObject(dir);

        snprintf(wpath, sizeof(wpath), "\\Notifications\\%s\\%s",
                 category, name);
        if (ObLookupObjectByName(wpath, ObpNotificationStateType, 0,
                                 &winner) == 0 && winner)
            return (KNF_STATE *)winner;   /* referenced */
        return NULL;
    }

    ObDereferenceObject(dir);
    klog(LOG_INFO, "knf", "state \\Notifications\\%s\\%s (lifetime=%d)",
         category, name, (int)lifetime);
    return st;   /* caller owns the alloc reference */
}

KNF_STATE *knf_lookup_state(const char *category, const char *name)
{
    char  path[KNF_NAME_MAX + 32];
    void *body = NULL;

    if (!category || !name)
        return NULL;

    snprintf(path, sizeof(path), "\\Notifications\\%s\\%s", category, name);
    if (ObLookupObjectByName(path, ObpNotificationStateType, 0, &body) != 0)
        return NULL;
    return (KNF_STATE *)body;
}

int knf_delete_state(const char *category, const char *name)
{
    void      *dir  = NULL;
    KNF_STATE *st   = NULL;

    if (!category || !name)
        return -1;

    dir = knf_category_dir(category);
    if (!dir)
        return -1;

    st = knf_lookup_state(category, name);
    if (!st) {
        ObDereferenceObject(dir);
        return -1;
    }

    /* Clear PERMANENT so the object can be freed once its last reference
     * drops, then unlink it from the category directory (this drops the
     * directory's reference). Our lookup reference keeps the body alive until
     * the final deref below, so the free happens exactly once here. */
    ObMakeTemporaryObject(st);
    ObpRemoveFromDirectory(dir, st);

    ObDereferenceObject(st);    /* releases our lookup ref -> frees the state */
    ObDereferenceObject(dir);
    return 0;
}

/* --- Init ---------------------------------------------------------------- */

void knf_init(void)
{
    void *notifications;
    uint32_t i, created = 0;

    POST16(POST16_KNF);

    ob_knf_type_init();

    if (!ObpRootDirectory) {
        klog(LOG_ERROR, "knf", "root namespace not initialised");
        POST16(POST16_KNF_OK);
        return;
    }

    notifications = knf_mkdir(ObpRootDirectory, "Notifications");
    if (!notifications) {
        klog(LOG_ERROR, "knf", "failed to create \\Notifications");
        POST16(POST16_KNF_OK);
        return;
    }

    for (i = 0; i < KNF_CATEGORY_COUNT; i++) {
        if (knf_mkdir(notifications, KNF_CATEGORIES[i]))
            created++;
    }

    klog(LOG_INFO, "knf",
         "Namespace: \\Notifications + %u categories (Kernel/Power/Security/Session)",
         created);

    POST16(POST16_KNF_OK);
}
