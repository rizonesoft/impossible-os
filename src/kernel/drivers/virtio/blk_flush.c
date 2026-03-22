/* blk_flush.c — Cache flush (2-descriptor chain) */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Flush I/O (2-descriptor chain: header + status, no data) ---- */
int virtio_blk_do_flush(void)
{
    struct virtio_blk_req req;
    uint8_t status_byte = 0xFF;
    int d0, d1;
    uint32_t timeout;
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint64_t submit_tsc = rdtsc_read();  /* Latency telemetry */

    if (!blk_initialized)
        return -1;

    /* Build flush request header — sector field is ignored */
    req.type     = VIRTIO_BLK_T_FLUSH;
    req.reserved = 0;
    req.sector   = 0;

    /* Allocate descriptors and build chain */
    if (has_indirect) {
        /* Indirect path: 2-entry table (header + status) */
        struct virtq_desc indirect[2];
        int di;

        if (blk_vqs[qi].num_free < 1) {
            klog(LOG_DEBUG, "virtio", "No free descriptor for indirect flush");
            return -1;
        }

        indirect[0].addr  = (uint64_t)(uintptr_t)&req;
        indirect[0].len   = sizeof(struct virtio_blk_req);
        indirect[0].flags = VIRTQ_DESC_F_NEXT;
        indirect[0].next  = 1;

        indirect[1].addr  = (uint64_t)(uintptr_t)&status_byte;
        indirect[1].len   = 1;
        indirect[1].flags = VIRTQ_DESC_F_WRITE;
        indirect[1].next  = 0;

        d0 = blk_vqs[qi].free_head;
        blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
        blk_vqs[qi].num_free--;

        blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)indirect;
        blk_vqs[qi].desc[d0].len   = 2 * 16;
        blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_INDIRECT;
        blk_vqs[qi].desc[d0].next  = 0;

        d1 = -1;

        di = (int)(blk_vqs[qi].avail->idx % blk_vqs[qi].size);
        blk_vqs[qi].avail->ring[di] = (uint16_t)d0;

        wmb();
        blk_vqs[qi].avail->idx++;
        mb();

        __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
        virtio_irq_flags[qi] = 0;
        __asm__ volatile ("sti");

        virtq_kick(&blk_vqs[qi]);

        if (blk_use_events) {
            if (!event_wait_timeout(&io_completions[qi], 5000)) {
                klog(LOG_DEBUG, "virtio", "Flush timeout (indirect)");
                if (!(rflags & (1 << 9)))
                    __asm__ volatile ("cli");
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
                return -1;
            }
        } else {
            timeout = 5000000;
            while (timeout-- > 0) {
                mb();
                if (blk_vqs[qi].used->idx != blk_vqs[qi].last_used)
                    break;
                __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
            }
            if (timeout == 0) {
                klog(LOG_DEBUG, "virtio", "Flush timeout (indirect poll)");
                if (!(rflags & (1 << 9)))
                    __asm__ volatile ("cli");
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
                return -1;
            }
        }

        if (!(rflags & (1 << 9)))
            __asm__ volatile ("cli");

        rmb();
        blk_vqs[qi].last_used++;
        if (has_event_idx)
            virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;
        virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);

        return (status_byte == VIRTIO_BLK_S_OK) ? 0 : -1;
    }

    /* Direct path: allocate 2 descriptors from the virtqueue free list */
    if (blk_vqs[qi].num_free < 2) {
        klog(LOG_DEBUG, "virtio", "No free descriptors for flush");
        return -1;
    }

    /* Descriptor 0: request header (device-readable) */
    d0 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    blk_vqs[qi].desc[d0].len   = sizeof(struct virtio_blk_req);
    blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 1: status byte (device-writable) — no data descriptor */
    d1 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d1].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].next = (uint16_t)d1;

    blk_vqs[qi].desc[d1].addr  = (uint64_t)(uintptr_t)&status_byte;
    blk_vqs[qi].desc[d1].len   = 1;
    blk_vqs[qi].desc[d1].flags = VIRTQ_DESC_F_WRITE;
    blk_vqs[qi].desc[d1].next  = 0;

    /* Add chain head to available ring */
    {
        uint16_t avail_idx = blk_vqs[qi].avail->idx % blk_vqs[qi].size;
        blk_vqs[qi].avail->ring[avail_idx] = (uint16_t)d0;
    }

    wmb();   /* Ensure descriptors visible before avail->idx update */
    blk_vqs[qi].avail->idx++;
    mb();    /* Ensure avail->idx visible before notification */

    /* Enable interrupts for IRQ delivery */
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    virtio_irq_flags[qi] = 0;
    __asm__ volatile ("sti");

    /* Notify device */
    virtq_kick(&blk_vqs[qi]);

    /* Wait for completion — event-driven or polling fallback */
    if (blk_use_events) {
        /* Flush may take longer — 10s timeout */
        if (!event_wait_timeout(&io_completions[qi], 10000)) {
            klog(LOG_DEBUG, "virtio", "Flush timeout (event)");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            return -1;
        }
    } else {
        timeout = 10000000;
        while (timeout-- > 0) {
            mb();
            if (blk_vqs[qi].used->idx != blk_vqs[qi].last_used)
                break;
            __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
        }
        if (timeout == 0) {
            klog(LOG_DEBUG, "virtio", "Flush timeout (poll)");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            return -1;
        }
    }

    /* Restore interrupt state */
    if (!(rflags & (1 << 9)))
        __asm__ volatile ("cli");

    rmb();   /* Barrier before reading used ring entries */

    /* Consume the used ring entry */
    blk_vqs[qi].last_used++;
    if (has_event_idx)
        virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;

    /* Free both descriptors */
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);

    latency_record(submit_tsc, LAT_TYPE_FLUSH);

    if (status_byte == VIRTIO_BLK_S_OK)
        return VIRTIO_IO_OK;
    if (status_byte == VIRTIO_BLK_S_UNSUPP) {
        error_stats.unsupp_errors++;
        return VIRTIO_IO_UNSUPP;
    }
    error_stats.io_errors++;
    return VIRTIO_IO_IOERR;
}


