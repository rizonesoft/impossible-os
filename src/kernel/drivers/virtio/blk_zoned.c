/* ============================================================================
 * blk_zoned.c -- Zoned Block Device (ZBD/ZNS) Support
 *
 * §11.1 -- 💎 Impossible OS Exclusive
 *
 * Zoned block devices divide the disk into sequential-write-only zones,
 * matching SMR (Shingled Magnetic Recording) drives and ZNS (Zoned Namespace)
 * SSDs. When VIRTIO_BLK_F_ZONED is negotiated, the device exposes zone
 * characteristics and supports zone management commands.
 *
 * Zone Types:
 *   - Conventional (0x1): random write OK (like a normal block device)
 *   - Sequential Write Required (0x2): writes must be sequential at WP
 *   - Sequential Write Preferred (0x3): sequential preferred, random allowed
 *
 * Zone Conditions:
 *   EMPTY → IMP_OPEN (on write) or EXP_OPEN (on explicit open)
 *   OPEN → CLOSED (on close) → EMPTY (on reset)
 *   OPEN/CLOSED → FULL (on finish or when capacity reached)
 *
 * Commands:
 *   ZONE_REPORT  -- enumerate zone descriptors
 *   ZONE_OPEN    -- explicitly open a zone for writing
 *   ZONE_CLOSE   -- close an open zone
 *   ZONE_FINISH  -- fill remaining capacity, transition to full
 *   ZONE_RESET   -- reset zone write pointer to start (erase)
 *   ZONE_APPEND  -- append data at write pointer, device returns actual LBA
 * ============================================================================ */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Zone configuration ---- */
struct zone_config {
    uint32_t model;             /* 0=none, 1=host-aware, 2=host-managed */
    uint32_t max_open_zones;    /* Max simultaneously open zones */
    uint32_t max_active_zones;  /* Max active (open + closed + finishing) */
    uint64_t zone_sectors;      /* Sectors per zone */
    uint32_t nr_zones;          /* Total number of zones */
};

static struct zone_config zcfg;

/* Statistics */
static uint32_t zone_reports;
static uint32_t zone_opens;
static uint32_t zone_closes;
static uint32_t zone_finishes;
static uint32_t zone_resets;
static uint32_t zone_appends;

/* ---- Zone management command (no data phase) ---- */

/* Submit a zone management command (OPEN, CLOSE, FINISH, RESET).
 * These commands have only header + status (no data buffer). */
static int zone_mgmt_cmd(uint32_t type, uint64_t zone_start_sector)
{
    struct virtio_blk_req req;
    uint8_t status_byte = 0xFF;
    int d0, d1;
    uint32_t timeout;
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint64_t submit_tsc = rdtsc_read();

    if (!blk_initialized)
        return -1;

    /* Build request header -- sector = zone start LBA */
    req.type     = type;
    req.reserved = 0;
    req.sector   = zone_start_sector;

    /* 2-descriptor chain: header + status */
    if (blk_vqs[qi].num_free < 2) {
        klog(LOG_DEBUG, "virtio", "Zone: no free descriptors for type %u",
               (uint64_t)type);
        return -1;
    }

    /* Descriptor 0: request header (device-readable) */
    d0 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    blk_vqs[qi].desc[d0].len   = sizeof(struct virtio_blk_req);
    blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 1: status byte (device-writable) */
    d1 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d1].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].next = (uint16_t)d1;

    blk_vqs[qi].desc[d1].addr  = (uint64_t)(uintptr_t)&status_byte;
    blk_vqs[qi].desc[d1].len   = 1;
    blk_vqs[qi].desc[d1].flags = VIRTQ_DESC_F_WRITE;
    blk_vqs[qi].desc[d1].next  = 0;

    /* Add to available ring */
    {
        uint16_t avail_idx = blk_vqs[qi].avail->idx % blk_vqs[qi].size;
        blk_vqs[qi].avail->ring[avail_idx] = (uint16_t)d0;
    }

    wmb();
    blk_vqs[qi].avail->idx++;
    mb();

    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    virtio_irq_flags[qi] = 0;
    __asm__ volatile ("sti");

    virtq_kick(&blk_vqs[qi]);

    /* Wait for completion */
    if (blk_use_events) {
        if (!event_wait_timeout(&io_completions[qi], 5000)) {
            klog(LOG_DEBUG, "virtio", "Zone cmd timeout (type %u)", (uint64_t)type);
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            return VIRTIO_IO_TIMEOUT;
        }
    } else {
        timeout = 5000000;
        while (timeout-- > 0) {
            mb();
            if (blk_vqs[qi].used->idx != blk_vqs[qi].last_used)
                break;
            __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
        }
        if (timeout == 0) {
            klog(LOG_DEBUG, "virtio", "Zone cmd timeout poll (type %u)",
                   (uint64_t)type);
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            return VIRTIO_IO_TIMEOUT;
        }
    }

    if (!(rflags & (1 << 9)))
        __asm__ volatile ("cli");

    rmb();
    blk_vqs[qi].last_used++;
    if (has_event_idx)
        virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;

    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);

    latency_record(submit_tsc, LAT_TYPE_DISCARD);

    if (status_byte == VIRTIO_BLK_S_OK)
        return VIRTIO_IO_OK;
    if (status_byte == VIRTIO_BLK_S_UNSUPP) {
        error_stats.unsupp_errors++;
        return VIRTIO_IO_UNSUPP;
    }
    error_stats.io_errors++;
    return VIRTIO_IO_IOERR;
}

/* ---- Public Zone Management API ---- */

/* Report zones starting at 'start_sector'.
 * 'descs' is an output buffer for zone descriptors.
 * 'max_descs' is the maximum number of descriptors to return.
 * Returns number of zones reported (>=0) or -1 on error. */
int virtio_blk_zone_report(uint64_t start_sector,
                           struct virtio_blk_zone_descriptor *descs,
                           uint32_t max_descs)
{
    struct virtio_blk_req req;
    uint8_t status_byte = 0xFF;
    int d0, d1, d2;
    uint32_t timeout;
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint32_t report_size;
    uint32_t nr_returned;

    if (!blk_initialized || !has_zoned)
        return -1;
    if (!descs || max_descs == 0)
        return -1;

    /* Response size: 8-byte header + descriptors */
    report_size = sizeof(struct virtio_blk_zone_report) +
                  max_descs * sizeof(struct virtio_blk_zone_descriptor);

    /* Build request header */
    req.type     = VIRTIO_BLK_T_ZONE_REPORT;
    req.reserved = 0;
    req.sector   = start_sector;

    /* 3-descriptor chain: header + report data (device-writable) + status */
    if (blk_vqs[qi].num_free < 3) {
        klog(LOG_DEBUG, "virtio", "Zone report: no free descriptors");
        return -1;
    }

    /* We need a contiguous buffer for the report header + descriptors.
     * Use a temporary PMM-allocated buffer, then copy descriptors out. */
    uintptr_t report_phys = pmm_alloc_contiguous(
        (report_size + 4095) / 4096);
    if (!report_phys) {
        klog(LOG_DEBUG, "virtio", "Zone report: PMM alloc failed (%u bytes)",
               (uint64_t)report_size);
        return -1;
    }

    uint8_t *report_buf = (uint8_t *)report_phys;
    {
        uint32_t bi;
        for (bi = 0; bi < report_size; bi++)
            report_buf[bi] = 0;
    }

    /* Descriptor 0: request header (device-readable) */
    d0 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    blk_vqs[qi].desc[d0].len   = sizeof(struct virtio_blk_req);
    blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 1: zone report data (device-writable) */
    d1 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d1].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].next = (uint16_t)d1;

    blk_vqs[qi].desc[d1].addr  = (uint64_t)report_phys;
    blk_vqs[qi].desc[d1].len   = report_size;
    blk_vqs[qi].desc[d1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;

    /* Descriptor 2: status byte (device-writable) */
    d2 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d2].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d1].next = (uint16_t)d2;

    blk_vqs[qi].desc[d2].addr  = (uint64_t)(uintptr_t)&status_byte;
    blk_vqs[qi].desc[d2].len   = 1;
    blk_vqs[qi].desc[d2].flags = VIRTQ_DESC_F_WRITE;
    blk_vqs[qi].desc[d2].next  = 0;

    /* Add to available ring */
    {
        uint16_t avail_idx = blk_vqs[qi].avail->idx % blk_vqs[qi].size;
        blk_vqs[qi].avail->ring[avail_idx] = (uint16_t)d0;
    }

    wmb();
    blk_vqs[qi].avail->idx++;
    mb();

    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    virtio_irq_flags[qi] = 0;
    __asm__ volatile ("sti");

    virtq_kick(&blk_vqs[qi]);

    /* Wait for completion */
    if (blk_use_events) {
        if (!event_wait_timeout(&io_completions[qi], 5000)) {
            klog(LOG_DEBUG, "virtio", "Zone report timeout");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
            pmm_free_frame(report_phys);
            return -1;
        }
    } else {
        timeout = 5000000;
        while (timeout-- > 0) {
            mb();
            if (blk_vqs[qi].used->idx != blk_vqs[qi].last_used)
                break;
            __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
        }
        if (timeout == 0) {
            klog(LOG_DEBUG, "virtio", "Zone report timeout (poll)");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
            pmm_free_frame(report_phys);
            return -1;
        }
    }

    if (!(rflags & (1 << 9)))
        __asm__ volatile ("cli");

    rmb();
    blk_vqs[qi].last_used++;
    if (has_event_idx)
        virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;

    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);

    if (status_byte != VIRTIO_BLK_S_OK) {
        pmm_free_frame(report_phys);
        return -1;
    }

    /* Parse report header */
    {
        struct virtio_blk_zone_report *hdr =
            (struct virtio_blk_zone_report *)report_buf;
        nr_returned = hdr->nr_zones;
        if (nr_returned > max_descs)
            nr_returned = max_descs;
    }

    /* Copy descriptors to caller's buffer */
    {
        uint8_t *src = report_buf + sizeof(struct virtio_blk_zone_report);
        uint8_t *dst = (uint8_t *)descs;
        uint32_t copy_size = nr_returned *
                             sizeof(struct virtio_blk_zone_descriptor);
        uint32_t ci;
        for (ci = 0; ci < copy_size; ci++)
            dst[ci] = src[ci];
    }

    pmm_free_frame(report_phys);
    zone_reports++;

    return (int)nr_returned;
}

/* Open a zone for writing.
 * zone_start: starting sector of the zone.
 * Returns 0 on success, -1 on error, 1 if not supported. */
int virtio_blk_zone_open(uint64_t zone_start)
{
    int ret;
    if (!has_zoned) return 1;
    ret = zone_mgmt_cmd(VIRTIO_BLK_T_ZONE_OPEN, zone_start);
    if (ret == VIRTIO_IO_OK) { zone_opens++; return 0; }
    if (ret == VIRTIO_IO_TIMEOUT) {
        error_stats.timeouts++;
        virtio_blk_reset();
    }
    return -1;
}

/* Close a zone.
 * zone_start: starting sector of the zone.
 * Returns 0 on success, -1 on error, 1 if not supported. */
int virtio_blk_zone_close(uint64_t zone_start)
{
    int ret;
    if (!has_zoned) return 1;
    ret = zone_mgmt_cmd(VIRTIO_BLK_T_ZONE_CLOSE, zone_start);
    if (ret == VIRTIO_IO_OK) { zone_closes++; return 0; }
    if (ret == VIRTIO_IO_TIMEOUT) {
        error_stats.timeouts++;
        virtio_blk_reset();
    }
    return -1;
}

/* Finish a zone (fill remaining capacity, transition to full).
 * zone_start: starting sector of the zone.
 * Returns 0 on success, -1 on error, 1 if not supported. */
int virtio_blk_zone_finish(uint64_t zone_start)
{
    int ret;
    if (!has_zoned) return 1;
    ret = zone_mgmt_cmd(VIRTIO_BLK_T_ZONE_FINISH, zone_start);
    if (ret == VIRTIO_IO_OK) { zone_finishes++; return 0; }
    if (ret == VIRTIO_IO_TIMEOUT) {
        error_stats.timeouts++;
        virtio_blk_reset();
    }
    return -1;
}

/* Reset a zone write pointer to start (erase zone data).
 * zone_start: starting sector of the zone.
 * Returns 0 on success, -1 on error, 1 if not supported. */
int virtio_blk_zone_reset(uint64_t zone_start)
{
    int ret;
    if (!has_zoned) return 1;
    ret = zone_mgmt_cmd(VIRTIO_BLK_T_ZONE_RESET, zone_start);
    if (ret == VIRTIO_IO_OK) { zone_resets++; return 0; }
    if (ret == VIRTIO_IO_TIMEOUT) {
        error_stats.timeouts++;
        virtio_blk_reset();
    }
    return -1;
}

/* Append data at zone write pointer.
 * The device writes data at the current WP and returns the actual LBA
 * where the data was written.
 * zone_start: starting sector of the zone
 * data: buffer to write
 * num_sectors: number of 512-byte sectors
 * out_sector: (optional) if non-NULL, receives the actual LBA written
 * Returns 0 on success, -1 on error, 1 if not supported. */
int virtio_blk_zone_append(uint64_t zone_start, const void *data,
                           uint32_t num_sectors, uint64_t *out_sector)
{
    struct virtio_blk_req req;
    uint8_t status_byte = 0xFF;
    int d0, d1, d2;
    uint32_t timeout;
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint64_t submit_tsc = rdtsc_read();
    uint32_t data_len = num_sectors * 512;

    if (!blk_initialized || !has_zoned)
        return 1;

    /* Build request header -- sector = zone start */
    req.type     = VIRTIO_BLK_T_ZONE_APPEND;
    req.reserved = 0;
    req.sector   = zone_start;

    /* 3-descriptor chain: header (dev-read) + data (dev-read) + status (dev-write) */
    if (blk_vqs[qi].num_free < 3) {
        klog(LOG_DEBUG, "virtio", "Zone append: no free descriptors");
        return -1;
    }

    /* Descriptor 0: request header */
    d0 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    blk_vqs[qi].desc[d0].len   = sizeof(struct virtio_blk_req);
    blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 1: data buffer (device-readable -- append data) */
    d1 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d1].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].next = (uint16_t)d1;

    blk_vqs[qi].desc[d1].addr  = (uint64_t)(uintptr_t)data;
    blk_vqs[qi].desc[d1].len   = data_len;
    blk_vqs[qi].desc[d1].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 2: status byte (device-writable) */
    d2 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d2].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d1].next = (uint16_t)d2;

    blk_vqs[qi].desc[d2].addr  = (uint64_t)(uintptr_t)&status_byte;
    blk_vqs[qi].desc[d2].len   = 1;
    blk_vqs[qi].desc[d2].flags = VIRTQ_DESC_F_WRITE;
    blk_vqs[qi].desc[d2].next  = 0;

    /* Add to available ring */
    {
        uint16_t avail_idx = blk_vqs[qi].avail->idx % blk_vqs[qi].size;
        blk_vqs[qi].avail->ring[avail_idx] = (uint16_t)d0;
    }

    wmb();
    blk_vqs[qi].avail->idx++;
    mb();

    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    virtio_irq_flags[qi] = 0;
    __asm__ volatile ("sti");

    virtq_kick(&blk_vqs[qi]);

    /* Wait for completion */
    if (blk_use_events) {
        if (!event_wait_timeout(&io_completions[qi], 5000)) {
            klog(LOG_DEBUG, "virtio", "Zone append timeout");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
            return VIRTIO_IO_TIMEOUT;
        }
    } else {
        timeout = 5000000;
        while (timeout-- > 0) {
            mb();
            if (blk_vqs[qi].used->idx != blk_vqs[qi].last_used)
                break;
            __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
        }
        if (timeout == 0) {
            klog(LOG_DEBUG, "virtio", "Zone append timeout (poll)");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
            return VIRTIO_IO_TIMEOUT;
        }
    }

    if (!(rflags & (1 << 9)))
        __asm__ volatile ("cli");

    rmb();
    blk_vqs[qi].last_used++;
    if (has_event_idx)
        virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;

    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);

    latency_record(submit_tsc, LAT_TYPE_WRITE);

    if (status_byte != VIRTIO_BLK_S_OK) {
        if (status_byte == VIRTIO_BLK_S_UNSUPP)
            error_stats.unsupp_errors++;
        else
            error_stats.io_errors++;
        return -1;
    }

    /* On success, the device updates req.sector with actual write LBA */
    if (out_sector)
        *out_sector = req.sector;

    zone_appends++;
    return 0;
}

/* ---- Zone Initialization ---- */

/* Initialize zone config -- called from blk_init after feature negotiation.
 * Reads zone parameters from device config space. */
void zone_init(void)
{
    zcfg.model           = 0;
    zcfg.max_open_zones  = 0;
    zcfg.max_active_zones = 0;
    zcfg.zone_sectors    = 0;
    zcfg.nr_zones        = 0;

    zone_reports  = 0;
    zone_opens    = 0;
    zone_closes   = 0;
    zone_finishes = 0;
    zone_resets   = 0;
    zone_appends  = 0;

    if (!has_zoned) {
        klog(LOG_DEBUG, "virtio", "Zone: not negotiated, skipping init");
        return;
    }

    /* Read zone model from config space (zoned model field).
     * Config layout for zoned devices:
     *   offset 0x48: uint8_t model (0=none, 1=host-aware, 2=host-managed)
     *   offset 0x4C: uint32_t max_open_zones
     *   offset 0x50: uint32_t max_active_zones
     *   offset 0x54: uint32_t zone_sectors */
    if (blk_dev.device_cfg) {
        zcfg.model = mmio_read8(
            (volatile uint8_t *)(blk_dev.device_cfg + 0x48));
        zcfg.max_open_zones = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + 0x4C));
        zcfg.max_active_zones = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + 0x50));
        zcfg.zone_sectors = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + 0x54));

        if (zcfg.zone_sectors > 0 && disk_capacity > 0)
            zcfg.nr_zones = (uint32_t)((disk_capacity + zcfg.zone_sectors - 1) /
                                        zcfg.zone_sectors);
    }

    klog(LOG_DEBUG, "virtio",
           "Zone: model=%u open_max=%u active_max=%u "
           "zone_sectors=%u nr_zones=%u",
           (uint64_t)zcfg.model,
           (uint64_t)zcfg.max_open_zones,
           (uint64_t)zcfg.max_active_zones,
           zcfg.zone_sectors,
           (uint64_t)zcfg.nr_zones);
}

/* Expose zone config and stats via Registry */
void zone_expose_registry(void)
{
    HKEY hk;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                       "HARDWARE\\VirtIO\\Block0\\Zones",
                       0, NULL, 0, 0, NULL, &hk, NULL) == 0) {
        RegSetDword(hk, "Model",          zcfg.model);
        RegSetDword(hk, "MaxOpenZones",   zcfg.max_open_zones);
        RegSetDword(hk, "MaxActiveZones", zcfg.max_active_zones);
        RegSetDword(hk, "ZoneSectors",    (uint32_t)zcfg.zone_sectors);
        RegSetDword(hk, "NrZones",        zcfg.nr_zones);
        RegSetDword(hk, "Reports",        zone_reports);
        RegSetDword(hk, "Opens",          zone_opens);
        RegSetDword(hk, "Closes",         zone_closes);
        RegSetDword(hk, "Finishes",       zone_finishes);
        RegSetDword(hk, "Resets",         zone_resets);
        RegSetDword(hk, "Appends",        zone_appends);
        RegCloseKey(hk);
    }
}

/* Check if zoned block device is active */
int zone_is_active(void)
{
    return has_zoned && (zcfg.model != 0);
}

/* Get zone configuration */
uint32_t zone_get_nr_zones(void)
{
    return zcfg.nr_zones;
}

uint64_t zone_get_zone_sectors(void)
{
    return zcfg.zone_sectors;
}

uint32_t zone_get_max_open(void)
{
    return zcfg.max_open_zones;
}

uint32_t zone_get_max_active(void)
{
    return zcfg.max_active_zones;
}
