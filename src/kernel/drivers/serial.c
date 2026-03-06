/* ============================================================================
 * serial.c — COM1 serial port driver
 *
 * Extracted from main.c for reuse by printk and other subsystems.
 * ============================================================================ */

#include "serial.h"

#define SERIAL_PORT 0x3F8

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
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0)
        ;
    outb(SERIAL_PORT, (uint8_t)c);
}

void serial_write(const char *str)
{
    while (*str) {
        if (*str == '\n')
            serial_putchar('\r');
        serial_putchar(*str++);
    }
}

char serial_trygetchar(void)
{
    /* Check Line Status Register bit 0 (Data Ready) */
    if ((inb(SERIAL_PORT + 5) & 0x01) == 0)
        return 0;
    return (char)inb(SERIAL_PORT);
}
