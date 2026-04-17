/* ============================================================================
 * gfx_simd_avx512.c -- AVX-512 SIMD acceleration for GFX primitives
 *
 * THIS FILE IS COMPILED WITH -mavx512f (separate translation unit).
 * Contains ONLY AVX-512 functions. Dispatch lives in gfx_simd.c / framebuffer.c
 * (compiled with -msse2) to avoid EVEX-encoded instructions on CPUs without
 * AVX-512 (e.g., QEMU TCG, pre-Skylake-SP bare metal).
 *
 * 512-bit ZMM registers: 16 ARGB pixels per iteration (64 bytes).
 *
 * XREF: 02-kernel-core/TODO-09-x86-64-architecture.md S3
 * ============================================================================ */

#include "gfx_simd.h"
#include "kernel/types.h"

/* ---- AVX-512 throttle burst ----
 *
 * Executes a tight loop of 512-bit operations to trigger frequency
 * throttling on consumer CPUs with AVX-512 thermal governors.
 * Called from simd_enable_avx512() while MPERF/APERF are being sampled.
 *
 * ~1000 iterations of ZMM adds is roughly 10 us at 3 GHz.
 */

void simd_avx512_burst(void)
{
    __asm__ volatile (
        "vpxorq  %%zmm0, %%zmm0, %%zmm0\n\t"
        "vpxorq  %%zmm1, %%zmm1, %%zmm1\n\t"
        "mov     $1000, %%ecx\n\t"
        "1:\n\t"
        "vpaddq  %%zmm0, %%zmm1, %%zmm0\n\t"
        "vpaddq  %%zmm1, %%zmm0, %%zmm1\n\t"
        "dec     %%ecx\n\t"
        "jnz     1b\n\t"
        "vzeroupper\n\t"
        :
        :
        : "ecx", "zmm0", "zmm1", "memory", "cc"
    );
}

/* ---- AVX-512 framebuffer blit: block copy of uint32_t pixels ----
 *
 * 16 pixels (64 bytes) per ZMM iteration with vmovdqu64.
 * vzeroupper at exit to prevent AVX-SSE transition penalties.
 */

void fb_blit_avx512(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    uint32_t i = 0;
    uint32_t aligned = count & ~15u;  /* 16 pixels = 64 bytes per ZMM */

    for (; i < aligned; i += 16) {
        __asm__ volatile (
            "vmovdqu64 (%[s]), %%zmm0\n\t"
            "vmovdqu64 %%zmm0, (%[d])\n\t"
            :
            : [s] "r"(src + i), [d] "r"(dst + i)
            : "memory", "zmm0"
        );
    }

    /* Scalar tail: 0-15 remaining pixels */
    for (; i < count; i++)
        dst[i] = src[i];

    __asm__ volatile ("vzeroupper" ::: "memory");
}

/* ---- AVX-512 framebuffer fill: block fill of uint32_t pixels ----
 *
 * 16 pixels (64 bytes) per ZMM iteration with vpbroadcastd + vmovdqu64.
 * Broadcast once, store in loop.
 */

void fb_fill_avx512(uint32_t *dst, uint32_t val, uint32_t count)
{
    uint32_t blocks = count >> 4;  /* number of 16-pixel (64-byte) blocks */

    if (blocks > 0) {
        uint32_t *p = dst;
        __asm__ volatile (
            "vpbroadcastd %[v], %%zmm0\n\t"
            "1:\n\t"
            "vmovdqu64    %%zmm0, (%[p])\n\t"
            "add          $64, %[p]\n\t"
            "dec          %[cnt]\n\t"
            "jnz          1b\n\t"
            : [p] "+r"(p), [cnt] "+r"(blocks)
            : [v] "r"(val)
            : "memory", "zmm0", "cc"
        );
    }

    /* Scalar tail: 0-15 remaining pixels */
    {
        uint32_t i = count & ~15u;
        for (; i < count; i++)
            dst[i] = val;
    }

    __asm__ volatile ("vzeroupper" ::: "memory");
}
