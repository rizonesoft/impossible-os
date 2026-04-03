/* ============================================================================
 * mmap.h -- Memory-mapped file support
 *
 * Maps files (or anonymous memory) into the virtual address space.
 * Pages are loaded on demand from the backing file via the page fault handler.
 *
 * Supported flags:
 *   MAP_PRIVATE  -- copy-on-write: writes go to a private copy, not the file
 *   MAP_SHARED   -- writes are visible to other mappings and flushed to disk
 *   MAP_ANON     -- anonymous mapping (no file, zero-filled pages)
 *
 * Protection:
 *   PROT_READ    -- pages are readable
 *   PROT_WRITE   -- pages are writable
 *   PROT_EXEC    -- pages are executable (no NX bit)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Protection flags */
#define PROT_NONE   0x00
#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04

/* Mapping flags */
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20      /* anonymous mapping (no file) */
#define MAP_FIXED   0x10      /* interpret addr exactly */

/* Error return from mmap */
#define MAP_FAILED  ((void *)(uintptr_t)-1)

/* Maximum concurrent mmap regions */
#define MMAP_MAX_REGIONS  64

/* mmap region descriptor */
typedef struct mmap_region {
    uintptr_t       base;       /* virtual base address (page-aligned) */
    uint64_t        length;     /* mapping length in bytes */
    uint32_t        prot;       /* PROT_READ | PROT_WRITE | PROT_EXEC */
    uint32_t        flags;      /* MAP_SHARED | MAP_PRIVATE | MAP_ANON */
    struct vfs_node *file;      /* backing VFS file (NULL for MAP_ANON) */
    uint32_t        offset;     /* file offset of mapping start */
    uint32_t        in_use;     /* 1 = region is allocated */
} mmap_region_t;

/* --- API --- */

/* Initialize the mmap subsystem. */
void mmap_init(void);

/* Map a file (or anonymous memory) into the address space.
 * addr: hint address (0 = kernel picks), length: mapping size,
 * prot: protection flags, flags: MAP_SHARED / MAP_PRIVATE / MAP_ANON,
 * file: open VFS node (NULL for MAP_ANON), offset: file offset.
 * Returns the mapped virtual address, or MAP_FAILED on error. */
void *mmap(void *addr, uint64_t length, uint32_t prot, uint32_t flags,
           struct vfs_node *file, uint32_t offset);

/* Unmap a previously mapped region.
 * Returns 0 on success, -1 on error. */
int munmap(void *addr, uint64_t length);

/* Flush dirty pages in a shared mapping back to the file.
 * Returns 0 on success, -1 on error. */
int msync(void *addr, uint64_t length);

/* Handle a page fault for mmap'd regions.
 * Called from the page fault handler.
 * Returns 1 if handled (page loaded), 0 if not an mmap fault. */
int mmap_handle_fault(uintptr_t fault_addr, uint64_t error_code);
