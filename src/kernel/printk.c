/* ============================================================================
 * printk.c — Kernel printf
 *
 * Outputs formatted text to BOTH the framebuffer console and serial port.
 * Supports: %d, %u, %x, %p, %s, %c, %%
 * ============================================================================ */

#include "kernel/printk.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/framebuffer.h"

/* GCC built-in variadic args (no libc needed) */
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

/* Output a character to both serial and framebuffer */
static void printk_putchar(char c)
{
    if (c == '\n')
        serial_putchar('\r');
    serial_putchar(c);
    fb_putchar(c);
}

static void printk_write(const char *s)
{
    while (*s) {
        printk_putchar(*s++);
    }
}

/* Print unsigned integer in given base */
static void print_uint(uint64_t val, uint32_t base, uint32_t min_digits)
{
    const char digits[] = "0123456789ABCDEF";
    char buf[20];
    int i = 0;

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }

    /* Pad with zeros if needed */
    while (i < (int)min_digits) {
        buf[i++] = '0';
    }

    /* Print in reverse */
    while (i > 0) {
        printk_putchar(buf[--i]);
    }
}

/* Print signed integer */
static void print_int(int64_t val)
{
    if (val < 0) {
        printk_putchar('-');
        print_uint((uint64_t)(-val), 10, 0);
    } else {
        print_uint((uint64_t)val, 10, 0);
    }
}

void printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            printk_putchar(*fmt++);
            continue;
        }

        fmt++; /* skip '%' */

        switch (*fmt) {
        case 'd':
        case 'i':
            print_int(va_arg(ap, int64_t));
            break;

        case 'u':
            print_uint(va_arg(ap, uint64_t), 10, 0);
            break;

        case 'x':
            printk_write("0x");
            print_uint(va_arg(ap, uint64_t), 16, 0);
            break;

        case 'p':
            printk_write("0x");
            print_uint(va_arg(ap, uint64_t), 16, 16);
            break;

        case 's': {
            const char *s = va_arg(ap, const char *);
            printk_write(s ? s : "(null)");
            break;
        }

        case 'c':
            printk_putchar((char)va_arg(ap, int));
            break;

        case '%':
            printk_putchar('%');
            break;

        case '\0':
            goto done;

        default:
            printk_putchar('%');
            printk_putchar(*fmt);
            break;
        }

        fmt++;
    }

done:
    va_end(ap);
}
