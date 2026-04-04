/* ============================================================================
 * timezone.c -- Timezone bias and DST management
 * ============================================================================ */

#include "kernel/time/timezone.h"
#include "kernel/klog.h"

/* Kernel-global timezone state -- defaults to UTC */
static struct tz_info s_tz = { 0, 0, 0 };

void timezone_init(void)
{
    /* Try reading from registry: HKLM\SYSTEM\...\TimeZoneInformation\Bias
     * Registry stores bias as positive-west (opposite of our convention).
     * For now, default to UTC until registry integration is wired. */
    s_tz.bias_minutes     = 0;
    s_tz.dst_bias_minutes = 0;
    s_tz.dst_active       = 0;

    klog(LOG_INFO, "time", "Timezone: UTC+%d (DST %s)",
         (uint64_t)(uint32_t)(-s_tz.bias_minutes / 60),
         s_tz.dst_active ? "active" : "inactive");
}

void timezone_set(const struct tz_info *tz)
{
    if (!tz) return;
    s_tz = *tz;

    klog(LOG_INFO, "time", "Timezone set: bias=%d min, DST=%d min (%s)",
         (uint64_t)(uint32_t)s_tz.bias_minutes,
         (uint64_t)(uint32_t)s_tz.dst_bias_minutes,
         s_tz.dst_active ? "active" : "inactive");
}

void timezone_get(struct tz_info *out)
{
    if (out) *out = s_tz;
}

int32_t timezone_total_bias(void)
{
    int32_t total = s_tz.bias_minutes;
    if (s_tz.dst_active)
        total += s_tz.dst_bias_minutes;
    return total;
}

FILETIME filetime_to_local(FILETIME utc)
{
    int64_t bias_ticks = (int64_t)timezone_total_bias()
                       * 60 * (int64_t)FILETIME_TICKS_PER_SECOND;
    return (FILETIME)((int64_t)utc + bias_ticks);
}

FILETIME filetime_from_local(FILETIME local)
{
    int64_t bias_ticks = (int64_t)timezone_total_bias()
                       * 60 * (int64_t)FILETIME_TICKS_PER_SECOND;
    return (FILETIME)((int64_t)local - bias_ticks);
}
