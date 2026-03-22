/* ============================================================================
 * blk_prefetch.c — Predictive Sequential Read-Ahead
 *
 * §17.1 — 🚀 Impossible OS Exclusive
 *
 * Detects sequential read patterns and speculatively prefetches ahead.
 * Neither Windows viostor nor Linux virtio-blk implement driver-level
 * read-ahead — they rely on the filesystem/block layer above.
 *
 * Design:
 *   - Track last 4 read (sector, count) pairs in a ring buffer
 *   - When 3+ consecutive reads are sequential, prefetch ahead
 *   - Prefetch buffer is PMM-allocated (default 128 KB = 256 sectors)
 *   - On read hit: copy from buffer (sub-microsecond latency)
 *   - On pattern break: discard buffer, reset history
 *   - Auto-disable during random I/O (miss rate > 80%)
 * ============================================================================ */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Prefetch configuration ---- */
#define PREFETCH_HISTORY_SIZE   4     /* Ring buffer depth */
#define PREFETCH_DEFAULT_SECTORS 256  /* 128 KB default prefetch */
#define PREFETCH_MIN_SEQUENTIAL  3    /* Consecutive sequential reads to trigger */
#define PREFETCH_MISS_RATE_LIMIT 80   /* Auto-disable above this miss % */

/* ---- Prefetch state ---- */

/* Read history entry */
struct read_history {
    uint64_t sector;    /* Starting LBA */
    uint32_t count;     /* Number of sectors */
};

/* Prefetch buffer state */
struct prefetch_state {
    /* Configuration */
    int      enabled;           /* Master enable */
    uint32_t prefetch_sectors;  /* How many sectors to prefetch */
    uint32_t min_sequential;    /* Sequential reads before prefetch */

    /* History ring */
    struct read_history history[PREFETCH_HISTORY_SIZE];
    uint32_t history_idx;       /* Next write position (0..3) */
    uint32_t history_count;     /* Number of valid entries */

    /* Prefetch buffer */
    void    *buffer;            /* PMM-allocated prefetch data */
    uint64_t buf_sector;        /* Starting sector of buffered data */
    uint32_t buf_count;         /* Number of valid sectors in buffer */
    int      buf_valid;         /* 1 if buffer contains valid data */
    uint32_t buf_pages;         /* PMM pages allocated for buffer */

    /* Statistics */
    uint32_t total_reads;       /* All read operations */
    uint32_t prefetch_hits;     /* Reads served from prefetch buffer */
    uint32_t prefetch_misses;   /* Reads that missed the buffer */
    uint32_t prefetches_issued; /* Speculative reads issued */

    /* Auto-disable tracking */
    uint32_t window_reads;      /* Reads in current window */
    uint32_t window_hits;       /* Hits in current window */
    int      auto_disabled;     /* 1 if disabled due to high miss rate */
};

static struct prefetch_state pf;

/* ---- Internal helpers ---- */

/* Add a read to the history ring */
static void pf_record_history(uint64_t sector, uint32_t count)
{
    pf.history[pf.history_idx].sector = sector;
    pf.history[pf.history_idx].count  = count;
    pf.history_idx = (pf.history_idx + 1) % PREFETCH_HISTORY_SIZE;
    if (pf.history_count < PREFETCH_HISTORY_SIZE)
        pf.history_count++;
}

/* Check if the last N reads form a sequential pattern.
 * Returns the next expected sector if sequential, 0 if not. */
static uint64_t pf_detect_sequential(void)
{
    uint32_t i, seq_count;
    uint32_t oldest_idx;
    struct read_history *prev, *curr;

    if (pf.history_count < pf.min_sequential)
        return 0;

    /* Walk history from oldest to newest checking continuity */
    seq_count = 0;
    oldest_idx = (pf.history_idx + PREFETCH_HISTORY_SIZE - pf.history_count)
                 % PREFETCH_HISTORY_SIZE;

    for (i = 0; i < pf.history_count - 1; i++) {
        uint32_t ci = (oldest_idx + i)     % PREFETCH_HISTORY_SIZE;
        uint32_t ni = (oldest_idx + i + 1) % PREFETCH_HISTORY_SIZE;

        prev = &pf.history[ci];
        curr = &pf.history[ni];

        if (prev->sector + prev->count == curr->sector) {
            seq_count++;
        } else {
            seq_count = 0;  /* Break in pattern — reset counter */
        }
    }

    /* Need min_sequential - 1 consecutive sequential pairs
     * (3 sequential reads = 2 sequential pairs) */
    if (seq_count >= pf.min_sequential - 1) {
        /* Return the next expected sector */
        uint32_t last_idx = (pf.history_idx + PREFETCH_HISTORY_SIZE - 1)
                            % PREFETCH_HISTORY_SIZE;
        return pf.history[last_idx].sector + pf.history[last_idx].count;
    }

    return 0;  /* Not sequential */
}

/* Discard prefetch buffer */
static void pf_discard_buffer(void)
{
    pf.buf_valid = 0;
    pf.buf_sector = 0;
    pf.buf_count = 0;
}

/* Issue a speculative prefetch read.
 * This is a synchronous device read into the prefetch buffer.
 * Future: could be made asynchronous with a background thread. */
static void pf_issue_prefetch(uint64_t start_sector)
{
    uint32_t sectors_to_read;
    int ret;

    if (!pf.buffer || !pf.enabled)
        return;

    /* Don't prefetch past device capacity */
    if (start_sector >= disk_capacity)
        return;

    sectors_to_read = pf.prefetch_sectors;
    if (start_sector + sectors_to_read > disk_capacity)
        sectors_to_read = (uint32_t)(disk_capacity - start_sector);

    if (sectors_to_read == 0)
        return;

    /* Issue the read directly via do_io */
    ret = virtio_blk_do_io(VIRTIO_BLK_T_IN, start_sector,
                           sectors_to_read * topo.blk_size, pf.buffer);

    if (ret == VIRTIO_IO_OK) {
        pf.buf_sector = start_sector;
        pf.buf_count  = sectors_to_read;
        pf.buf_valid  = 1;
        pf.prefetches_issued++;
    } else {
        pf_discard_buffer();
    }
}

/* Check auto-disable: if miss rate > threshold over a 100-read window,
 * stop prefetching to avoid wasting bandwidth on random workloads. */
static void pf_check_auto_disable(void)
{
    uint32_t miss_rate;

    pf.window_reads++;

    if (pf.window_reads < 100)
        return;  /* Not enough data yet */

    if (pf.window_reads > 0) {
        miss_rate = 100 - (pf.window_hits * 100 / pf.window_reads);
    } else {
        miss_rate = 100;
    }

    if (miss_rate > PREFETCH_MISS_RATE_LIMIT) {
        if (!pf.auto_disabled) {
            pf.auto_disabled = 1;
            pf_discard_buffer();
            klog(LOG_DEBUG, "virtio",
                   "Prefetch: auto-disabled (miss rate %u%% > %u%%)",
                   (uint64_t)miss_rate,
                   (uint64_t)PREFETCH_MISS_RATE_LIMIT);
        }
    } else {
        if (pf.auto_disabled) {
            pf.auto_disabled = 0;
            klog(LOG_DEBUG, "virtio",
                   "Prefetch: re-enabled (miss rate %u%%)",
                   (uint64_t)miss_rate);
        }
    }

    /* Reset window */
    pf.window_reads = 0;
    pf.window_hits  = 0;
}

/* ---- Public API ---- */

/* Initialize prefetch subsystem. Called from virtio_blk_init(). */
void prefetch_init(void)
{
    uint32_t size_kb;
    uint32_t i;

    /* Zero everything */
    pf.enabled = 0;
    pf.prefetch_sectors = PREFETCH_DEFAULT_SECTORS;
    pf.min_sequential = PREFETCH_MIN_SEQUENTIAL;
    pf.history_idx = 0;
    pf.history_count = 0;
    pf.buf_valid = 0;
    pf.buf_sector = 0;
    pf.buf_count = 0;
    pf.buffer = 0;
    pf.total_reads = 0;
    pf.prefetch_hits = 0;
    pf.prefetch_misses = 0;
    pf.prefetches_issued = 0;
    pf.window_reads = 0;
    pf.window_hits = 0;
    pf.auto_disabled = 0;
    for (i = 0; i < PREFETCH_HISTORY_SIZE; i++) {
        pf.history[i].sector = 0;
        pf.history[i].count = 0;
    }

    /* Read configuration from Registry */
    {
        HKEY hKey = (HKEY)0;
        uint32_t val;
        int reg_enabled = 1;

        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\Prefetch", 0,
                KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegGetDword(hKey, "Enabled", &val) == ERROR_SUCCESS)
                reg_enabled = (int)val;
            if (RegGetDword(hKey, "SizeKB", &val) == ERROR_SUCCESS)
                size_kb = val;
            if (RegGetDword(hKey, "MinSequential", &val) == ERROR_SUCCESS)
                pf.min_sequential = val;
            RegCloseKey(hKey);
        }

        if (!reg_enabled) {
            klog(LOG_DEBUG, "virtio", "Prefetch: disabled by Registry");
            return;
        }
    }

    if (size_kb < 4)   size_kb = 4;     /* Minimum 4 KB */
    if (size_kb > 4096) size_kb = 4096; /* Maximum 4 MB */
    pf.prefetch_sectors = (size_kb * 1024) / topo.blk_size;

    if (pf.min_sequential < 2) pf.min_sequential = 2;
    if (pf.min_sequential > PREFETCH_HISTORY_SIZE)
        pf.min_sequential = PREFETCH_HISTORY_SIZE;

    /* Allocate prefetch buffer via PMM (large allocation) */
    uint32_t pages_needed = (size_kb * 1024 + 4095) / 4096;
    uintptr_t buf_phys = pmm_alloc_contiguous(pages_needed);
    if (!buf_phys) {
        klog(LOG_DEBUG, "virtio",
               "Prefetch: failed to allocate %u KB buffer", (uint64_t)size_kb);
        return;
    }
    pf.buffer = (void *)buf_phys;
    pf.buf_pages = pages_needed;

    pf.enabled = 1;
    klog(LOG_DEBUG, "virtio",
           "Prefetch: enabled (size=%uKB sectors=%u min_seq=%u)",
           (uint64_t)size_kb, (uint64_t)pf.prefetch_sectors,
           (uint64_t)pf.min_sequential);

    /* Write default config to Registry for visibility */
    {
        HKEY hKey = (HKEY)0;
        uint32_t disp;

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\Prefetch", 0,
                (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                &hKey, &disp) == ERROR_SUCCESS) {
            RegSetDword(hKey, "Enabled", 1);
            RegSetDword(hKey, "SizeKB", size_kb);
            RegSetDword(hKey, "MinSequential", pf.min_sequential);
            RegCloseKey(hKey);
        }
    }
}

/* Shut down prefetch. Called from virtio_blk_shutdown(). */
void prefetch_shutdown(void)
{
    if (pf.buffer) {
        uint32_t i;
        uintptr_t base = (uintptr_t)pf.buffer;
        for (i = 0; i < pf.buf_pages; i++)
            pmm_free_frame(base + i * 4096);
        pf.buffer = 0;
        pf.buf_pages = 0;
    }
    pf.enabled = 0;
    pf.buf_valid = 0;
}

/* Try to serve a read from the prefetch buffer.
 * Returns 1 if hit (data copied to buffer), 0 if miss. */
int prefetch_try_read(uint64_t sector, uint32_t count, void *buffer)
{
    uint64_t req_end, buf_end;
    uint64_t offset_sectors;
    uint8_t *src;

    pf.total_reads++;

    if (!pf.enabled || !pf.buf_valid || pf.auto_disabled)
        return 0;

    /* Check if requested range falls entirely within prefetch buffer */
    req_end = sector + count;
    buf_end = pf.buf_sector + pf.buf_count;

    if (sector >= pf.buf_sector && req_end <= buf_end) {
        /* HIT — copy from prefetch buffer */
        offset_sectors = sector - pf.buf_sector;
        src = (uint8_t *)pf.buffer + (offset_sectors * topo.blk_size);

        /* Use byte-by-byte copy (no memcpy in freestanding kernel) */
        {
            uint8_t *d = (uint8_t *)buffer;
            uint32_t bytes = count * topo.blk_size;
            uint32_t i;
            for (i = 0; i < bytes; i++)
                d[i] = src[i];
        }

        pf.prefetch_hits++;
        pf.window_hits++;
        return 1;  /* Hit */
    }

    pf.prefetch_misses++;
    return 0;  /* Miss */
}

/* Record a completed read and potentially trigger prefetch.
 * Called after every successful read from virtio_blk_read(). */
void prefetch_after_read(uint64_t sector, uint32_t count)
{
    uint64_t next_sector;

    if (!pf.enabled || pf.auto_disabled)
        return;

    pf_check_auto_disable();

    /* Record in history */
    pf_record_history(sector, count);

    /* Check for sequential pattern */
    next_sector = pf_detect_sequential();
    if (next_sector == 0)
        return;  /* Not sequential */

    /* Don't re-prefetch if buffer already covers this range */
    if (pf.buf_valid && pf.buf_sector == next_sector)
        return;

    /* Prefetch ahead */
    pf_issue_prefetch(next_sector);
}

/* Invalidate prefetch buffer on write (data coherence).
 * Called from virtio_blk_write(). */
void prefetch_invalidate(uint64_t sector, uint32_t count)
{
    uint64_t write_end, buf_end;

    if (!pf.buf_valid)
        return;

    write_end = sector + count;
    buf_end   = pf.buf_sector + pf.buf_count;

    /* If write overlaps prefetch buffer, discard it */
    if (sector < buf_end && write_end > pf.buf_sector) {
        pf_discard_buffer();
    }
}

/* Expose prefetch stats to Registry. Called from virtio_blk_init(). */
void prefetch_expose_registry(void)
{
    HKEY hKey = (HKEY)0;
    uint32_t disp;
    uint32_t hit_rate;

    if (pf.total_reads > 0)
        hit_rate = pf.prefetch_hits * 100 / pf.total_reads;
    else
        hit_rate = 0;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "HARDWARE\\VirtIO\\Block0\\Prefetch", 0,
            (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
            &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "Enabled", pf.enabled ? 1 : 0);
        RegSetDword(hKey, "HitRate", hit_rate);
        RegSetDword(hKey, "Hits", pf.prefetch_hits);
        RegSetDword(hKey, "Misses", pf.prefetch_misses);
        RegSetDword(hKey, "TotalReads", pf.total_reads);
        RegSetDword(hKey, "PrefetchesIssued", pf.prefetches_issued);
        RegSetDword(hKey, "AutoDisabled", pf.auto_disabled ? 1 : 0);
        RegCloseKey(hKey);
    }
}
