/* ============================================================================
 * log.c — Kernel logging subsystem
 *
 * Outputs structured log messages to the serial port (COM1) only.
 * Each message includes:  [LEVEL][SUBSYS] formatted message\n
 *
 * This module does NOT use printk() — it writes directly to serial to avoid
 * polluting the framebuffer when the desktop compositor is active.
 * ============================================================================ */

#include "kernel/log.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/pit.h"

/* GCC built-in variadic args (no libc needed) */
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

/* ---- Internal helpers ---- */

/* Write a single character to serial with \r\n translation */
static void log_putchar(char c)
{
    if (c == '\n')
        serial_putchar('\r');
    serial_putchar(c);
}

/* Write a null-terminated string to serial */
static void log_puts(const char *s)
{
    while (*s)
        log_putchar(*s++);
}

/* Print unsigned integer in the given base */
static void log_print_uint(uint64_t val, uint32_t base, uint32_t min_digits)
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

    while (i < (int)min_digits)
        buf[i++] = '0';

    while (i > 0)
        log_putchar(buf[--i]);
}

/* Print signed integer */
static void log_print_int(int64_t val)
{
    if (val < 0) {
        log_putchar('-');
        log_print_uint((uint64_t)(-val), 10, 0);
    } else {
        log_print_uint((uint64_t)val, 10, 0);
    }
}

/* Minimal printf-style formatter writing to serial only */
static void log_vprintf(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            log_putchar(*fmt++);
            continue;
        }

        fmt++;  /* skip '%' */

        switch (*fmt) {
        case 'd':
        case 'i':
            log_print_int(va_arg(ap, int64_t));
            break;

        case 'u':
            log_print_uint(va_arg(ap, uint64_t), 10, 0);
            break;

        case 'x':
            log_puts("0x");
            log_print_uint(va_arg(ap, uint64_t), 16, 0);
            break;

        case 'p':
            log_puts("0x");
            log_print_uint(va_arg(ap, uint64_t), 16, 16);
            break;

        case 's': {
            const char *s = va_arg(ap, const char *);
            log_puts(s ? s : "(null)");
            break;
        }

        case 'c':
            log_putchar((char)va_arg(ap, int));
            break;

        case '%':
            log_putchar('%');
            break;

        case '\0':
            return;

        default:
            log_putchar('%');
            log_putchar(*fmt);
            break;
        }

        fmt++;
    }
}

/* Print the uptime timestamp: [HHH:MM:SS] */
static void log_print_timestamp(void)
{
    uint64_t secs = uptime();
    uint32_t hrs  = (uint32_t)(secs / 3600);
    uint32_t mins = (uint32_t)((secs % 3600) / 60);
    uint32_t s    = (uint32_t)(secs % 60);

    log_putchar('[');
    log_print_uint(hrs, 10, 3);
    log_putchar(':');
    log_print_uint(mins, 10, 2);
    log_putchar(':');
    log_print_uint(s, 10, 2);
    log_putchar(']');
}

/* Print the severity tag and subsystem */
static void log_print_prefix(const char *level, const char *subsystem)
{
    log_print_timestamp();
    log_putchar('[');
    log_puts(level);
    log_putchar(']');

    if (subsystem && subsystem[0]) {
        log_putchar('[');
        log_puts(subsystem);
        log_putchar(']');
    }

    log_putchar(' ');
}

/* ---- Initialization ---- */

void log_init(void)
{
    /* Serial port must already be initialized via serial_init() */
    log_puts("\n--- Impossible OS Kernel Log ---\n");
    log_print_prefix("INFO ", "LOG");
    log_puts("Logging subsystem initialized\n");
}

/* ---- Public API ---- */

void log_info(const char *subsystem, const char *fmt, ...)
{
#if LOG_MIN_LEVEL <= LOG_LEVEL_INFO
    va_list ap;
    va_start(ap, fmt);
    log_print_prefix("INFO ", subsystem);
    log_vprintf(fmt, ap);
    log_putchar('\n');
    va_end(ap);
#else
    (void)subsystem;
    (void)fmt;
#endif
}

void log_warn(const char *subsystem, const char *fmt, ...)
{
#if LOG_MIN_LEVEL <= LOG_LEVEL_WARN
    va_list ap;
    va_start(ap, fmt);
    log_print_prefix("WARN ", subsystem);
    log_vprintf(fmt, ap);
    log_putchar('\n');
    va_end(ap);
#else
    (void)subsystem;
    (void)fmt;
#endif
}

void log_error(const char *subsystem, const char *fmt, ...)
{
#if LOG_MIN_LEVEL <= LOG_LEVEL_ERROR
    va_list ap;
    va_start(ap, fmt);
    log_print_prefix("ERROR", subsystem);
    log_vprintf(fmt, ap);
    log_putchar('\n');
    va_end(ap);
#else
    (void)subsystem;
    (void)fmt;
#endif
}
