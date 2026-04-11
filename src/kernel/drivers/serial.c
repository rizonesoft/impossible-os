/* ============================================================================
 * serial.c -- COM1 serial port driver
 *
 * Extracted from main.c for reuse by printk and other subsystems.
 * ============================================================================ */

#include "kernel/drivers/serial.h"
#include "kernel/boot_info.h"
#include "kernel/sched/spinlock.h"

/* Serial port I/O base -- read from boot_info at serial_init().
 * Default to COM1 (0x3F8) for safety during early klog before init. */
static uint16_t s_serial_port = 0x3F8;

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
    if (!s_serial_port) return;
    while ((inb(s_serial_port + 5) & 0x20) == 0)
        ;
    outb(s_serial_port, (uint8_t)c);
}

void serial_init(void)
{
    uint16_t divisor;

    /* Read the serial port probed by the bootloader (S4/S10).
     * 0 = no UART detected; serial output becomes a no-op.
     * Honor the bootloader result unconditionally -- it already probed
     * SPCR + COM1 + COM2.  Do not default to COM1 after boot handoff. */
    s_serial_port = g_boot_info.serial_port;

    if (!s_serial_port) return;

    outb(s_serial_port + 1, 0x00);    /* Disable interrupts */

    /* Honor SPCR baud rate if available.
     * 0 = firmware-configured (SPCR baud code 0), preserve existing divisor.
     * Only accept known standard rates; anything else keeps existing divisor. */
    {
        uint32_t baud = g_boot_info.serial_baud;
        if (baud == 9600 || baud == 19200 || baud == 38400 ||
            baud == 57600 || baud == 115200) {
            divisor = (uint16_t)(115200 / baud);
        } else if (baud == 0) {
            divisor = 0;  /* preserve firmware-configured divisor */
        } else {
            divisor = 3;  /* unknown rate -- fall back to 38400 */
        }
    }

    if (divisor > 0) {
        outb(s_serial_port + 3, 0x80);    /* Enable DLAB */
        outb(s_serial_port + 0, (uint8_t)(divisor & 0xFF));
        outb(s_serial_port + 1, (uint8_t)((divisor >> 8) & 0xFF));
    }
    outb(s_serial_port + 3, 0x03);    /* 8N1 (also clears DLAB) */
    outb(s_serial_port + 2, 0xC7);    /* FIFO */
    outb(s_serial_port + 4, 0x0B);    /* IRQs, RTS/DSR */
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
    if (!s_serial_port) return 0;
    if ((inb(s_serial_port + 5) & 0x01) == 0)
        return 0;
    return (char)inb(s_serial_port);
}
