/* ============================================================================
 * symtab.c -- Kernel symbol table loader and resolver
 *
 * Loads a packed binary symbol table (KSYM format) from the boot filesystem
 * and provides O(log n) address→name resolution via binary search.
 *
 * Binary format (produced by tools/convert_symmap.py):
 *   Header:  "KSYM" (4 bytes) + entry_count (uint32 LE)
 *   Entries: address (uint64 LE) + name (32 bytes, null-padded)
 * ============================================================================ */

#include "kernel/symtab.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/types.h"

#define SYMTAB_MAGIC     0x4D59534B  /* "KSYM" in little-endian */
#define SYMTAB_NAME_LEN  32
#define SYMTAB_PATH      "C:\\Impossible\\System\\kernel.sym"

/* Packed entry: 8 bytes address + 32 bytes name = 40 bytes */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    char     name[SYMTAB_NAME_LEN];
} symtab_entry_t;

/* Header: 4 bytes magic + 4 bytes count = 8 bytes */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t count;
} symtab_header_t;

/* Global state */
static symtab_entry_t *g_entries  = NULL;
static uint32_t        g_count    = 0;
static int             g_loaded   = 0;

void symtab_init(void)
{
    struct vfs_node *f;
    symtab_header_t hdr;
    int rc;
    uint32_t data_size;
    uint32_t pages;

    f = vfs_open(SYMTAB_PATH, 0);
    if (!f) {
        klog(LOG_WARN, "SYMTAB", "kernel.sym not found at %s", SYMTAB_PATH);
        return;
    }

    /* Read header */
    rc = vfs_read(f, 0, sizeof(hdr), (uint8_t *)&hdr);
    if (rc < (int)sizeof(hdr)) {
        klog(LOG_ERROR, "SYMTAB", "Failed to read header (%d bytes)", (int64_t)rc);
        vfs_close(f);
        return;
    }

    if (hdr.magic != SYMTAB_MAGIC) {
        klog(LOG_ERROR, "SYMTAB", "Bad magic: 0x%08X (expected 0x%08X)",
                  hdr.magic, SYMTAB_MAGIC);
        vfs_close(f);
        return;
    }

    if (hdr.count == 0 || hdr.count > 100000) {
        klog(LOG_ERROR, "SYMTAB", "Invalid symbol count: %u", hdr.count);
        vfs_close(f);
        return;
    }

    /* Allocate memory for entries using PMM (may be large) */
    data_size = hdr.count * sizeof(symtab_entry_t);
    pages = (data_size + 4095) / 4096;
    g_entries = (symtab_entry_t *)pmm_alloc_contiguous(pages);
    if (!g_entries) {
        klog(LOG_ERROR, "SYMTAB", "Failed to allocate %u pages for %u symbols",
                  pages, hdr.count);
        vfs_close(f);
        return;
    }

    /* Read all entries */
    rc = vfs_read(f, sizeof(hdr), data_size, (uint8_t *)g_entries);
    vfs_close(f);

    if (rc < (int)data_size) {
        klog(LOG_ERROR, "SYMTAB", "Short read: got %d, expected %u", (int64_t)rc,
             (uint64_t)data_size);
        /* Free PMM pages */
        for (uint32_t i = 0; i < pages; i++) {
            pmm_free_frame((uintptr_t)g_entries + i * 4096);
        }
        g_entries = NULL;
        return;
    }

    g_count  = hdr.count;
    g_loaded = 1;
    klog(LOG_INFO, "SYMTAB", "Symbol map loaded: %u symbols (%u KB)",
             g_count, data_size / 1024);
}

const char *symtab_resolve(uint64_t addr, uint64_t *offset)
{
    uint32_t lo, hi, mid;

    if (!g_loaded || !g_entries || g_count == 0)
        return NULL;

    /* Binary search: find the largest entry address <= addr */
    lo = 0;
    hi = g_count;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (g_entries[mid].addr <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo == 0)
        return NULL;  /* addr is before the first symbol */

    lo--;  /* lo now points to the symbol containing addr */

    if (offset)
        *offset = addr - g_entries[lo].addr;

    return g_entries[lo].name;
}

uint32_t symtab_count(void)
{
    return g_count;
}
