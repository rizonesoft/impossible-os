/* ============================================================================
 * ntfs_compress.c -- LZNT1 Decompression for Compressed NTFS Files
 *
 * NTFS transparent compression uses LZNT1 (LZ77 variant), applied to
 * "compression units" of 2^N clusters (typically N=4, so 16 clusters = 64 KB).
 *
 * When $DATA attribute flags include 0x0001 (compressed), the data runs
 * encode compression units as:
 *   - run_length == unit_size: stored uncompressed, read directly
 *   - run_length <  unit_size: LZNT1-compressed, decompress
 *   - run is sparse (LCN == -1): entire unit is zeros
 *
 * LZNT1 sub-block format (processes 4096-byte chunks):
 *   - 2-byte header: bit 15 = compressed flag, bits 0-11 = data size - 1
 *   - If compressed: token stream of literal bytes + back-references
 *   - If not compressed: raw 4096 bytes
 *   - Token: bit 7 = 1 -> back-reference (offset,length), 0 -> literal byte
 *   - Back-reference displacement bits vary with current output position
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* LZNT1 sub-block size is always 4096 bytes of decompressed data */
#define LZNT1_BLOCK_SIZE  4096

/* Maximum compressed sub-block data (excluding 2-byte header) */
#define LZNT1_MAX_BLOCK_DATA  4095

/* ============================================================================
 * LZNT1 Core Decompressor
 *
 * The LZNT1 algorithm operates on 4096-byte sub-blocks. A compressed stream
 * consists of one or more sub-blocks, each with a 2-byte header:
 *   bits 0-11: block data size - 1 (so value 0 means 1 byte of data)
 *   bit 12-14: signature (must be 011 = 0x3)
 *   bit 15: 1 = compressed, 0 = uncompressed (raw data follows)
 *
 * In a compressed sub-block, data is a sequence of tokens. Each group of
 * 8 tokens is preceded by a 1-byte flag; bit N of the flag controls token N:
 *   flag bit = 0: token is a literal byte (copy to output)
 *   flag bit = 1: token is a 2-byte back-reference (displacement, length)
 *
 * Back-reference encoding: the 16-bit value is split into displacement (high)
 * and length (low) fields. The split point depends on the current output
 * position within the 4096-byte block:
 *   displacement_bits = max(4, ceil(log2(output_position)))
 *   length_bits = 16 - displacement_bits
 * ============================================================================ */

/* Compute the number of displacement bits for a given output position.
 * This is the key to LZNT1's variable-length back-reference encoding.
 * At position 0: 4 bits, at position 1-15: 4 bits,
 * at positions 16-31: 5 bits, 32-63: 6 bits, etc. */
static int displacement_bits(int position)
{
    int bits = 4;  /* Minimum is 4 */
    int threshold = 0x10;  /* 16 */

    while (threshold < position && bits < 12) {
        threshold <<= 1;
        bits++;
    }

    return bits;
}

/* Decompress a single LZNT1 sub-block.
 * src: pointer to compressed data (after the 2-byte header).
 * src_len: number of bytes of compressed data.
 * dst: output buffer (must be 4096 bytes).
 * Returns number of bytes decompressed, or -1 on error. */
static int lznt1_decompress_block(const uint8_t *src, int src_len,
                                   uint8_t *dst)
{
    int src_pos = 0;
    int dst_pos = 0;

    while (src_pos < src_len && dst_pos < LZNT1_BLOCK_SIZE) {
        uint8_t flags;
        int bit;

        if (src_pos >= src_len)
            break;

        /* Read flag byte -- controls the next 8 tokens */
        flags = src[src_pos++];

        for (bit = 0; bit < 8 && src_pos < src_len &&
             dst_pos < LZNT1_BLOCK_SIZE; bit++) {

            if (!(flags & (1u << bit))) {
                /* Literal byte */
                dst[dst_pos++] = src[src_pos++];
            } else {
                /* Back-reference: 2-byte (offset, length) pair */
                uint16_t ref;
                int disp_bits;
                int len_bits;
                int displacement;
                int length;
                int copy_pos;
                int j;

                if (src_pos + 2 > src_len)
                    return dst_pos;  /* Truncated -- return what we have */

                ref = (uint16_t)src[src_pos] |
                      ((uint16_t)src[src_pos + 1] << 8);
                src_pos += 2;

                /* Compute field widths from current output position */
                disp_bits = displacement_bits(dst_pos);
                len_bits = 16 - disp_bits;

                /* Extract displacement and length */
                displacement = (ref >> len_bits) + 1;
                length = (ref & ((1 << len_bits) - 1)) + 3;

                /* Copy from earlier in the output buffer */
                copy_pos = dst_pos - displacement;
                if (copy_pos < 0)
                    return -1;  /* Invalid back-reference */

                for (j = 0; j < length && dst_pos < LZNT1_BLOCK_SIZE; j++) {
                    dst[dst_pos] = dst[copy_pos];
                    dst_pos++;
                    copy_pos++;
                }
            }
        }
    }

    return dst_pos;
}

int ntfs_lznt1_decompress(const uint8_t *src, uint32_t src_len,
                          uint8_t *dst, uint32_t dst_len)
{
    uint32_t src_pos = 0;
    uint32_t dst_pos = 0;

    if (!src || !dst || src_len == 0 || dst_len == 0)
        return -1;

    while (src_pos + 2 <= src_len && dst_pos < dst_len) {
        uint16_t block_header;
        uint16_t block_size;
        int is_compressed;
        int decomp_result;

        /* Read 2-byte sub-block header */
        block_header = (uint16_t)src[src_pos] |
                       ((uint16_t)src[src_pos + 1] << 8);
        src_pos += 2;

        /* A header of 0x0000 means end of compressed stream */
        if (block_header == 0)
            break;

        /* Parse header fields */
        block_size = (block_header & 0x0FFF) + 1;  /* bits 0-11: size - 1 */
        is_compressed = (block_header & 0x8000) ? 1 : 0;  /* bit 15 */

        if (src_pos + block_size > src_len)
            break;  /* Truncated stream */

        if (is_compressed) {
            /* Compressed sub-block -- decompress via LZNT1 */
            uint8_t temp[LZNT1_BLOCK_SIZE];

            decomp_result = lznt1_decompress_block(src + src_pos,
                                                    (int)block_size,
                                                    temp);
            if (decomp_result < 0)
                return -1;

            /* Copy decompressed data to output */
            {
                uint32_t to_copy = (uint32_t)decomp_result;
                if (dst_pos + to_copy > dst_len)
                    to_copy = dst_len - dst_pos;
                ntfs_memcpy(dst + dst_pos, temp, to_copy);
                dst_pos += to_copy;
            }
        } else {
            /* Uncompressed sub-block -- raw data */
            uint32_t to_copy = block_size;
            if (dst_pos + to_copy > dst_len)
                to_copy = dst_len - dst_pos;
            ntfs_memcpy(dst + dst_pos, src + src_pos, to_copy);
            dst_pos += to_copy;
        }

        src_pos += block_size;
    }

    return (int)dst_pos;
}

/* ============================================================================
 * Compressed file data reader
 *
 * Decompresses data on-the-fly for files with the compressed flag.
 * Processes compression units (typically 16 clusters = 64 KB each):
 *   - Scans data runs to identify compression unit boundaries
 *   - Reads compressed data from disk
 *   - Decompresses via LZNT1
 *   - Copies the requested byte range from the decompressed output
 * ============================================================================ */

int64_t ntfs_read_compressed_data(struct ntfs_volume *vol,
                                  const struct ntfs_data_run *runs,
                                  int run_count,
                                  uint64_t real_size,
                                  uint16_t compression_unit_shift,
                                  uint64_t file_offset,
                                  uint64_t length,
                                  void *buffer)
{
    uint8_t *buf = (uint8_t *)buffer;
    uint64_t cluster_size;
    uint64_t cu_clusters;  /* Clusters per compression unit */
    uint64_t cu_bytes;     /* Bytes per compression unit */
    uint64_t bytes_read = 0;

    if (!vol || !runs || !buffer || run_count <= 0)
        return -1;

    cluster_size = vol->cluster_size;

    /* Compression unit size: 2^(shift) clusters */
    if (compression_unit_shift == 0 || compression_unit_shift > 16)
        return -1;  /* Invalid compression unit */
    cu_clusters = 1ULL << compression_unit_shift;
    cu_bytes = cu_clusters * cluster_size;

    /* Cap read at real_size */
    if (file_offset >= real_size)
        return 0;
    if (file_offset + length > real_size)
        length = real_size - file_offset;

    /* Process each compression unit that overlaps the requested range */
    while (bytes_read < length) {
        uint64_t abs_pos = file_offset + bytes_read;
        uint64_t cu_index = abs_pos / cu_bytes;       /* Which CU? */
        uint64_t cu_start_vcn = cu_index * cu_clusters;
        uint64_t offset_in_cu = abs_pos % cu_bytes;   /* Offset within CU */
        uint64_t want = length - bytes_read;
        uint64_t avail = cu_bytes - offset_in_cu;
        uint64_t chunk;
        int ri;
        uint64_t cu_disk_clusters = 0;
        int cu_is_sparse = 1;
        uint64_t cu_first_lcn = 0;

        if (want > avail)
            want = avail;
        chunk = want;

        /* Find how many clusters are allocated for this CU by scanning runs.
         * A compression unit spans cu_clusters virtual clusters starting
         * at cu_start_vcn. We need to count how many physical clusters
         * are allocated across that range. */
        cu_disk_clusters = 0;
        cu_is_sparse = 1;
        cu_first_lcn = 0;

        for (ri = 0; ri < run_count; ri++) {
            uint64_t run_start = runs[ri].vcn_start;
            uint64_t run_end = run_start + runs[ri].length;
            uint64_t cu_end_vcn = cu_start_vcn + cu_clusters;

            /* Does this run overlap the CU range? */
            if (run_end <= cu_start_vcn || run_start >= cu_end_vcn)
                continue;

            if (runs[ri].lcn != NTFS_LCN_SPARSE) {
                /* Non-sparse run overlapping this CU */
                uint64_t overlap_start = (run_start > cu_start_vcn) ?
                                          run_start : cu_start_vcn;
                uint64_t overlap_end = (run_end < cu_end_vcn) ?
                                        run_end : cu_end_vcn;
                uint64_t overlap = overlap_end - overlap_start;

                if (cu_is_sparse) {
                    /* First non-sparse run -- record its LCN */
                    uint64_t vcn_offset = overlap_start - run_start;
                    cu_first_lcn = runs[ri].lcn + vcn_offset;
                    cu_is_sparse = 0;
                }
                cu_disk_clusters += overlap;
            }
        }

        if (cu_is_sparse) {
            /* Entire CU is sparse -- fill with zeros */
            ntfs_memset(buf + bytes_read, 0, chunk);
        } else if (cu_disk_clusters >= cu_clusters) {
            /* Full CU stored uncompressed -- read directly */
            int64_t direct_read = ntfs_read_data(vol, runs, run_count,
                                                  real_size, abs_pos,
                                                  chunk, buf + bytes_read);
            if (direct_read < 0)
                return -1;
            chunk = (uint64_t)direct_read;
        } else {
            /* Compressed CU -- disk clusters < unit size */
            uint32_t comp_bytes = (uint32_t)(cu_disk_clusters * cluster_size);
            uint8_t *comp_buf;
            uint8_t *decomp_buf;
            int decomp_result;

            /* Allocate bounce buffers for compressed + decompressed data */
            comp_buf = (uint8_t *)kmalloc(comp_bytes);
            if (!comp_buf)
                return -1;

            decomp_buf = (uint8_t *)kmalloc((uint32_t)cu_bytes);
            if (!decomp_buf) {
                kfree(comp_buf);
                return -1;
            }

            /* Read compressed data from disk. The compressed clusters are
             * contiguous starting at cu_first_lcn. */
            {
                uint64_t disk_lba = (cu_first_lcn * cluster_size) /
                                    vol->bytes_per_sector;
                uint32_t sect_count = comp_bytes / vol->bytes_per_sector;
                if (sect_count == 0)
                    sect_count = 1;

                if (blkdev_read(vol->dev, disk_lba, sect_count,
                                comp_buf) != 0) {
                    kfree(comp_buf);
                    kfree(decomp_buf);
                    return -1;
                }
            }

            /* Decompress */
            ntfs_memset(decomp_buf, 0, cu_bytes);
            decomp_result = ntfs_lznt1_decompress(comp_buf, comp_bytes,
                                                   decomp_buf,
                                                   (uint32_t)cu_bytes);

            kfree(comp_buf);

            if (decomp_result < 0) {
                kfree(decomp_buf);
                klog(LOG_ERROR, "ntfs",
                     "LZNT1 decompression failed at CU %llu",
                     cu_index);
                return -1;
            }

            /* Copy requested range from decompressed buffer.
             * Use cu_bytes (not decomp_result) as the available size because
             * the buffer was pre-zeroed to cu_bytes -- any bytes beyond
             * decomp_result are valid zeros (sub-block padding). */
            {
                uint32_t to_copy = (uint32_t)chunk;
                if (offset_in_cu + to_copy > (uint32_t)cu_bytes)
                    to_copy = (uint32_t)cu_bytes -
                              (uint32_t)offset_in_cu;
                ntfs_memcpy(buf + bytes_read,
                            decomp_buf + offset_in_cu, to_copy);
                chunk = to_copy;
            }

            kfree(decomp_buf);
        }

        bytes_read += chunk;
        if (chunk == 0)
            break;  /* No progress -- prevent infinite loop */
    }

    return (int64_t)bytes_read;
}

/* ============================================================================
 * LZNT1 Compressor
 *
 * Produces a byte stream that ntfs_lznt1_decompress() can consume exactly.
 * The stream is a sequence of 4096-byte sub-blocks:
 *   - 2-byte header: bits 0-11 = data_size-1, bits 12-14 = 0x3, bit 15 = compressed?
 *   - If compressed:  token stream (flag bytes + literal/back-ref tokens)
 *   - If !compressed: raw 4096 bytes
 *
 * Match finder: hash-chain on 3-byte sequences, bucket=hash(src[i..i+2])%256,
 * chain depth limited to 16 steps per match.  Minimum match = 3 bytes.
 * ============================================================================ */

#define LZNT1_MAX_RUNS_PER_FILE  64  /* Max data runs tracked per write (matches ntfs_data_write.c) */
#define LZNT1_HASH_BITS  8          /* 256 buckets -- small, fits on stack */
#define LZNT1_HASH_SIZE  (1 << LZNT1_HASH_BITS)
#define LZNT1_CHAIN_DEPTH  16       /* Max steps in the hash chain per byte */

/* Pack 3 bytes into a hash bucket index */
static int lznt1_hash3(const uint8_t *p)
{
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return (int)(((v ^ (v >> 8)) ^ (v >> 16)) & (LZNT1_HASH_SIZE - 1));
}

/* Compress a single 4096-byte sub-block using LZNT1.
 * src:     input buffer, exactly src_len bytes (≤ 4096).
 * dst:     output buffer (must be at least src_len + 2 bytes).
 * Returns total bytes written (including 2-byte header),
 * or 0 if the result wouldn't fit in dst or is larger than src. */
static int lznt1_compress_block(const uint8_t *src, int src_len,
                                uint8_t *dst, int dst_max)
{
    /* Hash table: head[h] = last position in src that hashed to h, or -1 */
    int head[LZNT1_HASH_SIZE];
    /* Linked list: prev[i] = previous position that shares the same hash */
    int prev[LZNT1_BLOCK_SIZE];

    /* Output buffer -- skip 2 bytes for the header we'll fill at the end */
    uint8_t out[LZNT1_BLOCK_SIZE + 16];   /* +16 headroom */
    int out_pos = 0;

    int src_pos = 0;
    int i;

    /* Token group state: one flag byte controls 8 tokens */
    int flag_pos;       /* Position of current flag byte in out[] */
    int token_count;    /* Tokens in current group (0–7) */
    uint8_t flag;       /* Current flag byte */

    if (src_len <= 0 || dst_max < src_len + 2)
        return 0;

    /* Initialize hash table */
    for (i = 0; i < LZNT1_HASH_SIZE; i++)
        head[i] = -1;
    for (i = 0; i < LZNT1_BLOCK_SIZE; i++)
        prev[i] = -1;

    /* Start first token group */
    flag_pos    = out_pos++;
    token_count = 0;
    flag        = 0;

    while (src_pos < src_len) {
        int best_len  = 0;
        int best_disp = 0;
        int disp_bits;
        int len_bits;
        int max_len;
        int max_disp;

        /* Compute current displacement-bits from output position */
        disp_bits = displacement_bits(src_pos);
        len_bits  = 16 - disp_bits;
        max_len   = (1 << len_bits) - 1 + 3;   /* max encodeable length */
        max_disp  = 1 << disp_bits;             /* max look-back distance */

        /* Try to find a match via hash chain (only if ≥ 3 bytes remain) */
        if (src_pos + 3 <= src_len) {
            int h     = lznt1_hash3(src + src_pos);
            int steps = 0;
            int j     = head[h];

            while (j >= 0 && steps < LZNT1_CHAIN_DEPTH) {
                int disp = src_pos - j;
                if (disp > max_disp)
                    break;  /* Too far back -- chain sorted by position */

                /* Measure match length */
                {
                    int avail = src_len - src_pos;
                    int mlen  = 0;
                    if (avail > max_len) avail = max_len;
                    while (mlen < avail && src[j + mlen] == src[src_pos + mlen])
                        mlen++;

                    if (mlen >= 3 && mlen > best_len) {
                        best_len  = mlen;
                        best_disp = disp;
                        if (mlen == max_len)
                            break;  /* Can't do better */
                    }
                }

                prev[j] = head[h];  /* shouldn't happen but guard */
                j = prev[j];
                steps++;
            }
        }

        /* Update hash chain for src_pos */
        if (src_pos + 3 <= src_len) {
            int h       = lznt1_hash3(src + src_pos);
            prev[src_pos] = head[h];
            head[h]       = src_pos;
        }

        if (best_len >= 3) {
            /* Emit a back-reference token */
            uint16_t encoded;
            int length_code;
            int disp_code;

            /* Recompute fields (position hasn't changed yet) */
            disp_bits   = displacement_bits(src_pos);
            len_bits    = 16 - disp_bits;
            length_code = best_len - 3;
            disp_code   = best_disp - 1;

            encoded = (uint16_t)(((uint16_t)disp_code << len_bits) |
                                 (uint16_t)length_code);

            /* Guard: output must have room for 2 token bytes */
            if (out_pos + 2 > (int)sizeof(out))
                goto store_raw;  /* Compressed output exceeded limit */

            flag |= (uint8_t)(1u << token_count);
            out[out_pos++] = (uint8_t)(encoded & 0xFF);
            out[out_pos++] = (uint8_t)(encoded >> 8);

            /* Advance hash chain for all consumed bytes */
            {
                int k;
                for (k = 1; k < best_len; k++) {
                    int pos = src_pos + k;
                    if (pos + 3 <= src_len) {
                        int h2       = lznt1_hash3(src + pos);
                        prev[pos]    = head[h2];
                        head[h2]     = pos;
                    }
                }
            }

            src_pos += best_len;
        } else {
            /* Emit literal byte (flag bit = 0, already cleared) */
            if (out_pos + 1 > (int)sizeof(out))
                goto store_raw;

            out[out_pos++] = src[src_pos++];
        }

        token_count++;

        /* When we have 8 tokens, flush the group */
        if (token_count == 8) {
            out[flag_pos] = flag;
            flag_pos      = out_pos++;
            token_count   = 0;
            flag          = 0;
        }

        /* If compressed output is already as large as the input, give up */
        if (out_pos + 2 >= src_len)
            goto store_raw;
    }

    /* Flush the final partial group (token_count may be 0..7) */
    if (token_count > 0)
        out[flag_pos] = flag;
    else
        out_pos = flag_pos;  /* Remove the trailing empty flag byte */

    /* Build header: compressed sub-block */
    {
        int data_size = out_pos;
        uint16_t hdr  = (uint16_t)((data_size - 1) |
                                    (0x3 << 12)     |
                                    (1u   << 15));   /* bit 15 = compressed */

        if (2 + data_size > dst_max)
            goto store_raw;

        dst[0] = (uint8_t)(hdr & 0xFF);
        dst[1] = (uint8_t)(hdr >> 8);
        ntfs_memcpy(dst + 2, out, (uint32_t)data_size);
        return 2 + data_size;
    }

store_raw:
    /* Store sub-block uncompressed: header (bit 15 = 0) + raw data */
    if (2 + src_len > dst_max)
        return 0;

    {
        uint16_t hdr = (uint16_t)((src_len - 1) | (0x3 << 12));
        dst[0] = (uint8_t)(hdr & 0xFF);
        dst[1] = (uint8_t)(hdr >> 8);
        ntfs_memcpy(dst + 2, src, (uint32_t)src_len);
        return 2 + src_len;
    }
}

int ntfs_lznt1_compress(const uint8_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_len)
{
    uint32_t src_pos = 0;
    uint32_t dst_pos = 0;

    if (!src || !dst || src_len == 0 || dst_len == 0)
        return -1;

    while (src_pos < src_len) {
        int block_input = (int)(src_len - src_pos);
        int written;

        if (block_input > LZNT1_BLOCK_SIZE)
            block_input = LZNT1_BLOCK_SIZE;

        /* Need room for at least 2-byte header + compressed data */
        if (dst_pos + 2 + (uint32_t)block_input > dst_len)
            return -1;  /* Output buffer too small */

        written = lznt1_compress_block(src + src_pos, block_input,
                                        dst + dst_pos,
                                        (int)(dst_len - dst_pos));
        if (written <= 0)
            return -1;

        src_pos += (uint32_t)block_input;
        dst_pos += (uint32_t)written;
    }

    /* Terminating 2-byte null header (marks end of compressed stream) */
    if (dst_pos + 2 > dst_len)
        return -1;
    dst[dst_pos++] = 0;
    dst[dst_pos++] = 0;

    return (int)dst_pos;
}

/* ============================================================================
 * Compressed file data writer
 *
 * Writes data to a compressed NTFS file by compression unit (CU), typically
 * 16 clusters = 64 KB.  For each CU:
 *   - All-zero input -> sparse run (no disk allocation)
 *   - Compresses well -> allocate fewer clusters, write compressed data
 *   - Doesn't compress -> allocate full CU, write raw data
 *
 * Partial CU writes use read-modify-write: decompress existing CU data,
 * apply the modification, recompress, write back.
 *
 * NOTE: This function handles SINGLE compression-unit-aligned chunks.
 * The higher-level ntfs_write_data() is responsible for splitting the
 * write across CU boundaries and detecting the compressed flag.
 * ============================================================================ */

/* Check whether a buffer is entirely zero. */
static int buf_is_all_zero(const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (buf[i]) return 0;
    }
    return 1;
}

/* Write the compressed data for one full compression unit.
 * cu_data:    uncompressed CU data (exactly cu_bytes bytes).
 * cu_bytes:   bytes per compression unit.
 * cu_start_vcn: first VCN of this CU.
 * runs / run_count: current file runs (updated in-place).
 * Returns NTFS_OK or NTFS_ERR_*. */
static int write_one_compressed_cu(struct ntfs_volume *vol,
                                    const uint8_t *cu_data,
                                    uint64_t cu_bytes,
                                    uint64_t cu_clusters,
                                    uint64_t cu_start_vcn,
                                    struct ntfs_data_run *runs,
                                    int *run_count)
{
    uint64_t cs = vol->cluster_size;
    uint64_t hint_lcn;
    int i;

    /* Find hint LCN from last non-sparse run */
    hint_lcn = vol->mft_lcn;
    for (i = 0; i < *run_count; i++) {
        if (runs[i].lcn != NTFS_LCN_SPARSE)
            hint_lcn = runs[i].lcn + runs[i].length;
    }

    /* Remove any existing runs that overlap this CU (free their clusters) */
    {
        uint64_t cu_end_vcn = cu_start_vcn + cu_clusters;
        int src, dst;

        for (src = 0; src < *run_count; src++) {
            uint64_t rstart = runs[src].vcn_start;
            uint64_t rend   = rstart + runs[src].length;
            if (rend > cu_start_vcn && rstart < cu_end_vcn) {
                /* Overlap -- free the clusters */
                if (runs[src].lcn != NTFS_LCN_SPARSE)
                    ntfs_free_clusters(vol, runs[src].lcn, runs[src].length);
                runs[src].length = 0;   /* Mark for removal */
            }
        }

        /* Compact run list */
        dst = 0;
        for (src = 0; src < *run_count; src++) {
            if (runs[src].length > 0)
                runs[dst++] = runs[src];
        }
        *run_count = dst;
    }

    /* Case 1: All-zero -- sparse run (no disk allocation) */
    if (buf_is_all_zero(cu_data, (uint32_t)cu_bytes)) {
        if (*run_count >= LZNT1_MAX_RUNS_PER_FILE - 1)
            return NTFS_ERR_IO;
        runs[*run_count].vcn_start = cu_start_vcn;
        runs[*run_count].lcn       = NTFS_LCN_SPARSE;
        runs[*run_count].length    = cu_clusters;
        (*run_count)++;
        klog(LOG_DEBUG, "ntfs", "compress: CU@%llu -> sparse", cu_start_vcn);
        return NTFS_OK;
    }

    /* Compress the CU data */
    {
        uint32_t comp_max = (uint32_t)(cu_bytes + 16);  /* +16 for headers */
        uint8_t *comp_buf = (uint8_t *)kmalloc(comp_max);
        int comp_len;
        uint64_t comp_clusters;
        uint64_t alloc_lcn;
        uint64_t disk_lba;
        uint32_t sect_count;

        if (!comp_buf)
            return NTFS_ERR_IO;

        comp_len = ntfs_lznt1_compress(cu_data, (uint32_t)cu_bytes,
                                        comp_buf, comp_max);

        if (comp_len <= 0 || (uint64_t)comp_len >= cu_bytes) {
            /* Compression didn't help -- store uncompressed (full CU) */
            kfree(comp_buf);
            goto store_uncompressed;
        }

        /* Allocate only as many clusters as needed for the compressed data */
        comp_clusters = ((uint64_t)comp_len + cs - 1) / cs;
        if (comp_clusters >= cu_clusters) {
            kfree(comp_buf);
            goto store_uncompressed;
        }

        alloc_lcn = ntfs_alloc_clusters(vol, comp_clusters, hint_lcn);
        if (alloc_lcn == 0) {
            kfree(comp_buf);
            return NTFS_ERR_FULL;
        }

        /* Write compressed data */
        disk_lba   = (alloc_lcn * cs) / vol->bytes_per_sector;
        sect_count = (uint32_t)(((uint64_t)comp_len + vol->bytes_per_sector - 1)
                                / vol->bytes_per_sector);
        if (sect_count == 0) sect_count = 1;

        {
            uint32_t write_size = sect_count * vol->bytes_per_sector;
            uint8_t *write_buf  = (uint8_t *)kmalloc(write_size);
            int wrc;

            if (!write_buf) {
                ntfs_free_clusters(vol, alloc_lcn, comp_clusters);
                kfree(comp_buf);
                return NTFS_ERR_IO;
            }

            ntfs_memset(write_buf, 0, write_size);
            ntfs_memcpy(write_buf, comp_buf, (uint32_t)comp_len);

            wrc = blkdev_write(vol->dev, disk_lba, sect_count, write_buf);
            kfree(write_buf);
            kfree(comp_buf);

            if (wrc != 0) {
                ntfs_free_clusters(vol, alloc_lcn, comp_clusters);
                return NTFS_ERR_IO;
            }
        }

        /* Append compressed run (comp_clusters < cu_clusters) + trailing sparse */
        if (*run_count + 2 > LZNT1_MAX_RUNS_PER_FILE) {
            ntfs_free_clusters(vol, alloc_lcn, comp_clusters);
            return NTFS_ERR_IO;
        }

        runs[*run_count].vcn_start = cu_start_vcn;
        runs[*run_count].lcn       = alloc_lcn;
        runs[*run_count].length    = comp_clusters;
        (*run_count)++;

        /* The remaining (cu_clusters − comp_clusters) VCNs have no LCN --
         * this is what signals NTFS that the CU is compressed (§9.1 read path). */
        runs[*run_count].vcn_start = cu_start_vcn + comp_clusters;
        runs[*run_count].lcn       = NTFS_LCN_SPARSE;
        runs[*run_count].length    = cu_clusters - comp_clusters;
        (*run_count)++;

        klog(LOG_DEBUG, "ntfs",
             "compress: CU@%llu -> %llu/%llu clusters (%d bytes)",
             cu_start_vcn, comp_clusters, cu_clusters, comp_len);
        return NTFS_OK;
    }

store_uncompressed:
    {
        /* Allocate full CU worth of clusters */
        uint64_t alloc_lcn = ntfs_alloc_clusters(vol, cu_clusters, hint_lcn);
        if (alloc_lcn == 0)
            return NTFS_ERR_FULL;

        /* Write raw CU data */
        {
            uint64_t disk_lba  = (alloc_lcn * cs) / vol->bytes_per_sector;
            uint32_t sect_cnt  = (uint32_t)(cu_bytes / vol->bytes_per_sector);
            int wrc;

            if (sect_cnt == 0) sect_cnt = 1;
            wrc = blkdev_write(vol->dev, disk_lba, sect_cnt,
                                (const void *)cu_data);
            if (wrc != 0) {
                ntfs_free_clusters(vol, alloc_lcn, cu_clusters);
                return NTFS_ERR_IO;
            }
        }

        if (*run_count >= LZNT1_MAX_RUNS_PER_FILE) {
            ntfs_free_clusters(vol, alloc_lcn, cu_clusters);
            return NTFS_ERR_IO;
        }

        runs[*run_count].vcn_start = cu_start_vcn;
        runs[*run_count].lcn       = alloc_lcn;
        runs[*run_count].length    = cu_clusters;
        (*run_count)++;

        klog(LOG_DEBUG, "ntfs",
             "compress: CU@%llu -> uncompressed (%llu clusters)",
             cu_start_vcn, cu_clusters);
        return NTFS_OK;
    }
}

int ntfs_write_compressed_data(struct ntfs_volume *vol, uint64_t inode,
                                uint64_t offset, uint64_t length,
                                const void *buffer)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *data_attr;
    struct ntfs_data_run runs[LZNT1_MAX_RUNS_PER_FILE];
    struct ntfs_nonres_header nrhdr;
    int run_count;
    uint32_t attr_off;
    uint64_t cu_clusters;
    uint64_t cu_bytes;
    uint64_t cs;
    uint64_t written = 0;
    struct ntfs_txn *txn;
    int rc = NTFS_OK;

    if (!vol || !buffer || length == 0)
        return NTFS_ERR_IO;

    cs  = vol->cluster_size;
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    data_attr = ntfs_attr_find(rec, &hdr, NTFS_ATTR_DATA, &ah);
    if (!data_attr || !ah.non_resident) {
        kfree(rec);
        return NTFS_ERR_IO;
    }

    run_count = ntfs_decode_data_runs(data_attr, runs,
                                       LZNT1_MAX_RUNS_PER_FILE, &nrhdr);
    if (run_count < 0) {
        kfree(rec);
        return NTFS_ERR_BAD_MAGIC;
    }

    if (nrhdr.compression_unit == 0) {
        /* Not a compressed attribute -- fall through to normal write */
        kfree(rec);
        return NTFS_ERR_IO;
    }

    cu_clusters = 1ULL << nrhdr.compression_unit;
    cu_bytes    = cu_clusters * cs;
    attr_off    = (uint32_t)(data_attr - rec);

    txn = ntfs_txn_begin(vol);
    if (!txn) {
        kfree(rec);
        return NTFS_ERR_IO;
    }

    /* Process each CU that the write range touches */
    while (written < length) {
        uint64_t abs_pos     = offset + written;
        uint64_t cu_index    = abs_pos / cu_bytes;
        uint64_t cu_start    = cu_index * cu_bytes;
        uint64_t cu_start_vcn = cu_index * cu_clusters;
        uint64_t off_in_cu   = abs_pos - cu_start;
        uint64_t want        = length - written;
        uint64_t avail       = cu_bytes - off_in_cu;
        uint64_t chunk       = (want < avail) ? want : avail;
        uint8_t *cu_plain;

        /* Allocate a CU-sized working buffer */
        cu_plain = (uint8_t *)kmalloc((uint32_t)cu_bytes);
        if (!cu_plain) {
            rc = NTFS_ERR_IO;
            break;
        }

        ntfs_memset(cu_plain, 0, cu_bytes);

        /* If this is a partial CU write, read back the existing data first */
        if (off_in_cu > 0 || chunk < cu_bytes) {
            int64_t rr = ntfs_read_compressed_data(vol, runs, run_count,
                                                    nrhdr.real_size,
                                                    nrhdr.compression_unit,
                                                    cu_start, cu_bytes,
                                                    cu_plain);
            if (rr < 0) {
                /* No existing data (e.g. new file) -- treat as zeroes */
                ntfs_memset(cu_plain, 0, cu_bytes);
            }
        }

        /* Apply the caller's data into the CU buffer */
        ntfs_memcpy(cu_plain + off_in_cu,
                    (const uint8_t *)buffer + written,
                    (uint32_t)chunk);

        /* Compress and write this CU */
        rc = write_one_compressed_cu(vol, cu_plain,
                                      cu_bytes, cu_clusters,
                                      cu_start_vcn,
                                      runs, &run_count);
        kfree(cu_plain);

        if (rc != NTFS_OK)
            break;

        written += chunk;
    }

    if (rc == NTFS_OK) {
        uint64_t new_real_size = offset + length;
        if (new_real_size < nrhdr.real_size)
            new_real_size = nrhdr.real_size;

        /* Compute total allocated size from runs */
        {
            uint64_t total_alloc_vcn = 0;
            int i;
            for (i = 0; i < run_count; i++)
                total_alloc_vcn += runs[i].length;
            nrhdr.alloc_size = total_alloc_vcn * cs;
        }

        nrhdr.real_size = new_real_size;

        /* Journal the run-list change */
        ntfs_txn_log(txn,
                     NTFS_LOG_OP_UPDATE_MAPPING, runs,
                     (uint16_t)(run_count * sizeof(struct ntfs_data_run)),
                     NTFS_LOG_OP_UPDATE_MAPPING, NULL, 0,
                     inode, (uint16_t)attr_off);

        /* Write updated non-resident $DATA attribute header back to MFT */
        {
            uint64_t last_vcn    = 0;
            uint8_t  run_buf[512];
            int      encoded;
            int      i;

            for (i = 0; i < run_count; i++)
                last_vcn += runs[i].length;
            if (last_vcn > 0) last_vcn--;

            encoded = ntfs_encode_data_runs(runs, run_count, run_buf, 512);
            if (encoded > 0) {
                uint16_t run_off = ntfs_le16(rec + attr_off + 0x20);
                ntfs_le64_write(rec + attr_off + 0x18, last_vcn);
                ntfs_le64_write(rec + attr_off + 0x28, nrhdr.alloc_size);
                ntfs_le64_write(rec + attr_off + 0x30, nrhdr.real_size);
                ntfs_le64_write(rec + attr_off + 0x38, nrhdr.real_size);
                ntfs_memcpy(rec + attr_off + run_off, run_buf, (uint32_t)encoded);
            }
        }

        ntfs_txn_commit(txn);
        rc = ntfs_write_mft_record(vol, inode, rec);
    } else {
        ntfs_txn_abort(txn);
    }

    ntfs_txn_free(txn);
    kfree(rec);
    return rc;
}

