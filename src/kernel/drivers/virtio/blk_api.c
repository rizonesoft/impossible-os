/* blk_api.c — Public API: read, write, flush, capacity, serial, lifetime */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Public API ---- */

int virtio_blk_read(uint64_t lba, uint32_t count, void *buffer)
{
    int ret;
    int retry;

    /* Check prefetch buffer first — sub-microsecond if hit */
    if (prefetch_try_read(lba, count, buffer))
        return 0;

    /* Check for merge opportunity with previous read */
    {
        uint64_t merged_sector;
        uint32_t merged_count;
        int merge_dir = merge_try_coalesce(VIRTIO_BLK_T_IN, lba, count,
                                           buffer, &merged_sector,
                                           &merged_count);
        if (merge_dir > 0) {
            ret = merge_execute_read(merge_dir, lba, count, buffer,
                                     merged_sector, merged_count);
            if (ret == VIRTIO_IO_OK) {
                prefetch_after_read(lba, count);
                return 0;
            }
            /* Merge read failed — fall through to normal path */
        }
    }

    for (retry = 0; retry <= VIRTIO_BLK_MAX_RETRIES; retry++) {
        ret = virtio_blk_do_io(VIRTIO_BLK_T_IN, lba, count * topo.blk_size, buffer);
        if (ret == VIRTIO_IO_OK) {
            /* Record history and potentially trigger prefetch */
            prefetch_after_read(lba, count);
            return 0;
        }
        if (ret == VIRTIO_IO_UNSUPP) {
            klog(LOG_DEBUG, "virtio", "Read: unsupported (lba=%u)",
                   (uint64_t)lba);
            return -1;
        }
        if (ret == VIRTIO_IO_TIMEOUT) {
            error_stats.timeouts++;
            klog(LOG_DEBUG, "virtio", "Read: timeout, triggering reset");
            virtio_blk_reset();
            return -1;
        }
        /* IOERR — retry */
        if (retry < VIRTIO_BLK_MAX_RETRIES) {
            klog(LOG_DEBUG, "virtio", "Read: I/O error, retry %u/%u (lba=%u)",
                   (uint64_t)(retry + 1), (uint64_t)VIRTIO_BLK_MAX_RETRIES,
                   (uint64_t)lba);
        }
    }
    klog(LOG_DEBUG, "virtio", "Read: failed after %u retries (lba=%u)",
           (uint64_t)VIRTIO_BLK_MAX_RETRIES, (uint64_t)lba);
    return -1;
}

int virtio_blk_write(uint64_t lba, uint32_t count, const void *buffer)
{
    int ret;
    int retry;

    if (is_read_only)
        return -1;  /* Device is read-only */

    /* Invalidate prefetch buffer if write overlaps it */
    prefetch_invalidate(lba, count);

    for (retry = 0; retry <= VIRTIO_BLK_MAX_RETRIES; retry++) {
        ret = virtio_blk_do_io(VIRTIO_BLK_T_OUT, lba, count * topo.blk_size,
                               (void *)buffer);
        if (ret == VIRTIO_IO_OK)
            return 0;
        if (ret == VIRTIO_IO_UNSUPP) {
            klog(LOG_DEBUG, "virtio", "Write: unsupported (lba=%u)",
                   (uint64_t)lba);
            return -1;
        }
        if (ret == VIRTIO_IO_TIMEOUT) {
            error_stats.timeouts++;
            klog(LOG_DEBUG, "virtio", "Write: timeout, triggering reset");
            virtio_blk_reset();
            return -1;
        }
        if (retry < VIRTIO_BLK_MAX_RETRIES) {
            klog(LOG_DEBUG, "virtio", "Write: I/O error, retry %u/%u (lba=%u)",
                   (uint64_t)(retry + 1), (uint64_t)VIRTIO_BLK_MAX_RETRIES,
                   (uint64_t)lba);
        }
    }
    klog(LOG_DEBUG, "virtio", "Write: failed after %u retries (lba=%u)",
           (uint64_t)VIRTIO_BLK_MAX_RETRIES, (uint64_t)lba);
    return -1;
}

int virtio_blk_set_write_cache(int enable)
{
    if (!blk_initialized || !has_config_wce)
        return -1;

    if (!blk_dev.device_cfg)
        return -1;

    /* Write the writeback field at device config offset 0x20 */
    volatile uint8_t *wb = (volatile uint8_t *)
        (blk_dev.device_cfg + VIRTIO_BLK_CFG_WRITEBACK);
    mmio_write8(wb, enable ? 1 : 0);

    klog(LOG_DEBUG, "virtio", "Write cache mode: %s",
           enable ? "writeback" : "writethrough");

    return 0;
}

uint64_t virtio_blk_capacity(void)
{
    return disk_capacity;
}

int virtio_blk_present(void)
{
    return blk_initialized;
}

uint16_t virtio_blk_num_queues(void)
{
    return num_queues;
}

uint32_t virtio_blk_block_size(void)
{
    return topo.blk_size;
}

const struct virtio_blk_topology *virtio_blk_topology(void)
{
    return &topo;
}

int virtio_blk_get_id(char *buf, uint32_t len)
{
    char tmp[VIRTIO_BLK_ID_BYTES];
    uint32_t i;
    int ret;

    if (!blk_initialized || !buf || len == 0)
        return -1;

    /* Zero the temp buffer — device may not write all 20 bytes */
    for (i = 0; i < VIRTIO_BLK_ID_BYTES; i++)
        tmp[i] = 0;

    /* GET_ID: 3-descriptor chain — header (type=8, sector=0) +
     * 20-byte device-writable buffer + status byte.
     * do_io sets F_WRITE on data descriptor for all non-OUT types. */
    ret = virtio_blk_do_io(VIRTIO_BLK_T_GET_ID, 0, VIRTIO_BLK_ID_BYTES, tmp);
    if (ret != 0)
        return -1;

    /* Copy and null-terminate */
    for (i = 0; i < len - 1 && i < VIRTIO_BLK_ID_BYTES; i++)
        buf[i] = tmp[i];
    buf[i] = '\0';

    return 0;
}

int virtio_blk_get_lifetime(struct virtio_blk_lifetime *out)
{
    uint8_t tmp[6];
    uint32_t i;
    int ret;

    if (!blk_initialized || !out)
        return -1;

    if (!has_lifetime)
        return 1;  /* Not supported */

    /* Zero the temp buffer */
    for (i = 0; i < sizeof(tmp); i++)
        tmp[i] = 0;

    /* GET_LIFETIME: 3-descriptor chain — header (type=10, sector=0) +
     * 6-byte device-writable buffer + status byte.
     * do_io sets F_WRITE on data descriptor for all non-OUT types. */
    ret = virtio_blk_do_io(VIRTIO_BLK_T_GET_LIFETIME, 0, sizeof(tmp), tmp);
    if (ret != 0)
        return -1;

    /* Parse little-endian fields */
    out->pre_eol_info = (uint16_t)tmp[0] | ((uint16_t)tmp[1] << 8);
    out->device_lifetime_est_typ_a = (uint16_t)tmp[2] | ((uint16_t)tmp[3] << 8);
    out->device_lifetime_est_typ_b = (uint16_t)tmp[4] | ((uint16_t)tmp[5] << 8);

    return 0;
}

const char *virtio_blk_serial(void)
{
    return device_serial;
}


