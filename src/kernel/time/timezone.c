/* ============================================================================
 * timezone.c -- Timezone bias and DST management
 *
 * The timezone state is published as a SINGLE atomic u64 snapshot so readers
 * (including the KUSER_SHARED_DATA updater, which runs at CLOCK_LEVEL IRQL) get
 * a coherent (bias, dst_bias, dst_active) tuple lock-free -- a plain 3-field
 * struct would tear across an SMP set/read interleave.
 *
 * Sign convention: bias_minutes is the internal UTC offset with WEST NEGATIVE
 * (local = UTC + bias). Win32 / registry store the OPPOSITE positive-west
 * `Bias`; a caller importing those values must negate first (the registry
 * import boundary is deferred until registry integration).
 * ============================================================================ */

#include "kernel/time/timezone.h"
#include "kernel/klog.h"

/* Plausible bounds (minutes). Real zones are within +/-14h; allow +/-24h for
 * slack. DST adjustment is at most a couple hours. Clamping keeps the packed
 * fields in range and prevents total-bias overflow. */
#define TZ_BIAS_LIMIT   (24 * 60)
#define TZ_DST_LIMIT    (4 * 60)

/* Packed snapshot: [0:16] bias_minutes (int16), [16:32] dst_bias_minutes
 * (int16), [32:33] dst_active. Default 0 = UTC, no DST. Written only via
 * __atomic_store_n RELEASE; read via __atomic_load_n ACQUIRE. */
static uint64_t s_tz_packed;

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint64_t tz_pack(int32_t bias, int32_t dst, uint8_t active)
{
    return ((uint64_t)(uint16_t)(int16_t)bias)
         | ((uint64_t)(uint16_t)(int16_t)dst << 16)
         | ((uint64_t)(active ? 1u : 0u) << 32);
}

static void tz_unpack(uint64_t p, struct tz_info *out)
{
    out->bias_minutes     = (int16_t)(uint16_t)(p & 0xFFFFu);
    out->dst_bias_minutes = (int16_t)(uint16_t)((p >> 16) & 0xFFFFu);
    out->dst_active       = (uint8_t)((p >> 32) & 1u);
}

void timezone_init(void)
{
    /* Default to UTC until registry/user configuration is wired. Registry
     * stores positive-west Bias (opposite of our convention); that import
     * conversion lands with registry integration. */
    __atomic_store_n(&s_tz_packed, 0, __ATOMIC_RELEASE);

    klog(LOG_INFO, "time", "Timezone: UTC (bias 0 min, DST inactive)");
}

void timezone_set(const struct tz_info *tz)
{
    int32_t bias, dst;
    if (!tz) return;

    bias = clamp_i32(tz->bias_minutes, -TZ_BIAS_LIMIT, TZ_BIAS_LIMIT);
    dst  = clamp_i32(tz->dst_bias_minutes, -TZ_DST_LIMIT, TZ_DST_LIMIT);

    __atomic_store_n(&s_tz_packed,
                     tz_pack(bias, dst, tz->dst_active),
                     __ATOMIC_RELEASE);

    if (bias != tz->bias_minutes || dst != tz->dst_bias_minutes)
        klog(LOG_WARN, "time",
             "Timezone set: bias/DST clamped to range (bias=%d dst=%d min, %s)",
             (int64_t)bias, (int64_t)dst,
             tz->dst_active ? "DST active" : "DST inactive");
    else
        klog(LOG_INFO, "time",
             "Timezone set: bias=%d min, DST=%d min (%s)",
             (int64_t)bias, (int64_t)dst,
             tz->dst_active ? "active" : "inactive");
}

void timezone_get(struct tz_info *out)
{
    if (out)
        tz_unpack(__atomic_load_n(&s_tz_packed, __ATOMIC_ACQUIRE), out);
}

int32_t timezone_total_bias(void)
{
    struct tz_info tz;
    int64_t total;
    tz_unpack(__atomic_load_n(&s_tz_packed, __ATOMIC_ACQUIRE), &tz);
    /* Both clamped at set time, so the sum cannot overflow int32. */
    total = (int64_t)tz.bias_minutes;
    if (tz.dst_active)
        total += (int64_t)tz.dst_bias_minutes;
    return (int32_t)total;
}

FILETIME filetime_to_local(FILETIME utc)
{
    int64_t bias_ticks, local;
    /* The placeholder is "no time" -- never apply a bias to it (would turn a
     * not-ready 0 into a bogus far-future local time). */
    if (utc == FILETIME_NOW_PLACEHOLDER)
        return utc;
    bias_ticks = (int64_t)timezone_total_bias()
               * 60 * (int64_t)FILETIME_TICKS_PER_SECOND;
    /* Real FILETIMEs (~1.3e17 for 2026) are far below INT64_MAX, so the signed
     * view is exact; clamp a near-epoch + west-bias underflow to 0. */
    local = (int64_t)utc + bias_ticks;
    if (local < 0)
        local = 0;
    return (FILETIME)local;
}

FILETIME filetime_from_local(FILETIME local)
{
    int64_t bias_ticks, utc;
    if (local == FILETIME_NOW_PLACEHOLDER)
        return local;
    bias_ticks = (int64_t)timezone_total_bias()
               * 60 * (int64_t)FILETIME_TICKS_PER_SECOND;
    utc = (int64_t)local - bias_ticks;
    if (utc < 0)
        utc = 0;
    return (FILETIME)utc;
}
