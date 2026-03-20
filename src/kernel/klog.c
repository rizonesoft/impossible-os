/* ============================================================================
 * klog.c — Unified kernel logging
 *
 * Outputs formatted messages with level prefix and subsystem tag.
 * LOG_DEBUG goes to serial only; LOG_INFO and above go to both
 * serial and framebuffer.
 *
 * Ring buffer stores the last KLOG_RING_SIZE entries for the debug console.
 * ============================================================================ */

#include "kernel/klog.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/timer.h"


/* GCC built-in variadic args (no libc needed) */
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

/* ---- Configuration ---- */

static log_level_t screen_min_level = LOG_INFO;

/* ---- Ring buffer ---- */

static klog_entry_t klog_ring[KLOG_RING_SIZE];
static uint32_t     klog_ring_head = 0;
static uint32_t     klog_ring_count = 0;

/* ---- Output helpers ---- */

/* Write a character to serial only */
static void serial_char(char c)
{
    if (c == '\n')
        serial_putchar('\r');
    serial_putchar(c);
}

/* Write a string to serial only */
static void serial_str(const char *s)
{
    while (*s)
        serial_char(*s++);
}

/* Write a character to framebuffer only */
static void fb_char(char c)
{
    fb_putchar(c);
}

/* Write a string to framebuffer only */
static void fb_str(const char *s)
{
    while (*s)
        fb_putchar(*s++);
}

/* ---- Number formatting ---- */

static void emit_uint(void (*put)(char), uint64_t val, uint32_t base,
                       uint32_t min_digits)
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
        put(buf[--i]);
}

static void emit_int(void (*put)(char), int64_t val)
{
    if (val < 0) {
        put('-');
        emit_uint(put, (uint64_t)(-val), 10, 0);
    } else {
        emit_uint(put, (uint64_t)val, 10, 0);
    }
}

/* ---- Format engine ---- */

/* Format into a fixed buffer (for ring buffer storage) */
static uint32_t vformat_buf(char *buf, uint32_t bufsize, const char *fmt,
                             va_list ap)
{
    uint32_t pos = 0;

    #define BUF_PUT(c) do { if (pos < bufsize - 1) buf[pos++] = (c); } while(0)

    while (*fmt) {
        if (*fmt != '%') {
            BUF_PUT(*fmt++);
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = va_arg(ap, int64_t);
            char tmp[20]; int n = 0;
            if (v < 0) { BUF_PUT('-'); v = -v; }
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; } }
            while (n > 0) BUF_PUT(tmp[--n]);
            break;
        }
        case 'u': {
            uint64_t v = va_arg(ap, uint64_t);
            char tmp[20]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; } }
            while (n > 0) BUF_PUT(tmp[--n]);
            break;
        }
        case 'x': {
            const char hex[] = "0123456789ABCDEF";
            uint64_t v = va_arg(ap, uint64_t);
            BUF_PUT('0'); BUF_PUT('x');
            char tmp[16]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = hex[v & 0xF]; v >>= 4; } }
            while (n > 0) BUF_PUT(tmp[--n]);
            break;
        }
        case 'p': {
            const char hex[] = "0123456789ABCDEF";
            uint64_t v = va_arg(ap, uint64_t);
            BUF_PUT('0'); BUF_PUT('x');
            for (int sh = 60; sh >= 0; sh -= 4)
                BUF_PUT(hex[(v >> sh) & 0xF]);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) BUF_PUT(*s++);
            break;
        }
        case 'c':
            BUF_PUT((char)va_arg(ap, int));
            break;
        case '%':
            BUF_PUT('%');
            break;
        case '\0':
            goto done;
        default:
            BUF_PUT('%');
            BUF_PUT(*fmt);
            break;
        }
        fmt++;
    }
done:
    buf[pos] = '\0';
    #undef BUF_PUT
    return pos;
}

/* Output formatted text to a put-char function */
static void vformat_emit(void (*put)(char), void (*putstr)(const char *),
                          const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            put(*fmt++);
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 'd': case 'i':
            emit_int(put, va_arg(ap, int64_t));
            break;
        case 'u':
            emit_uint(put, va_arg(ap, uint64_t), 10, 0);
            break;
        case 'x':
            putstr("0x");
            emit_uint(put, va_arg(ap, uint64_t), 16, 0);
            break;
        case 'p':
            putstr("0x");
            emit_uint(put, va_arg(ap, uint64_t), 16, 16);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            putstr(s ? s : "(null)");
            break;
        }
        case 'c':
            put((char)va_arg(ap, int));
            break;
        case '%':
            put('%');
            break;
        case '\0':
            return;
        default:
            put('%');
            put(*fmt);
            break;
        }
        fmt++;
    }
}

/* ---- Level prefixes and colors ---- */

static const char *level_prefix[] = {
    "[..] ",   /* LOG_DEBUG */
    "[OK] ",   /* LOG_INFO  */
    "[--] ",   /* LOG_WARN  */
    "[!!] ",   /* LOG_ERROR */
    "[**] ",   /* LOG_FATAL */
};

static const uint32_t level_color[] = {
    FB_COLOR_FG_DEFAULT,  /* LOG_DEBUG — shouldn't reach FB */
    FB_COLOR_GREEN,       /* LOG_INFO  */
    FB_COLOR_YELLOW,      /* LOG_WARN  */
    FB_COLOR_RED,         /* LOG_ERROR */
    FB_COLOR_RED,         /* LOG_FATAL */
};

/* ---- Public API ---- */

void klog_set_screen_level(log_level_t min_level)
{
    screen_min_level = min_level;
}

const klog_entry_t *klog_get_ring(uint32_t *out_count, uint32_t *out_head)
{
    if (out_count) *out_count = klog_ring_count;
    if (out_head)  *out_head  = klog_ring_head;
    return klog_ring;
}

void klog(log_level_t level, const char *subsystem, const char *fmt, ...)
{
    va_list ap, ap2;

    /* ---- Store in ring buffer ---- */
    {
        klog_entry_t *e = &klog_ring[klog_ring_head];
        e->level = level;
        e->subsystem = subsystem;
        e->timestamp = (uint32_t)system_get_ticks();

        va_start(ap, fmt);
        vformat_buf(e->message, sizeof(e->message), fmt, ap);
        va_end(ap);

        klog_ring_head = (klog_ring_head + 1) % KLOG_RING_SIZE;
        if (klog_ring_count < KLOG_RING_SIZE)
            klog_ring_count++;
    }

    /* ---- Always output to serial ---- */
    {
        /* Timestamp: [  1.234]  (seconds space-padded to 4, ms zero-padded to 3)
         * PIT is 100 Hz → 1 tick = 10 ms.  Before pit_init() ticks = 0. */
        uint64_t ms  = system_get_ticks() * 10;
        uint32_t sec = (uint32_t)(ms / 1000);
        uint32_t fms = (uint32_t)(ms % 1000);

        /* emit '[' + space-padded seconds + '.' + zero-padded ms + '] ' */
        serial_char('[');
        {
            char secs[12]; int n = 0;
            uint32_t v = sec;
            if (v == 0) { secs[n++] = '0'; }
            else { while (v > 0) { secs[n++] = '0' + (char)(v % 10); v /= 10; } }
            int pad = 3 - n;
            while (pad-- > 0) serial_char(' ');
            while (n > 0) serial_char(secs[--n]);
        }
        serial_char('.');
        emit_uint(serial_char, (uint64_t)fms, 10, 3);
        serial_char(']');
        serial_char(' ');

        serial_str(level_prefix[level]);
        if (subsystem && subsystem[0]) {
            serial_str(subsystem);
            serial_str(": ");
        }
        va_start(ap2, fmt);
        vformat_emit(serial_char, serial_str, fmt, ap2);
        va_end(ap2);
        serial_char('\n');
    }

    /* ---- Output to framebuffer if level >= screen threshold ---- */
    if (level >= screen_min_level) {
        /* Colored prefix */
        fb_set_color(level_color[level], FB_COLOR_BG_DEFAULT);
        fb_str(level_prefix[level]);
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);

        /* Subsystem tag */
        if (subsystem && subsystem[0]) {
            fb_str(subsystem);
            fb_str(": ");
        }

        /* Formatted message (framebuffer only — serial was done above) */
        va_start(ap, fmt);
        vformat_emit(fb_char, fb_str, fmt, ap);
        va_end(ap);
        fb_putchar('\n');
    }

    /* ---- Live debug log: write to X:\BOOT_NNN.LOG immediately ---- */
    if (klog_disk_live_active()) {
        klog_entry_t *last = &klog_ring[(klog_ring_head == 0
            ? KLOG_RING_SIZE - 1 : klog_ring_head - 1)];
        klog_disk_append(last);
        klog_disk_flush();
    }

    /* ---- FATAL: halt ---- */
    if (level == LOG_FATAL) {
        serial_str("[**] FATAL — system halted\r\n");
        for (;;)
            __asm__ volatile ("hlt");
    }
}
