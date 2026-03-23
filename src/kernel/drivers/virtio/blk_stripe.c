/* ============================================================================
 * blk_stripe.c -- Multi-Device RAID-0 Striping
 *
 * S19.1 -- 🚀 Impossible OS Exclusive
 *
 * Neither Windows viostor nor Linux virtio-blk implement driver-level striping
 * -- both rely on software RAID layers above (Storage Spaces, md/dm).
 *
 * When 2+ VirtIO block devices of equal capacity are detected and striping
 * is enabled, this module assembles them into a RAID-0 stripe set exposed
 * as a single virtual blkdev ("virtio-stripe0"). I/O requests are split at
 * stripe boundaries and dispatched to the correct member device.
 *
 * Design:
 *   - Scan for additional VirtIO block devices beyond the primary
 *   - Initialize each with independent PCI transport + virtqueues
 *   - Stripe map: target_dev = (lba / stripe_sectors) % num_devices
 *   - Split requests that cross stripe boundaries into sub-requests
 *   - Register virtual blkdev with aggregate capacity
 * ============================================================================ */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Stripe configuration ---- */
#define STRIPE_MAX_DEVICES      8      /* Max devices in a stripe set */
#define STRIPE_DEFAULT_SECTORS  128    /* Default stripe width: 64 KB = 128x512 */

/* ---- Per-member device state ---- */
struct stripe_member {
    struct virtio_pci_dev  pci_dev;
    struct virtqueue       vqs[1];     /* Single queue per member for simplicity */
    uint64_t               capacity;   /* Sectors */
    uint8_t                pci_bus;
    uint8_t                pci_slot;
    uint8_t                pci_func;
    int                    active;
    char                   serial[VIRTIO_BLK_ID_BYTES + 1];
};

/* ---- Stripe set state ---- */
struct stripe_set {
    int                    enabled;
    int                    active;      /* 1 if stripe set is assembled and live */
    uint32_t               num_devices; /* Number of member devices */
    uint32_t               stripe_sectors; /* Stripe width in sectors */
    uint64_t               total_capacity; /* Aggregate capacity in sectors */
    struct stripe_member   members[STRIPE_MAX_DEVICES];

    /* Statistics */
    uint32_t               reads;
    uint32_t               writes;
    uint32_t               splits;     /* Requests split across stripe boundaries */
};

static struct stripe_set stripe;

/* ---- Low-level I/O to a specific member device ---- */
static int stripe_member_io(struct stripe_member *m, uint32_t type,
                            uint64_t sector, uint32_t len, void *buffer)
{
    struct virtio_blk_req req;
    uint8_t status_byte = 0xFF;
    int d0, d1, d2;
    uint32_t timeout;

    if (!m->active)
        return -1;

    req.type     = type;
    req.reserved = 0;
    req.sector   = sector;

    /* Allocate 3 descriptors from member's queue */
    if (m->vqs[0].num_free < 3)
        return -1;

    d0 = m->vqs[0].free_head;
    m->vqs[0].free_head = m->vqs[0].desc[d0].next;
    m->vqs[0].num_free--;

    m->vqs[0].desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    m->vqs[0].desc[d0].len   = sizeof(struct virtio_blk_req);
    m->vqs[0].desc[d0].flags = VIRTQ_DESC_F_NEXT;

    d1 = m->vqs[0].free_head;
    m->vqs[0].free_head = m->vqs[0].desc[d1].next;
    m->vqs[0].num_free--;

    m->vqs[0].desc[d0].next = (uint16_t)d1;
    m->vqs[0].desc[d1].addr  = (uint64_t)(uintptr_t)buffer;
    m->vqs[0].desc[d1].len   = len;
    m->vqs[0].desc[d1].flags = VIRTQ_DESC_F_NEXT;
    if (type != VIRTIO_BLK_T_OUT)
        m->vqs[0].desc[d1].flags |= VIRTQ_DESC_F_WRITE;

    d2 = m->vqs[0].free_head;
    m->vqs[0].free_head = m->vqs[0].desc[d2].next;
    m->vqs[0].num_free--;

    m->vqs[0].desc[d1].next = (uint16_t)d2;
    m->vqs[0].desc[d2].addr  = (uint64_t)(uintptr_t)&status_byte;
    m->vqs[0].desc[d2].len   = 1;
    m->vqs[0].desc[d2].flags = VIRTQ_DESC_F_WRITE;
    m->vqs[0].desc[d2].next  = 0;

    /* Add to available ring */
    {
        uint16_t avail_idx = m->vqs[0].avail->idx % m->vqs[0].size;
        m->vqs[0].avail->ring[avail_idx] = (uint16_t)d0;
    }

    wmb();
    m->vqs[0].avail->idx++;
    mb();

    /* Kick and poll for completion */
    virtq_kick(&m->vqs[0]);

    timeout = 5000000;
    while (timeout-- > 0) {
        mb();
        if (m->vqs[0].used->idx != m->vqs[0].last_used)
            break;
        __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
    }

    if (timeout == 0) {
        virtq_free_desc(&m->vqs[0], (uint16_t)d0);
        virtq_free_desc(&m->vqs[0], (uint16_t)d1);
        virtq_free_desc(&m->vqs[0], (uint16_t)d2);
        return -1;
    }

    rmb();
    m->vqs[0].last_used++;
    virtq_free_desc(&m->vqs[0], (uint16_t)d0);
    virtq_free_desc(&m->vqs[0], (uint16_t)d1);
    virtq_free_desc(&m->vqs[0], (uint16_t)d2);

    return (status_byte == VIRTIO_BLK_S_OK) ? 0 : -1;
}

/* ---- Stripe I/O: split at stripe boundaries ---- */

/* Map a virtual LBA to (device_index, device_lba) */
static void stripe_map(uint64_t virt_lba, uint32_t *dev_idx,
                       uint64_t *dev_lba)
{
    uint64_t stripe_num;
    uint64_t offset_in_stripe;

    stripe_num = virt_lba / stripe.stripe_sectors;
    offset_in_stripe = virt_lba % stripe.stripe_sectors;

    *dev_idx = (uint32_t)(stripe_num % stripe.num_devices);
    *dev_lba = (stripe_num / stripe.num_devices) * stripe.stripe_sectors
               + offset_in_stripe;
}

/* Read from stripe set -- split requests at stripe boundaries */
static int stripe_read(uint64_t lba, uint32_t count, void *buf,
                       void *driver_data)
{
    uint8_t *ptr = (uint8_t *)buf;
    uint64_t remaining = count;
    uint64_t cur_lba = lba;

    (void)driver_data;
    stripe.reads++;

    while (remaining > 0) {
        uint32_t dev_idx;
        uint64_t dev_lba;
        uint32_t chunk;
        uint64_t offset_in_stripe;
        uint64_t sectors_to_boundary;
        int ret;

        stripe_map(cur_lba, &dev_idx, &dev_lba);

        /* How many sectors until the next stripe boundary? */
        offset_in_stripe = cur_lba % stripe.stripe_sectors;
        sectors_to_boundary = stripe.stripe_sectors - offset_in_stripe;

        chunk = (uint32_t)(remaining < sectors_to_boundary
                           ? remaining : sectors_to_boundary);

        ret = stripe_member_io(&stripe.members[dev_idx],
                               VIRTIO_BLK_T_IN, dev_lba,
                               chunk * topo.blk_size, ptr);
        if (ret != 0)
            return -1;

        if (chunk < count)
            stripe.splits++;

        ptr     += chunk * topo.blk_size;
        cur_lba += chunk;
        remaining -= chunk;
    }

    return 0;
}

/* Write to stripe set */
static int stripe_write(uint64_t lba, uint32_t count, const void *buf,
                        void *driver_data)
{
    const uint8_t *ptr = (const uint8_t *)buf;
    uint64_t remaining = count;
    uint64_t cur_lba = lba;

    (void)driver_data;
    stripe.writes++;

    while (remaining > 0) {
        uint32_t dev_idx;
        uint64_t dev_lba;
        uint32_t chunk;
        uint64_t offset_in_stripe;
        uint64_t sectors_to_boundary;
        int ret;

        stripe_map(cur_lba, &dev_idx, &dev_lba);

        offset_in_stripe = cur_lba % stripe.stripe_sectors;
        sectors_to_boundary = stripe.stripe_sectors - offset_in_stripe;

        chunk = (uint32_t)(remaining < sectors_to_boundary
                           ? remaining : sectors_to_boundary);

        ret = stripe_member_io(&stripe.members[dev_idx],
                               VIRTIO_BLK_T_OUT, dev_lba,
                               chunk * topo.blk_size, (void *)ptr);
        if (ret != 0)
            return -1;

        ptr     += chunk * topo.blk_size;
        cur_lba += chunk;
        remaining -= chunk;
    }

    return 0;
}

/* Flush all member devices */
static int stripe_flush(void *driver_data)
{
    uint32_t i;
    struct virtio_blk_req req;
    uint8_t status;
    int any_fail = 0;

    (void)driver_data;

    req.type     = VIRTIO_BLK_T_FLUSH;
    req.reserved = 0;
    req.sector   = 0;

    for (i = 0; i < stripe.num_devices; i++) {
        if (!stripe.members[i].active)
            continue;

        status = 0xFF;
        /* Simple 2-descriptor flush: header + status */
        if (stripe.members[i].vqs[0].num_free < 2)
            continue;

        {
            struct virtqueue *vq = &stripe.members[i].vqs[0];
            int h, s;
            uint32_t timeout;

            h = vq->free_head;
            vq->free_head = vq->desc[h].next;
            vq->num_free--;

            vq->desc[h].addr  = (uint64_t)(uintptr_t)&req;
            vq->desc[h].len   = sizeof(struct virtio_blk_req);
            vq->desc[h].flags = VIRTQ_DESC_F_NEXT;

            s = vq->free_head;
            vq->free_head = vq->desc[s].next;
            vq->num_free--;

            vq->desc[h].next  = (uint16_t)s;
            vq->desc[s].addr  = (uint64_t)(uintptr_t)&status;
            vq->desc[s].len   = 1;
            vq->desc[s].flags = VIRTQ_DESC_F_WRITE;
            vq->desc[s].next  = 0;

            {
                uint16_t ai = vq->avail->idx % vq->size;
                vq->avail->ring[ai] = (uint16_t)h;
            }
            wmb();
            vq->avail->idx++;
            mb();

            virtq_kick(vq);

            timeout = 5000000;
            while (timeout-- > 0) {
                mb();
                if (vq->used->idx != vq->last_used)
                    break;
                __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
            }

            rmb();
            vq->last_used++;
            virtq_free_desc(vq, (uint16_t)h);
            virtq_free_desc(vq, (uint16_t)s);

            if (status != VIRTIO_BLK_S_OK)
                any_fail = 1;
        }
    }

    return any_fail ? -1 : 0;
}

/* Discard on stripe set: split and route to correct devices */
static int stripe_discard(uint64_t sector, uint32_t num_sectors,
                          void *driver_data)
{
    (void)driver_data;
    (void)sector;
    (void)num_sectors;
    /* Discard across stripe is complex -- defer to per-device discard via
     * the same stripe_map logic. For now, no-op (safe: data is just not trimmed). */
    return 0;
}

/* ---- Public API ---- */

/* Scan for additional VirtIO block devices and attempt stripe assembly.
 * Called from virtio_blk_init() after the primary device is initialized.
 *
 * The primary device (already initialized as blk_dev) occupies
 * (saved_pci_bus, saved_pci_dev, saved_pci_func). We scan for other
 * VirtIO block PCI devices, initialize them minimally, and if they
 * match the primary's capacity, assemble a stripe set. */
void stripe_init(void)
{
    uint8_t bus, slot, func;
    uint32_t found = 0;
    uint32_t i;

    /* Zero state */
    stripe.active = 0;
    stripe.num_devices = 0;
    stripe.stripe_sectors = STRIPE_DEFAULT_SECTORS;
    stripe.total_capacity = 0;
    stripe.reads = 0;
    stripe.writes = 0;
    stripe.splits = 0;
    for (i = 0; i < STRIPE_MAX_DEVICES; i++)
        stripe.members[i].active = 0;

    /* Check if stripe is enabled in Registry (default: false -- opt-in) */
    {
        HKEY hKey = (HKEY)0;
        uint32_t val = 0;

        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\Stripe", 0,
                KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegGetDword(hKey, "Enabled", &val) == ERROR_SUCCESS)
                stripe.enabled = (int)val;
            RegCloseKey(hKey);
        }
    }

    if (!stripe.enabled) {
        klog(LOG_DEBUG, "virtio",
               "Stripe: disabled (opt-in via HKLM\\SYSTEM\\Drivers\\VirtIO\\Stripe\\Enabled)");
        return;
    }

    /* Use opt_io_size for stripe width if available */
    if (topo.opt_io_size > 0) {
        stripe.stripe_sectors = topo.opt_io_size;
        klog(LOG_DEBUG, "virtio", "Stripe: using opt_io_size=%u sectors",
               (uint64_t)stripe.stripe_sectors);
    }

    /* Scan PCI for additional VirtIO block devices */
    for (bus = 0; bus < 8; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (func = 0; func < 8; func++) {
                uint16_t vid, did, subsys;
                uint8_t status_byte;

                /* Skip the primary device */
                if (bus == saved_pci_bus && slot == saved_pci_dev &&
                    func == saved_pci_func)
                    continue;

                vid = pci_read16(bus, slot, func, 0x00);
                did = pci_read16(bus, slot, func, 0x02);

                if (vid != VIRTIO_BLK_VENDOR_ID)
                    continue;
                if (did != VIRTIO_BLK_DEVICE_ID_MOD &&
                    did != VIRTIO_BLK_DEVICE_ID_LEG)
                    continue;

                subsys = pci_read16(bus, slot, func, 0x2E);
                if (did == VIRTIO_BLK_DEVICE_ID_LEG && subsys != 2)
                    continue;

                if (found >= STRIPE_MAX_DEVICES - 1)
                    break;  /* Reserve slot 0 for primary */

                klog(LOG_DEBUG, "virtio",
                       "Stripe: found additional VirtIO-blk at PCI %u:%u.%u",
                       (uint64_t)bus, (uint64_t)slot, (uint64_t)func);

                /* Initialize this device */
                {
                    struct stripe_member *m = &stripe.members[found + 1];
                    uint16_t cmd;

                    m->pci_bus  = bus;
                    m->pci_slot = slot;
                    m->pci_func = func;

                    /* Enable bus mastering */
                    cmd = pci_read16(bus, slot, func, 0x04);
                    cmd |= (1 << 2) | (1 << 1);
                    pci_write16(bus, slot, func, 0x04, cmd);

                    /* Init PCI transport */
                    if (virtio_pci_init(&m->pci_dev, bus, slot, func) != 0) {
                        klog(LOG_DEBUG, "virtio",
                               "Stripe: failed to init PCI transport for %u:%u.%u",
                               (uint64_t)bus, (uint64_t)slot, (uint64_t)func);
                        continue;
                    }

                    /* Reset + ACKNOWLEDGE + DRIVER */
                    virtio_set_status(&m->pci_dev, 0);
                    status_byte = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
                    virtio_set_status(&m->pci_dev, status_byte);

                    /* Accept VIRTIO_F_VERSION_1 */
                    write_driver_features(1, 1);
                    write_driver_features(0, 0);

                    status_byte |= VIRTIO_STATUS_FEATURES_OK;
                    virtio_set_status(&m->pci_dev, status_byte);

                    /* Init single queue */
                    if (virtq_init(&m->vqs[0], &m->pci_dev, 0) != 0) {
                        klog(LOG_DEBUG, "virtio",
                               "Stripe: failed to init queue for %u:%u.%u",
                               (uint64_t)bus, (uint64_t)slot, (uint64_t)func);
                        continue;
                    }

                    /* DRIVER_OK */
                    status_byte |= VIRTIO_STATUS_DRIVER_OK;
                    virtio_set_status(&m->pci_dev, status_byte);

                    /* Read capacity */
                    if (m->pci_dev.device_cfg) {
                        volatile uint32_t *lo = (volatile uint32_t *)
                            (m->pci_dev.device_cfg + VIRTIO_BLK_CFG_CAPACITY);
                        volatile uint32_t *hi = (volatile uint32_t *)
                            (m->pci_dev.device_cfg + VIRTIO_BLK_CFG_CAPACITY + 4);
                        m->capacity = ((uint64_t)mmio_read32(hi) << 32) |
                                      (uint64_t)mmio_read32(lo);
                    }

                    m->serial[0] = '\0';
                    m->active = 1;
                    found++;

                    klog(LOG_DEBUG, "virtio",
                           "Stripe: member %u at PCI %u:%u.%u, capacity=%u MiB",
                           (uint64_t)(found), (uint64_t)bus,
                           (uint64_t)slot, (uint64_t)func,
                           (uint64_t)(m->capacity / 2048));
                }
            }
        }
    }

    if (found == 0) {
        klog(LOG_DEBUG, "virtio", "Stripe: no additional devices found");
        return;
    }

    /* Set up primary device as member 0 */
    stripe.members[0].pci_dev  = blk_dev;
    stripe.members[0].vqs[0]   = blk_vqs[0];
    stripe.members[0].capacity = disk_capacity;
    stripe.members[0].pci_bus  = saved_pci_bus;
    stripe.members[0].pci_slot = saved_pci_dev;
    stripe.members[0].pci_func = saved_pci_func;
    stripe.members[0].active   = 1;
    /* Copy serial */
    {
        uint32_t si;
        for (si = 0; si < VIRTIO_BLK_ID_BYTES && device_serial[si]; si++)
            stripe.members[0].serial[si] = device_serial[si];
        stripe.members[0].serial[si] = '\0';
    }

    stripe.num_devices = found + 1;  /* Primary + additional */

    /* Verify all members have equal capacity */
    {
        uint64_t primary_cap = stripe.members[0].capacity;
        uint32_t matching = 1;

        for (i = 1; i < stripe.num_devices; i++) {
            if (stripe.members[i].capacity != primary_cap) {
                klog(LOG_DEBUG, "virtio",
                       "Stripe: member %u capacity %u != primary %u -- skipping",
                       (uint64_t)i,
                       (uint64_t)(stripe.members[i].capacity / 2048),
                       (uint64_t)(primary_cap / 2048));
                matching = 0;
            }
        }

        if (!matching) {
            klog(LOG_DEBUG, "virtio",
                   "Stripe: capacity mismatch -- stripe not assembled");
            return;
        }
    }

    /* Assemble stripe set */
    stripe.total_capacity = stripe.members[0].capacity * stripe.num_devices;
    stripe.active = 1;

    klog(LOG_DEBUG, "virtio",
           "Stripe: RAID-0 assembled -- %u devices x %u MiB = %u MiB total"
           " (stripe=%u sectors)",
           (uint64_t)stripe.num_devices,
           (uint64_t)(stripe.members[0].capacity / 2048),
           (uint64_t)(stripe.total_capacity / 2048),
           (uint64_t)stripe.stripe_sectors);

    /* Register as virtual blkdev */
    {
        struct blkdev bdev;
        uint32_t si;
        const char *name = "virtio-stripe0";

        for (si = 0; name[si] && si < 15; si++)
            bdev.name[si] = name[si];
        bdev.name[si] = '\0';

        bdev.sector_size  = topo.blk_size;
        bdev.sector_count = stripe.total_capacity;
        bdev.read         = stripe_read;
        bdev.write        = stripe_write;
        bdev.flush        = stripe_flush;
        bdev.discard      = stripe_discard;
        bdev.driver_data  = (void *)&stripe;
        bdev.active       = 1;

        if (blkdev_register(&bdev) == 0) {
            klog(LOG_DEBUG, "virtio",
                   "Stripe: registered blkdev 'virtio-stripe0' (%u MiB)",
                   (uint64_t)(stripe.total_capacity / 2048));
        } else {
            klog(LOG_DEBUG, "virtio",
                   "Stripe: failed to register blkdev");
            stripe.active = 0;
        }
    }

    /* Expose stripe info via Registry */
    {
        HKEY hKey = (HKEY)0;
        uint32_t disp;

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                "HARDWARE\\VirtIO\\Stripe0", 0,
                (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                &hKey, &disp) == ERROR_SUCCESS) {
            RegSetDword(hKey, "NumDevices", stripe.num_devices);
            RegSetDword(hKey, "StripeWidthSectors", stripe.stripe_sectors);
            RegSetDword(hKey, "StripeWidthBytes",
                        stripe.stripe_sectors * topo.blk_size);
            RegSetDword(hKey, "TotalCapacityMiB",
                        (uint32_t)(stripe.total_capacity / 2048));
            RegSetDword(hKey, "MemberCapacityMiB",
                        (uint32_t)(stripe.members[0].capacity / 2048));
            RegCloseKey(hKey);
        }

        /* Write config defaults */
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\Stripe", 0,
                (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                &hKey, &disp) == ERROR_SUCCESS) {
            RegSetDword(hKey, "Enabled", 1);
            RegCloseKey(hKey);
        }
    }
}

/* Check if stripe set is active */
int stripe_is_active(void)
{
    return stripe.active;
}

/* Expose stripe stats to Registry */
void stripe_expose_registry(void)
{
    HKEY hKey = (HKEY)0;
    uint32_t disp;

    if (!stripe.active)
        return;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "HARDWARE\\VirtIO\\Stripe0\\Stats", 0,
            (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
            &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "Reads", stripe.reads);
        RegSetDword(hKey, "Writes", stripe.writes);
        RegSetDword(hKey, "Splits", stripe.splits);
        RegCloseKey(hKey);
    }
}
