/* ============================================================================
 * memops.c -- SIMD-accelerated memory operations
 *
 * THIS FILE IS COMPILED WITH -mavx2 (separate from the rest of the kernel).
 * All SIMD code uses inline assembly to avoid needing <immintrin.h> in
 * freestanding mode.
 *
 * AVX2 functions: 32 bytes/iter using YMM registers, vzeroupper at exit.
 * SSE2 functions: 16 bytes/iter using XMM registers.
 * Dispatch: cpu_has() selects best available path at runtime.
 *
 * XREF: 02-kernel-core/TODO-19-x86-64-architecture.md S2
 * ============================================================================ */

#include "kernel/mm/memops.h"
#include "kernel/cpuid.h"

/* Pull in scalar memcpy/memset from libc for fallback */
extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int c, size_t n);

/* simd_avx2_ok is set by simd_enable_avx() after verifying both
 * cpu_has(CPU_FEATURE_AVX2) AND g_cpu.xcr0_active bit 2 (AVX).
 * Raw CPUID alone is not sufficient: XCR0 must be programmed. */
extern int simd_avx2_ok;

/* ---- AVX2 memcpy: 32-byte vmovdqu loop ---- */

void *memcpy_avx(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i = 0;
    size_t bulk = n & ~(size_t)31;

    for (; i < bulk; i += 32) {
        __asm__ volatile (
            "vmovdqu (%[s]), %%ymm0\n\t"
            "vmovdqu %%ymm0, (%[d])\n\t"
            :
            : [s] "r"(s + i), [d] "r"(d + i)
            : "memory", "ymm0"
        );
    }

    /* Scalar tail: 0-31 remaining bytes */
    for (; i < n; i++)
        d[i] = s[i];

    __asm__ volatile ("vzeroupper" ::: "memory");
    return dst;
}

/* ---- AVX2 memset: vpbroadcastb + vmovdqu store loop ---- */

void *memset_avx(void *dst, int val, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    size_t bulk = n >> 5;  /* number of 32-byte blocks */

    if (bulk > 0) {
        /* Broadcast once, store in internal asm loop. Single block keeps
         * the YMM register lifetime fully visible to the compiler. */
        uint8_t *p = d;
        __asm__ volatile (
            "vmovd       %[v], %%xmm0\n\t"
            "vpbroadcastb %%xmm0, %%ymm0\n\t"
            "1:\n\t"
            "vmovdqu     %%ymm0, (%[p])\n\t"
            "add         $32, %[p]\n\t"
            "dec         %[cnt]\n\t"
            "jnz         1b\n\t"
            : [p] "+r"(p), [cnt] "+r"(bulk)
            : [v] "r"((uint32_t)(uint8_t)val)
            : "memory", "ymm0", "cc"
        );
    }

    /* Scalar tail: 0-31 remaining bytes */
    {
        size_t i = n & ~(size_t)31;
        for (; i < n; i++)
            d[i] = (uint8_t)val;
    }

    __asm__ volatile ("vzeroupper" ::: "memory");
    return dst;
}

/* ---- SSE2 memcpy: 16-byte movdqu loop ---- */

void *memcpy_sse2(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i = 0;
    size_t bulk = n & ~(size_t)15;

    for (; i < bulk; i += 16) {
        __asm__ volatile (
            "movdqu (%[s]), %%xmm0\n\t"
            "movdqu %%xmm0, (%[d])\n\t"
            :
            : [s] "r"(s + i), [d] "r"(d + i)
            : "memory", "xmm0"
        );
    }

    /* Scalar tail: 0-15 remaining bytes */
    for (; i < n; i++)
        d[i] = s[i];

    return dst;
}

/* ---- SSE2 memset: pshufd broadcast + movdqu store loop ---- */

void *memset_sse2(void *dst, int val, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t byte = (uint8_t)val;
    size_t bulk = n >> 4;  /* number of 16-byte blocks */

    /* Build 32-bit value with byte repeated 4 times */
    uint32_t fill = (uint32_t)byte
                  | ((uint32_t)byte << 8)
                  | ((uint32_t)byte << 16)
                  | ((uint32_t)byte << 24);

    if (bulk > 0) {
        /* Broadcast once, store in internal asm loop. */
        uint8_t *p = d;
        __asm__ volatile (
            "movd    %[v], %%xmm0\n\t"
            "pshufd  $0, %%xmm0, %%xmm0\n\t"
            "1:\n\t"
            "movdqu  %%xmm0, (%[p])\n\t"
            "add     $16, %[p]\n\t"
            "dec     %[cnt]\n\t"
            "jnz     1b\n\t"
            : [p] "+r"(p), [cnt] "+r"(bulk)
            : [v] "r"(fill)
            : "memory", "xmm0", "cc"
        );
    }

    /* Scalar tail: 0-15 remaining bytes */
    {
        size_t i = n & ~(size_t)15;
        for (; i < n; i++)
            d[i] = byte;
    }

    return dst;
}

/* ---- Dispatch: select best available path ---- */

void *memcpy_fast(void *dst, const void *src, size_t n)
{
    if (simd_avx2_ok)
        return memcpy_avx(dst, src, n);
    if (cpu_has(CPU_FEATURE_SSE2))
        return memcpy_sse2(dst, src, n);
    return memcpy(dst, src, n);
}

void *memset_fast(void *dst, int val, size_t n)
{
    if (simd_avx2_ok)
        return memset_avx(dst, val, n);
    if (cpu_has(CPU_FEATURE_SSE2))
        return memset_sse2(dst, val, n);
    return memset(dst, val, n);
}
