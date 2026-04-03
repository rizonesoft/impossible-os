/* ============================================================================
 * ob_type.h -- Object type descriptor
 *
 * Each kernel object type (File, Process, Thread, Event, …) is described by
 * an OBJECT_TYPE singleton registered at boot.  The callbacks drive lifetime
 * and namespace behaviour for all instances of that type.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

typedef struct object_type {
    const char *name;           /* type name: "File", "Process", … */
    size_t      body_size;      /* sizeof the object body (for combined alloc) */

    /* Called when handle_count drops to 0 */
    void (*on_close)(void *body, uint32_t handle_count);

    /* Called when ref_count drops to 0 -- free body resources */
    void (*on_delete)(void *body);

    /* Optional access check on open (return 0 = allow, -1 = deny) */
    int (*on_open)(void *body, uint32_t access);

    /* Namespace parse -- walk remaining path through this object */
    int (*on_parse)(void *body, const char *remaining, void **result);
} OBJECT_TYPE;
