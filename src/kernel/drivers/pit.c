/* ============================================================================
 * pit.c — Programmable Interval Timer (8253/8254) driver
 *
 * Programs PIT channel 0 in rate generator mode (mode 2) to fire
 * IRQ 0 at ~100 Hz. Each tick increments a global counter used for
 * timekeeping, sleep_ms(), and uptime().
 * ============================================================================ */

#include "pit.h"
#include "pic.h"
#include "idt.h"
#include "printk.h"

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

/* IRQ 0 handler — called on every PIT tick */
static void pit_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    tick_count++;
    pic_send_eoi(IRQ_TIMER);
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

    printk("[OK] PIT timer: %u Hz (divisor %u)\n",
           (uint64_t)pit_actual_freq, (uint64_t)pit_divisor);
}

uint64_t pit_get_ticks(void)
{
    return tick_count;
}

void sleep_ms(uint32_t ms)
{
    /* Convert ms to ticks: ticks = ms * freq / 1000 */
    uint64_t target = tick_count + ((uint64_t)ms * pit_actual_freq / 1000);

    /* Busy-wait until enough ticks have elapsed */
    while (tick_count < target) {
        __asm__ volatile ("hlt");   /* Sleep until next interrupt */
    }
}

uint64_t uptime(void)
{
    return tick_count / pit_actual_freq;
}
