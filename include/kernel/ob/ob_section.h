/* ============================================================================
 * ob_section.h -- Section (shared memory) object type for the Object Manager
 *
 * SECTION_OBJECT represents a mappable memory region backed by contiguous
 * physical pages.  Foundation for MapViewOfFile and cross-process shared
 * memory.  Named sections are inserted into \BaseNamedObjects.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/sched/spinlock.h"
#include "kernel/ob/handle_table.h"

/* Forward-declare task struct */
struct task;
struct vfs_node;

/* Section allocation attributes (Windows-compatible subset) */
#define SEC_IMAGE       0x01000000u
#define SEC_RESERVE     0x04000000u
#define SEC_COMMIT      0x08000000u
#define SEC_NOCACHE     0x10000000u

/* Section protection flags (handle / map rights, legacy) */
#define SECTION_MAP_READ    0x0001
#define SECTION_MAP_WRITE   0x0002
#define SECTION_MAP_EXECUTE 0x0004
#define SECTION_ALL_ACCESS  (SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE)

/* Maximum concurrent view mappings per section */
#define SECTION_MAX_VIEWS   16

#define SECTION_BACKING_PATH  256

/* --- View tracking ------------------------------------------------------- */

typedef struct section_view {
    uint32_t  task_pid;       /* PID of the task holding this view */
    uintptr_t base_addr;      /* virtual address of the mapping */
    uint64_t  section_offset; /* byte offset within section object */
    uint32_t  view_bytes;     /* bytes mapped (subset of section) */
    uint32_t  in_use;         /* 1 = active mapping */
} SECTION_VIEW;

/* --- SECTION_OBJECT body ------------------------------------------------- */

typedef struct section_object {
    spinlock_t        lk;           /* protects views, map_count, phys, size */
    uintptr_t         phys_base;    /* physical base address of backing pages */
    uint32_t          size;         /* total section size in bytes */
    uint32_t          page_count;   /* number of 4 KiB physical pages */
    uint32_t          protect;      /* SECTION_MAP_* granted at create */
    uint32_t          section_page_protect; /* PAGE_* at create */
    uint32_t          allocation_attributes; /* SEC_* */
    uint32_t          map_count;    /* number of active view mappings */
    char              backing_path[SECTION_BACKING_PATH];
    SECTION_VIEW      views[SECTION_MAX_VIEWS];
} SECTION_OBJECT;

/* --- API ----------------------------------------------------------------- */

void ob_section_type_init(void);

HANDLE ObCreateSection(HANDLE_TABLE *ht, uint32_t size, uint32_t protect,
                       const char *name);

/*
 * ObCreateSectionEx -- full section create (anonymous, pagefile-backed, or
 * file-backed).  max_size_bytes is MaximumSize.QuadPart (must be non-zero
 * for anonymous sections).  file_handle INVALID_HANDLE_VALUE or 0 for none.
 */
HANDLE ObCreateSectionEx(HANDLE_TABLE *ht, uint64_t max_size_bytes,
                         uint32_t section_page_protect,
                         uint32_t allocation_attributes, HANDLE file_handle,
                         const char *name, uint32_t section_desired_access);

HANDLE ObOpenSection(HANDLE_TABLE *ht, uint32_t desired_access,
                     const char *name);

uintptr_t ObMapViewOfSection(HANDLE_TABLE *ht, HANDLE section_handle,
                             uint32_t view_owner_pid);

uintptr_t ObMapViewOfSectionFull(HANDLE_TABLE *ht, HANDLE section_handle,
                                 uint32_t view_owner_pid,
                                 uint64_t section_offset_bytes,
                                 uint32_t view_size_bytes);

int ObUnmapViewOfSection(HANDLE_TABLE *ht, HANDLE section_handle,
                         uintptr_t base_address);

int ObUnmapViewOfSectionByBase(HANDLE_TABLE *ht, uint32_t view_owner_pid,
                               uintptr_t base_address);

NTSTATUS ObExtendSectionObject(SECTION_OBJECT *so, uint64_t new_max_bytes);

NTSTATUS ObQuerySectionObject(SECTION_OBJECT *so, uint32_t info_class,
                             void *buffer, uint64_t buffer_length,
                             uint64_t *return_length);

int ObAreMappedFilesTheSame(HANDLE_TABLE *ht, uint32_t pid,
                            uintptr_t addr1, uintptr_t addr2);
