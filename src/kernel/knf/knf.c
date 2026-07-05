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
#include "kernel/sched/irql.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"

extern void  *memset(void *s, int c, size_t n);
extern void  *memcpy(void *d, const void *s, size_t n);
extern int    memcmp(const void *a, const void *b, size_t n);
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

/* Buffer for a full "\Notifications\<category>\<name>" path. Both category and
 * name are validated components (each < KNF_NAME_MAX), so the worst case is the
 * 15-char "\Notifications\" prefix + two max components + a separator + NUL.
 * Sized off KNF_NAME_MAX so a name cap bump keeps the buffers in sync. */
#define KNF_PATH_MAX (2 * KNF_NAME_MAX + 24)
_Static_assert(KNF_PATH_MAX >= 15 + 2 * (KNF_NAME_MAX - 1) + 2,
               "KNF_PATH_MAX must hold \\Notifications\\<cat>\\<name>");

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

/* A namespace component (category or leaf name) must be non-empty, fit a
 * directory component, and carry NO path separator -- otherwise a value like
 * "Security\\" or "" would let the single-component insert and the
 * path-walking lookup disagree, aliasing the wrong directory/object. */
static int knf_valid_component(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= KNF_NAME_MAX)
        return 0;
    for (size_t k = 0; k < n; k++) {
        if (s[k] == '\\' || s[k] == '/')
            return 0;
    }
    return 1;
}

/* Resolve \Notifications\<category> to a referenced directory body, or NULL.
 * Caller must ObDereferenceObject the result. */
static void *knf_category_dir(const char *category)
{
    char path[KNF_PATH_MAX];
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

    if (!ObpNotificationStateType || !category || !name)
        return NULL;

    /* Lifetime must be a defined class (an out-of-range value must not slip
     * past the privilege gate below while still getting OB_FLAG_PERMANENT). */
    if (lifetime < KNF_LIFETIME_WELLKNOWN || lifetime > KNF_LIFETIME_TEMPORARY)
        return NULL;

    /* Both the category and the leaf name are single namespace components:
     * validate each (non-empty, bounded, no separator) so no crafted value
     * aliases the wrong directory or object. */
    if (!knf_valid_component(category) || !knf_valid_component(name))
        return NULL;

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
        char wpath[KNF_PATH_MAX];
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
    char  path[KNF_PATH_MAX];
    void *body = NULL;

    if (!category || !name ||
        !knf_valid_component(category) || !knf_valid_component(name))
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
    int        removed;

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
    removed = ObpRemoveFromDirectory(dir, st);

    /* removed == -1 means a concurrent knf_delete_state unlinked the state
     * (and already dropped the directory's reference) between our lookup and
     * this call; the state is gone either way and our deref below releases the
     * final reference, so this is an idempotent success -- NOT a leak (the dir
     * reference is only ever dropped once, by whichever caller wins the race).
     * We never reach here with the state under a different directory because
     * knf_lookup_state resolved it under this same category path. */
    (void)removed;

    ObDereferenceObject(st);    /* releases our lookup ref -> frees the state */
    ObDereferenceObject(dir);
    return 0;
}

/* --- Publish / subscribe (kernel API) ------------------------------------ */

KNF_STATE *knf_open_state(const char *category, const char *name)
{
    /* Open-existing is exactly the object-model lookup (no create). */
    return knf_lookup_state(category, name);
}

NTSTATUS knf_reserve_payload(KNF_STATE *st, uint32_t cap)
{
    void    *newbuf, *oldbuf = NULL;
    uint64_t flags;

    if (!st || cap == 0 || cap > KNF_MAX_PAYLOAD)
        return STATUS_INVALID_PARAMETER;

    /* Reserve exists precisely so a later DISPATCH_LEVEL publish need not
     * allocate; the allocation itself must therefore happen at PASSIVE_LEVEL. */
    if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
        return STATUS_UNSUCCESSFUL;

    newbuf = kmalloc(cap);          /* cap <= KNF_MAX_PAYLOAD (4096): kmalloc range */
    if (!newbuf)
        return STATUS_INSUFFICIENT_RESOURCES;

    spin_lock_irqsave(&st->lock, &flags);
    if (st->payload_cap >= cap) {
        /* Another path already grew the buffer at least this large. */
        spin_unlock_irqrestore(&st->lock, flags);
        kfree(newbuf);
        return STATUS_SUCCESS;
    }
    if (st->payload && st->payload_len)
        memcpy(newbuf, st->payload, st->payload_len);
    oldbuf          = st->payload;
    st->payload     = newbuf;
    st->payload_cap = cap;
    /* payload_len and sequence unchanged: reserve is NOT a publish. */
    spin_unlock_irqrestore(&st->lock, flags);

    if (oldbuf)
        kfree(oldbuf);             /* free the old buffer OUTSIDE the lock */
    return STATUS_SUCCESS;
}

NTSTATUS knf_publish(KNF_STATE *st, const KNF_TYPE_ID *type_id,
                     const void *data, uint32_t len,
                     const uint64_t *matching_change_stamp,
                     uint64_t *out_prev, uint64_t *out_new)
{
    void                  *newbuf = NULL, *oldbuf = NULL;
    uint64_t               flags, prev, next_seq;
    struct knf_subscriber *sub;

    if (!st || len > KNF_MAX_PAYLOAD || (len > 0 && !data))
        return STATUS_INVALID_PARAMETER;

    /* Typed states accept only a payload carrying the registered tag; the blob
     * stays opaque to the kernel but the type contract is enforced. */
    if (st->has_type_id) {
        if (!type_id || memcmp(type_id, &st->type_id, sizeof(st->type_id)) != 0)
            return STATUS_OBJECT_TYPE_MISMATCH;
    }

    /* Pre-allocate any growth BEFORE taking the lock -- kmalloc must never run
     * under a spinlock. Only PASSIVE_LEVEL callers may allocate; a
     * DISPATCH_LEVEL publish relies on a prior knf_reserve_payload and installs
     * nothing new here (newbuf stays NULL -> the grow branch below fails). The
     * payload_cap read is racy but only ever grows under the lock, so a stale
     * small read just over-allocates a spare buffer we free after unlock. */
    if (len > 0 && KeGetCurrentIrql() < DISPATCH_LEVEL && len > st->payload_cap) {
        newbuf = kmalloc(len);
        if (!newbuf)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    spin_lock_irqsave(&st->lock, &flags);

    prev = (uint64_t)atomic64_read(&st->sequence);

    /* Strict monotonicity: never wrap the change stamp. At the maximum value a
     * prev+1 would alias 0 (a fresh state) and the `last_seen < next_seq` arm
     * below would silently drop the notification. Reject instead. (2^64 - 1
     * publishes is unreachable in practice; this guards fault-injection and
     * long-uptime correctness.) */
    if (prev == (uint64_t)0xFFFFFFFFFFFFFFFFull) {
        spin_unlock_irqrestore(&st->lock, flags);
        if (newbuf)
            kfree(newbuf);
        return STATUS_INVALID_PARAMETER;
    }

    /* Conditional (CAS) publish: bail if the caller's expected stamp is stale.
     * The compare-and-bump runs under the per-state lock (all writers serialize
     * here), so lock-free atomic64_read consumers see a monotonic sequence. */
    if (matching_change_stamp && *matching_change_stamp != prev) {
        spin_unlock_irqrestore(&st->lock, flags);
        if (newbuf)
            kfree(newbuf);
        return STATUS_UNSUCCESSFUL;
    }

    if (len > 0) {
        if (len > st->payload_cap) {
            if (!newbuf) {
                /* DISPATCH_LEVEL publish with no reserved room: the contract is
                 * to pre-size at PASSIVE via knf_reserve_payload. */
                spin_unlock_irqrestore(&st->lock, flags);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            oldbuf          = st->payload;
            st->payload     = newbuf;
            st->payload_cap = len;
            newbuf          = NULL;   /* installed; do not free below */
        }
        memcpy(st->payload, data, len);
        st->payload_len = len;
    } else {
        st->payload_len = 0;          /* sequence-only signal publish */
    }

    next_seq = prev + 1;
    atomic64_set(&st->sequence, (int64_t)next_seq);

    /* Arm every subscriber that has not yet seen this advance. Coalescing (a
     * subscriber that already has a pending notification keeps its earlier
     * pending_prev) is the natural level-triggered behavior; the missed-update
     * count + edge-triggered option are the coalescing/retention layer. */
    for (sub = st->subscribers; sub; sub = sub->next) {
        if (!sub->has_pending && sub->last_seen < next_seq) {
            sub->pending_prev = sub->last_seen;
            sub->has_pending  = 1;
        }
    }

    spin_unlock_irqrestore(&st->lock, flags);

    if (newbuf)
        kfree(newbuf);                /* pre-alloced but buffer already sufficed */
    if (oldbuf)
        kfree(oldbuf);                /* replaced buffer, freed outside the lock */

    if (out_prev)
        *out_prev = prev;
    if (out_new)
        *out_new = next_seq;
    return STATUS_SUCCESS;
}

NTSTATUS knf_subscribe(KNF_STATE *st, struct knf_subscriber **out_sub)
{
    struct knf_subscriber *sub;
    uint64_t               flags;

    if (!st || !out_sub)
        return STATUS_INVALID_PARAMETER;
    *out_sub = NULL;

    sub = (struct knf_subscriber *)kmalloc(sizeof(*sub));
    if (!sub)
        return STATUS_INSUFFICIENT_RESOURCES;
    memset(sub, 0, sizeof(*sub));

    /* Pin the state so it cannot be torn down while this subscription is live
     * (knf_unsubscribe drops this reference). */
    ObReferenceObject(st);
    sub->state = st;

    spin_lock_irqsave(&st->lock, &flags);
    /* Baseline at the current sequence: only future publishes notify. */
    sub->last_seen   = (uint64_t)atomic64_read(&st->sequence);
    sub->has_pending = 0;
    sub->next        = st->subscribers;
    st->subscribers  = sub;
    spin_unlock_irqrestore(&st->lock, flags);

    *out_sub = sub;
    return STATUS_SUCCESS;
}

NTSTATUS knf_unsubscribe(struct knf_subscriber **psub)
{
    KNF_STATE              *st;
    struct knf_subscriber  *sub;
    struct knf_subscriber **pp;
    uint64_t                flags;
    int                     found = 0;

    /* Consume the handle: once freed the node pointer must never be reused, so
     * we take it by reference and null it on success. A second call therefore
     * sees *psub == NULL and returns INVALID_PARAMETER instead of dereferencing
     * freed memory (no use-after-free on a double unsubscribe). */
    if (!psub || !*psub || !(*psub)->state)
        return STATUS_INVALID_PARAMETER;
    sub = *psub;
    st  = sub->state;

    spin_lock_irqsave(&st->lock, &flags);
    for (pp = &st->subscribers; *pp; pp = &(*pp)->next) {
        if (*pp == sub) {
            *pp   = sub->next;
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&st->lock, flags);

    if (!found)
        return STATUS_NOT_FOUND;      /* node not on its state's list */

    kfree(sub);                       /* free the node OUTSIDE the lock */
    ObDereferenceObject(st);          /* drop the pin taken by knf_subscribe */
    *psub = (struct knf_subscriber *)0;   /* consumed: caller cannot reuse it */
    return STATUS_SUCCESS;
}

NTSTATUS knf_subscription_poll(struct knf_subscriber *sub,
                               uint64_t *out_prev, uint64_t *out_new)
{
    KNF_STATE *st;
    uint64_t   flags, cur;
    NTSTATUS   status;

    if (!sub || !sub->state)
        return STATUS_INVALID_PARAMETER;
    st = sub->state;

    spin_lock_irqsave(&st->lock, &flags);
    if (sub->has_pending) {
        cur = (uint64_t)atomic64_read(&st->sequence);
        if (out_prev)
            *out_prev = sub->pending_prev;
        if (out_new)
            *out_new = cur;
        sub->last_seen   = cur;
        sub->has_pending = 0;
        status = STATUS_SUCCESS;
    } else {
        status = STATUS_NO_MORE_ENTRIES;
    }
    spin_unlock_irqrestore(&st->lock, flags);
    return status;
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
