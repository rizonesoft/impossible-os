/* ============================================================================
 * ob_callback.c -- Object Manager handle operation callbacks
 *
 * Implements ObRegisterCallbacks / ObUnRegisterCallbacks and the internal
 * invoke helpers called from ObpAllocateHandle and NtDuplicateObject.
 *
 * Callback nodes are stored in a fixed-size array sorted by altitude.
 * SMP-safe via irqsave spinlock -- callbacks are invoked in the handle
 * allocation hot path so the lock hold time must be minimal.
 *
 * XREF: 02-kernel-core/TODO-05-object-manager.md S13
 * ============================================================================ */

#include "kernel/ob/ob_callback.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"

/* ---- Callback registry -------------------------------------------------- */

#define OB_MAX_CALLBACKS  16
/* Highest issuable registration id. g_next_cb_id is refused here so the
 * subsequent ++ can never signed-overflow (UB) and no handle is ever <= 0. */
#define OB_CB_ID_MAX      0x7FFFFFFF
/* Upper bound on (type, op) matches for one invoke: every node can carry up to
 * OB_MAX_CALLBACK_OPS entries that match. Snapshots are taken into a local array
 * of this size so driver callbacks run OUTSIDE the lock (see invoke helpers). */
#define OB_MAX_CALLBACK_MATCHES  (OB_MAX_CALLBACKS * OB_MAX_CALLBACK_OPS)
/* The snapshot bound MUST equal the true worst case (every node contributing
 * every op) so the snapshot can never silently drop a matching callback. If
 * either cap changes, this re-derives and the assert keeps them in lockstep. */
_Static_assert(OB_MAX_CALLBACK_MATCHES == OB_MAX_CALLBACKS * OB_MAX_CALLBACK_OPS,
               "callback snapshot bound must cover the worst-case match count");

typedef struct {
    int32_t                      id;        /* stable identity, NOT array position */
    int                          active;
    uint32_t                     altitude;
    void                        *context;
    uint16_t                     op_count;
    OB_OPERATION_REGISTRATION    ops[OB_MAX_CALLBACK_OPS];
} ob_callback_node_t;

static ob_callback_node_t  g_callbacks[OB_MAX_CALLBACKS];
static uint32_t            g_callback_count;
/* Monotonic registration id. Starts at 1 so 0 is never a live handle and a
 * stale/zeroed handle cannot alias a real registration. Decoupled from array
 * position so altitude-sort shifts never invalidate an outstanding handle. */
static int32_t             g_next_cb_id = 1;
static DEFINE_SPINLOCK(s_cb_lock);

/* ---- ObRegisterCallbacks ------------------------------------------------ */

int ObRegisterCallbacks(const OB_CALLBACK_REGISTRATION *reg,
                        OB_CALLBACK_HANDLE *out_handle)
{
    uint64_t irq_flags;

    if (!reg || !out_handle)
        return -1;
    if (reg->version != OB_CALLBACK_VERSION)
        return -1;
    if (reg->operation_count == 0 || reg->operation_count > OB_MAX_CALLBACK_OPS)
        return -1;

    spin_lock_irqsave(&s_cb_lock, &irq_flags);

    if (g_callback_count >= OB_MAX_CALLBACKS) {
        spin_unlock_irqrestore(&s_cb_lock, irq_flags);
        klog(LOG_ERROR, "ob", "ObRegisterCallbacks: table full (%u/%u)",
             g_callback_count, (uint32_t)OB_MAX_CALLBACKS);
        return -1;
    }

    /* Refuse before the id counter would overflow int32 (UB) or issue a
     * non-positive handle that ObUnRegisterCallbacks rejects. 2^31 is far
     * beyond any real register/unregister churn; fail cleanly if ever reached. */
    if (g_next_cb_id == OB_CB_ID_MAX) {
        spin_unlock_irqrestore(&s_cb_lock, irq_flags);
        klog(LOG_ERROR, "ob", "ObRegisterCallbacks: id space exhausted");
        return -1;
    }

    /* Find insertion point to keep sorted by altitude (ascending) */
    uint32_t pos = g_callback_count;
    for (uint32_t i = 0; i < g_callback_count; i++) {
        if (reg->altitude < g_callbacks[i].altitude) {
            pos = i;
            break;
        }
    }

    /* Shift higher entries up */
    for (uint32_t j = g_callback_count; j > pos; j--)
        g_callbacks[j] = g_callbacks[j - 1];

    /* Insert new node with a stable id (NOT the array slot, which shifts) */
    ob_callback_node_t *node = &g_callbacks[pos];
    node->id       = g_next_cb_id++;
    node->active   = 1;
    node->altitude = reg->altitude;
    node->context  = reg->context;
    node->op_count = reg->operation_count;
    for (uint16_t k = 0; k < reg->operation_count; k++)
        node->ops[k] = reg->operations[k];

    /* Atomic store pairs with the lockless relaxed load in the invoke fast
     * path (ob_invoke_pre/post_callbacks); under the lock here, so the RMW
     * atomicity is belt-and-suspenders but keeps the load/store well-formed. */
    __atomic_fetch_add(&g_callback_count, 1, __ATOMIC_RELAXED);
    OB_CALLBACK_HANDLE handle = (OB_CALLBACK_HANDLE)node->id;

    spin_unlock_irqrestore(&s_cb_lock, irq_flags);

    *out_handle = handle;
    klog(LOG_DEBUG, "ob", "ObRegisterCallbacks: altitude %u, %u ops, handle %d",
         reg->altitude, (uint32_t)reg->operation_count, (int32_t)handle);
    return 0;
}

/* ---- ObUnRegisterCallbacks ---------------------------------------------- */

void ObUnRegisterCallbacks(OB_CALLBACK_HANDLE handle)
{
    uint64_t irq_flags;

    if (handle <= 0)   /* ids start at 1; 0 / negative is never live */
        return;

    spin_lock_irqsave(&s_cb_lock, &irq_flags);

    /* Find the node by stable id, not by array position -- the array is
     * altitude-sorted and shifts on every register/unregister, so the slot a
     * handle once occupied may now hold a different (or reused) registration. */
    uint32_t pos = g_callback_count;
    for (uint32_t i = 0; i < g_callback_count; i++) {
        if (g_callbacks[i].active && g_callbacks[i].id == (int32_t)handle) {
            pos = i;
            break;
        }
    }
    if (pos >= g_callback_count) {
        spin_unlock_irqrestore(&s_cb_lock, irq_flags);
        return;   /* no live registration with this id */
    }

    /* Shift entries down to fill the gap */
    for (uint32_t j = pos; j + 1 < g_callback_count; j++)
        g_callbacks[j] = g_callbacks[j + 1];

    /* Atomic store pairs with the lockless relaxed load in the invoke fast
     * path; __atomic_sub_fetch returns the post-decrement count = index of the
     * now-freed last slot. */
    uint32_t freed = __atomic_sub_fetch(&g_callback_count, 1, __ATOMIC_RELAXED);

    /* Zero the freed slot */
    g_callbacks[freed].active = 0;
    g_callbacks[freed].id = 0;

    spin_unlock_irqrestore(&s_cb_lock, irq_flags);

    klog(LOG_DEBUG, "ob", "ObUnRegisterCallbacks: handle %d removed",
         (int32_t)handle);
}

/* ---- Internal invoke helpers -------------------------------------------- */

/* Snapshot taken under the lock: a (callback fn, context) pair. Driver code is
 * invoked from the snapshot AFTER the lock is released so an arbitrary callback
 * cannot deadlock/hang with IRQs off (a callback may take locks, allocate, log,
 * or re-enter the handle path). */
typedef struct {
    OB_PRE_OPERATION_CALLBACK   pre;
    OB_POST_OPERATION_CALLBACK  post;
    void                       *context;
} ob_cb_snap_t;

/* Slow path -- factored out and NOT inlined so the OB_MAX_CALLBACK_MATCHES
 * snapshot array (~1.5 KiB) is only reserved on the stack when callbacks are
 * actually registered, never on the common 0-callback ObpAllocateHandle path. */
__attribute__((noinline))
static int ob_dispatch_pre(OB_OPERATION op, void *object,
                           const OBJECT_TYPE *type, uint32_t *access)
{
    uint64_t irq_flags;
    ob_cb_snap_t snap[OB_MAX_CALLBACK_MATCHES];
    uint32_t n = 0;

    /* Phase 1: snapshot matching pre-callbacks in altitude order under the lock */
    spin_lock_irqsave(&s_cb_lock, &irq_flags);
    for (uint32_t i = 0; i < g_callback_count && n < OB_MAX_CALLBACK_MATCHES; i++) {
        ob_callback_node_t *node = &g_callbacks[i];
        if (!node->active) continue;
        for (uint16_t k = 0; k < node->op_count && n < OB_MAX_CALLBACK_MATCHES; k++) {
            if (node->ops[k].object_type != type) continue;
            if (!(node->ops[k].operations & (uint32_t)op)) continue;
            if (!node->ops[k].pre_callback) continue;
            snap[n].pre     = node->ops[k].pre_callback;
            snap[n].context = node->context;
            n++;
        }
    }
    spin_unlock_irqrestore(&s_cb_lock, irq_flags);

    /* Phase 2: invoke OUTSIDE the lock with a MONOTONICALLY-DECREASING ceiling.
     * Before each callback, snapshot the mask it is allowed to see; after it
     * runs, clamp back to that snapshot. A callback can therefore only STRIP
     * rights -- it can neither mint a right the caller never requested NOR
     * re-add a right an earlier (lower-altitude) callback already removed. The
     * earlier filter wins, which is what an anti-tamper chain requires. Deny
     * the operation outright if the mask is fully stripped to 0. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t prior = *access;   /* ceiling for this callback = what survives so far */
        OB_PRE_OPERATION_INFORMATION info = {
            .operation      = op,
            .object         = object,
            .object_type    = type,
            .desired_access = access,
            .context        = snap[i].context,
        };
        snap[i].pre(&info);
        *access &= prior;       /* strip-only: cannot exceed the pre-callback mask */
        /* Deny only when THIS callback zeroed a previously-nonzero mask -- i.e.
         * a filter actively stripped the last right (the header's "desired_access
         * was zeroed" deny contract). A request that ENTERED this callback at 0
         * (prior == 0) was never granted anything to strip, so a no-op callback
         * on a legitimately zero-access request must not flip it to a denial. */
        if (*access == 0 && prior != 0)
            return -1;          /* callback stripped the last right -> deny */
    }
    return 0;
}

int ob_invoke_pre_callbacks(OB_OPERATION op, void *object,
                            const OBJECT_TYPE *type, uint32_t *access)
{
    /* Fast path: no registered callbacks -> nothing to do, and no snapshot frame
     * is reserved (the array lives in ob_dispatch_pre, called only when needed).
     * Relaxed load is fine: a register racing this read is unordered with the
     * operation either way, and ob_dispatch_pre re-checks under the lock. */
    if (__atomic_load_n(&g_callback_count, __ATOMIC_RELAXED) == 0)
        return 0;
    return ob_dispatch_pre(op, object, type, access);
}

__attribute__((noinline))
static void ob_dispatch_post(OB_OPERATION op, void *object,
                             const OBJECT_TYPE *type, uint32_t granted)
{
    uint64_t irq_flags;
    ob_cb_snap_t snap[OB_MAX_CALLBACK_MATCHES];
    uint32_t n = 0;

    spin_lock_irqsave(&s_cb_lock, &irq_flags);
    for (uint32_t i = 0; i < g_callback_count && n < OB_MAX_CALLBACK_MATCHES; i++) {
        ob_callback_node_t *node = &g_callbacks[i];
        if (!node->active) continue;
        for (uint16_t k = 0; k < node->op_count && n < OB_MAX_CALLBACK_MATCHES; k++) {
            if (node->ops[k].object_type != type) continue;
            if (!(node->ops[k].operations & (uint32_t)op)) continue;
            if (!node->ops[k].post_callback) continue;
            snap[n].post    = node->ops[k].post_callback;
            snap[n].context = node->context;
            n++;
        }
    }
    spin_unlock_irqrestore(&s_cb_lock, irq_flags);

    for (uint32_t i = 0; i < n; i++) {
        OB_POST_OPERATION_INFORMATION info = {
            .operation      = op,
            .object         = object,
            .object_type    = type,
            .granted_access = granted,
            .context        = snap[i].context,
        };
        snap[i].post(&info);
    }
}

void ob_invoke_post_callbacks(OB_OPERATION op, void *object,
                              const OBJECT_TYPE *type, uint32_t granted)
{
    /* Fast path: no registered callbacks -> no snapshot frame reserved. */
    if (__atomic_load_n(&g_callback_count, __ATOMIC_RELAXED) == 0)
        return;
    ob_dispatch_post(op, object, type, granted);
}
