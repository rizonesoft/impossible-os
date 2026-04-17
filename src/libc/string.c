/* ============================================================================
 * string.c -- Freestanding string and memory operations
 *
 * Canonical kernel libc.  All functions are self-contained with no
 * external dependencies.  Compiled as part of the kernel image.
 *
 * XREF: 02-kernel-core/TODO-03-kernel-libraries.md §1
 * ============================================================================ */

#include "libc/string.h"

/* ---- Memory operations -------------------------------------------------- */

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    if (d < s || d >= s + n) {
        /* Forward copy -- no overlap or dst is before src */
        for (i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        /* Reverse copy -- dst overlaps src from behind */
        for (i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    size_t i;
    for (i = 0; i < n; i++)
        d[i] = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    size_t i;
    for (i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] == (uint8_t)c)
            return (void *)(p + i);
    }
    return (void *)0;
}

/* ---- String operations -------------------------------------------------- */

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst;
    size_t i;
    while (*d) d++;
    for (i = 0; i < n && src[i]; i++)
        d[i] = src[i];
    d[i] = '\0';
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)0;
}

char *strrchr(const char *s, int c)
{
    const char *last = (void *)0;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t nlen;
    if (!needle[0]) return (char *)haystack;
    nlen = strlen(needle);
    while (*haystack) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return (char *)0;
}

/* ---- BSD safe string operations ----------------------------------------- */

size_t strlcpy(char *dst, const char *src, size_t n)
{
    size_t slen = strlen(src);
    if (n > 0) {
        size_t copy = (slen < n - 1) ? slen : n - 1;
        size_t i;
        for (i = 0; i < copy; i++)
            dst[i] = src[i];
        dst[copy] = '\0';
    }
    return slen;
}

size_t strlcat(char *dst, const char *src, size_t n)
{
    size_t dlen = strlen(dst);
    size_t slen = strlen(src);
    if (dlen < n) {
        size_t avail = n - dlen - 1;
        size_t copy = (slen < avail) ? slen : avail;
        size_t i;
        for (i = 0; i < copy; i++)
            dst[dlen + i] = src[i];
        dst[dlen + copy] = '\0';
    }
    return dlen + slen;
}

/* ---- Numeric conversions ------------------------------------------------ */

unsigned long strtoul(const char *s, char **end, int base)
{
    unsigned long val = 0;
    int digit;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    /* Auto-detect base */
    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else { base = 8; }
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            digit = *s - 'A' + 10;
        else
            break;
        if (digit >= base) break;
        val = val * (unsigned long)base + (unsigned long)digit;
        s++;
    }

    if (end) *end = (char *)s;
    return val;
}

long strtol(const char *s, char **end, int base)
{
    int neg = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    unsigned long uval = strtoul(s, end, base);
    return neg ? -(long)uval : (long)uval;
}

int atoi(const char *s)
{
    return (int)strtol(s, (char **)0, 10);
}

/* ---- Formatted output --------------------------------------------------- */

/* Length modifier enum */
enum { LEN_NONE, LEN_L, LEN_LL, LEN_Z, LEN_H, LEN_HH };

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos = 0;    /* current write position */
    int total = 0;     /* total chars that would be written */

    /* Write a char: always count, only store if space */
    #define PUTC(c) do { \
        if (pos < size - 1 && size > 0) buf[pos++] = (c); \
        total++; \
    } while (0)

    while (*fmt) {
        if (*fmt != '%') { PUTC(*fmt++); continue; }
        fmt++; /* skip '%' */
        if (*fmt == '\0') break;
        if (*fmt == '%') { PUTC('%'); fmt++; continue; }

        /* Parse flags */
        int f_zero = 0, f_left = 0, f_plus = 0, f_space = 0;
        for (;;) {
            if (*fmt == '0')      { f_zero = 1; fmt++; }
            else if (*fmt == '-') { f_left = 1; fmt++; }
            else if (*fmt == '+') { f_plus = 1; fmt++; }
            else if (*fmt == ' ') { f_space = 1; fmt++; }
            else break;
        }
        if (f_left) f_zero = 0;  /* left-align overrides zero-pad */

        /* Parse width */
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else { while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; } }

        /* Parse precision */
        int prec = -1;  /* -1 = not specified */
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else { while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; } }
        }

        /* Parse length modifier */
        int length = LEN_NONE;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { length = LEN_LL; fmt++; }
            else { length = LEN_L; }
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') { length = LEN_HH; fmt++; }
            else { length = LEN_H; }
        } else if (*fmt == 'z') {
            length = LEN_Z; fmt++;
        }

        /* Specifier */
        char spec = *fmt;
        if (spec == '\0') break;
        fmt++;

        switch (spec) {
        case 'd': case 'i': {
            int64_t v;
            if (length == LEN_LL)     v = va_arg(ap, int64_t);
            else if (length == LEN_L) v = (int64_t)va_arg(ap, long);
            else                      v = (int64_t)va_arg(ap, int);
            if (length == LEN_H) v = (int16_t)v;
            if (length == LEN_HH) v = (int8_t)v;

            char tmp[20]; int n = 0; int neg = 0;
            uint64_t uv;
            if (v < 0) { neg = 1; uv = (uint64_t)(-v); }
            else { uv = (uint64_t)v; }
            if (uv == 0) { tmp[n++] = '0'; }
            else { while (uv > 0) { tmp[n++] = '0' + (char)(uv % 10); uv /= 10; } }

            int sign = neg ? 1 : (f_plus ? 1 : (f_space ? 1 : 0));
            int pad = width - n - sign;
            if (pad < 0) pad = 0;

            if (!f_left && !f_zero) { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            if (neg) PUTC('-');
            else if (f_plus) PUTC('+');
            else if (f_space) PUTC(' ');
            if (!f_left && f_zero) { int j; for (j = 0; j < pad; j++) PUTC('0'); }
            while (n > 0) PUTC(tmp[--n]);
            if (f_left) { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v;
            if (length == LEN_LL)     v = va_arg(ap, uint64_t);
            else if (length == LEN_L || length == LEN_Z) v = (uint64_t)va_arg(ap, unsigned long);
            else                      v = (uint64_t)va_arg(ap, unsigned int);
            if (length == LEN_H) v = (uint16_t)v;
            if (length == LEN_HH) v = (uint8_t)v;

            int base_val = (spec == 'o') ? 8 : ((spec == 'x' || spec == 'X') ? 16 : 10);
            const char *digits = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[22]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = digits[v % (uint64_t)base_val]; v /= (uint64_t)base_val; } }

            int pad = width - n;
            if (pad < 0) pad = 0;

            if (!f_left && !f_zero) { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            if (!f_left && f_zero)  { int j; for (j = 0; j < pad; j++) PUTC('0'); }
            while (n > 0) PUTC(tmp[--n]);
            if (f_left) { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            const char *hex = "0123456789abcdef";
            PUTC('0'); PUTC('x');
            int started = 0;
            int sh;
            for (sh = 60; sh >= 0; sh -= 4) {
                int d = (int)((v >> sh) & 0xF);
                if (d || started || sh == 0) { PUTC(hex[d]); started = 1; }
            }
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            while (s[slen]) slen++;
            if (prec >= 0 && slen > prec) slen = prec;

            int pad = width - slen;
            if (pad < 0) pad = 0;

            if (!f_left) { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            { int j; for (j = 0; j < slen; j++) PUTC(s[j]); }
            if (f_left)  { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            break;
        }
        case 'c':
            PUTC((char)va_arg(ap, int));
            break;
        default:
            PUTC('%');
            PUTC(spec);
            break;
        }
    }

    #undef PUTC

    /* Always NUL-terminate */
    if (size > 0)
        buf[pos < size ? pos : size - 1] = '\0';

    return total;
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
