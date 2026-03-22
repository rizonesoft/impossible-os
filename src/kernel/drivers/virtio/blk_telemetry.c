/* blk_telemetry.c — Latency histogram engine, adaptive polling, public stats API */

#include "kernel/drivers/virtio/blk_internal.h"

/* Bucket labels for Registry */
const char *lat_bucket_names[LAT_BUCKET_COUNT] = {
    "Lt1us", "1_10us", "10_100us", "100us_1ms",
    "1_10ms", "10_100ms", "Gt100ms"
};

const char *lat_type_names[LAT_TYPE_COUNT] = {
    "Read", "Write", "Flush", "Discard"
};

/* Record a latency measurement.
 * submit_tsc: rdtsc value at submission time
 * type: LAT_TYPE_READ, LAT_TYPE_WRITE, etc. */
void latency_record(uint64_t submit_tsc, int type)
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
uint64_t latency_percentile(const struct latency_hist *h,
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

const char *io_mode_names[] = {
    "interrupt", "hybrid", "poll"
};

/* Check if 100ms window has elapsed and transition modes if needed */
void adaptive_check_window(void)
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
int hybrid_spin_poll(struct virtqueue *vq, uint32_t spin_us)
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

