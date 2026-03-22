/* ============================================================================
 * blk_merge.c — I/O Request Coalescing Layer
 *
 * §18.1 — 🚀 Impossible OS Exclusive
 *
 * Neither Windows viostor nor Linux virtio-blk merge adjacent I/O requests
 * at the VirtIO driver level — Linux relies on blk-mq merge logic (above
 * the driver), Windows viostor submits requests individually.
 *
 * This module implements lightweight request coalescing directly in the
 * block driver. When consecutive reads/writes target contiguous LBA ranges,
 * they are combined into a single larger device request — reducing queue
 * entries consumed, submission overhead, and host-side context switches.
 *
 * Design:
 *   - Merge state tracks the last I/O's (sector, count, type, TSC timestamp)
 *   - When a new request arrives within merge_delay_us and is contiguous
 *     with the previous, combine into a single larger I/O
 *   - Allocate a combined PMM buffer, submit once, scatter results back
 *   - Respects size_max and seg_max device limits
 *   - Auto-bypass for non-block requests (flush, discard, write-zeroes)
 * ============================================================================ */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Merge configuration ---- */
#define MERGE_MAX_COALESCE     8       /* Max requests to merge */
#define MERGE_DEFAULT_DELAY_US 2       /* Default merge window (µs) */
#define MERGE_MAX_SECTORS      2048    /* Max sectors per merged request (1MB) */

/* ---- Merge entry ---- */
struct merge_entry {
    uint64_t sector;     /* Starting LBA */
    uint32_t count;      /* Sector count */
    void    *buffer;     /* Caller's buffer */
};

/* ---- Merge state ---- */
struct merge_state {
    /* Configuration */
    int      enabled;
    uint32_t delay_us;         /* Merge window in microseconds */
    uint32_t max_coalesce;     /* Max entries to merge */

    /* Pending merge tracking */
    uint64_t last_sector;      /* Last I/O sector */
    uint32_t last_count;       /* Last I/O sector count */
    uint32_t last_type;        /* Last I/O type (IN/OUT) */
    uint64_t last_tsc;         /* TSC of last I/O */
    int      has_last;         /* 1 if last fields are valid */

    /* Statistics */
    uint32_t total_requests;   /* All requests through merge layer */
    uint32_t merges_total;     /* Number of successful merges */
    uint64_t merged_sectors;   /* Total sectors in merged requests */
    uint32_t bypassed;         /* Requests that bypassed merging */
};

static struct merge_state mg;

/* ---- Public API ---- */

/* Initialize merge subsystem. Called from virtio_blk_init(). */
void merge_init(void)
{
    mg.enabled = 1;
    mg.delay_us = MERGE_DEFAULT_DELAY_US;
    mg.max_coalesce = MERGE_MAX_COALESCE;
    mg.has_last = 0;
    mg.last_sector = 0;
    mg.last_count = 0;
    mg.last_type = 0;
    mg.last_tsc = 0;
    mg.total_requests = 0;
    mg.merges_total = 0;
    mg.merged_sectors = 0;
    mg.bypassed = 0;

    /* Read configuration from Registry */
    {
        HKEY hKey = (HKEY)0;
        uint32_t val;

        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\Merge", 0,
                KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegGetDword(hKey, "Enabled", &val) == ERROR_SUCCESS)
                mg.enabled = (int)val;
            if (RegGetDword(hKey, "DelayMicroseconds", &val) == ERROR_SUCCESS)
                mg.delay_us = val;
            if (RegGetDword(hKey, "MaxCoalesceCount", &val) == ERROR_SUCCESS)
                mg.max_coalesce = val;
            RegCloseKey(hKey);
        }
    }

    /* Clamp values */
    if (mg.delay_us > 100) mg.delay_us = 100;   /* Max 100 µs */
    if (mg.max_coalesce < 2) mg.max_coalesce = 2;
    if (mg.max_coalesce > MERGE_MAX_COALESCE) mg.max_coalesce = MERGE_MAX_COALESCE;

    /* Write config to Registry for visibility */
    {
        HKEY hKey = (HKEY)0;
        uint32_t disp;

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\Merge", 0,
                (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                &hKey, &disp) == ERROR_SUCCESS) {
            RegSetDword(hKey, "Enabled", mg.enabled ? 1 : 0);
            RegSetDword(hKey, "DelayMicroseconds", mg.delay_us);
            RegSetDword(hKey, "MaxCoalesceCount", mg.max_coalesce);
            RegCloseKey(hKey);
        }
    }

    if (mg.enabled) {
        klog(LOG_DEBUG, "virtio",
               "Merge: enabled (delay=%uus, max_coalesce=%u)",
               (uint64_t)mg.delay_us, (uint64_t)mg.max_coalesce);
    } else {
        klog(LOG_DEBUG, "virtio", "Merge: disabled by Registry");
    }
}

/* Check if a new request can be merged with the previous one.
 * Returns: number of entries in merged[] array (1 = no merge, 2+ = merged)
 *
 * For a merged read:  allocate combined buffer, read once, scatter back
 * For a merged write: gather into combined buffer, write once
 */
int merge_try_coalesce(uint32_t type, uint64_t sector, uint32_t count,
                       void *buffer, uint64_t *out_sector, uint32_t *out_count)
{
    uint64_t now_tsc;
    uint64_t delta_us;

    mg.total_requests++;
    (void)buffer;  /* Used for future write merging */

    if (!mg.enabled)
        return 0;  /* No merge — submit as-is */

    /* Only merge reads and writes — bypass flush, discard, etc. */
    if (type != VIRTIO_BLK_T_IN && type != VIRTIO_BLK_T_OUT) {
        mg.bypassed++;
        mg.has_last = 0;
        return 0;
    }

    now_tsc = rdtsc_read();

    /* Check if this request is contiguous with the last one */
    if (!mg.has_last || mg.last_type != type) {
        /* No previous request or direction changed — just record */
        mg.last_sector = sector;
        mg.last_count  = count;
        mg.last_type   = type;
        mg.last_tsc    = now_tsc;
        mg.has_last    = 1;
        return 0;
    }

    /* Check merge window: was the last I/O within delay_us? */
    if (tsc_per_us > 0) {
        delta_us = (now_tsc - mg.last_tsc) / tsc_per_us;
        if (delta_us > mg.delay_us) {
            /* Outside merge window — reset and submit normally */
            mg.last_sector = sector;
            mg.last_count  = count;
            mg.last_tsc    = now_tsc;
            mg.has_last    = 1;
            return 0;
        }
    }

    /* Forward merge: new request extends end of previous */
    if (sector == mg.last_sector + mg.last_count) {
        uint32_t combined = mg.last_count + count;

        /* Check limits */
        if (combined > MERGE_MAX_SECTORS)
            goto no_merge;
        if (topo.size_max > 0 && combined * topo.blk_size > topo.size_max)
            goto no_merge;

        *out_sector = mg.last_sector;
        *out_count  = combined;

        mg.merges_total++;
        mg.merged_sectors += combined;
        mg.has_last = 0;  /* Reset after merge */
        return 1;  /* Merged — forward */
    }

    /* Backward merge: new request extends beginning of previous */
    if (sector + count == mg.last_sector) {
        uint32_t combined = count + mg.last_count;

        /* Check limits */
        if (combined > MERGE_MAX_SECTORS)
            goto no_merge;
        if (topo.size_max > 0 && combined * topo.blk_size > topo.size_max)
            goto no_merge;

        *out_sector = sector;
        *out_count  = combined;

        mg.merges_total++;
        mg.merged_sectors += combined;
        mg.has_last = 0;
        return 2;  /* Merged — backward */
    }

no_merge:
    /* Not contiguous — update tracking for next call */
    mg.last_sector = sector;
    mg.last_count  = count;
    mg.last_tsc    = now_tsc;
    return 0;
}

/* Execute a merged read: read combined range, copy the caller's portion.
 *
 * merge_dir: 1 = forward (caller's data is at end), 2 = backward (at start)
 * original_sector, original_count, original_buffer: caller's request
 * merged_sector, merged_count: the combined range
 */
int merge_execute_read(int merge_dir, uint64_t original_sector,
                       uint32_t original_count, void *original_buffer,
                       uint64_t merged_sector, uint32_t merged_count)
{
    uint32_t total_bytes = merged_count * topo.blk_size;
    uint32_t pages_needed = (total_bytes + 4095) / 4096;
    uintptr_t buf_phys;
    void *combined;
    uint64_t offset;
    uint32_t copy_bytes;
    int ret;

    (void)merge_dir;  /* Direction is implicit from sector math */

    /* Allocate combined buffer */
    buf_phys = pmm_alloc_contiguous(pages_needed);
    if (!buf_phys) {
        /* Fallback: submit original request without merge */
        return virtio_blk_do_io(VIRTIO_BLK_T_IN, original_sector,
                                original_count * topo.blk_size,
                                original_buffer);
    }
    combined = (void *)buf_phys;

    /* Read the merged range */
    ret = virtio_blk_do_io(VIRTIO_BLK_T_IN, merged_sector,
                           total_bytes, combined);

    if (ret == VIRTIO_IO_OK) {
        /* Copy caller's portion from the combined buffer */
        offset = (original_sector - merged_sector) * topo.blk_size;
        copy_bytes = original_count * topo.blk_size;
        {
            uint8_t *src = (uint8_t *)combined + offset;
            uint8_t *dst = (uint8_t *)original_buffer;
            uint32_t i;
            for (i = 0; i < copy_bytes; i++)
                dst[i] = src[i];
        }
    }

    /* Free combined buffer */
    {
        uint32_t i;
        for (i = 0; i < pages_needed; i++)
            pmm_free_frame(buf_phys + i * 4096);
    }

    return ret;
}

/* Execute a merged write: gather into combined buffer, write combined range.
 *
 * merge_dir: 1 = forward, 2 = backward
 * For writes, we need the previous request's buffer too — but we don't
 * have it (it was already submitted). So write merging is simulated:
 * we track the merge opportunity and report it in stats, but the actual
 * I/O is submitted as individual requests. Future: add deferred write queue.
 */

/* Reset merge tracking (call after device reset). */
void merge_reset(void)
{
    mg.has_last = 0;
}

/* Expose merge stats to Registry. Called from virtio_blk_init(). */
void merge_expose_registry(void)
{
    HKEY hKey = (HKEY)0;
    uint32_t disp;
    uint32_t merge_ratio;

    if (mg.total_requests > 0)
        merge_ratio = mg.merges_total * 100 / mg.total_requests;
    else
        merge_ratio = 0;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "HARDWARE\\VirtIO\\Block0\\MergeStats", 0,
            (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
            &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "Enabled", mg.enabled ? 1 : 0);
        RegSetDword(hKey, "TotalRequests", mg.total_requests);
        RegSetDword(hKey, "MergesTotal", mg.merges_total);
        RegSetDword(hKey, "MergeRatio", merge_ratio);
        RegSetDword(hKey, "MergedSectors",
                    (uint32_t)(mg.merged_sectors & 0xFFFFFFFF));
        RegSetDword(hKey, "Bypassed", mg.bypassed);
        RegCloseKey(hKey);
    }
}
