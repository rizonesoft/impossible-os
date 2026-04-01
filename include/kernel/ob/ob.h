/* ============================================================================
 * ob.h — Object Manager core: OBJECT_HEADER, flags, macros, API
 *
 * Every kernel object body is preceded in memory by an OBJECT_HEADER.
 * ob_alloc_object() returns a pointer to the body; the header is at
 * body - sizeof(OBJECT_HEADER).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/atomic.h"
#include "kernel/boot_init.h"
#include "kernel/ob/ob_type.h"

/* Forward-declare — implemented in §8 (security descriptor integration) */
typedef struct security_descriptor SECURITY_DESCRIPTOR;

/* --- Object flags -------------------------------------------------------- */

#define OB_FLAG_PERMANENT    (1u << 0)  /* never deleted when refcount hits 0 */
#define OB_FLAG_KERNEL_ONLY  (1u << 1)  /* not accessible from user mode */
#define OB_FLAG_NAMED        (1u << 2)  /* object has a namespace entry */
#define OB_FLAG_PMM_ALLOC    (1u << 3)  /* allocated via pmm_alloc_contiguous (not kmalloc) */

/* --- OBJECT_HEADER ------------------------------------------------------- */

typedef struct object_header {
    atomic_t             ref_count;     /* atomic reference count (SMP-safe) */
    uint32_t             handle_count;  /* number of open handles */
    const OBJECT_TYPE   *type;          /* type descriptor */
    const char          *name;          /* namespace name (NULL if unnamed) */
    SECURITY_DESCRIPTOR *security;      /* NULL until §8 */
    uint32_t             flags;         /* OB_FLAG_* */
    uint32_t             _pad;          /* align body to 8 bytes */
} OBJECT_HEADER;

/* --- Pointer conversion macros ------------------------------------------- */

#define OB_HEADER_FROM_BODY(body_ptr) \
    ((OBJECT_HEADER *)((uint8_t *)(body_ptr) - sizeof(OBJECT_HEADER)))

#define OB_BODY_FROM_HEADER(hdr_ptr) \
    ((void *)((uint8_t *)(hdr_ptr) + sizeof(OBJECT_HEADER)))

/* --- Limits -------------------------------------------------------------- */

#define OB_MAX_TYPES  32

/* --- Built-in type singletons (populated by ob_init) --------------------- */

extern const OBJECT_TYPE *ObpFileType;
extern const OBJECT_TYPE *ObpProcessType;
extern const OBJECT_TYPE *ObpThreadType;
extern const OBJECT_TYPE *ObpDirectoryType;
extern const OBJECT_TYPE *ObpSymlinkType;
extern const OBJECT_TYPE *ObpEventType;
extern const OBJECT_TYPE *ObpMutexType;
extern const OBJECT_TYPE *ObpSemaphoreType;
extern const OBJECT_TYPE *ObpSectionType;
extern const OBJECT_TYPE *ObpTimerType;

/* --- API ----------------------------------------------------------------- */

/*
 * ob_create_type — register a new object type
 *
 * Copies *tmpl into the global type table.  Returns a pointer to the
 * internal copy, or NULL if the table is full (OB_MAX_TYPES reached).
 */
const OBJECT_TYPE *ob_create_type(const OBJECT_TYPE *tmpl);

/*
 * ob_alloc_object — allocate header + body as a single block
 *
 * Uses kmalloc for total sizes ≤ 4096, pmm_alloc_contiguous otherwise.
 * The block is zero-filled; ref_count is initialised to 1; type is set.
 * Returns a pointer to the body (not the header), or NULL on failure.
 */
void *ob_alloc_object(const OBJECT_TYPE *type);

/*
 * ObReferenceObject — increment the reference count on an object body
 */
void ObReferenceObject(void *body);

/*
 * ObDereferenceObject — decrement the reference count
 *
 * When the count reaches 0 and OB_FLAG_PERMANENT is not set:
 *   1. Calls type->on_delete(body) if non-NULL
 *   2. Frees the combined header+body allocation
 * Returns the new reference count (0 means the object was freed).
 */
int32_t ObDereferenceObject(void *body);

/*
 * ObReferenceObjectByPointer — validate type, then increment ref count
 *
 * Returns 0 on success, -1 if the object's type does not match the
 * expected type (body is not referenced in that case).
 */
int ObReferenceObjectByPointer(void *body, const OBJECT_TYPE *expected_type,
                               uint32_t access);

/*
 * ObMakeTemporaryObject — clear OB_FLAG_PERMANENT so the object can be
 * deleted when its reference count reaches 0.
 */
void ObMakeTemporaryObject(void *body);

/*
 * ob_init — initialise the Object Manager subsystem
 *
 * Registers all built-in type singletons.  Called during boot_phase2,
 * before registry_init.  Requires SUBSYS_HEAP.
 */
boot_result_t ob_init(void);

/* Pull in namespace API (directories, symlinks, path resolution) */
#include "kernel/ob/ob_ns.h"
