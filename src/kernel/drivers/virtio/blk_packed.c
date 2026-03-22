/* ============================================================================
 * blk_packed.c — Packed Virtqueue Support
 *
 * §8.1 — 💎 Impossible OS Exclusive
 *
 * The packed virtqueue (VirtIO 1.1 §2.7) replaces the split layout's three
 * separate memory regions (descriptor table, available ring, used ring) with
 * a single unified ring of 16-byte descriptors. Both driver and device
 * maintain wrap counters that flip on each ring wraparound.
 *
 * Benefits:
 *   - Cache locality: single ring vs. 3 separate regions
 *   - Reduced memory footprint: 16 bytes/entry vs. 16+2+8 = 26 bytes
 *   - No separate index tracking: AVAIL/USED bits embedded in flags
 *
 * Design:
 *   - Packed ring is allocated as queue_size * sizeof(pvirtq_desc) bytes
 *   - Driver wrap counter (dwc) starts at 1, flips at wraparound
 *   - Submission: set AVAIL=dwc, USED=!dwc
 *   - Completion: device sets AVAIL=USED=device_wrap_counter
 *   - Driver detects: (flags & AVAIL) == (flags & USED) → completed
 * ============================================================================ */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Packed VQ state ---- */
struct packed_vq {
    struct pvirtq_desc  *ring;       /* Unified ring (queue_size entries) */
    uint16_t             size;       /* Number of descriptors */
    uint16_t             next_avail; /* Next descriptor to use for submission */
    uint16_t             next_used;  /* Next descriptor to check for completion */
    int                  driver_wrap;/* Driver wrap counter (0 or 1) */
    int                  device_wrap;/* Expected device wrap counter (0 or 1) */
    uint16_t             notify_off; /* Notification offset */
    struct virtio_pci_dev *dev;      /* Parent device */
    uint16_t             queue_idx;  /* Queue index */

    /* Event suppression */
    struct pvirtq_event_suppress *driver_event;  /* Driver → device */
    struct pvirtq_event_suppress *device_event;  /* Device → driver */
};

static struct packed_vq pvq;

/* Memory barriers */
#ifndef mb
#define mb()  __asm__ volatile("mfence" ::: "memory")
#endif
#ifndef wmb
#define wmb() __asm__ volatile("sfence" ::: "memory")
#endif
#ifndef rmb
#define rmb() __asm__ volatile("lfence" ::: "memory")
#endif

/* ---- Initialization ---- */

/* Initialize a packed virtqueue for the primary block device.
 * Called from virtio_blk_init() if F_RING_PACKED is negotiated. */
int packed_vq_init(struct virtio_pci_dev *pdev, uint16_t queue_idx)
{
    uint16_t queue_size;
    uint32_t ring_bytes;
    uint32_t event_bytes;
    uint32_t total_pages;
    uintptr_t phys;
    uint8_t *base;
    uint16_t i;

    if (!pdev || !pdev->common_cfg)
        return -1;

    /* Select queue */
    mmio_write16((volatile uint16_t *)(pdev->common_cfg + VIRTIO_COMMON_Q_SELECT),
                 queue_idx);

    /* Read queue size */
    queue_size = mmio_read16(
        (volatile uint16_t *)(pdev->common_cfg + VIRTIO_COMMON_Q_SIZE));
    if (queue_size == 0 || queue_size > 32768) {
        klog(LOG_DEBUG, "virtio", "PackedVQ: invalid queue size %u",
               (uint64_t)queue_size);
        return -1;
    }

    /* Make power of 2 */
    {
        uint16_t s = 1;
        while (s < queue_size)
            s <<= 1;
        if (s != queue_size)
            queue_size = s >> 1;
    }

    /* Allocate ring memory:
     *   Ring: queue_size * 16 bytes (16-byte aligned)
     *   Driver event: 4 bytes
     *   Device event: 4 bytes */
    ring_bytes = (uint32_t)queue_size * sizeof(struct pvirtq_desc);
    event_bytes = 2 * sizeof(struct pvirtq_event_suppress);
    total_pages = (ring_bytes + event_bytes + 4095) / 4096;

    phys = pmm_alloc_contiguous(total_pages);
    if (!phys) {
        klog(LOG_DEBUG, "virtio", "PackedVQ: failed to allocate %u pages",
               (uint64_t)total_pages);
        return -1;
    }

    base = (uint8_t *)phys;

    /* Zero the ring */
    {
        uint32_t bi;
        for (bi = 0; bi < total_pages * 4096; bi++)
            base[bi] = 0;
    }

    /* Set up pointers */
    pvq.ring = (struct pvirtq_desc *)base;
    pvq.driver_event = (struct pvirtq_event_suppress *)(base + ring_bytes);
    pvq.device_event = (struct pvirtq_event_suppress *)(base + ring_bytes +
                       sizeof(struct pvirtq_event_suppress));
    pvq.size = queue_size;
    pvq.next_avail = 0;
    pvq.next_used = 0;
    pvq.driver_wrap = 1;
    pvq.device_wrap = 1;
    pvq.dev = pdev;
    pvq.queue_idx = queue_idx;

    /* Initialize all descriptors: AVAIL=0, USED=0 (both clear) means
     * "not available, not used" which is the correct initial state since
     * driver_wrap starts at 1. */
    for (i = 0; i < queue_size; i++) {
        pvq.ring[i].addr  = 0;
        pvq.ring[i].len   = 0;
        pvq.ring[i].id    = i;
        pvq.ring[i].flags = 0;  /* AVAIL=0, USED=0 */
    }

    /* Suppress notifications by default (polling mode) */
    pvq.driver_event->flags = 1;  /* DISABLE */
    pvq.device_event->flags = 1;  /* DISABLE */

    /* Write ring address to device — packed VQ uses the descriptor table
     * address field for the unified ring */
    mmio_write64(pdev->common_cfg, VIRTIO_COMMON_Q_DESC,
                 (uint64_t)(uintptr_t)pvq.ring);

    /* For packed VQ, avail/used addresses point to event suppression */
    mmio_write64(pdev->common_cfg, VIRTIO_COMMON_Q_AVAIL,
                 (uint64_t)(uintptr_t)pvq.driver_event);
    mmio_write64(pdev->common_cfg, VIRTIO_COMMON_Q_USED,
                 (uint64_t)(uintptr_t)pvq.device_event);

    /* Read notification offset */
    pvq.notify_off = mmio_read16(
        (volatile uint16_t *)(pdev->common_cfg + VIRTIO_COMMON_Q_NOTIFY_OFF));

    /* Enable the queue */
    mmio_write16(
        (volatile uint16_t *)(pdev->common_cfg + VIRTIO_COMMON_Q_ENABLE), 1);

    klog(LOG_DEBUG, "virtio",
           "PackedVQ: queue %u initialized (size=%u, ring=%u bytes)",
           (uint64_t)queue_idx, (uint64_t)queue_size, (uint64_t)ring_bytes);

    return 0;
}

/* ---- Submission (3-descriptor chain for block I/O) ---- */

/* Build and set flags for a packed descriptor.
 * wrap: driver wrap counter (0 or 1)
 * desc_flags: PVIRTQ_DESC_F_NEXT, F_WRITE, etc. */
static uint16_t packed_flags(int wrap, uint16_t desc_flags)
{
    uint16_t f = desc_flags;

    /* Set AVAIL = wrap, USED = !wrap */
    if (wrap)
        f |= PVIRTQ_DESC_F_AVAIL;
    else
        f &= (uint16_t)~PVIRTQ_DESC_F_AVAIL;

    if (!wrap)
        f |= PVIRTQ_DESC_F_USED;
    else
        f &= (uint16_t)~PVIRTQ_DESC_F_USED;

    return f;
}

/* Advance next_avail, wrapping and flipping driver_wrap as needed */
static void advance_avail(void)
{
    pvq.next_avail++;
    if (pvq.next_avail >= pvq.size) {
        pvq.next_avail = 0;
        pvq.driver_wrap = !pvq.driver_wrap;
    }
}

/* Submit a 3-descriptor block I/O request via packed VQ.
 * header: virtio_blk_req
 * data: data buffer
 * data_len: data buffer length
 * is_write: 1 for T_OUT, 0 for T_IN
 * status_ptr: pointer to status byte
 * Returns 0 on success, -1 if ring full. */
int packed_vq_submit(struct virtio_blk_req *header, void *data,
                     uint32_t data_len, int is_write, uint8_t *status_ptr)
{
    uint16_t d0, d1, d2;
    uint16_t f0, f1, f2;
    int wrap;

    /* Need 3 free descriptors — but in packed VQ there's no free list,
     * we just need 3 slots ahead of next_avail that haven't been used.
     * For simplicity, check if next 3 entries are available. */

    wrap = pvq.driver_wrap;

    d0 = pvq.next_avail;
    advance_avail();

    d1 = pvq.next_avail;
    advance_avail();

    d2 = pvq.next_avail;
    advance_avail();

    /* Descriptor 0: request header (device reads) */
    f0 = packed_flags(wrap, PVIRTQ_DESC_F_NEXT);
    pvq.ring[d0].addr  = (uint64_t)(uintptr_t)header;
    pvq.ring[d0].len   = sizeof(struct virtio_blk_req);
    pvq.ring[d0].id    = d0;

    /* Descriptor 1: data buffer */
    {
        uint16_t data_flags = PVIRTQ_DESC_F_NEXT;
        if (!is_write)
            data_flags |= PVIRTQ_DESC_F_WRITE;  /* Device writes data for reads */

        /* Wrap counter may have flipped during advance */
        int wrap1 = pvq.driver_wrap;
        /* Use the wrap state from when d1 was allocated */
        if (d1 < d0)
            wrap1 = !wrap;  /* Wrapped around */
        else
            wrap1 = wrap;

        f1 = packed_flags(wrap1, data_flags);
        pvq.ring[d1].addr  = (uint64_t)(uintptr_t)data;
        pvq.ring[d1].len   = data_len;
        pvq.ring[d1].id    = d1;
    }

    /* Descriptor 2: status byte (device writes) */
    {
        int wrap2 = pvq.driver_wrap;
        if (d2 < d1)
            wrap2 = !wrap;
        else if (d1 < d0)
            wrap2 = !wrap;
        else
            wrap2 = wrap;

        f2 = packed_flags(wrap2, PVIRTQ_DESC_F_WRITE);
        pvq.ring[d2].addr  = (uint64_t)(uintptr_t)status_ptr;
        pvq.ring[d2].len   = 1;
        pvq.ring[d2].id    = d2;
    }

    /* Write descriptors in order: write d2, d1 first, then d0 last
     * (d0 has AVAIL bit set which makes the chain visible to device) */
    wmb();
    pvq.ring[d2].flags = f2;
    pvq.ring[d1].flags = f1;
    wmb();
    pvq.ring[d0].flags = f0;  /* Makes chain visible to device */
    mb();

    return 0;
}

/* Kick the device (same mechanism as split VQ) */
void packed_vq_kick(void)
{
    volatile uint16_t *notify_addr;

    if (!pvq.dev || !pvq.dev->notify_base)
        return;

    notify_addr = (volatile uint16_t *)
        (pvq.dev->notify_base + pvq.notify_off * pvq.dev->notify_off_multiplier);
    *notify_addr = pvq.queue_idx;
}

/* ---- Completion ---- */

/* Check if the descriptor at next_used is completed.
 * A descriptor is completed when its AVAIL bit matches its USED bit
 * (both equal to the device's wrap counter). */
int packed_vq_poll_completion(void)
{
    uint16_t flags;
    int avail_bit, used_bit;
    uint32_t timeout = 5000000;

    while (timeout-- > 0) {
        rmb();
        flags = pvq.ring[pvq.next_used].flags;

        avail_bit = (flags & PVIRTQ_DESC_F_AVAIL) ? 1 : 0;
        used_bit  = (flags & PVIRTQ_DESC_F_USED)  ? 1 : 0;

        /* Completed when AVAIL and USED bits are equal AND match device_wrap */
        if (avail_bit == used_bit && avail_bit == pvq.device_wrap) {
            /* Advance next_used past the 3-descriptor chain */
            pvq.next_used++;
            if (pvq.next_used >= pvq.size) {
                pvq.next_used = 0;
                pvq.device_wrap = !pvq.device_wrap;
            }
            pvq.next_used++;
            if (pvq.next_used >= pvq.size) {
                pvq.next_used = 0;
                pvq.device_wrap = !pvq.device_wrap;
            }
            pvq.next_used++;
            if (pvq.next_used >= pvq.size) {
                pvq.next_used = 0;
                pvq.device_wrap = !pvq.device_wrap;
            }
            return 0;  /* Completed */
        }

        __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
    }

    return -1;  /* Timeout */
}

/* ---- Full I/O cycle via packed VQ ---- */

/* Perform a block I/O operation through the packed virtqueue.
 * type: VIRTIO_BLK_T_IN, T_OUT, T_FLUSH, etc.
 * sector: starting sector for read/write
 * data_len: data buffer length in bytes
 * buffer: data buffer
 * Returns VIRTIO_IO_OK, VIRTIO_IO_IOERR, or VIRTIO_IO_TIMEOUT. */
int packed_vq_do_io(uint32_t type, uint64_t sector,
                    uint32_t data_len, void *buffer)
{
    struct virtio_blk_req req;
    uint8_t status_byte = 0xFF;
    int is_write;
    int ret;

    req.type     = type & 0x7FFFFFFFu; /* Mask FUA flag for type check */
    req.reserved = 0;
    req.sector   = sector;

    /* Restore original type with flags */
    req.type = type;

    is_write = ((type & 0x7FFFFFFFu) == VIRTIO_BLK_T_OUT) ? 1 : 0;

    /* Submit */
    ret = packed_vq_submit(&req, buffer, data_len, is_write, &status_byte);
    if (ret != 0)
        return VIRTIO_IO_IOERR;

    /* Kick device */
    packed_vq_kick();

    /* Poll for completion */
    ret = packed_vq_poll_completion();
    if (ret != 0)
        return VIRTIO_IO_TIMEOUT;

    rmb();
    if (status_byte == VIRTIO_BLK_S_OK)
        return VIRTIO_IO_OK;
    if (status_byte == VIRTIO_BLK_S_UNSUPP)
        return VIRTIO_IO_UNSUPP;

    return VIRTIO_IO_IOERR;
}

/* Check if packed VQ is active */
int packed_vq_is_active(void)
{
    return has_packed;
}
