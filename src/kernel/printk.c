/* ============================================================================
 * printk.c -- Kernel printf
 *
 * Outputs formatted text to BOTH the framebuffer console and serial port.
 * Supports: %d, %u, %x, %p, %s, %c, %%
 *
 * Atomicity contract: printk() formats the entire output into a
 * single bounded buffer and calls serial_write() ONCE per printk()
 * invocation, so the serial log cannot have interleaved partial
 * lines from concurrent klog() / printk() surfaces.  The naive
 * char-at-a-time path lets klog() (which holds the serial spinlock
 * for a full line) interleave between any two characters of a
 * printk() line because per-char serial_putchar() reacquires the
 * lock per byte.
 * ============================================================================ */

#include "kernel/printk.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/framebuffer.h"

/* GCC built-in variadic args (no libc needed) */
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

/* Per-printk()-call line buffer. Sized to match klog() live path
 * (klog.c:934 char line[512]) so any caller migrated from printk()
 * to klog() never sees a smaller capacity. The truncation marker
 * '~' lands at line[511] when the format would have exceeded the
 * buffer, mirroring klog.c:1037. */
#define PRINTK_LINE_MAX 512

typedef struct {
    char  buf[PRINTK_LINE_MAX];
    int   pos;
    int   truncated;
} printk_line_t;

static inline void plnk_putc(printk_line_t *l, char c)
{
    if (l->pos >= PRINTK_LINE_MAX - 1) {
        l->truncated = 1;
        return;
    }
    l->buf[l->pos++] = c;
}

static inline void plnk_puts(printk_line_t *l, const char *s)
{
    while (*s) {
        if (l->pos >= PRINTK_LINE_MAX - 1) {
            l->truncated = 1;
            return;
        }
        l->buf[l->pos++] = *s++;
    }
}

/* Print unsigned integer in given base into the line buffer. */
static void plnk_uint(printk_line_t *l, uint64_t val, uint32_t base, uint32_t min_digits)
{
    const char digits[] = "0123456789ABCDEF";
    char tmp[20];
    int i = 0;

    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = digits[val % base];
            val /= base;
        }
    }
    while (i < (int)min_digits) {
        tmp[i++] = '0';
    }
    while (i > 0) {
        plnk_putc(l, tmp[--i]);
    }
}

static void plnk_int(printk_line_t *l, int64_t val)
{
    if (val < 0) {
        plnk_putc(l, '-');
        plnk_uint(l, (uint64_t)(-val), 10, 0);
    } else {
        plnk_uint(l, (uint64_t)val, 10, 0);
    }
}

void printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    printk_line_t L;
    L.pos = 0;
    L.truncated = 0;

    while (*fmt) {
        if (*fmt != '%') {
            /* Plain newline -- serial_write() inserts \r before every \n
             * itself (see src/kernel/drivers/serial.c:96-98), so DO NOT
             * pre-expand here.  Pre-expansion produces \r\r\n on serial
             * because both surfaces normalize.  fb_write() handles
             * plain \n natively. */
            plnk_putc(&L, *fmt++);
            continue;
        }

        fmt++; /* skip '%' */
        if (*fmt == '\0') break;
        if (*fmt == '%') { plnk_putc(&L, '%'); fmt++; continue; }

        /* Parse zero-pad flag */
        int zero_pad = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        (void)zero_pad;

        /* Parse width */
        uint32_t width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (uint32_t)(*fmt - '0');
            fmt++;
        }

        /* Parse length modifier: skip l, ll, h, hh */
        while (*fmt == 'l' || *fmt == 'h') fmt++;

        /* Specifier */
        switch (*fmt) {
        case 'd':
        case 'i':
            plnk_int(&L, va_arg(ap, int64_t));
            break;
        case 'u':
            plnk_uint(&L, va_arg(ap, uint64_t), 10, width);
            break;
        case 'x':
        case 'X':
            plnk_uint(&L, va_arg(ap, uint64_t), 16, width);
            break;
        case 'p':
            plnk_puts(&L, "0x");
            plnk_uint(&L, va_arg(ap, uint64_t), 16, 16);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            plnk_puts(&L, s ? s : "(null)");
            break;
        }
        case 'c':
            plnk_putc(&L, (char)va_arg(ap, int));
            break;
        case '\0':
            goto done;
        default:
            plnk_putc(&L, '%');
            plnk_putc(&L, *fmt);
            break;
        }

        fmt++;
    }

done:
    va_end(ap);

    /* Truncation marker if the format overflowed the buffer.
     * Reserve space for the marker by overwriting the last byte. */
    if (L.truncated && L.pos >= 1) {
        L.buf[PRINTK_LINE_MAX - 2] = '~';
        L.pos = PRINTK_LINE_MAX - 1;
    }
    L.buf[L.pos] = '\0';

    /* Atomic emit: one lock-protected serial write + one framebuffer
     * write for the whole formatted line. */
    serial_write(L.buf);
    fb_write(L.buf);
}
