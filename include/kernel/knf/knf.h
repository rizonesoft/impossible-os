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

/* --- Limits -------------------------------------------------------------- */

/* Leaf state name cap. MUST stay <= OB_NAME_MAX (the namespace component
 * limit, 64) -- a state is published as a namespace component, so a longer
 * name could never be inserted. knf.c static-asserts the relationship. */
#define KNF_NAME_MAX     64     /* leaf state name, null-terminated */
#define KNF_MAX_PAYLOAD  4096   /* max published payload bytes (publish enforces) */

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

struct knf_subscriber;   /* defined by the waitable-subscriptions section */

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
    uint32_t        payload_cap;         /* allocated capacity of payload */
    struct knf_subscriber *subscribers;  /* subscriber list head */

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
 * the category or state does not exist. NOTE: does not yet tear down live
 * subscribers -- that ordering is owned by the subscription-teardown path;
 * in the object-model layer a deleted state has none.
 */
int knf_delete_state(const char *category, const char *name);

/* Registered "NotificationState" type singleton (NULL until ob_knf_type_init). */
extern const struct object_type *ObpNotificationStateType;
