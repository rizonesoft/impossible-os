/* ============================================================================
 * lz4.c -- Kernel LZ4 block compressor wrapper (kernel embedded libraries)
 *
 * Thin, size-safe boundary over the vendored freestanding LZ4 v1.10.0 core
 * (src/libs/lz4/lz4.c, compiled with LZ4_FREESTANDING=1). The vendored API is
 * int-based; this wrapper takes size_t lengths and rejects out-of-range values
 * before narrowing, so a 64-bit caller length can never truncate into LZ4's
 * pointer arithmetic.
 *
 * Stateless and reentrant: no shared mutable state, no allocation -- safe to
 * call concurrently from any CPU. Callers own both buffers (output > 4 KiB
 * must come from pmm_alloc_contiguous per the CLAUDE.md memory rule).
 * ============================================================================ */

#include "libs/lz4.h"
#include "libs/lz4/lz4.h"

/* The vendored core narrows every length to a signed int internally, so the
 * wrapper's largest safe length is INT_MAX. LZ4_BLOCK_INPUT_MAX (0x7E000000)
 * is already below this, but dst_capacity has no such intrinsic ceiling. */
#define LZ4_WRAP_INT_MAX 0x7FFFFFFF

/* SMP-reentrancy by design: lz4.c is built with LZ4_MEMORY_USAGE=11 so the LZ4
 * compression hash-table state (LZ4_STREAM_MINSIZE, ~2 KiB) lives on the
 * caller's stack -- per-CPU and per-call, with zero shared mutable state and no
 * allocator dependency. (Both pmm and the kmalloc heap are unsynchronized in
 * this kernel, so an allocator-backed state could not be made reentrant; an
 * on-stack state is the only genuinely concurrent-safe option.) The Makefile
 * caps the LZ4 frame with -Wframe-larger-than so a future LZ4_MEMORY_USAGE bump
 * cannot silently overflow the 8 KiB kernel task stacks. */

size_t lz4_compress_bound(size_t input_size)
{
    if (input_size > LZ4_BLOCK_INPUT_MAX) {
        return 0;
    }
    return (size_t)LZ4_compressBound((int)input_size);
}

int lz4_compress(const void *src, size_t src_size, void *dst, size_t dst_capacity)
{
    if (src == NULL || dst == NULL || src_size == 0) {
        return LZ4_ERR_ARG;
    }
    if (src_size > LZ4_BLOCK_INPUT_MAX || dst_capacity > LZ4_WRAP_INT_MAX) {
        return LZ4_ERR_RANGE;
    }

    int produced = LZ4_compress_default((const char *)src, (char *)dst,
                                        (int)src_size, (int)dst_capacity);
    /* LZ4_compress_default returns 0 when dst_capacity is too small. */
    return (produced > 0) ? produced : LZ4_ERR_FAIL;
}

int lz4_decompress(const void *src, size_t src_size, void *dst, size_t dst_capacity)
{
    if (src == NULL || dst == NULL || src_size == 0) {
        return LZ4_ERR_ARG;
    }
    if (src_size > LZ4_WRAP_INT_MAX || dst_capacity > LZ4_WRAP_INT_MAX) {
        return LZ4_ERR_RANGE;
    }

    int produced = LZ4_decompress_safe((const char *)src, (char *)dst,
                                       (int)src_size, (int)dst_capacity);
    /* LZ4_decompress_safe returns a negative value on malformed input. */
    return (produced >= 0) ? produced : LZ4_ERR_FAIL;
}
