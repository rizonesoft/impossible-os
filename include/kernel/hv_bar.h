/* ============================================================================
 * hv_bar.h -- Visual debug bars for bare-metal boot debugging
 *
 * Draws colored horizontal bars directly to VRAM at key boot stages.
 * Zero dependencies: uses only g_boot_info.fb for the framebuffer address.
 * Each bar is 4px tall at a unique Y offset so you can see exactly how far
 * boot progressed before a crash/restart.
 *
 * Usage:  HV_BAR(n)   where n = 0..15
 * Colors cycle through a high-contrast palette.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_info.h"

/* Bar height in pixels */
#define HV_BAR_HEIGHT 4

/* Draw bar number `n` (0-based).  Placed at y = n * (HV_BAR_HEIGHT + 1).
 * Writes directly to the hardware framebuffer -- no back buffer, no swap. */
static inline void hv_bar(uint32_t n)
{
    static const uint32_t colors[] = {
        0x00FF0000,  /*  0  red        - kernel_main entry         */
        0x0000FF00,  /*  1  green      - boot_info parsed          */
        0x000000FF,  /*  2  blue       - PMM done                  */
        0x00FFFF00,  /*  3  yellow     - VMM done                  */
        0x00FF00FF,  /*  4  magenta    - HEAP done                 */
        0x0000FFFF,  /*  5  cyan       - KLOG done                 */
        0x00FF8800,  /*  6  orange     - Phase 1 entry (GDT/IDT)   */
        0x008800FF,  /*  7  purple     - LAPIC/IOAPIC done         */
        0x0088FF00,  /*  8  lime       - Timer done                */
        0x00FF0088,  /*  9  pink       - FB init done              */
        0x000088FF,  /* 10  sky blue   - Phase 2 entry (PCI)       */
        0x00FFFFFF,  /* 11  white      - VFS done                  */
        0x00888888,  /* 12  gray       - Phase 3 entry (sched)     */
        0x00FFAA00,  /* 13  amber      - Desktop init              */
        0x0000FF88,  /* 14  teal       - Compositor entry          */
        0x00FF4444,  /* 15  light red  - reserved                  */
    };

    uint32_t color;
    uint32_t *fb;
    uint32_t w, pitch_px, y_start, y, x;

    if (!g_boot_info.fb_available || !g_boot_info.fb.addr)
        return;

    fb       = (uint32_t *)(uintptr_t)g_boot_info.fb.addr;
    w        = g_boot_info.fb.width;
    pitch_px = g_boot_info.fb.pitch / 4;  /* pitch is in bytes, 4 bytes/pixel */
    color    = colors[n & 0xF];
    y_start  = n * (HV_BAR_HEIGHT + 1);

    for (y = y_start; y < y_start + HV_BAR_HEIGHT && y < g_boot_info.fb.height; y++)
        for (x = 0; x < w; x++)
            fb[y * pitch_px + x] = color;
}

/* Convenience macro */
#define HV_BAR(n) hv_bar(n)
