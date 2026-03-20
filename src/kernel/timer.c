/* ============================================================================
 * timer.c — Unified Timer Subsystem (UTS) Implementation
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

/* THE single source of truth — set once by timer_hal_init() (§6.4) */
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

/* ---- UTS initialization (§6.4) ---- */

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
             "UTS: %s selected (QEMU TCG — PIT is wall-clock accurate)",
             g_system_timer->name);
    } else {
        /* Real HW, Hyper-V, VMware, VBox, KVM: LAPIC is the best timer.
         * The calibration waterfall finds the freq without assuming PIT. */
        lapic_timer_calibrate();   /* waterfall: MSR → CPUID → HPET → PM → PIT */
        lapic_timer_init(100);     /* 100 Hz periodic mode */
        g_system_timer = &lapic_driver;

        /* Suppress PIT — never program it on non-TCG platforms.
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
