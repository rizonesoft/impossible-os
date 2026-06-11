/* ============================================================================
 * pit.c -- Programmable Interval Timer (8253/8254) driver
 *
 * Programs PIT channel 0 in rate generator mode (mode 2) to fire
 * IRQ 0 at ~100 Hz. Each tick increments a global counter used for
 * timekeeping, sleep_ms(), and uptime().
 * ============================================================================ */

#include "kernel/drivers/pit.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/acpi.h"
#include "kernel/boot_info.h"
#include "kernel/timer.h"
#include "kernel/time/mono_clock.h"
#include "kernel/idt.h"
#include "kernel/klog.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"

/* Spinlock protecting tick_count.
 * Used with irqsave/irqrestore so it is safe even if called from
 * inside another IRQ handler where IRQs are already off. */
static spinlock_t pit_lock = SPINLOCK_INIT;

/* PIT I/O ports */
#define PIT_CHANNEL0  0x40
#define PIT_CMD       0x43

/* PIT command byte:
 * Bits 7-6: Channel 0 (00)
 * Bits 5-4: Access mode lo/hi byte (11)
 * Bits 3-1: Mode 2 -- rate generator (010)
 * Bit 0:    Binary counting (0)
 * = 0b00110100 = 0x34 */
#define PIT_CMD_CH0_RATE  0x34

/* Inline port I/O */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Global tick counter (volatile -- modified in interrupt context) */
static volatile uint64_t tick_count = 0;

/* Computed divisor and actual frequency */
static uint32_t pit_divisor;
static uint32_t pit_actual_freq;

/* Increment the tick counter and fire the UTS tick callback. The callback
 * fires AFTER releasing pit_lock -- it may render frames and copy to VRAM;
 * holding the timekeeping lock across that stalls pit_get_ticks() on other
 * CPUs. The NT timer scan moved to pit_irq_handler so the PIT tick path
 * runs the same post-tick sequence as lapic_timer_handler(). */
void pit_tick_increment(void)
{
    uint64_t flags;

    spin_lock_irqsave(&pit_lock, &flags);
    tick_count++;
    spin_unlock_irqrestore(&pit_lock, flags);

    timer_tick_callback_fire();
}

/* Set the actual timer frequency (when LAPIC timer replaces PIT). */
void pit_set_freq(uint32_t hz)
{
    pit_actual_freq = hz;
}

/* Stop PIT channel 0 hardware. Tick counter + sleep_ms remain usable. */
void pit_stop(void)
{
    /* Program PIT channel 0 to mode 0, count = 0 → stops generating IRQs */
    outb(PIT_CMD, 0x30);   /* channel 0, lobyte/hibyte, mode 0, binary */
    outb(PIT_CHANNEL0, 0);
    outb(PIT_CHANNEL0, 0);
}

/* IRQ 0 handler -- called on every PIT tick.
 * Drives preemptive scheduling on TCG (where LAPIC timer is not the
 * tick source). Without this, compositor_run() starves all other tasks
 * because the PIT never calls schedule(). */
static uint64_t pit_irq_handler(struct interrupt_frame *frame)
{
    extern uint64_t schedule(struct interrupt_frame *frame);
    extern uint32_t dpc_drain_current_cpu(void);
    extern void kusd_update_time(void);
    extern void nt_timer_tick(void);
    /* Same post-tick order as lapic_timer_handler(): tick + callback,
     * EOI, KUSD time for user readers, NT timer scan (sees fresh KUSD),
     * DPC drain, then schedule. Keep the two paths in lockstep. */
    pit_tick_increment();
    irq_eoi(IRQ_TIMER);
    kusd_update_time();
    nt_timer_tick();
    dpc_drain_current_cpu();
    return schedule(frame);
}

int pit_present(void)
{
    extern struct boot_info g_boot_info;
    /* No ACPI: legacy PC, PIT assumed present. With ACPI: PIT exists only
     * when PCAT_COMPAT=1 and the FADT is not hardware-reduced -- the same
     * rule the LAPIC calibration Tier 3 enforces before touching PIT ports. */
    if (!g_boot_info.acpi_available)
        return 1;
    return acpi_pcat_compat() && !acpi_hw_reduced();
}

void pit_init(void)
{
    /* Calculate the PIT divisor for the target frequency */
    pit_divisor = PIT_BASE_FREQ / PIT_TARGET_FREQ;
    pit_actual_freq = PIT_BASE_FREQ / pit_divisor;

    /* Program PIT channel 0: rate generator mode, lo/hi access */
    outb(PIT_CMD, PIT_CMD_CH0_RATE);
    outb(PIT_CHANNEL0, (uint8_t)(pit_divisor & 0xFF));        /* Low byte */
    outb(PIT_CHANNEL0, (uint8_t)((pit_divisor >> 8) & 0xFF)); /* High byte */

    /* Register our IRQ 0 handler (interrupt vector 32 after PIC remap) */
    idt_register_handler(32, pit_irq_handler);

    /* Unmask IRQ 0: PIC path (no-op when PIC disabled) + IOAPIC path.
     * IOAPIC routes IRQs masked by default; unmask only after handler exists
     * so the PIT never fires into an unregistered IDT slot. */
    pic_unmask_irq(IRQ_TIMER);
    if (ioapic_available())
        ioapic_unmask_irq(ioapic_isa_to_gsi(IRQ_TIMER));

    klog(LOG_INFO, "timer", "PIT timer: %u Hz (divisor %u)",
           (uint64_t)pit_actual_freq, (uint64_t)pit_divisor);
}

uint64_t pit_get_ticks(void)
{
    uint64_t t;
    uint64_t flags;
    spin_lock_irqsave(&pit_lock, &flags);
    t = tick_count;
    spin_unlock_irqrestore(&pit_lock, flags);
    return t;
}

/* ---- PIT-specific sleep (used by pit_driver vtable) ---- */

static void pit_sleep_ms(uint32_t ms)
{
    uint64_t target;
    uint64_t flags;
    spin_lock_irqsave(&pit_lock, &flags);
    target = tick_count + ((uint64_t)ms * pit_actual_freq / 1000);
    spin_unlock_irqrestore(&pit_lock, flags);

    /* Normal sleep: wait for ticks.
     * Note: PIT is only selected as g_system_timer on QEMU TCG,
     * where PIT hardware is always emulated and ticks always fire. */
    while (pit_get_ticks() < target)
        __asm__ volatile("hlt");
}

uint32_t pit_get_freq(void)
{
    return pit_actual_freq;
}

/* Wrapper to match timer_driver_t.init signature (PIT ignores hz -- always PIT_TARGET_FREQ) */
static void pit_init_wrapper(uint32_t hz)
{
    (void)hz;
    pit_init();
}

/* ---- PIT driver vtable (exposed for timer HAL selection) ---- */

timer_driver_t pit_driver = {
    .name      = "PIT",
    .init      = pit_init_wrapper,
    .get_ticks = pit_get_ticks,
    .sleep_ms  = pit_sleep_ms,
    .get_freq  = pit_get_freq,
    /* Single clocksource contract: delegate to mono_clock instead of
     * growing a second ns path; pre-init mono_ns() returns 0 and
     * uptime_ns() falls back to the tick counter. No one-shot on PIT. */
    .read_ns   = mono_ns,
};

