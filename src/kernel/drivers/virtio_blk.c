/* ============================================================================
 * virtio_blk.c — VirtIO Block Device Driver
 *
 * VirtIO legacy (0.9.5) PIO-based block device driver.
 *
 * Initialization:
 *   1. Find device via PCI (0x1AF4:0x1001)
 *   2. Enable bus mastering
 *   3. Read BAR0 for I/O port base
 *   4. Reset device, set ACKNOWLEDGE + DRIVER status
 *   5. Negotiate features (none needed for basic I/O)
 *   6. Allocate and configure virtqueue 0 (request queue)
 *   7. Set DRIVER_OK
 *
 * I/O flow (synchronous, polling):
 *   1. Build a 3-descriptor chain: header → data → status
 *   2. Add to available ring, notify device
 *   3. Poll used ring until device responds
 *   4. Check status byte
 * ============================================================================ */

#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/pci.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/heap.h"
#include "kernel/idt.h"
#include "kernel/printk.h"

/* --- Port I/O helpers --- */
static inline void vio_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void vio_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void vio_outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t vio_inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t vio_inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t vio_inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* --- Driver state --- */
static uint16_t io_base = 0;         /* BAR0 I/O port base */
static struct virtqueue vq;          /* request queue (queue 0) */
static uint64_t disk_capacity = 0;   /* total sectors */
static int initialized = 0;
static volatile int virtio_irq_fired = 0;  /* set by IRQ handler */

/* --- VirtIO IRQ handler (IDT 43 = PCI IRQ 11) --- */
static uint64_t virtio_irq_handler(struct interrupt_frame *frame)
{
    /* Read ISR status to acknowledge the virtio interrupt */
    if (io_base)
        (void)vio_inb(io_base + VIRTIO_ISR_STATUS);

    virtio_irq_fired = 1;

    /* Send EOI to PIC (IRQ 11 is on slave PIC, need EOI to both) */
    vio_outb(0xA0, 0x20);  /* slave PIC EOI */
    vio_outb(0x20, 0x20);  /* master PIC EOI */

    return (uint64_t)frame;
}

/* Align up to 'align' (must be power of 2) */
static inline uintptr_t align_up(uintptr_t val, uintptr_t align)
{
    return (val + align - 1) & ~(align - 1);
}

/* --- Virtqueue setup --- */

static int virtqueue_init(uint16_t queue_index)
{
    uint16_t qsize;
    uintptr_t desc_addr, avail_addr, used_addr;
    uint32_t total_size;
    uint32_t i;
    uint8_t *raw_mem;

    /* Select queue */
    vio_outw(io_base + VIRTIO_QUEUE_SELECT, queue_index);

    /* Read queue size */
    qsize = vio_inw(io_base + VIRTIO_QUEUE_SIZE);
    if (qsize == 0) {
        printk("[VIRTIO] Queue %u: size = 0 (not available)\n",
               (uint64_t)queue_index);
        return -1;
    }

    if (qsize > VIRTIO_QUEUE_SIZE_MAX)
        qsize = VIRTIO_QUEUE_SIZE_MAX;

    vq.size = qsize;

    /* Calculate total memory needed.
     * Desc table: 16 * qsize
     * Avail ring: 6 + 2 * qsize
     * (pad to page boundary)
     * Used ring: 6 + 8 * qsize
     * (pad to page boundary) */
    {
        uintptr_t desc_sz = (uintptr_t)qsize * 16;
        uintptr_t avail_sz = 6 + (uintptr_t)qsize * 2;
        uintptr_t used_off = align_up(desc_sz + avail_sz, 4096);
        uintptr_t used_sz = 6 + (uintptr_t)qsize * 8;
        total_size = (uint32_t)align_up(used_off + used_sz, 4096);
    }

    /* Allocate contiguous memory + extra page for alignment */
    raw_mem = (uint8_t *)kmalloc(total_size + 4096);
    if (!raw_mem) {
        printk("[VIRTIO] Failed to allocate virtqueue memory\n");
        return -1;
    }

    /* Page-align */
    desc_addr = align_up((uintptr_t)raw_mem, 4096);

    /* Zero the entire allocation */
    {
        uint8_t *p = (uint8_t *)desc_addr;
        for (i = 0; i < total_size; i++)
            p[i] = 0;
    }

    /* Set pointers */
    vq.desc = (struct vring_desc *)desc_addr;

    avail_addr = desc_addr + (uintptr_t)qsize * 16;
    vq.avail = (struct vring_avail *)avail_addr;

    {
        uintptr_t desc_sz = (uintptr_t)qsize * 16;
        uintptr_t avail_sz = 6 + (uintptr_t)qsize * 2;
        uintptr_t used_off = align_up(desc_sz + avail_sz, 4096);
        used_addr = desc_addr + used_off;
        vq.used = (struct vring_used *)used_addr;
    }

    /* Build free list */
    for (i = 0; i < (uint32_t)qsize; i++) {
        vq.desc[i].next = (uint16_t)(i + 1);
        vq.desc[i].flags = 0;
    }
    vq.free_head = 0;
    vq.last_used = 0;
    vq.num_free = qsize;

    /* Tell device where queue lives (PFN) */
    vio_outl(io_base + VIRTIO_QUEUE_ADDRESS, (uint32_t)(desc_addr >> 12));

    printk("[VIRTIO] Queue %u: size=%u desc=%p avail=%p used=%p PFN=%x\n",
           (uint64_t)queue_index, (uint64_t)qsize,
           desc_addr, avail_addr, used_addr,
           (uint64_t)(uint32_t)(desc_addr >> 12));

    return 0;
}

/* Allocate a descriptor from the free list */
static int vq_alloc_desc(void)
{
    uint16_t idx;
    if (vq.num_free == 0)
        return -1;
    idx = vq.free_head;
    vq.free_head = vq.desc[idx].next;
    vq.num_free--;
    return (int)idx;
}

/* Free a descriptor back to the free list */
static void vq_free_desc(uint16_t idx)
{
    vq.desc[idx].next = vq.free_head;
    vq.desc[idx].flags = 0;
    vq.free_head = idx;
    vq.num_free++;
}

/* --- Block I/O core --- */

static int virtio_blk_do_io(uint32_t type, uint64_t sector,
                             void *buffer, uint32_t len)
{
    struct virtio_blk_req *req;
    uint8_t *status_ptr;
    int d0, d1, d2;
    uint16_t avail_idx;
    uint32_t timeout;
    int result = -1;

    if (!initialized)
        return -1;

    /* Heap-allocated to ensure DMA-accessible identity-mapped addresses */
    req = (struct virtio_blk_req *)kmalloc(sizeof(struct virtio_blk_req));
    status_ptr = (uint8_t *)kmalloc(16);
    if (!req || !status_ptr) {
        if (req) kfree(req);
        if (status_ptr) kfree(status_ptr);
        return -1;
    }

    req->type = type;
    req->reserved = 0;
    req->sector = sector;
    *status_ptr = 0xFF;

    d0 = vq_alloc_desc();
    d1 = vq_alloc_desc();
    d2 = vq_alloc_desc();
    if (d0 < 0 || d1 < 0 || d2 < 0) {
        kfree(req);
        kfree(status_ptr);
        return -1;
    }

    /* Descriptor 0: request header (device-readable) */
    vq.desc[d0].addr = (uint64_t)(uintptr_t)req;
    vq.desc[d0].len = (uint32_t)sizeof(struct virtio_blk_req);
    vq.desc[d0].flags = VRING_DESC_F_NEXT;
    vq.desc[d0].next = (uint16_t)d1;

    /* Descriptor 1: data buffer */
    vq.desc[d1].addr = (uint64_t)(uintptr_t)buffer;
    vq.desc[d1].len = len;
    vq.desc[d1].flags = VRING_DESC_F_NEXT;
    if (type == VIRTIO_BLK_T_IN)
        vq.desc[d1].flags |= VRING_DESC_F_WRITE;
    vq.desc[d1].next = (uint16_t)d2;

    /* Descriptor 2: status byte (device-writable) */
    vq.desc[d2].addr = (uint64_t)(uintptr_t)status_ptr;
    vq.desc[d2].len = 1;
    vq.desc[d2].flags = VRING_DESC_F_WRITE;
    vq.desc[d2].next = 0;

    vq.avail->flags = 0;
    avail_idx = vq.avail->idx % vq.size;
    vq.avail->ring[avail_idx] = (uint16_t)d0;

    __asm__ volatile ("sfence" ::: "memory");
    vq.avail->idx++;
    __asm__ volatile ("sfence" ::: "memory");



    /* Enable interrupts for IRQ delivery */
    virtio_irq_fired = 0;
    __asm__ volatile ("sti");

    /* Notify the device */
    vio_outw(io_base + VIRTIO_QUEUE_NOTIFY, 0);

    /* Poll for completion with I/O port delays.
     * Port 0x80 reads cause bus cycles that give QEMU's dataplane
     * thread a chance to process the request. */
    timeout = 1000000;
    while (timeout > 0) {
        /* I/O delay — yields to QEMU event loop */
        (void)vio_inb(0x80);
        (void)vio_inb(0x80);
        (void)vio_inb(0x80);
        (void)vio_inb(0x80);

        __asm__ volatile ("" ::: "memory");
        if (vq.used->idx != vq.last_used)
            break;
        timeout--;
    }

    if (timeout == 0) {
        printk("[VIRTIO] I/O timeout (avail=%u, used=%u, status=%x)\n",
               (uint64_t)vq.avail->idx, (uint64_t)vq.used->idx,
               (uint64_t)*status_ptr);
        vq_free_desc((uint16_t)d0);
        vq_free_desc((uint16_t)d1);
        vq_free_desc((uint16_t)d2);
        kfree(req);
        kfree(status_ptr);
        return -1;
    }

    (void)vio_inb(io_base + VIRTIO_ISR_STATUS);
    vq.last_used++;

    vq_free_desc((uint16_t)d0);
    vq_free_desc((uint16_t)d1);
    vq_free_desc((uint16_t)d2);

    result = (*status_ptr == VIRTIO_BLK_S_OK) ? 0 : -1;

    kfree(req);
    kfree(status_ptr);
    return result;
}

/* --- Public API --- */

int virtio_blk_init(void)
{
    struct pci_device dev;
    uint8_t status;
    uint32_t features;

    /* Find virtio-blk device on PCI bus */
    dev = pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_BLK_DEVICE_ID);
    if (!dev.found) {
        /* Try transitional device ID */
        dev = pci_find_device(VIRTIO_VENDOR_ID, 0x1042);
        if (!dev.found) {
            printk("[--] VirtIO-blk: not detected\n");
            return -1;
        }
    }

    printk("[VIRTIO] Found virtio-blk at PCI %u:%u.%u\n",
           (uint64_t)dev.bus, (uint64_t)dev.dev, (uint64_t)dev.func);

    /* Enable bus mastering */
    pci_enable_bus_mastering(&dev);

    /* Debug: verify PCI command register */
    {
        uint16_t cmd = pci_read16(dev.bus, dev.dev, dev.func, PCI_COMMAND);
        printk("[VIRTIO] PCI cmd: %x (BM=%u IO=%u MEM=%u) IRQ=%u BAR0=%x\n",
               (uint64_t)cmd,
               (uint64_t)((cmd >> 2) & 1),
               (uint64_t)(cmd & 1),
               (uint64_t)((cmd >> 1) & 1),
               (uint64_t)dev.irq_line,
               (uint64_t)dev.bar[0]);
    }

    /* Get I/O port base from BAR0 (bit 0 = 1 means I/O space) */
    if (!(dev.bar[0] & 1)) {
        printk("[VIRTIO] BAR0 is MMIO, not I/O — unsupported\n");
        return -1;
    }
    io_base = (uint16_t)(dev.bar[0] & ~0x3);
    printk("[VIRTIO] I/O base: %x\n", (uint64_t)io_base);

    /* Check if device is already initialized by BIOS/firmware */
    {
        uint8_t cur_status = vio_inb(io_base + VIRTIO_DEVICE_STATUS);
        vio_outw(io_base + VIRTIO_QUEUE_SELECT, 0);
        uint32_t cur_pfn = vio_inl(io_base + VIRTIO_QUEUE_ADDRESS);
        uint16_t qsize = vio_inw(io_base + VIRTIO_QUEUE_SIZE);

        if (cur_status & VIRTIO_STATUS_DRIVER_OK && cur_pfn != 0 && qsize != 0) {
            /* Device already initialized! Reuse the existing queue. */
            uintptr_t desc_addr = (uintptr_t)cur_pfn << 12;
            uintptr_t avail_addr = desc_addr + (uintptr_t)qsize * 16;
            uintptr_t desc_sz = (uintptr_t)qsize * 16;
            uintptr_t avail_sz = 6 + (uintptr_t)qsize * 2;
            uintptr_t used_off = align_up(desc_sz + avail_sz, 4096);
            uintptr_t used_addr = desc_addr + used_off;

            vq.desc = (struct vring_desc *)desc_addr;
            vq.avail = (struct vring_avail *)avail_addr;
            vq.used = (struct vring_used *)used_addr;
            vq.size = qsize;

            /* Read where the BIOS left off */
            vq.last_used = vq.used->idx;

            /* Rebuild free list starting from where BIOS stopped.
             * The BIOS used some descriptors (likely 0-2), so start free list
             * from descriptor 3. */
            {
                uint32_t i;
                for (i = 0; i < (uint32_t)qsize; i++) {
                    vq.desc[i].next = (uint16_t)(i + 1);
                    vq.desc[i].flags = 0;
                    vq.desc[i].addr = 0;
                    vq.desc[i].len = 0;
                }
                vq.free_head = 0;
                vq.num_free = qsize;
            }

            /* Read device features for info */
            features = vio_inl(io_base + VIRTIO_DEVICE_FEATURES);
            printk("[VIRTIO] Device features: %x\n", (uint64_t)features);
            printk("[VIRTIO] Reusing BIOS queue: PFN=%x status=%x qsize=%u\n",
                   (uint64_t)cur_pfn, (uint64_t)cur_status, (uint64_t)qsize);
            printk("[VIRTIO] avail_idx=%u used_idx=%u last_used=%u\n",
                   (uint64_t)vq.avail->idx, (uint64_t)vq.used->idx,
                   (uint64_t)vq.last_used);
            goto skip_init;
        }

        printk("[VIRTIO] Device not pre-init (status=%x pfn=%x), doing full init\n",
               (uint64_t)cur_status, (uint64_t)cur_pfn);
    }

    /* Full device reset sequence (for non-pre-initialized devices) */
    vio_outw(io_base + VIRTIO_QUEUE_SELECT, 0);
    vio_outl(io_base + VIRTIO_QUEUE_ADDRESS, 0);
    vio_outb(io_base + VIRTIO_DEVICE_STATUS, 0);
    {
        uint32_t wait;
        for (wait = 0; wait < 100000; wait++) {
            if (vio_inb(io_base + VIRTIO_DEVICE_STATUS) == 0)
                break;
            (void)vio_inb(0x80);
        }
    }

    status = VIRTIO_STATUS_ACKNOWLEDGE;
    vio_outb(io_base + VIRTIO_DEVICE_STATUS, status);
    status |= VIRTIO_STATUS_DRIVER;
    vio_outb(io_base + VIRTIO_DEVICE_STATUS, status);

    features = vio_inl(io_base + VIRTIO_DEVICE_FEATURES);
    printk("[VIRTIO] Device features: %x\n", (uint64_t)features);
    vio_outl(io_base + VIRTIO_GUEST_FEATURES, 0);

    if (virtqueue_init(0) != 0) {
        vio_outb(io_base + VIRTIO_DEVICE_STATUS,
                 status | VIRTIO_STATUS_FAILED);
        return -1;
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    vio_outb(io_base + VIRTIO_DEVICE_STATUS, status);

skip_init:

    /* Read disk capacity from device config */
    {
        uint32_t cap_lo = vio_inl(io_base + VIRTIO_BLK_CAPACITY_LO);
        uint32_t cap_hi = vio_inl(io_base + VIRTIO_BLK_CAPACITY_HI);
        disk_capacity = ((uint64_t)cap_hi << 32) | (uint64_t)cap_lo;
    }

    initialized = 1;

    /* Register IRQ handler for virtio interrupts (IRQ 11 = IDT 43) */
    idt_register_handler(43, virtio_irq_handler);

    /* Unmask IRQ 11 on the slave PIC (bit 3 of OCW1 on port 0xA1)
     * Also ensure cascade IRQ 2 is unmasked on master PIC */
    {
        uint8_t mask;
        /* Unmask IRQ 2 (cascade) on master PIC */
        mask = vio_inb(0x21);
        mask &= ~(1 << 2);
        vio_outb(0x21, mask);
        /* Unmask IRQ 11 (bit 3) on slave PIC */
        mask = vio_inb(0xA1);
        mask &= ~(1 << 3);
        vio_outb(0xA1, mask);
    }

    {
        uint64_t size_mb = disk_capacity * 512 / (1024 * 1024);
        printk("[OK] VirtIO-blk: %u MiB (%u sectors)\n",
               size_mb, disk_capacity);
    }

    return 0;
}

int virtio_blk_read(uint64_t lba, uint32_t count, void *buffer)
{
    uint32_t i;
    uint8_t *buf = (uint8_t *)buffer;

    for (i = 0; i < count; i++) {
        if (virtio_blk_do_io(VIRTIO_BLK_T_IN, lba + i,
                              buf + i * 512, 512) != 0)
            return -1;
    }
    return 0;
}

int virtio_blk_write(uint64_t lba, uint32_t count, const void *buffer)
{
    uint32_t i;
    const uint8_t *buf = (const uint8_t *)buffer;

    /* Cast away const for the I/O function (buffer is device-writable
     * for reads, but device-readable for writes — the flag handles it) */
    for (i = 0; i < count; i++) {
        if (virtio_blk_do_io(VIRTIO_BLK_T_OUT, lba + i,
                              (void *)(buf + i * 512), 512) != 0)
            return -1;
    }
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
