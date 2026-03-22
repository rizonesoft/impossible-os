/* blk_init.c — Init, reset, shutdown, hot-plug/unplug */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Initialization ---- */

/* Forward declaration — virtio_blk_init is also called by reset */
int virtio_blk_init(void);

/* Reset device and re-initialize.
 * Called on DEVICE_NEEDS_RESET or unrecoverable I/O timeout. */
int virtio_blk_reset(void)
{
    /* Reset merge tracking before device reset */
    merge_reset();
    uint32_t wait;
    uint16_t qi = 0;  /* Reset always targets queue 0 */

    if (!blk_initialized)
        return -1;

    /* Try per-queue reset first (less disruptive than full device reset) */
    if (has_ring_reset) {
        klog(LOG_DEBUG, "virtio", "Attempting per-queue reset (F_RING_RESET)");
        if (virtio_queue_reset(&blk_vqs[qi]) == 0) {
            klog(LOG_DEBUG, "virtio", "Per-queue reset succeeded");
            error_stats.resets++;
            return 0;
        }
        klog(LOG_DEBUG, "virtio",
               "Per-queue reset failed — falling back to full device reset");
    }

    klog(LOG_DEBUG, "virtio", "Full device reset — reinitializing");
    error_stats.resets++;

    /* VirtIO §2.1.2: driver writes 0 to device_status to reset */
    blk_initialized = 0;
    blk_use_events = 0;
    virtio_set_status(&blk_dev, 0);

    /* Wait for device to acknowledge reset (status reads back 0) */
    wait = 100000;
    while (wait-- > 0) {
        if (virtio_get_status(&blk_dev) == 0)
            break;
        __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
    }
    if (wait == 0) {
        klog(LOG_DEBUG, "virtio", "Device did not reset — aborting");
        return -1;
    }

    /* Re-run full initialization */
    return virtio_blk_init();
}

/* Flush with retry logic */
int virtio_blk_flush(void)
{
    int ret;
    int retry;

    if (!blk_initialized)
        return -1;
    if (!has_flush)
        return 1;
    if (is_read_only)
        return 0;

    for (retry = 0; retry <= VIRTIO_BLK_MAX_RETRIES; retry++) {
        ret = virtio_blk_do_flush();
        if (ret == VIRTIO_IO_OK)
            return 0;
        if (ret == VIRTIO_IO_UNSUPP)
            return -1;
        if (ret == VIRTIO_IO_TIMEOUT) {
            error_stats.timeouts++;
            klog(LOG_DEBUG, "virtio", "Flush: timeout, triggering reset");
            virtio_blk_reset();
            return -1;
        }
        if (retry < VIRTIO_BLK_MAX_RETRIES) {
            klog(LOG_DEBUG, "virtio", "Flush: I/O error, retry %u/%u",
                   (uint64_t)(retry + 1), (uint64_t)VIRTIO_BLK_MAX_RETRIES);
        }
    }
    klog(LOG_DEBUG, "virtio", "Flush: failed after %u retries",
           (uint64_t)VIRTIO_BLK_MAX_RETRIES);
    return -1;
}


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
    topo.max_discard_sectors = 0;
    topo.max_discard_seg = 0;
    topo.discard_sector_alignment = 0;
    topo.max_wz_sectors = 0;
    topo.max_wz_seg = 0;
    topo.wz_may_unmap = 0;

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

    /* Save PCI coordinates for surprise removal detection and hot-unplug */
    saved_pci_bus  = dev.bus;
    saved_pci_dev  = dev.dev;
    saved_pci_func = dev.func;

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

    /* Negotiate F_DISCARD (bit 11): discard (TRIM/UNMAP) support */
    if (feat_lo & (1u << VIRTIO_BLK_F_DISCARD)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_DISCARD);
        has_discard = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_DISCARD (TRIM)");
    }

    /* Negotiate F_WRITE_ZEROES (bit 12): write-zeroes command */
    if (feat_lo & (1u << VIRTIO_BLK_F_WRITE_ZEROES)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_WRITE_ZEROES);
        has_write_zeroes = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_WRITE_ZEROES");
    }

    /* Negotiate F_LIFETIME (bit 13): device lifetime metrics (JESD84-B50) */
    if (feat_lo & (1u << VIRTIO_BLK_F_LIFETIME)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_LIFETIME);
        has_lifetime = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_LIFETIME");
    }

    /* Negotiate F_FUA (bit 14): Force Unit Access per-request (proposed spec) */
    if (feat_lo & (1u << VIRTIO_BLK_F_FUA)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_FUA);
        has_fua = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_FUA (per-request write-through)");
    }

    /* Negotiate F_INLINE_CRYPTO (bit 15): inline encryption (proposed spec) */
    if (feat_lo & (1u << VIRTIO_BLK_F_INLINE_CRYPTO)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_INLINE_CRYPTO);
        has_inline_crypto = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_INLINE_CRYPTO");
    }

    /* Negotiate F_MQ (bit 22): multi-queue (per-CPU request queues) */
    if (feat_lo & (1u << VIRTIO_BLK_F_MQ)) {
        driver_feat_lo |= (1u << VIRTIO_BLK_F_MQ);
        has_mq = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_MQ (multi-queue)");
    }

    /* Negotiate F_RING_INDIRECT_DESC (bit 28): indirect descriptor tables */
    if (feat_lo & (1u << VIRTIO_F_RING_INDIRECT_DESC)) {
        driver_feat_lo |= (1u << VIRTIO_F_RING_INDIRECT_DESC);
        has_indirect = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_RING_INDIRECT_DESC");
    }

    /* Negotiate F_RING_EVENT_IDX (bit 29): interrupt coalescing */
    if (feat_lo & (1u << VIRTIO_F_RING_EVENT_IDX)) {
        driver_feat_lo |= (1u << VIRTIO_F_RING_EVENT_IDX);
        has_event_idx = 1;
        klog(LOG_DEBUG, "virtio", "Negotiated F_RING_EVENT_IDX");
    }

    /* Accept VIRTIO_F_VERSION_1 (bit 0 of page 1)
     * and negotiate VIRTIO_F_RING_RESET (bit 8 of page 1) */
    {
        uint32_t feat_hi = read_device_features(1);
        uint32_t driver_feat_hi = 1;  /* Bit 0 = VIRTIO_F_VERSION_1 */

        /* F_IN_ORDER is bit 35 = page 1 bit 3.
         * When negotiated, the device guarantees it processes and returns
         * descriptors in strict submission order.  This eliminates the
         * need to match used_elem.id — the driver can reclaim buffers
         * sequentially (FIFO), reducing cache and TLB pressure. */
        if (feat_hi & (1u << (VIRTIO_F_IN_ORDER - 32))) {
            driver_feat_hi |= (1u << (VIRTIO_F_IN_ORDER - 32));
            has_in_order = 1;
            klog(LOG_DEBUG, "virtio",
                   "Negotiated F_IN_ORDER (sequential completion)");
        }

        /* F_NOTIFICATION_DATA is bit 38 = page 1 bit 6.
         * When negotiated, notification writes carry additional data:
         * Split VQ: (vqn & 0xFFFF) | (next_avail_idx << 16)
         * This tells the host exactly where new descriptors begin,
         * avoiding a full ring scan on each kick. */
        if (feat_hi & (1u << (VIRTIO_F_NOTIFICATION_DATA - 32))) {
            driver_feat_hi |= (1u << (VIRTIO_F_NOTIFICATION_DATA - 32));
            has_notify_data = 1;
            klog(LOG_DEBUG, "virtio",
                   "Negotiated F_NOTIFICATION_DATA (32-bit kick)");
        }

        /* F_RING_RESET is bit 40 = page 1 bit 8 */
        if (feat_hi & (1u << (VIRTIO_F_RING_RESET - 32))) {
            driver_feat_hi |= (1u << (VIRTIO_F_RING_RESET - 32));
            has_ring_reset = 1;
            klog(LOG_DEBUG, "virtio", "Negotiated F_RING_RESET");
        }

        write_driver_features(1, driver_feat_hi);
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

    /* Step 6: Read num_queues and set up virtqueue(s) */
    num_queues = 1;  /* Default: single queue */
    if (has_mq && blk_dev.device_cfg) {
        uint16_t nq = mmio_read16(
            (volatile uint16_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_NUM_QUEUES));
        if (nq > 1) {
            if (nq > VIRTIO_BLK_MAX_QUEUES)
                nq = VIRTIO_BLK_MAX_QUEUES;
            num_queues = nq;
            klog(LOG_DEBUG, "virtio", "Multi-queue: %u request queues",
                   (uint64_t)num_queues);
        }
    }
    topo.num_queues = num_queues;

    /* Initialize all request queues */
    {
        int qi;
        for (qi = 0; qi < (int)num_queues; qi++) {
            if (virtq_init(&blk_vqs[qi], &blk_dev, (uint16_t)qi) != 0) {
                klog(LOG_DEBUG, "virtio",
                       "Failed to init request queue %u", (uint64_t)qi);
                virtio_set_status(&blk_dev, VIRTIO_STATUS_FAILED);
                return -1;
            }
            /* Set event_idx flag on each queue */
            blk_vqs[qi].event_idx = has_event_idx ? 1 : 0;
            /* Set notify_data flag on each queue */
            blk_vqs[qi].notify_data = has_notify_data ? 1 : 0;
            if (has_event_idx) {
                /* Clear NO_INTERRUPT flag — event_idx supersedes it */
                blk_vqs[qi].avail->flags = 0;
                /* Set initial used_event to current last_used */
                virtq_used_event(&blk_vqs[qi]) = blk_vqs[qi].last_used;
            }
        }
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

    /* Read discard limits if F_DISCARD negotiated */
    if (has_discard && blk_dev.device_cfg) {
        topo.max_discard_sectors = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_MAX_DISCARD_SECTORS));
        topo.max_discard_seg = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_MAX_DISCARD_SEG));
        topo.discard_sector_alignment = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_DISCARD_ALIGN));
        klog(LOG_DEBUG, "virtio", "Discard: max_sectors=%u max_seg=%u align=%u",
               (uint64_t)topo.max_discard_sectors,
               (uint64_t)topo.max_discard_seg,
               (uint64_t)topo.discard_sector_alignment);
    }

    /* Read write-zeroes limits if F_WRITE_ZEROES negotiated */
    if (has_write_zeroes && blk_dev.device_cfg) {
        topo.max_wz_sectors = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_MAX_WZ_SECTORS));
        topo.max_wz_seg = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_MAX_WZ_SEG));
        topo.wz_may_unmap = mmio_read8(
            (volatile uint8_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_WZ_MAY_UNMAP));
        klog(LOG_DEBUG, "virtio", "Write-zeroes: max_sectors=%u max_seg=%u may_unmap=%u",
               (uint64_t)topo.max_wz_sectors,
               (uint64_t)topo.max_wz_seg,
               (uint64_t)topo.wz_may_unmap);
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

    blk_initialized = 1;

    /* Initialize per-queue I/O completion events */
    {
        int qi;
        for (qi = 0; qi < (int)num_queues; qi++) {
            event_init(&io_completions[qi], "virtio-blk-io",
                       EVENT_AUTO_RESET, 0);
        }
    }

    /* Set up MSI-X interrupts */
    if (num_queues > 1) {
        /* Multi-queue: one MSI-X vector per queue + config */
        if (virtio_pci_setup_msix_multi(&blk_dev, dev.bus, dev.dev, dev.func,
                                        num_queues, msix_vec_queues,
                                        &msix_vec_config) != 0) {
            klog(LOG_DEBUG, "virtio",
                   "MSI-X multi setup failed — falling back to single queue");
            /* Fall back to single queue + single MSI-X */
            num_queues = 1;
            topo.num_queues = 1;
            if (virtio_pci_setup_msix(&blk_dev, dev.bus, dev.dev, dev.func,
                                      &msix_vec_queues[0],
                                      &msix_vec_config) != 0) {
                klog(LOG_DEBUG, "virtio", "MSI-X setup failed — polling only");
            } else {
                irq_register(msix_vec_queues[0], virtio_blk_queue_irq,
                             (void *)(uintptr_t)0, "virtio-blk-q0");
                irq_register(msix_vec_config, virtio_blk_config_irq,
                             NULL, "virtio-blk-cfg");
                blk_use_events = 1;
            }
        } else {
            /* Register per-queue ISR handlers */
            int qi;
            for (qi = 0; qi < (int)num_queues; qi++) {
                char name[20];
                name[0]='v'; name[1]='i'; name[2]='r'; name[3]='t';
                name[4]='i'; name[5]='o'; name[6]='-'; name[7]='b';
                name[8]='l'; name[9]='k'; name[10]='-'; name[11]='q';
                name[12] = (char)('0' + qi);
                name[13] = '\0';
                irq_register(msix_vec_queues[qi], virtio_blk_queue_irq,
                             (void *)(uintptr_t)qi, name);
            }
            irq_register(msix_vec_config, virtio_blk_config_irq,
                         NULL, "virtio-blk-cfg");
            blk_use_events = 1;
            klog(LOG_DEBUG, "virtio",
                   "Multi-queue MSI-X: %u queues with per-queue interrupts",
                   (uint64_t)num_queues);
        }
    } else {
        /* Single queue: use original MSI-X setup */
        if (virtio_pci_setup_msix(&blk_dev, dev.bus, dev.dev, dev.func,
                                  &msix_vec_queues[0],
                                  &msix_vec_config) != 0) {
            klog(LOG_DEBUG, "virtio", "MSI-X setup failed — polling only");
        } else {
            irq_register(msix_vec_queues[0], virtio_blk_queue_irq,
                         (void *)(uintptr_t)0, "virtio-blk-q0");
            irq_register(msix_vec_config, virtio_blk_config_irq,
                         NULL, "virtio-blk-cfg");
            blk_use_events = 1;
            klog(LOG_DEBUG, "virtio",
                   "Async I/O: interrupt-driven completion enabled");
        }
    }

    klog(LOG_DEBUG, "virtio", "VirtIO-blk: %u MiB (%u sectors), blk_size=%u, opt_io=%u%s%s",
           (uint64_t)(disk_capacity / 2048),
           (uint64_t)disk_capacity,
           (uint64_t)topo.blk_size,
           (uint64_t)topo.opt_io_size,
           has_flush ? ", flush" : "",
           has_config_wce ? ", wce" : "");

    /* Retrieve device serial number via GET_ID */
    if (virtio_blk_get_id(device_serial, sizeof(device_serial)) == 0) {
        klog(LOG_DEBUG, "virtio", "Device ID: \"%s\"", device_serial);
    } else {
        device_serial[0] = '\0';
        klog(LOG_DEBUG, "virtio", "GET_ID failed — no device serial");
    }

    /* Retrieve device lifetime metrics via GET_LIFETIME */
    if (has_lifetime) {
        struct virtio_blk_lifetime lt;
        if (virtio_blk_get_lifetime(&lt) == 0) {
            /* Decode pre-EOL in human terms */
            const char *eol_status = "unknown";
            if (lt.pre_eol_info == VIRTIO_BLK_PRE_EOL_NORMAL)
                eol_status = "normal";
            else if (lt.pre_eol_info == VIRTIO_BLK_PRE_EOL_WARNING)
                eol_status = "warning (80%% consumed)";
            else if (lt.pre_eol_info == VIRTIO_BLK_PRE_EOL_URGENT)
                eol_status = "urgent (90%% consumed)";

            /* Compute remaining life percentage from typ_a (SLC wear).
             * Values 1–10 are in 10%% increments of used life.
             * remaining = max(0, 100 - typ_a * 10) */
            uint32_t remaining = 100;
            if (lt.device_lifetime_est_typ_a >= 1 &&
                lt.device_lifetime_est_typ_a <= 10) {
                remaining = 100 -
                    (uint32_t)lt.device_lifetime_est_typ_a * 10;
            } else if (lt.device_lifetime_est_typ_a >= 11) {
                remaining = 0;  /* Exceeded */
            }

            klog(LOG_DEBUG, "virtio",
                   "Block: device lifetime: %u%% remaining"
                   " (eol=%s, a=%u, b=%u)",
                   (uint64_t)remaining, eol_status,
                   (uint64_t)lt.device_lifetime_est_typ_a,
                   (uint64_t)lt.device_lifetime_est_typ_b);

            /* Expose via Registry: HKLM\HARDWARE\VirtIO\Block0\Lifetime */
            {

                HKEY hKey = (HKEY)0;
                uint32_t disp;
                if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                        "HARDWARE\\VirtIO\\Block0\\Lifetime", 0,
                        (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                        &hKey, &disp) == ERROR_SUCCESS) {
                    RegSetDword(hKey, "PreEolInfo",
                                (uint32_t)lt.pre_eol_info);
                    RegSetDword(hKey, "LifetimeEstTypA",
                                (uint32_t)lt.device_lifetime_est_typ_a);
                    RegSetDword(hKey, "LifetimeEstTypB",
                                (uint32_t)lt.device_lifetime_est_typ_b);
                    RegSetDword(hKey, "RemainingLifePct",
                                remaining);
                    RegSetString(hKey, "EolStatus", eol_status);
                    RegCloseKey(hKey);
                }
            }
        } else {
            klog(LOG_DEBUG, "virtio", "GET_LIFETIME failed");
        }
    }

    /* ---- Initialize Adaptive Hybrid Polling Engine ---- */
    calibrate_tsc();

    /* Read adaptive polling config from Registry (if available) */
    {
        HKEY hKey = (HKEY)0;
        uint32_t val;

        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\AdaptivePolling", 0,
                KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegGetDword(hKey, "Enabled", &val) == ERROR_SUCCESS)
                adaptive.enabled = (int)val;
            if (RegGetDword(hKey, "LowThreshold", &val) == ERROR_SUCCESS)
                adaptive.low_threshold = val;
            if (RegGetDword(hKey, "HighThreshold", &val) == ERROR_SUCCESS)
                adaptive.high_threshold = val;
            if (RegGetDword(hKey, "SpinMicroseconds", &val) == ERROR_SUCCESS)
                adaptive.spin_us = val;
            RegCloseKey(hKey);
        }
    }

    /* Compute window_ticks: ticks in 100ms */
    {
        uint32_t freq = system_get_freq();
        adaptive.window_ticks = freq / 10;  /* 100ms = 1/10th of a second */
        if (adaptive.window_ticks == 0)
            adaptive.window_ticks = 10;  /* Fallback for pre-timer */
        adaptive.window_start = system_get_ticks();
    }

    /* Write default config to Registry for visibility */
    {
        HKEY hKey = (HKEY)0;
        uint32_t disp;

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\AdaptivePolling", 0,
                (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                &hKey, &disp) == ERROR_SUCCESS) {
            RegSetDword(hKey, "Enabled",
                        (uint32_t)adaptive.enabled);
            RegSetDword(hKey, "LowThreshold",
                        adaptive.low_threshold);
            RegSetDword(hKey, "HighThreshold",
                        adaptive.high_threshold);
            RegSetDword(hKey, "SpinMicroseconds",
                        adaptive.spin_us);
            RegCloseKey(hKey);
        }

        /* Expose current mode */
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                "HARDWARE\\VirtIO\\Block0", 0,
                (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                &hKey, &disp) == ERROR_SUCCESS) {
            RegSetString(hKey, "IoMode",
                         io_mode_names[adaptive.mode]);
            RegCloseKey(hKey);
        }
    }

    klog(LOG_DEBUG, "virtio",
           "Adaptive polling: %s (low=%u, high=%u, spin=%uus, tsc/us=%u)",
           adaptive.enabled ? "enabled" : "disabled",
           (uint64_t)adaptive.low_threshold,
           (uint64_t)adaptive.high_threshold,
           (uint64_t)adaptive.spin_us,
           (uint64_t)tsc_per_us);

    /* ---- I/O Priority Queue Activation ---- */
    if (has_mq && num_queues >= IO_QUEUE_TIERS) {
        priority_queues_active = 1;
        klog(LOG_DEBUG, "virtio",
               "I/O priority queues: ACTIVE (%u queues) — "
               "Q0=Critical/High, Q1=Normal, Q2=Low/VeryLow",
               (uint64_t)num_queues);

        /* Expose queue mapping and initial stats via Registry */
        {
            HKEY hKey = (HKEY)0;
            uint32_t disp;
            static const char *tier_names[] = {"High", "Normal", "Low"};
            uint16_t t;

            for (t = 0; t < IO_QUEUE_TIERS; t++) {
                char path[80];
                /* Build path: HARDWARE\VirtIO\Block0\QueueStats\<tier> */
                int pos = 0;
                const char *prefix = "HARDWARE\\VirtIO\\Block0\\QueueStats\\";
                const char *s;
                for (s = prefix; *s; s++)
                    path[pos++] = *s;
                for (s = tier_names[t]; *s; s++)
                    path[pos++] = *s;
                path[pos] = '\0';

                if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0,
                        (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                        &hKey, &disp) == ERROR_SUCCESS) {
                    RegSetDword(hKey, "QueueIndex", (uint32_t)t);
                    RegSetDword(hKey, "IOPS", 0);
                    RegSetDword(hKey, "TotalIO", 0);
                    RegCloseKey(hKey);
                }
            }
        }
    } else {
        priority_queues_active = 0;
        klog(LOG_DEBUG, "virtio",
               "I/O priority queues: inactive (need MQ with >= 3 queues, "
               "have %u)", (uint64_t)num_queues);
    }

    /* ---- I/O Latency Telemetry Init ---- */
    {
        HKEY hKey = (HKEY)0;
        uint32_t val;

        /* Read config */
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\LatencyTracking", 0,
                KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegGetDword(hKey, "Enabled", &val) == ERROR_SUCCESS)
                latency_tracking_enabled = (int)val;
            RegCloseKey(hKey);
        }

        /* Write back defaults */
        {
            uint32_t disp;
            if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\Drivers\\VirtIO\\LatencyTracking", 0,
                    (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                    &hKey, &disp) == ERROR_SUCCESS) {
                RegSetDword(hKey, "Enabled",
                            (uint32_t)latency_tracking_enabled);
                RegCloseKey(hKey);
            }
        }

        /* Create per-type latency Registry keys with initial values */
        if (latency_tracking_enabled) {
            int t;
            for (t = 0; t < LAT_TYPE_COUNT; t++) {
                char path[96];
                int pos = 0;
                const char *prefix = "HARDWARE\\VirtIO\\Block0\\Latency\\";
                const char *s;
                uint32_t disp;
                int b;

                for (s = prefix; *s; s++)
                    path[pos++] = *s;
                for (s = lat_type_names[t]; *s; s++)
                    path[pos++] = *s;
                path[pos] = '\0';

                if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0,
                        (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                        &hKey, &disp) == ERROR_SUCCESS) {
                    RegSetDword(hKey, "Avg_us", 0);
                    RegSetDword(hKey, "P50_us", 0);
                    RegSetDword(hKey, "P99_us", 0);
                    RegSetDword(hKey, "P999_us", 0);
                    RegSetDword(hKey, "Min_us", 0);
                    RegSetDword(hKey, "Max_us", 0);
                    RegSetDword(hKey, "Total", 0);

                    /* Create histogram sub-key */
                    {
                        char hist_path[128];
                        int hp = 0;
                        for (s = path; *s; s++)
                            hist_path[hp++] = *s;
                        hist_path[hp++] = '\\';
                        {
                            const char *hs = "Histogram";
                            for (s = hs; *s; s++)
                                hist_path[hp++] = *s;
                        }
                        hist_path[hp] = '\0';

                        HKEY hHist = (HKEY)0;
                        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                                hist_path, 0, (const char *)0, 0,
                                KEY_ALL_ACCESS, (void *)0,
                                &hHist, &disp) == ERROR_SUCCESS) {
                            for (b = 0; b < LAT_BUCKET_COUNT; b++)
                                RegSetDword(hHist,
                                    lat_bucket_names[b], 0);
                            RegCloseKey(hHist);
                        }
                    }
                    RegCloseKey(hKey);
                }
            }
        }
    }

    klog(LOG_DEBUG, "virtio",
           "Latency telemetry: %s (tsc/us=%u, ns/tick=%u)",
           latency_tracking_enabled ? "enabled" : "disabled",
           (uint64_t)tsc_per_us,
           (uint64_t)(tsc_per_us > 0 ? 1000 / tsc_per_us : 0));

    /* ---- Sequential Prefetch Init ---- */
    prefetch_init();
    prefetch_expose_registry();

    /* ---- I/O Request Merge Init ---- */
    merge_init();
    merge_expose_registry();

    /* ---- Multi-Device Stripe Init ---- */
    stripe_init();
    stripe_expose_registry();

    /* ---- Force Unit Access (FUA) Init ---- */
    fua_enabled = 1;  /* Default: enabled when supported */
    fua_writes = 0;
    fua_fallback_writes = 0;
    {
        HKEY hKey = (HKEY)0;
        uint32_t val;

        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\FUA", 0,
                KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegGetDword(hKey, "Enabled", &val) == ERROR_SUCCESS)
                fua_enabled = (int)val;
            RegCloseKey(hKey);
        }
    }
    /* Write FUA config and initial stats to Registry */
    {
        HKEY hKey = (HKEY)0;
        uint32_t disp;

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\FUA", 0,
                (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                &hKey, &disp) == ERROR_SUCCESS) {
            RegSetDword(hKey, "Enabled", fua_enabled ? 1 : 0);
            RegSetDword(hKey, "NativeSupport", has_fua ? 1 : 0);
            RegCloseKey(hKey);
        }

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                "HARDWARE\\VirtIO\\Block0\\FUA", 0,
                (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                &hKey, &disp) == ERROR_SUCCESS) {
            RegSetDword(hKey, "FuaWrites", 0);
            RegSetDword(hKey, "FallbackWrites", 0);
            RegSetDword(hKey, "NativeSupport", has_fua ? 1 : 0);
            RegCloseKey(hKey);
        }
    }
    klog(LOG_DEBUG, "virtio", "FUA: %s (native=%s)",
           fua_enabled ? "enabled" : "disabled",
           has_fua ? "yes" : "no (fallback to T_OUT+T_FLUSH)");

    /* ---- Inline Encryption Init ---- */
    crypto_init();
    crypto_expose_registry();

    return 0;
}

/* ---- Hot-Plug / Hot-Unplug ---- */

int virtio_blk_is_surprise_removed(void)
{
    uint16_t vid;

    if (!blk_initialized)
        return 1;  /* Not blk_initialized = effectively gone */

    /* Read PCI vendor ID — if device is surprise-removed, returns 0xFFFF */
    vid = pci_read16(saved_pci_bus, saved_pci_dev, saved_pci_func, 0x00);
    return (vid == 0xFFFF) ? 1 : 0;
}

void virtio_blk_shutdown(void)
{
    int surprise;
    int i;

    if (!blk_initialized)
        return;

    surprise = virtio_blk_is_surprise_removed();

    klog(LOG_DEBUG, "virtio", "Shutdown: %s",
           surprise ? "surprise removal" : "managed removal");

    /* 1. Stop accepting new I/O */
    blk_initialized = 0;
    blk_use_events = 0;

    /* Release prefetch buffer */
    prefetch_shutdown();

    /* 2. If device still present, flush caches and quiesce */
    if (!surprise) {
        /* Flush any dirty cache data */
        if (has_flush) {
            int flush_ret = virtio_blk_flush();
            (void)flush_ret;
        }

        /* Reset device status to stop processing */
        virtio_set_status(&blk_dev, 0);

        /* Wait for device to acknowledge reset */
        {
            uint32_t wait = 100000;
            while (wait-- > 0) {
                if (virtio_get_status(&blk_dev) == 0)
                    break;
                __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
            }
        }
    } else {
        klog(LOG_DEBUG, "virtio",
               "Surprise removal — skipping device I/O during teardown");
    }

    /* 3. Free all virtqueue ring memory */
    {
        int q;
        for (q = 0; q < (int)num_queues; q++) {
            if (blk_vqs[q].desc) {
                uintptr_t old_desc = (uintptr_t)blk_vqs[q].desc;
                uint64_t desc_sz  = ((uint64_t)blk_vqs[q].size * 16 + 15) & ~(uint64_t)15;
                uint64_t avail_sz = (6 + (uint64_t)blk_vqs[q].size * 2 + 1) & ~(uint64_t)1;
                uint64_t used_sz  = (6 + (uint64_t)blk_vqs[q].size * 8 + 3) & ~(uint64_t)3;
                uint64_t total    = desc_sz + avail_sz + used_sz;
                uint64_t old_pages = (total + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

                for (i = 0; i < (int)old_pages; i++)
                    pmm_free_frame(old_desc + (uint64_t)i * PMM_FRAME_SIZE);

                blk_vqs[q].desc  = (struct virtq_desc *)0;
                blk_vqs[q].avail = (struct virtq_avail *)0;
                blk_vqs[q].used  = (struct virtq_used *)0;
            }
        }
    }

    /* 4. Free MSI-X vectors (all queues + config) */
    {
        int q;
        for (q = 0; q < (int)num_queues; q++) {
            if (msix_vec_queues[q]) {
                irq_free_vector(msix_vec_queues[q]);
                msix_vec_queues[q] = 0;
            }
        }
    }
    if (msix_vec_config) {
        irq_free_vector(msix_vec_config);
        msix_vec_config = 0;
    }

    /* 5. Unregister from block device layer */
    blkdev_unregister("virtio0");

    /* 6. Clear all driver state */
    has_flush        = 0;
    has_config_wce   = 0;
    has_blk_size     = 0;
    has_topology     = 0;
    has_size_max     = 0;
    has_seg_max      = 0;
    has_discard      = 0;
    has_write_zeroes = 0;
    has_ring_reset   = 0;
    has_mq           = 0;
    has_indirect     = 0;
    has_event_idx    = 0;
    is_read_only     = 0;
    disk_capacity    = 0;
    num_queues       = 1;

    klog(LOG_DEBUG, "virtio", "Block: device %s",
           surprise ? "surprise removal cleanup complete"
                    : "unregistered (managed removal)");
}

void virtio_blk_hotunplug(void)
{
    klog(LOG_DEBUG, "virtio",
           "Block: hot-unplug at PCI %02x:%02x.%x",
           (uint64_t)saved_pci_bus, (uint64_t)saved_pci_dev,
           (uint64_t)saved_pci_func);
    virtio_blk_shutdown();
}

int virtio_blk_hotplug(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint16_t vid, did, subsys;

    /* Verify this is a VirtIO block device */
    vid = pci_read16(bus, dev, func, 0x00);
    if (vid != VIRTIO_BLK_VENDOR_ID)
        return -1;

    did = pci_read16(bus, dev, func, 0x02);
    if (did != VIRTIO_BLK_DEVICE_ID_MOD && did != VIRTIO_BLK_DEVICE_ID_LEG)
        return -1;

    subsys = pci_read16(bus, dev, func, 0x2E);
    if (did == VIRTIO_BLK_DEVICE_ID_LEG && subsys != 0x0002)
        return -1;

    klog(LOG_DEBUG, "virtio",
           "Block: hot-plugged new device at PCI %02x:%02x.%x",
           (uint64_t)bus, (uint64_t)dev, (uint64_t)func);

    /* If already blk_initialized, shut down first */
    if (blk_initialized)
        virtio_blk_shutdown();

    /* Run full initialization */
    if (virtio_blk_init() != 0) {
        klog(LOG_DEBUG, "virtio", "Block: hot-plug init failed");
        return -1;
    }

    /* Register with block device layer */
    {
        struct blkdev bd = {0};
        bd.name[0]='v'; bd.name[1]='i'; bd.name[2]='r';
        bd.name[3]='t'; bd.name[4]='i'; bd.name[5]='o';
        bd.name[6]='0'; bd.name[7]='\0';
        bd.sector_size  = virtio_blk_block_size();
        bd.sector_count = virtio_blk_capacity();
        bd.read  = (blkdev_read_fn)0;   /* Wired by blkdev_adapters */
        bd.write = (blkdev_write_fn)0;
        bd.flush = (blkdev_flush_fn)0;
        bd.discard = (blkdev_discard_fn)0;
        bd.driver_data = (void *)0;
    }

    klog(LOG_DEBUG, "virtio",
           "Block: hot-plug complete — %u MiB",
           (uint64_t)(disk_capacity / 2048));

    return 0;
}

