/* ============================================================================
 * gfx_simd.h — SSE2 SIMD acceleration for GFX primitives
 *
 * When SSE2 is available, the gfx library dispatches hot-path operations
 * (alpha blending, gradient fill, blur) through these vectorized routines.
 * Each processes 4 ARGB pixels per iteration using 128-bit XMM registers.
 *
 * FPU State:  Call simd_save_state() / simd_restore_state() to protect
 *             user-mode FPU/SSE/AVX registers across kernel SIMD use.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- FPU / SSE state save/restore ---- */

/* 512-byte FXSAVE area — must be 16-byte aligned */
typedef struct __attribute__((aligned(16))) {
    uint8_t data[512];
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
