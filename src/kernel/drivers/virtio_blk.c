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
 * Flush: 2-descriptor chain (header, status) — no data buffer.
 * ============================================================================ */

#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/virtio.h"
#include "kernel/drivers/pci.h"
#include "kernel/irq.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "registry.h"
#include "kernel/barrier.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/sched/event.h"
#include "kernel/sched/task.h"
#include "kernel/mm/pmm.h"
#include "kernel/smp.h"
#include "kernel/timer.h"

/* ---- Driver state ---- */
static struct virtio_pci_dev  blk_dev;         /* Modern PCI transport */
static struct virtqueue       blk_vqs[VIRTIO_BLK_MAX_QUEUES]; /* Request queues */
static uint64_t               disk_capacity;   /* Total 512-byte sectors */
static int                    initialized;     /* 1 if init succeeded */
static volatile int           virtio_irq_flags[VIRTIO_BLK_MAX_QUEUES];

/* PCI coordinates (saved for surprise removal detection and hot-unplug) */
static uint8_t                saved_pci_bus;
static uint8_t                saved_pci_dev;
static uint8_t                saved_pci_func;
static uint8_t                msix_vec_queues[VIRTIO_BLK_MAX_QUEUES];
static uint8_t                msix_vec_config; /* MSI-X IDT vector: config */
static int                    use_events;      /* 1 after event_t is live */
static uint16_t               num_queues = 1;  /* Active queue count */

/* Interrupt-driven I/O completion events (one per queue) */
static event_t                io_completions[VIRTIO_BLK_MAX_QUEUES];

/* Feature negotiation results */
static int                    has_flush;       /* F_FLUSH negotiated */
static int                    has_config_wce;  /* F_CONFIG_WCE negotiated */
static int                    has_blk_size;    /* F_BLK_SIZE negotiated */
static int                    has_topology;    /* F_TOPOLOGY negotiated */
static int                    has_size_max;    /* F_SIZE_MAX negotiated */
static int                    has_seg_max;     /* F_SEG_MAX negotiated */
static int                    has_discard;     /* F_DISCARD negotiated */
static int                    has_write_zeroes; /* F_WRITE_ZEROES negotiated */
static int                    has_lifetime;    /* F_LIFETIME negotiated */
static int                    has_ring_reset;  /* F_RING_RESET negotiated */
static int                    has_in_order;    /* F_IN_ORDER negotiated */
static int                    has_notify_data; /* F_NOTIFICATION_DATA negotiated */
static int                    has_mq;          /* F_MQ negotiated */
static int                    has_indirect;    /* F_RING_INDIRECT_DESC negotiated */
static int                    has_event_idx;   /* F_RING_EVENT_IDX negotiated */
static int                    is_read_only;    /* F_RO detected */

/* Block size and topology info */
static struct virtio_blk_topology topo;

/* Device serial number (GET_ID result) */
static char device_serial[VIRTIO_BLK_ID_BYTES + 1]; /* +1 for null terminator */

/* Error statistics */
static struct {
    uint32_t io_errors;      /* S_IOERR responses received */
    uint32_t unsupp_errors;  /* S_UNSUPP responses received */
    uint32_t timeouts;       /* I/O completions that timed out */
    uint32_t resets;         /* Full device resets performed */
    uint32_t config_changes; /* Config change interrupts handled */
} error_stats;

/* Internal return codes for do_io / do_flush */
#define VIRTIO_IO_OK        0
#define VIRTIO_IO_TIMEOUT  (-1)
#define VIRTIO_IO_IOERR    (-2)   /* S_IOERR — retryable */
#define VIRTIO_IO_UNSUPP   (-3)   /* S_UNSUPP — not retryable */

/* Max retries for transient I/O errors */
#define VIRTIO_BLK_MAX_RETRIES  3

/* Per-queue I/O statistics (used by both adaptive polling and priority queues) */
static struct {
    uint32_t io_completed;     /* Total I/Os completed on this queue */
    uint32_t io_count_window;  /* I/Os in current 100ms window */
    uint32_t last_iops;        /* IOPS from last completed window */
} queue_stats[VIRTIO_BLK_MAX_QUEUES];

/* ---- TSC (Time Stamp Counter) Infrastructure ---- */

/* Read TSC for sub-microsecond timing */
static inline uint64_t rdtsc_read(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Calibrate: approximate TSC ticks per microsecond.
 * Called once during init using port 0x80 delay (~1 µs each). */
static uint64_t tsc_per_us = 2000;  /* Conservative default (2 GHz) */

static void calibrate_tsc(void)
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

/* ---- I/O Latency Telemetry Engine (§16.1 — 🚀 Impossible OS Exclusive) ---- */

/* Logarithmic histogram bucket boundaries (in nanoseconds) */
#define LAT_BUCKET_COUNT  7
#define LAT_1US     1000ULL
#define LAT_10US    10000ULL
#define LAT_100US   100000ULL
#define LAT_1MS     1000000ULL
#define LAT_10MS    10000000ULL
#define LAT_100MS   100000000ULL

/* Bucket labels for Registry */
static const char *lat_bucket_names[LAT_BUCKET_COUNT] = {
    "Lt1us", "1_10us", "10_100us", "100us_1ms",
    "1_10ms", "10_100ms", "Gt100ms"
};

/* I/O type indices */
#define LAT_TYPE_READ    0
#define LAT_TYPE_WRITE   1
#define LAT_TYPE_FLUSH   2
#define LAT_TYPE_DISCARD 3
#define LAT_TYPE_COUNT   4

static const char *lat_type_names[LAT_TYPE_COUNT] = {
    "Read", "Write", "Flush", "Discard"
};

/* Per-type latency histogram and running stats */
struct latency_hist {
    uint32_t buckets[LAT_BUCKET_COUNT];   /* Histogram bins */
    uint64_t total_ns;        /* Sum of all latencies (for average) */
    uint32_t total_requests;  /* Number of recorded requests */
    uint64_t min_ns;          /* Minimum observed latency */
    uint64_t max_ns;          /* Maximum observed latency */
};

static struct latency_hist lat_hists[LAT_TYPE_COUNT];
static int latency_tracking_enabled = 1;  /* Master enable */

/* Compute ns_per_tick from tsc_per_us (set by calibrate_tsc).
 * ns_per_tick = 1000 / tsc_per_us. We store the reciprocal for fast
 * multiplication: latency_ns = delta_tsc * 1000 / tsc_per_us. */

/* Record a latency measurement.
 * submit_tsc: rdtsc value at submission time
 * type: LAT_TYPE_READ, LAT_TYPE_WRITE, etc. */
static void latency_record(uint64_t submit_tsc, int type)
{
    uint64_t delta_tsc, latency_ns;
    struct latency_hist *h;
    int bucket;

    if (!latency_tracking_enabled || type >= LAT_TYPE_COUNT)
        return;

    delta_tsc = rdtsc_read() - submit_tsc;

    /* Convert TSC ticks to nanoseconds: ns = ticks * 1000 / tsc_per_us */
    if (tsc_per_us > 0)
        latency_ns = delta_tsc * 1000 / tsc_per_us;
    else
        latency_ns = delta_tsc;  /* Fallback: raw ticks */

    h = &lat_hists[type];

    /* Bucket into logarithmic histogram */
    if (latency_ns < LAT_1US)
        bucket = 0;
    else if (latency_ns < LAT_10US)
        bucket = 1;
    else if (latency_ns < LAT_100US)
        bucket = 2;
    else if (latency_ns < LAT_1MS)
        bucket = 3;
    else if (latency_ns < LAT_10MS)
        bucket = 4;
    else if (latency_ns < LAT_100MS)
        bucket = 5;
    else
        bucket = 6;

    h->buckets[bucket]++;
    h->total_ns += latency_ns;
    h->total_requests++;

    if (latency_ns < h->min_ns || h->min_ns == 0)
        h->min_ns = latency_ns;
    if (latency_ns > h->max_ns)
        h->max_ns = latency_ns;
}

/* Compute approximate percentile from histogram (in nanoseconds).
 * Uses linear interpolation within the bucket. */
static uint64_t latency_percentile(const struct latency_hist *h,
                                    uint32_t percentile_x10)
{
    /* Bucket upper bounds in ns */
    static const uint64_t bounds[LAT_BUCKET_COUNT] = {
        LAT_1US, LAT_10US, LAT_100US, LAT_1MS,
        LAT_10MS, LAT_100MS, LAT_100MS * 10
    };
    uint32_t target, cumulative;
    int i;

    if (h->total_requests == 0)
        return 0;

    /* target = the rank we're looking for */
    target = (uint32_t)((uint64_t)h->total_requests * percentile_x10 / 1000);
    if (target == 0)
        target = 1;

    cumulative = 0;
    for (i = 0; i < LAT_BUCKET_COUNT; i++) {
        cumulative += h->buckets[i];
        if (cumulative >= target) {
            /* Return the upper bound of this bucket as approximation */
            return bounds[i];
        }
    }
    return bounds[LAT_BUCKET_COUNT - 1];
}

/* Public API: retrieve latency statistics for a given I/O type.
 * Returns stats in microseconds. Called by Disk Manager for dashboards. */
void virtio_blk_get_latency_stats(int type, uint32_t *avg_us,
    uint32_t *p50_us, uint32_t *p99_us, uint32_t *p999_us,
    uint32_t *min_us, uint32_t *max_us, uint32_t *total)
{
    const struct latency_hist *h;
    if (type < 0 || type >= LAT_TYPE_COUNT) {
        if (avg_us) *avg_us = 0;
        if (p50_us) *p50_us = 0;
        if (p99_us) *p99_us = 0;
        if (p999_us) *p999_us = 0;
        if (min_us) *min_us = 0;
        if (max_us) *max_us = 0;
        if (total) *total = 0;
        return;
    }
    h = &lat_hists[type];

    if (avg_us) {
        *avg_us = h->total_requests > 0
            ? (uint32_t)(h->total_ns / h->total_requests / 1000)
            : 0;
    }
    if (p50_us)  *p50_us  = (uint32_t)(latency_percentile(h, 500) / 1000);
    if (p99_us)  *p99_us  = (uint32_t)(latency_percentile(h, 990) / 1000);
    if (p999_us) *p999_us = (uint32_t)(latency_percentile(h, 999) / 1000);
    if (min_us)  *min_us  = (uint32_t)(h->min_ns / 1000);
    if (max_us)  *max_us  = (uint32_t)(h->max_ns / 1000);
    if (total)   *total   = h->total_requests;
}

/* ---- Adaptive Hybrid Polling Engine ---- */

/* I/O completion modes */
#define VIRTIO_IO_MODE_INTERRUPT  0  /* Pure ISR-driven */
#define VIRTIO_IO_MODE_HYBRID     1  /* Brief spin, fallback to ISR */
#define VIRTIO_IO_MODE_POLL       2  /* Pure polling, interrupts off */

static const char *io_mode_names[] = {
    "interrupt", "hybrid", "poll"
};

/* Adaptive polling configuration and state */
static struct {
    int      enabled;           /* Master enable (default 1) */
    int      mode;              /* Current VIRTIO_IO_MODE_* */
    uint32_t low_threshold;     /* IOPS below this → INTERRUPT (default 1000) */
    uint32_t high_threshold;    /* IOPS above this → POLL (default 50000) */
    uint32_t spin_us;           /* Hybrid spin window in µs (default 4) */

    /* Rolling IOPS tracking (100ms window) */
    uint32_t io_count;          /* I/Os completed in current window */
    uint64_t window_start;      /* system_get_ticks() at window start */
    uint32_t window_ticks;      /* Ticks per 100ms window */
    uint32_t last_iops;         /* IOPS from the last completed window */

    /* Hysteresis counters (prevent mode thrashing) */
    uint32_t up_count;          /* Consecutive windows above threshold */
    uint32_t down_count;        /* Consecutive windows below threshold */
} adaptive = {
    .enabled = 1,
    .mode = VIRTIO_IO_MODE_INTERRUPT,
    .low_threshold = 1000,
    .high_threshold = 50000,
    .spin_us = 4,
};

/* Check if 100ms window has elapsed and transition modes if needed */
static void adaptive_check_window(void)
{
    uint64_t now;
    uint32_t elapsed_iops;

    if (!adaptive.enabled || !adaptive.window_ticks)
        return;

    now = system_get_ticks();
    if ((now - adaptive.window_start) < adaptive.window_ticks)
        return;  /* Window not yet elapsed */

    /* Window complete — compute IOPS */
    elapsed_iops = adaptive.io_count * 10;  /* 100ms → multiply by 10 for per-second */
    adaptive.last_iops = elapsed_iops;
    adaptive.io_count = 0;
    adaptive.window_start = now;

    /* Per-queue IOPS */
    {
        uint16_t q;
        for (q = 0; q < num_queues; q++) {
            queue_stats[q].last_iops = queue_stats[q].io_count_window * 10;
            queue_stats[q].io_count_window = 0;
        }
    }

    /* Mode transition logic with hysteresis */
    switch (adaptive.mode) {
    case VIRTIO_IO_MODE_INTERRUPT:
        if (elapsed_iops > adaptive.low_threshold) {
            adaptive.up_count++;
            adaptive.down_count = 0;
            if (adaptive.up_count >= 3) {
                adaptive.mode = VIRTIO_IO_MODE_HYBRID;
                adaptive.up_count = 0;
                klog(LOG_DEBUG, "virtio",
                       "Block: I/O mode -> HYBRID (IOPS=%u)",
                       (uint64_t)elapsed_iops);
            }
        } else {
            adaptive.up_count = 0;
        }
        break;

    case VIRTIO_IO_MODE_HYBRID:
        if (elapsed_iops > adaptive.high_threshold) {
            adaptive.up_count++;
            adaptive.down_count = 0;
            if (adaptive.up_count >= 3) {
                adaptive.mode = VIRTIO_IO_MODE_POLL;
                adaptive.up_count = 0;
                klog(LOG_DEBUG, "virtio",
                       "Block: I/O mode -> POLL (IOPS=%u)",
                       (uint64_t)elapsed_iops);
            }
        } else if (elapsed_iops < (adaptive.low_threshold * 4 / 5)) {
            adaptive.down_count++;
            adaptive.up_count = 0;
            if (adaptive.down_count >= 5) {
                adaptive.mode = VIRTIO_IO_MODE_INTERRUPT;
                adaptive.down_count = 0;
                klog(LOG_DEBUG, "virtio",
                       "Block: I/O mode -> INTERRUPT (IOPS=%u)",
                       (uint64_t)elapsed_iops);
            }
        } else {
            adaptive.up_count = 0;
            adaptive.down_count = 0;
        }
        break;

    case VIRTIO_IO_MODE_POLL:
        if (elapsed_iops < (adaptive.high_threshold * 4 / 5)) {
            adaptive.down_count++;
            adaptive.up_count = 0;
            if (adaptive.down_count >= 5) {
                adaptive.mode = VIRTIO_IO_MODE_HYBRID;
                adaptive.down_count = 0;
                klog(LOG_DEBUG, "virtio",
                       "Block: I/O mode -> HYBRID (IOPS=%u)",
                       (uint64_t)elapsed_iops);
            }
        } else {
            adaptive.down_count = 0;
        }
        break;
    }
}

/* Brief rdtsc-based spin checking used->idx.
 * Returns 1 if completion arrived within spin window, 0 otherwise. */
static int hybrid_spin_poll(struct virtqueue *vq, uint32_t spin_us)
{
    uint64_t deadline = rdtsc_read() + (uint64_t)spin_us * tsc_per_us;

    while (rdtsc_read() < deadline) {
        mb();
        if (vq->used->idx != vq->last_used)
            return 1;  /* Completed during spin */
        __asm__ volatile ("pause");  /* Reduce power + SMT contention */
    }
    return 0;  /* Spin window expired — fall back to ISR */
}

/* ---- I/O Priority Queue Mapping (§13.1 — 🚀 Impossible OS Exclusive) ---- */

/* Win32-compatible I/O priority levels */
#define IO_PRIO_VERY_LOW   0  /* Background: indexing, prefetch */
#define IO_PRIO_LOW        1  /* Background: defrag, cleanup */
#define IO_PRIO_NORMAL     2  /* Standard user I/O */
#define IO_PRIO_HIGH       3  /* System, page faults */
#define IO_PRIO_CRITICAL   4  /* Real-time, paging supervisor */

/* Priority tiers map to queue classes */
#define IO_QUEUE_HIGH      0  /* Critical + High priority */
#define IO_QUEUE_NORMAL    1  /* Normal priority */
#define IO_QUEUE_LOW       2  /* Low + VeryLow (background) */
#define IO_QUEUE_TIERS     3  /* Number of priority tiers */

static int priority_queues_active;  /* 1 if num_queues >= 3 and F_MQ */

/* Map thread scheduling priority to I/O priority level.
 * Thread priority (0-31) maps to Win32-style I/O priority. */
static inline int thread_prio_to_io_prio(uint32_t thread_priority)
{
    if (thread_priority >= THREAD_PRIO_REALTIME)
        return IO_PRIO_CRITICAL;
    if (thread_priority >= THREAD_PRIO_HIGH)
        return IO_PRIO_HIGH;
    if (thread_priority >= THREAD_PRIO_NORMAL)
        return IO_PRIO_NORMAL;
    if (thread_priority >= THREAD_PRIO_LOW)
        return IO_PRIO_LOW;
    return IO_PRIO_VERY_LOW;
}

/* Map I/O priority level to target queue index.
 * When priority queues are active (num_queues >= 3):
 *   Queue 0 = Critical/High, Queue 1 = Normal, Queue 2 = Low/VeryLow
 * When not active: falls through to CPU-based selection. */
static inline uint16_t io_prio_to_queue(int io_prio)
{
    switch (io_prio) {
    case IO_PRIO_CRITICAL:
    case IO_PRIO_HIGH:
        return IO_QUEUE_HIGH;    /* Queue 0 */
    case IO_PRIO_NORMAL:
        return IO_QUEUE_NORMAL;  /* Queue 1 */
    case IO_PRIO_LOW:
    case IO_PRIO_VERY_LOW:
    default:
        return IO_QUEUE_LOW;     /* Queue 2 */
    }
}

/* ---- Helper: get queue index for current I/O ---- */
static inline uint16_t get_queue_idx(void)
{
    if (num_queues <= 1)
        return 0;

    /* Priority-based queue selection when 3+ queues available */
    if (priority_queues_active) {
        struct thread *thr = thread_current();
        if (thr) {
            int io_prio = thread_prio_to_io_prio(thr->priority);
            return io_prio_to_queue(io_prio);
        }
        return IO_QUEUE_NORMAL;  /* Default: normal if no thread context */
    }

    /* Fallback: round-robin across CPUs */
    return (uint16_t)(smp_cpu_id() % num_queues);
}

/* ---- MSI-X IRQ handlers ---- */

/* Queue completion interrupt — device placed buffers in the used ring.
 * The ctx pointer carries the queue index. */
static void virtio_blk_queue_irq(uint8_t vector, void *ctx)
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
            if (use_events)
                event_set(&io_completions[IO_QUEUE_HIGH]);
        }
    }

    if (qi >= 0 && qi < (int)num_queues) {
        virtio_irq_flags[qi] = 1;
        if (use_events)
            event_set(&io_completions[qi]);
    }
}

/* Config change interrupt — device resized, topology changed, etc.
 * VirtIO 1.2 §4.1.4.5: ISR bit 1 indicates device config has changed. */
static void virtio_blk_config_irq(uint8_t vector, void *ctx)
{
    (void)vector;
    (void)ctx;

    /* Read ISR to acknowledge interrupt (read clears pending bits) */
    uint8_t isr = 0;
    if (blk_dev.isr_cfg) {
        isr = virtio_read_isr(&blk_dev);
    }

    /* Check DEVICE_NEEDS_RESET (status bit 6) — fatal condition */
    uint8_t st = virtio_get_status(&blk_dev);
    if (st & VIRTIO_STATUS_DEVICE_NEEDS_RESET) {
        klog(LOG_DEBUG, "virtio", "Config ISR: DEVICE_NEEDS_RESET — reset required");
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
 * Called from config change ISR — keep fast and non-blocking. */
void virtio_blk_handle_config_change(void)
{
    if (!initialized || !blk_dev.device_cfg)
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
            /* Disk grew — safe, notify block device layer */
            klog(LOG_DEBUG, "virtio",
                   "Config change: capacity increased %u -> %u sectors (hot-resize)",
                   old_capacity, new_capacity);
            blkdev_update_capacity("virtio0", new_capacity);
        } else {
            /* Disk shrunk — dangerous! Data beyond new boundary is lost.
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
        /* We don't cache writeback state currently — just log the change */
        klog(LOG_DEBUG, "virtio", "Config change: writeback=%u", (uint64_t)new_wb);
    }
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
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint64_t submit_tsc = rdtsc_read();  /* Latency telemetry: stamp submission */
    int lat_type = (type == VIRTIO_BLK_T_IN) ? LAT_TYPE_READ : LAT_TYPE_WRITE;

    if (!initialized)
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

        /* Wait for completion — adaptive strategy */
        __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
        virtio_irq_flags[qi] = 0;
        __asm__ volatile ("sti");

        virtq_kick(&blk_vqs[qi]);

        {
            int current_mode = (use_events && adaptive.enabled)
                               ? adaptive.mode
                               : VIRTIO_IO_MODE_INTERRUPT;

            if (!use_events) {
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
        int current_mode = (use_events && adaptive.enabled)
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

        /* Wait for completion — mode-dependent strategy */
        if (!use_events) {
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
                /* Spin didn't catch it — fall back to ISR wait */
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

/* ---- Flush I/O (2-descriptor chain: header + status, no data) ---- */
static int virtio_blk_do_flush(void)
{
    struct virtio_blk_req req;
    uint8_t status_byte = 0xFF;
    int d0, d1;
    uint32_t timeout;
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint64_t submit_tsc = rdtsc_read();  /* Latency telemetry */

    if (!initialized)
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

        if (use_events) {
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
    if (use_events) {
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

/* ---- Public API ---- */

int virtio_blk_read(uint64_t lba, uint32_t count, void *buffer)
{
    int ret;
    int retry;

    for (retry = 0; retry <= VIRTIO_BLK_MAX_RETRIES; retry++) {
        ret = virtio_blk_do_io(VIRTIO_BLK_T_IN, lba, count * topo.blk_size, buffer);
        if (ret == VIRTIO_IO_OK)
            return 0;
        if (ret == VIRTIO_IO_UNSUPP) {
            klog(LOG_DEBUG, "virtio", "Read: unsupported (lba=%u)",
                   (uint64_t)lba);
            return -1;
        }
        if (ret == VIRTIO_IO_TIMEOUT) {
            error_stats.timeouts++;
            klog(LOG_DEBUG, "virtio", "Read: timeout, triggering reset");
            virtio_blk_reset();
            return -1;
        }
        /* IOERR — retry */
        if (retry < VIRTIO_BLK_MAX_RETRIES) {
            klog(LOG_DEBUG, "virtio", "Read: I/O error, retry %u/%u (lba=%u)",
                   (uint64_t)(retry + 1), (uint64_t)VIRTIO_BLK_MAX_RETRIES,
                   (uint64_t)lba);
        }
    }
    klog(LOG_DEBUG, "virtio", "Read: failed after %u retries (lba=%u)",
           (uint64_t)VIRTIO_BLK_MAX_RETRIES, (uint64_t)lba);
    return -1;
}

int virtio_blk_write(uint64_t lba, uint32_t count, const void *buffer)
{
    int ret;
    int retry;

    if (is_read_only)
        return -1;  /* Device is read-only */

    for (retry = 0; retry <= VIRTIO_BLK_MAX_RETRIES; retry++) {
        ret = virtio_blk_do_io(VIRTIO_BLK_T_OUT, lba, count * topo.blk_size,
                               (void *)buffer);
        if (ret == VIRTIO_IO_OK)
            return 0;
        if (ret == VIRTIO_IO_UNSUPP) {
            klog(LOG_DEBUG, "virtio", "Write: unsupported (lba=%u)",
                   (uint64_t)lba);
            return -1;
        }
        if (ret == VIRTIO_IO_TIMEOUT) {
            error_stats.timeouts++;
            klog(LOG_DEBUG, "virtio", "Write: timeout, triggering reset");
            virtio_blk_reset();
            return -1;
        }
        if (retry < VIRTIO_BLK_MAX_RETRIES) {
            klog(LOG_DEBUG, "virtio", "Write: I/O error, retry %u/%u (lba=%u)",
                   (uint64_t)(retry + 1), (uint64_t)VIRTIO_BLK_MAX_RETRIES,
                   (uint64_t)lba);
        }
    }
    klog(LOG_DEBUG, "virtio", "Write: failed after %u retries (lba=%u)",
           (uint64_t)VIRTIO_BLK_MAX_RETRIES, (uint64_t)lba);
    return -1;
}

int virtio_blk_set_write_cache(int enable)
{
    if (!initialized || !has_config_wce)
        return -1;

    if (!blk_dev.device_cfg)
        return -1;

    /* Write the writeback field at device config offset 0x20 */
    volatile uint8_t *wb = (volatile uint8_t *)
        (blk_dev.device_cfg + VIRTIO_BLK_CFG_WRITEBACK);
    mmio_write8(wb, enable ? 1 : 0);

    klog(LOG_DEBUG, "virtio", "Write cache mode: %s",
           enable ? "writeback" : "writethrough");

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

uint16_t virtio_blk_num_queues(void)
{
    return num_queues;
}

uint32_t virtio_blk_block_size(void)
{
    return topo.blk_size;
}

const struct virtio_blk_topology *virtio_blk_topology(void)
{
    return &topo;
}

int virtio_blk_get_id(char *buf, uint32_t len)
{
    char tmp[VIRTIO_BLK_ID_BYTES];
    uint32_t i;
    int ret;

    if (!initialized || !buf || len == 0)
        return -1;

    /* Zero the temp buffer — device may not write all 20 bytes */
    for (i = 0; i < VIRTIO_BLK_ID_BYTES; i++)
        tmp[i] = 0;

    /* GET_ID: 3-descriptor chain — header (type=8, sector=0) +
     * 20-byte device-writable buffer + status byte.
     * do_io sets F_WRITE on data descriptor for all non-OUT types. */
    ret = virtio_blk_do_io(VIRTIO_BLK_T_GET_ID, 0, VIRTIO_BLK_ID_BYTES, tmp);
    if (ret != 0)
        return -1;

    /* Copy and null-terminate */
    for (i = 0; i < len - 1 && i < VIRTIO_BLK_ID_BYTES; i++)
        buf[i] = tmp[i];
    buf[i] = '\0';

    return 0;
}

int virtio_blk_get_lifetime(struct virtio_blk_lifetime *out)
{
    uint8_t tmp[6];
    uint32_t i;
    int ret;

    if (!initialized || !out)
        return -1;

    if (!has_lifetime)
        return 1;  /* Not supported */

    /* Zero the temp buffer */
    for (i = 0; i < sizeof(tmp); i++)
        tmp[i] = 0;

    /* GET_LIFETIME: 3-descriptor chain — header (type=10, sector=0) +
     * 6-byte device-writable buffer + status byte.
     * do_io sets F_WRITE on data descriptor for all non-OUT types. */
    ret = virtio_blk_do_io(VIRTIO_BLK_T_GET_LIFETIME, 0, sizeof(tmp), tmp);
    if (ret != 0)
        return -1;

    /* Parse little-endian fields */
    out->pre_eol_info = (uint16_t)tmp[0] | ((uint16_t)tmp[1] << 8);
    out->device_lifetime_est_typ_a = (uint16_t)tmp[2] | ((uint16_t)tmp[3] << 8);
    out->device_lifetime_est_typ_b = (uint16_t)tmp[4] | ((uint16_t)tmp[5] << 8);

    return 0;
}

const char *virtio_blk_serial(void)
{
    return device_serial;
}

/* ---- Initialization ---- */

/* Forward declaration — virtio_blk_init is also called by reset */
int virtio_blk_init(void);

/* Reset device and re-initialize.
 * Called on DEVICE_NEEDS_RESET or unrecoverable I/O timeout. */
int virtio_blk_reset(void)
{
    uint32_t wait;
    uint16_t qi = 0;  /* Reset always targets queue 0 */

    if (!initialized)
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
    initialized = 0;
    use_events = 0;
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

    if (!initialized)
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

/* ---- Discard (TRIM) ---- */

/* Submit a single discard request for one segment.
 * Uses 3-descriptor chain: header (T_DISCARD) + segment (device-readable) + status. */
static int virtio_blk_do_discard(uint64_t sector, uint32_t num_sectors)
{
    struct virtio_blk_req req;
    struct virtio_blk_discard_write_zeroes seg;
    uint8_t status_byte = 0xFF;
    int d0, d1, d2;
    uint32_t timeout;
    uint64_t rflags;
    uint16_t qi = get_queue_idx();
    uint64_t submit_tsc = rdtsc_read();  /* Latency telemetry */

    if (!initialized)
        return -1;

    /* Build request header — sector field is ignored for discard */
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

        if (use_events) {
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

    /* Descriptor 1: discard segment data (device-readable — NOT F_WRITE!) */
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
    if (use_events) {
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

/* Public discard API — splits large requests to fit max_discard_sectors */
int virtio_blk_discard(uint64_t sector, uint32_t num_sectors)
{
    int ret;
    uint32_t max_per_cmd;
    uint32_t chunk;

    if (!initialized)
        return -1;
    if (!has_discard)
        return 1;  /* Not supported — same convention as flush */
    if (is_read_only)
        return 0;  /* RO device — discard is a no-op */

    /* Determine max sectors per discard command */
    max_per_cmd = topo.max_discard_sectors;
    if (max_per_cmd == 0)
        max_per_cmd = num_sectors;  /* No limit — send all at once */

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
static int virtio_blk_do_write_zeroes(uint64_t sector, uint32_t num_sectors,
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

    if (!initialized)
        return -1;

    /* Build request header — sector field is ignored for write-zeroes */
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

        if (use_events) {
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

    /* Descriptor 1: write-zeroes segment data (device-readable — NOT F_WRITE!) */
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
    if (use_events) {
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

/* Public write-zeroes API — splits large requests to fit max_wz_sectors */
int virtio_blk_write_zeroes(uint64_t sector, uint32_t num_sectors, int unmap)
{
    int ret;
    uint32_t max_per_cmd;
    uint32_t chunk;
    uint32_t flags;

    if (!initialized)
        return -1;
    if (!has_write_zeroes)
        return 1;  /* Not supported */
    if (is_read_only)
        return 0;  /* RO device — write-zeroes is a no-op */

    /* Only set unmap flag if device allows it */
    flags = (unmap && topo.wz_may_unmap) ? 1u : 0u;

    /* Determine max sectors per write-zeroes command */
    max_per_cmd = topo.max_wz_sectors;
    if (max_per_cmd == 0)
        max_per_cmd = num_sectors;  /* No limit — send all at once */

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

    initialized = 1;

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
                use_events = 1;
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
            use_events = 1;
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
            use_events = 1;
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

    return 0;
}

/* ---- Hot-Plug / Hot-Unplug ---- */

int virtio_blk_is_surprise_removed(void)
{
    uint16_t vid;

    if (!initialized)
        return 1;  /* Not initialized = effectively gone */

    /* Read PCI vendor ID — if device is surprise-removed, returns 0xFFFF */
    vid = pci_read16(saved_pci_bus, saved_pci_dev, saved_pci_func, 0x00);
    return (vid == 0xFFFF) ? 1 : 0;
}

void virtio_blk_shutdown(void)
{
    int surprise;
    int i;

    if (!initialized)
        return;

    surprise = virtio_blk_is_surprise_removed();

    klog(LOG_DEBUG, "virtio", "Shutdown: %s",
           surprise ? "surprise removal" : "managed removal");

    /* 1. Stop accepting new I/O */
    initialized = 0;
    use_events = 0;

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

    /* If already initialized, shut down first */
    if (initialized)
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
