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

uint64_t uptime_ns(void)
{
    /* Prefer read_ns() if the driver provides it */
    if (g_system_timer && g_system_timer->read_ns) {
        uint64_t ns = g_system_timer->read_ns();
        if (ns > 0) return ns;
    }
    /* Fallback: ticks * (1e9 / freq) */
    {
        uint32_t freq = system_get_freq();
        if (freq == 0) return 0;
        return (system_get_ticks() * 1000000000ULL) / freq;
    }
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

void timer_tick_callback_fire(void)
{
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

    /* Bare metal timer workaround REMOVED — root cause was clac (#UD on
     * CPUs without SMAP CPUID support) in isr_common_stub, fixed 2026-03-29.
     * Hardware interrupts now work on bare metal. */

    if (platform_is_tcg()) {
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
