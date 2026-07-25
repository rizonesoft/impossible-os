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
#include "kernel/quota/quota.h"   /* per-state / per-subscription charge receipts */

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

/* --- Observability bridge flags (KNF_TRACE_*) --------------------------- *
 * Per-state, opt-in (default 0). When set, knf_publish mirrors the publish to
 * klog and/or ETW after releasing the state lock. The bridge runs ONLY below
 * DISPATCH_LEVEL (a DPC/DISPATCH publish skips tracing -- observability is
 * best-effort, never at ISR/DPC priority) and is guarded against re-entry.   */
#define KNF_TRACE_ETW     0x1u   /* emit an ETW_EVT_KNF_PUBLISH record on publish */
#define KNF_TRACE_KLOG    0x2u   /* emit one klog line on publish */
#define KNF_PERSIST_LAST  0x4u   /* reserved/no-op: retention is now unconditional
                                  * (every publish retains; knf_query_last_kernel
                                  * always returns the last payload regardless) */
#define KNF_TRACE_ALL     (KNF_TRACE_ETW | KNF_TRACE_KLOG | KNF_PERSIST_LAST)

/* --- Delivery / retention policy flags (KNF_MODE_*) --------------------- *
 * Per-state, set under the state lock. KNF_MODE_SECRET marks a payload whose
 * bytes must not cross to user mode un-redacted: the kernel-private retention
 * query (knf_query_last_kernel) still returns the raw payload to KernelMode
 * callers, but the user-mode WNF query surface (native syscalls) MUST redact a
 * secret state's payload before returning it -- enforcement lands with that
 * surface. Reserved-but-honored here as the policy source of truth.           */
#define KNF_MODE_SECRET   0x1u   /* payload is secret: user-mode query must redact */
#define KNF_MODE_ALL      (KNF_MODE_SECRET)

/* --- ETW publish record (ABI: consumed by ETW session readers) ---------- *
 * knf_publish emits this as ETW_EVT_KNF_PUBLISH (see <kernel/etw.h>) when a
 * state carries KNF_TRACE_ETW. Little-endian, PACKED: the layout is pinned so
 * out-of-tree readers decode it identically. Do NOT reorder or drop the packed
 * attribute -- a natural (unpacked) struct with these fields tail-pads to
 * 2*KNF_NAME_MAX + 24 on x86-64, which would misalign every consumer that
 * recreates it from the event id. The size is asserted so the writer and any
 * in-tree reader that includes this header agree on the wire format.
 * category+name together are the state's unique identity: KNF leaf names are
 * only unique within a category directory (\Notifications\<category>\<name>),
 * so a record MUST carry both to disambiguate e.g. Kernel\Foo from Power\Foo. */
typedef struct knf_etw_record {
    char     category[KNF_NAME_MAX];/* namespace category (Kernel/Power/... NUL-pad) */
    char     name[KNF_NAME_MAX];   /* leaf state name (NUL-padded)            */
    uint64_t sequence;             /* change stamp after this publish         */
    uint32_t pid;                  /* best-effort publisher PID               */
    uint32_t tid;                  /* best-effort publisher TID               */
    int32_t  status;               /* NTSTATUS of the publish                 */
} __attribute__((packed)) KNF_ETW_RECORD;

_Static_assert(sizeof(KNF_ETW_RECORD) == 2 * KNF_NAME_MAX + 20,
               "KNF_ETW_RECORD is a pinned ETW ABI record -- layout must not drift");
/* Pin every field OFFSET, not just the total size: the struct is packed, so a
 * same-width reorder (e.g. swapping pid/tid) would keep sizeof at 2*NAME_MAX+20
 * yet silently change the wire layout every reader decodes. */
_Static_assert(__builtin_offsetof(KNF_ETW_RECORD, category) == 0,               "KNF ABI: category offset");
_Static_assert(__builtin_offsetof(KNF_ETW_RECORD, name)     == KNF_NAME_MAX,     "KNF ABI: name offset");
_Static_assert(__builtin_offsetof(KNF_ETW_RECORD, sequence) == 2 * KNF_NAME_MAX, "KNF ABI: sequence offset");
_Static_assert(__builtin_offsetof(KNF_ETW_RECORD, pid)      == 2 * KNF_NAME_MAX + 8,  "KNF ABI: pid offset");
_Static_assert(__builtin_offsetof(KNF_ETW_RECORD, tid)      == 2 * KNF_NAME_MAX + 12, "KNF ABI: tid offset");
_Static_assert(__builtin_offsetof(KNF_ETW_RECORD, status)   == 2 * KNF_NAME_MAX + 16, "KNF ABI: status offset");

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
    uint64_t               missed;        /* updates coalesced away since last poll (lock):
                                           * level-triggered publishes that landed while a
                                           * notification was already pending. Saturating. */
    /* QUOTA_RES_NOTIFICATION_SUB charge for this node, billed to the task that
     * subscribed. Owned by the node, so every path that frees the node returns
     * it -- there is no separate lookup to get wrong. The token is stored BY
     * VALUE beside the receipt exactly as quota.h requires: the node is freed
     * immediately after the return, so a token loaded back through recycled
     * storage could otherwise return a stranger's charge. */
    quota_charge_receipt_t quota;
    uint64_t               quota_token;
};

/* --- KNF_STATE object body ----------------------------------------------- */

typedef struct knf_state {
    char            name[KNF_NAME_MAX];  /* leaf name under category dir */
    char            category[KNF_NAME_MAX]; /* owning category dir name (trace identity) */
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
    uint32_t        trace_flags;         /* KNF_TRACE_* bridge opt-ins (set/read under lock) */
    uint32_t        mode_flags;          /* KNF_MODE_* delivery/retention policy (set/read under lock) */
    /* Quota charges for this state, both billed to the creating task and both
     * returned in knf_state_on_delete -- the ONE teardown point every path
     * (normal delete AND the create-or-open loser) funnels through.
     *
     * `quota_bytes` charges the state's retention BUDGET (KNF_MAX_PAYLOAD), not
     * its current payload_len. Charging actual bytes would need to re-charge a
     * delta as the buffer grows, and a receipt holds one charge at a time by
     * design, so a grow would have to drop the old charge before taking the new
     * one and could then find the budget gone while still holding the buffer.
     * The budget is the honest quantity anyway: a live state can retain up to
     * KNF_MAX_PAYLOAD at any moment, so that is what it pins from the user.
     * Tightening this to high-water actual bytes needs the atomic multi-block
     * adjust primitive (TODO-25 s11), which does not exist yet. */
    quota_charge_receipt_t quota_state;  /* QUOTA_RES_NOTIFICATION_STATE, 1  */
    uint64_t        quota_state_token;
    quota_charge_receipt_t quota_bytes;  /* QUOTA_RES_NOTIFICATION_BYTES     */
    uint64_t        quota_bytes_token;

    uint8_t         diag_counted;        /* 1 once counted into the live-state diag tally
                                          * (only after a successful insert): gates the
                                          * on_delete decrement so a create-or-open loser,
                                          * which is destroyed without ever being counted,
                                          * does not underflow the counter. Zero-init. */

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
 * knf_set_trace_flags -- set the KNF_TRACE_* observability opt-ins on a state
 * (replaces the current set; guarded by the state lock). Only KNF_TRACE_* bits
 * are honored. Returns STATUS_SUCCESS or STATUS_INVALID_PARAMETER.
 */
NTSTATUS knf_set_trace_flags(KNF_STATE *st, uint32_t flags);

/*
 * knf_set_mode -- set the KNF_MODE_* delivery/retention policy on a state
 * (replaces the current set; guarded by the state lock). Only KNF_MODE_* bits
 * are honored. Returns STATUS_SUCCESS or STATUS_INVALID_PARAMETER.
 */
NTSTATUS knf_set_mode(KNF_STATE *st, uint32_t flags);

/*
 * knf_query_last_kernel -- KernelMode-only query-after-miss: copy the state's
 * retained last payload (up to cap bytes) into buf and report the current
 * change stamp, without a subscription. Snapshots under the state lock. A state
 * with no payload yet returns STATUS_SUCCESS with *out_len == 0. This is
 * deliberately kernel-private: the user-mode WNF query surface must layer
 * KNF_MODE_SECRET redaction on top (a raw user-mode payload read would leak a
 * secret state), so it is NOT the primitive user mode calls directly.
 */
NTSTATUS knf_query_last_kernel(KNF_STATE *st, void *buf, uint32_t cap,
                               uint32_t *out_len, uint64_t *out_seq);

/*
 * knf_subscription_missed_count -- read-and-reset the subscriber's coalesced-
 * update counter (number of publishes that landed while a notification was
 * already pending, i.e. updates the subscriber will never see individually).
 * Guarded by the state lock. Returns the count since the last call and clears it.
 */
uint64_t knf_subscription_missed_count(struct knf_subscriber *sub);

/*
 * knf_trace_drops_at_dispatch_count -- number of traced publishes that were
 * dropped because they ran at >= DISPATCH_LEVEL (the klog/ETW bridge takes
 * locks in thread context and cannot run at DPC/ISR priority). Exposed so the
 * best-effort bridge's losses are observable rather than silent (the KNF
 * diagnostics counters surface this).
 */
uint64_t knf_trace_drops_at_dispatch_count(void);

/*
 * knf_trace_skips_guard_count -- number of traced publishes suppressed by the
 * recursion guard: same-CPU re-entry from a klog/ETW re-publish, a concurrent
 * trace aliased onto the same thread by the global scheduler cursor, OR a
 * pre-scheduler publish with no current thread to arm the guard. Counted so
 * every intentional trace-suppression path is observable, not silently lost.
 */
uint64_t knf_trace_skips_guard_count(void);

/*
 * Diagnostics counters surfaced through NtQuerySystemInformation
 * (SystemNotificationInformation). live_state/subscriber are live-object counts
 * (inc/dec); publish/coalesced/security-denial are cumulative tallies. All are
 * best-effort relaxed atomics (diagnostics, not control decisions).
 */
uint64_t knf_diag_live_state_count(void);
uint64_t knf_diag_subscriber_count(void);
uint64_t knf_diag_publish_count(void);
uint64_t knf_diag_coalesced_count(void);
uint64_t knf_diag_security_denial_count(void);

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
 * KNF_MAX_SUBSCRIBERS_PER_STATE is reached), or STATUS_QUOTA_EXCEEDED (the
 * caller is at its QUOTA_RES_NOTIFICATION_SUB budget). The quota status is
 * distinct from INSUFFICIENT_RESOURCES on purpose: the subscriber cap bounds
 * ONE state's list, while the quota bounds how many subscriptions a single
 * user holds across every state, and a caller that cannot tell them apart
 * would retry forever against a budget that is not going to move.
 *
 * It can ALSO return the charge path's two lifecycle statuses verbatim, and both
 * are distinct from the quota refusal above:
 *   STATUS_RETRY -- TRANSIENT. A job-membership transition briefly holds the
 *     caller's charge gate closed (see quota_gate_quiesce in quota_ledger.h).
 *     Nothing was charged and nothing is over budget; retry. Flattening this into
 *     STATUS_QUOTA_EXCEEDED, which this function used to do, told user mode a
 *     process was out of quota when it had merely raced a job assignment.
 *   STATUS_PROCESS_IS_TERMINATING -- the calling process is dying; do not retry.
 *   STATUS_INTEGER_OVERFLOW -- an accounting-integrity or identity-domain
 *     failure inside the charge path (a receipt's generation space exhausted,
 *     or a counter found in an impossible state). NOT a quota refusal and NOT
 *     retryable: retrying re-derives the same result, and treating it as
 *     'over budget' would hide a real accounting fault.
 * Call at PASSIVE_LEVEL (allocates).
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
