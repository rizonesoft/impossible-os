/* ============================================================================
 * memops_avx512.c -- AVX-512 accelerated memory operations
 *
 * THIS FILE IS COMPILED WITH -mavx512f (separate translation unit).
 * Contains ONLY AVX-512 functions. SSE2/AVX2 fallbacks and dispatch live in
 * memops_sse.c (compiled with -msse2 only) to avoid EVEX-encoded instructions
 * on CPUs without AVX-512 (e.g., QEMU TCG, pre-Skylake-SP).
 *
 * XREF: 02-kernel-core/TODO-09-x86-64-architecture.md S3
 * ============================================================================ */

#include "kernel/mm/memops.h"

/* ---- AVX-512 memcpy: 64-byte vmovdqu64 loop ---- */

void *memcpy_avx512(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i = 0;
    size_t bulk = n & ~(size_t)63;

    for (; i < bulk; i += 64) {
        __asm__ volatile (
            "vmovdqu64 (%[s]), %%zmm0\n\t"
            "vmovdqu64 %%zmm0, (%[d])\n\t"
            :
            : [s] "r"(s + i), [d] "r"(d + i)
            : "memory", "zmm0"
        );
    }

    /* Scalar tail: 0-63 remaining bytes */
    for (; i < n; i++)
        d[i] = s[i];

    __asm__ volatile ("vzeroupper" ::: "memory");
    return dst;
}

/* ---- AVX-512 memset: vpbroadcastb + vmovdqu64 store loop ---- */

void *memset_avx512(void *dst, int val, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    size_t bulk = n >> 6;  /* number of 64-byte blocks */

    if (bulk > 0) {
        /* Build 32-bit fill word with byte repeated 4 times, then
         * vpbroadcastd to ZMM. vpbroadcastd is AVX512F; vpbroadcastb
         * to ZMM requires AVX512BW which may not be present. */
        uint8_t byte = (uint8_t)val;
        uint32_t fill = (uint32_t)byte
                      | ((uint32_t)byte << 8)
                      | ((uint32_t)byte << 16)
                      | ((uint32_t)byte << 24);
        uint8_t *p = d;
        __asm__ volatile (
            "vpbroadcastd %[v], %%zmm0\n\t"
            "1:\n\t"
            "vmovdqu64    %%zmm0, (%[p])\n\t"
            "add          $64, %[p]\n\t"
            "dec          %[cnt]\n\t"
            "jnz          1b\n\t"
            : [p] "+r"(p), [cnt] "+r"(bulk)
            : [v] "r"(fill)
            : "memory", "zmm0", "cc"
        );
    }

    /* Scalar tail: 0-63 remaining bytes */
    {
        size_t i = n & ~(size_t)63;
        for (; i < n; i++)
            d[i] = (uint8_t)val;
    }

    __asm__ volatile ("vzeroupper" ::: "memory");
    return dst;
}
