/* ============================================================================
 * ob_section.h -- Section (shared memory) object type for the Object Manager
 *
 * SECTION_OBJECT represents a mappable memory region backed by contiguous
 * physical pages.  Foundation for MapViewOfFile and cross-process shared
 * memory.  Named sections are inserted into \BaseNamedObjects.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/ob/handle_table.h"

/* Forward-declare task struct */
struct task;

/* Section protection flags */
#define SECTION_MAP_READ    0x0001
#define SECTION_MAP_WRITE   0x0002
#define SECTION_MAP_EXECUTE 0x0004
#define SECTION_ALL_ACCESS  (SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE)

/* Maximum concurrent view mappings per section */
#define SECTION_MAX_VIEWS   16

/* --- View tracking ------------------------------------------------------- */

typedef struct section_view {
    uint32_t  task_pid;    /* PID of the task holding this view */
    uintptr_t base_addr;   /* virtual address of the mapping */
    uint32_t  size;        /* mapped size in bytes */
    uint32_t  in_use;      /* 1 = active mapping */
} SECTION_VIEW;

/* --- SECTION_OBJECT body ------------------------------------------------- */

typedef struct section_object {
    uintptr_t    phys_base;    /* physical base address of backing pages */
    uint32_t     size;         /* total section size in bytes */
    uint32_t     page_count;   /* number of 4 KiB physical pages */
    uint32_t     protect;      /* protection flags */
    uint32_t     map_count;    /* number of active view mappings */
    SECTION_VIEW views[SECTION_MAX_VIEWS]; /* view tracking table */
} SECTION_OBJECT;

/* --- API ----------------------------------------------------------------- */

void ob_section_type_init(void);

/*
 * ObCreateSection -- allocate a section backed by contiguous physical pages.
 *
 * size:    section size in bytes (rounded up to page boundary).
 * protect: SECTION_MAP_* protection flags.
 * name:    NULL for unnamed, or a name in \BaseNamedObjects.
 *
 * Returns a HANDLE to the section, or INVALID_HANDLE_VALUE on failure.
 */
HANDLE ObCreateSection(HANDLE_TABLE *ht, uint32_t size, uint32_t protect,
                       const char *name);

/*
 * ObMapViewOfSection -- map section pages into a task's address space.
 *
 * Currently returns the identity-mapped physical address since all tasks
 * share the kernel address space.  When per-process address spaces are
 * fully implemented, this will create VMM page-table entries.
 *
 * section_handle: HANDLE to the section object.
 * Returns the virtual base address of the mapping, or 0 on failure.
 */
uintptr_t ObMapViewOfSection(HANDLE_TABLE *ht, HANDLE section_handle);

/*
 * ObUnmapViewOfSection -- remove a view mapping.
 *
 * section_handle: HANDLE to the section object.
 * base_address:   virtual address returned by ObMapViewOfSection.
 * Returns 0 on success, -1 on failure.
 */
int ObUnmapViewOfSection(HANDLE_TABLE *ht, HANDLE section_handle,
                         uintptr_t base_address);
