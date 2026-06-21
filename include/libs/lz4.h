/* ============================================================================
 * lz4.h -- Kernel LZ4 block compressor API (thin wrapper over vendored LZ4)
 *
 * Freestanding port of LZ4 v1.10.0 (src/libs/lz4/), built with
 * LZ4_FREESTANDING=1 (block-only; the heap-backed stream and .lz4 frame
 * APIs are disabled). Intended for crash dumps, hibernation images, and EIF
 * compressed segments (consumer wiring tracked in the kernel-libraries TODO).
 *
 * Memory rule (CLAUDE.md): the block API allocates nothing -- callers own
 * both buffers. Output buffers larger than 4 KiB MUST come from
 * pmm_alloc_contiguous(), never kmalloc (which is capped at 4 KiB).
 *
 * Size discipline: the public API takes size_t lengths and rejects any value
 * above the LZ4 maxima BEFORE narrowing to the vendored int-based core, so a
 * 64-bit length from a size_t caller (crash dump / hibernation extent) can
 * never silently truncate into LZ4's pointer arithmetic and feed a negative
 * length into the decoder's pointer math.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Largest input LZ4 block compression accepts (matches the vendored
 * LZ4_MAX_INPUT_SIZE). Inputs above this are rejected, never narrowed. */
#define LZ4_BLOCK_INPUT_MAX 0x7E000000u

/* Error codes (all negative; success is a non-negative byte count). */
#define LZ4_ERR_ARG   (-1) /* NULL buffer or zero-length input */
#define LZ4_ERR_RANGE (-2) /* a length exceeds LZ4_BLOCK_INPUT_MAX / INT_MAX */
#define LZ4_ERR_FAIL  (-3) /* dst too small, or malformed/corrupt input */

/* Worst-case compressed size for input_size bytes. Returns 0 when input_size
 * exceeds LZ4_BLOCK_INPUT_MAX (callers size their dst buffer from this, so a
 * 0 return means "input too large to compress as one block"). */
size_t lz4_compress_bound(size_t input_size);

/* Compress src[0, src_size) into dst[0, dst_capacity). Returns the compressed
 * byte count (> 0) on success, or a negative LZ4_ERR_* code. No allocation; dst
 * must be at least lz4_compress_bound(src_size) to guarantee success.
 *
 * SMP-reentrant: the ~2 KiB LZ4 hash-table state (built with LZ4_MEMORY_USAGE=11
 * so it fits the kernel stack) lives on the caller's stack -- no shared state,
 * no allocator. Needs ~2.5 KiB of stack; safe on the 8 KiB kernel task stacks. */
int lz4_compress(const void *src, size_t src_size, void *dst, size_t dst_capacity);

/* Decompress src[0, src_size) into dst[0, dst_capacity). Bounds-checked
 * (LZ4_decompress_safe): never reads or writes outside the supplied buffers
 * even on malformed/hostile input. Returns the decompressed byte count (>= 0)
 * or a negative LZ4_ERR_* code. Use this -- not the unsafe fast path -- for
 * any off-box or untrusted frame (crash dumps, hibernation images). */
int lz4_decompress(const void *src, size_t src_size, void *dst, size_t dst_capacity);
