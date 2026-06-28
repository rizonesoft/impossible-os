/* ============================================================================
 * memops_sse.c -- SSE2 memory operations + SIMD dispatch
 *
 * THIS FILE IS COMPILED WITH -msse2 (NOT -mavx2). This is critical:
 * with -mavx2, the compiler emits VEX-encoded instructions even for
 * scalar code, which crashes on CPUs without AVX (e.g., QEMU TCG).
 *
 * SSE2 functions: 16 bytes/iter using XMM registers.
 * Dispatch: simd_avx2_ok selects AVX2, cpu_has(SSE2) selects SSE2,
 *           else scalar fallback from libc.
 *
 * XREF: 02-kernel-core/TODO-09-x86-64-architecture.md S2
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

/* simd_avx512_ok is set by simd_enable_avx512() after verifying
 * AVX512F + XCR0 bits 5-7 + MPERF/APERF throttle check passes. */
extern int simd_avx512_ok;

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

/* Acquire-load the dispatch flags: the boot self-benchmark (hw_profile.c) can
 * clear these via __ATOMIC_RELEASE after other CPUs are live, so readers must
 * pair with an acquire to make the handoff well-defined (free on x86). */
void *memcpy_fast(void *dst, const void *src, size_t n)
{
    if (__atomic_load_n(&simd_avx512_ok, __ATOMIC_ACQUIRE))
        return memcpy_avx512(dst, src, n);
    if (__atomic_load_n(&simd_avx2_ok, __ATOMIC_ACQUIRE))
        return memcpy_avx(dst, src, n);
    if (cpu_has(CPU_FEATURE_SSE2))
        return memcpy_sse2(dst, src, n);
    return memcpy(dst, src, n);
}

void *memset_fast(void *dst, int val, size_t n)
{
    if (__atomic_load_n(&simd_avx512_ok, __ATOMIC_ACQUIRE))
        return memset_avx512(dst, val, n);
    if (__atomic_load_n(&simd_avx2_ok, __ATOMIC_ACQUIRE))
        return memset_avx(dst, val, n);
    if (cpu_has(CPU_FEATURE_SSE2))
        return memset_sse2(dst, val, n);
    return memset(dst, val, n);
}
