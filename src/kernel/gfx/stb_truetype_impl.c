/* ============================================================================
 * stb_truetype_impl.c — stb_truetype implementation unit
 *
 * THIS FILE IS COMPILED WITH -msse2 (separate from rest of kernel) to
 * support the floating-point math needed by stb_truetype.
 *
 * Redirects all stdlib dependencies to kernel equivalents:
 *   - malloc/free → kmalloc/kfree
 *   - math.h → kmath.h (software float)
 *   - string.h → kernel/string.h
 *   - assert → kernel printk
 * ============================================================================ */

/* --- Redirect memory allocation --- */
#include "kernel/mm/heap.h"
#define STBTT_malloc(x, u)   ((void)(u), kmalloc(x))
#define STBTT_free(x, u)     ((void)(u), kfree(x))

/* --- Redirect math functions --- */
#include "kernel/kmath.h"
#define STBTT_sqrt(x)     kmath_sqrt(x)
#define STBTT_pow(x, y)   kmath_pow(x, y)
#define STBTT_fmod(x, y)  kmath_fmod(x, y)
#define STBTT_cos(x)      kmath_cos(x)
#define STBTT_acos(x)     kmath_acos(x)
#define STBTT_fabs(x)     kmath_fabs(x)
#define STBTT_ifloor(x)   ((int)kmath_floor(x))
#define STBTT_iceil(x)    ((int)kmath_ceil(x))

/* --- Redirect string functions --- */
/* --- String functions for freestanding (GCC -msse2 generates explicit calls) --- */
#ifndef _KERNEL_STRING_FOR_STB_
#define _KERNEL_STRING_FOR_STB_

typedef unsigned long stb_size_t;

/* These may already be defined by other .o files; mark as weak symbols
 * so the linker picks whichever copy it finds. */
__attribute__((weak))
void *memcpy(void *dst, const void *src, stb_size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    stb_size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

__attribute__((weak))
void *memset(void *s, int c, stb_size_t n)
{
    unsigned char *p = (unsigned char *)s;
    stb_size_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

__attribute__((weak))
stb_size_t strlen(const char *s)
{
    stb_size_t len = 0;
    while (s[len]) len++;
    return len;
}

__attribute__((weak))
void *memmove(void *dst, const void *src, stb_size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s2 = (const unsigned char *)src;
    stb_size_t i;
    if (d < s2) {
        for (i = 0; i < n; i++) d[i] = s2[i];
    } else {
        for (i = n; i > 0; i--) d[i-1] = s2[i-1];
    }
    return dst;
}

#endif

#define STBTT_strlen(s)        strlen(s)
#define STBTT_memcpy(d, s, n)  memcpy(d, s, n)
#define STBTT_memset(d, v, n)  memset(d, v, n)

/* --- Redirect assert --- */
#define STBTT_assert(x)  ((void)0)  /* disable asserts in production kernel */

/* --- Include the actual implementation --- */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
