/* ============================================================================
 * kernel_main — Minimal kernel entry point
 *
 * Writes "Impossible OS" to the serial port (COM1, 0x3F8) so we can verify
 * the boot pipeline works via QEMU's -serial stdio output.
 * ============================================================================ */

#define SERIAL_PORT 0x3F8

/* Inline assembly helpers for port I/O */
static inline void outb(unsigned short port, unsigned char val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Initialize the serial port (COM1) */
static void serial_init(void)
{
    outb(SERIAL_PORT + 1, 0x00);    /* Disable interrupts */
    outb(SERIAL_PORT + 3, 0x80);    /* Enable DLAB (set baud rate) */
    outb(SERIAL_PORT + 0, 0x03);    /* 38400 baud (divisor = 3) */
    outb(SERIAL_PORT + 1, 0x00);    /*   high byte */
    outb(SERIAL_PORT + 3, 0x03);    /* 8 bits, no parity, 1 stop bit */
    outb(SERIAL_PORT + 2, 0xC7);    /* Enable FIFO, clear, 14-byte threshold */
    outb(SERIAL_PORT + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

/* Write a single character to serial */
static void serial_putchar(char c)
{
    /* Wait until transmit buffer is empty */
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0)
        ;
    outb(SERIAL_PORT, (unsigned char)c);
}

/* Write a string to serial */
static void serial_write(const char *str)
{
    while (*str) {
        if (*str == '\n')
            serial_putchar('\r');
        serial_putchar(*str++);
    }
}

/* Kernel entry point — called from entry.asm */
void kernel_main(unsigned int magic, unsigned int *mbi)
{
    (void)magic;  /* Multiboot2 magic — will validate later */
    (void)mbi;    /* Multiboot2 info pointer — will parse later */

    serial_init();

    serial_write("\n");
    serial_write("================================================\n");
    serial_write("  Impossible OS Kernel Loaded\n");
    serial_write("  Architecture: x86-64 (32-bit stub for now)\n");
    serial_write("  Boot: UEFI via GRUB + Multiboot2\n");
    serial_write("================================================\n");
    serial_write("\n");
    serial_write("[OK] Serial output working\n");
    serial_write("[OK] Build pipeline verified\n");
    serial_write("\n");
    serial_write("Halting.\n");

    /* Halt */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
