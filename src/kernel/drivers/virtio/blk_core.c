/* blk_core.c -- Driver globals, TSC, IRQ handlers, config change, feature helpers */

#define BLK_DEFINE_GLOBALS
#include "kernel/drivers/virtio/blk_internal.h"

void calibrate_tsc(void)
{
    uint64_t start, end;
    uint32_t i;
    start = rdtsc_read();
    for (i = 0; i < 1000; i++)
        __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
    end = rdtsc_read();
    /* 1000 port reads ≈ 1000 µs ≈ 1 ms */
    tsc_per_us = (end - start) / 1000;
    if (tsc_per_us == 0)
        tsc_per_us = 2000;  /* Fallback */
}

/* ---- MSI-X IRQ handlers ---- */

/* Queue completion interrupt -- device placed buffers in the used ring.
 * The ctx pointer carries the queue index. */
void virtio_blk_queue_irq(uint8_t vector, void *ctx)
{
    int qi = (int)(uintptr_t)ctx;
    (void)vector;

    /* When priority queues are active, process high-priority queue first
     * by handling queue 0 (Critical/High) completions before others. */
    if (priority_queues_active && qi != IO_QUEUE_HIGH) {
        /* Check if high-priority queue has pending completions */
        if (blk_vqs[IO_QUEUE_HIGH].used->idx !=
            blk_vqs[IO_QUEUE_HIGH].last_used) {
            virtio_irq_flags[IO_QUEUE_HIGH] = 1;
            if (blk_use_events)
                event_set(&io_completions[IO_QUEUE_HIGH]);
        }
    }

    if (qi >= 0 && qi < (int)num_queues) {
        virtio_irq_flags[qi] = 1;
        if (blk_use_events)
            event_set(&io_completions[qi]);
    }
}

/* Config change interrupt -- device resized, topology changed, etc.
 * VirtIO 1.2 §4.1.4.5: ISR bit 1 indicates device config has changed. */
void virtio_blk_config_irq(uint8_t vector, void *ctx)
{
    (void)vector;
    (void)ctx;

    /* Read ISR to acknowledge interrupt (read clears pending bits) */
    uint8_t isr = 0;
    if (blk_dev.isr_cfg) {
        isr = virtio_read_isr(&blk_dev);
    }

    /* Check DEVICE_NEEDS_RESET (status bit 6) -- fatal condition */
    uint8_t st = virtio_get_status(&blk_dev);
    if (st & VIRTIO_STATUS_DEVICE_NEEDS_RESET) {
        klog(LOG_DEBUG, "virtio", "Config ISR: DEVICE_NEEDS_RESET -- reset required");
        return;
    }

    /* Bit 1 = configuration change (capacity, topology, writeback) */
    if (isr & VIRTIO_PCI_ISR_CONFIG) {
        virtio_blk_handle_config_change();
    }
}

/* ---- Live config change handling (§14.1) ---- */

/* Atomically re-read device configuration using config_generation loop.
 * Detects changes to capacity, topology, and writeback mode.
 * Called from config change ISR -- keep fast and non-blocking. */
void virtio_blk_handle_config_change(void)
{
    if (!blk_initialized || !blk_dev.device_cfg)
        return;

    /* ---- Atomically read new capacity via config_generation ---- */
    uint64_t new_capacity;
    {
        uint8_t gen1, gen2;
        do {
            gen1 = virtio_read_config_generation(&blk_dev);
            volatile uint32_t *cap_lo = (volatile uint32_t *)
                (blk_dev.device_cfg + VIRTIO_BLK_CFG_CAPACITY);
            volatile uint32_t *cap_hi = (volatile uint32_t *)
                (blk_dev.device_cfg + VIRTIO_BLK_CFG_CAPACITY + 4);
            new_capacity = ((uint64_t)mmio_read32(cap_hi) << 32) |
                            (uint64_t)mmio_read32(cap_lo);
            gen2 = virtio_read_config_generation(&blk_dev);
        } while (gen1 != gen2);
    }

    /* ---- Detect capacity change (hot-resize) ---- */
    if (new_capacity != disk_capacity) {
        uint64_t old_capacity = disk_capacity;
        disk_capacity = new_capacity;

        if (new_capacity > old_capacity) {
            /* Disk grew -- safe, notify block device layer */
            klog(LOG_DEBUG, "virtio",
                   "Config change: capacity increased %u -> %u sectors (hot-resize)",
                   old_capacity, new_capacity);
            blkdev_update_capacity("virtio0", new_capacity);
        } else {
            /* Disk shrunk -- dangerous! Data beyond new boundary is lost.
             * Log critical warning but still update to prevent OOB I/O. */
            klog(LOG_DEBUG, "virtio",
                   "Config change: WARNING capacity decreased %u -> %u sectors",
                   old_capacity, new_capacity);
            blkdev_update_capacity("virtio0", new_capacity);
        }

        error_stats.config_changes++;
    }

    /* ---- Detect topology change ---- */
    if (has_topology && blk_dev.device_cfg) {
        uint8_t new_phys_exp = mmio_read8(
            (volatile uint8_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_PHYS_BLK_EXP));
        uint32_t new_opt_io = mmio_read32(
            (volatile uint32_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_OPT_IO_SIZE));

        if (new_phys_exp != topo.physical_block_exp ||
            new_opt_io != topo.opt_io_size) {
            klog(LOG_DEBUG, "virtio",
                   "Config change: topology phys_exp %u->%u opt_io %u->%u",
                   (uint64_t)topo.physical_block_exp, (uint64_t)new_phys_exp,
                   (uint64_t)topo.opt_io_size, (uint64_t)new_opt_io);
            topo.physical_block_exp = new_phys_exp;
            topo.alignment_offset = mmio_read8(
                (volatile uint8_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_ALIGN_OFFSET));
            topo.min_io_size = mmio_read16(
                (volatile uint16_t *)(blk_dev.device_cfg + VIRTIO_BLK_CFG_MIN_IO_SIZE));
            topo.opt_io_size = new_opt_io;
            error_stats.config_changes++;
        }
    }

    /* ---- Detect writeback mode change ---- */
    if (has_config_wce && blk_dev.device_cfg) {
        volatile uint8_t *wb = (volatile uint8_t *)
            (blk_dev.device_cfg + VIRTIO_BLK_CFG_WRITEBACK);
        uint8_t new_wb = mmio_read8(wb);
        /* We don't cache writeback state currently -- just log the change */
        klog(LOG_DEBUG, "virtio", "Config change: writeback=%u", (uint64_t)new_wb);
    }
}

/* ---- Feature negotiation ---- */
uint32_t read_device_features(uint32_t page)
{
    volatile uint8_t *cfg = blk_dev.common_cfg;
    mmio_write32((volatile uint32_t *)(cfg + VIRTIO_COMMON_DFSELECT), page);
    return mmio_read32((volatile uint32_t *)(cfg + VIRTIO_COMMON_DF));
}

void write_driver_features(uint32_t page, uint32_t features)
{
    volatile uint8_t *cfg = blk_dev.common_cfg;
    mmio_write32((volatile uint32_t *)(cfg + VIRTIO_COMMON_GFSELECT), page);
    mmio_write32((volatile uint32_t *)(cfg + VIRTIO_COMMON_GF), features);
}


