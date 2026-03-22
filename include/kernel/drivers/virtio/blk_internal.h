/* ============================================================================
 * blk_internal.h — VirtIO Block Driver Internal Shared State
 *
 * Shared between blk_*.c files. NOT part of the public API.
 * One file (blk_core.c) defines BLK_DEFINE_GLOBALS to instantiate storage.
 * ============================================================================ */

#pragma once

#include "kernel/drivers/virtio/blk.h"
#include "kernel/drivers/virtio/virtio.h"
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

#ifdef BLK_DEFINE_GLOBALS
#define BLK_GLOBAL
#define BLK_GLOBAL_INIT(val)  = val
#else
#define BLK_GLOBAL             extern
#define BLK_GLOBAL_INIT(val)
#endif

/* ---- Driver state ---- */
BLK_GLOBAL struct virtio_pci_dev  blk_dev;
BLK_GLOBAL struct virtqueue       blk_vqs[VIRTIO_BLK_MAX_QUEUES];
BLK_GLOBAL uint64_t               disk_capacity;
BLK_GLOBAL int                    blk_initialized;
BLK_GLOBAL volatile int           virtio_irq_flags[VIRTIO_BLK_MAX_QUEUES];

BLK_GLOBAL uint8_t                saved_pci_bus;
BLK_GLOBAL uint8_t                saved_pci_dev;
BLK_GLOBAL uint8_t                saved_pci_func;
BLK_GLOBAL uint8_t                msix_vec_queues[VIRTIO_BLK_MAX_QUEUES];
BLK_GLOBAL uint8_t                msix_vec_config;
BLK_GLOBAL int                    blk_use_events;
BLK_GLOBAL uint16_t               num_queues BLK_GLOBAL_INIT(1);
BLK_GLOBAL event_t                io_completions[VIRTIO_BLK_MAX_QUEUES];

/* Feature flags */
BLK_GLOBAL int has_flush;
BLK_GLOBAL int has_config_wce;
BLK_GLOBAL int has_blk_size;
BLK_GLOBAL int has_topology;
BLK_GLOBAL int has_size_max;
BLK_GLOBAL int has_seg_max;
BLK_GLOBAL int has_discard;
BLK_GLOBAL int has_write_zeroes;
BLK_GLOBAL int has_lifetime;
BLK_GLOBAL int has_ring_reset;
BLK_GLOBAL int has_in_order;
BLK_GLOBAL int has_notify_data;
BLK_GLOBAL int has_mq;
BLK_GLOBAL int has_indirect;
BLK_GLOBAL int has_event_idx;
BLK_GLOBAL int is_read_only;

BLK_GLOBAL struct virtio_blk_topology topo;
BLK_GLOBAL char device_serial[VIRTIO_BLK_ID_BYTES + 1];

/* Error statistics */
struct blk_error_stats {
    uint32_t io_errors;
    uint32_t unsupp_errors;
    uint32_t timeouts;
    uint32_t resets;
    uint32_t config_changes;
};
BLK_GLOBAL struct blk_error_stats error_stats;

/* Internal return codes */
#define VIRTIO_IO_OK        0
#define VIRTIO_IO_TIMEOUT  (-1)
#define VIRTIO_IO_IOERR    (-2)
#define VIRTIO_IO_UNSUPP   (-3)
#define VIRTIO_BLK_MAX_RETRIES  3

/* Per-queue stats */
struct blk_queue_stats {
    uint32_t io_completed;
    uint32_t io_count_window;
    uint32_t last_iops;
};
BLK_GLOBAL struct blk_queue_stats queue_stats[VIRTIO_BLK_MAX_QUEUES];

/* ---- TSC Infrastructure ---- */
BLK_GLOBAL uint64_t tsc_per_us BLK_GLOBAL_INIT(2000);

static inline uint64_t rdtsc_read(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void calibrate_tsc(void);

/* ---- Latency Telemetry ---- */
#define LAT_BUCKET_COUNT  7
#define LAT_1US     1000ULL
#define LAT_10US    10000ULL
#define LAT_100US   100000ULL
#define LAT_1MS     1000000ULL
#define LAT_10MS    10000000ULL
#define LAT_100MS   100000000ULL

#define LAT_TYPE_READ    0
#define LAT_TYPE_WRITE   1
#define LAT_TYPE_FLUSH   2
#define LAT_TYPE_DISCARD 3
#define LAT_TYPE_COUNT   4

struct latency_hist {
    uint32_t buckets[LAT_BUCKET_COUNT];
    uint64_t total_ns;
    uint32_t total_requests;
    uint64_t min_ns;
    uint64_t max_ns;
};

BLK_GLOBAL struct latency_hist lat_hists[LAT_TYPE_COUNT];
BLK_GLOBAL int latency_tracking_enabled BLK_GLOBAL_INIT(1);

void latency_record(uint64_t submit_tsc, int type);
uint64_t latency_percentile(const struct latency_hist *h, uint32_t percentile_x10);

extern const char *lat_bucket_names[LAT_BUCKET_COUNT];
extern const char *lat_type_names[LAT_TYPE_COUNT];

/* ---- Adaptive Hybrid Polling ---- */
#define VIRTIO_IO_MODE_INTERRUPT  0
#define VIRTIO_IO_MODE_HYBRID     1
#define VIRTIO_IO_MODE_POLL       2

struct adaptive_state {
    int      enabled;
    int      mode;
    uint32_t low_threshold;
    uint32_t high_threshold;
    uint32_t spin_us;
    uint32_t io_count;
    uint64_t window_start;
    uint32_t window_ticks;
    uint32_t last_iops;
    uint32_t up_count;
    uint32_t down_count;
};

#ifdef BLK_DEFINE_GLOBALS
struct adaptive_state adaptive = {
    .enabled = 1,
    .mode = VIRTIO_IO_MODE_INTERRUPT,
    .low_threshold = 1000,
    .high_threshold = 50000,
    .spin_us = 4,
};
#else
extern struct adaptive_state adaptive;
#endif

extern const char *io_mode_names[];

void adaptive_check_window(void);
int  hybrid_spin_poll(struct virtqueue *vq, uint32_t spin_us);

/* ---- I/O Priority ---- */
#define IO_PRIO_VERY_LOW   0
#define IO_PRIO_LOW        1
#define IO_PRIO_NORMAL     2
#define IO_PRIO_HIGH       3
#define IO_PRIO_CRITICAL   4

#define IO_QUEUE_HIGH      0
#define IO_QUEUE_NORMAL    1
#define IO_QUEUE_LOW       2
#define IO_QUEUE_TIERS     3

BLK_GLOBAL int priority_queues_active;

static inline int thread_prio_to_io_prio(uint32_t thread_priority)
{
    if (thread_priority >= THREAD_PRIO_REALTIME)  return IO_PRIO_CRITICAL;
    if (thread_priority >= THREAD_PRIO_HIGH)      return IO_PRIO_HIGH;
    if (thread_priority >= THREAD_PRIO_NORMAL)    return IO_PRIO_NORMAL;
    if (thread_priority >= THREAD_PRIO_LOW)       return IO_PRIO_LOW;
    return IO_PRIO_VERY_LOW;
}

static inline uint16_t io_prio_to_queue(int io_prio)
{
    switch (io_prio) {
    case IO_PRIO_CRITICAL: case IO_PRIO_HIGH: return IO_QUEUE_HIGH;
    case IO_PRIO_NORMAL:                       return IO_QUEUE_NORMAL;
    default:                                   return IO_QUEUE_LOW;
    }
}

static inline uint16_t get_queue_idx(void)
{
    if (num_queues <= 1) return 0;
    if (priority_queues_active) {
        struct thread *thr = thread_current();
        if (thr) return io_prio_to_queue(thread_prio_to_io_prio(thr->priority));
        return IO_QUEUE_NORMAL;
    }
    return (uint16_t)(smp_cpu_id() % num_queues);
}

/* ---- Cross-file function declarations ---- */
int  virtio_blk_do_io(uint32_t type, uint64_t sector, uint32_t len, void *buffer);
int  virtio_blk_do_flush(void);
int  virtio_blk_do_discard(uint64_t sector, uint32_t num_sectors);
int  virtio_blk_do_write_zeroes(uint64_t sector, uint32_t num_sectors, uint32_t flags);

uint32_t read_device_features(uint32_t page);
void     write_driver_features(uint32_t page, uint32_t features);
void     virtio_blk_queue_irq(uint8_t vector, void *ctx);
void     virtio_blk_config_irq(uint8_t vector, void *ctx);

/* ---- Prefetch (defined in blk_prefetch.c) ---- */
void prefetch_init(void);
void prefetch_shutdown(void);
int  prefetch_try_read(uint64_t sector, uint32_t count, void *buffer);
void prefetch_after_read(uint64_t sector, uint32_t count);
void prefetch_invalidate(uint64_t sector, uint32_t count);
void prefetch_expose_registry(void);

/* ---- Merge (defined in blk_merge.c) ---- */
void merge_init(void);
void merge_reset(void);
int  merge_try_coalesce(uint32_t type, uint64_t sector, uint32_t count,
                        void *buffer, uint64_t *out_sector, uint32_t *out_count);
int  merge_execute_read(int merge_dir, uint64_t original_sector,
                        uint32_t original_count, void *original_buffer,
                        uint64_t merged_sector, uint32_t merged_count);
void merge_expose_registry(void);

/* ---- Stripe (defined in blk_stripe.c) ---- */
void stripe_init(void);
int  stripe_is_active(void);
void stripe_expose_registry(void);
