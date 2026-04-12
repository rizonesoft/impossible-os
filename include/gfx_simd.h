/* ============================================================================
 * gfx_simd.h -- SSE2 / AVX2 SIMD acceleration for GFX primitives
 *
 * When SSE2 is available, the gfx library dispatches hot-path operations
 * (alpha blending, gradient fill, blur) through these vectorized routines.
 * SSE2: 4 ARGB pixels per iteration using 128-bit XMM registers.
 * AVX2: 8 ARGB pixels per iteration using 256-bit YMM registers.
 *
 * FPU State:  Call simd_save_state() / simd_restore_state() to protect
 *             user-mode FPU/SSE/AVX registers across kernel SIMD use.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- FPU / SSE state save/restore ---- */

/* 512-byte FXSAVE area -- must be 16-byte aligned.
 * Also large enough for XSAVE (x87+SSE+AVX = ~832 bytes). */
typedef struct __attribute__((aligned(64))) {
    uint8_t data[1024];
} fxsave_area_t;

/* Save the current FPU/SSE state */
void simd_save_state(fxsave_area_t *area);

/* Restore previously saved FPU/SSE state */
void simd_restore_state(const fxsave_area_t *area);

/* Enable SSE (set CR0.EM=0, CR0.MP=1, CR4.OSFXSR=1, CR4.OSXMMEXCPT=1) */
void simd_enable_sse(void);

/* ---- Capability detection ---- */

/* Returns 1 if SSE2 is supported (CPUID.01H:EDX bit 26) */
int simd_has_sse2(void);

/* Returns 1 if AVX2 is supported (CPUID.07H:EBX bit 5) */
int simd_has_avx2(void);

/* ---- SSE2 accelerated operations ---- */

/* Alpha-blend src onto dst, 4 pixels per iteration.
 * count MUST be divisible by 4.  Trailing pixels handled by caller. */
void simd_blend_pixels_sse2(uint32_t *dst, const uint32_t *src, uint32_t count);

/* Fill dst with a linear gradient (vertical), 4 pixels per iteration.
 * Writes `count` pixels at the given row, interpolating between c0 and c1.
 * t_start: fixed-point 8.8 start position, t_step: increment per pixel. */
void simd_gradient_row_sse2(uint32_t *dst, uint32_t count,
                            uint32_t c0, uint32_t c1,
                            uint32_t t_start_256, uint32_t t_step_256);

/* Horizontal blur accumulate pass, 4 pixels per iteration.
 * Sums ARGB channels from src into running accumulators. */
void simd_blur_accum_sse2(const uint32_t *src, uint32_t count,
                           uint32_t *sum_buf);

/* ---- AVX2 accelerated operations (8 pixels per iteration) ---- */

/* Runtime flag: set to 1 at boot if AVX2 is available and enabled */
extern int simd_avx2_ok;

/* Enable AVX (CR4.OSXSAVE + XCR0 bits 0,1,2).
 * Must be called before any AVX2 function. */
void simd_enable_avx(void);

/* Alpha-blend src onto dst, 8 pixels per iteration (AVX2).
 * Scalar fallback handles trailing pixels. */
void simd_blend_pixels_avx2(uint32_t *dst, const uint32_t *src, uint32_t count);

/* Horizontal blur accumulate pass, 8 pixels per iteration (AVX2). */
void simd_blur_accum_avx2(const uint32_t *src, uint32_t count,
                           uint32_t *sum_buf);

/* Block copy of uint32_t pixels, 8 pixels per iteration (AVX2).
 * Scalar fallback handles trailing pixels. vzeroupper at exit. */
void fb_blit_avx(uint32_t *dst, const uint32_t *src, uint32_t count);

/* Block fill of uint32_t pixels, 8 pixels per iteration (AVX2).
 * Uses vpbroadcastd to replicate the 32-bit pixel value. */
void fb_fill_avx(uint32_t *dst, uint32_t val, uint32_t count);
