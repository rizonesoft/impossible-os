/* ============================================================================
 * virtio.c -- VirtIO PCI modern transport
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

#include "kernel/drivers/virtio/virtio.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/pci_pm.h"
#include "kernel/mm/pmm.h"
#include "kernel/irq.h"
#include "kernel/klog.h"
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

    klog(LOG_DEBUG, "virtio", "Mapping BAR%u: phys 0x%x, size 0x%x",
           (uint64_t)bar_idx, bar_addr, (uint64_t)bar_size);

    map_mmio_range(bar_addr, bar_size);

    if (mapped_bar_count < 6) {
        mapped_bars[mapped_bar_count] = bar_addr;
        mapped_bar_sizes[mapped_bar_count] = bar_size;
        mapped_bar_count++;
    }
}

/* ---- PCI capability list walking ---- */

/* Capability-list offsets and the list-present flag come from the shared PCI
 * headers. They were duplicated here with different names (PCI_CAP_PTR,
 * PCI_STATUS_REG, PCI_STATUS_CAP), which is two definitions of one hardware
 * fact and a place for silent drift. */

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
    status = pci_read16(bus, pci_dev, func, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) {
        klog(LOG_DEBUG, "virtio", "Device has no PCI capabilities");
        return -1;
    }

    /* Get pointer to first capability */
    cap_off = pci_read8(bus, pci_dev, func, PCI_CAP_PTR_TYPE01);
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
                /* I/O space BAR -- shouldn't happen for modern transport */
                klog(LOG_DEBUG, "virtio", "Unexpected I/O BAR %u for cap type %u",
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
                klog(LOG_DEBUG, "virtio", "  Common cfg: BAR%u+0x%x (len %u)",
                       (uint64_t)bar, (uint64_t)offset, (uint64_t)length);
                break;

            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                dev->notify_base = mmio;
                /* Read the notify_off_multiplier (at cap_off + 16) */
                dev->notify_off_multiplier = pci_read32(bus, pci_dev, func,
                                                         cap_off + 16);
                found_notify = 1;
                klog(LOG_DEBUG, "virtio", "  Notify: BAR%u+0x%x (mult %u)",
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
        klog(LOG_DEBUG, "virtio", "Missing required caps: common=%u notify=%u isr=%u",
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
        klog(LOG_DEBUG, "virtio", "Queue %u has size 0", (uint64_t)queue_idx);
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

    /* Use PMM -- virtqueue buffers can be large with multi-queue,
     * and the kernel heap is only 2 MiB (rules.md Known Gotchas). */
    uint64_t pages_needed = (total + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    uintptr_t phys = pmm_alloc_contiguous(pages_needed);
    if (!phys) {
        klog(LOG_DEBUG, "virtio", "Cannot allocate %u bytes (%u pages) for queue %u",
               (uint64_t)total, (uint64_t)pages_needed, (uint64_t)queue_idx);
        return -1;
    }
    desc_mem = (uint8_t *)phys;
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

    klog(LOG_DEBUG, "virtio", "Queue %u: size=%u, desc=0x%x, notify_off=%u",
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

    /* Memory barrier -- ensure descriptor is written before idx update */
    __asm__ volatile("mfence" ::: "memory");

    vq->avail->idx++;

    return (int)idx;
}

void virtq_kick(struct virtqueue *vq)
{
    /* Memory barrier before notify -- ensure avail->idx is visible */
    __asm__ volatile("mfence" ::: "memory");

    /* Calculate notification register address */
    uint64_t notify_addr = (uint64_t)vq->dev->notify_base +
                           (uint64_t)vq->notify_off *
                           (uint64_t)vq->dev->notify_off_multiplier;

    if (vq->event_idx) {
        /* EVENT_IDX path (VirtIO §2.7.7.2):
         * Notify only if avail->idx crosses the avail_event threshold.
         * Uses wrapping 16-bit arithmetic:
         *   notify if (new_idx - event - 1) < (new_idx - old_idx)
         * Since we increment avail->idx by 1 per submission,
         * (new_idx - old_idx) == 1, so notify if new_idx == event + 1 */
        uint16_t new_idx = vq->avail->idx;
        uint16_t event   = virtq_avail_event(vq);

        __asm__ volatile("mfence" ::: "memory");

        /* Wrapping subtract: notify if we just crossed the threshold */
        if ((uint16_t)(new_idx - event - 1) < 1) {
            if (vq->notify_data) {
                /* NOTIFICATION_DATA (VirtIO 1.2 spec): pack queue index + avail idx
                 * Split VQ format: low 16 = vqn, high 16 = next_avail_idx */
                uint32_t data = ((uint32_t)vq->queue_idx & 0xFFFF) |
                                ((uint32_t)vq->avail->idx << 16);
                mmio_write32((volatile uint32_t *)notify_addr, data);
            } else {
                mmio_write16((volatile uint16_t *)notify_addr, vq->queue_idx);
            }
        }
    } else {
        /* Legacy path: always notify (no suppression) */
        if (vq->notify_data) {
            uint32_t data = ((uint32_t)vq->queue_idx & 0xFFFF) |
                            ((uint32_t)vq->avail->idx << 16);
            mmio_write32((volatile uint32_t *)notify_addr, data);
        } else {
            mmio_write16((volatile uint16_t *)notify_addr, vq->queue_idx);
        }
    }
}

int virtq_get_buf(struct virtqueue *vq, uint32_t *len)
{
    uint16_t used_idx;
    uint32_t desc_idx;

    /* Memory barrier -- read used->idx after device writes */
    __asm__ volatile("mfence" ::: "memory");

    if (vq->last_used == vq->used->idx)
        return -1;  /* Nothing new */

    used_idx = vq->last_used % vq->size;
    desc_idx = vq->used->ring[used_idx].id;

    /* The descriptor id is DEVICE-SUPPLIED data: a malformed or hostile
     * device can return an id outside the descriptor table, and callers
     * index their buffer arrays with it. Treat out-of-range as ring
     * corruption: consume the entry and report nothing-new so the drain
     * loop terminates instead of corrupting memory. */
    if (desc_idx >= vq->size) {
        klog(LOG_ERROR, "virtio",
             "used-ring id %u >= queue size %u -- corrupt ring entry dropped",
             (uint64_t)desc_idx, (uint64_t)vq->size);
        vq->last_used++;
        return -1;
    }

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

uint8_t virtio_read_config_generation(struct virtio_pci_dev *dev)
{
    return mmio_read8(dev->common_cfg + VIRTIO_COMMON_CFGGEN);
}

int virtio_queue_reset(struct virtqueue *vq)
{
    volatile uint8_t *cfg;
    uint16_t qsz;
    uint64_t desc_sz, avail_sz, used_sz, total;
    uint8_t *desc_mem, *avail_mem, *used_mem;
    uint64_t pages_needed;
    uintptr_t phys;
    uintptr_t old_desc;
    uint64_t old_pages;
    uint32_t wait;
    int i;

    if (!vq || !vq->dev || !vq->dev->common_cfg)
        return -1;

    cfg = vq->dev->common_cfg;
    qsz = vq->size;

    klog(LOG_DEBUG, "virtio", "Queue %u: initiating per-queue reset",
           (uint64_t)vq->queue_idx);

    /* 1. Select the queue */
    mmio_write16((volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_SELECT),
                  vq->queue_idx);

    /* 2. Write queue_reset = 1 to request reset */
    mmio_write16((volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_RESET), 1);

    /* 3. Poll until device acknowledges reset (readback = 1) */
    wait = 100000;
    while (wait-- > 0) {
        uint16_t val = mmio_read16(
            (volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_RESET));
        if (val == 1)
            break;
        __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
    }
    if (wait == 0) {
        klog(LOG_DEBUG, "virtio",
               "Queue %u: device did not acknowledge reset",
               (uint64_t)vq->queue_idx);
        return -1;
    }

    /* 4. Free old ring memory (desc/avail/used are contiguous from one alloc) */
    old_desc = (uintptr_t)vq->desc;
    desc_sz  = vq_align((uint64_t)qsz * 16, 16);
    avail_sz = vq_align(6 + (uint64_t)qsz * 2, 2);
    used_sz  = vq_align(6 + (uint64_t)qsz * 8, 4);
    total    = desc_sz + avail_sz + used_sz;
    old_pages = (total + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

    for (i = 0; i < (int)old_pages; i++)
        pmm_free_frame(old_desc + (uint64_t)i * PMM_FRAME_SIZE);

    /* 5. Reallocate fresh ring memory */
    pages_needed = old_pages;
    phys = pmm_alloc_contiguous(pages_needed);
    if (!phys) {
        klog(LOG_DEBUG, "virtio",
               "Queue %u: cannot reallocate ring memory",
               (uint64_t)vq->queue_idx);
        return -1;
    }
    desc_mem = (uint8_t *)phys;
    vio_memset(desc_mem, 0, total);

    avail_mem = desc_mem + desc_sz;
    used_mem  = avail_mem + avail_sz;

    /* 6. Update virtqueue pointers */
    vq->desc  = (struct virtq_desc *)desc_mem;
    vq->avail = (struct virtq_avail *)avail_mem;
    vq->used  = (struct virtq_used *)used_mem;

    /* 7. Re-initialize free descriptor chain */
    for (i = 0; i < qsz - 1; i++) {
        vq->desc[i].next  = (uint16_t)(i + 1);
        vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
    }
    vq->desc[qsz - 1].next  = 0;
    vq->desc[qsz - 1].flags = 0;
    vq->free_head = 0;
    vq->num_free  = qsz;
    vq->last_used = 0;

    /* 8. Write new ring addresses to device */
    mmio_write64(cfg, VIRTIO_COMMON_Q_DESC,  (uint64_t)desc_mem);
    mmio_write64(cfg, VIRTIO_COMMON_Q_AVAIL, (uint64_t)avail_mem);
    mmio_write64(cfg, VIRTIO_COMMON_Q_USED,  (uint64_t)used_mem);

    /* 9. Write queue_reset = 0 to re-enable the queue */
    mmio_write16((volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_RESET), 0);

    klog(LOG_DEBUG, "virtio",
           "Queue %u: reset complete, desc=0x%x",
           (uint64_t)vq->queue_idx, (uint64_t)(uintptr_t)desc_mem);

    return 0;
}

/* ---- MSI-X setup ---- */

int virtio_pci_setup_msix(struct virtio_pci_dev *dev,
                          uint8_t bus, uint8_t pci_dev, uint8_t func,
                          uint8_t *queue_vector, uint8_t *config_vector)
{
    uint16_t status;
    uint8_t cap_off;
    uint8_t msix_cap_off = 0;
    uint16_t msix_ctrl;
    uint16_t table_size;
    uint8_t  table_bir;     /* BAR Indicator Register (which BAR) */
    uint32_t table_offset;
    uint64_t bar_addr;
    volatile struct msix_table_entry *msix_table;
    uint8_t vec_queue, vec_config;
    uint16_t readback;

    /* Walk PCI capability list for MSI-X (cap ID 0x11) */
    status = pci_read16(bus, pci_dev, func, 0x06);
    if (!(status & (1 << 4))) {
        klog(LOG_DEBUG, "virtio", "MSI-X: no PCI capabilities list");
        return -1;
    }

    cap_off = pci_read8(bus, pci_dev, func, 0x34) & 0xFC;
    while (cap_off != 0) {
        uint8_t cap_id = pci_read8(bus, pci_dev, func, cap_off);
        if (cap_id == PCI_CAP_ID_MSIX) {
            msix_cap_off = cap_off;
            break;
        }
        cap_off = pci_read8(bus, pci_dev, func, cap_off + 1) & 0xFC;
    }

    if (!msix_cap_off) {
        klog(LOG_DEBUG, "virtio", "MSI-X: capability not found");
        return -1;
    }

    /* Read MSI-X Message Control (cap_off + 2) */
    msix_ctrl = pci_read16(bus, pci_dev, func, msix_cap_off + 2);
    table_size = (msix_ctrl & 0x07FF) + 1;  /* bits 10:0 = N-1 */

    if (table_size < 2) {
        klog(LOG_DEBUG, "virtio", "MSI-X: table too small (%u entries)",
               (uint64_t)table_size);
        return -1;
    }

    /* Read Table BIR and offset (cap_off + 4) */
    {
        uint32_t table_reg = pci_read32(bus, pci_dev, func, msix_cap_off + 4);
        table_bir    = (uint8_t)(table_reg & 0x07);
        table_offset = table_reg & 0xFFFFFFF8;
    }

    /* Read BAR for MSI-X table */
    {
        uint32_t bar_val = pci_read32(bus, pci_dev, func,
                                       PCI_BAR0 + table_bir * 4);
        if (bar_val & 0x01) {
            klog(LOG_DEBUG, "virtio", "MSI-X: BAR%u is I/O space", (uint64_t)table_bir);
            return -1;
        }

        uint8_t bar_type = (bar_val >> 1) & 0x03;
        bar_addr = (uint64_t)(bar_val & 0xFFFFFFF0);

        if (bar_type == 0x02) {
            uint32_t bar_hi = pci_read32(bus, pci_dev, func,
                                          PCI_BAR0 + (table_bir + 1) * 4);
            bar_addr |= ((uint64_t)bar_hi << 32);
        }
    }

    /* Map the MSI-X table BAR */
    ensure_bar_mapped(bar_addr, table_bir, bus, pci_dev, func);

    msix_table = (volatile struct msix_table_entry *)(bar_addr + table_offset);

    /* Allocate IDT vectors from dynamic range */
    vec_queue = irq_alloc_vector();
    if (!vec_queue) {
        klog(LOG_DEBUG, "virtio", "MSI-X: no free IDT vector for queue");
        return -1;
    }

    vec_config = irq_alloc_vector();
    if (!vec_config) {
        irq_free_vector(vec_queue);
        klog(LOG_DEBUG, "virtio", "MSI-X: no free IDT vector for config");
        return -1;
    }

    /* Program MSI-X table entry 0: request queue interrupt */
    msix_table[0].msg_addr_lo = 0xFEE00000;  /* LAPIC base, CPU 0 */
    msix_table[0].msg_addr_hi = 0;
    msix_table[0].msg_data    = vec_queue;    /* IDT vector */
    msix_table[0].vector_ctrl = 0;            /* Unmasked */

    /* Program MSI-X table entry 1: config change interrupt */
    msix_table[1].msg_addr_lo = 0xFEE00000;
    msix_table[1].msg_addr_hi = 0;
    msix_table[1].msg_data    = vec_config;
    msix_table[1].vector_ctrl = 0;

    /* Assign MSI-X vectors in VirtIO common config:
     * queue_msix_vector for queue 0, config_msix_vector for config changes */
    volatile uint8_t *cfg = dev->common_cfg;

    /* Select queue 0 and assign MSI-X vector 0 */
    mmio_write16((volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_SELECT), 0);
    mmio_write16((volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_MSIX_VECTOR), 0);

    /* Readback: device returns 0xFFFF if vector assignment failed */
    readback = mmio_read16(
        (volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_MSIX_VECTOR));
    if (readback == 0xFFFF) {
        klog(LOG_DEBUG, "virtio", "MSI-X: queue vector assignment rejected");
        irq_free_vector(vec_queue);
        irq_free_vector(vec_config);
        return -1;
    }

    /* Assign config change vector (MSI-X table entry 1) */
    mmio_write16((volatile uint16_t *)(cfg + VIRTIO_COMMON_MSIX_CONFIG), 1);

    readback = mmio_read16(
        (volatile uint16_t *)(cfg + VIRTIO_COMMON_MSIX_CONFIG));
    if (readback == 0xFFFF) {
        klog(LOG_DEBUG, "virtio", "MSI-X: config vector assignment rejected");
        irq_free_vector(vec_queue);
        irq_free_vector(vec_config);
        return -1;
    }

    /* Enable MSI-X: set bit 15 in Message Control, clear function mask (bit 14) */
    msix_ctrl = pci_read16(bus, pci_dev, func, msix_cap_off + 2);
    msix_ctrl |= (1 << 15);    /* MSI-X Enable */
    msix_ctrl &= ~(1 << 14);   /* Clear Function Mask */
    pci_write16(bus, pci_dev, func, msix_cap_off + 2, msix_ctrl);

    /* Disable legacy INTx: set bit 10 in PCI Command Register */
    {
        uint16_t cmd = pci_read16(bus, pci_dev, func, PCI_COMMAND);
        cmd |= PCI_CMD_INT_DISABLE;
        pci_write16(bus, pci_dev, func, PCI_COMMAND, cmd);
    }

    *queue_vector  = vec_queue;
    *config_vector = vec_config;

    klog(LOG_DEBUG, "virtio",
           "MSI-X enabled: %u entries, queue->vec 0x%x, config->vec 0x%x",
           (uint64_t)table_size, (uint64_t)vec_queue, (uint64_t)vec_config);

    return 0;
}

int virtio_pci_setup_msix_multi(struct virtio_pci_dev *dev,
                                uint8_t bus, uint8_t pci_dev, uint8_t func,
                                uint16_t nqueues,
                                uint8_t *queue_vectors, uint8_t *config_vector)
{
    uint16_t status;
    uint8_t cap_off;
    uint8_t msix_cap_off = 0;
    uint16_t msix_ctrl;
    uint16_t table_size;
    uint8_t  table_bir;
    uint32_t table_offset;
    uint64_t bar_addr;
    volatile struct msix_table_entry *msix_table;
    uint8_t vec_config;
    uint16_t readback;
    int i;
    int allocated = 0;

    /* Walk PCI capability list for MSI-X (cap ID 0x11) */
    status = pci_read16(bus, pci_dev, func, 0x06);
    if (!(status & (1 << 4))) {
        klog(LOG_DEBUG, "virtio", "MSI-X multi: no PCI capabilities list");
        return -1;
    }

    cap_off = pci_read8(bus, pci_dev, func, 0x34) & 0xFC;
    while (cap_off != 0) {
        uint8_t cap_id = pci_read8(bus, pci_dev, func, cap_off);
        if (cap_id == PCI_CAP_ID_MSIX) {
            msix_cap_off = cap_off;
            break;
        }
        cap_off = pci_read8(bus, pci_dev, func, cap_off + 1) & 0xFC;
    }

    if (!msix_cap_off) {
        klog(LOG_DEBUG, "virtio", "MSI-X multi: capability not found");
        return -1;
    }

    /* Read MSI-X Message Control */
    msix_ctrl = pci_read16(bus, pci_dev, func, msix_cap_off + 2);
    table_size = (msix_ctrl & 0x07FF) + 1;

    /* Need nqueues + 1 entries (queues + config) */
    if (table_size < (uint16_t)(nqueues + 1)) {
        klog(LOG_DEBUG, "virtio",
               "MSI-X multi: table too small (%u entries, need %u)",
               (uint64_t)table_size, (uint64_t)(nqueues + 1));
        return -1;
    }

    /* Read Table BIR and offset */
    {
        uint32_t table_reg = pci_read32(bus, pci_dev, func, msix_cap_off + 4);
        table_bir    = (uint8_t)(table_reg & 0x07);
        table_offset = table_reg & 0xFFFFFFF8;
    }

    /* Read BAR for MSI-X table */
    {
        uint32_t bar_val = pci_read32(bus, pci_dev, func,
                                       PCI_BAR0 + table_bir * 4);
        if (bar_val & 0x01) {
            klog(LOG_DEBUG, "virtio", "MSI-X multi: BAR%u is I/O space",
                   (uint64_t)table_bir);
            return -1;
        }

        uint8_t bar_type = (bar_val >> 1) & 0x03;
        bar_addr = (uint64_t)(bar_val & 0xFFFFFFF0);

        if (bar_type == 0x02) {
            uint32_t bar_hi = pci_read32(bus, pci_dev, func,
                                          PCI_BAR0 + (table_bir + 1) * 4);
            bar_addr |= ((uint64_t)bar_hi << 32);
        }
    }

    ensure_bar_mapped(bar_addr, table_bir, bus, pci_dev, func);
    msix_table = (volatile struct msix_table_entry *)(bar_addr + table_offset);

    /* Allocate IDT vectors: one per queue + one for config */
    for (i = 0; i < (int)nqueues; i++) {
        queue_vectors[i] = irq_alloc_vector();
        if (!queue_vectors[i]) {
            klog(LOG_DEBUG, "virtio",
                   "MSI-X multi: no free IDT vector for queue %u",
                   (uint64_t)i);
            goto fail_free;
        }
        allocated++;
    }

    vec_config = irq_alloc_vector();
    if (!vec_config) {
        klog(LOG_DEBUG, "virtio",
               "MSI-X multi: no free IDT vector for config");
        goto fail_free;
    }

    /* Program MSI-X table entries for queues */
    for (i = 0; i < (int)nqueues; i++) {
        msix_table[i].msg_addr_lo = 0xFEE00000;  /* LAPIC base, CPU 0 */
        msix_table[i].msg_addr_hi = 0;
        msix_table[i].msg_data    = queue_vectors[i];
        msix_table[i].vector_ctrl = 0;  /* Unmasked */
    }

    /* Program MSI-X table entry for config (entry N) */
    msix_table[nqueues].msg_addr_lo = 0xFEE00000;
    msix_table[nqueues].msg_addr_hi = 0;
    msix_table[nqueues].msg_data    = vec_config;
    msix_table[nqueues].vector_ctrl = 0;

    /* Assign MSI-X vectors in VirtIO common config */
    {
        volatile uint8_t *cfg = dev->common_cfg;

        for (i = 0; i < (int)nqueues; i++) {
            mmio_write16((volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_SELECT),
                          (uint16_t)i);
            mmio_write16(
                (volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_MSIX_VECTOR),
                (uint16_t)i);

            readback = mmio_read16(
                (volatile uint16_t *)(cfg + VIRTIO_COMMON_Q_MSIX_VECTOR));
            if (readback == 0xFFFF) {
                klog(LOG_DEBUG, "virtio",
                       "MSI-X multi: queue %u vector assignment rejected",
                       (uint64_t)i);
                irq_free_vector(vec_config);
                goto fail_free;
            }
        }

        /* Assign config change vector (MSI-X table entry N) */
        mmio_write16((volatile uint16_t *)(cfg + VIRTIO_COMMON_MSIX_CONFIG),
                      nqueues);

        readback = mmio_read16(
            (volatile uint16_t *)(cfg + VIRTIO_COMMON_MSIX_CONFIG));
        if (readback == 0xFFFF) {
            klog(LOG_DEBUG, "virtio",
                   "MSI-X multi: config vector assignment rejected");
            irq_free_vector(vec_config);
            goto fail_free;
        }
    }

    /* Enable MSI-X */
    msix_ctrl = pci_read16(bus, pci_dev, func, msix_cap_off + 2);
    msix_ctrl |= (1 << 15);
    msix_ctrl &= ~(1 << 14);
    pci_write16(bus, pci_dev, func, msix_cap_off + 2, msix_ctrl);

    /* Disable legacy INTx */
    {
        uint16_t cmd = pci_read16(bus, pci_dev, func, PCI_COMMAND);
        cmd |= PCI_CMD_INT_DISABLE;
        pci_write16(bus, pci_dev, func, PCI_COMMAND, cmd);
    }

    *config_vector = vec_config;

    klog(LOG_DEBUG, "virtio",
           "MSI-X multi: %u queue vectors + config, table_size=%u",
           (uint64_t)nqueues, (uint64_t)table_size);

    return 0;

fail_free:
    for (i = 0; i < allocated; i++)
        irq_free_vector(queue_vectors[i]);
    return -1;
}
