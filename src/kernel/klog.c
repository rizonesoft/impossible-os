/* ============================================================================
 * klog.c -- Unified kernel logging
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

/* Write a string to framebuffer only */
static void fb_str(const char *s)
{
    while (*s)
        fb_putchar(*s++);
}

/* ---- Format engine ---- */

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
        fmt++; /* skip '%' */
        if (*fmt == '\0') break;
        if (*fmt == '%') { BUF_PUT('%'); fmt++; continue; }

        /* Parse zero-pad flag */
        int zero_pad = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse length modifier: skip l, ll, h, hh */
        while (*fmt == 'l' || *fmt == 'h') fmt++;

        /* Specifier */
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = va_arg(ap, int64_t);
            char tmp[20]; int n = 0;
            if (v < 0) { BUF_PUT('-'); v = -v; }
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; } }
            while (n < width) { BUF_PUT(zero_pad ? '0' : ' '); width--; }
            while (n > 0) BUF_PUT(tmp[--n]);
            break;
        }
        case 'u': {
            uint64_t v = va_arg(ap, uint64_t);
            char tmp[20]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; } }
            while (n < width) { BUF_PUT(zero_pad ? '0' : ' '); width--; }
            while (n > 0) BUF_PUT(tmp[--n]);
            break;
        }
        case 'x': case 'X': {
            const char *hex = (*fmt == 'X') ? "0123456789ABCDEF"
                                            : "0123456789abcdef";
            uint64_t v = va_arg(ap, uint64_t);
            char tmp[16]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = hex[v & 0xF]; v >>= 4; } }
            while (n < width) { BUF_PUT(zero_pad ? '0' : ' '); width--; }
            while (n > 0) BUF_PUT(tmp[--n]);
            break;
        }
        case 'p': {
            const char hex[] = "0123456789abcdef";
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


/* ---- Level prefixes, ANSI serial colors, and framebuffer colors ---- */

static const char *level_prefix[] = {
    "[INFO] ",   /* LOG_DEBUG */
    "[ OK ] ",   /* LOG_INFO  */
    "[WARN] ",   /* LOG_WARN  */
    "[FAIL] ",   /* LOG_ERROR */
    "[CRIT] ",   /* LOG_FATAL */
};

/* ANSI escape sequences applied to the [LEVEL] token in serial output.
 * Only the token itself is colored; timestamp, subsystem, and message remain
 * default so the eye goes straight to the severity indicator.
 *
 *   LOG_DEBUG  [INFO]  dark-grey   — low-noise background chatter
 *   LOG_INFO   [ OK ]  green       — normal success
 *   LOG_WARN   [WARN]  yellow      — degraded / non-fatal
 *   LOG_ERROR  [FAIL]  red         — recoverable error
 *   LOG_FATAL  [CRIT]  bold+red    — fatal, system halted
 */
#define ANSI_RESET    "\033[0m"
#define ANSI_DGREY    "\033[90m"    /* dark grey  */
#define ANSI_GREEN    "\033[32m"
#define ANSI_YELLOW   "\033[33m"
#define ANSI_RED      "\033[31m"
#define ANSI_BOLD_RED "\033[1;31m"

static const char *level_ansi[] = {
    ANSI_DGREY,     /* LOG_DEBUG  [INFO] */
    ANSI_GREEN,     /* LOG_INFO   [ OK ] */
    ANSI_YELLOW,    /* LOG_WARN   [WARN] */
    ANSI_RED,       /* LOG_ERROR  [FAIL] */
    ANSI_BOLD_RED,  /* LOG_FATAL  [CRIT] */
};

static const uint32_t level_color[] = {
    FB_COLOR_FG_DEFAULT,  /* LOG_DEBUG -- shouldn't reach FB */
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
    va_list ap;
    klog_entry_t *e;

    /* ---- Store formatted message in ring buffer ---- */
    e = &klog_ring[klog_ring_head];
    e->level     = level;
    e->subsystem = subsystem;
    e->timestamp = (uint32_t)system_get_ticks();

    va_start(ap, fmt);
    vformat_buf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);

    klog_ring_head = (klog_ring_head + 1) % KLOG_RING_SIZE;
    if (klog_ring_count < KLOG_RING_SIZE)
        klog_ring_count++;

    /* ---- Build complete serial line in a stack buffer, then write atomically ----
     *
     * Formatting into a buffer first and calling serial_write() ONCE is critical:
     * serial_write() holds the spinlock for the entire string, preventing any
     * IRQ handler from injecting characters between ours. */
    {
        /* Line format: "[  X.XXX] [LEVEL] subsystem: message\n"
         * Max size: 11 (ts) + 7 (level) + 16 (subsys+": ") + 128 (msg) + 2 = 164 */
        char line[256];
        uint32_t pos = 0;

        #define LP(c) do { if (pos < sizeof(line)-1) line[pos++] = (c); } while(0)
        #define LS(s) do { const char *_p = (s); while (*_p && pos < sizeof(line)-1) line[pos++] = *_p++; } while(0)

        /* Timestamp: [  X.XXX] */
        uint64_t ms  = system_get_ticks() * 10;
        uint32_t sec = (uint32_t)(ms / 1000);
        uint32_t fms = (uint32_t)(ms % 1000);

        LP('[');
        {
            char tmp[8]; int n = 0;
            uint32_t v = sec;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; } }
            int pad = 3 - n;
            while (pad-- > 0) LP(' ');
            while (n > 0) LP(tmp[--n]);
        }
        LP('.');
        LP('0' + (char)((fms / 100) % 10));
        LP('0' + (char)((fms /  10) % 10));
        LP('0' + (char)( fms        % 10));
        LP(']'); LP(' ');

        /* Colored level prefix + subsystem + pre-formatted message */
        LS(level_ansi[level]);
        LS(level_prefix[level]);
        LS(ANSI_RESET);
        if (subsystem && subsystem[0]) { LS(subsystem); LS(": "); }
        LS(e->message);
        LP('\n');
        line[pos] = '\0';

        #undef LP
        #undef LS

        serial_write(line);  /* atomic: spinlock held for entire line */
    }

    /* ---- Output to framebuffer if level >= screen threshold ---- */
    if (level >= screen_min_level) {
        fb_set_color(level_color[level], FB_COLOR_BG_DEFAULT);
        fb_str(level_prefix[level]);
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);

        if (subsystem && subsystem[0]) {
            fb_str(subsystem);
            fb_str(": ");
        }
        fb_str(e->message);   /* reuse pre-formatted ring buffer entry */
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
        serial_write("[**] FATAL -- system halted\r\n");
        for (;;)
            __asm__ volatile ("hlt");
    }
}
