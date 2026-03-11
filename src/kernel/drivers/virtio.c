/* ============================================================================
 * virtio.c — VirtIO PCI modern transport
 *
 * Split virtqueue management for VirtIO modern PCI devices.
 * Discovers config regions via PCI capabilities and uses MMIO access.
 *
 * The modern PCI transport uses vendor-specific PCI capabilities (ID 0x09)
 * to point to MMIO regions in BARs for:
 *   - Common configuration (type 1)
 *   - Notifications (type 2)
 *   - ISR status (type 3)
 *   - Device-specific config (type 4)
 * ============================================================================ */

#include "kernel/drivers/virtio.h"
#include "kernel/drivers/pci.h"
#include "kernel/mm/heap.h"
#include "kernel/printk.h"
#include "kernel/mm/vmm.h"

/* MMIO helpers are now in virtio.h as static inline */

/* ---- Memory helpers ---- */
static void vio_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = val;
}

/* ---- MMIO BAR mapping ----
 * VirtIO modern transport BAR addresses may be above the identity-mapped
 * region.  We must create page table entries (identity mapped, uncacheable)
 * before accessing them. */

/* Track mapped BARs to avoid double-mapping */
static uint64_t mapped_bars[6];
static uint32_t mapped_bar_sizes[6];
static uint8_t  mapped_bar_count;

static void map_mmio_range(uint64_t phys_addr, uint32_t length)
{
    uint64_t page_start = phys_addr & ~(uint64_t)0xFFF;
    uint64_t page_end   = (phys_addr + length + 0xFFF) & ~(uint64_t)0xFFF;
    uint64_t page;
    uint64_t flags = VMM_KERNEL_RW | VMM_FLAG_NOCACHE | VMM_FLAG_WRITETHROUGH;

    for (page = page_start; page < page_end; page += VMM_PAGE_SIZE) {
        /* Only map if not already identity-mapped */
        if (vmm_get_physical(page) == 0) {
            vmm_map_page(page, page, flags);
        }
    }
}

/* Map a BAR's MMIO region (only once per BAR) */
static void ensure_bar_mapped(uint64_t bar_addr, uint8_t bar_idx,
                               uint8_t bus, uint8_t pci_dev, uint8_t func)
{
    uint8_t i;
    uint32_t bar_size;

    /* Check if already mapped */
    for (i = 0; i < mapped_bar_count; i++) {
        if (mapped_bars[i] == bar_addr)
            return;
    }

    /* Determine BAR size by writing all 1s and reading back */
    uint32_t orig = pci_read32(bus, pci_dev, func, PCI_BAR0 + bar_idx * 4);
    pci_write32(bus, pci_dev, func, PCI_BAR0 + bar_idx * 4, 0xFFFFFFFF);
    bar_size = pci_read32(bus, pci_dev, func, PCI_BAR0 + bar_idx * 4);
    pci_write32(bus, pci_dev, func, PCI_BAR0 + bar_idx * 4, orig);

    /* Decode size: invert, mask type bits, add 1 */
    bar_size &= 0xFFFFFFF0;  /* Mask lower 4 bits */
    bar_size = ~bar_size + 1;

    if (bar_size == 0 || bar_size > 0x100000)
        bar_size = 0x10000;  /* Default to 64 KiB if probe fails */

    printk("[VIRTIO] Mapping BAR%u: phys 0x%x, size 0x%x\n",
           (uint64_t)bar_idx, bar_addr, (uint64_t)bar_size);

    map_mmio_range(bar_addr, bar_size);

    if (mapped_bar_count < 6) {
        mapped_bars[mapped_bar_count] = bar_addr;
        mapped_bar_sizes[mapped_bar_count] = bar_size;
        mapped_bar_count++;
    }
}

/* ---- PCI capability list walking ---- */

/* PCI configuration space offsets for capability list */
#define PCI_CAP_PTR       0x34   /* Pointer to first capability */
#define PCI_STATUS_REG    0x06   /* Status register */
#define PCI_STATUS_CAP    (1 << 4) /* Capabilities list present */

/* Read a VirtIO PCI capability at the given config space offset.
 * Returns the capability type, or -1 if not a VirtIO cap. */
static int read_virtio_cap(uint8_t bus, uint8_t dev, uint8_t func,
                           uint8_t cap_off,
                           uint8_t *out_bar, uint32_t *out_offset,
                           uint32_t *out_length)
{
    uint8_t  cap_id   = pci_read8(bus, dev, func, cap_off);
    uint8_t  cfg_type = pci_read8(bus, dev, func, cap_off + 3);
    uint8_t  bar      = pci_read8(bus, dev, func, cap_off + 4);
    uint32_t offset   = pci_read32(bus, dev, func, cap_off + 8);
    uint32_t length   = pci_read32(bus, dev, func, cap_off + 12);

    if (cap_id != PCI_CAP_ID_VENDOR)
        return -1;

    *out_bar    = bar;
    *out_offset = offset;
    *out_length = length;

    return (int)cfg_type;
}

/* ---- Public API ---- */

int virtio_pci_init(struct virtio_pci_dev *dev,
                    uint8_t bus, uint8_t pci_dev, uint8_t func)
{
    uint16_t status;
    uint8_t  cap_off;
    int found_common = 0, found_notify = 0, found_isr = 0, found_device = 0;

    mapped_bar_count = 0;

    /* Zero out the device struct */
    dev->common_cfg   = NULL;
    dev->notify_base  = NULL;
    dev->isr_cfg      = NULL;
    dev->device_cfg   = NULL;
    dev->notify_off_multiplier = 0;

    /* Check if device has capabilities list */
    status = pci_read16(bus, pci_dev, func, PCI_STATUS_REG);
    if (!(status & PCI_STATUS_CAP)) {
        printk("[VIRTIO] Device has no PCI capabilities\n");
        return -1;
    }

    /* Get pointer to first capability */
    cap_off = pci_read8(bus, pci_dev, func, PCI_CAP_PTR);
    cap_off &= 0xFC;  /* Align to DWORD */

    /* Walk the capability list */
    while (cap_off != 0) {
        uint8_t  bar;
        uint32_t offset, length;
        int cap_type;

        cap_type = read_virtio_cap(bus, pci_dev, func, cap_off,
                                   &bar, &offset, &length);

        if (cap_type >= 0) {
            /* Get the BAR base address (MMIO) */
            uint32_t bar_val = pci_read32(bus, pci_dev, func,
                                           PCI_BAR0 + bar * 4);
            uint64_t bar_addr;

            if (bar_val & 0x01) {
                /* I/O space BAR — shouldn't happen for modern transport */
                printk("[VIRTIO] Unexpected I/O BAR %u for cap type %u\n",
                       (uint64_t)bar, (uint64_t)cap_type);
                goto next_cap;
            }

            /* Memory-space BAR */
            uint8_t bar_type = (bar_val >> 1) & 0x03;
            bar_addr = (uint64_t)(bar_val & 0xFFFFFFF0);

            if (bar_type == 0x02) {
                /* 64-bit BAR: read next BAR for high 32 bits */
                uint32_t bar_hi = pci_read32(bus, pci_dev, func,
                                              PCI_BAR0 + (bar + 1) * 4);
                bar_addr |= ((uint64_t)bar_hi << 32);
            }

            /* Map the BAR region into kernel page tables */
            ensure_bar_mapped(bar_addr, bar, bus, pci_dev, func);

            volatile uint8_t *mmio = (volatile uint8_t *)bar_addr + offset;

            switch (cap_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                dev->common_cfg = mmio;
                found_common = 1;
                printk("[VIRTIO]   Common cfg: BAR%u+0x%x (len %u)\n",
                       (uint64_t)bar, (uint64_t)offset, (uint64_t)length);
                break;

            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                dev->notify_base = mmio;
                /* Read the notify_off_multiplier (at cap_off + 16) */
                dev->notify_off_multiplier = pci_read32(bus, pci_dev, func,
                                                         cap_off + 16);
                found_notify = 1;
                printk("[VIRTIO]   Notify: BAR%u+0x%x (mult %u)\n",
                       (uint64_t)bar, (uint64_t)offset,
                       (uint64_t)dev->notify_off_multiplier);
                break;

            case VIRTIO_PCI_CAP_ISR_CFG:
                dev->isr_cfg = mmio;
                found_isr = 1;
                break;

            case VIRTIO_PCI_CAP_DEVICE_CFG:
                dev->device_cfg = mmio;
                found_device = 1;
                break;

            default:
                break;
            }
        }

next_cap:
        /* Next capability */
        cap_off = pci_read8(bus, pci_dev, func, cap_off + 1);
        cap_off &= 0xFC;
    }

    if (!found_common || !found_notify || !found_isr) {
        printk("[VIRTIO] Missing required caps: common=%u notify=%u isr=%u\n",
               (uint64_t)found_common, (uint64_t)found_notify,
               (uint64_t)found_isr);
        return -1;
    }

    (void)found_device;  /* Device cfg is optional for some devices */
    return 0;
}

/* ---- Virtqueue ring size calculations ---- */

/* Align a value up to the given power-of-2 alignment */
static uint64_t vq_align(uint64_t val, uint64_t align)
{
    return (val + align - 1) & ~(align - 1);
}

int virtq_init(struct virtqueue *vq, struct virtio_pci_dev *dev,
               uint16_t queue_idx)
{
    volatile uint8_t *cfg = dev->common_cfg;
    uint16_t qsz;
    uint64_t desc_sz, avail_sz, used_sz, total;
    uint8_t *desc_mem, *avail_mem, *used_mem;
    int i;

    vq->dev       = dev;
    vq->queue_idx = queue_idx;

    /* 1. Select the queue */
    mmio_write16((volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_SELECT),
                  queue_idx);

    /* 2. Read queue size (number of descriptors, power of 2) */
    qsz = mmio_read16((volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_SIZE));
    if (qsz == 0) {
        printk("[VIRTIO] Queue %u has size 0\n", (uint64_t)queue_idx);
        return -1;
    }

    vq->size     = qsz;
    vq->num_free = qsz;

    /* 3. Allocate physically contiguous memory for each ring.
     *    Modern transport allows separate addresses for desc/avail/used.
     *    Our kernel heap is identity-mapped, so virt == phys. */
    desc_sz  = vq_align((uint64_t)qsz * 16, 16);      /* 16 bytes/desc */
    avail_sz = vq_align(6 + (uint64_t)qsz * 2, 2);    /* avail ring */
    used_sz  = vq_align(6 + (uint64_t)qsz * 8, 4);    /* used ring */
    total    = desc_sz + avail_sz + used_sz;

    desc_mem = (uint8_t *)kmalloc((size_t)total);
    if (!desc_mem) {
        printk("[VIRTIO] Cannot allocate %u bytes for queue %u\n",
               (uint64_t)total, (uint64_t)queue_idx);
        return -1;
    }
    vio_memset(desc_mem, 0, total);

    avail_mem = desc_mem + desc_sz;
    used_mem  = avail_mem + avail_sz;

    /* 4. Set up pointers */
    vq->desc  = (struct virtq_desc *)desc_mem;
    vq->avail = (struct virtq_avail *)avail_mem;
    vq->used  = (struct virtq_used *)used_mem;

    /* 5. Initialize free descriptor chain */
    for (i = 0; i < qsz - 1; i++) {
        vq->desc[i].next  = (uint16_t)(i + 1);
        vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
    }
    vq->desc[qsz - 1].next  = 0;
    vq->desc[qsz - 1].flags = 0;
    vq->free_head = 0;
    vq->last_used = 0;

    /* 6. Tell the device the queue addresses (modern: 64-bit MMIO) */
    mmio_write64(cfg, VIRTIO_COMMON_Q_DESC,  (uint64_t)desc_mem);
    mmio_write64(cfg, VIRTIO_COMMON_Q_AVAIL, (uint64_t)avail_mem);
    mmio_write64(cfg, VIRTIO_COMMON_Q_USED,  (uint64_t)used_mem);

    /* 7. Read the notification offset for this queue */
    vq->notify_off = mmio_read16(
        (volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_NOTIFY_OFF));

    /* 8. Enable the queue */
    mmio_write16((volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_ENABLE), 1);

    printk("[VIRTIO] Queue %u: size=%u, desc=0x%x, notify_off=%u\n",
           (uint64_t)queue_idx, (uint64_t)qsz,
           (uint64_t)(uint64_t)desc_mem, (uint64_t)vq->notify_off);

    return 0;
}

int virtq_add_buf(struct virtqueue *vq, void *buf, uint32_t len,
                  uint16_t flags)
{
    uint16_t idx;

    if (vq->num_free == 0)
        return -1;

    /* Grab a free descriptor */
    idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;

    /* Fill in the descriptor */
    vq->desc[idx].addr  = (uint64_t)buf;
    vq->desc[idx].len   = len;
    vq->desc[idx].flags = flags;
    vq->desc[idx].next  = 0;

    /* Add to the available ring */
    uint16_t avail_idx = vq->avail->idx % vq->size;
    vq->avail->ring[avail_idx] = idx;

    /* Memory barrier — ensure descriptor is written before idx update */
    __asm__ volatile("mfence" ::: "memory");

    vq->avail->idx++;

    return (int)idx;
}

void virtq_kick(struct virtqueue *vq)
{
    /* Memory barrier before notify */
    __asm__ volatile("mfence" ::: "memory");

    /* Modern transport: write queue index to
     * notify_base + notify_off * notify_off_multiplier */
    uint64_t notify_addr = (uint64_t)vq->dev->notify_base +
                           (uint64_t)vq->notify_off *
                           (uint64_t)vq->dev->notify_off_multiplier;
    mmio_write16((volatile uint16_t *)notify_addr, vq->queue_idx);
}

int virtq_get_buf(struct virtqueue *vq, uint32_t *len)
{
    uint16_t used_idx;
    uint32_t desc_idx;

    /* Memory barrier — read used->idx after device writes */
    __asm__ volatile("mfence" ::: "memory");

    if (vq->last_used == vq->used->idx)
        return -1;  /* Nothing new */

    used_idx = vq->last_used % vq->size;
    desc_idx = vq->used->ring[used_idx].id;

    if (len)
        *len = vq->used->ring[used_idx].len;

    vq->last_used++;

    return (int)desc_idx;
}

void virtq_free_desc(struct virtqueue *vq, uint16_t idx)
{
    vq->desc[idx].next  = vq->free_head;
    vq->desc[idx].flags = VIRTQ_DESC_F_NEXT;
    vq->free_head = idx;
    vq->num_free++;
}

uint8_t virtio_get_status(struct virtio_pci_dev *dev)
{
    return mmio_read8(dev->common_cfg + VIRTIO_COMMON_STATUS);
}

void virtio_set_status(struct virtio_pci_dev *dev, uint8_t status)
{
    mmio_write8(dev->common_cfg + VIRTIO_COMMON_STATUS, status);
}

uint8_t virtio_read_isr(struct virtio_pci_dev *dev)
{
    return mmio_read8(dev->isr_cfg);
}
