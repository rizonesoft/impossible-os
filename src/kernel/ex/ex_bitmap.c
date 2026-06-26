/* ============================================================================
 * ex_bitmap.c -- RTL_BITMAP: general bit-vector utility (TODO-06 S7).
 *
 * A caller-owned bit buffer with set/clear/test/range/find primitives, matching
 * the Windows RtlBitMap surface. CALLER-SERIALIZED (not thread-safe) -- the
 * consumer owns synchronization, exactly like Windows.
 *
 * PADDING SAFETY: bits at indices >= SizeOfBitMap are padding in the final
 * uint32 word. Every whole-word read masks them off via tail_mask(); every
 * range/find op is bounded by SizeOfBitMap; single/range writes are index-
 * checked. So padding can never be counted, returned in a run, or mutated --
 * which matters because a caller using the bitmap as an index allocator would
 * otherwise hand out or corrupt out-of-range indices.
 *
 * Arch-neutral: no inline asm, no arch headers; popcount via __builtin_popcount.
 * ============================================================================ */

#include "kernel/ex.h"
#include "libc/string.h"   /* memset / memcpy */

#define BPW RTL_BITMAP_BITS_PER_WORD   /* 32 */

/* Number of words backing `size` bits. */
static inline uint32_t word_count(uint32_t size)
{
    return RTL_BITMAP_WORDS(size);
}

/* Mask of the VALID bits in the final word (all-ones if the size is a whole
 * multiple of 32, or 0 when there is no final word). */
static inline uint32_t tail_mask(uint32_t size)
{
    uint32_t rem = size & (BPW - 1u);
    return rem ? ((1u << rem) - 1u) : 0xFFFFFFFFu;
}

/* Mask of bits [lo, hi] within one word (0 <= lo <= hi <= 31). */
static inline uint32_t range_mask(uint32_t lo, uint32_t hi)
{
    uint32_t low  = (lo == 0)  ? 0xFFFFFFFFu : ~((1u << lo) - 1u);        /* bits >= lo */
    uint32_t high = (hi >= 31) ? 0xFFFFFFFFu : ((1u << (hi + 1u)) - 1u);  /* bits <= hi */
    return low & high;
}

/* Clamp a [start, count) request to the valid range; returns false (caller
 * does nothing) if it is empty or fully out of range, else writes [*fw..*lw]
 * word span with fb/lb the first/last bit indices within those end words. */
static bool clamp_range(const RTL_BITMAP *bm, uint32_t start, uint32_t count,
                        uint32_t *fw, uint32_t *fb, uint32_t *lw, uint32_t *lb)
{
    uint32_t end;
    if (count == 0 || start >= bm->SizeOfBitMap)
        return false;
    /* end = min(start + count, size), overflow-safe. */
    end = (count >= bm->SizeOfBitMap - start) ? bm->SizeOfBitMap : start + count;
    *fw = start / BPW;          *fb = start & (BPW - 1u);
    *lw = (end - 1u) / BPW;     *lb = (end - 1u) & (BPW - 1u);
    return true;
}

void RtlInitializeBitMap(RTL_BITMAP *bm, uint32_t *buffer, uint32_t size_bits)
{
    if (!bm)
        return;
    bm->SizeOfBitMap = size_bits;
    bm->Buffer = buffer;
}

void RtlClearAllBits(RTL_BITMAP *bm)
{
    if (!bm || !bm->Buffer)
        return;
    memset(bm->Buffer, 0, (size_t)word_count(bm->SizeOfBitMap) * sizeof(uint32_t));
}

void RtlSetAllBits(RTL_BITMAP *bm)
{
    uint32_t wc, i;
    if (!bm || !bm->Buffer || bm->SizeOfBitMap == 0)
        return;
    wc = word_count(bm->SizeOfBitMap);
    for (i = 0; i + 1u < wc; i++)
        bm->Buffer[i] = 0xFFFFFFFFu;
    /* Final word: only the valid bits, leaving padding clear. */
    bm->Buffer[wc - 1u] = tail_mask(bm->SizeOfBitMap);
}

void RtlSetBit(RTL_BITMAP *bm, uint32_t bit)
{
    if (!bm || !bm->Buffer || bit >= bm->SizeOfBitMap)
        return;
    bm->Buffer[bit / BPW] |= (1u << (bit & (BPW - 1u)));
}

void RtlClearBit(RTL_BITMAP *bm, uint32_t bit)
{
    if (!bm || !bm->Buffer || bit >= bm->SizeOfBitMap)
        return;
    bm->Buffer[bit / BPW] &= ~(1u << (bit & (BPW - 1u)));
}

bool RtlTestBit(const RTL_BITMAP *bm, uint32_t bit)
{
    if (!bm || !bm->Buffer || bit >= bm->SizeOfBitMap)
        return false;
    return (bm->Buffer[bit / BPW] >> (bit & (BPW - 1u))) & 1u;
}

void RtlSetBits(RTL_BITMAP *bm, uint32_t start, uint32_t count)
{
    uint32_t fw, fb, lw, lb, w;
    if (!bm || !bm->Buffer || !clamp_range(bm, start, count, &fw, &fb, &lw, &lb))
        return;
    if (fw == lw) {
        bm->Buffer[fw] |= range_mask(fb, lb);
        return;
    }
    bm->Buffer[fw] |= range_mask(fb, BPW - 1u);
    for (w = fw + 1u; w < lw; w++)
        bm->Buffer[w] = 0xFFFFFFFFu;
    bm->Buffer[lw] |= range_mask(0, lb);
}

void RtlClearBits(RTL_BITMAP *bm, uint32_t start, uint32_t count)
{
    uint32_t fw, fb, lw, lb, w;
    if (!bm || !bm->Buffer || !clamp_range(bm, start, count, &fw, &fb, &lw, &lb))
        return;
    if (fw == lw) {
        bm->Buffer[fw] &= ~range_mask(fb, lb);
        return;
    }
    bm->Buffer[fw] &= ~range_mask(fb, BPW - 1u);
    for (w = fw + 1u; w < lw; w++)
        bm->Buffer[w] = 0u;
    bm->Buffer[lw] &= ~range_mask(0, lb);
}

/* Shared body for AreBitsSet/AreBitsClear: `want_set` selects the predicate.
 * An out-of-range or empty request returns the oob/empty result the public
 * wrappers document (count 0 -> true; out of range -> false). */
static bool are_bits(const RTL_BITMAP *bm, uint32_t start, uint32_t count,
                     bool want_set)
{
    uint32_t fw, fb, lw, lb, w, m;
    if (!bm || !bm->Buffer)
        return false;
    if (count == 0)
        return true;
    /* The WHOLE range must be in bounds (unlike the write ops, which clamp). */
    if (start >= bm->SizeOfBitMap || count > bm->SizeOfBitMap - start)
        return false;
    fw = start / BPW;          fb = start & (BPW - 1u);
    lw = (start + count - 1u) / BPW;
    lb = (start + count - 1u) & (BPW - 1u);

    if (fw == lw) {
        m = range_mask(fb, lb);
        return want_set ? ((bm->Buffer[fw] & m) == m)
                        : ((bm->Buffer[fw] & m) == 0u);
    }
    m = range_mask(fb, BPW - 1u);
    if (want_set ? ((bm->Buffer[fw] & m) != m) : ((bm->Buffer[fw] & m) != 0u))
        return false;
    for (w = fw + 1u; w < lw; w++) {
        if (want_set ? (bm->Buffer[w] != 0xFFFFFFFFu) : (bm->Buffer[w] != 0u))
            return false;
    }
    m = range_mask(0, lb);
    return want_set ? ((bm->Buffer[lw] & m) == m) : ((bm->Buffer[lw] & m) == 0u);
}

bool RtlAreBitsSet(const RTL_BITMAP *bm, uint32_t start, uint32_t count)
{
    return are_bits(bm, start, count, true);
}

bool RtlAreBitsClear(const RTL_BITMAP *bm, uint32_t start, uint32_t count)
{
    return are_bits(bm, start, count, false);
}

uint32_t RtlNumberOfSetBits(const RTL_BITMAP *bm)
{
    uint32_t wc, i, n = 0;
    if (!bm || !bm->Buffer || bm->SizeOfBitMap == 0)
        return 0;
    wc = word_count(bm->SizeOfBitMap);
    for (i = 0; i + 1u < wc; i++)
        n += (uint32_t)__builtin_popcount(bm->Buffer[i]);
    /* Final word masked so padding bits never count. */
    n += (uint32_t)__builtin_popcount(bm->Buffer[wc - 1u] & tail_mask(bm->SizeOfBitMap));
    return n;
}

uint32_t RtlNumberOfClearBits(const RTL_BITMAP *bm)
{
    if (!bm)
        return 0;
    return bm->SizeOfBitMap - RtlNumberOfSetBits(bm);
}

/* First run of `count` contiguous bits (clear if want_set==false, set if true)
 * whose start index is >= `from` and which fits entirely within SizeOfBitMap.
 * Linear bit scan with run accumulation, O(size). */
static uint32_t find_run(const RTL_BITMAP *bm, uint32_t count, uint32_t from,
                         bool want_set)
{
    uint32_t i, run = 0;
    if (count == 0)
        return (from <= bm->SizeOfBitMap) ? from : bm->SizeOfBitMap;
    if (count > bm->SizeOfBitMap || from >= bm->SizeOfBitMap)
        return RTL_BITMAP_NOT_FOUND;
    for (i = from; i < bm->SizeOfBitMap; i++) {
        bool bit = (bm->Buffer[i / BPW] >> (i & (BPW - 1u))) & 1u;
        if (bit == want_set) {
            if (++run == count)
                return i - count + 1u;
        } else {
            run = 0;
        }
    }
    return RTL_BITMAP_NOT_FOUND;
}

/* Search [hint, size) first, then [0, size) (which covers the wrap to [0,hint)).
 * Returns the first matching run start or NOT_FOUND. */
static uint32_t find_bits(const RTL_BITMAP *bm, uint32_t count, uint32_t hint,
                          bool want_set)
{
    uint32_t r;
    if (!bm || !bm->Buffer)
        return RTL_BITMAP_NOT_FOUND;
    if (count == 0)
        return (hint <= bm->SizeOfBitMap) ? hint : bm->SizeOfBitMap;
    r = find_run(bm, count, (hint < bm->SizeOfBitMap) ? hint : 0u, want_set);
    if (r != RTL_BITMAP_NOT_FOUND)
        return r;
    if (hint == 0)
        return RTL_BITMAP_NOT_FOUND;   /* already searched from 0 */
    return find_run(bm, count, 0u, want_set);
}

uint32_t RtlFindClearBits(const RTL_BITMAP *bm, uint32_t count, uint32_t hint)
{
    return find_bits(bm, count, hint, false);
}

uint32_t RtlFindSetBits(const RTL_BITMAP *bm, uint32_t count, uint32_t hint)
{
    return find_bits(bm, count, hint, true);
}

uint32_t RtlFindClearBitsAndSet(RTL_BITMAP *bm, uint32_t count, uint32_t hint)
{
    uint32_t r = find_bits(bm, count, hint, false);
    if (r != RTL_BITMAP_NOT_FOUND && count != 0)
        RtlSetBits(bm, r, count);
    return r;
}

uint32_t RtlFindSetBitsAndClear(RTL_BITMAP *bm, uint32_t count, uint32_t hint)
{
    uint32_t r = find_bits(bm, count, hint, true);
    if (r != RTL_BITMAP_NOT_FOUND && count != 0)
        RtlClearBits(bm, r, count);
    return r;
}
