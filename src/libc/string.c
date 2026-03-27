/* ============================================================================
 * string.c -- Freestanding string and memory operations
 *
 * Canonical kernel libc.  All functions are self-contained with no
 * external dependencies.  Compiled as part of the kernel image.
 *
 * XREF: 02-kernel-core/TODO-20-kernel-libraries.md §1
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
