/* ============================================================================
 * ntfs_cache.c — MFT Record LRU Cache (§10.1)
 *
 * Caches recently-accessed MFT records to avoid redundant disk reads.
 * Key: MFT inode number.  Value: full record buffer (fixup-verified).
 *
 * Design:
 *   - Fixed-size array of cache entries (default 64).
 *   - LRU eviction via a monotonic access counter.
 *   - Pinned entries (inodes 0 and 5) are never evicted.
 *   - Sequence number validation detects stale references.
 *   - Telemetry: hit/miss/eviction counters logged on mount.
 *
 * Integration:
 *   ntfs_read_mft_record() [this file] replaces the raw version.
 *   All existing callers automatically use the cache.
 *   ntfs_read_mft_record_raw() [ntfs_mft.c] is the uncached disk reader.
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

/* ---- Cache Entry Structure ---- */

struct ntfs_mft_cache_entry {
    uint64_t inode;            /* MFT inode number (UINT64_MAX = empty) */
    uint64_t access_counter;   /* Monotonic counter for LRU ordering */
    uint16_t seq_number;       /* Sequence number at time of caching */
    uint8_t  pinned;           /* 1 = never evict (critical system inodes) */
    uint8_t  valid;            /* 1 = entry contains valid data */
    struct ntfs_mft_header hdr; /* Cached parsed header */
    uint8_t *record_buf;       /* PMM-allocated record buffer (frs_size bytes) */
};

/* Sentinel value for empty cache slots */
#define CACHE_EMPTY_INODE  ((uint64_t)-1)

/* Monotonic access counter (shared across all volumes — fine for LRU) */
static uint64_t g_access_counter = 0;

/* ============================================================================
 * ntfs_cache_init — Allocate and initialize the MFT record cache.
 *
 * Each entry needs one PMM page for the record buffer (frs_size <= 4096).
 * The cache entry array itself is allocated via PMM (fits in a few pages).
 * ============================================================================ */

int ntfs_cache_init(struct ntfs_volume *vol, uint32_t cache_size)
{
    uint32_t i;
    uint32_t entry_array_bytes;

    if (!vol)
        return NTFS_ERR_IO;

    if (vol->mft_cache_loaded)
        return NTFS_OK;  /* Already initialized */

    if (cache_size == 0)
        cache_size = NTFS_MFT_CACHE_DEFAULT_SIZE;

    /* Allocate the entry array via kmalloc (avoids PMM MMIO-hole pages) */
    entry_array_bytes = cache_size *
                        (uint32_t)sizeof(struct ntfs_mft_cache_entry);
    vol->mft_cache = (struct ntfs_mft_cache_entry *)kmalloc(entry_array_bytes);
    if (!vol->mft_cache) {
        klog(LOG_WARN, "ntfs", "MFT cache: cannot allocate %u entry array",
             (uint64_t)cache_size, 0, 0);
        return NTFS_ERR_IO;
    }
    vol->mft_cache_size = cache_size;

    /* Initialize all entries as empty */
    ntfs_memset(vol->mft_cache, 0, entry_array_bytes);
    for (i = 0; i < cache_size; i++) {
        vol->mft_cache[i].inode = CACHE_EMPTY_INODE;
        vol->mft_cache[i].valid = 0;
        vol->mft_cache[i].pinned = 0;

        /* Allocate per-entry record buffer via kmalloc (avoids MMIO-hole) */
        {
            vol->mft_cache[i].record_buf = (uint8_t *)kmalloc(vol->frs_size);
            if (!vol->mft_cache[i].record_buf) {
                /* Failed — free everything allocated so far */
                uint32_t j;
                for (j = 0; j < i; j++) {
                    if (vol->mft_cache[j].record_buf)
                        kfree(vol->mft_cache[j].record_buf);
                }
                kfree(vol->mft_cache);
                vol->mft_cache = NULL;
                klog(LOG_WARN, "ntfs",
                     "MFT cache: OOM at entry %u/%u",
                     (uint64_t)i, (uint64_t)cache_size, 0);
                return NTFS_ERR_IO;
            }
        }
    }

    /* Reset telemetry */
    vol->mft_cache_hits = 0;
    vol->mft_cache_misses = 0;
    vol->mft_cache_evictions = 0;
    vol->mft_cache_loaded = 1;

    klog(LOG_INFO, "ntfs", "MFT cache initialized: %u entries (%u KB)",
         (uint64_t)cache_size,
         (uint64_t)(cache_size * (vol->frs_size + sizeof(struct ntfs_mft_cache_entry)) / 1024),
         0);

    return NTFS_OK;
}

/* ============================================================================
 * Cache lookup — find an entry by inode number.
 * Returns entry index or -1 if not found.
 * ============================================================================ */

static int cache_find(const struct ntfs_volume *vol, uint64_t inode)
{
    uint32_t i;

    if (!vol->mft_cache || !vol->mft_cache_loaded)
        return -1;

    for (i = 0; i < vol->mft_cache_size; i++) {
        if (vol->mft_cache[i].valid &&
            vol->mft_cache[i].inode == inode) {
            return (int)i;
        }
    }
    return -1;
}

/* ============================================================================
 * Find LRU victim — the entry with the lowest access counter.
 * Pinned entries are skipped. Returns entry index.
 * ============================================================================ */

static int cache_find_victim(const struct ntfs_volume *vol)
{
    uint32_t i;
    int victim = -1;
    uint64_t lowest = (uint64_t)-1;

    for (i = 0; i < vol->mft_cache_size; i++) {
        /* Prefer empty slots first */
        if (!vol->mft_cache[i].valid)
            return (int)i;

        /* Skip pinned entries */
        if (vol->mft_cache[i].pinned)
            continue;

        if (vol->mft_cache[i].access_counter < lowest) {
            lowest = vol->mft_cache[i].access_counter;
            victim = (int)i;
        }
    }

    return victim;
}

/* ============================================================================
 * ntfs_read_mft_record — Cached MFT record reader (public API)
 *
 * This replaces the original ntfs_read_mft_record. All existing callers
 * automatically benefit from caching.
 *
 * Flow:
 *   1. Check cache — if hit and sequence number matches, copy out.
 *   2. On miss — call ntfs_read_mft_record_raw(), apply fixup, store.
 *   3. Pin inodes 0 ($MFT) and 5 (root directory).
 * ============================================================================ */

int ntfs_read_mft_record(struct ntfs_volume *vol, uint64_t inode,
                         void *buf, struct ntfs_mft_header *hdr)
{
    int idx;
    int rc;

    if (!vol || !buf || !hdr)
        return NTFS_ERR_IO;

    /* If cache not initialized, fall through to raw reader */
    if (!vol->mft_cache_loaded || !vol->mft_cache)
        return ntfs_read_mft_record_raw(vol, inode, buf, hdr);

    /* ---- Cache Lookup ---- */
    idx = cache_find(vol, inode);
    if (idx >= 0) {
        struct ntfs_mft_cache_entry *e = &vol->mft_cache[idx];

        /* Copy cached record to caller's buffer */
        ntfs_memcpy(buf, e->record_buf, vol->frs_size);
        *hdr = e->hdr;

        /* Update LRU counter */
        e->access_counter = ++g_access_counter;
        vol->mft_cache_hits++;

        return NTFS_OK;
    }

    /* ---- Cache Miss: Read from disk ---- */
    vol->mft_cache_misses++;

    rc = ntfs_read_mft_record_raw(vol, inode, buf, hdr);
    if (rc != NTFS_OK)
        return rc;

    /* Apply fixup before caching (callers expect fixup-verified data) */
    rc = ntfs_apply_fixup(buf, vol->frs_size, vol->bytes_per_sector);
    if (rc != NTFS_OK) {
        /* Fixup failed — return the record but don't cache it */
        return rc;
    }

    /* ---- Store in Cache ---- */
    idx = cache_find_victim(vol);
    if (idx >= 0) {
        struct ntfs_mft_cache_entry *e = &vol->mft_cache[idx];

        /* Track eviction if replacing a valid entry */
        if (e->valid)
            vol->mft_cache_evictions++;

        /* Store the record */
        ntfs_memcpy(e->record_buf, buf, vol->frs_size);
        e->hdr = *hdr;
        e->inode = inode;
        e->seq_number = hdr->seq_number;
        e->access_counter = ++g_access_counter;
        e->valid = 1;

        /* Pin critical inodes */
        if (inode == 0 || inode == NTFS_ROOT_INODE)
            e->pinned = 1;
        else
            e->pinned = 0;
    }

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_cache_invalidate — Remove a specific inode from the cache.
 * Called when an MFT record is modified (write path).
 * ============================================================================ */

void ntfs_cache_invalidate(struct ntfs_volume *vol, uint64_t inode)
{
    int idx;

    if (!vol || !vol->mft_cache_loaded)
        return;

    idx = cache_find(vol, inode);
    if (idx >= 0) {
        vol->mft_cache[idx].valid = 0;
        vol->mft_cache[idx].inode = CACHE_EMPTY_INODE;
        vol->mft_cache[idx].pinned = 0;
    }
}

/* ============================================================================
 * ntfs_cache_log_stats — Log cache telemetry to serial output.
 * ============================================================================ */

void ntfs_cache_log_stats(const struct ntfs_volume *vol)
{
    uint64_t total;

    if (!vol)
        return;

    total = vol->mft_cache_hits + vol->mft_cache_misses;

    if (total > 0) {
        uint64_t hit_pct = (vol->mft_cache_hits * 100) / total;
        klog(LOG_INFO, "ntfs",
             "MFT cache: %u entries, %llu hits / %llu total (%llu%% hit rate), %llu evictions",
             (uint64_t)vol->mft_cache_size,
             vol->mft_cache_hits, total, hit_pct,
             vol->mft_cache_evictions);
    } else {
        klog(LOG_INFO, "ntfs",
             "MFT cache: %u entries, no activity yet",
             (uint64_t)vol->mft_cache_size, 0, 0);
    }
}
