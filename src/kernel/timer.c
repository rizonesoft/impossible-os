/* ============================================================================
 * timer.c -- Unified Timer Subsystem (UTS) Implementation
 *
 * All kernel timekeeping routes through g_system_timer.  This file provides
 * the hardware-agnostic wrappers that delegate to whichever backend
 * (PIT or LAPIC) was selected by timer_hal_init().
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
#include "kernel/sched/spinlock.h"

/* THE single source of truth -- set once by timer_hal_init() */
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
    /* Derive from the rebased monotonic source: dividing the lifetime
     * tick counter by the LIVE frequency would rewind across a
     * KeSetTimerResolution rate change (ticks accumulated at the old
     * rate reinterpreted at the new one) */
    return uptime_ns() / 1000000000ULL;
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

/* Timer mode transitions (arm one-shot / quiesce / resume) share one
 * lock so an arm can never rewrite an LVT that a UEFI runtime call just
 * masked. The timer ISR does NOT take this lock: holders run with IRQs
 * disabled (irqsave) and the tick ISR is BSP-only, so the ISR and a
 * same-CPU holder are mutually excluded; the cross-CPU ISR-restore case
 * preserves the mask bit instead. */
static spinlock_t s_timer_mode_lock = SPINLOCK_INIT;
static int s_quiesced;   /* guarded by s_timer_mode_lock */

/* Pure delegation core (testable with a fake driver): refuses a NULL
 * driver or a backend without one-shot support, otherwise forwards the
 * absolute deadline and preserves the backend's return value. */
int timer_arm_oneshot_on(timer_driver_t *drv, uint64_t deadline_mono_ns)
{
    if (!drv || !drv->arm_oneshot)
        return -1;
    return drv->arm_oneshot(deadline_mono_ns);
}

int timer_arm_oneshot(uint64_t deadline_mono_ns)
{
    uint64_t irqf;
    int rc;

    spin_lock_irqsave(&s_timer_mode_lock, &irqf);
    if (s_quiesced) {
        /* A UEFI runtime call masked the tick: arming now would rewrite
         * the LVT unmasked while firmware may have interrupts enabled */
        spin_unlock_irqrestore(&s_timer_mode_lock, irqf);
        return -1;
    }
    rc = timer_arm_oneshot_on(g_system_timer, deadline_mono_ns);
    spin_unlock_irqrestore(&s_timer_mode_lock, irqf);
    return rc;
}

/* ---- Timer tick callback (ISR-context periodic callback) ---- */

static void (*tick_cb_fn)(void) = (void *)0;
static uint32_t tick_cb_divisor = 0;
static uint32_t tick_cb_counter = 0;

/* Register/unregister run with interrupts enabled while the timer ISR reads
 * the same slot. Publish fn LAST with release ordering so the ISR never
 * observes a non-NULL fn paired with an unwritten divisor; retract fn FIRST
 * so the ISR bails out before divisor/counter are cleared. */
void timer_register_tick_callback(void (*fn)(void), uint32_t every_n_ticks)
{
    tick_cb_counter = 0;
    tick_cb_divisor = every_n_ticks;
    __atomic_store_n(&tick_cb_fn, fn, __ATOMIC_RELEASE);
}

void timer_unregister_tick_callback(void)
{
    __atomic_store_n(&tick_cb_fn, (void (*)(void))0, __ATOMIC_RELEASE);
    tick_cb_divisor = 0;
    tick_cb_counter = 0;
}

void timer_tick_callback_fire(void)
{
    /* CR0/CR4 safety-bit periodic verify (TODO-09-boot S7). Own counter, NOT
     * the singleton tick_cb_fn slot below (that is owned by the boot splash and
     * unregistered at desktop start). The LAPIC timer fires only on the BSP (AP
     * LAPIC timers are masked), so this is the BSP periodic integrity check;
     * APs are verified at ap_cpu_harden tail + on #GP. */
    {
        extern void cpu_cr_pin_tick(void);
        static uint32_t cr_pin_counter;
        if (++cr_pin_counter >= 256) {
            cr_pin_counter = 0;
            cpu_cr_pin_tick();
        }
    }

    /* Snapshot fn with acquire ordering (pairs with the release store in
     * register/unregister) and call the snapshot -- never re-read the slot
     * between the NULL check and the indirect call. */
    void (*fn)(void) = __atomic_load_n(&tick_cb_fn, __ATOMIC_ACQUIRE);
    uint32_t divisor = tick_cb_divisor;
    if (!fn || divisor == 0) return;
    tick_cb_counter++;
    if (tick_cb_counter >= divisor) {
        tick_cb_counter = 0;
        fn();
    }
}

#include "kernel/cpuid_platform.h"
#include "kernel/drivers/pit.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/drivers/pic.h"
#include "kernel/klog.h"
#include "kernel/boot_halt.h"

/* Serialized tick-rate change: the rebase + LAPIC reprogram must be one
 * mode transition (an interleaved arm/quiesce could pair an old epoch
 * with a new rate). PIT backend has a fixed rate: no-op there. */
int timer_set_tick_hz(uint32_t new_hz)
{
    uint64_t irqf;
    int rc = -1;

    spin_lock_irqsave(&s_timer_mode_lock, &irqf);
    if (g_system_timer == &lapic_driver)
        rc = lapic_timer_set_hz(new_hz);
    spin_unlock_irqrestore(&s_timer_mode_lock, irqf);
    return rc;
}

/* ---- Backend-aware tick quiesce (UEFI runtime-call safety) ----
 * Single-user (serialized by the UEFI RT mutex); the mode lock below
 * additionally excludes a concurrent one-shot arm. State is "which
 * backend was masked + what to restore". */
static uint32_t s_quiesce_saved_lvt;   /* LAPIC backend: pre-mask LVT */
static int      s_quiesce_pit_masked;  /* PIT backend: 1=IOAPIC, 2=PIC */

void timer_hal_quiesce(void)
{
    uint64_t irqf;

    spin_lock_irqsave(&s_timer_mode_lock, &irqf);
    if (s_quiesced || !g_system_timer) {
        spin_unlock_irqrestore(&s_timer_mode_lock, irqf);
        return;
    }

    if (g_system_timer == &lapic_driver) {
        uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
        lapic_write(LAPIC_REG_LVT_TIMER, lvt | LVT_MASKED);
        s_quiesce_saved_lvt = lvt;
    } else if (g_system_timer == &pit_driver) {
        /* The PIT tick arrives through whichever controller routes ISA
         * IRQ0 -- mask at that controller, not at the 8254 itself */
        if (ioapic_available()) {
            ioapic_mask_irq(ioapic_isa_to_gsi(0));
            s_quiesce_pit_masked = 1;
        } else if (pic_available()) {
            pic_mask_irq(0);
            s_quiesce_pit_masked = 2;
        }
    }
    s_quiesced = 1;
    spin_unlock_irqrestore(&s_timer_mode_lock, irqf);
}

void timer_hal_resume(void)
{
    uint64_t irqf;

    spin_lock_irqsave(&s_timer_mode_lock, &irqf);
    if (!s_quiesced) {
        spin_unlock_irqrestore(&s_timer_mode_lock, irqf);
        return;
    }

    if (g_system_timer == &lapic_driver) {
        lapic_write(LAPIC_REG_LVT_TIMER, s_quiesce_saved_lvt);
        /* An armed one-shot whose deadline expired while masked never
         * reaches the ISR auto-restore: drop it and force periodic */
        lapic_timer_resume_fixup();
    } else if (s_quiesce_pit_masked == 1) {
        ioapic_unmask_irq(ioapic_isa_to_gsi(0));
    } else if (s_quiesce_pit_masked == 2) {
        pic_unmask_irq(0);
    }
    s_quiesce_saved_lvt = 0;
    s_quiesce_pit_masked = 0;
    s_quiesced = 0;
    spin_unlock_irqrestore(&s_timer_mode_lock, irqf);
}

void timer_hal_init(void)
{
    platform_detect();

    /* Bare metal timer workaround REMOVED -- root cause was clac (#UD on
     * CPUs without SMAP CPUID support) in isr_common_stub, fixed 2026-03-29.
     * Hardware interrupts now work on bare metal. */

    if (platform_is_tcg()) {
        pit_init();
        g_system_timer = &pit_driver;
        klog(LOG_INFO, "timer",
             "UTS: %s selected (QEMU TCG -- PIT is wall-clock accurate)",
             g_system_timer->name);
    } else if (lapic_available()) {
        /* Real HW, Hyper-V, VMware, VBox, KVM: LAPIC is the best timer.
         * The calibration waterfall finds the freq without assuming PIT. */
        lapic_timer_calibrate();   /* waterfall: MSR → CPUID → HPET → PM → PIT */

        if (!lapic_timer_calibrated()) {
            /* All calibration tiers failed: the hardcoded estimate must
             * never drive the system tick. Degrade to a routable PIT or
             * fail loud -- a guessed LAPIC frequency is a silent-skew boot. */
            if (pit_present() && (ioapic_available() || pic_available())) {
                klog(LOG_WARN, "timer",
                     "UTS: LAPIC calibration failed -- falling back to PIT");
                pit_init();
                g_system_timer = &pit_driver;
                klog(LOG_INFO, "timer",
                     "UTS: %s selected (calibration-fallback)",
                     g_system_timer->name);
                return;
            }
            boot_halt("timer: LAPIC calibration failed and no usable PIT "
                      "fallback -- no trustworthy tick source");
        }

        lapic_timer_init(100);     /* 100 Hz periodic mode */

        g_system_timer = &lapic_driver;

        /* Suppress PIT -- never program it on non-TCG platforms.
         * Mask the PIT's routed GSI (ISA IRQ 0 is usually overridden to
         * GSI 2) to prevent ghost ticks from stale PIT state. */
        if (ioapic_available())
            ioapic_mask_irq(ioapic_isa_to_gsi(0));

        klog(LOG_INFO, "timer",
             "UTS: %s selected (%s, %u ticks/ms = %u MHz bus)",
             g_system_timer->name,
             platform_name(),
             (uint64_t)lapic_timer_ticks_per_ms(),
             (uint64_t)(lapic_timer_ticks_per_ms() / 1000));
    } else if (pit_present() && (ioapic_available() || pic_available())) {
        /* LAPIC bring-up failed but the platform has a PIT and an external
         * controller can route it: degrade to the PIT tick instead of
         * booting timer-less. */
        klog(LOG_WARN, "timer",
             "UTS: LAPIC unavailable -- falling back to PIT");
        pit_init();
        g_system_timer = &pit_driver;
        klog(LOG_INFO, "timer", "UTS: %s selected (LAPIC-fallback)",
             g_system_timer->name);
    } else {
        /* No LAPIC timer and no usable PIT (absent or unroutable): a
         * silent timer-less boot hangs later with no diagnostic. */
        boot_halt("timer: no usable tick source (LAPIC failed, PIT absent or unroutable)");
    }
}
