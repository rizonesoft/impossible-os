/* ============================================================================
 * ob_type.h -- Object type descriptor
 *
 * Each kernel object type (File, Process, Thread, Event, …) is described by
 * an OBJECT_TYPE singleton registered at boot.  The callbacks drive lifetime
 * and namespace behaviour for all instances of that type.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/atomic.h"

typedef struct object_type {
    const char *name;           /* type name: "File", "Process", ... */
    size_t      body_size;      /* sizeof the object body (for combined alloc) */

    /* Called when handle_count drops to 0 */
    void (*on_close)(void *body, uint32_t handle_count);

    /* Called when ref_count drops to 0 -- free body resources */
    void (*on_delete)(void *body);

    /* Optional access check on open (return 0 = allow, -1 = deny) */
    int (*on_open)(void *body, uint32_t access);

    /* Namespace parse -- walk remaining path through this object */
    int (*on_parse)(void *body, const char *remaining, void **result);

    /* Per-type statistics -- SMP-safe via atomics */
    atomic_t    total_objects;   /* current live objects of this type */
    atomic_t    total_handles;   /* current open handles to objects of this type */
    uint32_t    peak_objects;    /* high-water mark for total_objects */
    uint32_t    peak_handles;    /* high-water mark for total_handles */

    /* Per-type tracing */
    uint8_t     tracing_enabled; /* 1 = allocate OB_TRACE_INFO for new objects */
    uint8_t     _trace_pad[3];
} OBJECT_TYPE;
