/* ============================================================================
 * blkdev_adapters.c -- Block device adapter wrappers + directory dump
 *
 * Thin wrappers adapting driver-specific APIs to the blkdev function
 * pointer signature:  int fn(uint64_t lba, uint32_t count, void *buf,
 *                            void *driver_data)
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/drivers/ata.h"
#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/ahci.h"
#include "kernel/drivers/xhci.h"
#include "kernel/drivers/xhci_dev.h"
#include "kernel/drivers/usb_msc.h"
#include "kernel/drivers/nvme.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"
#include "kernel/fs/vfs.h"
/* Own declarations: the BLKDEV_UNSAFE_* mask bits and the blkdev_register_all
 * prototype live here, so including it is what makes a signature change a
 * compile error at BOTH ends rather than at the call site only. */
#include "main/main_internal.h"

/* ---- Block device adapter wrappers ---- */

static int blkdev_virtio_read(uint64_t lba, uint32_t count, void *buf,
                               void *driver_data)
{
    (void)driver_data;
    return virtio_blk_read(lba, count, buf);
}

static int blkdev_virtio_write(uint64_t lba, uint32_t count,
                                const void *buf, void *driver_data)
{
    (void)driver_data;
    return virtio_blk_write(lba, count, buf);
}

static int blkdev_virtio_flush(void *driver_data)
{
    int rc;
    (void)driver_data;
    rc = virtio_blk_flush();
    /* virtio_blk_flush returns 1 when VIRTIO_BLK_F_FLUSH was not
     * negotiated: per the VirtIO spec the device then has no volatile
     * write cache, so completed writes are already durable. The blkdev
     * contract is "0 = durable" -- normalize. */
    return (rc == 1) ? 0 : rc;
}

static int blkdev_virtio_discard(uint64_t sector, uint32_t num_sectors,
                                 void *driver_data)
{
    (void)driver_data;
    return virtio_blk_discard(sector, num_sectors);
}

static int blkdev_ahci_read(uint64_t lba, uint32_t count, void *buf,
                              void *driver_data)
{
    int port = (int)(uintptr_t)driver_data;
    return ahci_read(port, lba, count, buf);
}

static int blkdev_ahci_write(uint64_t lba, uint32_t count,
                               const void *buf, void *driver_data)
{
    int port = (int)(uintptr_t)driver_data;
    return ahci_write(port, lba, count, buf);
}

static int blkdev_ahci_discard(uint64_t sector, uint32_t num_sectors,
                                void *driver_data)
{
    int port = (int)(uintptr_t)driver_data;
    return ahci_trim(port, sector, num_sectors);
}

static int blkdev_ahci_flush(void *driver_data)
{
    int port = (int)(uintptr_t)driver_data;
    return ahci_flush(port);
}

static int blkdev_atapi_read(uint64_t lba, uint32_t count, void *buf,
                               void *driver_data)
{
    int port = (int)(uintptr_t)driver_data;
    return ahci_atapi_read(port, (uint32_t)lba, count, buf);
}

static int blkdev_ata_read(uint64_t lba, uint32_t count, void *buf,
                             void *driver_data)
{
    (void)driver_data;
    return ata_read_sectors((uint32_t)lba, (uint8_t)count, buf);
}

static int blkdev_ata_write(uint64_t lba, uint32_t count,
                              const void *buf, void *driver_data)
{
    (void)driver_data;
    return ata_write_sectors((uint32_t)lba, (uint8_t)count, buf);
}

/* ---- USB MSC adapter ---- */

/* driver_data packs controller index (high 16) + device index (low 16) */
#define MSC_PACK(ctrl, dev)   ((void *)(uintptr_t)(((uint32_t)(ctrl) << 16) | (uint32_t)(dev)))
#define MSC_CTRL(dd)          ((int)((uint32_t)(uintptr_t)(dd) >> 16))
#define MSC_DEV(dd)           ((int)((uint32_t)(uintptr_t)(dd) & 0xFFFF))

static int blkdev_usb_msc_read(uint64_t lba, uint32_t count, void *buf,
                                void *driver_data)
{
    int ci = MSC_CTRL(driver_data);
    int di = MSC_DEV(driver_data);
    struct xhci_controller *hc = xhci_get_controller_mut(ci);
    struct xhci_device *dev = xhci_get_controller_mut(ci) ? xhci_get_device(di) : NULL;
    if (!hc || !dev) return -1;
    if (lba > 0xFFFFFFFFull) return -1;   /* READ(10) carries a 32-bit LBA */
    return usb_msc_read_sectors(hc, dev, (uint32_t)lba, count, buf);
}

static int blkdev_usb_msc_write(uint64_t lba, uint32_t count,
                                 const void *buf, void *driver_data)
{
    int ci = MSC_CTRL(driver_data);
    int di = MSC_DEV(driver_data);
    struct xhci_controller *hc = xhci_get_controller_mut(ci);
    struct xhci_device *dev = xhci_get_controller_mut(ci) ? xhci_get_device(di) : NULL;
    if (!hc || !dev) return -1;
    if (lba > 0xFFFFFFFFull) return -1;   /* WRITE(10) carries a 32-bit LBA */
    return usb_msc_write_sectors(hc, dev, (uint32_t)lba, count, buf);
}

/* ---- NVMe adapter ---- */

static int blkdev_nvme_read(uint64_t lba, uint32_t count, void *buf,
                             void *driver_data)
{
    int ctrl = (int)(uintptr_t)driver_data;
    return nvme_read_sectors(ctrl, lba, count, buf);
}

static int blkdev_nvme_write(uint64_t lba, uint32_t count,
                              const void *buf, void *driver_data)
{
    int ctrl = (int)(uintptr_t)driver_data;
    return nvme_write_sectors(ctrl, lba, count, buf);
}

static int blkdev_nvme_flush(void *driver_data)
{
    int ctrl = (int)(uintptr_t)driver_data;
    return nvme_flush(ctrl);
}

static int blkdev_nvme_shutdown(void *driver_data)
{
    int ctrl = (int)(uintptr_t)driver_data;
    return nvme_shutdown(ctrl);
}

/* ---- Directory tree dump (serial-only) ---- */

/* Format a uint64_t with comma separators (e.g. 604696 → "604,696"). */
static uint32_t format_size_commas(uint64_t val, char *buf)
{
    char raw[20];
    int rn = 0, bi = 0, digits, groups, rem;

    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }

    while (val > 0) { raw[rn++] = '0' + (char)(val % 10); val /= 10; }

    digits = rn;
    groups = (digits - 1) / 3;
    rem    = digits - groups * 3;
    if (rem == 0) { rem = 3; groups--; }

    {
        int ri = rn - 1;
        int g;
        for (g = 0; g < rem && ri >= 0; g++)
            buf[bi++] = raw[ri--];
        while (ri >= 0) {
            buf[bi++] = ',';
            buf[bi++] = raw[ri--];
            if (ri >= 0) buf[bi++] = raw[ri--];
            if (ri >= 0) buf[bi++] = raw[ri--];
        }
    }
    buf[bi] = '\0';
    return (uint32_t)bi;
}

#define TREE_PATH_COL  60

uint32_t dump_dir_tree(const char *parent_path, struct vfs_node *node,
                       uint32_t depth)
{
    uint32_t idx = 0;
    uint32_t total = 0;
    struct vfs_dirent *de;

    if (depth > 20) return 0;

    while ((de = vfs_readdir(node, idx)) != 0) {
        char full_path[256];
        uint32_t pi = 0, ni = 0;
        const char *pp = parent_path;
        while (*pp && pi < 254) full_path[pi++] = *pp++;
        if (pi > 0 && full_path[pi - 1] != '\\')
            full_path[pi++] = '\\';
        while (de->name[ni] && pi < 254)
            full_path[pi++] = de->name[ni++];

        if (de->type & VFS_DIRECTORY) {
            if (pi < 254) full_path[pi++] = '\\';
            full_path[pi] = '\0';

            {
                char line[128];
                uint32_t li = 0, p;
                for (p = 0; full_path[p] && li < 120; p++)
                    line[li++] = full_path[p];
                while (li < TREE_PATH_COL && li < 120)
                    line[li++] = ' ';
                line[li++] = '<'; line[li++] = 'D'; line[li++] = 'I';
                line[li++] = 'R'; line[li++] = '>';
                line[li] = '\0';
                klog(LOG_DEBUG, "tree", "%s", line);
            }

            {
                full_path[pi - 1] = '\0';
                struct vfs_node *sub = vfs_finddir(node, de->name);
                if (sub)
                    total += dump_dir_tree(full_path, sub, depth + 1);
            }
        } else {
            full_path[pi] = '\0';

            struct vfs_stat st;
            uint64_t fsize = 0;
            if (node->ops && node->ops->stat) {
                struct vfs_node *fnode =
                    vfs_finddir(node, de->name);
                if (fnode && fnode->ops && fnode->ops->stat) {
                    if (fnode->ops->stat(fnode, &st) == 0)
                        fsize = st.size;
                }
            }

            {
                char line[128];
                char size_buf[26];
                uint32_t li = 0, p, sn, pad;

                for (p = 0; full_path[p] && li < 120; p++)
                    line[li++] = full_path[p];

                sn = format_size_commas(fsize, size_buf);

                {
                    uint32_t target = TREE_PATH_COL + 12;
                    uint32_t needed = sn + 2;
                    uint32_t fill = (target > li + needed) ?
                                    target - li - needed : 1;
                    for (pad = 0; pad < fill && li < 120; pad++)
                        line[li++] = ' ';
                }

                for (p = 0; size_buf[p] && li < 124; p++)
                    line[li++] = size_buf[p];
                line[li++] = ' '; line[li++] = 'B';
                line[li] = '\0';
                klog(LOG_DEBUG, "tree", "%s", line);
            }
        }
        total++;
        idx++;
    }
    return total;
}

/* ---- Block device registration ---- */

void blkdev_register_all(uint32_t unsafe_mask)
{
    struct blkdev bd;

    /* A driver named in the mask lost its async initializer part-way through
     * (TODO-10 S27) and nothing completed it, so every field below that would
     * be read out of its globals -- capacity, sector size, queue state -- may be
     * partial. Registering it anyway publishes those numbers to the VFS as if
     * they were a real device. */
    if (unsafe_mask)
        klog(LOG_ERROR, "blkdev",
             "Skipping registration for degraded storage driver(s), mask=0x%x",
             unsafe_mask);

    /* ATA master */
    if (!(unsafe_mask & BLKDEV_UNSAFE_ATA) && ata_get_drive(0)->present) {
        const struct ata_drive *drv = ata_get_drive(0);
        bd = (struct blkdev){0};
        bd.name[0]='a'; bd.name[1]='t'; bd.name[2]='a';
        bd.name[3]='0'; bd.name[4]='\0';
        bd.sector_size  = 512;
        bd.sector_count = (uint64_t)drv->sectors;
        bd.read  = blkdev_ata_read;
        bd.write = blkdev_ata_write;
        bd.driver_data  = (void *)0;
        blkdev_register(&bd);
    }

    /* VirtIO-blk */
    if (!(unsafe_mask & BLKDEV_UNSAFE_VIRTIO) && virtio_blk_present()) {
        bd = (struct blkdev){0};
        bd.name[0]='v'; bd.name[1]='i'; bd.name[2]='r';
        bd.name[3]='t'; bd.name[4]='i'; bd.name[5]='o';
        bd.name[6]='0'; bd.name[7]='\0';
        bd.sector_size  = virtio_blk_block_size();
        bd.sector_count = virtio_blk_capacity();
        bd.read  = blkdev_virtio_read;
        bd.write = blkdev_virtio_write;
        bd.flush = blkdev_virtio_flush;
        bd.discard = blkdev_virtio_discard;
        bd.driver_data  = (void *)0;
        blkdev_register(&bd);
    }

    /* AHCI / SATA drives */
    if (!(unsafe_mask & BLKDEV_UNSAFE_AHCI)) {
        int di;
        for (di = 0; di < ahci_drive_count(); di++) {
            bd = (struct blkdev){0};
            bd.name[0]='s'; bd.name[1]='a'; bd.name[2]='t';
            bd.name[3]='a'; bd.name[4]='0' + (char)di;
            bd.name[5]='\0';
            bd.sector_size  = ahci_sector_size(di);
            bd.sector_count = ahci_capacity(di);
            bd.read  = blkdev_ahci_read;
            bd.write = blkdev_ahci_write;
            bd.flush = blkdev_ahci_flush;
            bd.discard = blkdev_ahci_discard;
            bd.driver_data  = (void *)(uintptr_t)di;
            blkdev_register(&bd);
        }
    }

    /* AHCI / ATAPI (optical) devices -- same controller, same partial state */
    if (!(unsafe_mask & BLKDEV_UNSAFE_AHCI)) {
        int ai;
        for (ai = 0; ai < ahci_atapi_count(); ai++) {
            bd = (struct blkdev){0};
            bd.name[0]='c'; bd.name[1]='d'; bd.name[2]='r';
            bd.name[3]='o'; bd.name[4]='m';
            bd.name[5]='0' + (char)ai; bd.name[6]='\0';
            bd.sector_size  = ahci_atapi_sector_size(ai);
            bd.sector_count = ahci_atapi_capacity(ai);
            bd.read  = blkdev_atapi_read;
            bd.write = NULL;
            bd.driver_data  = (void *)(uintptr_t)ai;
            blkdev_register(&bd);
        }
    }

    /* USB MSC (Mass Storage) devices */
    {
        int msc_count = xhci_msc_device_count();
        int mi;
        for (mi = 0; mi < msc_count; mi++) {
            int dev_idx = xhci_msc_device_index(mi);
            struct xhci_device *dev = xhci_get_device(dev_idx);
            const struct usb_msc_info *info;

            if (!dev || dev_idx < 0) continue;
            info = usb_msc_get_info(dev);
            if (!info) continue;

            /* Route block I/O to the device's OWNING controller, not a hardcoded
             * controller 0: with two+ xHCI controllers a device on controller 1
             * would otherwise have its reads/writes issued against controller 0. */
            int ctrl_idx = xhci_controller_index(dev->owner);
            if (ctrl_idx < 0) continue;

            bd = (struct blkdev){0};
            bd.name[0]='u'; bd.name[1]='s'; bd.name[2]='b';
            bd.name[3]='0' + (char)mi; bd.name[4]='\0';
            bd.sector_size  = info->sector_size;
            bd.sector_count = info->sector_count;
            bd.read  = blkdev_usb_msc_read;
            bd.write = blkdev_usb_msc_write;
            /* Pack the owning controller + global device index into driver_data */
            bd.driver_data = MSC_PACK(ctrl_idx, dev_idx);
            blkdev_register(&bd);
        }
    }

    /* NVMe namespaces */
    POST16(POST16_NVME_BLK);
    if (!(unsafe_mask & BLKDEV_UNSAFE_NVME)) {
        int ci;
        for (ci = 0; ci < nvme_controller_count(); ci++) {
            struct nvme_controller *nc = nvme_get_controller(ci);
            if (!nc || !nc->io_queue_active ||
                nc->ns_sector_size == 0 || nc->ns_lba_count == 0)
                continue;
            bd = (struct blkdev){0};
            bd.name[0]='n'; bd.name[1]='v'; bd.name[2]='m';
            bd.name[3]='e'; bd.name[4]='0' + (char)ci;
            bd.name[5]='\0';
            bd.sector_size  = nc->ns_sector_size;
            bd.sector_count = nc->ns_lba_count;
            bd.read  = blkdev_nvme_read;
            bd.write = blkdev_nvme_write;
            bd.flush = blkdev_nvme_flush;       /* NVM Flush -- honest blkdev_sync */
            bd.shutdown = blkdev_nvme_shutdown; /* CC.SHN on poweroff/reboot */
            bd.driver_data  = (void *)(uintptr_t)ci;
            blkdev_register(&bd);
        }
    }
    POST16(POST16_NVME_BLK_OK);

    blkdev_list();
}
