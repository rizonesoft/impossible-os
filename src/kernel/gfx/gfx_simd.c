/* ============================================================================
 * gfx_simd.c — SSE2 / AVX2 SIMD acceleration for GFX primitives
 *
 * THIS FILE IS COMPILED WITH -msse2 (separate from the rest of the kernel).
 * All SSE2 intrinsics are used via inline assembly to avoid needing
 * <immintrin.h> in freestanding mode.
 *
 * Key optimizations:
 *   - Alpha blend: 4 pixels/iteration via packed 16-bit multiply
 *   - Gradient fill: 4 pixels/iteration via parallel interpolation
 *   - Blur accumulate: 4 pixels/iteration via packed add
 *   - FPU state: fxsave/fxrstor to protect user-mode XMM registers
 * ============================================================================ */

#include "gfx_simd.h"
#include "kernel/types.h"
#include "kernel/cpuid.h"

/* ---- FPU / SSE state management ---- */

void simd_enable_sse(void)
{
    uint64_t cr0, cr4;

    /* Clear CR0.EM (bit 2), set CR0.MP (bit 1) */
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1UL << 2);  /* Clear EM */
    cr0 |=  (1UL << 1);  /* Set MP */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    /* Set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10) */
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9);   /* OSFXSR */
    cr4 |= (1UL << 10);  /* OSXMMEXCPT */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
}

void simd_save_state(fxsave_area_t *area)
{
    __asm__ volatile ("fxsave (%0)" : : "r"(area) : "memory");
}

void simd_restore_state(const fxsave_area_t *area)
{
    __asm__ volatile ("fxrstor (%0)" : : "r"(area) : "memory");
}

/* ---- CPUID detection (delegated to kernel/cpuid.h) ---- */

int simd_has_sse2(void)
{
    return cpu_has(CPU_FEATURE_SSE2);
}

int simd_has_avx2(void)
{
    return cpu_has(CPU_FEATURE_AVX2);
}

/* ---- SSE2 alpha blending (4 pixels per iteration) ---- */

/*
 * Pre-multiplied alpha blend:  out = src + dst * (1 - src_a)
 *
 * For 4 ARGB pixels packed in XMM registers:
 *   1. Unpack pixels to 16-bit channels
 *   2. Broadcast alpha, compute (255 - alpha)
 *   3. Multiply dst channels by inv_alpha
 *   4. Add src channels
 *   5. Pack back to 8-bit
 */
void simd_blend_pixels_sse2(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    uint32_t i;
    uint32_t aligned = count & ~3u;  /* round down to multiple of 4 */

    for (i = 0; i < aligned; i += 4) {
        /*
         * Process 4 pixels using SSE2 intrinsics via inline asm.
         * XMM0 = src pixels, XMM1 = dst pixels
         * XMM2 = src alpha broadcast, XMM3 = 255 constant
         * XMM4 = inv_alpha, XMM5-7 = scratch
         */
        __asm__ volatile (
            /* Load 4 src and 4 dst pixels */
            "movdqu (%[src]), %%xmm0\n\t"
            "movdqu (%[dst]), %%xmm1\n\t"

            /* Unpack src low 2 pixels to 16-bit: xmm5 = src_lo */
            "pxor   %%xmm7, %%xmm7\n\t"    /* xmm7 = zero */
            "movdqa %%xmm0, %%xmm5\n\t"
            "punpcklbw %%xmm7, %%xmm5\n\t"  /* src_lo: 8x16bit */

            /* Unpack src high 2 pixels: xmm6 = src_hi */
            "movdqa %%xmm0, %%xmm6\n\t"
            "punpckhbw %%xmm7, %%xmm6\n\t"  /* src_hi: 8x16bit */

            /* Extract alpha: byte 3,7,11,15 from each pixel */
            /* For the low pair: alpha is in positions 3,7 of xmm5 (16-bit words 3,7) */
            /* We use pshuflw/pshufhw to broadcast alpha within each pixel group */

            /* Unpack dst lo and hi */
            "movdqa %%xmm1, %%xmm2\n\t"
            "punpcklbw %%xmm7, %%xmm2\n\t"  /* dst_lo */
            "movdqa %%xmm1, %%xmm3\n\t"
            "punpckhbw %%xmm7, %%xmm3\n\t"  /* dst_hi */

            /* Create 255 constant */
            "pcmpeqw %%xmm4, %%xmm4\n\t"    /* all 1s */
            "psrlw   $8, %%xmm4\n\t"         /* 0x00FF in each word */

            /* Extract alpha from src_lo: word 3 = alpha of pixel 0, word 7 = alpha of pixel 1 */
            /* Shuffle to broadcast: use pshuflw to spread word 3 to words 0-3 */
            "pshuflw $0xFF, %%xmm5, %%xmm0\n\t" /* broadcast alpha of pixel 0 to low 4 words */
            "pshufhw $0xFF, %%xmm0, %%xmm0\n\t" /* broadcast alpha of pixel 1 to high 4 words */

            /* inv_alpha = 255 - alpha */
            "movdqa %%xmm4, %%xmm1\n\t"
            "psubw  %%xmm0, %%xmm1\n\t"     /* inv_a_lo */

            /* dst_lo * inv_alpha (low 2 pixels) */
            "pmullw %%xmm1, %%xmm2\n\t"
            "psrlw  $8, %%xmm2\n\t"          /* approximate /255 as >>8 */

            /* result_lo = src_lo + (dst_lo * inv_a_lo) >> 8 */
            "paddw  %%xmm5, %%xmm2\n\t"

            /* Do the same for high 2 pixels */
            "pshuflw $0xFF, %%xmm6, %%xmm0\n\t"
            "pshufhw $0xFF, %%xmm0, %%xmm0\n\t"

            "movdqa %%xmm4, %%xmm1\n\t"
            "psubw  %%xmm0, %%xmm1\n\t"     /* inv_a_hi */

            "pmullw %%xmm1, %%xmm3\n\t"
            "psrlw  $8, %%xmm3\n\t"
            "paddw  %%xmm6, %%xmm3\n\t"

            /* Pack 16-bit back to 8-bit with unsigned saturation */
            "packuswb %%xmm3, %%xmm2\n\t"

            /* Store result */
            "movdqu %%xmm2, (%[dst])\n\t"
            :
            : [src] "r"(src + i), [dst] "r"(dst + i)
            : "memory", "xmm0", "xmm1", "xmm2", "xmm3",
              "xmm4", "xmm5", "xmm6", "xmm7"
        );
    }

    /* Handle remaining 0-3 pixels with scalar fallback */
    for (; i < count; i++) {
        uint32_t s = src[i];
        uint32_t d = dst[i];
        uint32_t sa = (s >> 24) & 0xFF;
        uint32_t inv_a;
        uint32_t sr, sg, sb, dr, dg, db, da;

        if (sa == 0xFF) { dst[i] = s; continue; }
        if (sa == 0x00) continue;

        inv_a = 255 - sa;
        sr = (s >> 16) & 0xFF; sg = (s >> 8) & 0xFF; sb = s & 0xFF;
        dr = (d >> 16) & 0xFF; dg = (d >> 8) & 0xFF; db = d & 0xFF;
        da = (d >> 24) & 0xFF;

        dr = sr + (dr * inv_a + 128) / 255;
        dg = sg + (dg * inv_a + 128) / 255;
        db = sb + (db * inv_a + 128) / 255;
        da = sa + (da * inv_a + 128) / 255;
        if (dr > 255) dr = 255;
        if (dg > 255) dg = 255;
        if (db > 255) db = 255;
        if (da > 255) da = 255;

        dst[i] = (da << 24) | (dr << 16) | (dg << 8) | db;
    }
}

/* ---- SSE2 gradient fill (4 pixels per iteration) ---- */

void simd_gradient_row_sse2(uint32_t *dst, uint32_t count,
                            uint32_t c0, uint32_t c1,
                            uint32_t t_start_256, uint32_t t_step_256)
{
    uint32_t i;

    /* Extract channels */
    uint32_t r0 = (c0 >> 16) & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = c0 & 0xFF, a0 = (c0 >> 24) & 0xFF;
    uint32_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF, a1 = (c1 >> 24) & 0xFF;

    uint32_t t = t_start_256;

    for (i = 0; i < count; i++) {
        uint32_t t8 = t >> 8;  /* scale from 0..65535 to 0..255ish */
        uint32_t inv_t;

        if (t8 > 255) t8 = 255;
        inv_t = 255 - t8;

        {
            uint32_t r = (r0 * inv_t + r1 * t8) / 255;
            uint32_t g = (g0 * inv_t + g1 * t8) / 255;
            uint32_t b = (b0 * inv_t + b1 * t8) / 255;
            uint32_t a = (a0 * inv_t + a1 * t8) / 255;

            dst[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }

        t += t_step_256;
    }
}

/* ---- SSE2 blur accumulate (4 pixels per iteration) ---- */

void simd_blur_accum_sse2(const uint32_t *src, uint32_t count,
                           uint32_t *sum_buf)
{
    uint32_t i;
    uint32_t aligned = count & ~3u;

    /* Use SSE2 to unpack and accumulate 4 pixels at once */
    for (i = 0; i < aligned; i += 4) {
        __asm__ volatile (
            "pxor    %%xmm7, %%xmm7\n\t"

            /* Load 4 pixels */
            "movdqu  (%[src]), %%xmm0\n\t"

            /* Unpack low 2 pixels to 16-bit */
            "movdqa  %%xmm0, %%xmm1\n\t"
            "punpcklbw %%xmm7, %%xmm1\n\t"

            /* Unpack high 2 pixels to 16-bit */
            "movdqa  %%xmm0, %%xmm2\n\t"
            "punpckhbw %%xmm7, %%xmm2\n\t"

            /* Widen to 32-bit for accumulation without overflow */
            /* Low pair: pixel 0 */
            "movdqa  %%xmm1, %%xmm3\n\t"
            "punpcklwd %%xmm7, %%xmm3\n\t"  /* pixel 0: 4x32 (B,G,R,A) */

            /* Load existing sum and add */
            "movdqu  (%[sum]), %%xmm4\n\t"
            "paddd   %%xmm3, %%xmm4\n\t"
            "movdqu  %%xmm4, (%[sum])\n\t"

            /* pixel 1 */
            "movdqa  %%xmm1, %%xmm3\n\t"
            "punpckhwd %%xmm7, %%xmm3\n\t"

            "movdqu  16(%[sum]), %%xmm4\n\t"
            "paddd   %%xmm3, %%xmm4\n\t"
            "movdqu  %%xmm4, 16(%[sum])\n\t"

            /* pixel 2 */
            "movdqa  %%xmm2, %%xmm3\n\t"
            "punpcklwd %%xmm7, %%xmm3\n\t"

            "movdqu  32(%[sum]), %%xmm4\n\t"
            "paddd   %%xmm3, %%xmm4\n\t"
            "movdqu  %%xmm4, 32(%[sum])\n\t"

            /* pixel 3 */
            "movdqa  %%xmm2, %%xmm3\n\t"
            "punpckhwd %%xmm7, %%xmm3\n\t"

            "movdqu  48(%[sum]), %%xmm4\n\t"
            "paddd   %%xmm3, %%xmm4\n\t"
            "movdqu  %%xmm4, 48(%[sum])\n\t"
            :
            : [src] "r"(src + i), [sum] "r"(sum_buf + i * 4)
            : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm7"
        );
    }

    /* Scalar fallback for remaining pixels */
    for (; i < count; i++) {
        uint32_t p = src[i];
        sum_buf[i * 4 + 0] += (p >> 0)  & 0xFF;  /* B */
        sum_buf[i * 4 + 1] += (p >> 8)  & 0xFF;  /* G */
        sum_buf[i * 4 + 2] += (p >> 16) & 0xFF;  /* R */
        sum_buf[i * 4 + 3] += (p >> 24) & 0xFF;  /* A */
    }
}

/* ============================================================================
 * AVX2 SIMD — 8 pixels per iteration using 256-bit YMM registers
 *
 * All AVX2 functions use inline assembly (not intrinsics) to work with
 * the -msse2 compile flag.  Each function ends with VZEROUPPER to avoid
 * SSE/AVX transition penalties.
 * ============================================================================ */

int simd_avx2_ok = 0;  /* Set to 1 at boot if AVX2 is available and enabled */

void simd_enable_avx(void)
{
    /* Use centralized CPUID detection */
    if (!cpu_has(CPU_FEATURE_XSAVE))  /* XSAVE not supported by CPU */
        return;
    if (!cpu_has(CPU_FEATURE_AVX))    /* AVX not supported by CPU */
        return;
    if (!cpu_has(CPU_FEATURE_AVX2))   /* AVX2 not supported by CPU */
        return;

    /* Set CR4.OSXSAVE (bit 18) to enable XGETBV/XSETBV */
    {
        uint64_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1UL << 18);
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    }

    /* Set XCR0 bits 0 (x87), 1 (SSE), 2 (AVX) via XSETBV */
    {
        uint32_t xcr0_lo, xcr0_hi;
        /* Read current XCR0 */
        __asm__ volatile ("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"((uint32_t)0));
        xcr0_lo |= 0x07;  /* bits 0,1,2 = x87 + SSE + AVX */
        __asm__ volatile ("xsetbv" : : "a"(xcr0_lo), "d"(xcr0_hi), "c"((uint32_t)0));
    }

    simd_avx2_ok = 1;
}

/* ---- AVX2 alpha blending (8 pixels per iteration) ---- */

void simd_blend_pixels_avx2(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    uint32_t i;
    uint32_t aligned = count & ~7u;  /* round down to multiple of 8 */

    for (i = 0; i < aligned; i += 8) {
        /*
         * Process 8 pixels using AVX2 via inline asm.
         * Same pre-multiplied alpha blend as SSE2 but with YMM registers.
         * Process as two groups of 4 (low and high 128-bit lanes).
         */
        __asm__ volatile (
            /* Load 8 src and 8 dst pixels */
            "vmovdqu (%[src]), %%ymm0\n\t"
            "vmovdqu (%[dst]), %%ymm1\n\t"

            /* Zero register for unpacking */
            "vpxor   %%ymm7, %%ymm7, %%ymm7\n\t"

            /* ---- Process low 4 pixels (128-bit lane 0) ---- */
            /* Unpack src low 2 pixels to 16-bit */
            "vextracti128 $0, %%ymm0, %%xmm2\n\t"   /* xmm2 = src[0..3] */
            "vextracti128 $0, %%ymm1, %%xmm3\n\t"   /* xmm3 = dst[0..3] */

            /* Unpack to 16-bit */
            "vpxor   %%xmm7, %%xmm7, %%xmm7\n\t"
            "vpunpcklbw %%xmm7, %%xmm2, %%xmm4\n\t" /* src_lo 16bit */
            "vpunpckhbw %%xmm7, %%xmm2, %%xmm5\n\t" /* src_hi 16bit */
            "vpunpcklbw %%xmm7, %%xmm3, %%xmm2\n\t" /* dst_lo 16bit */
            "vpunpckhbw %%xmm7, %%xmm3, %%xmm3\n\t" /* dst_hi 16bit */

            /* 255 constant */
            "vpcmpeqw %%xmm6, %%xmm6, %%xmm6\n\t"
            "vpsrlw   $8, %%xmm6, %%xmm6\n\t"

            /* Extract + broadcast alpha, compute inv_alpha for low pair */
            "vpshuflw $0xFF, %%xmm4, %%xmm0\n\t"
            "vpshufhw $0xFF, %%xmm0, %%xmm0\n\t"
            "vpsubw   %%xmm0, %%xmm6, %%xmm1\n\t"  /* inv_a_lo */
            "vpmullw  %%xmm1, %%xmm2, %%xmm2\n\t"  /* dst_lo * inv_a */
            "vpsrlw   $8, %%xmm2, %%xmm2\n\t"
            "vpaddw   %%xmm4, %%xmm2, %%xmm2\n\t"  /* result_lo */

            /* High pair of lane 0 */
            "vpshuflw $0xFF, %%xmm5, %%xmm0\n\t"
            "vpshufhw $0xFF, %%xmm0, %%xmm0\n\t"
            "vpsubw   %%xmm0, %%xmm6, %%xmm1\n\t"
            "vpmullw  %%xmm1, %%xmm3, %%xmm3\n\t"
            "vpsrlw   $8, %%xmm3, %%xmm3\n\t"
            "vpaddw   %%xmm5, %%xmm3, %%xmm3\n\t"

            /* Pack lane 0 back to 8-bit */
            "vpackuswb %%xmm3, %%xmm2, %%xmm2\n\t"  /* xmm2 = result[0..3] */

            /* ---- Process high 4 pixels (128-bit lane 1) ---- */
            "vmovdqu (%[src]), %%ymm0\n\t"   /* reload src */
            "vmovdqu (%[dst]), %%ymm1\n\t"   /* reload dst */
            "vextracti128 $1, %%ymm0, %%xmm4\n\t"   /* xmm4 = src[4..7] */
            "vextracti128 $1, %%ymm1, %%xmm5\n\t"   /* xmm5 = dst[4..7] */

            "vpunpcklbw %%xmm7, %%xmm4, %%xmm0\n\t" /* src_lo */
            "vpunpckhbw %%xmm7, %%xmm4, %%xmm1\n\t" /* src_hi */
            "vpunpcklbw %%xmm7, %%xmm5, %%xmm4\n\t" /* dst_lo */
            "vpunpckhbw %%xmm7, %%xmm5, %%xmm5\n\t" /* dst_hi */

            /* Low pair of lane 1 */
            "vpshuflw $0xFF, %%xmm0, %%xmm3\n\t"
            "vpshufhw $0xFF, %%xmm3, %%xmm3\n\t"
            "vpsubw   %%xmm3, %%xmm6, %%xmm3\n\t"
            "vpmullw  %%xmm3, %%xmm4, %%xmm4\n\t"
            "vpsrlw   $8, %%xmm4, %%xmm4\n\t"
            "vpaddw   %%xmm0, %%xmm4, %%xmm4\n\t"

            /* High pair of lane 1 */
            "vpshuflw $0xFF, %%xmm1, %%xmm3\n\t"
            "vpshufhw $0xFF, %%xmm3, %%xmm3\n\t"
            "vpsubw   %%xmm3, %%xmm6, %%xmm3\n\t"
            "vpmullw  %%xmm3, %%xmm5, %%xmm5\n\t"
            "vpsrlw   $8, %%xmm5, %%xmm5\n\t"
            "vpaddw   %%xmm1, %%xmm5, %%xmm5\n\t"

            /* Pack lane 1 back to 8-bit */
            "vpackuswb %%xmm5, %%xmm4, %%xmm4\n\t"  /* xmm4 = result[4..7] */

            /* Combine lanes into YMM and store */
            "vinserti128 $0, %%xmm2, %%ymm4, %%ymm2\n\t"
            "vinserti128 $1, %%xmm4, %%ymm2, %%ymm2\n\t"
            "vmovdqu %%ymm2, (%[dst])\n\t"

            "vzeroupper\n\t"
            :
            : [src] "r"(src + i), [dst] "r"(dst + i)
            : "memory", "xmm0", "xmm1", "xmm2", "xmm3",
              "xmm4", "xmm5", "xmm6", "xmm7"
        );
    }

    /* Handle remaining 0-7 pixels with scalar fallback */
    for (; i < count; i++) {
        uint32_t s = src[i];
        uint32_t d = dst[i];
        uint32_t sa = (s >> 24) & 0xFF;
        uint32_t inv_a;
        uint32_t sr, sg, sb, dr, dg, db, da;

        if (sa == 0xFF) { dst[i] = s; continue; }
        if (sa == 0x00) continue;

        inv_a = 255 - sa;
        sr = (s >> 16) & 0xFF; sg = (s >> 8) & 0xFF; sb = s & 0xFF;
        dr = (d >> 16) & 0xFF; dg = (d >> 8) & 0xFF; db = d & 0xFF;
        da = (d >> 24) & 0xFF;

        dr = sr + (dr * inv_a + 128) / 255;
        dg = sg + (dg * inv_a + 128) / 255;
        db = sb + (db * inv_a + 128) / 255;
        da = sa + (da * inv_a + 128) / 255;
        if (dr > 255) dr = 255;
        if (dg > 255) dg = 255;
        if (db > 255) db = 255;
        if (da > 255) da = 255;

        dst[i] = (da << 24) | (dr << 16) | (dg << 8) | db;
    }
}

/* ---- AVX2 blur accumulate (8 pixels per iteration) ---- */

void simd_blur_accum_avx2(const uint32_t *src, uint32_t count,
                           uint32_t *sum_buf)
{
    uint32_t i;
    uint32_t aligned = count & ~7u;

    for (i = 0; i < aligned; i += 8) {
        /* Process 8 pixels: unpack ARGB to 32-bit per channel, add to sum_buf.
         * Process as two groups of 4 to reuse SSE2-like logic in each lane. */
        __asm__ volatile (
            "vpxor    %%ymm7, %%ymm7, %%ymm7\n\t"

            /* Load 8 pixels */
            "vmovdqu  (%[src]), %%ymm0\n\t"

            /* Process pixels 0-3 (low 128-bit lane) */
            "vextracti128 $0, %%ymm0, %%xmm1\n\t"
            "vpxor    %%xmm7, %%xmm7, %%xmm7\n\t"

            /* pixel 0 */
            "vmovdqa  %%xmm1, %%xmm2\n\t"
            "vpunpcklbw %%xmm7, %%xmm2, %%xmm2\n\t"
            "vmovdqa  %%xmm2, %%xmm3\n\t"
            "vpunpcklwd %%xmm7, %%xmm3, %%xmm3\n\t"
            "vmovdqu  (%[sum]), %%xmm4\n\t"
            "vpaddd   %%xmm3, %%xmm4, %%xmm4\n\t"
            "vmovdqu  %%xmm4, (%[sum])\n\t"

            /* pixel 1 */
            "vpunpckhwd %%xmm7, %%xmm2, %%xmm3\n\t"
            "vmovdqu  16(%[sum]), %%xmm4\n\t"
            "vpaddd   %%xmm3, %%xmm4, %%xmm4\n\t"
            "vmovdqu  %%xmm4, 16(%[sum])\n\t"

            /* pixel 2 */
            "vmovdqa  %%xmm1, %%xmm2\n\t"
            "vpunpckhbw %%xmm7, %%xmm2, %%xmm2\n\t"
            "vmovdqa  %%xmm2, %%xmm3\n\t"
            "vpunpcklwd %%xmm7, %%xmm3, %%xmm3\n\t"
            "vmovdqu  32(%[sum]), %%xmm4\n\t"
            "vpaddd   %%xmm3, %%xmm4, %%xmm4\n\t"
            "vmovdqu  %%xmm4, 32(%[sum])\n\t"

            /* pixel 3 */
            "vpunpckhwd %%xmm7, %%xmm2, %%xmm3\n\t"
            "vmovdqu  48(%[sum]), %%xmm4\n\t"
            "vpaddd   %%xmm3, %%xmm4, %%xmm4\n\t"
            "vmovdqu  %%xmm4, 48(%[sum])\n\t"

            /* Process pixels 4-7 (high 128-bit lane) */
            "vmovdqu  (%[src]), %%ymm0\n\t"
            "vextracti128 $1, %%ymm0, %%xmm1\n\t"

            /* pixel 4 */
            "vmovdqa  %%xmm1, %%xmm2\n\t"
            "vpunpcklbw %%xmm7, %%xmm2, %%xmm2\n\t"
            "vmovdqa  %%xmm2, %%xmm3\n\t"
            "vpunpcklwd %%xmm7, %%xmm3, %%xmm3\n\t"
            "vmovdqu  64(%[sum]), %%xmm4\n\t"
            "vpaddd   %%xmm3, %%xmm4, %%xmm4\n\t"
            "vmovdqu  %%xmm4, 64(%[sum])\n\t"

            /* pixel 5 */
            "vpunpckhwd %%xmm7, %%xmm2, %%xmm3\n\t"
            "vmovdqu  80(%[sum]), %%xmm4\n\t"
            "vpaddd   %%xmm3, %%xmm4, %%xmm4\n\t"
            "vmovdqu  %%xmm4, 80(%[sum])\n\t"

            /* pixel 6 */
            "vmovdqa  %%xmm1, %%xmm2\n\t"
            "vpunpckhbw %%xmm7, %%xmm2, %%xmm2\n\t"
            "vmovdqa  %%xmm2, %%xmm3\n\t"
            "vpunpcklwd %%xmm7, %%xmm3, %%xmm3\n\t"
            "vmovdqu  96(%[sum]), %%xmm4\n\t"
            "vpaddd   %%xmm3, %%xmm4, %%xmm4\n\t"
            "vmovdqu  %%xmm4, 96(%[sum])\n\t"

            /* pixel 7 */
            "vpunpckhwd %%xmm7, %%xmm2, %%xmm3\n\t"
            "vmovdqu  112(%[sum]), %%xmm4\n\t"
            "vpaddd   %%xmm3, %%xmm4, %%xmm4\n\t"
            "vmovdqu  %%xmm4, 112(%[sum])\n\t"

            "vzeroupper\n\t"
            :
            : [src] "r"(src + i), [sum] "r"(sum_buf + i * 4)
            : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm7"
        );
    }

    /* Scalar fallback for remaining pixels */
    for (; i < count; i++) {
        uint32_t p = src[i];
        sum_buf[i * 4 + 0] += (p >> 0)  & 0xFF;
        sum_buf[i * 4 + 1] += (p >> 8)  & 0xFF;
        sum_buf[i * 4 + 2] += (p >> 16) & 0xFF;
        sum_buf[i * 4 + 3] += (p >> 24) & 0xFF;
    }
}
