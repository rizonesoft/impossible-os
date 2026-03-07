/* ============================================================================
 * shmem.c — Named shared memory regions
 *
 * Global table of named memory regions backed by kmalloc.
 * Reference counted — freed when the last user unmaps.
 * ============================================================================ */

#include "kernel/ipc/shmem.h"
#include "kernel/mm/heap.h"
#include "kernel/printk.h"

/* Global shared memory table */
static shmem_region_t regions[SHMEM_MAX];
static uint32_t shmem_inited = 0;

/* Simple string helpers (no libc in freestanding) */
static int shmem_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

static void shmem_strncpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

void shmem_init(void)
{
    uint32_t i;
    for (i = 0; i < SHMEM_MAX; i++) {
        regions[i].in_use = 0;
        regions[i].refcount = 0;
        regions[i].base = (void *)0;
        regions[i].size = 0;
    }
    shmem_inited = 1;
}

int shmem_create(const char *name, uint32_t size)
{
    uint32_t i;
    void *mem;

    if (!shmem_inited)
        shmem_init();

    if (!name || size == 0)
        return -1;

    /* Check if name already exists */
    for (i = 0; i < SHMEM_MAX; i++) {
        if (regions[i].in_use && shmem_strcmp(regions[i].name, name) == 0) {
            printk("[SHMEM] Region \"%s\" already exists (id=%u)\n",
                   name, (uint64_t)i);
            return (int)i;  /* return existing region */
        }
    }

    /* Find free slot */
    for (i = 0; i < SHMEM_MAX; i++) {
        if (!regions[i].in_use)
            break;
    }

    if (i >= SHMEM_MAX) {
        printk("[SHMEM] No free region slots\n");
        return -1;
    }

    /* Allocate backing memory */
    mem = kmalloc(size);
    if (!mem) {
        printk("[SHMEM] Failed to allocate %u bytes\n", (uint64_t)size);
        return -1;
    }

    /* Zero the memory */
    {
        uint8_t *p = (uint8_t *)mem;
        uint32_t j;
        for (j = 0; j < size; j++)
            p[j] = 0;
    }

    /* Initialize the region */
    shmem_strncpy(regions[i].name, name, SHMEM_NAME_MAX);
    regions[i].base = mem;
    regions[i].size = size;
    regions[i].refcount = 1;  /* creator counts as first user */
    regions[i].in_use = 1;

    printk("[OK] SHMEM \"%s\" created (id=%u, %u bytes at 0x%x)\n",
           name, (uint64_t)i, (uint64_t)size, (uint64_t)(uintptr_t)mem);

    return (int)i;
}

int shmem_open(const char *name)
{
    uint32_t i;

    if (!shmem_inited)
        shmem_init();

    if (!name)
        return -1;

    for (i = 0; i < SHMEM_MAX; i++) {
        if (regions[i].in_use && shmem_strcmp(regions[i].name, name) == 0)
            return (int)i;
    }

    return -1;  /* not found */
}

void *shmem_map(int id)
{
    if (id < 0 || id >= (int)SHMEM_MAX)
        return (void *)0;

    if (!regions[id].in_use)
        return (void *)0;

    /* Increment reference count */
    regions[id].refcount++;

    return regions[id].base;
}

void shmem_unmap(int id)
{
    if (id < 0 || id >= (int)SHMEM_MAX)
        return;

    if (!regions[id].in_use)
        return;

    if (regions[id].refcount > 0)
        regions[id].refcount--;

    /* Free if no more users */
    if (regions[id].refcount == 0) {
        printk("[SHMEM] Region \"%s\" (id=%u) freed\n",
               regions[id].name, (uint64_t)id);
        kfree(regions[id].base);
        regions[id].base = (void *)0;
        regions[id].size = 0;
        regions[id].in_use = 0;
        regions[id].name[0] = '\0';
    }
}
