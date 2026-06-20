/* ============================================================================
 * string.c -- Freestanding string and memory operations
 *
 * Canonical kernel libc.  All functions are self-contained with no
 * external dependencies.  Compiled as part of the kernel image.
 *
 * XREF: 02-kernel-core/TODO-03-kernel-libraries.md
 * ============================================================================ */

#include "libc/string.h"

/* ---- Memory operations -------------------------------------------------- */

/* Word size for the scalar fast path. The kernel builds -mno-sse/-mno-sse2/
 * -mno-mmx, so this word-at-a-time loop -- not SIMD -- is the common fast path
 * for the aligned page/block/struct copies storage, graphics, IPC, and the
 * parsers all funnel through. Bytewise prefix/tail handle alignment and small
 * sizes; the word loop runs only when src and dst share the same misalignment.
 *
 * memword_t is may_alias: the word loads/stores punned over a byte buffer would
 * otherwise violate strict aliasing and miscompile under -O2 (the kernel does
 * not build -fno-strict-aliasing). */
typedef unsigned long __attribute__((__may_alias__)) memword_t;
#define MEMWORD sizeof(memword_t)

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (n >= MEMWORD && ((((uintptr_t)d ^ (uintptr_t)s) & (MEMWORD - 1)) == 0)) {
        while ((uintptr_t)d & (MEMWORD - 1)) { *d++ = *s++; n--; }
        while (n >= MEMWORD) {
            *(memword_t *)d = *(const memword_t *)s;
            d += MEMWORD; s += MEMWORD; n -= MEMWORD;
        }
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (n == 0 || d == s) return dst;
    if (d < s || d >= s + n) {
        /* Forward copy -- no overlap or dst is before src. */
        if (n >= MEMWORD && ((((uintptr_t)d ^ (uintptr_t)s) & (MEMWORD - 1)) == 0)) {
            while ((uintptr_t)d & (MEMWORD - 1)) { *d++ = *s++; n--; }
            while (n >= MEMWORD) {
                *(memword_t *)d = *(const memword_t *)s;
                d += MEMWORD; s += MEMWORD; n -= MEMWORD;
            }
        }
        while (n--) *d++ = *s++;
    } else {
        /* Reverse copy -- dst overlaps src from behind. */
        d += n; s += n;
        if (n >= MEMWORD && ((((uintptr_t)d ^ (uintptr_t)s) & (MEMWORD - 1)) == 0)) {
            while ((uintptr_t)d & (MEMWORD - 1)) { *--d = *--s; n--; }
            while (n >= MEMWORD) {
                d -= MEMWORD; s -= MEMWORD; n -= MEMWORD;
                *(memword_t *)d = *(const memword_t *)s;
            }
        }
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t b = (uint8_t)c;
    if (n >= MEMWORD) {
        unsigned long w = b;
        for (size_t k = 8; k < MEMWORD * 8; k <<= 1) w |= w << k;   /* fill all bytes */
        while ((uintptr_t)d & (MEMWORD - 1)) { *d++ = b; n--; }
        while (n >= MEMWORD) { *(memword_t *)d = w; d += MEMWORD; n -= MEMWORD; }
    }
    while (n--) *d++ = b;
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
    if (!needle[0]) return (char *)haystack;
    size_t nlen = strlen(needle);
    char first = needle[0];
    /* Skip directly to each occurrence of the first needle char rather than
     * running strncmp at every position; avoids re-scanning runs that cannot
     * begin a match (the bounded-progression form, not full byte-by-byte). */
    for (;;) {
        while (*haystack && *haystack != first) haystack++;
        if (!*haystack) return (char *)0;
        if (strncmp(haystack, needle, nlen) == 0) return (char *)haystack;
        haystack++;
    }
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
    /* BSD contract: inspect AT MOST n bytes of dst (it may be unterminated
     * within an n-sized buffer). strlen(dst) here would overread. */
    size_t dlen = 0;
    while (dlen < n && dst[dlen] != '\0') dlen++;
    size_t slen = strlen(src);
    if (dlen < n) {
        size_t avail = n - dlen - 1;
        size_t copy = (slen < avail) ? slen : avail;
        size_t i;
        for (i = 0; i < copy; i++)
            dst[dlen + i] = src[i];
        dst[dlen + copy] = '\0';
    }
    /* If dst had no NUL within n, dlen == n; BSD returns n + slen (no write). */
    return dlen + slen;
}

/* ---- Numeric conversions ------------------------------------------------ */

/* Freestanding limits (no <limits.h>). `long`/`unsigned long` are 64-bit. */
#define STRTO_ULONG_MAX  0xFFFFFFFFFFFFFFFFUL
#define STRTO_LONG_MAX   0x7FFFFFFFFFFFFFFFL
#define STRTO_LONG_MIN   (-STRTO_LONG_MAX - 1L)
#define IS_HEX(c)  (((c) >= '0' && (c) <= '9') || ((c) >= 'a' && (c) <= 'f') || \
                    ((c) >= 'A' && (c) <= 'F'))

/* Sign-less digit core shared by strtoul/strtol. `s` is positioned AFTER any
 * leading whitespace and sign. Handles base 0 auto-detect + guarded 0x prefix +
 * overflow-clamped accumulation. Sets *endp to the stop position, *overflowp,
 * and *anyp (whether any digit converted). Keeping the sign out of here is what
 * stops strtol -- which consumes its own sign -- from accepting a doubled sign
 * (`++1`, `--1`) when it then delegates here. */
static unsigned long strtoul_core(const char *s, const char **endp, int base,
                                  int *overflowp, int *anyp)
{
    unsigned long val = 0;
    int digit, any = 0, overflow = 0;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && IS_HEX(s[2])) { base = 16; s += 2; }
        else if (s[0] == '0') base = 8;
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && IS_HEX(s[2])) {
        s += 2;
    }

    unsigned long cutoff = STRTO_ULONG_MAX / (unsigned long)base;
    unsigned long cutlim = STRTO_ULONG_MAX % (unsigned long)base;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9')      digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        any = 1;
        if (overflow || val > cutoff ||
            (val == cutoff && (unsigned long)digit > cutlim)) {
            overflow = 1;          /* keep consuming digits, value stays clamped */
            continue;
        }
        val = val * (unsigned long)base + (unsigned long)digit;
    }

    *endp = s;
    *overflowp = overflow;
    *anyp = any;
    return overflow ? STRTO_ULONG_MAX : val;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    const char *start = s;

    if (base != 0 && (base < 2 || base > 36)) {   /* invalid base: no conversion */
        if (end) *end = (char *)s;
        return 0;
    }

    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    /* Optional sign (C strtoul applies unary minus to the unsigned result). */
    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }

    const char *cend; int overflow, any;
    unsigned long val = strtoul_core(s, &cend, base, &overflow, &any);

    /* endptr points at the original start when no digits were converted. */
    if (end) *end = (char *)(any ? cend : start);
    if (!any) return 0;
    /* Overflow returns the ULONG_MAX sentinel regardless of sign -- negating the
     * clamped value would wrap a huge negative input to a tiny number. */
    if (overflow) return STRTO_ULONG_MAX;
    return neg ? ((unsigned long)0 - val) : val;
}

long strtol(const char *s, char **end, int base)
{
    const char *start = s;
    int neg = 0;

    if (base != 0 && (base < 2 || base > 36)) {   /* invalid base: no conversion */
        if (end) *end = (char *)s;
        return 0;
    }

    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    const char *cend; int overflow, any;
    unsigned long uval = strtoul_core(s, &cend, base, &overflow, &any);
    if (!any) {                     /* no digits: nothing converted */
        if (end) *end = (char *)start;
        return 0;
    }
    if (end) *end = (char *)cend;

    /* Clamp to LONG_MIN/LONG_MAX without negating LONG_MIN (which is UB). */
    if (neg) {
        if (overflow || uval > (unsigned long)STRTO_LONG_MAX + 1UL) return STRTO_LONG_MIN;
        if (uval == (unsigned long)STRTO_LONG_MAX + 1UL) return STRTO_LONG_MIN;
        return -(long)uval;
    }
    if (overflow || uval > (unsigned long)STRTO_LONG_MAX) return STRTO_LONG_MAX;
    return (long)uval;
}

int atoi(const char *s)
{
    return (int)strtol(s, (char **)0, 10);
}

/* ---- Formatted output --------------------------------------------------- */

/* Length modifier enum */
enum { LEN_NONE, LEN_L, LEN_LL, LEN_Z, LEN_H, LEN_HH };

/* Field-width / precision cap: bounds untrusted/malformed format fields so a
 * giant width can neither overflow signed int parsing nor spin a huge pad loop. */
#define FMT_FIELD_MAX 4096

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos = 0;    /* current write position */
    int total = 0;     /* total chars that would be written */

    /* Write a char: always count, only store if space. The argument MUST be
     * evaluated unconditionally -- callers pass side-effecting expressions like
     * `tmp[--n]` and `*fmt++`, so guarding evaluation behind the space check
     * would skip the side effect and spin forever once the buffer fills (or when
     * size == 0). Bind to a local first, then store conditionally. */
    #define PUTC(c) do { \
        char _putc_ch = (char)(c); \
        if (size > 0 && pos < size - 1) buf[pos++] = _putc_ch; \
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

        /* Parse width. Cap accumulation at FMT_FIELD_MAX so a hostile/malformed
         * field cannot overflow signed int or trigger an enormous pad loop. */
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                if (width > FMT_FIELD_MAX) width = FMT_FIELD_MAX;
                fmt++;
            }
        }
        /* A negative width (only reachable via `*`) means left-align (C semantics). */
        if (width < 0) { f_left = 1; f_zero = 0; width = (width < -FMT_FIELD_MAX) ? FMT_FIELD_MAX : -width; }
        if (width > FMT_FIELD_MAX) width = FMT_FIELD_MAX;

        /* Parse precision (same overflow cap). */
        int prec = -1;  /* -1 = not specified */
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0');
                    if (prec > FMT_FIELD_MAX) prec = FMT_FIELD_MAX;
                    fmt++;
                }
            }
        }
        if (prec < -1) prec = -1;            /* negative `*` precision == omitted */
        if (prec > FMT_FIELD_MAX) prec = FMT_FIELD_MAX;

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
            else if (length == LEN_L || length == LEN_Z) v = (int64_t)va_arg(ap, long);
            else                      v = (int64_t)va_arg(ap, int);
            if (length == LEN_H) v = (int16_t)v;
            if (length == LEN_HH) v = (int8_t)v;

            char tmp[20]; int n = 0; int neg = 0;
            uint64_t uv;
            /* Two's-complement magnitude: `-v` is UB for INT64_MIN, so derive the
             * magnitude with unsigned arithmetic (well-defined for all v). */
            if (v < 0) { neg = 1; uv = (uint64_t)0 - (uint64_t)v; }
            else { uv = (uint64_t)v; }
            /* C: precision is the minimum digit count; precision 0 with value 0
             * produces no digits at all. */
            if (!(prec == 0 && uv == 0)) {
                if (uv == 0) { tmp[n++] = '0'; }
                else { while (uv > 0) { tmp[n++] = '0' + (char)(uv % 10); uv /= 10; } }
            }
            int zeros = (prec > n) ? (prec - n) : 0;      /* precision leading zeros */
            int zpad = (prec < 0) && f_zero;              /* precision overrides `0` */
            int sign = neg ? 1 : (f_plus ? 1 : (f_space ? 1 : 0));
            int pad = width - n - zeros - sign;
            if (pad < 0) pad = 0;

            if (!f_left && !zpad) { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            if (neg) PUTC('-');
            else if (f_plus) PUTC('+');
            else if (f_space) PUTC(' ');
            if (!f_left && zpad) { int j; for (j = 0; j < pad; j++) PUTC('0'); }
            { int j; for (j = 0; j < zeros; j++) PUTC('0'); }
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
            if (!(prec == 0 && v == 0)) {
                if (v == 0) { tmp[n++] = '0'; }
                else { while (v > 0) { tmp[n++] = digits[v % (uint64_t)base_val]; v /= (uint64_t)base_val; } }
            }
            int zeros = (prec > n) ? (prec - n) : 0;      /* precision leading zeros */
            int zpad = (prec < 0) && f_zero;              /* precision overrides `0` */
            int pad = width - n - zeros;
            if (pad < 0) pad = 0;

            if (!f_left && !zpad) { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            if (!f_left && zpad)  { int j; for (j = 0; j < pad; j++) PUTC('0'); }
            { int j; for (j = 0; j < zeros; j++) PUTC('0'); }
            while (n > 0) PUTC(tmp[--n]);
            if (f_left) { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            const char *hex = "0123456789abcdef";
            char tmp[16]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = hex[v & 0xF]; v >>= 4; } }
            int pad = width - 2 - n;          /* width counts the "0x" prefix */
            if (pad < 0) pad = 0;
            if (!f_left && !f_zero) { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            PUTC('0'); PUTC('x');
            if (!f_left && f_zero)  { int j; for (j = 0; j < pad; j++) PUTC('0'); }
            while (n > 0) PUTC(tmp[--n]);
            if (f_left) { int j; for (j = 0; j < pad; j++) PUTC(' '); }
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            /* Bound the scan by precision: `%.Ns` must not read past N bytes even
             * when the source buffer is not NUL-terminated within N. */
            if (prec >= 0) { while (slen < prec && s[slen]) slen++; }
            else           { while (s[slen]) slen++; }

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
