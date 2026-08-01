/* ============================================================================
 * gfx_simd.c -- SSE2 / AVX2 SIMD acceleration for GFX primitives
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
#include "kernel/msr.h"
#include "kernel/klog.h"

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
 * AVX2 SIMD -- 8 pixels per iteration using 256-bit YMM registers
 *
 * All AVX2 functions use inline assembly (not intrinsics) to work with
 * the -msse2 compile flag.  Each function ends with VZEROUPPER to avoid
 * SSE/AVX transition penalties.
 * ============================================================================ */

int simd_avx2_ok = 0;   /* Set to 1 at boot if AVX2 is available and enabled */
int simd_avx512_ok = 0; /* Set to 1 at boot if AVX-512 passes throttle check */

/* CAN this CPU execute AVX2, as opposed to SHOULD the dispatcher choose it.
 *
 * simd_avx2_ok answers the second question and is deliberately mutable: the
 * boot-time auto-tune in hw_profile.c clears it when the measured AVX2 gain
 * over SSE2 falls under HW_PROFILE_SIMD_GAIN_PCT, which is exactly the right
 * behaviour for a dispatch flag. It is the WRONG question for anything asking
 * whether the AVX2 code paths execute correctly, because the auto-tune's input
 * is a wall-clock micro-benchmark: on a loaded host the same machine answers
 * differently run to run.
 *
 * That difference was measured, not theorised. Four AVX2 correctness suites
 * gated on simd_avx2_ok, so on 9 of 10 identical runs they skipped and on the
 * 10th they ran -- moving the suite's headline assertion total by exactly 6
 * (and its skip count by 4) with no code change at all -- a headline number
 * that moves on its own cannot be used as the regression signal every reader
 * treats it as.
 *
 * Set once by the feature probe and never cleared, so a correctness test can
 * ask the question it actually means.
 *
 * CONCURRENCY: plain access is correct and deliberate. The single write is in
 * simd_enable_avx(), which runs in boot_phase0 BEFORE any AP is brought up,
 * and every reader is a BSP-side test suite. It is NOT the same shape as
 * simd_avx2_ok one line above, which is accessed with __ATOMIC_ACQUIRE/RELEASE
 * precisely because the auto-tune rewrites it after boot and the memops
 * dispatcher reads it from other CPUs. Stated rather than left to inference:
 * anything that starts writing this after AP bringup, or reads it off the BSP,
 * has to convert BOTH sides to atomics rather than copying the plain read. */
int simd_avx2_capable = 0;

void simd_enable_avx(void)
{
    /* XCR0 is now configured by cpu_configure_xcr0() in cpuid_init().
     * Just check if AVX2 was successfully enabled. */
    if (!cpu_has(CPU_FEATURE_AVX2))
        return;

    /* Verify XCR0 has AVX bit set (cpu_configure_xcr0 should have done this) */
    {
        extern struct cpu_features g_cpu;
        if (!(g_cpu.xcr0_active & (1UL << 2)))
            return;  /* AVX not enabled in XCR0 */
    }

    simd_avx2_ok = 1;
    simd_avx2_capable = 1;
}

/* ---- AVX-512 opt-in with MPERF/APERF throttle guard ----
 *
 * Consumer CPUs (Rocket Lake, Alder Lake) often throttle core frequency
 * when executing 512-bit instructions. This check runs a ~10 us AVX-512
 * micro-burst and compares APERF/MPERF ratios before/after. If the
 * effective frequency drops > 5%, AVX-512 is disabled by clearing
 * XCR0 bits 5-7 and the kernel falls back to AVX2 paths.
 *
 * MPERF increments at the maximum non-turbo ratio (fixed reference).
 * APERF increments at the actual running frequency. A ratio drop means
 * the CPU reduced frequency in response to heavy vector instructions.
 *
 * Must be called after simd_enable_avx() and cpu_configure_xcr0().
 */

void simd_enable_avx512(void)
{
    extern struct cpu_features g_cpu;
    uint64_t mperf0, aperf0, mperf1, aperf1;
    uint64_t dmperf, daperf;

    if (!cpu_has(CPU_FEATURE_AVX512F))
        return;

    /* Verify XCR0 bits 5-7 are set (opmask + ZMM_Hi256 + Hi16_ZMM) */
    if ((g_cpu.xcr0_active & 0xE0) != 0xE0)
        return;

    /* Try reading MPERF/APERF; skip throttle check if unavailable
     * (e.g., some hypervisors do not expose these MSRs) */
    if (msr_try_read(MSR_IA32_MPERF, &mperf0) != 0 ||
        msr_try_read(MSR_IA32_APERF, &aperf0) != 0) {
        /* Cannot read performance counters; enable without throttle check */
        klog(LOG_WARN, "simd",
             "MPERF/APERF unavailable; enabling AVX-512 without throttle check");
        simd_avx512_ok = 1;
        return;
    }

    /* Execute AVX-512 micro-burst (~10 us of 512-bit operations) */
    simd_avx512_burst();

    /* Sample counters after burst; use msr_try_read for both post-burst
     * reads since these MSRs are optional telemetry. */
    if (msr_try_read(MSR_IA32_MPERF, &mperf1) != 0 ||
        msr_try_read(MSR_IA32_APERF, &aperf1) != 0) {
        klog(LOG_WARN, "simd",
             "MPERF/APERF post-burst read failed; enabling AVX-512");
        simd_avx512_ok = 1;
        return;
    }

    dmperf = mperf1 - mperf0;
    daperf = aperf1 - aperf0;

    /* Guard against zero or implausibly large deltas (VM pause, counter
     * wraparound). For a ~10us burst, deltas should be < 1M cycles.
     * Cap at 2^56 to prevent overflow in the multiply below.
     * Fail closed: if telemetry is broken, do NOT enable AVX-512. */
    if (dmperf == 0 || dmperf > (1ULL << 56) ||
        daperf == 0 || daperf > (1ULL << 56)) {
        klog(LOG_WARN, "simd",
             "MPERF/APERF delta out of range; AVX-512 disabled (fail-closed)");
        return;
    }

    /* Check if frequency dropped > 5%:
     * ratio = daperf / dmperf; throttled if ratio < 0.95
     * Integer math: throttled if daperf * 100 < dmperf * 95
     * Overflow-safe: deltas capped at 2^56, so *100 fits in 64 bits. */
    if (daperf * 100 < dmperf * 95) {
        /* Throttling detected; disable AVX-512 by clearing XCR0 bits 5-7 */
        uint64_t mask = g_cpu.xcr0_active & ~0xE0UL;
        uint32_t lo = (uint32_t)mask;
        uint32_t hi = (uint32_t)(mask >> 32);
        __asm__ volatile ("xsetbv" : : "a"(lo), "d"(hi), "c"((uint32_t)0));
        g_cpu.xcr0_active = mask;

        klog(LOG_WARN, "simd",
             "AVX-512 throttling detected (APERF/MPERF=%u%%), disabled",
             (uint64_t)(daperf * 100 / dmperf));
        return;
    }

    simd_avx512_ok = 1;
    klog(LOG_INFO, "simd",
         "AVX-512 enabled (16 pixels/iter, APERF/MPERF=%u%%)",
         (uint64_t)(daperf * 100 / dmperf));
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

/* ---- AVX2 framebuffer blit: block copy of uint32_t pixels ---- */

void fb_blit_avx(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    uint32_t i = 0;
    uint32_t aligned = count & ~7u;  /* 8 pixels = 32 bytes per YMM */

    for (; i < aligned; i += 8) {
        __asm__ volatile (
            "vmovdqu (%[s]), %%ymm0\n\t"
            "vmovdqu %%ymm0, (%[d])\n\t"
            :
            : [s] "r"(src + i), [d] "r"(dst + i)
            : "memory", "xmm0"
        );
    }

    /* Scalar tail: 0-7 remaining pixels */
    for (; i < count; i++)
        dst[i] = src[i];

    __asm__ volatile ("vzeroupper" ::: "memory");
}

/* ---- AVX2 framebuffer fill: block fill of uint32_t pixels ---- */

void fb_fill_avx(uint32_t *dst, uint32_t val, uint32_t count)
{
    uint32_t blocks = count >> 3;  /* number of 8-pixel (32-byte) blocks */

    if (blocks > 0) {
        /* Broadcast once, store in internal asm loop. Single block keeps
         * the YMM register lifetime fully visible to the compiler. */
        uint32_t *p = dst;
        __asm__ volatile (
            "vmovd       %[v], %%xmm0\n\t"
            "vpbroadcastd %%xmm0, %%ymm0\n\t"
            "1:\n\t"
            "vmovdqu     %%ymm0, (%[p])\n\t"
            "add         $32, %[p]\n\t"
            "dec         %[cnt]\n\t"
            "jnz         1b\n\t"
            : [p] "+r"(p), [cnt] "+r"(blocks)
            : [v] "r"(val)
            : "memory", "xmm0", "cc"
        );
    }

    /* Scalar tail: 0-7 remaining pixels */
    {
        uint32_t i = count & ~7u;
        for (; i < count; i++)
            dst[i] = val;
    }

    __asm__ volatile ("vzeroupper" ::: "memory");
}
