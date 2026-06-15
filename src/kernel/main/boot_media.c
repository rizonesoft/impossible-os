/* ============================================================================
 * boot_media.c -- Boot media speed detection (TODO-19 USB boot hardening)
 *
 * Times a 4 KiB boot-volume read once and classifies the media so the kernel can
 * adapt to slow USB media. See boot_media.h for the contract.
 * ============================================================================ */

#include "kernel/boot_media.h"
#include "kernel/fs/vfs.h"
#include "kernel/klog.h"
#include "kernel/timer.h"

static boot_media_speed_t s_boot_media_speed = BOOT_MEDIA_UNKNOWN;

boot_media_speed_t boot_media_classify(uint32_t us_per_4kib)
{
    if (us_per_4kib < BOOT_MEDIA_FAST_MAX_US)
        return BOOT_MEDIA_FAST;
    if (us_per_4kib < BOOT_MEDIA_MEDIUM_MAX_US)
        return BOOT_MEDIA_MEDIUM;
    return BOOT_MEDIA_SLOW;
}

const char *boot_media_speed_name(boot_media_speed_t s)
{
    switch (s) {
    case BOOT_MEDIA_FAST:   return "fast";
    case BOOT_MEDIA_MEDIUM: return "medium";
    case BOOT_MEDIA_SLOW:   return "slow";
    default:                return "unknown";
    }
}

int boot_media_is_usb_class(boot_media_speed_t s)
{
    return s == BOOT_MEDIA_MEDIUM || s == BOOT_MEDIA_SLOW;
}

boot_media_speed_t boot_media_speed(void)
{
    return s_boot_media_speed;
}

void boot_media_probe(void)
{
    /* Idempotent: only probe once. */
    if (s_boot_media_speed != BOOT_MEDIA_UNKNOWN)
        return;

    /* Time a 4 KiB read of the first available boot-volume file. Best-effort: if no
     * candidate opens, leave UNKNOWN and let the reactive slow-media auto-enable
     * (the >5s-flush path) cover it -- no false adaptation on an unmeasured boot. */
    /* Candidates must be >= 4 KiB so the timed read is a full 4 KiB sample (short
     * EOF reads are skipped below). System binaries are reliably larger; small or
     * late-created files (e.g. JSON diag) would only ever short-read to UNKNOWN. */
    static const char *candidates[] = {
        "C:\\Impossible\\System32\\cmd.exe",
        "C:\\Impossible\\System32\\ntdll.dll",
        "C:\\cmd.exe",
    };
    static uint8_t probe_buf[4096];

    uint32_t ci;
    for (ci = 0; ci < sizeof(candidates) / sizeof(candidates[0]); ci++) {
        struct vfs_node *f = vfs_open(candidates[ci], VFS_O_READ);
        if (!f)
            continue;

        uint64_t t0 = uptime_ns();
        int n = vfs_read(f, 0, sizeof(probe_buf), probe_buf);
        uint64_t el_ns = uptime_ns() - t0;
        vfs_close(f);

        /* Only classify a FULL 4 KiB read -- a short/EOF read (file < 4 KiB or a
         * clamped FAT32/IXFS read) would time a partial sample and mislabel slow
         * media as fast, suppressing the deferred-klog + test-skip adaptations. */
        if (n != (int)sizeof(probe_buf))
            continue;  /* short / error -- try the next candidate */

        uint32_t us = (uint32_t)(el_ns / 1000ull);
        s_boot_media_speed = boot_media_classify(us);

        /* Proactively switch klog to deferred (RAM-batched) flushing on slow media
         * BEFORE the klog line below: in live-logging (DEBUG) mode that klog would
         * otherwise trigger the first slow flush before deferral is armed. The
         * boot-end forced flush (klog_disk_flush_all) drains the RAM batch. */
        if (s_boot_media_speed == BOOT_MEDIA_SLOW)
            klog_set_deferred(1);

        klog(LOG_INFO, "boot", "Boot media speed: %s (%u us/4KiB)",
             boot_media_speed_name(s_boot_media_speed), (uint64_t)us);
        return;
    }
    /* No candidate readable -- stay UNKNOWN (safe default). */
}
