/* ============================================================================
 * ob_callback.h -- Object Manager handle operation callbacks
 *
 * Allows kernel-mode code to register pre- and post-operation callbacks on
 * handle create and duplicate operations.  Matches Win11's ObRegisterCallbacks
 * API for driver compatibility and anti-tamper enforcement.
 *
 * XREF: 02-kernel-core/TODO-03-object-manager.md S13
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/ob/ob_type.h"

/* Operation types */
typedef enum {
    OB_OPERATION_HANDLE_CREATE    = 1,
    OB_OPERATION_HANDLE_DUPLICATE = 2,
} OB_OPERATION;

/* Pre-operation info -- pre-callback can modify *desired_access to strip bits */
typedef struct {
    OB_OPERATION         operation;
    void                *object;
    const OBJECT_TYPE   *object_type;
    uint32_t            *desired_access;  /* mutable -- pre-callback can strip bits */
    void                *context;
} OB_PRE_OPERATION_INFORMATION;

/* Post-operation info -- granted_access is read-only */
typedef struct {
    OB_OPERATION         operation;
    void                *object;
    const OBJECT_TYPE   *object_type;
    uint32_t             granted_access;  /* read-only */
    void                *context;
} OB_POST_OPERATION_INFORMATION;

/* Callback function types */
typedef void (*OB_PRE_OPERATION_CALLBACK)(OB_PRE_OPERATION_INFORMATION *info);
typedef void (*OB_POST_OPERATION_CALLBACK)(OB_POST_OPERATION_INFORMATION *info);

/* Per-type operation registration */
typedef struct {
    const OBJECT_TYPE          *object_type;     /* which type to filter */
    uint32_t                    operations;       /* bitmask of OB_OPERATION values */
    OB_PRE_OPERATION_CALLBACK   pre_callback;
    OB_POST_OPERATION_CALLBACK  post_callback;
} OB_OPERATION_REGISTRATION;

/* Top-level registration structure */
#define OB_CALLBACK_VERSION  1
#define OB_MAX_CALLBACK_OPS  4    /* max operation entries per registration */

typedef struct {
    uint16_t                     version;         /* OB_CALLBACK_VERSION */
    uint16_t                     operation_count;  /* number of entries in operations[] */
    uint32_t                     altitude;         /* sort order: lower = earlier invocation */
    void                        *context;          /* caller-supplied context */
    OB_OPERATION_REGISTRATION    operations[OB_MAX_CALLBACK_OPS];
} OB_CALLBACK_REGISTRATION;

/* Registration handle (opaque) */
typedef int32_t OB_CALLBACK_HANDLE;

#define OB_INVALID_CALLBACK_HANDLE  (-1)

/* ---- Public API ---- */

/* Register callbacks. Returns 0 on success, -1 on failure.
 * *out_handle receives a handle for later unregistration. */
int ObRegisterCallbacks(const OB_CALLBACK_REGISTRATION *reg,
                        OB_CALLBACK_HANDLE *out_handle);

/* Unregister previously registered callbacks. */
void ObUnRegisterCallbacks(OB_CALLBACK_HANDLE handle);

/* Internal: invoke pre-callbacks for an operation. Returns 0 to proceed,
 * -1 if desired_access was zeroed (deny). Called by ObpAllocateHandle. */
int ob_invoke_pre_callbacks(OB_OPERATION op, void *object,
                            const OBJECT_TYPE *type, uint32_t *access);

/* Internal: invoke post-callbacks after a successful operation. */
void ob_invoke_post_callbacks(OB_OPERATION op, void *object,
                              const OBJECT_TYPE *type, uint32_t granted);
