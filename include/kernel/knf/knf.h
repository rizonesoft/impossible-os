/* ============================================================================
 * knf.h -- Kernel Notification Facility (WNF-style state-change notifications)
 *
 * A KNF_STATE is an Object Manager object (type "NotificationState") living
 * under \Notifications\<Category>\<Name>. Producers publish a typed payload
 * with a monotonic sequence (change stamp); consumers subscribe and wait.
 *
 * This file is the object model (TODO-16 notification state object type):
 * the object type, namespace tree, create primitive, and metadata fields.
 * Publish/subscribe, waitable subscriptions, access policy, coalescing, and
 * persistent registry backing are the later TODO-16 sections; the reserved
 * body fields below are filled by those sections.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/atomic.h"
#include "kernel/sched/spinlock.h"
#include "kernel/nt/ntstatus.h"

/* --- Limits -------------------------------------------------------------- */

/* Leaf state name cap. MUST stay <= OB_NAME_MAX (the namespace component
 * limit, 64) -- a state is published as a namespace component, so a longer
 * name could never be inserted. knf.c static-asserts the relationship. */
#define KNF_NAME_MAX     64     /* leaf state name, null-terminated */
#define KNF_MAX_PAYLOAD  4096   /* max published payload bytes (publish enforces) */

/* Per-state subscriber cap. Publish walks the subscriber list under the state
 * spinlock (IRQs disabled), so the list length bounds the lock-hold time on the
 * DPC/DISPATCH_LEVEL publish path. Cap it so one hot state cannot grow an
 * unbounded list and inflate interrupt latency. Generous vs the 100-subscriber
 * stress target; knf_subscribe returns STATUS_INSUFFICIENT_RESOURCES at the cap. */
#define KNF_MAX_SUBSCRIBERS_PER_STATE 4096

/* --- Lifetime classes (mirror WNF_STATE_NAME_LIFETIME) ------------------- */

typedef enum knf_lifetime {
    KNF_LIFETIME_WELLKNOWN  = 0,  /* system-provisioned; always present */
    KNF_LIFETIME_PERMANENT  = 1,  /* survives publisher exit; needs privilege */
    KNF_LIFETIME_PERSISTENT = 2,  /* survives reboot (registry backing later) */
    KNF_LIFETIME_TEMPORARY  = 3,  /* freed at last handle close (removal later) */
} KNF_LIFETIME;

/* --- Data scope (mirror WNF_DATA_SCOPE) ---------------------------------- *
 * v1: scope is ADVISORY only -- a state has a single payload instance. Per-
 * scope isolated instances (one payload per session/user) are a later item.
 * The field is kept for forward-compat.                                     */
typedef enum knf_data_scope {
    KNF_SCOPE_SYSTEM  = 0,
    KNF_SCOPE_SESSION = 1,
    KNF_SCOPE_USER    = 2,
    KNF_SCOPE_MACHINE = 3,
    KNF_SCOPE_PROCESS = 4,
} KNF_DATA_SCOPE;

/* Optional 16-byte GUID typing the opaque payload blob (WNF_TYPE_ID). */
typedef struct knf_type_id {
    uint8_t bytes[16];
} KNF_TYPE_ID;

/* --- Subscriber node (kernel publish/subscribe, non-waitable) ------------ *
 * An in-kernel subscription to a KNF_STATE. A subscriber remembers the last
 * sequence it acknowledged; a publish that advances past it arms `has_pending`
 * so a later knf_subscription_poll() reports (prev, new). The Ob-handle
 * waitable wrapper (NtWaitForSingleObject), NtClose/exit teardown, and the
 * close-vs-publish refcount race belong to the waitable-user-subscriptions
 * layer -- this node is the primitive that layer wraps.
 *
 * LIFETIME: knf_subscribe() takes an Ob reference on the state; knf_unsubscribe()
 * drops it. A live subscription therefore pins the KNF_STATE body, so it cannot
 * be freed (via knf_delete_state) while any subscriber references it, and
 * knf_state_on_delete always observes an empty subscriber list.
 *
 * LOCKING: `state` is immutable for the node's whole lifetime (set once in
 * knf_subscribe, never rewritten), so poll/unsubscribe read it locklessly to
 * find the lock to take -- safe under the single-owner contract below. All
 * OTHER fields (list linkage + last_seen/pending_prev/has_pending) are mutated
 * only under KNF_STATE.lock. The node's own kmalloc/kfree bracket the lock. */
struct knf_subscriber {
    struct knf_subscriber *next;          /* per-state list linkage (lock) */
    struct knf_state      *state;         /* ref-pinned owning state (immutable) */
    uint64_t               last_seen;     /* highest sequence acknowledged (lock) */
    uint64_t               pending_prev;  /* sequence before the pending publish (lock) */
    int                    has_pending;   /* a publish advanced past last_seen (lock) */
};

/* --- KNF_STATE object body ----------------------------------------------- */

typedef struct knf_state {
    char            name[KNF_NAME_MAX];  /* leaf name under category dir */
    atomic64_t      sequence;            /* monotonic change stamp (0 at create) */
    KNF_LIFETIME    lifetime;
    KNF_DATA_SCOPE  scope;
    KNF_TYPE_ID     type_id;             /* payload type tag (has_type_id gates) */
    int             has_type_id;

    /* Reserved forward-compat fields. body_size is fixed at type-registration
     * time, so later sections FILL these rather than grow the body. Payload is
     * a pointer (allocated by the retention path on publish), NOT an inline
     * buffer. */
    void           *payload;             /* retention: allocated on publish */
    uint32_t        payload_len;         /* bytes valid in payload */
    uint32_t        payload_cap;         /* allocated capacity (grows only, under lock;
                                          * publish reads it locklessly-relaxed to gate
                                          * pre-lock alloc, then re-checks under lock) */
    struct knf_subscriber *subscribers;  /* subscriber list head */
    uint32_t        subscriber_count;    /* len(subscribers); bounds lock-hold walk */

    spinlock_t      lock;                /* guards payload + subscribers + publish */
} KNF_STATE;

/* --- Access modes (match ssdt_previous_mode() convention) ---------------- */
#define KNF_KERNEL_MODE  0
#define KNF_USER_MODE    1

/* --- API (object-model subset) ------------------------------------------- */

/* Register the "NotificationState" Object Manager type. Called by knf_init. */
void ob_knf_type_init(void);

/* Create the \Notifications namespace tree and register the type. Called from
 * boot_phase2 after the registry is up (single-threaded, pre-scheduler). */
void knf_init(void);

/*
 * knf_create_state -- create a notification state under a category directory.
 *
 * category : one of "Kernel", "Power", "Security", "Session" (the object-model
 *            roots; the built-in catalog section adds more). Must exist.
 * name     : leaf name (< KNF_NAME_MAX), unique within the category.
 * lifetime : KNF_LIFETIME_*. PERMANENT/PERSISTENT from KNF_USER_MODE require
 *            SE_CREATE_PERMANENT_PRIVILEGE (matches WNF).
 * scope    : advisory in v1 (see KNF_DATA_SCOPE).
 * type_id  : optional payload GUID; NULL for untyped.
 * access_mode : KNF_KERNEL_MODE (always allowed) or KNF_USER_MODE (checked).
 *
 * Returns a referenced KNF_STATE body on success (caller drops its ref via
 * ObDereferenceObject once the namespace / a handle hold their own refs), or
 * NULL on failure (bad args, privilege denied, name collision, OOM).
 */
KNF_STATE *knf_create_state(const char *category, const char *name,
                            KNF_LIFETIME lifetime, KNF_DATA_SCOPE scope,
                            const KNF_TYPE_ID *type_id, uint32_t access_mode);

/*
 * knf_lookup_state -- resolve \Notifications\<category>\<name> to a KNF_STATE.
 * Returns a referenced body (caller must ObDereferenceObject) or NULL.
 */
KNF_STATE *knf_lookup_state(const char *category, const char *name);

/*
 * knf_delete_state -- unlink \Notifications\<category>\<name> from the
 * namespace and free the state (regardless of lifetime class). The native
 * NtDeleteWnfStateName surface will wrap this. Returns 0 on success, -1 if
 * the category or state does not exist. Deleting a state that still has live
 * subscribers is safe: each subscription pins the body via an Ob reference, so
 * the delete only unlinks it from the namespace; the body is freed once the
 * last subscriber unsubscribes. Delete does not proactively tear down
 * subscribers (they keep polling a still-referenced but unnamed state).
 */
int knf_delete_state(const char *category, const char *name);

/* --- Publish / subscribe (kernel API) ------------------------------------ */

/*
 * knf_open_state -- resolve an EXISTING \Notifications\<category>\<name> for
 * publish/subscribe. Unlike knf_create_state (create-or-open) this never
 * creates. Returns a referenced KNF_STATE body (caller ObDereferenceObject) or
 * NULL if it does not exist / bad args.
 */
KNF_STATE *knf_open_state(const char *category, const char *name);

/*
 * knf_reserve_payload -- pre-size a state's payload buffer to `cap` bytes
 * WITHOUT publishing (no sequence bump, no subscriber wake). This is the
 * PASSIVE_LEVEL path that makes a later DISPATCH_LEVEL/DPC knf_publish() of up
 * to `cap` bytes allocation-free. cap must be 1..KNF_MAX_PAYLOAD. Returns
 * STATUS_SUCCESS (buffer now >= cap), STATUS_INVALID_PARAMETER (bad args),
 * STATUS_UNSUCCESSFUL (called at DISPATCH_LEVEL or above -- reserve is
 * PASSIVE-only), or STATUS_INSUFFICIENT_RESOURCES (OOM).
 */
NTSTATUS knf_reserve_payload(KNF_STATE *st, uint32_t cap);

/*
 * knf_publish -- publish a new payload + advance the change stamp.
 *
 * type_id : must match the state's registered WNF_TYPE_ID when the state was
 *           created typed (has_type_id); ignored for untyped states. NULL is
 *           only valid for an untyped state.
 * data/len: payload bytes; len 0 is a sequence-only (signal) publish. len must
 *           be <= KNF_MAX_PAYLOAD.
 * matching_change_stamp : optional CAS guard. When non-NULL the publish only
 *           proceeds if the current sequence equals *matching_change_stamp
 *           (mirrors WNF conditional update); a mismatch returns
 *           STATUS_UNSUCCESSFUL without changing state.
 * out_prev/out_new : optional; receive the pre- and post-publish sequence.
 *
 * IRQL: callable at <= DISPATCH_LEVEL. A caller ABOVE DISPATCH_LEVEL (DIRQL /
 * device ISR) is rejected with STATUS_UNSUCCESSFUL BEFORE any work -- the
 * publish takes an IRQ-disabling spinlock and copies up to KNF_MAX_PAYLOAD
 * bytes, which must not run at ISR/DIRQL priority. At DISPATCH_LEVEL the payload
 * buffer is NOT grown (no allocation under a raised IRQL) -- a publish whose len
 * exceeds the current payload capacity returns STATUS_INSUFFICIENT_RESOURCES
 * unless the state was pre-sized via knf_reserve_payload at PASSIVE_LEVEL. Never
 * blocks; never allocates or calls out while holding the per-state lock.
 *
 * The change stamp is strictly monotonic and never wraps: a publish at the
 * maximum sequence (UINT64_MAX) is rejected with STATUS_INVALID_PARAMETER so
 * the stamp can never alias 0 (a fresh state) or skip a subscriber arm.
 *
 * Returns STATUS_SUCCESS, STATUS_INVALID_PARAMETER (NULL state / oversize len /
 * NULL data with len>0 / sequence at max), STATUS_OBJECT_TYPE_MISMATCH (typed
 * state, wrong tag), STATUS_UNSUCCESSFUL (CAS mismatch), or
 * STATUS_INSUFFICIENT_RESOURCES.
 */
NTSTATUS knf_publish(KNF_STATE *st, const KNF_TYPE_ID *type_id,
                     const void *data, uint32_t len,
                     const uint64_t *matching_change_stamp,
                     uint64_t *out_prev, uint64_t *out_new);

/*
 * knf_subscribe -- register an in-kernel subscription on `st`. Captures the
 * current sequence as the subscription's baseline (only future publishes
 * notify) and takes an Ob reference on the state (see struct knf_subscriber).
 * Returns STATUS_SUCCESS with *out_sub set, STATUS_INVALID_PARAMETER,
 * STATUS_UNSUCCESSFUL (called at >= DISPATCH_LEVEL -- allocates, PASSIVE-only),
 * or STATUS_INSUFFICIENT_RESOURCES (OOM, or the per-state subscriber cap
 * KNF_MAX_SUBSCRIBERS_PER_STATE is reached). Call at PASSIVE_LEVEL (allocates).
 */
NTSTATUS knf_subscribe(KNF_STATE *st, struct knf_subscriber **out_sub);

/*
 * knf_unsubscribe -- unlink and free a subscription, dropping the state pin.
 * CONSUMES the handle: on success the freed node pointer is nulled through
 * `*psub` so a caller can never reuse (or double-free) a dangling pointer.
 * Returns STATUS_SUCCESS (unlinked, freed, *psub set NULL),
 * STATUS_INVALID_PARAMETER (NULL psub / already-consumed *psub), or
 * STATUS_NOT_FOUND (node was not on its state's list). Call at PASSIVE_LEVEL.
 *
 * SINGLE-OWNER CONTRACT: a subscription handle has exactly one owner. Two
 * concurrent knf_unsubscribe/knf_subscription_poll calls on the SAME handle
 * (or a caller keeping a raw alias of a consumed handle) are undefined -- the
 * consume-and-null closes the sequential double-unsubscribe window but not a
 * cross-CPU alias race. The reference-counted, close-vs-publish-safe subscriber
 * lifetime (needed for NtClose/process-exit teardown) is the waitable-user-
 * subscriptions layer's job, which wraps this node in a refcounted Ob object.
 */
NTSTATUS knf_unsubscribe(struct knf_subscriber **psub);

/*
 * knf_subscription_poll -- non-blocking read of a pending notification.
 * If a publish has advanced the sequence past the subscriber's last-seen value,
 * reports (*out_prev = sequence before that advance, *out_new = current
 * sequence), advances the subscriber's baseline, and returns STATUS_SUCCESS.
 * Returns STATUS_NO_MORE_ENTRIES when nothing is pending, or
 * STATUS_INVALID_PARAMETER for a NULL / consumed subscriber handle. (The
 * blocking wait that wakes a consumer before it polls is the
 * waitable-user-subscriptions layer.)
 */
NTSTATUS knf_subscription_poll(struct knf_subscriber *sub,
                               uint64_t *out_prev, uint64_t *out_new);

/* Registered "NotificationState" type singleton (NULL until ob_knf_type_init). */
extern const struct object_type *ObpNotificationStateType;
