/* ============================================================================
 * ntfs_compress.c — LZNT1 Decompression for Compressed NTFS Files
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
 *   - Token: bit 7 = 1 → back-reference (offset,length), 0 → literal byte
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

        /* Read flag byte — controls the next 8 tokens */
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
                    return dst_pos;  /* Truncated — return what we have */

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
            /* Compressed sub-block — decompress via LZNT1 */
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
            /* Uncompressed sub-block — raw data */
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
                    /* First non-sparse run — record its LCN */
                    uint64_t vcn_offset = overlap_start - run_start;
                    cu_first_lcn = runs[ri].lcn + vcn_offset;
                    cu_is_sparse = 0;
                }
                cu_disk_clusters += overlap;
            }
        }

        if (cu_is_sparse) {
            /* Entire CU is sparse — fill with zeros */
            ntfs_memset(buf + bytes_read, 0, chunk);
        } else if (cu_disk_clusters >= cu_clusters) {
            /* Full CU stored uncompressed — read directly */
            int64_t direct_read = ntfs_read_data(vol, runs, run_count,
                                                  real_size, abs_pos,
                                                  chunk, buf + bytes_read);
            if (direct_read < 0)
                return -1;
            chunk = (uint64_t)direct_read;
        } else {
            /* Compressed CU — disk clusters < unit size */
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

            /* Copy requested range from decompressed buffer */
            {
                uint32_t to_copy = (uint32_t)chunk;
                if (offset_in_cu + to_copy > (uint32_t)decomp_result)
                    to_copy = (uint32_t)decomp_result -
                              (uint32_t)offset_in_cu;
                ntfs_memcpy(buf + bytes_read,
                            decomp_buf + offset_in_cu, to_copy);
                chunk = to_copy;
            }

            kfree(decomp_buf);
        }

        bytes_read += chunk;
        if (chunk == 0)
            break;  /* No progress — prevent infinite loop */
    }

    return (int64_t)bytes_read;
}
