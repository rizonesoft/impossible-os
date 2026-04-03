/* blk_io.c -- Block read/write I/O (3-descriptor chain) */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Block I/O ---- */
int virtio_blk_do_io(uint32_t type, uint64_t sector,
                             uint32_t len, void *buffer)
{
    struct virtio_blk_req req;
    uint8_t status_byte = 0xFF;
    int d0, d1, d2;
    uint32_t timeout;
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint64_t submit_tsc = rdtsc_read();  /* Latency telemetry: stamp submission */
    int lat_type = (type == VIRTIO_BLK_T_IN) ? LAT_TYPE_READ : LAT_TYPE_WRITE;

    if (!blk_initialized)
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

    /* Allocate descriptors and build chain */
    if (has_indirect) {
        /* Indirect path: build 3-entry indirect table on stack,
         * allocate only 1 descriptor from main ring (saves 2 slots). */
        struct virtq_desc indirect[3];
        int di;

        if (blk_vqs[qi].num_free < 1) {
            klog(LOG_DEBUG, "virtio", "No free descriptor for indirect I/O");
            return -1;
        }

        /* Build indirect table: header + data + status */
        indirect[0].addr  = (uint64_t)(uintptr_t)&req;
        indirect[0].len   = sizeof(struct virtio_blk_req);
        indirect[0].flags = VIRTQ_DESC_F_NEXT;
        indirect[0].next  = 1;

        indirect[1].addr  = (uint64_t)(uintptr_t)buffer;
        indirect[1].len   = len;
        indirect[1].flags = VIRTQ_DESC_F_NEXT;
        if (type != VIRTIO_BLK_T_OUT)
            indirect[1].flags |= VIRTQ_DESC_F_WRITE;
        indirect[1].next  = 2;

        indirect[2].addr  = (uint64_t)(uintptr_t)&status_byte;
        indirect[2].len   = 1;
        indirect[2].flags = VIRTQ_DESC_F_WRITE;
        indirect[2].next  = 0;

        /* Allocate 1 primary descriptor */
        d0 = blk_vqs[qi].free_head;
        blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
        blk_vqs[qi].num_free--;

        /* Primary descriptor: points to indirect table */
        blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)indirect;
        blk_vqs[qi].desc[d0].len   = 3 * 16;  /* 3 entries × 16 bytes each */
        blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_INDIRECT;
        blk_vqs[qi].desc[d0].next  = 0;

        d1 = -1;  /* Mark as unused for cleanup */
        d2 = -1;

        /* Add to available ring */
        di = (int)(blk_vqs[qi].avail->idx % blk_vqs[qi].size);
        blk_vqs[qi].avail->ring[di] = (uint16_t)d0;

        wmb();
        blk_vqs[qi].avail->idx++;
        mb();

        /* Wait for completion -- adaptive strategy */
        __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
        virtio_irq_flags[qi] = 0;
        __asm__ volatile ("sti");

        virtq_kick(&blk_vqs[qi]);

        {
            int current_mode = (blk_use_events && adaptive.enabled)
                               ? adaptive.mode
                               : VIRTIO_IO_MODE_INTERRUPT;

            if (!blk_use_events) {
                timeout = 5000000;
                while (timeout-- > 0) {
                    mb();
                    if (blk_vqs[qi].used->idx != blk_vqs[qi].last_used)
                        break;
                    __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
                }
                if (timeout == 0) {
                    klog(LOG_DEBUG, "virtio",
                           "I/O timeout (indirect poll, avail=%u, used=%u)",
                           (uint64_t)blk_vqs[qi].avail->idx,
                           (uint64_t)blk_vqs[qi].used->idx);
                    if (!(rflags & (1 << 9)))
                        __asm__ volatile ("cli");
                    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
                    return -1;
                }
            } else if (current_mode == VIRTIO_IO_MODE_POLL) {
                timeout = 5000000;
                while (timeout-- > 0) {
                    mb();
                    if (blk_vqs[qi].used->idx != blk_vqs[qi].last_used)
                        break;
                    __asm__ volatile ("pause");
                }
                if (timeout == 0) {
                    klog(LOG_DEBUG, "virtio",
                           "I/O timeout (indirect adaptive poll)");
                    if (!(rflags & (1 << 9)))
                        __asm__ volatile ("cli");
                    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
                    return -1;
                }
            } else if (current_mode == VIRTIO_IO_MODE_HYBRID) {
                if (!hybrid_spin_poll(&blk_vqs[qi], adaptive.spin_us)) {
                    if (!event_wait_timeout(&io_completions[qi], 5000)) {
                        klog(LOG_DEBUG, "virtio",
                               "I/O timeout (indirect hybrid)");
                        if (!(rflags & (1 << 9)))
                            __asm__ volatile ("cli");
                        virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
                        return -1;
                    }
                }
            } else {
                if (!event_wait_timeout(&io_completions[qi], 5000)) {
                    klog(LOG_DEBUG, "virtio",
                           "I/O timeout (indirect, avail=%u, used=%u)",
                           (uint64_t)blk_vqs[qi].avail->idx,
                           (uint64_t)blk_vqs[qi].used->idx);
                    if (!(rflags & (1 << 9)))
                        __asm__ volatile ("cli");
                    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
                    return -1;
                }
            }
        }

        if (!(rflags & (1 << 9)))
            __asm__ volatile ("cli");

        rmb();
        blk_vqs[qi].last_used++;
        if (has_event_idx)
            virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;
        virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);

        adaptive.io_count++;
        queue_stats[qi].io_completed++;
        queue_stats[qi].io_count_window++;
        latency_record(submit_tsc, lat_type);
        adaptive_check_window();

        return (status_byte == VIRTIO_BLK_S_OK) ? 0 : -1;
    }

    /* Direct path: allocate 3 descriptors from the virtqueue free list */
    if (blk_vqs[qi].num_free < 3) {
        klog(LOG_DEBUG, "virtio", "No free descriptors");
        return -1;
    }

    /* Descriptor 0: request header (device-readable) */
    d0 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    blk_vqs[qi].desc[d0].len   = sizeof(struct virtio_blk_req);
    blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 1: data buffer */
    d1 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d1].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].next = (uint16_t)d1;

    blk_vqs[qi].desc[d1].addr  = (uint64_t)(uintptr_t)buffer;
    blk_vqs[qi].desc[d1].len   = len;
    blk_vqs[qi].desc[d1].flags = VIRTQ_DESC_F_NEXT;
    if (type != VIRTIO_BLK_T_OUT)
        blk_vqs[qi].desc[d1].flags |= VIRTQ_DESC_F_WRITE;  /* device writes to buf */

    /* Descriptor 2: status byte (device-writable) */
    d2 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d2].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d1].next = (uint16_t)d2;

    blk_vqs[qi].desc[d2].addr  = (uint64_t)(uintptr_t)&status_byte;
    blk_vqs[qi].desc[d2].len   = 1;
    blk_vqs[qi].desc[d2].flags = VIRTQ_DESC_F_WRITE;
    blk_vqs[qi].desc[d2].next  = 0;

    /* Add chain head to available ring */
    {
        uint16_t avail_idx = blk_vqs[qi].avail->idx % blk_vqs[qi].size;
        blk_vqs[qi].avail->ring[avail_idx] = (uint16_t)d0;
    }

    /* VirtIO §2.7.13.1: driver MUST perform a suitable device-specific
     * memory barrier before the avail->idx update to ensure the device
     * sees the descriptor chain written above. */
    wmb();
    blk_vqs[qi].avail->idx++;

    /* Barrier before notification: device must see the new avail->idx
     * before we write the notification register. */
    mb();

    /* ---- Adaptive completion strategy ---- */
    {
        int current_mode = (blk_use_events && adaptive.enabled)
                           ? adaptive.mode
                           : VIRTIO_IO_MODE_INTERRUPT;

        /* POLL mode: suppress device interrupts */
        if (current_mode == VIRTIO_IO_MODE_POLL) {
            blk_vqs[qi].avail->flags |= VIRTQ_AVAIL_F_NO_INTERRUPT;
            wmb();
        } else {
            blk_vqs[qi].avail->flags &= ~VIRTQ_AVAIL_F_NO_INTERRUPT;
            wmb();
        }

        /* Clear completion flag and enable CPU interrupts */
        __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
        virtio_irq_flags[qi] = 0;
        __asm__ volatile ("sti");

        /* Notify device (modern MMIO notification) */
        virtq_kick(&blk_vqs[qi]);

        /* Wait for completion -- mode-dependent strategy */
        if (!blk_use_events) {
            /* Pre-scheduler: always poll (no events available) */
            timeout = 5000000;
            while (timeout-- > 0) {
                mb();
                if (blk_vqs[qi].used->idx != blk_vqs[qi].last_used)
                    break;
                __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
            }
            if (timeout == 0) {
                klog(LOG_DEBUG, "virtio",
                       "I/O timeout (poll, avail=%u, used=%u)",
                       (uint64_t)blk_vqs[qi].avail->idx,
                       (uint64_t)blk_vqs[qi].used->idx);
                if (!(rflags & (1 << 9)))
                    __asm__ volatile ("cli");
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
                return -1;
            }
        } else if (current_mode == VIRTIO_IO_MODE_POLL) {
            /* Pure polling: spin on used->idx with timeout */
            timeout = 5000000;
            while (timeout-- > 0) {
                mb();
                if (blk_vqs[qi].used->idx != blk_vqs[qi].last_used)
                    break;
                __asm__ volatile ("pause");
            }
            if (timeout == 0) {
                klog(LOG_DEBUG, "virtio",
                       "I/O timeout (adaptive poll, avail=%u, used=%u)",
                       (uint64_t)blk_vqs[qi].avail->idx,
                       (uint64_t)blk_vqs[qi].used->idx);
                if (!(rflags & (1 << 9)))
                    __asm__ volatile ("cli");
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
                return -1;
            }
        } else if (current_mode == VIRTIO_IO_MODE_HYBRID) {
            /* Hybrid: brief rdtsc spin, then fallback to ISR */
            if (!hybrid_spin_poll(&blk_vqs[qi], adaptive.spin_us)) {
                /* Spin didn't catch it -- fall back to ISR wait */
                if (!event_wait_timeout(&io_completions[qi], 5000)) {
                    klog(LOG_DEBUG, "virtio",
                           "I/O timeout (hybrid, avail=%u, used=%u)",
                           (uint64_t)blk_vqs[qi].avail->idx,
                           (uint64_t)blk_vqs[qi].used->idx);
                    if (!(rflags & (1 << 9)))
                        __asm__ volatile ("cli");
                    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
                    virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
                    virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
                    return -1;
                }
            }
        } else {
            /* Pure interrupt: ISR calls event_set(), we block here */
            if (!event_wait_timeout(&io_completions[qi], 5000)) {
                klog(LOG_DEBUG, "virtio",
                       "I/O timeout (event, avail=%u, used=%u)",
                       (uint64_t)blk_vqs[qi].avail->idx,
                       (uint64_t)blk_vqs[qi].used->idx);
                if (!(rflags & (1 << 9)))
                    __asm__ volatile ("cli");
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
                virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
                return -1;
            }
        }
    }

    /* Restore interrupt state */
    if (!(rflags & (1 << 9)))
        __asm__ volatile ("cli");

    /* VirtIO §2.7.14: after reading used->idx, driver MUST perform
     * a read barrier before accessing used->ring[] entries. */
    rmb();

    /* Consume the used ring entry */
    blk_vqs[qi].last_used++;
    if (has_event_idx)
        virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;

    /* Free all three descriptors */
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);

    /* Track IOPS for adaptive mode switching */
    adaptive.io_count++;
    queue_stats[qi].io_completed++;
    queue_stats[qi].io_count_window++;
    latency_record(submit_tsc, lat_type);
    adaptive_check_window();

    /* Differentiate status codes */
    if (status_byte == VIRTIO_BLK_S_OK)
        return VIRTIO_IO_OK;
    if (status_byte == VIRTIO_BLK_S_UNSUPP) {
        error_stats.unsupp_errors++;
        return VIRTIO_IO_UNSUPP;
    }
    /* S_IOERR or any unknown status */
    error_stats.io_errors++;
    return VIRTIO_IO_IOERR;
}


