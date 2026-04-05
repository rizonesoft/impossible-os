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
 * XREF: 02-kernel-core/TODO-03-object-manager.md S13
 * ============================================================================ */

#include "kernel/ob/ob_callback.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"

/* ---- Callback registry -------------------------------------------------- */

#define OB_MAX_CALLBACKS  16

typedef struct {
    int                          active;
    uint32_t                     altitude;
    void                        *context;
    uint16_t                     op_count;
    OB_OPERATION_REGISTRATION    ops[OB_MAX_CALLBACK_OPS];
} ob_callback_node_t;

static ob_callback_node_t  g_callbacks[OB_MAX_CALLBACKS];
static uint32_t            g_callback_count;
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

    /* Insert new node */
    ob_callback_node_t *node = &g_callbacks[pos];
    node->active   = 1;
    node->altitude = reg->altitude;
    node->context  = reg->context;
    node->op_count = reg->operation_count;
    for (uint16_t k = 0; k < reg->operation_count; k++)
        node->ops[k] = reg->operations[k];

    g_callback_count++;
    OB_CALLBACK_HANDLE handle = (OB_CALLBACK_HANDLE)pos;

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

    if (handle < 0 || (uint32_t)handle >= OB_MAX_CALLBACKS)
        return;

    spin_lock_irqsave(&s_cb_lock, &irq_flags);

    if ((uint32_t)handle >= g_callback_count ||
        !g_callbacks[handle].active) {
        spin_unlock_irqrestore(&s_cb_lock, irq_flags);
        return;
    }

    /* Shift entries down to fill the gap */
    for (uint32_t j = (uint32_t)handle; j + 1 < g_callback_count; j++)
        g_callbacks[j] = g_callbacks[j + 1];

    g_callback_count--;

    /* Zero the freed slot */
    g_callbacks[g_callback_count].active = 0;

    spin_unlock_irqrestore(&s_cb_lock, irq_flags);

    klog(LOG_DEBUG, "ob", "ObUnRegisterCallbacks: handle %d removed",
         (int32_t)handle);
}

/* ---- Internal invoke helpers -------------------------------------------- */

int ob_invoke_pre_callbacks(OB_OPERATION op, void *object,
                            const OBJECT_TYPE *type, uint32_t *access)
{
    uint64_t irq_flags;

    spin_lock_irqsave(&s_cb_lock, &irq_flags);

    for (uint32_t i = 0; i < g_callback_count; i++) {
        ob_callback_node_t *node = &g_callbacks[i];
        if (!node->active) continue;

        for (uint16_t k = 0; k < node->op_count; k++) {
            if (node->ops[k].object_type != type)
                continue;
            if (!(node->ops[k].operations & (uint32_t)op))
                continue;
            if (!node->ops[k].pre_callback)
                continue;

            OB_PRE_OPERATION_INFORMATION info = {
                .operation      = op,
                .object         = object,
                .object_type    = type,
                .desired_access = access,
                .context        = node->context,
            };

            node->ops[k].pre_callback(&info);

            /* If callback zeroed access, deny the operation */
            if (*access == 0) {
                spin_unlock_irqrestore(&s_cb_lock, irq_flags);
                return -1;
            }
        }
    }

    spin_unlock_irqrestore(&s_cb_lock, irq_flags);
    return 0;
}

void ob_invoke_post_callbacks(OB_OPERATION op, void *object,
                              const OBJECT_TYPE *type, uint32_t granted)
{
    uint64_t irq_flags;

    spin_lock_irqsave(&s_cb_lock, &irq_flags);

    for (uint32_t i = 0; i < g_callback_count; i++) {
        ob_callback_node_t *node = &g_callbacks[i];
        if (!node->active) continue;

        for (uint16_t k = 0; k < node->op_count; k++) {
            if (node->ops[k].object_type != type)
                continue;
            if (!(node->ops[k].operations & (uint32_t)op))
                continue;
            if (!node->ops[k].post_callback)
                continue;

            OB_POST_OPERATION_INFORMATION info = {
                .operation      = op,
                .object         = object,
                .object_type    = type,
                .granted_access = granted,
                .context        = node->context,
            };

            node->ops[k].post_callback(&info);
        }
    }

    spin_unlock_irqrestore(&s_cb_lock, irq_flags);
}
