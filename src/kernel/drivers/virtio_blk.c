/* ============================================================================
 * virtio_blk.c — VirtIO Block Device Driver (Modern 1.0 Transport)
 *
 * Uses the modern VirtIO PCI transport from virtio.c for MMIO-based
 * configuration, notification, and split virtqueue management.
 *
 * Init sequence (VirtIO 1.0 §3.1.1):
 *   1. Reset device (status = 0)
 *   2. Set ACKNOWLEDGE
 *   3. Set DRIVER
 *   4. Read/negotiate features
 *   5. Set FEATURES_OK
 *   6. Set up virtqueue 0 (request queue)
 *   7. Set DRIVER_OK
 *
 * I/O: 3-descriptor chain per request (header, data, status).
 * Flush: 2-descriptor chain (header, status) — no data buffer.
 * ============================================================================ */

#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/virtio.h"
#include "kernel/drivers/pci.h"
#include "kernel/irq.h"
#include "kernel/klog.h"
#include "kernel/printk.h"

/* ---- Driver state ---- */
static struct virtio_pci_dev  blk_dev;         /* Modern PCI transport */
static struct virtqueue       blk_vq;          /* Request queue (queue 0) */
static uint64_t               disk_capacity;   /* Total 512-byte sectors */
static int                    initialized;     /* 1 if init succeeded */
static volatile int           virtio_irq_fired; /* Queue completion flag */
static uint8_t                msix_vec_queue;  /* MSI-X IDT vector: queue */
static uint8_t                msix_vec_config; /* MSI-X IDT vector: config */

/* Feature negotiation results */
static int                    has_flush;       /* F_FLUSH negotiated */
static int                    has_config_wce;  /* F_CONFIG_WCE negotiated */
static int                    has_blk_size;    /* F_BLK_SIZE negotiated */
static int                    has_topology;    /* F_TOPOLOGY negotiated */
static int                    has_size_max;    /* F_SIZE_MAX negotiated */
static int                    has_seg_max;     /* F_SEG_MAX negotiated */
static int                    is_read_only;    /* F_RO detected */

/* Block size and topology info */
static struct virtio_blk_topology topo;

/* ---- MSI-X IRQ handlers ---- */

/* Queue completion interrupt — device placed buffers in the used ring */
static void virtio_blk_queue_irq(uint8_t vector, void *ctx)
{
    (void)vector;
    (void)ctx;
    virtio_irq_fired = 1;
}

/* Config change interrupt — device resized, topology changed, etc. */
static void virtio_blk_config_irq(uint8_t vector, void *ctx)
{
    (void)vector;
    (void)ctx;
    /* Read ISR to acknowledge; config change handling is TODO §14.1 */
    if (blk_dev.isr_cfg) {
        (void)virtio_read_isr(&blk_dev);
    }
    klog(LOG_DEBUG, "virtio", "Config change interrupt received");
}

/* ---- Feature negotiation ---- */
static uint32_t read_device_features(uint32_t page)
{
    volatile uint8_t *cfg = blk_dev.common_cfg;
    mmio_write32((volatile uint32_t *)(cfg + VIRTIO_COMMON_DFSELECT), page);
    return mmio_read32((volatile uint32_t *)(cfg + VIRTIO_COMMON_DF));
}

static void write_driver_features(uint32_t page, uint32_t features)
{
    volatile uint8_t *cfg = blk_dev.common_cfg;
    mmio_write32((volatile uint32_t *)(cfg + VIRTIO_COMMON_GFSELECT), page);
    mmio_write32((volatile uint32_t *)(cfg + VIRTIO_COMMON_GF), features);
}

/* ---- Block I/O ---- */
static int virtio_blk_do_io(uint32_t type, uint64_t sector,
                             uint32_t len, void *buffer)
{
    struct virtio_blk_req req;
    uint8_t status_byte = 0xFF;
    int d0, d1, d2;
    uint32_t timeout;
    uint64_t rflags;

    if (!initialized)
        return -1;

    /* Enforce size_max: reject single-segment I/O exceeding device limit */
    if (topo.size_max > 0 && len > topo.size_max) {
        klog(LOG_DEBUG, "virtio", "I/O size %u exceeds size_max %u",
               (uint64_t)len, (uint64_t)topo.size_max);
        return -1;
    }

    /* Build request header */
    req.type     = type;
    req.reserved = 0;
    req.sector   = sector;

    /* Allocate 3 descriptors from the virtqueue free list */
    if (blk_vq.num_free < 3) {
        klog(LOG_DEBUG, "virtio", "No free descriptors");
        return -1;
    }

    /* Descriptor 0: request header (device-readable) */
    d0 = blk_vq.free_head;
    blk_vq.free_head = blk_vq.desc[d0].next;
    blk_vq.num_free--;

    blk_vq.desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    blk_vq.desc[d0].len   = sizeof(struct virtio_blk_req);
    blk_vq.desc[d0].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 1: data buffer */
    d1 = blk_vq.free_head;
    blk_vq.free_head = blk_vq.desc[d1].next;
    blk_vq.num_free--;

    blk_vq.desc[d0].next = (uint16_t)d1;

    blk_vq.desc[d1].addr  = (uint64_t)(uintptr_t)buffer;
    blk_vq.desc[d1].len   = len;
    blk_vq.desc[d1].flags = VIRTQ_DESC_F_NEXT;
    if (type == VIRTIO_BLK_T_IN)
        blk_vq.desc[d1].flags |= VIRTQ_DESC_F_WRITE;  /* device writes to buf */

    /* Descriptor 2: status byte (device-writable) */
    d2 = blk_vq.free_head;
    blk_vq.free_head = blk_vq.desc[d2].next;
    blk_vq.num_free--;

    blk_vq.desc[d1].next = (uint16_t)d2;

    blk_vq.desc[d2].addr  = (uint64_t)(uintptr_t)&status_byte;
    blk_vq.desc[d2].len   = 1;
    blk_vq.desc[d2].flags = VIRTQ_DESC_F_WRITE;
    blk_vq.desc[d2].next  = 0;

    /* Add chain head to available ring */
    uint16_t avail_idx = blk_vq.avail->idx % blk_vq.size;
    blk_vq.avail->ring[avail_idx] = (uint16_t)d0;

    __asm__ volatile ("mfence" ::: "memory");
    blk_vq.avail->idx++;
    __asm__ volatile ("mfence" ::: "memory");

    /* Enable interrupts for IRQ delivery. Save current RFLAGS first. */
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    virtio_irq_fired = 0;
    __asm__ volatile ("sti");

    /* Notify device (modern MMIO notification) */
    virtq_kick(&blk_vq);

    /* Poll for completion */
    timeout = 5000000;
    while (timeout-- > 0) {
        __asm__ volatile ("mfence" ::: "memory");
        if (blk_vq.used->idx != blk_vq.last_used)
            break;
        /* Yield CPU briefly — read a port to create a ~1µs delay */
        __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
    }

    /* Restore interrupt state if it was previously disabled (bit 9 in RFLAGS) */
    if (!(rflags & (1 << 9))) {
        __asm__ volatile ("cli");
    }

    if (timeout == 0) {
        klog(LOG_DEBUG, "virtio", "I/O timeout (avail=%u, used=%u, status=%x)",
               (uint64_t)blk_vq.avail->idx, (uint64_t)blk_vq.used->idx,
               (uint64_t)status_byte);
        /* Free descriptors */
        virtq_free_desc(&blk_vq, (uint16_t)d0);
        virtq_free_desc(&blk_vq, (uint16_t)d1);
        virtq_free_desc(&blk_vq, (uint16_t)d2);
        return -1;
    }

    /* Consume the used ring entry */
    blk_vq.last_used++;

    /* Free all three descriptors */
    virtq_free_desc(&blk_vq, (uint16_t)d0);
    virtq_free_desc(&blk_vq, (uint16_t)d1);
    virtq_free_desc(&blk_vq, (uint16_t)d2);

    return (status_byte == VIRTIO_BLK_S_OK) ? 0 : -1;
}

/* ---- Flush I/O (2-descriptor chain: header + status, no data) ---- */
static int virtio_blk_do_flush(void)
{
    struct virtio_blk_req req;
    uint8_t status_byte = 0xFF;
    int d0, d1;
    uint32_t timeout;
    uint64_t rflags;

    if (!initialized)
        return -1;

    /* Build flush request header — sector field is ignored */
    req.type     = VIRTIO_BLK_T_FLUSH;
    req.reserved = 0;
    req.sector   = 0;

    /* Allocate 2 descriptors from the virtqueue free list */
    if (blk_vq.num_free < 2) {
        klog(LOG_DEBUG, "virtio", "No free descriptors for flush");
        return -1;
    }

    /* Descriptor 0: request header (device-readable) */
    d0 = blk_vq.free_head;
    blk_vq.free_head = blk_vq.desc[d0].next;
    blk_vq.num_free--;

    blk_vq.desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    blk_vq.desc[d0].len   = sizeof(struct virtio_blk_req);
    blk_vq.desc[d0].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 1: status byte (device-writable) — no data descriptor */
    d1 = blk_vq.free_head;
    blk_vq.free_head = blk_vq.desc[d1].next;
    blk_vq.num_free--;

    blk_vq.desc[d0].next = (uint16_t)d1;

    blk_vq.desc[d1].addr  = (uint64_t)(uintptr_t)&status_byte;
    blk_vq.desc[d1].len   = 1;
    blk_vq.desc[d1].flags = VIRTQ_DESC_F_WRITE;
    blk_vq.desc[d1].next  = 0;

    /* Add chain head to available ring */
    uint16_t avail_idx = blk_vq.avail->idx % blk_vq.size;
    blk_vq.avail->ring[avail_idx] = (uint16_t)d0;

    __asm__ volatile ("mfence" ::: "memory");
    blk_vq.avail->idx++;
    __asm__ volatile ("mfence" ::: "memory");

    /* Enable interrupts for IRQ delivery */
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    virtio_irq_fired = 0;
    __asm__ volatile ("sti");

    /* Notify device */
    virtq_kick(&blk_vq);

    /* Poll for completion — flush may take longer than normal I/O */
    timeout = 10000000;
    while (timeout-- > 0) {
        __asm__ volatile ("mfence" ::: "memory");
        if (blk_vq.used->idx != blk_vq.last_used)
            break;
        __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
    }

    /* Restore interrupt state */
    if (!(rflags & (1 << 9))) {
        __asm__ volatile ("cli");
    }

    if (timeout == 0) {
        klog(LOG_DEBUG, "virtio", "Flush timeout");
        virtq_free_desc(&blk_vq, (uint16_t)d0);
        virtq_free_desc(&blk_vq, (uint16_t)d1);
        return -1;
    }

    /* Consume the used ring entry */
    blk_vq.last_used++;

    /* Free both descriptors */
    virtq_free_desc(&blk_vq, (uint16_t)d0);
    virtq_free_desc(&blk_vq, (uint16_t)d1);

    return (status_byte == VIRTIO_BLK_S_OK) ? 0 : -1;
}

/* ---- Public API ---- */

int virtio_blk_read(uint64_t lba, uint32_t count, void *buffer)
{
    return virtio_blk_do_io(VIRTIO_BLK_T_IN, lba, count * topo.blk_size, buffer);
}

int virtio_blk_write(uint64_t lba, uint32_t count, const void *buffer)
{
    if (is_read_only)
        return -1;  /* Device is read-only */
    return virtio_blk_do_io(VIRTIO_BLK_T_OUT, lba, count * topo.blk_size, (void *)buffer);
}

int virtio_blk_flush(void)
{
    if (!initialized)
        return -1;

    /* If F_FLUSH was not negotiated, flush is a no-op (writethrough mode) */
    if (!has_flush)
        return 1;

    /* Read-only device has nothing to flush */
    if (is_read_only)
        return 0;

    return virtio_blk_do_flush();
}

int virtio_blk_set_write_cache(int enable)
{
    if (!initialized || !has_config_wce)
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
    return initialized;
}

uint32_t virtio_blk_block_size(void)
{
    return topo.blk_size;
}

const struct virtio_blk_topology *virtio_blk_topology(void)
{
    return &topo;
}

/* ---- Initialization ---- */

int virtio_blk_init(void)
{
    struct pci_device dev;
    int found = 0;
    uint8_t bus, slot, func;
    uint32_t feat_lo;
    uint32_t driver_feat_lo;
    uint8_t status;

    /* Default topology — 512-byte sectors, no limits */
    topo.blk_size = 512;
    topo.physical_block_exp = 0;
    topo.alignment_offset = 0;
    topo.min_io_size = 0;
    topo.opt_io_size = 0;
    topo.size_max = 0;
    topo.seg_max = 0;

    /* Scan PCI for virtio-blk: modern ID 0x1042 or transitional ID 0x1001 */
    for (bus = 0; bus < 8 && !found; bus++) {
        for (slot = 0; slot < 32 && !found; slot++) {
            for (func = 0; func < 8 && !found; func++) {
                uint16_t vid = pci_read16(bus, slot, func, 0x00);
                uint16_t did = pci_read16(bus, slot, func, 0x02);

                if (vid != VIRTIO_BLK_VENDOR_ID)
                    continue;
                if (did != VIRTIO_BLK_DEVICE_ID_MOD &&
                    did != VIRTIO_BLK_DEVICE_ID_LEG)
                    continue;

                /* Check subsystem ID for block device (subsys = 2) */
                uint16_t subsys = pci_read16(bus, slot, func, 0x2E);
                if (did == VIRTIO_BLK_DEVICE_ID_LEG && subsys != 2)
                    continue;

                dev.vendor_id = vid;
                dev.device_id = did;
                dev.bus    = bus;
                dev.dev   = slot;
                dev.func   = func;
                found = 1;
            }
        }
    }

    if (!found) {
        klog(LOG_DEBUG, "virtio", "No virtio-blk device found");
        return -1;
    }

    klog(LOG_DEBUG, "virtio", "Found at PCI %u:%u.%u (devID=%x)",
           (uint64_t)dev.bus, (uint64_t)dev.dev, (uint64_t)dev.func,
           (uint64_t)dev.device_id);

    /* Enable bus mastering and memory space */
    {
        uint16_t cmd = pci_read16(dev.bus, dev.dev, dev.func, 0x04);
        cmd |= (1 << 2) | (1 << 1);  /* Bus Master + Memory Space */
        pci_write16(dev.bus, dev.dev, dev.func, 0x04, cmd);
    }

    /* Initialize modern PCI transport (walk capabilities, map BARs) */
    if (virtio_pci_init(&blk_dev, dev.bus, dev.dev, dev.func) != 0) {
        klog(LOG_DEBUG, "virtio", "Failed to init modern PCI transport");
        return -1;
    }

    /* Step 1: Reset device */
    virtio_set_status(&blk_dev, 0);

    /* Step 2: ACKNOWLEDGE */
    status = VIRTIO_STATUS_ACKNOWLEDGE;
    virtio_set_status(&blk_dev, status);

    /* Step 3: DRIVER */
    status |= VIRTIO_STATUS_DRIVER;
    virtio_set_status(&blk_dev, status);

    /* Step 4: Read and negotiate features */
    feat_lo = read_device_features(0);
    klog(LOG_DEBUG, "virtio", "Device features[0]: %x", (uint64_t)feat_lo);

    /* Build driver feature set — accept features we support */
    driver_feat_lo = 0;

    /* Negotiate F_FLUSH (bit 6): cache flush for write barriers */
    if (feat_lo & (1u << VIRTIO_BLK_F_FLUSH)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_FLUSH);
        has_flush = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_FLUSH (write barriers)");
    }

    /* Negotiate F_CONFIG_WCE (bit 9): writeback cache control */
    if (feat_lo & (1u << VIRTIO_BLK_F_CONFIG_WCE)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_CONFIG_WCE);
        has_config_wce = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_CONFIG_WCE (write cache control)");
    }

    /* Negotiate F_BLK_SIZE (bit 5): logical block size */
    if (feat_lo & (1u << VIRTIO_BLK_F_BLK_SIZE)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_BLK_SIZE);
        has_blk_size = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_BLK_SIZE");
    }

    /* Negotiate F_TOPOLOGY (bit 7): physical block topology */
    if (feat_lo & (1u << VIRTIO_BLK_F_TOPOLOGY)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_TOPOLOGY);
        has_topology = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_TOPOLOGY");
    }

    /* Negotiate F_SIZE_MAX (bit 0): max segment size */
    if (feat_lo & (1u << VIRTIO_BLK_F_SIZE_MAX)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_SIZE_MAX);
        has_size_max = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_SIZE_MAX");
    }

    /* Negotiate F_SEG_MAX (bit 1): max segments per request */
    if (feat_lo & (1u << VIRTIO_BLK_F_SEG_MAX)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_SEG_MAX);
        has_seg_max = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_SEG_MAX");
    }

    /* Check F_RO (bit 4): read-only device — accept the bit to acknowledge */
    if (feat_lo & (1u << VIRTIO_BLK_F_RO)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_RO);
        is_read_only = 1;
        klog(LOG_DEBUG, "virtio", "Device is READ-ONLY (F_RO)");
    }

    /* Accept VIRTIO_F_VERSION_1 (bit 0 of page 1) */
    {
        uint32_t feat_hi = read_device_features(1);
        (void)feat_hi;
        /* We must negotiate VERSION_1 for modern transport */
        write_driver_features(1, 1);  /* Bit 0 of page 1 = VIRTIO_F_VERSION_1 */
        write_driver_features(0, driver_feat_lo);
    }

    /* Step 5: FEATURES_OK (modern transport requires this) */
    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_set_status(&blk_dev, status);

    /* Verify FEATURES_OK was accepted */
    {
        uint8_t cur = virtio_get_status(&blk_dev);
        if (!(cur & VIRTIO_STATUS_FEATURES_OK)) {
            klog(LOG_DEBUG, "virtio", "FEATURES_OK not accepted by device");
            virtio_set_status(&blk_dev, VIRTIO_STATUS_FAILED);
            return -1;
        }
    }

    /* Step 6: Set up virtqueue 0 (request queue) */
    if (virtq_init(&blk_vq, &blk_dev, 0) != 0) {
        klog(LOG_DEBUG, "virtio", "Failed to init request queue");
        virtio_set_status(&blk_dev, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Step 7: DRIVER_OK — device is live */
    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_set_status(&blk_dev, status);

    /* Read disk capacity from device-specific config MMIO.
     * Use config_generation counter for atomic read — a live-resize
     * event between the two 32-bit MMIO reads would corrupt the
     * 64-bit value.  Retry if generation changed. */
    if (blk_dev.device_cfg) {
        uint8_t gen1, gen2;
        do {
            gen1 = virtio_read_config_generation(&blk_dev);
            volatile uint32_t *cap_lo = (volatile uint32_t *)
                (blk_dev.device_cfg + VIRTIO_BLK_CFG_CAPACITY);
            volatile uint32_t *cap_hi = (volatile uint32_t *)
                (blk_dev.device_cfg + VIRTIO_BLK_CFG_CAPACITY + 4);
            disk_capacity = ((uint64_t)mmio_read32(cap_hi) << 32) |
                            (uint64_t)mmio_read32(cap_lo);
            gen2 = virtio_read_config_generation(&blk_dev);
        } while (gen1 != gen2);
    }

    /* Read writeback cache mode if F_CONFIG_WCE negotiated */
    if (has_config_wce && blk_dev.device_cfg) {
        volatile uint8_t *wb = (volatile uint8_t *)
            (blk_dev.device_cfg + VIRTIO_BLK_CFG_WRITEBACK);
        uint8_t wb_val = mmio_read8(wb);
        klog(LOG_DEBUG, "virtio", "Write cache: %s",
               wb_val ? "writeback" : "writethrough");
    }

    /* Read block size if F_BLK_SIZE negotiated */
    if (has_blk_size && blk_dev.device_cfg) {
        volatile uint32_t *bs = (volatile uint32_t *)
            (blk_dev.device_cfg + VIRTIO_BLK_CFG_BLK_SIZE);
        topo.blk_size = mmio_read32(bs);
        if (topo.blk_size == 0)
            topo.blk_size = 512;  /* Sanity: never allow zero */
        klog(LOG_DEBUG, "virtio", "Block size: %u bytes",
               (uint64_t)topo.blk_size);
    }

    /* Read topology if F_TOPOLOGY negotiated */
    if (has_topology && blk_dev.device_cfg) {
        topo.physical_block_exp = mmio_read8(
            (volatile uint8_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_PHYS_BLK_EXP));
        topo.alignment_offset = mmio_read8(
            (volatile uint8_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_ALIGN_OFFSET));
        topo.min_io_size = mmio_read16(
            (volatile uint16_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_MIN_IO_SIZE));
        topo.opt_io_size = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_OPT_IO_SIZE));
        klog(LOG_DEBUG, "virtio", "Topology: phys_exp=%u align=%u min_io=%u opt_io=%u",
               (uint64_t)topo.physical_block_exp,
               (uint64_t)topo.alignment_offset,
               (uint64_t)topo.min_io_size,
               (uint64_t)topo.opt_io_size);
    }

    /* Read segment limits if F_SIZE_MAX / F_SEG_MAX negotiated */
    if (has_size_max && blk_dev.device_cfg) {
        topo.size_max = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_SIZE_MAX));
        klog(LOG_DEBUG, "virtio", "Max segment size: %u bytes",
               (uint64_t)topo.size_max);
    }
    if (has_seg_max && blk_dev.device_cfg) {
        topo.seg_max = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_SEG_MAX));
        klog(LOG_DEBUG, "virtio", "Max segments/request: %u",
               (uint64_t)topo.seg_max);
    }

    /* Check for DEVICE_NEEDS_RESET — abort if device signalled failure */
    {
        uint8_t cur = virtio_get_status(&blk_dev);
        if (cur & VIRTIO_STATUS_DEVICE_NEEDS_RESET) {
            klog(LOG_DEBUG, "virtio", "Device needs reset after init — aborting");
            virtio_set_status(&blk_dev, VIRTIO_STATUS_FAILED);
            return -1;
        }
    }

    initialized = 1;

    /* Set up MSI-X interrupts (replaces legacy PIC — rules.md APIC-only) */
    if (virtio_pci_setup_msix(&blk_dev, dev.bus, dev.dev, dev.func,
                              &msix_vec_queue, &msix_vec_config) != 0) {
        klog(LOG_DEBUG, "virtio", "MSI-X setup failed — falling back to polling only");
        /* Polling-only mode still works via the timeout loop in do_io */
    } else {
        /* Register IRQ handlers for both MSI-X vectors */
        irq_register(msix_vec_queue, virtio_blk_queue_irq, NULL, "virtio-blk-q0");
        irq_register(msix_vec_config, virtio_blk_config_irq, NULL, "virtio-blk-cfg");
    }

    klog(LOG_DEBUG, "virtio", "VirtIO-blk: %u MiB (%u sectors), blk_size=%u, opt_io=%u%s%s",
           (uint64_t)(disk_capacity / 2048),
           (uint64_t)disk_capacity,
           (uint64_t)topo.blk_size,
           (uint64_t)topo.opt_io_size,
           has_flush ? ", flush" : "",
           has_config_wce ? ", wce" : "");

    return 0;
}
