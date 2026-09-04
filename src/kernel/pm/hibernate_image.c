/* ============================================================================
 * hibernate_image.c -- S4 hibernation image codec (format in pm/hibernate.h)
 *
 * Produces and verifies the authoritative on-disk hibernation image. Pure by
 * construction: no globals, no allocation, no locking, no disk or ACPI
 * contact, so it is SMP-safe by having nothing to share and is exercisable
 * end to end from unit tests. The write path that feeds it real pages, the
 * AEAD layer that makes an on-disk image confidential, and the resume
 * consumer all live in section 28 and are blocked on prerequisites owned
 * elsewhere.
 *
 * The decoder treats its input as hostile: every descriptor is bounds-checked
 * against the remaining image before it is used, LZ4 runs through the
 * bounds-checked safe decompressor, the produced length must match the
 * declared length exactly, and nothing is trustworthy until
 * hibernate_image_decode_finish() has proved the whole payload.
 *
 * XREF: 02-kernel-core/TODO-26-power-management.md section 4
 * ============================================================================ */

#include "kernel/pm/hibernate.h"
#include "kernel/kchecksum.h"
#include "libs/lz4.h"
#include "libc/string.h"

/* The header CRC is computed with its own 4 bytes taken as zero, so the two
 * spans either side of it are hashed with these zeros between them. */
#define HIBER_HDR_CRC_OFF   136u
#define HIBER_HDR_CRC_LEN   4u

/* The CRC span is written as literals so the two kcrc32c_cont calls read as
 * the format's own rule, but a literal that silently stops matching the struct
 * would checksum the wrong bytes on both sides of a mirror. Bind them. */
_Static_assert(HIBER_HDR_CRC_OFF
               == __builtin_offsetof(hiber_header_t, header_crc32c),
               "header CRC span must start at the header_crc32c field");
_Static_assert(HIBER_HDR_CRC_LEN
               == sizeof(((hiber_header_t *)0)->header_crc32c),
               "header CRC span must skip exactly the CRC field");

/* An 8-byte-aligned image buffer lets the header be read in place instead of
 * copied onto a kernel stack that only has 8 KiB to spare. */
#define HIBER_IMAGE_ALIGN   8u

static int hiber_ptr_aligned(const void *p)
{
    return ((uintptr_t)p % HIBER_IMAGE_ALIGN) == 0;
}

/* CRC-32C over the header exactly as the format defines it: all
 * HIBER_HEADER_BYTES, with header_crc32c itself read as zero. */
static uint32_t hiber_header_crc(const uint8_t *hdr)
{
    static const uint8_t zeros[HIBER_HDR_CRC_LEN] = { 0, 0, 0, 0 };
    uint32_t crc;

    crc = kcrc32c(hdr, HIBER_HDR_CRC_OFF);
    crc = kcrc32c_cont(crc, zeros, HIBER_HDR_CRC_LEN);
    crc = kcrc32c_cont(crc, hdr + HIBER_HDR_CRC_OFF + HIBER_HDR_CRC_LEN,
                       HIBER_HEADER_BYTES - HIBER_HDR_CRC_OFF - HIBER_HDR_CRC_LEN);
    return crc;
}

static int hiber_resume_type_known(uint32_t t)
{
    return t == HIBER_RESUME_FULL
        || t == HIBER_RESUME_FAST_STARTUP
        || t == HIBER_RESUME_CRASH_TEST;
}

static int hiber_cipher_known(uint32_t c)
{
    return c == HIBER_CIPHER_NONE || c == HIBER_CIPHER_AES_256_GCM;
}

uint64_t hibernate_image_encoded_bound(uint64_t page_count)
{
    /* chunk_count is a u32 and the worst case is one chunk per page, which is
     * what a fully sparse used-page set produces. Bounding on
     * ceil(pages / HIBER_CHUNK_PAGES) would under-size that image. */
    if (page_count > 0xFFFFFFFFull) {
        return 0;
    }
    return (uint64_t)HIBER_HEADER_BYTES
         + page_count * ((uint64_t)sizeof(hiber_chunk_desc_t)
                         + (uint64_t)HIBER_PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * Encoder
 * ------------------------------------------------------------------------- */

int hibernate_image_begin(hiber_encoder_t *enc, void *out, size_t out_cap)
{
    if (enc == NULL) {
        return HIBER_ERR_ARG;
    }
    /* Zero the state BEFORE any other check can return. A caller that ignores
     * a failed begin would otherwise hand the next call a struct full of stack
     * garbage, and append_chunk's first write is through enc->out. Zeroed,
     * `started` is 0 and every later call refuses with HIBER_ERR_STATE. */
    memset(enc, 0, sizeof(*enc));

    if (out == NULL) {
        return HIBER_ERR_ARG;
    }
    if (!hiber_ptr_aligned(out)) {
        return HIBER_ERR_ARG;
    }
    if (out_cap < HIBER_HEADER_BYTES) {
        return HIBER_ERR_NOSPACE;
    }

    /* Zeroing the header area is what makes the reserved bytes zero and the
     * AEAD block an explicit "no cipher" rather than uninitialised bytes. */
    memset(out, 0, HIBER_HEADER_BYTES);

    enc->out     = (uint8_t *)out;
    enc->out_cap = out_cap;
    enc->cursor  = HIBER_HEADER_BYTES;
    enc->started = 1;
    return HIBER_OK;
}

int hibernate_image_append_chunk(hiber_encoder_t *enc, uint64_t start_pfn,
                                 const void *pages, uint32_t page_count)
{
    hiber_chunk_desc_t desc;
    uint8_t *payload;
    size_t   avail;
    uint32_t uncompressed_len;
    uint32_t stored_len;
    uint32_t flags;
    int      rc;

    if (enc == NULL || pages == NULL) {
        return HIBER_ERR_ARG;
    }
    if (!enc->started || enc->finalized) {
        return HIBER_ERR_STATE;
    }
    if (page_count == 0 || page_count > HIBER_CHUNK_PAGES) {
        return HIBER_ERR_ARG;
    }
    /* Strictly ascending and non-overlapping: the chunk sequence IS the
     * destination map, so an out-of-order append would let one chunk's pages
     * silently land on top of another's at restore time. */
    if (start_pfn < enc->next_pfn) {
        return HIBER_ERR_ORDER;
    }
    /* Bounding the extent by HIBER_MAX_PFN rather than by 64-bit wrap is what
     * keeps every PFN convertible to a byte address at restore time. The
     * comparison is on the LAST INCLUDED frame, so an extent ending exactly on
     * HIBER_MAX_PFN is legal -- that frame's page is fully addressable, and
     * excluding it would make this side disagree with the documented rule a
     * separately compiled mirror implements. page_count is already known
     * non-zero, so page_count - 1 cannot underflow. */
    if (start_pfn > HIBER_MAX_PFN
        || (uint64_t)(page_count - 1u) > HIBER_MAX_PFN - start_pfn) {
        return HIBER_ERR_BOUNDS;
    }
    if (enc->chunk_count == 0xFFFFFFFFu) {
        return HIBER_ERR_COUNT;
    }

    uncompressed_len = page_count * HIBER_PAGE_SIZE;

    if (enc->cursor > enc->out_cap
        || (enc->out_cap - enc->cursor) < sizeof(hiber_chunk_desc_t)) {
        return HIBER_ERR_NOSPACE;
    }
    payload = enc->out + enc->cursor + sizeof(hiber_chunk_desc_t);
    avail   = enc->out_cap - enc->cursor - sizeof(hiber_chunk_desc_t);
    /* Room for the verbatim fallback is what guarantees progress: compression
     * may decline, storing never can. */
    if (avail < (size_t)uncompressed_len) {
        return HIBER_ERR_NOSPACE;
    }

    /* Capping the compressor at one byte below the input makes "did not
     * shrink" a refusal from LZ4 itself rather than a second size test here,
     * and it is what keeps an incompressible image from growing.
     *
     * KNOWN COST, deliberately taken: LZ4 emits sequences as it goes and only
     * discovers the budget is exhausted partway through (src/libs/lz4/lz4.c
     * returns 0 at :1210 and :1314 with its output pointer already advanced),
     * so a near-incompressible chunk can be written here and then written
     * again by the verbatim fallback below. The alternative -- compressing
     * into a caller-owned workspace and copying the winner in once -- removes
     * that but adds a copy to EVERY successful chunk, which is the common
     * case. Which is cheaper depends on the page mix of a real image, and the
     * write path that would produce one is section 28. Measured there, not
     * guessed at here. -> XREF: 02-kernel-core/TODO-26-power-management.md, the hibernation
     * write-path section. */
    rc = lz4_compress(pages, (size_t)uncompressed_len, payload,
                      (size_t)uncompressed_len - 1u);
    if (rc > 0) {
        stored_len = (uint32_t)rc;
        flags      = 0u;
    } else {
        memcpy(payload, pages, (size_t)uncompressed_len);
        stored_len = uncompressed_len;
        flags      = HIBER_CHUNK_FLAG_STORED;
    }

    desc.start_pfn        = start_pfn;
    desc.page_count       = page_count;
    desc.uncompressed_len = uncompressed_len;
    desc.stored_len       = stored_len;
    desc.flags            = flags;
    memcpy(enc->out + enc->cursor, &desc, sizeof(desc));

    /* The payload CRC covers descriptors as well as data, so a tampered
     * destination PFN is a CRC failure and not just an ordering failure. */
    enc->payload_crc = kcrc32c_cont(enc->payload_crc, enc->out + enc->cursor,
                                    sizeof(desc) + (size_t)stored_len);

    enc->cursor     += sizeof(desc) + (size_t)stored_len;
    enc->chunk_count += 1u;
    enc->page_count  += (uint64_t)page_count;
    enc->next_pfn     = start_pfn + (uint64_t)page_count;
    return HIBER_OK;
}

int hibernate_image_finalize(hiber_encoder_t *enc, const hiber_ident_t *ident,
                             uint64_t *image_bytes_out)
{
    hiber_header_t *h;

    if (enc == NULL || ident == NULL || image_bytes_out == NULL) {
        return HIBER_ERR_ARG;
    }
    if (!enc->started || enc->finalized) {
        return HIBER_ERR_STATE;
    }
    if (!hiber_resume_type_known(ident->resume_type)) {
        return HIBER_ERR_ARG;
    }
    /* An image that claims zero CPUs is one no resume could ever validate
     * against, so it is refused where it is produced rather than where it is
     * read. */
    if (ident->cpu_count_present == 0u) {
        return HIBER_ERR_ARG;
    }

    h = (hiber_header_t *)enc->out;
    h->magic             = HIBER_MAGIC;
    h->format_version    = HIBER_FORMAT_VERSION;
    h->header_bytes      = HIBER_HEADER_BYTES;
    h->image_bytes       = (uint64_t)enc->cursor;
    h->page_count        = enc->page_count;
    h->chunk_count       = enc->chunk_count;
    h->resume_type       = ident->resume_type;
    h->boot_info_version = ident->boot_info_version;
    h->flags             = 0u;
    h->root_volume_id    = ident->root_volume_id;
    h->resume_generation = ident->resume_generation;
    h->cpu_count_present = ident->cpu_count_present;
    h->total_ram_pages   = ident->total_ram_pages;
    memcpy(h->kernel_id, ident->kernel_id, HIBER_KERNEL_ID_BYTES);
    /* Confidentiality is section 28's: the codec states plaintext rather than
     * leaving a reader to infer it from a zero field. */
    h->aead_cipher_id    = HIBER_CIPHER_NONE;
    h->aead_key_id       = 0u;
    h->payload_crc32c    = enc->payload_crc;

    /* hiber_header_crc substitutes zeros for this field's own span, so the
     * stored value never feeds its own checksum and needs no pre-zeroing. */
    h->header_crc32c     = hiber_header_crc(enc->out);

    *image_bytes_out = (uint64_t)enc->cursor;
    enc->finalized   = 1;
    return HIBER_OK;
}

/* ---------------------------------------------------------------------------
 * Decoder
 * ------------------------------------------------------------------------- */

int hibernate_image_header_validate(const void *img, size_t img_len,
                                    const hiber_header_t **out)
{
    const hiber_header_t *h;
    const uint8_t *bytes = (const uint8_t *)img;
    uint32_t i;

    if (img == NULL || out == NULL) {
        return HIBER_ERR_ARG;
    }
    if (!hiber_ptr_aligned(img)) {
        return HIBER_ERR_ARG;
    }
    if (img_len < HIBER_HEADER_BYTES) {
        return HIBER_ERR_TRUNCATED;
    }

    h = (const hiber_header_t *)img;
    if (h->magic != HIBER_MAGIC) {
        return HIBER_ERR_MAGIC;
    }
    if (h->format_version != HIBER_FORMAT_VERSION
        || h->header_bytes != HIBER_HEADER_BYTES) {
        return HIBER_ERR_FORMAT;
    }
    /* Nothing below this line may be trusted before the header proves itself,
     * so the CRC comes before every field-level check. */
    if (hiber_header_crc(bytes) != h->header_crc32c) {
        return HIBER_ERR_HEADER_CRC;
    }
    for (i = 0; i < sizeof(h->reserved); i++) {
        if (h->reserved[i] != 0) {
            return HIBER_ERR_RESERVED;
        }
    }
    if ((h->flags & ~(uint32_t)HIBER_HEADER_FLAGS_KNOWN) != 0u) {
        return HIBER_ERR_FLAGS;
    }
    if (!hiber_resume_type_known(h->resume_type)
        || !hiber_cipher_known(h->aead_cipher_id)) {
        return HIBER_ERR_FORMAT;
    }
    /* Under NONE the AEAD block is reserved space, and the reserved rule above
     * would otherwise leave these 32 CRC-covered bytes free to carry anything.
     * It also removes a downgrade ambiguity before section 28 lands the cipher:
     * a plaintext image can never arrive already carrying a key id, nonce and
     * tag that a later reader might be tempted to act on. */
    if (h->aead_cipher_id == HIBER_CIPHER_NONE) {
        if (h->aead_key_id != 0u) {
            return HIBER_ERR_RESERVED;
        }
        for (i = 0; i < sizeof(h->aead_nonce); i++) {
            if (h->aead_nonce[i] != 0) {
                return HIBER_ERR_RESERVED;
            }
        }
        for (i = 0; i < sizeof(h->aead_tag); i++) {
            if (h->aead_tag[i] != 0) {
                return HIBER_ERR_RESERVED;
            }
        }
    }
    if (h->cpu_count_present == 0u) {
        return HIBER_ERR_FORMAT;
    }
    if (h->image_bytes < (uint64_t)HIBER_HEADER_BYTES) {
        return HIBER_ERR_BOUNDS;
    }
    if (h->image_bytes > (uint64_t)img_len) {
        return HIBER_ERR_TRUNCATED;
    }

    if (h->chunk_count == 0u) {
        if (h->page_count != 0u
            || h->image_bytes != (uint64_t)HIBER_HEADER_BYTES) {
            return HIBER_ERR_COUNT;
        }
    } else {
        uint64_t payload = h->image_bytes - (uint64_t)HIBER_HEADER_BYTES;
        /* Each chunk carries at least a descriptor, and covers between one
         * page and a full chunk of them. */
        if (h->page_count < (uint64_t)h->chunk_count
            || h->page_count > (uint64_t)h->chunk_count
                               * (uint64_t)HIBER_CHUNK_PAGES) {
            return HIBER_ERR_COUNT;
        }
        if (payload < (uint64_t)h->chunk_count
                      * (uint64_t)sizeof(hiber_chunk_desc_t)) {
            return HIBER_ERR_TRUNCATED;
        }
    }

    *out = h;
    return HIBER_OK;
}

int hibernate_image_decode_begin(hiber_decoder_t *dec, const void *img,
                                 size_t img_len)
{
    const hiber_header_t *h;
    int rc;

    if (dec == NULL) {
        return HIBER_ERR_ARG;
    }
    /* Same rule as the encoder: a failed begin leaves a state that refuses
     * every later call rather than one full of stack garbage. */
    memset(dec, 0, sizeof(*dec));

    rc = hibernate_image_header_validate(img, img_len, &h);
    if (rc != HIBER_OK) {
        return rc;
    }

    dec->img             = (const uint8_t *)img;
    /* The header's own length wins over the caller's capacity: a reader
     * working in sectors holds more bytes than the image occupies, and those
     * extra bytes are outside the image by definition. A suffix the header
     * DECLARES is the dangerous case, and decode_finish refuses it. */
    dec->img_len         = (size_t)h->image_bytes;
    dec->cursor          = HIBER_HEADER_BYTES;
    dec->declared_pages  = h->page_count;
    dec->declared_chunks = h->chunk_count;
    dec->declared_crc    = h->payload_crc32c;
    dec->started         = 1;
    return HIBER_OK;
}

int hibernate_image_read_chunk(hiber_decoder_t *dec, void *dst, size_t dst_cap,
                               uint64_t *out_start_pfn,
                               uint32_t *out_page_count)
{
    hiber_chunk_desc_t desc;
    const uint8_t *rec;
    size_t remaining;
    int rc;

    if (dec == NULL || dst == NULL || out_start_pfn == NULL
        || out_page_count == NULL) {
        return HIBER_ERR_ARG;
    }
    if (!dec->started || dec->finished) {
        return HIBER_ERR_STATE;
    }
    if (dec->chunks_read >= dec->declared_chunks) {
        return HIBER_ERR_STATE;
    }

    remaining = dec->img_len - dec->cursor;
    if (remaining < sizeof(desc)) {
        return HIBER_ERR_TRUNCATED;
    }
    rec = dec->img + dec->cursor;
    memcpy(&desc, rec, sizeof(desc));

    if (desc.page_count == 0u || desc.page_count > HIBER_CHUNK_PAGES) {
        return HIBER_ERR_BOUNDS;
    }
    if (desc.uncompressed_len != desc.page_count * HIBER_PAGE_SIZE) {
        return HIBER_ERR_BOUNDS;
    }
    if ((desc.flags & ~(uint32_t)HIBER_CHUNK_FLAGS_KNOWN) != 0u) {
        return HIBER_ERR_FLAGS;
    }
    if (desc.start_pfn < dec->next_pfn) {
        return HIBER_ERR_ORDER;
    }
    /* Same last-included-frame comparison as the encoder; page_count was
     * already rejected at zero above, so the subtraction is safe. */
    if (desc.start_pfn > HIBER_MAX_PFN
        || (uint64_t)(desc.page_count - 1u) > HIBER_MAX_PFN - desc.start_pfn) {
        return HIBER_ERR_BOUNDS;
    }
    if ((desc.flags & HIBER_CHUNK_FLAG_STORED) != 0u) {
        if (desc.stored_len != desc.uncompressed_len) {
            return HIBER_ERR_BOUNDS;
        }
    } else {
        /* A compressed record that did not shrink is not a record this
         * encoder can produce, so accepting one would only widen what a
         * hostile image may claim. */
        if (desc.stored_len == 0u || desc.stored_len >= desc.uncompressed_len) {
            return HIBER_ERR_BOUNDS;
        }
    }
    if ((remaining - sizeof(desc)) < (size_t)desc.stored_len) {
        return HIBER_ERR_TRUNCATED;
    }
    if (dst_cap < (size_t)desc.uncompressed_len) {
        return HIBER_ERR_NOSPACE;
    }

    if ((desc.flags & HIBER_CHUNK_FLAG_STORED) != 0u) {
        memcpy(dst, rec + sizeof(desc), (size_t)desc.stored_len);
    } else {
        /* The capacity is the DECLARED output, not the caller's buffer: a
         * forged descriptor claiming one page while carrying a block that
         * expands to several must be refused before those extra bytes are
         * written, not after. */
        rc = lz4_decompress(rec + sizeof(desc), (size_t)desc.stored_len,
                            dst, (size_t)desc.uncompressed_len);
        /* Short output is a failure, not a partial success: the remaining
         * bytes of dst would otherwise carry whatever was there before. */
        if (rc < 0 || (uint32_t)rc != desc.uncompressed_len) {
            return HIBER_ERR_LZ4;
        }
    }

    dec->payload_crc = kcrc32c_cont(dec->payload_crc, rec,
                                    sizeof(desc) + (size_t)desc.stored_len);
    dec->cursor     += sizeof(desc) + (size_t)desc.stored_len;
    dec->chunks_read += 1u;
    dec->pages_read  += (uint64_t)desc.page_count;
    dec->next_pfn     = desc.start_pfn + (uint64_t)desc.page_count;

    *out_start_pfn  = desc.start_pfn;
    *out_page_count = desc.page_count;
    return HIBER_OK;
}

int hibernate_image_decode_finish(hiber_decoder_t *dec)
{
    if (dec == NULL) {
        return HIBER_ERR_ARG;
    }
    if (!dec->started || dec->finished) {
        return HIBER_ERR_STATE;
    }
    if (dec->chunks_read != dec->declared_chunks
        || dec->pages_read != dec->declared_pages) {
        return HIBER_ERR_COUNT;
    }
    if (dec->cursor != dec->img_len) {
        return HIBER_ERR_TRAILING;
    }
    if (dec->payload_crc != dec->declared_crc) {
        return HIBER_ERR_PAYLOAD_CRC;
    }
    dec->finished = 1;
    return HIBER_OK;
}

/* ---------------------------------------------------------------------------
 * Identity guard
 * ------------------------------------------------------------------------- */

hiber_ident_result_t hibernate_image_kernel_matches(const hiber_header_t *hdr,
                                                    const hiber_ident_t *now)
{
    if (hdr == NULL || now == NULL) {
        /* Refusing is the only safe answer: resuming into the wrong kernel
         * restores an address space and a call stack that its code no longer
         * matches. */
        return HIBER_IDENT_KERNEL_ARTIFACT_MISMATCH;
    }
    if (hdr->format_version != HIBER_FORMAT_VERSION) {
        return HIBER_IDENT_FORMAT_VERSION_MISMATCH;
    }
    if (memcmp(hdr->kernel_id, now->kernel_id, HIBER_KERNEL_ID_BYTES) != 0) {
        return HIBER_IDENT_KERNEL_ARTIFACT_MISMATCH;
    }
    /* A handoff-ABI change is a different failure from a different kernel
     * binary, and an operator reading the log needs to be able to tell them
     * apart. */
    if (hdr->boot_info_version != now->boot_info_version) {
        return HIBER_IDENT_BOOT_INFO_ABI_MISMATCH;
    }
    if (hdr->resume_type != now->resume_type) {
        return HIBER_IDENT_RESUME_TYPE_MISMATCH;
    }
    /* FEWER CPUs than the image was captured on is fatal: the image carries
     * per-CPU state for processors that no longer exist. MORE is fine -- the
     * extra ones simply were not running when it was captured. That asymmetry
     * is why this is an inequality and not an equality. */
    if (now->cpu_count_present < hdr->cpu_count_present) {
        return HIBER_IDENT_CPU_TOPOLOGY_MISMATCH;
    }
    /* root_volume_id, resume_generation and total_ram_pages are deliberately
     * NOT compared here; the header documents which component owns each. */
    return HIBER_IDENT_OK;
}
