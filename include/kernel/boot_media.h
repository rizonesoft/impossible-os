/* ============================================================================
 * boot_media.h -- Boot media speed detection (TODO-19 USB boot hardening)
 *
 * Times a 4 KiB boot-volume read once at boot and classifies the media as
 * fast / medium / slow, so the kernel can adapt (proactively enable deferred
 * klog flushing on slow USB media, skip non-critical boot tests, tune timeouts).
 * The measured value is a KERNEL-runtime global (NOT a boot_info ABI field --
 * boot_info is the bootloader handoff, set pre-kernel).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

typedef enum {
    BOOT_MEDIA_UNKNOWN = 0,  /* not yet probed */
    BOOT_MEDIA_FAST    = 1,  /* SSD / NVMe   (< 1 ms / 4 KiB) */
    BOOT_MEDIA_MEDIUM  = 2,  /* USB 3.0      (1-10 ms / 4 KiB) */
    BOOT_MEDIA_SLOW    = 3   /* USB 2.0      (> 10 ms / 4 KiB) */
} boot_media_speed_t;

#define BOOT_MEDIA_FAST_MAX_US    1000u   /* < 1 ms  -> FAST */
#define BOOT_MEDIA_MEDIUM_MAX_US  10000u  /* < 10 ms -> MEDIUM, else SLOW */

/* Classify a per-4-KiB read time (microseconds) into a media speed class. Pure;
 * exposed for tests. */
boot_media_speed_t boot_media_classify(uint32_t us_per_4kib);

/* Human-readable class name ("fast"/"medium"/"slow"/"unknown"). Pure. */
const char *boot_media_speed_name(boot_media_speed_t s);

/* 1 when the class is USB-attached storage (MEDIUM = USB 3.0, SLOW = USB 2.0),
 * 0 for FAST (SSD/NVMe) and UNKNOWN. Used to reduce I/O-heavy debug boot tests on
 * USB media so a slow boot does not freeze. Pure; exposed for tests. */
int boot_media_is_usb_class(boot_media_speed_t s);

/* The probed boot-media speed (BOOT_MEDIA_UNKNOWN until boot_media_probe runs). */
boot_media_speed_t boot_media_speed(void);

/* Probe once: time a FULL 4 KiB boot-volume read, classify + store the result, log
 * it, and proactively enable deferred klog flushing on SLOW media. Idempotent (a
 * second call is a no-op once a class is set). Call after VFS mount but BEFORE the
 * klog disk / live-log enable, so a SLOW classification arms deferred mode before
 * any disk-log flush pays the slow-media cost. */
void boot_media_probe(void);
