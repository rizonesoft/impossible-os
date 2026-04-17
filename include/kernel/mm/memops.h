/* ============================================================================
 * memops.h -- SIMD-accelerated memory operations
 *
 * Provides AVX-512, AVX2, and SSE2 memcpy/memset with runtime dispatch.
 * memops_avx512.c is compiled with -mavx512f, memops.c with -mavx2;
 * callers use memcpy_fast()/memset_fast() which branch to the best
 * available path at runtime.
 *
 * WARNING: These functions use XMM/YMM registers without save/restore.
 * Safe to call from the compositor path (which already uses SIMD via
 * gfx_simd.c blend/blur). For arbitrary kernel contexts (syscall handlers,
 * ISRs), a kernel_fpu_begin/end protocol is needed first to avoid
 * corrupting the current task's lazy FPU state.
 *
 * XREF: 02-kernel-core/TODO-09-x86-64-architecture.md S2, S3
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- AVX-512 implementations (require simd_avx512_ok, i.e., XCR0 bits 5-7) ---- */

void *memcpy_avx512(void *dst, const void *src, size_t n);
void *memset_avx512(void *dst, int val, size_t n);

/* ---- AVX2 implementations (require simd_avx2_ok, i.e., XCR0 bit 2) ---- */

void *memcpy_avx(void *dst, const void *src, size_t n);
void *memset_avx(void *dst, int val, size_t n);

/* ---- SSE2 implementations (require CPU_FEATURE_SSE2) ---- */

void *memcpy_sse2(void *dst, const void *src, size_t n);
void *memset_sse2(void *dst, int val, size_t n);

/* ---- Runtime dispatch: AVX-512 -> AVX2 -> SSE2 -> scalar ---- */

void *memcpy_fast(void *dst, const void *src, size_t n);
void *memset_fast(void *dst, int val, size_t n);
