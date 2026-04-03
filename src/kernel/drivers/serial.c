/* ============================================================================
 * serial.c -- COM1 serial port driver
 *
 * Extracted from main.c for reuse by printk and other subsystems.
 * ============================================================================ */

#include "kernel/drivers/serial.h"
#include "kernel/sched/spinlock.h"

#define SERIAL_PORT 0x3F8

/* Protects UART register access from concurrent threads and IRQ handlers */
static spinlock_t g_serial_lock = SPINLOCK_INIT;

/* Inline port I/O helpers */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Raw unlocked UART write -- caller must hold g_serial_lock */
static inline void serial_putchar_raw(char c)
{
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0)
        ;
    outb(SERIAL_PORT, (uint8_t)c);
}

void serial_init(void)
{
    outb(SERIAL_PORT + 1, 0x00);    /* Disable interrupts */
    outb(SERIAL_PORT + 3, 0x80);    /* Enable DLAB */
    outb(SERIAL_PORT + 0, 0x03);    /* 38400 baud */
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03);    /* 8N1 */
    outb(SERIAL_PORT + 2, 0xC7);    /* FIFO */
    outb(SERIAL_PORT + 4, 0x0B);    /* IRQs, RTS/DSR */
}

void serial_putchar(char c)
{
    uint64_t flags;
    spin_lock_irqsave(&g_serial_lock, &flags);
    serial_putchar_raw(c);
    spin_unlock_irqrestore(&g_serial_lock, flags);
}

/* Hold the lock for the entire string so no other caller can interleave */
void serial_write(const char *str)
{
    uint64_t flags;
    spin_lock_irqsave(&g_serial_lock, &flags);
    while (*str) {
        if (*str == '\n')
            serial_putchar_raw('\r');
        serial_putchar_raw(*str++);
    }
    spin_unlock_irqrestore(&g_serial_lock, flags);
}

char serial_trygetchar(void)
{
    /* Check Line Status Register bit 0 (Data Ready) */
    if ((inb(SERIAL_PORT + 5) & 0x01) == 0)
        return 0;
    return (char)inb(SERIAL_PORT);
}
