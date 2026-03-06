/* ============================================================================
 * stdio.c — Standard I/O functions
 *
 * printf supports: %d, %i, %u, %x, %X, %p, %s, %c, %ld, %li, %lu, %lx, %%
 * ============================================================================ */

#include "stdio.h"
#include "string.h"
#include "syscall.h"

/* --- Character I/O --- */

int putchar(int c)
{
    char ch = (char)c;
    sys_write(STDOUT_FD, &ch, 1);
    return c;
}

int puts(const char *s)
{
    size_t len = strlen(s);
    sys_write(STDOUT_FD, s, len);
    putchar('\n');
    return 0;
}

int getchar(void)
{
    char c;
    long n = sys_read(STDIN_FD, &c, 1);
    if (n <= 0)
        return EOF;
    return (int)(uint8_t)c;
}

/* --- Formatted output engine ---
 * Writes to a buffer (for snprintf) or directly to stdout (for printf). */

struct fmt_state {
    char  *buf;       /* output buffer (NULL for stdout) */
    size_t buf_size;  /* buffer capacity */
    size_t written;   /* characters written */
};

static void fmt_putchar(struct fmt_state *st, char c)
{
    if (st->buf) {
        if (st->written + 1 < st->buf_size)
            st->buf[st->written] = c;
    } else {
        putchar(c);
    }
    st->written++;
}

static void fmt_puts(struct fmt_state *st, const char *s)
{
    while (*s)
        fmt_putchar(st, *s++);
}

static void fmt_putu(struct fmt_state *st, unsigned long val, int base,
                     int uppercase)
{
    char buf[24];
    int i = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        fmt_putchar(st, '0');
        return;
    }

    while (val > 0) {
        buf[i++] = digits[val % (unsigned long)base];
        val /= (unsigned long)base;
    }

    while (i > 0)
        fmt_putchar(st, buf[--i]);
}

static void fmt_putd(struct fmt_state *st, long val)
{
    unsigned long uval;
    if (val < 0) {
        fmt_putchar(st, '-');
        uval = (unsigned long)(-(val + 1)) + 1;
    } else {
        uval = (unsigned long)val;
    }
    fmt_putu(st, uval, 10, 0);
}

static int do_vprintf(struct fmt_state *st, const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            fmt_putchar(st, *fmt++);
            continue;
        }

        fmt++;  /* skip '%' */

        /* Check for 'l' (long) modifier */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i':
            if (is_long)
                fmt_putd(st, va_arg(ap, long));
            else
                fmt_putd(st, (long)va_arg(ap, int));
            break;

        case 'u':
            if (is_long)
                fmt_putu(st, va_arg(ap, unsigned long), 10, 0);
            else
                fmt_putu(st, (unsigned long)va_arg(ap, unsigned int), 10, 0);
            break;

        case 'x':
            if (is_long)
                fmt_putu(st, va_arg(ap, unsigned long), 16, 0);
            else
                fmt_putu(st, (unsigned long)va_arg(ap, unsigned int), 16, 0);
            break;

        case 'X':
            if (is_long)
                fmt_putu(st, va_arg(ap, unsigned long), 16, 1);
            else
                fmt_putu(st, (unsigned long)va_arg(ap, unsigned int), 16, 1);
            break;

        case 'p':
            fmt_puts(st, "0x");
            fmt_putu(st, (unsigned long)va_arg(ap, void *), 16, 0);
            break;

        case 's': {
            const char *s = va_arg(ap, const char *);
            fmt_puts(st, s ? s : "(null)");
            break;
        }

        case 'c':
            fmt_putchar(st, (char)va_arg(ap, int));
            break;

        case '%':
            fmt_putchar(st, '%');
            break;

        case '\0':
            /* Trailing '%' at end of format string */
            goto done;

        default:
            /* Unknown format — print as-is */
            fmt_putchar(st, '%');
            if (is_long)
                fmt_putchar(st, 'l');
            fmt_putchar(st, *fmt);
            break;
        }

        fmt++;
    }

done:
    return (int)st->written;
}

/* --- Public API --- */

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct fmt_state st = { buf, size, 0 };
    int ret = do_vprintf(&st, fmt, ap);

    /* Null-terminate the buffer */
    if (buf && size > 0) {
        size_t term = st.written < size ? st.written : size - 1;
        buf[term] = '\0';
    }

    return ret;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int printf(const char *fmt, ...)
{
    struct fmt_state st = { NULL, 0, 0 };
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = do_vprintf(&st, fmt, ap);
    va_end(ap);
    return ret;
}
