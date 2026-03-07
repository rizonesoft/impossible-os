/* ============================================================================
 * shmem.h — Named shared memory regions
 *
 * Allows multiple tasks to share a memory region by name.
 * The first task calls shmem_create() to allocate and name the region.
 * Other tasks call shmem_map() to get a pointer to the same backing memory.
 * shmem_unmap() decrements the reference count; when it reaches 0, the
 * region is freed.
 *
 * Current design: single address space (all tasks share the kernel page
 * tables), so "mapping" simply returns the kernel virtual address.
 * When per-process page tables are added, shmem_map() will create
 * the appropriate page mappings.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum number of named shared memory regions */
#define SHMEM_MAX       16

/* Maximum name length */
#define SHMEM_NAME_MAX  32

/* Shared memory region descriptor */
typedef struct shmem_region {
    char        name[SHMEM_NAME_MAX]; /* region name */
    void       *base;                 /* kernel virtual address of backing memory */
    uint32_t    size;                 /* size in bytes */
    uint32_t    refcount;             /* number of tasks using this region */
    uint32_t    in_use;               /* 1 = slot is allocated */
} shmem_region_t;

/* --- API --- */

/* Create a named shared memory region.
 * Returns the region ID (>= 0) on success, -1 on failure. */
int shmem_create(const char *name, uint32_t size);

/* Look up a shared memory region by name.
 * Returns the region ID (>= 0) if found, -1 if not found. */
int shmem_open(const char *name);

/* Map a shared memory region into the calling task.
 * Returns a pointer to the shared memory, or NULL on failure. */
void *shmem_map(int id);

/* Unmap a shared memory region from the calling task.
 * Decrements reference count; frees when it reaches 0. */
void shmem_unmap(int id);

/* Initialize the shared memory subsystem. */
void shmem_init(void);
