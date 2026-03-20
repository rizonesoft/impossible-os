/* ============================================================================
 * pit.c — Programmable Interval Timer (8253/8254) driver
 *
 * Programs PIT channel 0 in rate generator mode (mode 2) to fire
 * IRQ 0 at ~100 Hz. Each tick increments a global counter used for
 * timekeeping, sleep_ms(), and uptime().
 * ============================================================================ */

#include "kernel/drivers/pit.h"
#include "kernel/drivers/pic.h"
#include "kernel/timer.h"
#include "kernel/idt.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"

/* Optional periodic callback (for boot splash animation etc.) */
static void (*pit_callback_fn)(void) = (void *)0;
static uint32_t pit_callback_divisor = 0;  /* call every N ticks */
static uint32_t pit_callback_counter = 0;

/* Spinlock protecting tick_count and callback state.
 * Used with irqsave/irqrestore so it is safe even if called from
 * inside another IRQ handler where IRQs are already off. */
static spinlock_t pit_lock = SPINLOCK_INIT;

/* PIT I/O ports */
#define PIT_CHANNEL0  0x40
#define PIT_CMD       0x43

/* PIT command byte:
 * Bits 7-6: Channel 0 (00)
 * Bits 5-4: Access mode lo/hi byte (11)
 * Bits 3-1: Mode 2 — rate generator (010)
 * Bit 0:    Binary counting (0)
 * = 0b00110100 = 0x34 */
#define PIT_CMD_CH0_RATE  0x34

/* Inline port I/O */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Global tick counter (volatile — modified in interrupt context) */
static volatile uint64_t tick_count = 0;

/* Computed divisor and actual frequency */
static uint32_t pit_divisor;
static uint32_t pit_actual_freq;

/* Increment tick counter and fire callback. Called from either
 * pit_irq_handler (PIT-as-tick-source) or LAPIC timer handler. */
int pit_tick_increment(void)
{
    int callback_fired = 0;
    uint64_t flags;

    spin_lock_irqsave(&pit_lock, &flags);
    tick_count++;

    /* Fire optional callback (e.g., boot splash animation) */
    if (pit_callback_fn) {
        pit_callback_counter++;
        if (pit_callback_counter >= pit_callback_divisor) {
            pit_callback_counter = 0;
            pit_callback_fn();
            callback_fired = 1;
        }
    }
    spin_unlock_irqrestore(&pit_lock, flags);
    return callback_fired;
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

/* IRQ 0 handler — called on every PIT tick.
 * Handles tick counting and spinner callback only.
 * Preemptive scheduling is driven by the LAPIC timer handler.
 * Returns the same frame (no context switch from PIT). */
static uint64_t pit_irq_handler(struct interrupt_frame *frame)
{
    pit_tick_increment();
    irq_eoi(IRQ_TIMER);
    return (uint64_t)frame;
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

    /* Unmask IRQ 0 on the PIC */
    pic_unmask_irq(IRQ_TIMER);

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

/* Wrapper to match timer_driver_t.init signature (PIT ignores hz — always PIT_TARGET_FREQ) */
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
};

void pit_register_callback(void (*fn)(void), uint32_t every_n_ticks)
{
    uint64_t flags;
    spin_lock_irqsave(&pit_lock, &flags);
    pit_callback_counter = 0;
    pit_callback_divisor = every_n_ticks;
    pit_callback_fn = fn;
    spin_unlock_irqrestore(&pit_lock, flags);
}

void pit_unregister_callback(void)
{
    uint64_t flags;
    spin_lock_irqsave(&pit_lock, &flags);
    pit_callback_fn = (void *)0;
    pit_callback_divisor = 0;
    pit_callback_counter = 0;
    spin_unlock_irqrestore(&pit_lock, flags);
}
