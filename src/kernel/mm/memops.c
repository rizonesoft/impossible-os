/* ============================================================================
 * memops.c -- AVX2-accelerated memory operations
 *
 * THIS FILE IS COMPILED WITH -mavx2 (separate from the rest of the kernel).
 * Contains ONLY AVX2 functions. SSE2 fallbacks and dispatch live in
 * memops_sse.c (compiled with -msse2 only) to avoid VEX-encoded instructions
 * on CPUs without AVX support (e.g., QEMU TCG).
 *
 * XREF: 02-kernel-core/TODO-19-x86-64-architecture.md S2
 * ============================================================================ */

#include "kernel/mm/memops.h"
#include "kernel/cpuid.h"

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
