/* ============================================================================
 * timezone.h -- Timezone bias and DST management
 *
 * Stores the UTC offset for local-time conversions (FAT32 timestamps,
 * GetLocalTime). Defaults to UTC (bias=0) until set from registry or
 * user configuration.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/filetime.h"

/* Timezone information */
struct tz_info {
    int32_t  bias_minutes;      /* UTC offset in minutes (negative = west) */
    int32_t  dst_bias_minutes;  /* additional DST offset (typically 60 or 0) */
    uint8_t  dst_active;        /* 1 if DST is currently in effect */
};

/* Initialize timezone -- reads from registry if available, defaults to UTC. */
void timezone_init(void);

/* Set timezone. Updates kernel global and persists to registry. */
void timezone_set(const struct tz_info *tz);

/* Get current timezone settings. */
void timezone_get(struct tz_info *out);

/* Get total bias (bias + DST if active) in minutes. */
int32_t timezone_total_bias(void);

/* Convert UTC FILETIME to local FILETIME. */
FILETIME filetime_to_local(FILETIME utc);

/* Convert local FILETIME to UTC FILETIME. */
FILETIME filetime_from_local(FILETIME local);
