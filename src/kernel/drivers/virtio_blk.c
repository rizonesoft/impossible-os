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
 * ============================================================================ */

#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/virtio.h"
#include "kernel/drivers/pci.h"
#include "kernel/mm/heap.h"
#include "kernel/printk.h"
#include "kernel/idt.h"

/* Forward declaration */
struct interrupt_frame;

/* ---- Driver state ---- */
static struct virtio_pci_dev  blk_dev;         /* Modern PCI transport */
static struct virtqueue       blk_vq;          /* Request queue (queue 0) */
static uint64_t               disk_capacity;   /* Total 512-byte sectors */
static int                    initialized;     /* 1 if init succeeded */
static volatile int           virtio_irq_fired; /* IRQ flag */
static uint8_t                blk_irq_line;    /* PCI IRQ line */

/* ---- IRQ handler ---- */
static uint64_t virtio_blk_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    /* Read ISR status to acknowledge the interrupt */
    if (blk_dev.isr_cfg) {
        (void)virtio_read_isr(&blk_dev);
    }
    virtio_irq_fired = 1;
    return 0;
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

    if (!initialized)
        return -1;

    /* Build request header */
    req.type     = type;
    req.reserved = 0;
    req.sector   = sector;

    /* Allocate 3 descriptors from the virtqueue free list */
    if (blk_vq.num_free < 3) {
        printk("[VIRTIO-BLK] No free descriptors\n");
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

    /* Enable interrupts for IRQ delivery */
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

    __asm__ volatile ("cli");

    if (timeout == 0) {
        printk("[VIRTIO-BLK] I/O timeout (avail=%u, used=%u, status=%x)\n",
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

/* ---- Public API ---- */

int virtio_blk_read(uint64_t lba, uint32_t count, void *buffer)
{
    return virtio_blk_do_io(VIRTIO_BLK_T_IN, lba, count * 512, buffer);
}

int virtio_blk_write(uint64_t lba, uint32_t count, const void *buffer)
{
    return virtio_blk_do_io(VIRTIO_BLK_T_OUT, lba, count * 512, (void *)buffer);
}

uint64_t virtio_blk_capacity(void)
{
    return disk_capacity;
}

int virtio_blk_present(void)
{
    return initialized;
}

/* ---- Initialization ---- */

int virtio_blk_init(void)
{
    struct pci_device dev;
    int found = 0;
    uint8_t bus, slot, func;
    uint32_t feat_lo;
    uint8_t status;

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
        printk("[VIRTIO-BLK] No virtio-blk device found\n");
        return -1;
    }

    printk("[VIRTIO-BLK] Found at PCI %u:%u.%u (devID=%x)\n",
           (uint64_t)dev.bus, (uint64_t)dev.dev, (uint64_t)dev.func,
           (uint64_t)dev.device_id);

    /* Enable bus mastering and memory space */
    {
        uint16_t cmd = pci_read16(dev.bus, dev.dev, dev.func, 0x04);
        cmd |= (1 << 2) | (1 << 1);  /* Bus Master + Memory Space */
        pci_write16(dev.bus, dev.dev, dev.func, 0x04, cmd);
    }

    /* Read IRQ line for interrupt handling */
    blk_irq_line = pci_read8(dev.bus, dev.dev, dev.func, 0x3C);

    /* Initialize modern PCI transport (walk capabilities, map BARs) */
    if (virtio_pci_init(&blk_dev, dev.bus, dev.dev, dev.func) != 0) {
        printk("[VIRTIO-BLK] Failed to init modern PCI transport\n");
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
    printk("[VIRTIO-BLK] Device features[0]: %x\n", (uint64_t)feat_lo);

    /* Accept VIRTIO_F_VERSION_1 (bit 0 of page 1) */
    {
        uint32_t feat_hi = read_device_features(1);
        (void)feat_hi;
        /* We must negotiate VERSION_1 for modern transport */
        write_driver_features(1, 1);  /* Bit 0 of page 1 = VIRTIO_F_VERSION_1 */
        write_driver_features(0, 0);  /* No optional block features needed */
    }

    /* Step 5: FEATURES_OK (modern transport requires this) */
    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_set_status(&blk_dev, status);

    /* Verify FEATURES_OK was accepted */
    {
        uint8_t cur = virtio_get_status(&blk_dev);
        if (!(cur & VIRTIO_STATUS_FEATURES_OK)) {
            printk("[VIRTIO-BLK] FEATURES_OK not accepted by device\n");
            virtio_set_status(&blk_dev, VIRTIO_STATUS_FAILED);
            return -1;
        }
    }

    /* Step 6: Set up virtqueue 0 (request queue) */
    if (virtq_init(&blk_vq, &blk_dev, 0) != 0) {
        printk("[VIRTIO-BLK] Failed to init request queue\n");
        virtio_set_status(&blk_dev, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Step 7: DRIVER_OK — device is live */
    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_set_status(&blk_dev, status);

    /* Read disk capacity from device-specific config MMIO */
    if (blk_dev.device_cfg) {
        volatile uint32_t *cap_lo = (volatile uint32_t *)
            (blk_dev.device_cfg + VIRTIO_BLK_CFG_CAPACITY);
        volatile uint32_t *cap_hi = (volatile uint32_t *)
            (blk_dev.device_cfg + VIRTIO_BLK_CFG_CAPACITY + 4);
        disk_capacity = ((uint64_t)mmio_read32(cap_hi) << 32) |
                        (uint64_t)mmio_read32(cap_lo);
    }

    initialized = 1;

    /* Register IRQ handler */
    if (blk_irq_line < 16) {
        idt_register_handler(32 + blk_irq_line, virtio_blk_irq_handler);
        /* Unmask the IRQ on the PIC */
        if (blk_irq_line < 8) {
            uint8_t mask;
            __asm__ volatile ("inb $0x21, %0" : "=a"(mask));
            mask &= ~(1 << blk_irq_line);
            __asm__ volatile ("outb %0, $0x21" :: "a"(mask));
        } else {
            uint8_t mask;
            __asm__ volatile ("inb $0xA1, %0" : "=a"(mask));
            mask &= ~(1 << (blk_irq_line - 8));
            __asm__ volatile ("outb %0, $0xA1" :: "a"(mask));
            /* Ensure cascade IRQ 2 is unmasked */
            __asm__ volatile ("inb $0x21, %0" : "=a"(mask));
            mask &= ~(1 << 2);
            __asm__ volatile ("outb %0, $0x21" :: "a"(mask));
        }
    }

    printk("[OK] VirtIO-blk: %u MiB (%u sectors)\n",
           (uint64_t)(disk_capacity / 2048),
           (uint64_t)disk_capacity);

    return 0;
}
