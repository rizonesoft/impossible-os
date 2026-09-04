/* ============================================================================
 * hibernate.h -- S4 hibernation image format and LZ4 chunk codec
 *
 * The AUTHORITATIVE on-disk format for an Impossible OS hibernation image.
 * Two independently compiled binaries parse this stream: the kernel (this
 * header) and the UEFI bootloader, which discovers and validates the image
 * before the kernel exists. That makes it a cross-binary ABI carrying the
 * same discipline as boot_info -- every offset pinned by _Static_assert,
 * every width fixed, and one canonical byte stream both sides must agree on.
 *
 * CANONICAL BYTE STREAM (normative; a reader that disagrees is wrong):
 *   - Little-endian, fixed-width, naturally aligned, no implicit padding in
 *     either the header or a chunk descriptor (asserted below).
 *   - The header occupies bytes [0, HIBER_HEADER_BYTES). The payload starts
 *     at exactly HIBER_HEADER_BYTES; there is no gap and no alignment slack.
 *   - `image_bytes` is the TOTAL encoded size INCLUDING the header, and it is
 *     the ONLY thing that says where the image ends. A decode that does not
 *     land exactly on image_bytes is a failure.
 *   - The `img_len` a caller passes is its BUFFER CAPACITY, not the image
 *     length: a reader working in sectors or pages almost always holds more
 *     bytes than the image occupies. Anything at or past image_bytes is
 *     outside the image, is covered by no checksum, and is never interpreted.
 *     A suffix the header DECLARES but no chunk accounts for is a different
 *     thing entirely and is refused (HIBER_ERR_TRAILING), which is what stops
 *     two parsers disagreeing about where the content ends.
 *   - Reserved bytes MUST be zero. `header_crc32c` covers all
 *     HIBER_HEADER_BYTES with its own 4 bytes taken as zero, so the reserved
 *     area is covered and cannot carry smuggled data.
 *   - The payload is a sequence of `chunk_count` records, each a
 *     hiber_chunk_desc_t immediately followed by `stored_len` bytes.
 *   - `payload_crc32c` is CRC-32C over the payload region exactly as stored,
 *     descriptors included: bytes [HIBER_HEADER_BYTES, image_bytes).
 *   - Unknown flag bits are REJECTED, never ignored, so a newer producer
 *     cannot be silently half-understood by an older reader.
 *
 * DESTINATION MAP: a hibernation image restores SPARSE physical pages, so
 * every chunk carries the physical frame it restores to. Descriptors are
 * strictly ascending by start_pfn and non-overlapping, which makes the chunk
 * sequence itself the destination map -- there is no separate table that can
 * fall out of sync with the data it describes. Every extent must also end at
 * or below HIBER_MAX_PFN INCLUSIVE, so a PFN can always be converted to a
 * byte address without wrapping.
 *
 * TRUST ORDER: this codec detects CORRUPTION, it does not AUTHENTICATE.
 * Decoded bytes stay PROVISIONAL until hibernate_image_decode_finish()
 * returns HIBER_OK, and an image read from storage an attacker can reach
 * must additionally have its AEAD tag verified before any decoded page is
 * copied to its physical destination. The AEAD fields below and the
 * `resume_generation` anti-replay counter are DEFINED here and POPULATED by
 * the write path. -> XREF: 02-kernel-core/TODO-26-power-management.md, the hibernation write-path section.
 *
 * SMP: every function is pure and touches only caller-owned state -- no
 * globals, no allocation, no locking, so two CPUs may encode two images
 * concurrently. Buffers larger than 4 KiB come from pmm_alloc_contiguous(),
 * never kmalloc().
 *
 * XREF: 02-kernel-core/TODO-26-power-management.md section 4
 * ============================================================================ */

#ifndef KERNEL_PM_HIBERNATE_H
#define KERNEL_PM_HIBERNATE_H

#include "kernel/types.h"

/* "IPOSHIBR" read as a little-endian u64. */
#define HIBER_MAGIC             0x52424948534F5049ull

/* Bumped on any change to the byte stream above. A reader refuses a version
 * it was not built for rather than guessing at the layout. */
#define HIBER_FORMAT_VERSION    1u

#define HIBER_HEADER_BYTES      4096u
#define HIBER_PAGE_SIZE         4096u

/* One chunk covers at most this many physically contiguous pages. 64 pages
 * (256 KiB) is the compression unit: large enough for LZ4 to find real
 * redundancy, small enough that the scratch-free encoder writes its output
 * straight into the caller's image buffer. */
#define HIBER_CHUNK_PAGES       64u
#define HIBER_CHUNK_BYTES       (HIBER_CHUNK_PAGES * HIBER_PAGE_SIZE)

/* Opaque, collision-resistant identity of the exact kernel artifact that
 * wrote the image. The codec never derives this -- it is supplied by the
 * caller and compared byte for byte, so its strength is a property of the
 * producer rather than of this file. */
#define HIBER_KERNEL_ID_BYTES   32u

/* The highest physical frame the format may name. A restore consumer turns a
 * PFN into a byte address, so a PFN above this wraps and would redirect
 * restored pages to the wrong memory. This frame is INCLUSIVE: its page spans
 * 0xFFFFFFFFFFFFF000 through 0xFFFFFFFFFFFFFFFF and is fully addressable, so
 * an extent whose LAST frame equals it is legal. Both the encoder and the
 * decoder refuse an extent whose last included frame runs past it, which also
 * subsumes the plain 64-bit wrap of start_pfn + page_count. */
#define HIBER_MAX_PFN           (0xFFFFFFFFFFFFFFFFull / HIBER_PAGE_SIZE)

#define HIBER_AEAD_NONCE_BYTES  12u
#define HIBER_AEAD_TAG_BYTES    16u

/* resume_type: what the image is FOR. A fast-startup image is not a full
 * hibernate and must not be resumed as one. */
#define HIBER_RESUME_FULL           1u
#define HIBER_RESUME_FAST_STARTUP   2u
#define HIBER_RESUME_CRASH_TEST     3u

/* aead_cipher_id. NONE states plaintext explicitly instead of implying it by
 * leaving a field zero-by-accident. */
#define HIBER_CIPHER_NONE           0u
#define HIBER_CIPHER_AES_256_GCM    1u

/* Chunk descriptor flags. */
#define HIBER_CHUNK_FLAG_STORED     (1u << 0)   /* verbatim, not LZ4 */
#define HIBER_CHUNK_FLAGS_KNOWN     (HIBER_CHUNK_FLAG_STORED)

/* Header flags. None defined yet; the mask exists so the reject-unknown rule
 * has something to test against from the first version. */
#define HIBER_HEADER_FLAGS_KNOWN    0u

/* Return codes. HIBER_OK is 0; every failure is a distinct negative value so
 * a caller can report WHY without a second query. */
#define HIBER_OK                 0
#define HIBER_ERR_ARG           (-1)   /* NULL buffer, impossible length */
#define HIBER_ERR_MAGIC         (-2)   /* not a hibernation image */
#define HIBER_ERR_FORMAT        (-3)   /* format_version / header_bytes wrong */
#define HIBER_ERR_HEADER_CRC    (-4)   /* header self-check failed */
#define HIBER_ERR_BOUNDS        (-5)   /* a length runs past the buffer */
#define HIBER_ERR_ORDER         (-6)   /* descriptors not ascending / overlap */
#define HIBER_ERR_FLAGS         (-7)   /* unknown flag bit set */
#define HIBER_ERR_LZ4           (-8)   /* compressor/decompressor refused */
#define HIBER_ERR_PAYLOAD_CRC   (-9)   /* payload corruption detected */
#define HIBER_ERR_TRUNCATED    (-10)   /* image ends mid-record */
#define HIBER_ERR_TRAILING     (-11)   /* bytes after the last chunk */
#define HIBER_ERR_COUNT        (-12)   /* chunk/page totals disagree */
#define HIBER_ERR_NOSPACE      (-13)   /* output buffer too small */
#define HIBER_ERR_STATE        (-14)   /* API used out of order */
#define HIBER_ERR_RESERVED     (-15)   /* reserved bytes not zero */

/* Identity mismatch reasons. A kernel-artifact mismatch and a boot_info ABI
 * mismatch are different failures with different operator meaning, so they
 * never collapse into one code. */
typedef enum hiber_ident_result {
    HIBER_IDENT_OK = 0,
    HIBER_IDENT_KERNEL_ARTIFACT_MISMATCH = 1,
    HIBER_IDENT_BOOT_INFO_ABI_MISMATCH   = 2,
    HIBER_IDENT_FORMAT_VERSION_MISMATCH  = 3,
    HIBER_IDENT_RESUME_TYPE_MISMATCH     = 4,
} hiber_ident_result_t;

/* ---------------------------------------------------------------------------
 * On-disk header. Exactly HIBER_HEADER_BYTES; every offset pinned below.
 * ------------------------------------------------------------------------- */
typedef struct hiber_header {
    uint64_t magic;                              /* 0   HIBER_MAGIC */
    uint32_t format_version;                     /* 8   HIBER_FORMAT_VERSION */
    uint32_t header_bytes;                       /* 12  HIBER_HEADER_BYTES */
    uint64_t image_bytes;                        /* 16  header + payload */
    uint64_t page_count;                         /* 24  pages restored */
    uint32_t chunk_count;                        /* 32  payload records */
    uint32_t resume_type;                        /* 36  HIBER_RESUME_* */
    uint32_t boot_info_version;                  /* 40  BOOT_INFO_VERSION */
    uint32_t flags;                              /* 44  HIBER_HEADER_FLAGS_* */
    uint64_t root_volume_id;                     /* 48  volume holding it */
    uint64_t resume_generation;                  /* 56  anti-replay counter */
    uint8_t  kernel_id[HIBER_KERNEL_ID_BYTES];   /* 64  exact artifact id */
    uint32_t aead_cipher_id;                     /* 96  HIBER_CIPHER_* */
    uint32_t aead_key_id;                        /* 100 sealed-key selector */
    uint8_t  aead_nonce[HIBER_AEAD_NONCE_BYTES]; /* 104 */
    uint8_t  aead_tag[HIBER_AEAD_TAG_BYTES];     /* 116 */
    uint32_t payload_crc32c;                     /* 132 over the payload */
    uint32_t header_crc32c;                      /* 136 over this header */
    uint8_t  reserved[3956];                     /* 140 MUST be zero */
} hiber_header_t;

_Static_assert(sizeof(hiber_header_t) == HIBER_HEADER_BYTES,
               "hibernation header is a 4 KiB on-disk ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, magic) == 0, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, format_version) == 8, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, header_bytes) == 12, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, image_bytes) == 16, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, page_count) == 24, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, chunk_count) == 32, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, resume_type) == 36, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, boot_info_version) == 40, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, flags) == 44, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, root_volume_id) == 48, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, resume_generation) == 56, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, kernel_id) == 64, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, aead_cipher_id) == 96, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, aead_key_id) == 100, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, aead_nonce) == 104, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, aead_tag) == 116, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, payload_crc32c) == 132, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, header_crc32c) == 136, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_header_t, reserved) == 140, "hiber ABI");

/* ---------------------------------------------------------------------------
 * On-disk chunk descriptor. Immediately followed by `stored_len` bytes.
 * ------------------------------------------------------------------------- */
typedef struct hiber_chunk_desc {
    uint64_t start_pfn;         /* 0  first physical frame restored */
    uint32_t page_count;        /* 8  1 .. HIBER_CHUNK_PAGES */
    uint32_t uncompressed_len;  /* 12 == page_count * HIBER_PAGE_SIZE */
    uint32_t stored_len;        /* 16 bytes that follow this descriptor */
    uint32_t flags;             /* 20 HIBER_CHUNK_FLAG_* */
} hiber_chunk_desc_t;

_Static_assert(sizeof(hiber_chunk_desc_t) == 24, "hiber chunk descriptor ABI");
_Static_assert(__builtin_offsetof(hiber_chunk_desc_t, start_pfn) == 0, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_chunk_desc_t, page_count) == 8, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_chunk_desc_t, uncompressed_len) == 12, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_chunk_desc_t, stored_len) == 16, "hiber ABI");
_Static_assert(__builtin_offsetof(hiber_chunk_desc_t, flags) == 20, "hiber ABI");

/* ---------------------------------------------------------------------------
 * Image identity, supplied by the caller at finalize and compared at resume.
 * ------------------------------------------------------------------------- */
typedef struct hiber_ident {
    uint8_t  kernel_id[HIBER_KERNEL_ID_BYTES];
    uint32_t boot_info_version;
    uint32_t resume_type;
    uint64_t root_volume_id;
    uint64_t resume_generation;
} hiber_ident_t;

/* ---------------------------------------------------------------------------
 * Encoder. Caller owns the output buffer; nothing is allocated.
 * ------------------------------------------------------------------------- */
typedef struct hiber_encoder {
    uint8_t *out;
    size_t   out_cap;
    size_t   cursor;        /* next free byte; starts at HIBER_HEADER_BYTES */
    uint64_t page_count;
    uint64_t next_pfn;      /* lowest PFN a further chunk may start at */
    uint32_t chunk_count;
    uint32_t payload_crc;
    uint8_t  started;
    uint8_t  finalized;
} hiber_encoder_t;

/* Begin an image in `out` (capacity `out_cap`, at least HIBER_HEADER_BYTES).
 * Zeroes the header area, so reserved bytes are zero by construction. */
int hibernate_image_begin(hiber_encoder_t *enc, void *out, size_t out_cap);

/* Append `page_count` physically contiguous pages starting at `start_pfn`,
 * read from `pages`. Compresses when that shrinks the chunk and stores the
 * pages verbatim when it does not, so an incompressible image never grows
 * beyond its uncompressed size plus descriptors. Chunks MUST be appended in
 * strictly ascending, non-overlapping PFN order. */
int hibernate_image_append_chunk(hiber_encoder_t *enc, uint64_t start_pfn,
                                 const void *pages, uint32_t page_count);

/* Write the header and finish the image. On success `*image_bytes_out`
 * receives the total encoded size. */
int hibernate_image_finalize(hiber_encoder_t *enc, const hiber_ident_t *ident,
                             uint64_t *image_bytes_out);

/* ---------------------------------------------------------------------------
 * Decoder. Every output byte is PROVISIONAL until decode_finish() succeeds.
 * ------------------------------------------------------------------------- */
typedef struct hiber_decoder {
    const uint8_t *img;
    size_t         img_len;     /* header->image_bytes, re-checked */
    size_t         cursor;
    uint64_t       pages_read;
    uint64_t       next_pfn;
    uint64_t       declared_pages;
    uint32_t       declared_chunks;
    uint32_t       declared_crc;
    uint32_t       chunks_read;
    uint32_t       payload_crc;
    uint8_t        started;
    uint8_t        finished;
} hiber_decoder_t;

/* Validate the header in `img` without copying it (a 4 KiB by-value copy has
 * no business on an 8 KiB kernel stack). `img_len` is the CAPACITY of the
 * caller's buffer; the image itself is the first `image_bytes` of it. On
 * success `*out` points into `img`. */
int hibernate_image_header_validate(const void *img, size_t img_len,
                                    const hiber_header_t **out);

int hibernate_image_decode_begin(hiber_decoder_t *dec, const void *img,
                                 size_t img_len);

/* Decode the next chunk into `dst`. On success `*out_start_pfn` and
 * `*out_page_count` say where those pages belong. */
int hibernate_image_read_chunk(hiber_decoder_t *dec, void *dst, size_t dst_cap,
                               uint64_t *out_start_pfn,
                               uint32_t *out_page_count);

/* The mandatory completion gate: proves the declared chunk count, page count
 * and encoded length were each exhausted exactly and that the payload CRC-32C
 * matches. Until this returns HIBER_OK nothing decoded may be trusted. */
int hibernate_image_decode_finish(hiber_decoder_t *dec);

/* ---------------------------------------------------------------------------
 * Identity guard.
 * ------------------------------------------------------------------------- */
hiber_ident_result_t hibernate_image_kernel_matches(const hiber_header_t *hdr,
                                                    const hiber_ident_t *now);

/* Worst-case encoded size for an image of `page_count` pages, so a caller can
 * size its output buffer before encoding. Returns 0 when the request cannot be
 * represented (overflow, or more pages than the format can describe). */
uint64_t hibernate_image_encoded_bound(uint64_t page_count);

#endif /* KERNEL_PM_HIBERNATE_H */
