/* ============================================================================
 * blkdev.c — Block Device Abstraction Layer
 *
 * Manages a global registry of block devices. Each hardware driver
 * (ATA, VirtIO-blk, AHCI) registers its device(s) at init time.
 * Higher layers (VFS, filesystem drivers) use blkdev_read/write()
 * without knowing which hardware driver backs the device.
 * ============================================================================ */

#include "kernel/drivers/blkdev.h"
#include "kernel/klog.h"
#include "kernel/printk.h"

/* ---- Device registry ---- */
static struct blkdev devices[BLKDEV_MAX];
static int           num_devices;

/* ---- String helpers ---- */
static int blk_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void blk_strncpy(char *dst, const char *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ---- API ---- */

int blkdev_register(const struct blkdev *dev)
{
    if (num_devices >= BLKDEV_MAX) {
        klog(LOG_DEBUG, "blk", "Registry full (%d devices)",
               (uint64_t)BLKDEV_MAX);
        return -1;
    }

    if (!dev || !dev->name[0] || !dev->read) {
        klog(LOG_DEBUG, "blk", "Invalid device registration");
        return -1;
    }

    struct blkdev *d = &devices[num_devices];
    blk_strncpy(d->name, dev->name, sizeof(d->name));
    d->sector_size  = dev->sector_size;
    d->sector_count = dev->sector_count;
    d->read         = dev->read;
    d->write        = dev->write;
    d->flush        = dev->flush;
    d->driver_data  = dev->driver_data;
    d->active       = 1;

    num_devices++;
    return 0;
}

const struct blkdev *blkdev_get(const char *name)
{
    int i;
    for (i = 0; i < num_devices; i++) {
        if (devices[i].active && blk_strcmp(devices[i].name, name) == 0)
            return &devices[i];
    }
    return (const struct blkdev *)0;
}

const struct blkdev *blkdev_get_by_index(int index)
{
    if (index < 0 || index >= num_devices)
        return (const struct blkdev *)0;
    if (!devices[index].active)
        return (const struct blkdev *)0;
    return &devices[index];
}

int blkdev_read(const struct blkdev *dev, uint64_t lba, uint32_t count,
                void *buf)
{
    if (!dev || !dev->read || !dev->active)
        return -1;
    return dev->read(lba, count, buf, dev->driver_data);
}

int blkdev_write(const struct blkdev *dev, uint64_t lba, uint32_t count,
                 const void *buf)
{
    if (!dev || !dev->write || !dev->active)
        return -1;
    return dev->write(lba, count, buf, dev->driver_data);
}

int blkdev_sync(const struct blkdev *dev)
{
    if (!dev || !dev->active)
        return -1;
    /* If no flush callback, device has no cache — success by default */
    if (!dev->flush)
        return 0;
    return dev->flush(dev->driver_data);
}

int blkdev_count(void)
{
    return num_devices;
}

int blkdev_update_capacity(const char *name, uint64_t new_sector_count)
{
    int i;
    for (i = 0; i < num_devices; i++) {
        if (devices[i].active && blk_strcmp(devices[i].name, name) == 0) {
            uint64_t old = devices[i].sector_count;
            devices[i].sector_count = new_sector_count;
            klog(LOG_DEBUG, "blk", "%s: capacity updated %u -> %u sectors",
                   devices[i].name, old, new_sector_count);
            return 0;
        }
    }
    return -1;
}

void blkdev_list(void)
{
    int i;

    if (num_devices == 0) {
        klog(LOG_DEBUG, "blk", "No block devices registered");
        return;
    }

    klog(LOG_DEBUG, "blk", "%u block device(s):", (uint64_t)num_devices);
    for (i = 0; i < num_devices; i++) {
        if (!devices[i].active)
            continue;

        uint64_t size_mb = (devices[i].sector_count *
                            (uint64_t)devices[i].sector_size) / (1024 * 1024);
        klog(LOG_DEBUG, "blk", "  %s: %u MiB (%u sectors, %u B/sec)",
               devices[i].name,
               size_mb,
               (uint64_t)devices[i].sector_count,
               (uint64_t)devices[i].sector_size);
    }
}
