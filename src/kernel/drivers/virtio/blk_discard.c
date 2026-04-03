/* blk_discard.c -- Discard (TRIM) and write-zeroes */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Discard (TRIM) ---- */

/* Submit a single discard request for one segment.
 * Uses 3-descriptor chain: header (T_DISCARD) + segment (device-readable) + status. */
int virtio_blk_do_discard(uint64_t sector, uint32_t num_sectors)
{
    struct virtio_blk_req req;
    struct virtio_blk_discard_write_zeroes seg;
    uint8_t status_byte = 0xFF;
    int d0, d1, d2;
    uint32_t timeout;
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint64_t submit_tsc = rdtsc_read();  /* Latency telemetry */

    if (!blk_initialized)
        return -1;

    /* Build request header -- sector field is ignored for discard */
    req.type     = VIRTIO_BLK_T_DISCARD;
    req.reserved = 0;
    req.sector   = 0;

    /* Build discard segment descriptor */
    seg.sector      = sector;
    seg.num_sectors = num_sectors;
    seg.flags       = 0;  /* 0 = discard (not unmap) */

    /* Allocate descriptors and build chain */
    if (has_indirect) {
        struct virtq_desc indirect[3];
        int di;

        if (blk_vqs[qi].num_free < 1) {
            klog(LOG_DEBUG, "virtio", "No free descriptor for indirect discard");
            return -1;
        }

        indirect[0].addr  = (uint64_t)(uintptr_t)&req;
        indirect[0].len   = sizeof(struct virtio_blk_req);
        indirect[0].flags = VIRTQ_DESC_F_NEXT;
        indirect[0].next  = 1;

        indirect[1].addr  = (uint64_t)(uintptr_t)&seg;
        indirect[1].len   = sizeof(struct virtio_blk_discard_write_zeroes);
        indirect[1].flags = VIRTQ_DESC_F_NEXT;  /* Device-readable */
        indirect[1].next  = 2;

        indirect[2].addr  = (uint64_t)(uintptr_t)&status_byte;
        indirect[2].len   = 1;
        indirect[2].flags = VIRTQ_DESC_F_WRITE;
        indirect[2].next  = 0;

        d0 = blk_vqs[qi].free_head;
        blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
        blk_vqs[qi].num_free--;

        blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)indirect;
        blk_vqs[qi].desc[d0].len   = 3 * 16;
        blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_INDIRECT;
        blk_vqs[qi].desc[d0].next  = 0;

        d1 = -1;
        d2 = -1;

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
                klog(LOG_DEBUG, "virtio", "Discard timeout (indirect)");
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
                klog(LOG_DEBUG, "virtio", "Discard timeout (indirect poll)");
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

    /* Direct path: allocate 3 descriptors */
    if (blk_vqs[qi].num_free < 3) {
        klog(LOG_DEBUG, "virtio", "No free descriptors for discard");
        return -1;
    }

    /* Descriptor 0: request header (device-readable) */
    d0 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    blk_vqs[qi].desc[d0].len   = sizeof(struct virtio_blk_req);
    blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 1: discard segment data (device-readable -- NOT F_WRITE!) */
    d1 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d1].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].next = (uint16_t)d1;

    blk_vqs[qi].desc[d1].addr  = (uint64_t)(uintptr_t)&seg;
    blk_vqs[qi].desc[d1].len   = sizeof(struct virtio_blk_discard_write_zeroes);
    blk_vqs[qi].desc[d1].flags = VIRTQ_DESC_F_NEXT;  /* Device-readable: no F_WRITE */

    /* Descriptor 2: status byte (device-writable) */
    d2 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d2].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d1].next = (uint16_t)d2;

    blk_vqs[qi].desc[d2].addr  = (uint64_t)(uintptr_t)&status_byte;
    blk_vqs[qi].desc[d2].len   = 1;
    blk_vqs[qi].desc[d2].flags = VIRTQ_DESC_F_WRITE;
    blk_vqs[qi].desc[d2].next  = 0;

    /* Add to available ring */
    {
        uint16_t avail_idx = blk_vqs[qi].avail->idx % blk_vqs[qi].size;
        blk_vqs[qi].avail->ring[avail_idx] = (uint16_t)d0;
    }

    wmb();
    blk_vqs[qi].avail->idx++;
    mb();

    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    virtio_irq_flags[qi] = 0;
    __asm__ volatile ("sti");

    virtq_kick(&blk_vqs[qi]);

    /* Wait for completion */
    if (blk_use_events) {
        if (!event_wait_timeout(&io_completions[qi], 5000)) {
            klog(LOG_DEBUG, "virtio", "Discard timeout");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
            return VIRTIO_IO_TIMEOUT;
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
            klog(LOG_DEBUG, "virtio", "Discard timeout (poll)");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
            return VIRTIO_IO_TIMEOUT;
        }
    }

    if (!(rflags & (1 << 9)))
        __asm__ volatile ("cli");

    rmb();
    blk_vqs[qi].last_used++;
    if (has_event_idx)
        virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;

    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);

    latency_record(submit_tsc, LAT_TYPE_DISCARD);

    if (status_byte == VIRTIO_BLK_S_OK)
        return VIRTIO_IO_OK;
    if (status_byte == VIRTIO_BLK_S_UNSUPP) {
        error_stats.unsupp_errors++;
        return VIRTIO_IO_UNSUPP;
    }
    error_stats.io_errors++;
    return VIRTIO_IO_IOERR;
}

/* Public discard API -- splits large requests to fit max_discard_sectors */
int virtio_blk_discard(uint64_t sector, uint32_t num_sectors)
{
    int ret;
    uint32_t max_per_cmd;
    uint32_t chunk;

    if (!blk_initialized)
        return -1;
    if (!has_discard)
        return 1;  /* Not supported -- same convention as flush */
    if (is_read_only)
        return 0;  /* RO device -- discard is a no-op */

    /* Determine max sectors per discard command */
    max_per_cmd = topo.max_discard_sectors;
    if (max_per_cmd == 0)
        max_per_cmd = num_sectors;  /* No limit -- send all at once */

    /* Split large discards into max_per_cmd-sized chunks */
    while (num_sectors > 0) {
        chunk = num_sectors;
        if (chunk > max_per_cmd)
            chunk = max_per_cmd;

        ret = virtio_blk_do_discard(sector, chunk);
        if (ret == VIRTIO_IO_TIMEOUT) {
            error_stats.timeouts++;
            klog(LOG_DEBUG, "virtio", "Discard: timeout, triggering reset");
            virtio_blk_reset();
            return -1;
        }
        if (ret != VIRTIO_IO_OK) {
            klog(LOG_DEBUG, "virtio", "Discard: failed at sector %u (%u sectors)",
                   sector, (uint64_t)chunk);
            return -1;
        }

        sector += chunk;
        num_sectors -= chunk;
    }

    return 0;
}

/* ---- Write-Zeroes ---- */

/* Submit a single write-zeroes request for one segment.
 * Uses 3-descriptor chain: header (T_WRITE_ZEROES) + segment (device-readable) + status. */
int virtio_blk_do_write_zeroes(uint64_t sector, uint32_t num_sectors,
                                      uint32_t flags)
{
    struct virtio_blk_req req;
    struct virtio_blk_discard_write_zeroes seg;
    uint8_t status_byte = 0xFF;
    int d0, d1, d2;
    uint32_t timeout;
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint64_t submit_tsc = rdtsc_read();  /* Latency telemetry */

    if (!blk_initialized)
        return -1;

    /* Build request header -- sector field is ignored for write-zeroes */
    req.type     = VIRTIO_BLK_T_WRITE_ZEROES;
    req.reserved = 0;
    req.sector   = 0;

    /* Build write-zeroes segment descriptor */
    seg.sector      = sector;
    seg.num_sectors = num_sectors;
    seg.flags       = flags;

    /* Allocate descriptors and build chain */
    if (has_indirect) {
        struct virtq_desc indirect[3];
        int di;

        if (blk_vqs[qi].num_free < 1) {
            klog(LOG_DEBUG, "virtio", "No free descriptor for indirect write-zeroes");
            return -1;
        }

        indirect[0].addr  = (uint64_t)(uintptr_t)&req;
        indirect[0].len   = sizeof(struct virtio_blk_req);
        indirect[0].flags = VIRTQ_DESC_F_NEXT;
        indirect[0].next  = 1;

        indirect[1].addr  = (uint64_t)(uintptr_t)&seg;
        indirect[1].len   = sizeof(struct virtio_blk_discard_write_zeroes);
        indirect[1].flags = VIRTQ_DESC_F_NEXT;  /* Device-readable */
        indirect[1].next  = 2;

        indirect[2].addr  = (uint64_t)(uintptr_t)&status_byte;
        indirect[2].len   = 1;
        indirect[2].flags = VIRTQ_DESC_F_WRITE;
        indirect[2].next  = 0;

        d0 = blk_vqs[qi].free_head;
        blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
        blk_vqs[qi].num_free--;

        blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)indirect;
        blk_vqs[qi].desc[d0].len   = 3 * 16;
        blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_INDIRECT;
        blk_vqs[qi].desc[d0].next  = 0;

        d1 = -1;
        d2 = -1;

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
                klog(LOG_DEBUG, "virtio", "Write-zeroes timeout (indirect)");
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
                klog(LOG_DEBUG, "virtio", "Write-zeroes timeout (indirect poll)");
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

    /* Direct path: allocate 3 descriptors */
    if (blk_vqs[qi].num_free < 3) {
        klog(LOG_DEBUG, "virtio", "No free descriptors for write-zeroes");
        return -1;
    }

    /* Descriptor 0: request header (device-readable) */
    d0 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    blk_vqs[qi].desc[d0].len   = sizeof(struct virtio_blk_req);
    blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 1: write-zeroes segment data (device-readable -- NOT F_WRITE!) */
    d1 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d1].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].next = (uint16_t)d1;

    blk_vqs[qi].desc[d1].addr  = (uint64_t)(uintptr_t)&seg;
    blk_vqs[qi].desc[d1].len   = sizeof(struct virtio_blk_discard_write_zeroes);
    blk_vqs[qi].desc[d1].flags = VIRTQ_DESC_F_NEXT;  /* Device-readable: no F_WRITE */

    /* Descriptor 2: status byte (device-writable) */
    d2 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d2].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d1].next = (uint16_t)d2;

    blk_vqs[qi].desc[d2].addr  = (uint64_t)(uintptr_t)&status_byte;
    blk_vqs[qi].desc[d2].len   = 1;
    blk_vqs[qi].desc[d2].flags = VIRTQ_DESC_F_WRITE;
    blk_vqs[qi].desc[d2].next  = 0;

    /* Add to available ring */
    {
        uint16_t avail_idx = blk_vqs[qi].avail->idx % blk_vqs[qi].size;
        blk_vqs[qi].avail->ring[avail_idx] = (uint16_t)d0;
    }

    wmb();
    blk_vqs[qi].avail->idx++;
    mb();

    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    virtio_irq_flags[qi] = 0;
    __asm__ volatile ("sti");

    virtq_kick(&blk_vqs[qi]);

    /* Wait for completion */
    if (blk_use_events) {
        if (!event_wait_timeout(&io_completions[qi], 5000)) {
            klog(LOG_DEBUG, "virtio", "Write-zeroes timeout");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
            return VIRTIO_IO_TIMEOUT;
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
            klog(LOG_DEBUG, "virtio", "Write-zeroes timeout (poll)");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
            return VIRTIO_IO_TIMEOUT;
        }
    }

    if (!(rflags & (1 << 9)))
        __asm__ volatile ("cli");

    rmb();
    blk_vqs[qi].last_used++;
    if (has_event_idx)
        virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;

    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);

    latency_record(submit_tsc, LAT_TYPE_DISCARD);  /* write_zeroes → discard */

    if (status_byte == VIRTIO_BLK_S_OK)
        return VIRTIO_IO_OK;
    if (status_byte == VIRTIO_BLK_S_UNSUPP) {
        error_stats.unsupp_errors++;
        return VIRTIO_IO_UNSUPP;
    }
    error_stats.io_errors++;
    return VIRTIO_IO_IOERR;
}

/* Public write-zeroes API -- splits large requests to fit max_wz_sectors */
int virtio_blk_write_zeroes(uint64_t sector, uint32_t num_sectors, int unmap)
{
    int ret;
    uint32_t max_per_cmd;
    uint32_t chunk;
    uint32_t flags;

    if (!blk_initialized)
        return -1;
    if (!has_write_zeroes)
        return 1;  /* Not supported */
    if (is_read_only)
        return 0;  /* RO device -- write-zeroes is a no-op */

    /* Only set unmap flag if device allows it */
    flags = (unmap && topo.wz_may_unmap) ? 1u : 0u;

    /* Determine max sectors per write-zeroes command */
    max_per_cmd = topo.max_wz_sectors;
    if (max_per_cmd == 0)
        max_per_cmd = num_sectors;  /* No limit -- send all at once */

    /* Split large requests into max_per_cmd-sized chunks */
    while (num_sectors > 0) {
        chunk = num_sectors;
        if (chunk > max_per_cmd)
            chunk = max_per_cmd;

        ret = virtio_blk_do_write_zeroes(sector, chunk, flags);
        if (ret == VIRTIO_IO_TIMEOUT) {
            error_stats.timeouts++;
            klog(LOG_DEBUG, "virtio", "Write-zeroes: timeout, triggering reset");
            virtio_blk_reset();
            return -1;
        }
        if (ret != VIRTIO_IO_OK) {
            klog(LOG_DEBUG, "virtio", "Write-zeroes: failed at sector %u (%u sectors)",
                   sector, (uint64_t)chunk);
            return -1;
        }

        sector += chunk;
        num_sectors -= chunk;
    }

    return 0;
}

/* ---- Secure Erase ---- */

/* Submit a single secure erase request for one segment.
 * Uses 3-descriptor chain: header (T_SECURE_ERASE) + segment (device-readable) + status.
 * Same segment format as discard. */
int virtio_blk_do_secure_erase(uint64_t sector, uint32_t num_sectors)
{
    struct virtio_blk_req req;
    struct virtio_blk_discard_write_zeroes seg;
    uint8_t status_byte = 0xFF;
    int d0, d1, d2;
    uint32_t timeout;
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint64_t submit_tsc = rdtsc_read();

    if (!blk_initialized)
        return -1;

    /* Build request header */
    req.type     = VIRTIO_BLK_T_SECURE_ERASE;
    req.reserved = 0;
    req.sector   = 0;  /* sector field ignored -- segment carries range */

    /* Build segment descriptor (same format as discard) */
    seg.sector      = sector;
    seg.num_sectors = num_sectors;
    seg.flags       = 0;

    /* Direct path: allocate 3 descriptors */
    if (blk_vqs[qi].num_free < 3) {
        klog(LOG_DEBUG, "virtio", "No free descriptors for secure erase");
        return -1;
    }

    /* Descriptor 0: request header (device-readable) */
    d0 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d0].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].addr  = (uint64_t)(uintptr_t)&req;
    blk_vqs[qi].desc[d0].len   = sizeof(struct virtio_blk_req);
    blk_vqs[qi].desc[d0].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 1: segment data (device-readable -- NOT F_WRITE!) */
    d1 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d1].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d0].next = (uint16_t)d1;

    blk_vqs[qi].desc[d1].addr  = (uint64_t)(uintptr_t)&seg;
    blk_vqs[qi].desc[d1].len   = sizeof(struct virtio_blk_discard_write_zeroes);
    blk_vqs[qi].desc[d1].flags = VIRTQ_DESC_F_NEXT;

    /* Descriptor 2: status byte (device-writable) */
    d2 = blk_vqs[qi].free_head;
    blk_vqs[qi].free_head = blk_vqs[qi].desc[d2].next;
    blk_vqs[qi].num_free--;

    blk_vqs[qi].desc[d1].next = (uint16_t)d2;

    blk_vqs[qi].desc[d2].addr  = (uint64_t)(uintptr_t)&status_byte;
    blk_vqs[qi].desc[d2].len   = 1;
    blk_vqs[qi].desc[d2].flags = VIRTQ_DESC_F_WRITE;
    blk_vqs[qi].desc[d2].next  = 0;

    /* Add to available ring */
    {
        uint16_t avail_idx = blk_vqs[qi].avail->idx % blk_vqs[qi].size;
        blk_vqs[qi].avail->ring[avail_idx] = (uint16_t)d0;
    }

    wmb();
    blk_vqs[qi].avail->idx++;
    mb();

    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    virtio_irq_flags[qi] = 0;
    __asm__ volatile ("sti");

    virtq_kick(&blk_vqs[qi]);

    /* Wait for completion */
    if (blk_use_events) {
        if (!event_wait_timeout(&io_completions[qi], 10000)) {
            klog(LOG_DEBUG, "virtio", "Secure erase timeout");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
            return VIRTIO_IO_TIMEOUT;
        }
    } else {
        timeout = 10000000;  /* Secure erase may take longer */
        while (timeout-- > 0) {
            mb();
            if (blk_vqs[qi].used->idx != blk_vqs[qi].last_used)
                break;
            __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
        }
        if (timeout == 0) {
            klog(LOG_DEBUG, "virtio", "Secure erase timeout (poll)");
            if (!(rflags & (1 << 9)))
                __asm__ volatile ("cli");
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
            virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);
            return VIRTIO_IO_TIMEOUT;
        }
    }

    if (!(rflags & (1 << 9)))
        __asm__ volatile ("cli");

    rmb();
    blk_vqs[qi].last_used++;
    if (has_event_idx)
        virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;

    virtq_free_desc(&blk_vqs[qi], (uint16_t)d0);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d1);
    virtq_free_desc(&blk_vqs[qi], (uint16_t)d2);

    latency_record(submit_tsc, LAT_TYPE_DISCARD);

    if (status_byte == VIRTIO_BLK_S_OK)
        return VIRTIO_IO_OK;
    if (status_byte == VIRTIO_BLK_S_UNSUPP) {
        error_stats.unsupp_errors++;
        return VIRTIO_IO_UNSUPP;
    }
    error_stats.io_errors++;
    return VIRTIO_IO_IOERR;
}

/* Public secure erase API -- splits large requests to fit max_serase_sectors */
int virtio_blk_secure_erase(uint64_t sector, uint32_t num_sectors)
{
    int ret;
    uint32_t max_per_cmd;
    uint32_t chunk;

    if (!blk_initialized)
        return -1;
    if (!has_secure_erase)
        return 1;  /* Not supported */
    if (is_read_only)
        return -1;  /* Cannot erase a read-only device */

    /* Determine max sectors per secure erase command */
    max_per_cmd = topo.max_serase_sectors;
    if (max_per_cmd == 0)
        max_per_cmd = num_sectors;

    /* Split large erases into max_per_cmd-sized chunks */
    while (num_sectors > 0) {
        chunk = num_sectors;
        if (chunk > max_per_cmd)
            chunk = max_per_cmd;

        ret = virtio_blk_do_secure_erase(sector, chunk);
        if (ret == VIRTIO_IO_TIMEOUT) {
            error_stats.timeouts++;
            klog(LOG_DEBUG, "virtio", "Secure erase: timeout, triggering reset");
            virtio_blk_reset();
            return -1;
        }
        if (ret != VIRTIO_IO_OK) {
            klog(LOG_DEBUG, "virtio", "Secure erase: failed at sector %u (%u sectors)",
                   sector, (uint64_t)chunk);
            return -1;
        }

        sector += chunk;
        num_sectors -= chunk;
    }

    return 0;
}
