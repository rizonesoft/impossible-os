/* ============================================================================
 * timer.c -- Unified Timer Subsystem (UTS) Implementation
 *
 * All kernel timekeeping routes through g_system_timer.  This file provides
 * the hardware-agnostic wrappers that delegate to whichever backend
 * (PIT or LAPIC) was selected by timer_hal_init() (§6.4).
 *
 * Before g_system_timer is set, all functions are safe no-ops:
 *   - sleep_ms() returns immediately
 *   - system_get_ticks() returns 0
 *   - uptime() returns 0
 * ============================================================================ */

#include "kernel/timer.h"
#include "kernel/boot_init.h"
#include "kernel/boot_info.h"
#include "kernel/drivers/framebuffer.h"

/* THE single source of truth -- set once by timer_hal_init() (§6.4) */
timer_driver_t *g_system_timer = (timer_driver_t *)0;

void sleep_ms(uint32_t ms)
{
    if (!g_system_timer || !g_system_timer->sleep_ms)
        return;
    g_system_timer->sleep_ms(ms);
}

uint64_t system_get_ticks(void)
{
    if (!g_system_timer || !g_system_timer->get_ticks)
        return 0;
    return g_system_timer->get_ticks();
}

uint32_t system_get_freq(void)
{
    if (!g_system_timer || !g_system_timer->get_freq)
        return 0;
    return g_system_timer->get_freq();
}

uint64_t uptime(void)
{
    uint32_t freq = system_get_freq();
    if (freq == 0)
        return 0;
    return system_get_ticks() / freq;
}

/* ---- Timer tick callback (ISR-context periodic callback) ---- */

static void (*tick_cb_fn)(void) = (void *)0;
static uint32_t tick_cb_divisor = 0;
static uint32_t tick_cb_counter = 0;

void timer_register_tick_callback(void (*fn)(void), uint32_t every_n_ticks)
{
    tick_cb_counter = 0;
    tick_cb_divisor = every_n_ticks;
    tick_cb_fn = fn;
}

void timer_unregister_tick_callback(void)
{
    tick_cb_fn = (void *)0;
    tick_cb_divisor = 0;
    tick_cb_counter = 0;
}

/* ---- Alive blink: 4x4 px top-left corner, toggled on timer interrupt ---- */

static uint8_t  s_blink_state;
static uint32_t s_blink_counter;
#define BLINK_RATE      50  /* toggle every 50 ticks = 0.5 sec at 100 Hz */
#define BLINK_X         4
#define BLINK_Y         4
#define BLINK_SIZE      8
#define BLINK_COLOR_ON  0x0000FF00  /* green */
#define BLINK_COLOR_OFF 0x00000000  /* black */

static void alive_blink_tick(void)
{
    if (!g_boot_info.config.heartbeat)
        return;
    if (!kernel_subsystem_ready(SUBSYS_FB))
        return;
    /* Stop once desktop compositor takes over */
    if (kernel_subsystem_ready(SUBSYS_DESKTOP))
        return;

    s_blink_counter++;
    if (s_blink_counter < BLINK_RATE)
        return;
    s_blink_counter = 0;
    s_blink_state = !s_blink_state;

    {
        /* Anti-aliased filled circle using distance-based alpha blending */
        int r = BLINK_SIZE / 2;
        int cx = BLINK_X + r;
        int cy = BLINK_Y + r;
        int dy, dx;
        uint32_t on_r = (BLINK_COLOR_ON >> 16) & 0xFF;
        uint32_t on_g = (BLINK_COLOR_ON >> 8) & 0xFF;
        uint32_t on_b = BLINK_COLOR_ON & 0xFF;

        for (dy = -r - 1; dy <= r + 1; dy++) {
            for (dx = -r - 1; dx <= r + 1; dx++) {
                /* Distance from center (scaled by 256 for sub-pixel precision) */
                uint32_t dist_sq = (uint32_t)(dx * dx + dy * dy);
                uint32_t r_sq = (uint32_t)(r * r);
                uint32_t px, py;
                uint32_t c;

                px = (uint32_t)(cx + dx);
                py = (uint32_t)(cy + dy);

                if (s_blink_state) {
                    if (dist_sq <= r_sq - (uint32_t)r) {
                        /* Fully inside */
                        c = BLINK_COLOR_ON;
                    } else if (dist_sq <= r_sq + (uint32_t)(r * 2)) {
                        /* Edge: blend with black based on distance */
                        uint32_t alpha = 255 - ((dist_sq - (r_sq - (uint32_t)r)) * 255)
                                         / ((uint32_t)(r * 3) + 1);
                        if (alpha > 255) alpha = 0;
                        c = ((on_r * alpha / 255) << 16) |
                            ((on_g * alpha / 255) << 8) |
                            (on_b * alpha / 255);
                    } else {
                        c = 0x00000000;
                    }
                } else {
                    c = 0x00000000;
                }
                fb_put_pixel(px, py, c);
            }
        }
    }
    fb_swap_rect(BLINK_X, BLINK_Y, BLINK_SIZE + 2, BLINK_SIZE + 2);
}

void timer_tick_callback_fire(void)
{
    alive_blink_tick();

    if (!tick_cb_fn) return;
    tick_cb_counter++;
    if (tick_cb_counter >= tick_cb_divisor) {
        tick_cb_counter = 0;
        tick_cb_fn();
    }
}

#include "kernel/cpuid_platform.h"
#include "kernel/drivers/pit.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/klog.h"

void timer_hal_init(void)
{
    platform_detect();

    if (platform_is_tcg()) {
        /* QEMU TCG: LAPIC timer runs from QEMU_CLOCK_VIRTUAL (~10x slower
         * than wall time).  PIT is host-backed = wall-clock accurate.
         * TCG always emulates legacy PIT hardware, so this is safe. */
        pit_init();
        g_system_timer = &pit_driver;
        klog(LOG_INFO, "timer",
             "UTS: %s selected (QEMU TCG -- PIT is wall-clock accurate)",
             g_system_timer->name);
    } else {
        /* Real HW, Hyper-V, VMware, VBox, KVM: LAPIC is the best timer.
         * The calibration waterfall finds the freq without assuming PIT. */
        lapic_timer_calibrate();   /* waterfall: MSR → CPUID → HPET → PM → PIT */
        lapic_timer_init(100);     /* 100 Hz periodic mode */
        g_system_timer = &lapic_driver;

        /* Suppress PIT -- never program it on non-TCG platforms.
         * Mask PIT IRQ0 to prevent ghost ticks from stale PIT state. */
        if (ioapic_available())
            ioapic_mask_irq(0);

        klog(LOG_INFO, "timer",
             "UTS: %s selected (%s, %u ticks/ms = %u MHz bus)",
             g_system_timer->name,
             platform_name(),
             (uint64_t)lapic_timer_ticks_per_ms(),
             (uint64_t)(lapic_timer_ticks_per_ms() / 1000));
    }
}
