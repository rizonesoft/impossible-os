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

/* --- String/memory functions provided by libc/string.c --- */
#include "libc/string.h"

#define STBTT_strlen(s)        strlen(s)
#define STBTT_memcpy(d, s, n)  memcpy(d, s, n)
#define STBTT_memset(d, v, n)  memset(d, v, n)

/* --- Redirect assert --- */
#define STBTT_assert(x)  ((void)0)  /* disable asserts in production kernel */

/* --- Include the actual implementation --- */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
