/* ============================================================================
 * test_hibernate_image.c -- S4 hibernation image codec tests (TODO-26 s4)
 *
 * Covers src/kernel/pm/hibernate_image.c: the S4 hibernation image format and
 * its LZ4 chunk codec. The codec is pure -- no disk, no ACPI, no scheduler --
 * so every path below runs against in-memory images with no live boot
 * infrastructure touched.
 *
 * The assertions are deliberately behavioural rather than constant echoes:
 * the layout tests read the ENCODED BYTES at fixed offsets (proving the
 * serializer agrees with the pinned ABI, which a _Static_assert cannot show),
 * and the rejection tests corrupt a real image and require the specific
 * refusal code the format promises.
 *
 * XREF: 02-kernel-core/TODO-26-power-management.md section 4
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/pm/hibernate.h"
#include "kernel/kchecksum.h"
#include "kernel/mm/pmm.h"
#include "libs/lz4.h"
#include "libc/string.h"

/* Two-page chunks keep every scratch buffer inside a handful of frames while
 * still exercising the multi-page descriptor arithmetic. */
#define TP_CHUNK_PAGES   2u
#define TP_CHUNK_BYTES   (TP_CHUNK_PAGES * HIBER_PAGE_SIZE)

/* Frames for the encoded image: header plus a worst-case (all stored) three
 * chunks, rounded up to whole pages. 4096 + 3 * (24 + 8192) = 28744, so five
 * frames was 8 KiB short of the case this comment describes and only the
 * compressible fixtures kept the multi-chunk tests inside it. */
#define TP_IMG_FRAMES    8u
#define TP_IMG_BYTES     (TP_IMG_FRAMES * HIBER_PAGE_SIZE)

/* A test-only kernel identity. The codec never derives one; it compares what
 * the caller supplies, so a fixed pattern is exactly what is under test. */
static const uint8_t tp_kernel_id_a[HIBER_KERNEL_ID_BYTES] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0, 0x0F,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
};

/* The arenas below are sized in FORMAT pages and allocated in PMM FRAMES.
 * Those are different constants -- one is fixed by the on-disk layout, the
 * other is architecture-dependent -- and every buffer bound here assumes they
 * agree. On a 16K- or 64K-page port they would not. */
_Static_assert(HIBER_PAGE_SIZE == PMM_FRAME_SIZE,
               "test arenas size format pages with PMM frames");

#define TP_BOOT_INFO_VERSION  0x1234u
#define TP_CPU_COUNT          4u

static void tp_ident_init(hiber_ident_t *id)
{
    memset(id, 0, sizeof(*id));
    memcpy(id->kernel_id, tp_kernel_id_a, HIBER_KERNEL_ID_BYTES);
    id->boot_info_version = TP_BOOT_INFO_VERSION;
    id->resume_type       = HIBER_RESUME_FULL;
    id->cpu_count_present = TP_CPU_COUNT;
    id->root_volume_id    = 0xFEEDFACEull;
    id->resume_generation = 7u;
    id->total_ram_pages   = 0x40000ull;
}

/* Highly redundant: LZ4 must shrink this well below its input. */
static void tp_fill_compressible(uint8_t *p, size_t n)
{
    memset(p, 0xA5, n);
}

/* An LCG byte stream. LZ4 cannot shrink it, so the encoder must take the
 * stored-verbatim path rather than emitting something larger than the input. */
static void tp_fill_incompressible(uint8_t *p, size_t n)
{
    uint32_t x = 0x1234567Fu;
    size_t i;

    for (i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)(x >> 24);
    }
}

/* Read a little-endian field straight out of the encoded bytes. */
static uint32_t tp_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t tp_le64(const uint8_t *p)
{
    return (uint64_t)tp_le32(p) | ((uint64_t)tp_le32(p + 4) << 32);
}

/* Scratch arena: one image buffer, one source buffer, one decode buffer. */
typedef struct tp_arena {
    uint8_t *img;
    uint8_t *src;
    uint8_t *dst;
    uintptr_t img_phys;
    uintptr_t src_phys;
    uintptr_t dst_phys;
} tp_arena_t;

#define TP_SRC_FRAMES  (TP_CHUNK_PAGES * 3u)
#define TP_DST_FRAMES  TP_CHUNK_PAGES

static int tp_arena_init(tp_arena_t *a)
{
    memset(a, 0, sizeof(*a));
    a->img_phys = pmm_alloc_contiguous(TP_IMG_FRAMES);
    a->src_phys = pmm_alloc_contiguous(TP_SRC_FRAMES);
    a->dst_phys = pmm_alloc_contiguous(TP_DST_FRAMES);
    if (a->img_phys == 0 || a->src_phys == 0 || a->dst_phys == 0) {
        if (a->img_phys != 0) pmm_free_contiguous(a->img_phys, TP_IMG_FRAMES);
        if (a->src_phys != 0) pmm_free_contiguous(a->src_phys, TP_SRC_FRAMES);
        if (a->dst_phys != 0) pmm_free_contiguous(a->dst_phys, TP_DST_FRAMES);
        memset(a, 0, sizeof(*a));
        return 0;
    }
    a->img = (uint8_t *)a->img_phys;
    a->src = (uint8_t *)a->src_phys;
    a->dst = (uint8_t *)a->dst_phys;
    return 1;
}

static void tp_arena_free(tp_arena_t *a)
{
    if (a->img_phys != 0) pmm_free_contiguous(a->img_phys, TP_IMG_FRAMES);
    if (a->src_phys != 0) pmm_free_contiguous(a->src_phys, TP_SRC_FRAMES);
    if (a->dst_phys != 0) pmm_free_contiguous(a->dst_phys, TP_DST_FRAMES);
    memset(a, 0, sizeof(*a));
}

/* Encode one chunk at `start_pfn` from `a->src`, returning the image size (or
 * 0 on any failure, which the caller asserts on). */
static uint64_t tp_encode_one(tp_arena_t *a, uint64_t start_pfn)
{
    hiber_encoder_t enc;
    hiber_ident_t   id;
    uint64_t        bytes = 0;

    tp_ident_init(&id);
    if (hibernate_image_begin(&enc, a->img, TP_IMG_BYTES) != HIBER_OK) {
        return 0;
    }
    if (hibernate_image_append_chunk(&enc, start_pfn, a->src,
                                     TP_CHUNK_PAGES) != HIBER_OK) {
        return 0;
    }
    if (hibernate_image_finalize(&enc, &id, &bytes) != HIBER_OK) {
        return 0;
    }
    return bytes;
}

/* ------------------------------------------------------------------------- */

static void test_hiber_encoded_bound(void)
{
    uint64_t one;

    TEST_ASSERT_EQ((int)hibernate_image_encoded_bound(0),
                   (int)HIBER_HEADER_BYTES,
                   "a zero-page image is exactly one header");

    one = hibernate_image_encoded_bound(1);
    /* Worst case is one chunk per page: descriptor plus a stored page. */
    TEST_ASSERT_EQ((int)(one - HIBER_HEADER_BYTES),
                   (int)(sizeof(hiber_chunk_desc_t) + HIBER_PAGE_SIZE),
                   "one page bounds to one descriptor plus one stored page");

    TEST_ASSERT(hibernate_image_encoded_bound(0x100000000ull) == 0,
                "a page count that cannot fit chunk_count is refused");
}

static void test_hiber_roundtrip_compressible(void)
{
    tp_arena_t a;
    hiber_decoder_t dec;
    uint64_t bytes, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x1000ull);
    TEST_ASSERT(bytes != 0, "compressible image encodes");
    TEST_ASSERT(bytes < (uint64_t)HIBER_HEADER_BYTES + TP_CHUNK_BYTES,
                "a redundant chunk encodes smaller than its raw pages");

    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                   HIBER_OK, "decoder accepts a freshly encoded image");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "chunk decodes");
    TEST_ASSERT(pfn == 0x1000ull, "chunk restores to the PFN it was written at");
    TEST_ASSERT_EQ((int)pages, (int)TP_CHUNK_PAGES, "chunk page count survives");
    TEST_ASSERT_EQ(memcmp(a.dst, a.src, TP_CHUNK_BYTES), 0,
                   "decoded pages are byte-identical to the originals");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_OK,
                   "completion gate accepts an intact image");

    tp_arena_free(&a);
}

static void test_hiber_roundtrip_incompressible(void)
{
    tp_arena_t a;
    hiber_decoder_t dec;
    hiber_chunk_desc_t desc;
    uint64_t bytes, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_incompressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x20ull);
    TEST_ASSERT(bytes != 0, "incompressible image encodes");
    /* The point of the stored path: an image that cannot be compressed must
     * not grow past its raw pages plus one descriptor. */
    TEST_ASSERT_EQ((int)(bytes - HIBER_HEADER_BYTES),
                   (int)(sizeof(hiber_chunk_desc_t) + TP_CHUNK_BYTES),
                   "an incompressible chunk is stored verbatim, never expanded");

    memcpy(&desc, a.img + HIBER_HEADER_BYTES, sizeof(desc));
    TEST_ASSERT_EQ((int)desc.flags, (int)HIBER_CHUNK_FLAG_STORED,
                   "the stored-verbatim flag is recorded");
    TEST_ASSERT_EQ((int)desc.stored_len, (int)desc.uncompressed_len,
                   "a stored chunk's lengths agree");

    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                   HIBER_OK, "stored image validates");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "stored chunk decodes");
    TEST_ASSERT_EQ(memcmp(a.dst, a.src, TP_CHUNK_BYTES), 0,
                   "stored chunk round-trips byte-identically");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_OK,
                   "stored image passes the completion gate");

    tp_arena_free(&a);
}

static void test_hiber_byte_layout(void)
{
    tp_arena_t a;
    uint64_t bytes;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x40ull);
    TEST_ASSERT(bytes != 0, "image for the layout check encodes");

    /* Read the ENCODED bytes at the offsets the cross-binary ABI pins, so a
     * serializer that drifted from the struct would fail here even though the
     * _Static_asserts still pass. */
    TEST_ASSERT(tp_le64(a.img + 0) == HIBER_MAGIC, "magic at offset 0");
    TEST_ASSERT_EQ((int)tp_le32(a.img + 8), 2,
                   "format_version at offset 8 is the pinned value 2");
    TEST_ASSERT_EQ((int)tp_le32(a.img + 12), (int)HIBER_HEADER_BYTES,
                   "header_bytes at offset 12");
    TEST_ASSERT(tp_le64(a.img + 16) == bytes, "image_bytes at offset 16");
    TEST_ASSERT(tp_le64(a.img + 24) == (uint64_t)TP_CHUNK_PAGES,
                "page_count at offset 24");
    TEST_ASSERT_EQ((int)tp_le32(a.img + 32), 1, "chunk_count at offset 32");
    TEST_ASSERT_EQ((int)tp_le32(a.img + 40), (int)TP_BOOT_INFO_VERSION,
                   "boot_info_version at offset 40");
    TEST_ASSERT_EQ((int)tp_le32(a.img + 96), (int)HIBER_CIPHER_NONE,
                   "plaintext is stated explicitly in aead_cipher_id");
    TEST_ASSERT_EQ((int)tp_le32(a.img + 140), (int)TP_CPU_COUNT,
                   "cpu_count_present at offset 140");
    TEST_ASSERT(tp_le64(a.img + 144) == 0x40000ull,
                "total_ram_pages at offset 144");
    /* Reserved bytes must be zero, and the first of them now starts at 152. */
    TEST_ASSERT_EQ((int)a.img[152], 0, "reserved area starts zeroed");

    tp_arena_free(&a);
}

static void test_hiber_header_rejections(void)
{
    tp_arena_t a;
    const hiber_header_t *h = NULL;
    hiber_header_t *w;
    uint64_t bytes;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0ull);
    TEST_ASSERT(bytes != 0, "image for the header checks encodes");
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_OK, "an intact header validates");

    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img,
                                                   HIBER_HEADER_BYTES - 1u, &h),
                   HIBER_ERR_TRUNCATED, "a buffer shorter than the header is refused");
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img + 1, (size_t)bytes, &h),
                   HIBER_ERR_ARG, "a misaligned image is refused");

    w = (hiber_header_t *)a.img;

    w->magic ^= 1ull;
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_MAGIC, "a bad magic is refused before anything else");
    w->magic ^= 1ull;

    w->format_version += 1u;
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_FORMAT, "a future format version is refused");
    w->format_version -= 1u;

    /* v1 put zero-only reserved space where v2 puts cpu_count_present and
     * total_ram_pages, so a v1 image must be refused as a VERSION mismatch
     * rather than surfacing later as some other malformed-image error. */
    w->format_version = 1u;
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_FORMAT,
                   "the superseded version 1 layout is refused by version");
    w->format_version = HIBER_FORMAT_VERSION;

    w->root_volume_id ^= 0xFFull;
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_HEADER_CRC, "a flipped header byte fails the header CRC");
    w->root_volume_id ^= 0xFFull;
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_OK, "restoring the byte restores validity");

    /* image_bytes claiming more than the caller handed over is a truncation,
     * not a CRC failure -- the header itself is intact. */
    w->image_bytes += 1ull;
    w->header_crc32c = 0u;
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_HEADER_CRC,
                   "editing image_bytes without fixing the CRC is caught");

    tp_arena_free(&a);
}

/* Rebuild the header CRC after deliberately editing a header field, so the
 * test can reach the field-level check that sits behind the CRC. */
static void tp_reseal_header(uint8_t *img)
{
    /* Derived from the struct, not re-typed: the production side binds its own
     * span constants with _Static_assert and this copy would otherwise be the
     * one place the span could drift unnoticed. */
    static const uint8_t zeros[4] = { 0, 0, 0, 0 };
    const size_t off = __builtin_offsetof(hiber_header_t, header_crc32c);
    const size_t len = sizeof(((hiber_header_t *)0)->header_crc32c);
    hiber_header_t *w = (hiber_header_t *)img;
    uint32_t crc;

    crc = kcrc32c(img, off);
    crc = kcrc32c_cont(crc, zeros, len);
    crc = kcrc32c_cont(crc, img + off + len, HIBER_HEADER_BYTES - off - len);
    w->header_crc32c = crc;
}

static void test_hiber_header_field_rejections(void)
{
    tp_arena_t a;
    const hiber_header_t *h = NULL;
    hiber_header_t *w;
    uint64_t bytes;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0ull);
    TEST_ASSERT(bytes != 0, "image for the field checks encodes");
    w = (hiber_header_t *)a.img;

    /* A resealed header proves the reseal helper itself is honest before it is
     * used to reach the checks behind the CRC. */
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_OK, "resealing reproduces a valid header CRC");

    w->reserved[7] = 0xFF;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_RESERVED,
                   "a non-zero reserved byte is refused even with a valid CRC");
    w->reserved[7] = 0;

    w->flags = 0x8000u;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_FLAGS, "an unknown header flag is refused, not ignored");
    w->flags = 0u;

    w->resume_type = 99u;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_FORMAT, "an unknown resume type is refused");
    w->resume_type = HIBER_RESUME_FULL;

    w->aead_cipher_id = 77u;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_FORMAT, "an unknown AEAD cipher id is refused");
    w->aead_cipher_id = HIBER_CIPHER_NONE;

    w->image_bytes = bytes + 1ull;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_TRUNCATED,
                   "an image_bytes past the buffer is a truncation");
    w->image_bytes = bytes;

    /* A page count no single chunk could ever hold contradicts the header on
     * its own terms, so it is caught before a byte of payload is touched. */
    w->page_count = (uint64_t)HIBER_CHUNK_PAGES + 1ull;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_COUNT,
                   "a page count no single chunk could hold is refused");

    /* A merely WRONG page count is indistinguishable at the header, which is
     * exactly why the completion gate re-counts what it actually decoded. */
    {
        hiber_decoder_t dec;
        uint64_t pfn = 0;
        uint32_t pages = 0;

        w->page_count = (uint64_t)TP_CHUNK_PAGES + 1ull;
        tp_reseal_header(a.img);
        TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                       HIBER_OK, "a plausible page count passes the header");
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "decoder begins on the overstated image");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_OK, "its one real chunk still decodes");
        TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_ERR_COUNT,
                       "the completion gate catches the overstated page count");
    }

    tp_arena_free(&a);
}

static void test_hiber_payload_corruption(void)
{
    tp_arena_t a;
    hiber_decoder_t dec;
    uint64_t bytes, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    /* The stored path is used deliberately: a flipped byte still decodes, so
     * the ONLY thing that can catch it is the completion gate's payload CRC.
     * On the compressed path LZ4 would often refuse first and the test would
     * prove nothing about the CRC. */
    tp_fill_incompressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x80ull);
    TEST_ASSERT(bytes != 0, "image for the corruption check encodes");

    a.img[HIBER_HEADER_BYTES + sizeof(hiber_chunk_desc_t) + 17u] ^= 0x40u;

    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                   HIBER_OK, "payload corruption does not disturb the header");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "a corrupted stored chunk still decodes");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_ERR_PAYLOAD_CRC,
                   "the completion gate is what catches payload corruption");

    tp_arena_free(&a);
}

static void test_hiber_descriptor_rejections(void)
{
    tp_arena_t a;
    hiber_decoder_t dec;
    hiber_chunk_desc_t desc;
    uint8_t *drec;
    uint64_t bytes, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    /* A COMPRESSIBLE source is what makes the truncation check meaningful: the
     * chunk's stored bytes are then far shorter than its pages, so a forged
     * stored_len can be internally consistent and still run past the image. */
    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x200ull);
    TEST_ASSERT(bytes != 0, "image for the descriptor checks encodes");
    drec = a.img + HIBER_HEADER_BYTES;
    memcpy(&desc, drec, sizeof(desc));
    TEST_ASSERT(desc.stored_len < desc.uncompressed_len,
                "the descriptor checks run against a compressed chunk");

    /* Descriptor validation runs before the payload CRC is consulted, so
     * these refusals do not need the CRC to be repaired. */
    {
        hiber_chunk_desc_t bad = desc;
        bad.flags = 0x40u;
        memcpy(drec, &bad, sizeof(bad));
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid with a tampered descriptor");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_FLAGS, "an unknown chunk flag is refused");
    }
    {
        /* Internally consistent (still shorter than the pages it claims to
         * decode to) yet far longer than the bytes actually present. */
        hiber_chunk_desc_t bad = desc;
        bad.stored_len = desc.uncompressed_len - 1u;
        memcpy(drec, &bad, sizeof(bad));
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_TRUNCATED,
                       "a stored_len past the image end is refused");
    }
    {
        hiber_chunk_desc_t bad = desc;
        bad.stored_len = desc.uncompressed_len;
        memcpy(drec, &bad, sizeof(bad));
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_BOUNDS,
                       "a compressed chunk that did not shrink is refused");
    }
    {
        hiber_chunk_desc_t bad = desc;
        bad.stored_len = 0u;
        memcpy(drec, &bad, sizeof(bad));
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_BOUNDS, "a zero-length chunk record is refused");
    }
    {
        /* Claiming the verbatim path while carrying compressed bytes is the
         * mismatch a stored chunk's length equality exists to catch. */
        hiber_chunk_desc_t bad = desc;
        bad.flags = HIBER_CHUNK_FLAG_STORED;
        memcpy(drec, &bad, sizeof(bad));
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_BOUNDS,
                       "a stored chunk whose lengths disagree is refused");
    }
    {
        hiber_chunk_desc_t bad = desc;
        bad.page_count = HIBER_CHUNK_PAGES + 1u;
        memcpy(drec, &bad, sizeof(bad));
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_BOUNDS,
                       "a chunk claiming more than the chunk size is refused");
    }
    {
        hiber_chunk_desc_t bad = desc;
        bad.uncompressed_len = desc.uncompressed_len - 1u;
        memcpy(drec, &bad, sizeof(bad));
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_BOUNDS,
                       "uncompressed_len must equal page_count times the page size");
    }

    memcpy(drec, &desc, sizeof(desc));
    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                   HIBER_OK, "restored descriptor validates again");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "restored descriptor decodes");

    tp_arena_free(&a);
}

static void test_hiber_destination_map(void)
{
    tp_arena_t a;
    hiber_encoder_t enc;
    hiber_decoder_t dec;
    hiber_ident_t id;
    uint64_t bytes = 0, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }
    tp_ident_init(&id);
    tp_fill_compressible(a.src, TP_CHUNK_BYTES * 2u);

    TEST_ASSERT_EQ(hibernate_image_begin(&enc, a.img, TP_IMG_BYTES), HIBER_OK,
                   "encoder begins");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0x100ull, a.src,
                                                TP_CHUNK_PAGES),
                   HIBER_OK, "first sparse extent appends");
    /* A gap between extents is the normal case: hibernation saves the pages
     * the PMM has in use, which are not contiguous. */
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0x900ull,
                                                a.src + TP_CHUNK_BYTES,
                                                TP_CHUNK_PAGES),
                   HIBER_OK, "second sparse extent appends after a gap");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0x100ull, a.src,
                                                TP_CHUNK_PAGES),
                   HIBER_ERR_ORDER, "a descending extent is refused");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0x901ull, a.src,
                                                TP_CHUNK_PAGES),
                   HIBER_ERR_ORDER, "an extent overlapping the previous is refused");
    TEST_ASSERT_EQ(hibernate_image_finalize(&enc, &id, &bytes), HIBER_OK,
                   "sparse image finalizes");

    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                   HIBER_OK, "sparse image validates");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "first extent decodes");
    TEST_ASSERT(pfn == 0x100ull, "first extent keeps its destination PFN");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "second extent decodes");
    TEST_ASSERT(pfn == 0x900ull, "second extent keeps its destination PFN");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_OK,
                   "sparse image passes the completion gate");

    tp_arena_free(&a);
}

static void test_hiber_completion_gate(void)
{
    tp_arena_t a;
    hiber_decoder_t dec;
    uint64_t bytes, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x300ull);
    TEST_ASSERT(bytes != 0, "image for the completion checks encodes");

    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                   HIBER_OK, "decoder begins");
    /* Finishing early must fail: the point of the gate is that a caller which
     * stops halfway cannot come away believing the image was whole. */
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_ERR_COUNT,
                   "finishing before every chunk is read is refused");

    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst,
                                              TP_CHUNK_BYTES - 1u, &pfn, &pages),
                   HIBER_ERR_NOSPACE,
                   "a destination smaller than the chunk is refused");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "the chunk decodes with a correctly sized buffer");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_ERR_STATE, "reading past the last chunk is refused");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_OK,
                   "the completion gate accepts a fully read image");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_ERR_STATE,
                   "the completion gate cannot be run twice");

    tp_arena_free(&a);
}

static void test_hiber_encoder_arguments(void)
{
    tp_arena_t a;
    hiber_encoder_t enc;
    hiber_ident_t id;
    uint64_t bytes = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }
    tp_ident_init(&id);

    TEST_ASSERT_EQ(hibernate_image_begin(&enc, a.img, HIBER_HEADER_BYTES - 1u),
                   HIBER_ERR_NOSPACE, "a buffer below one header is refused");
    TEST_ASSERT_EQ(hibernate_image_begin(&enc, a.img + 1, TP_IMG_BYTES),
                   HIBER_ERR_ARG, "a misaligned output buffer is refused");
    TEST_ASSERT_EQ(hibernate_image_begin(&enc, NULL, TP_IMG_BYTES),
                   HIBER_ERR_ARG, "a NULL output buffer is refused");

    TEST_ASSERT_EQ(hibernate_image_begin(&enc, a.img, TP_IMG_BYTES), HIBER_OK,
                   "encoder begins");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0, a.src, 0),
                   HIBER_ERR_ARG, "a zero-page chunk is refused");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0, a.src,
                                                HIBER_CHUNK_PAGES + 1u),
                   HIBER_ERR_ARG, "a chunk larger than the chunk size is refused");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0xFFFFFFFFFFFFFFFFull,
                                                a.src, TP_CHUNK_PAGES),
                   HIBER_ERR_BOUNDS, "a chunk whose PFN range wraps is refused");

    /* An image with no pages is legal and must survive the whole pipeline. */
    TEST_ASSERT_EQ(hibernate_image_finalize(&enc, &id, &bytes), HIBER_OK,
                   "an empty image finalizes");
    TEST_ASSERT(bytes == (uint64_t)HIBER_HEADER_BYTES,
                "an empty image is exactly one header");
    TEST_ASSERT_EQ(hibernate_image_finalize(&enc, &id, &bytes),
                   HIBER_ERR_STATE, "finalizing twice is refused");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0, a.src, TP_CHUNK_PAGES),
                   HIBER_ERR_STATE, "appending after finalize is refused");

    tp_arena_free(&a);
}

static void test_hiber_empty_image_roundtrip(void)
{
    tp_arena_t a;
    hiber_encoder_t enc;
    hiber_decoder_t dec;
    hiber_ident_t id;
    uint64_t bytes = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }
    tp_ident_init(&id);

    TEST_ASSERT_EQ(hibernate_image_begin(&enc, a.img, TP_IMG_BYTES), HIBER_OK,
                   "encoder begins");
    TEST_ASSERT_EQ(hibernate_image_finalize(&enc, &id, &bytes), HIBER_OK,
                   "empty image finalizes");
    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                   HIBER_OK, "empty image validates");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_OK,
                   "empty image passes the completion gate with no chunks read");

    tp_arena_free(&a);
}

static void test_hiber_identity_guard(void)
{
    tp_arena_t a;
    const hiber_header_t *h = NULL;
    hiber_ident_t now;
    uint64_t bytes;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x400ull);
    TEST_ASSERT(bytes != 0, "image for the identity checks encodes");
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_OK, "header validates");

    tp_ident_init(&now);
    TEST_ASSERT_EQ((int)hibernate_image_kernel_matches(h, &now),
                   (int)HIBER_IDENT_OK, "the writing kernel's own identity matches");

    /* A different kernel binary and a different handoff ABI are different
     * failures, and an operator reading the log needs to tell them apart. */
    tp_ident_init(&now);
    now.kernel_id[31] ^= 0x01;
    TEST_ASSERT_EQ((int)hibernate_image_kernel_matches(h, &now),
                   (int)HIBER_IDENT_KERNEL_ARTIFACT_MISMATCH,
                   "a one-bit kernel id difference refuses the image");

    tp_ident_init(&now);
    now.boot_info_version += 1u;
    TEST_ASSERT_EQ((int)hibernate_image_kernel_matches(h, &now),
                   (int)HIBER_IDENT_BOOT_INFO_ABI_MISMATCH,
                   "a handoff ABI change reports its own reason");

    tp_ident_init(&now);
    now.resume_type = HIBER_RESUME_FAST_STARTUP;
    TEST_ASSERT_EQ((int)hibernate_image_kernel_matches(h, &now),
                   (int)HIBER_IDENT_RESUME_TYPE_MISMATCH,
                   "a fast-startup image is not accepted as a full hibernate");

    TEST_ASSERT_EQ((int)hibernate_image_kernel_matches(NULL, &now),
                   (int)HIBER_IDENT_KERNEL_ARTIFACT_MISMATCH,
                   "a missing header refuses rather than defaulting to a match");
    TEST_ASSERT_EQ((int)hibernate_image_kernel_matches(h, NULL),
                   (int)HIBER_IDENT_KERNEL_ARTIFACT_MISMATCH,
                   "a missing current identity refuses");

    tp_arena_free(&a);
}

static void test_hiber_pfn_ceiling(void)
{
    tp_arena_t a;
    hiber_encoder_t enc;
    hiber_decoder_t dec;
    hiber_ident_t id;
    hiber_chunk_desc_t desc;
    uint8_t *drec;
    uint64_t bytes = 0, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }
    tp_ident_init(&id);
    tp_fill_compressible(a.src, TP_CHUNK_BYTES);

    /* The single highest frame, on its own encoder so the ordering cursor this
     * append leaves behind cannot affect the sequence below. */
    {
        hiber_encoder_t solo;
        TEST_ASSERT_EQ(hibernate_image_begin(&solo, a.img, TP_IMG_BYTES),
                       HIBER_OK, "solo encoder begins");
        TEST_ASSERT_EQ(hibernate_image_append_chunk(&solo, HIBER_MAX_PFN,
                                                    a.src, 1u),
                       HIBER_OK,
                       "a one-page extent ON the ceiling frame is accepted");
    }

    TEST_ASSERT_EQ(hibernate_image_begin(&enc, a.img, TP_IMG_BYTES), HIBER_OK,
                   "encoder begins");
    /* HIBER_MAX_PFN is INCLUSIVE, so the cases below straddle it by one frame
     * each. An off-by-one here is not a rounding error: the header states the
     * rule normatively and a UEFI mirror implementing it would then accept
     * what this side rejects. */
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc,
                                                HIBER_MAX_PFN - TP_CHUNK_PAGES + 2ull,
                                                a.src, TP_CHUNK_PAGES),
                   HIBER_ERR_BOUNDS,
                   "an extent whose last frame is one past the ceiling is refused");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, HIBER_MAX_PFN + 1ull,
                                                a.src, TP_CHUNK_PAGES),
                   HIBER_ERR_BOUNDS, "a start PFN above the ceiling is refused");
    /* The last frame landing exactly ON the ceiling is legal and must encode. */
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc,
                                                HIBER_MAX_PFN - TP_CHUNK_PAGES + 1ull,
                                                a.src, TP_CHUNK_PAGES),
                   HIBER_OK,
                   "an extent whose last frame IS the ceiling is accepted");
    TEST_ASSERT_EQ(hibernate_image_finalize(&enc, &id, &bytes), HIBER_OK,
                   "boundary image finalizes");
    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                   HIBER_OK, "boundary image validates");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "boundary extent decodes");
    TEST_ASSERT(pfn == HIBER_MAX_PFN - TP_CHUNK_PAGES + 1ull,
                "boundary extent keeps its PFN");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_OK,
                   "boundary image passes the completion gate");

    /* A hostile image can carry a PFN the encoder would never emit, so the
     * decoder must apply the same ceiling rather than trusting its producer. */
    drec = a.img + HIBER_HEADER_BYTES;
    memcpy(&desc, drec, sizeof(desc));
    {
        hiber_chunk_desc_t bad = desc;
        bad.start_pfn = HIBER_MAX_PFN;
        memcpy(drec, &bad, sizeof(bad));
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_BOUNDS,
                       "a forged descriptor past the PFN ceiling is refused");
    }
    memcpy(drec, &desc, sizeof(desc));

    tp_arena_free(&a);
}

static void test_hiber_hostile_decompression(void)
{
    tp_arena_t a;
    hiber_decoder_t dec;
    hiber_chunk_desc_t desc;
    uint8_t *drec;
    uint64_t bytes, pfn = 0;
    uint32_t pages = 0;
    size_t   i;
    int      canary_intact;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x500ull);
    TEST_ASSERT(bytes != 0, "image for the decompression checks encodes");
    drec = a.img + HIBER_HEADER_BYTES;
    memcpy(&desc, drec, sizeof(desc));

    {
        /* A one-page descriptor in front of a block that expands to two. The
         * refusal must arrive before the extra page is written, so the canary
         * past the DECLARED output is what this case actually asserts -- the
         * return code alone would look identical either way. */
        hiber_chunk_desc_t bad = desc;
        bad.page_count       = 1u;
        bad.uncompressed_len = HIBER_PAGE_SIZE;
        memcpy(drec, &bad, sizeof(bad));

        memset(a.dst, 0x5A, TP_CHUNK_BYTES);
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_LZ4,
                       "a block expanding past its declared pages is refused");
        canary_intact = 1;
        for (i = HIBER_PAGE_SIZE; i < TP_CHUNK_BYTES; i++) {
            if (a.dst[i] != 0x5A) {
                canary_intact = 0;
                break;
            }
        }
        TEST_ASSERT(canary_intact,
                    "nothing is written past the declared output length");
    }
    {
        /* A malformed block: a literal-length token that runs off the end of
         * the input the descriptor declares. */
        hiber_chunk_desc_t bad = desc;
        uint8_t saved = drec[sizeof(desc)];

        memcpy(drec, &bad, sizeof(bad));
        drec[sizeof(desc)] = 0xFFu;
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_LZ4, "a malformed compressed block is refused");
        drec[sizeof(desc)] = saved;
    }
    {
        /* Valid LZ4 that decodes SHORT of the declared length: accepting it
         * would leave the rest of the destination page holding stale bytes. */
        hiber_chunk_desc_t bad = desc;
        bad.page_count       = TP_CHUNK_PAGES;
        bad.uncompressed_len = TP_CHUNK_BYTES;
        memcpy(drec, &bad, sizeof(bad));
        /* Re-encode a single page's worth into the record so the block is
         * valid but produces half the declared bytes. */
        {
            int clen = lz4_compress(a.src, HIBER_PAGE_SIZE,
                                    drec + sizeof(desc),
                                    (size_t)desc.uncompressed_len - 1u);
            TEST_ASSERT(clen > 0, "short-output fixture compresses");
            bad.stored_len = (uint32_t)clen;
            memcpy(drec, &bad, sizeof(bad));
        }
        TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                       HIBER_OK, "header still valid");
        TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                                  &pfn, &pages),
                       HIBER_ERR_LZ4,
                       "valid LZ4 producing fewer bytes than declared is refused");
    }

    tp_arena_free(&a);
}

static void test_hiber_truncation_frontiers(void)
{
    tp_arena_t a;
    hiber_encoder_t enc;
    hiber_decoder_t dec;
    hiber_ident_t id;
    const hiber_header_t *h = NULL;
    hiber_header_t *w;
    uint64_t bytes = 0, first_rec_end = 0, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }
    tp_ident_init(&id);
    tp_fill_compressible(a.src, TP_CHUNK_BYTES * 2u);

    TEST_ASSERT_EQ(hibernate_image_begin(&enc, a.img, TP_IMG_BYTES), HIBER_OK,
                   "encoder begins");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0x10ull, a.src,
                                                TP_CHUNK_PAGES),
                   HIBER_OK, "first chunk appends");
    first_rec_end = (uint64_t)enc.cursor;
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0x50ull,
                                                a.src + TP_CHUNK_BYTES,
                                                TP_CHUNK_PAGES),
                   HIBER_OK, "second chunk appends");
    TEST_ASSERT_EQ(hibernate_image_finalize(&enc, &id, &bytes), HIBER_OK,
                   "two-chunk image finalizes");
    w = (hiber_header_t *)a.img;

    /* An image_bytes below one header would underflow the payload
     * subtraction if it were not refused first. */
    w->image_bytes = (uint64_t)HIBER_HEADER_BYTES - 1ull;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_BOUNDS,
                   "an image_bytes below one header is refused");

    /* A payload too small to hold even the descriptors it claims. */
    w->image_bytes = (uint64_t)HIBER_HEADER_BYTES + 40ull;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_TRUNCATED,
                   "a payload smaller than its own descriptors is refused");

    /* A second record with fewer than a descriptor's worth of bytes left:
     * the first record still decodes, and the truncation is caught before the
     * descriptor is copied out. */
    w->image_bytes = first_rec_end + 10ull;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes),
                   HIBER_OK, "the truncated image still has a valid header");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "the intact first record decodes");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_ERR_TRUNCATED,
                   "a record with fewer than a descriptor's bytes left is refused");

    tp_arena_free(&a);
}

static void test_hiber_oversized_buffer(void)
{
    tp_arena_t a;
    hiber_decoder_t dec;
    uint64_t bytes, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x700ull);
    TEST_ASSERT(bytes != 0, "image for the oversized-buffer check encodes");

    /* A reader working in sectors or pages holds MORE bytes than the image
     * occupies, so an undeclared suffix must be ignored rather than refused.
     * The header is untouched here: image_bytes still describes the real
     * image, which is what distinguishes this from the declared-suffix case
     * that decode_finish rejects. */
    a.img[bytes] = 0xC3u;
    a.img[bytes + 1u] = 0x5Au;

    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img,
                                                (size_t)bytes + 2u),
                   HIBER_OK, "a buffer larger than the image is accepted");
    TEST_ASSERT(dec.img_len == (size_t)bytes,
                "the decoder bounds itself by image_bytes, not by the buffer");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "the chunk decodes from the oversized buffer");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_OK,
                   "an UNdeclared suffix is outside the image and is ignored");

    tp_arena_free(&a);
}

static void test_hiber_trailing_bytes(void)
{
    tp_arena_t a;
    hiber_decoder_t dec;
    hiber_header_t *w;
    uint64_t bytes, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x600ull);
    TEST_ASSERT(bytes != 0, "image for the trailing check encodes");

    /* Declare one extra byte the chunk records do not account for, and reseal
     * so the header itself is beyond reproach. payload_crc32c still describes
     * only the consumed record, so the ONLY thing that can reject this
     * parser-ambiguous image is the exact-end check. */
    a.img[bytes] = 0xC3u;
    w = (hiber_header_t *)a.img;
    w->image_bytes = bytes + 1ull;
    tp_reseal_header(a.img);

    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, (size_t)bytes + 1u),
                   HIBER_OK, "the padded image has a valid header");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "its one real chunk decodes");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_ERR_TRAILING,
                   "unconsumed trailing bytes are refused");

    tp_arena_free(&a);
}

static void test_hiber_full_chunk(void)
{
    /* Every other suite runs at two pages. This one runs at the format's real
     * chunk size, so the 256 KiB LZ4 path and HIBER_CHUNK_BYTES are actually
     * exercised rather than merely defined. */
    uintptr_t img_phys, src_phys, dst_phys;
    uint8_t *img, *src, *dst;
    hiber_encoder_t enc;
    hiber_decoder_t dec;
    hiber_ident_t id;
    uint64_t bytes = 0, pfn = 0;
    uint32_t pages = 0;
    const uint32_t img_frames = HIBER_CHUNK_PAGES + 2u;

    img_phys = pmm_alloc_contiguous(img_frames);
    src_phys = pmm_alloc_contiguous(HIBER_CHUNK_PAGES);
    dst_phys = pmm_alloc_contiguous(HIBER_CHUNK_PAGES);
    if (img_phys == 0 || src_phys == 0 || dst_phys == 0) {
        if (img_phys != 0) pmm_free_contiguous(img_phys, img_frames);
        if (src_phys != 0) pmm_free_contiguous(src_phys, HIBER_CHUNK_PAGES);
        if (dst_phys != 0) pmm_free_contiguous(dst_phys, HIBER_CHUNK_PAGES);
        TEST_SKIP("full-chunk fixture needs 194 contiguous frames");
        return;
    }
    img = (uint8_t *)img_phys;
    src = (uint8_t *)src_phys;
    dst = (uint8_t *)dst_phys;

    tp_ident_init(&id);
    tp_fill_compressible(src, HIBER_CHUNK_BYTES);

    TEST_ASSERT_EQ(hibernate_image_begin(&enc, img,
                                         (size_t)img_frames * HIBER_PAGE_SIZE),
                   HIBER_OK, "full-chunk encoder begins");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0x2000ull, src,
                                                HIBER_CHUNK_PAGES),
                   HIBER_OK, "a full 64-page chunk appends");
    TEST_ASSERT_EQ(hibernate_image_finalize(&enc, &id, &bytes), HIBER_OK,
                   "full-chunk image finalizes");

    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, img, (size_t)bytes),
                   HIBER_OK, "full-chunk image validates");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, dst, HIBER_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_OK, "the full chunk decodes");
    TEST_ASSERT_EQ((int)pages, (int)HIBER_CHUNK_PAGES,
                   "the full chunk keeps its page count");
    TEST_ASSERT_EQ(memcmp(dst, src, HIBER_CHUNK_BYTES), 0,
                   "256 KiB round-trips byte-identically");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_OK,
                   "full-chunk image passes the completion gate");

    pmm_free_contiguous(img_phys, img_frames);
    pmm_free_contiguous(src_phys, HIBER_CHUNK_PAGES);
    pmm_free_contiguous(dst_phys, HIBER_CHUNK_PAGES);
}

static void test_hiber_null_arguments(void)
{
    tp_arena_t a;
    hiber_encoder_t enc;
    hiber_decoder_t dec;
    hiber_ident_t id;
    const hiber_header_t *h = NULL;
    uint64_t bytes = 0, pfn = 0;
    uint32_t pages = 0;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }
    tp_ident_init(&id);

    TEST_ASSERT_EQ(hibernate_image_begin(NULL, a.img, TP_IMG_BYTES),
                   HIBER_ERR_ARG, "a NULL encoder is refused");
    TEST_ASSERT_EQ(hibernate_image_decode_begin(NULL, a.img, TP_IMG_BYTES),
                   HIBER_ERR_ARG, "a NULL decoder is refused");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(NULL), HIBER_ERR_ARG,
                   "a NULL decoder is refused by the completion gate");
    TEST_ASSERT_EQ(hibernate_image_header_validate(NULL, TP_IMG_BYTES, &h),
                   HIBER_ERR_ARG, "a NULL image is refused");
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, TP_IMG_BYTES, NULL),
                   HIBER_ERR_ARG, "a NULL out-pointer is refused");

    /* A failed begin must leave state that REFUSES, not state that is
     * whatever the caller's stack happened to hold: the assertion macros do
     * not abort, so a regression would otherwise walk into a wild write. */
    TEST_ASSERT_EQ(hibernate_image_begin(&enc, NULL, TP_IMG_BYTES),
                   HIBER_ERR_ARG, "a NULL output buffer is refused");
    TEST_ASSERT_EQ(hibernate_image_append_chunk(&enc, 0, a.src, TP_CHUNK_PAGES),
                   HIBER_ERR_STATE, "appending after a failed begin is refused");
    TEST_ASSERT_EQ(hibernate_image_finalize(&enc, &id, &bytes),
                   HIBER_ERR_STATE, "finalizing after a failed begin is refused");

    TEST_ASSERT_EQ(hibernate_image_decode_begin(&dec, a.img, 8u),
                   HIBER_ERR_TRUNCATED, "a too-short image is refused");
    TEST_ASSERT_EQ(hibernate_image_read_chunk(&dec, a.dst, TP_CHUNK_BYTES,
                                              &pfn, &pages),
                   HIBER_ERR_STATE, "reading after a failed begin is refused");
    TEST_ASSERT_EQ(hibernate_image_decode_finish(&dec), HIBER_ERR_STATE,
                   "finishing after a failed begin is refused");

    TEST_ASSERT(hibernate_image_encoded_bound(0xFFFFFFFFull) != 0,
                "the largest representable page count still bounds");

    tp_arena_free(&a);
}

static void test_hiber_aead_block_under_none(void)
{
    tp_arena_t a;
    const hiber_header_t *h = NULL;
    hiber_header_t *w;
    hiber_encoder_t enc;
    hiber_ident_t id;
    uint64_t bytes;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0x900ull);
    TEST_ASSERT(bytes != 0, "image for the AEAD-block check encodes");
    w = (hiber_header_t *)a.img;

    /* Under HIBER_CIPHER_NONE the AEAD block is reserved space. Leaving it
     * unconstrained would put 32 CRC-covered bytes outside the reserved rule
     * and hand a later cipher-aware reader a plaintext image that already
     * carries a key id, nonce and tag. */
    w->aead_key_id = 1u;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_RESERVED,
                   "a key id under CIPHER_NONE is refused");
    w->aead_key_id = 0u;

    w->aead_nonce[11] = 0xAAu;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_RESERVED,
                   "a nonce under CIPHER_NONE is refused");
    w->aead_nonce[11] = 0u;

    w->aead_tag[0] = 0x01u;
    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_ERR_RESERVED,
                   "a tag under CIPHER_NONE is refused");
    w->aead_tag[0] = 0u;

    tp_reseal_header(a.img);
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_OK, "a zeroed AEAD block under CIPHER_NONE validates");

    /* A zero CPU count is refused where the image is produced, not only where
     * it is read. */
    tp_ident_init(&id);
    id.cpu_count_present = 0u;
    TEST_ASSERT_EQ(hibernate_image_begin(&enc, a.img, TP_IMG_BYTES), HIBER_OK,
                   "encoder begins");
    TEST_ASSERT_EQ(hibernate_image_finalize(&enc, &id, &bytes),
                   HIBER_ERR_ARG, "an image claiming zero CPUs is refused");

    tp_arena_free(&a);
}

static void test_hiber_cpu_topology_guard(void)
{
    tp_arena_t a;
    const hiber_header_t *h = NULL;
    hiber_ident_t now;
    uint64_t bytes;

    if (!tp_arena_init(&a)) {
        TEST_SKIP("hibernation codec scratch frames unavailable");
        return;
    }

    tp_fill_compressible(a.src, TP_CHUNK_BYTES);
    bytes = tp_encode_one(&a, 0xA00ull);
    TEST_ASSERT(bytes != 0, "image for the topology checks encodes");
    TEST_ASSERT_EQ(hibernate_image_header_validate(a.img, (size_t)bytes, &h),
                   HIBER_OK, "header validates");
    TEST_ASSERT_EQ((int)h->cpu_count_present, (int)TP_CPU_COUNT,
                   "the captured CPU count survives into the header");

    /* Fewer CPUs than the image was captured on is fatal -- it carries
     * per-CPU state for processors that no longer exist. More is fine. */
    tp_ident_init(&now);
    now.cpu_count_present = TP_CPU_COUNT - 1u;
    TEST_ASSERT_EQ((int)hibernate_image_kernel_matches(h, &now),
                   (int)HIBER_IDENT_CPU_TOPOLOGY_MISMATCH,
                   "resuming on fewer CPUs than were captured is refused");

    tp_ident_init(&now);
    now.cpu_count_present = TP_CPU_COUNT + 4u;
    TEST_ASSERT_EQ((int)hibernate_image_kernel_matches(h, &now),
                   (int)HIBER_IDENT_OK,
                   "resuming on more CPUs than were captured is accepted");

    /* The three fields the guard deliberately does not compare must not
     * quietly start failing the image if a future edit adds them. */
    tp_ident_init(&now);
    now.root_volume_id    ^= 0xFFull;
    now.resume_generation += 100u;
    now.total_ram_pages   /= 2u;
    TEST_ASSERT_EQ((int)hibernate_image_kernel_matches(h, &now),
                   (int)HIBER_IDENT_OK,
                   "volume id, generation and RAM size are not this guard's job");

    tp_arena_free(&a);
}

/* ---- Registration ---- */

void test_register_hibernate_image(void)
{
    test_suite_register_cat("PM: hibernation encoded-size bound",
                            test_hiber_encoded_bound, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation compressible round-trip",
                            test_hiber_roundtrip_compressible, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation stored-verbatim round-trip",
                            test_hiber_roundtrip_incompressible, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation on-disk byte layout",
                            test_hiber_byte_layout, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation header rejections",
                            test_hiber_header_rejections, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation header field rejections",
                            test_hiber_header_field_rejections, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation payload corruption",
                            test_hiber_payload_corruption, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation descriptor rejections",
                            test_hiber_descriptor_rejections, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation sparse destination map",
                            test_hiber_destination_map, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation decode completion gate",
                            test_hiber_completion_gate, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation encoder argument checks",
                            test_hiber_encoder_arguments, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation empty image round-trip",
                            test_hiber_empty_image_roundtrip, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation kernel identity guard",
                            test_hiber_identity_guard, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation PFN address ceiling",
                            test_hiber_pfn_ceiling, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation hostile decompression",
                            test_hiber_hostile_decompression, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation truncation frontiers",
                            test_hiber_truncation_frontiers, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation trailing bytes",
                            test_hiber_trailing_bytes, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation oversized read buffer",
                            test_hiber_oversized_buffer, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation full 64-page chunk",
                            test_hiber_full_chunk, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation NULL and failed-begin arguments",
                            test_hiber_null_arguments, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation AEAD block under CIPHER_NONE",
                            test_hiber_aead_block_under_none, TEST_CAT_BOOT);
    test_suite_register_cat("PM: hibernation CPU topology guard",
                            test_hiber_cpu_topology_guard, TEST_CAT_BOOT);

}

#endif /* KERNEL_TESTS */
