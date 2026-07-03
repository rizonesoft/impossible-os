/* ============================================================================
 * nls.c -- nls_table_v1 loader + compiled invariant fallback + query accessors
 *
 * Section 4 of the atom/NLS/locale subsystem. See nls.h for the format contract
 * and the two-fold-authority design (rtl_upcase_char is the always-compiled
 * ASCII/Latin-1 authority; nls_upcase_char is the full-BMP superset that adds
 * U+0100.. from a loaded table but NEVER lets disk data override < U+0100).
 *
 * SMP: nls_init() runs once at Phase 2; APs are already online, so the active
 * descriptor is published with a release store and read with an acquire load.
 * The descriptor and its backing PMM blob are immutable after publish and never
 * freed. A parse/load failure leaves s_active NULL and the accessors serve the
 * compiled fallback.
 * ============================================================================ */

#include "kernel/types.h"
#include "libc/string.h"           /* memcpy / memset */
#include "kernel/nt/nls.h"
#include "kernel/nt/nt_rtlstr.h"   /* rtl_upcase_char -- the < U+0100 authority */
#include "kernel/kchecksum.h"      /* kcrc32 / kcrc32_cont */
#include "kernel/fs/vfs.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"      /* SUBSYS_NLS, kernel_subsystem_apply_result, BOOT_* */

/* Active table, release-published by nls_init(). NULL => compiled fallback. */
static const nls_published_t *s_active = 0;

/* Storage for the one loaded descriptor (filled by parse, then published). */
static nls_published_t s_loaded;

#define NLS_INVARIANT_LCID  0x007Fu   /* LOCALE_INVARIANT */
#define NLS_PAGE_SIZE       4096u
#define NLS_TABLE_PATH      "C:\\Impossible\\System\\NLS\\invariant.nls"

/* ---- Compiled invariant CTYPE1 classification (ASCII + Latin-1) ----------- */
/* Best-effort C1 flags for U+0000..U+00FF, served when no CTYPE1 chunk covers
 * the code point. The authoritative full-BMP data comes from a loaded table. */
static uint16_t nls_compiled_ctype1(uint16_t c)
{
    if (c > 0xFF)
        return 0;   /* compiled range is ASCII + Latin-1 only */

    /* C0/C1 control ranges + DEL. */
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F)) {
        uint16_t f = NLS_C1_CNTRL | NLS_C1_DEFINED;
        if (c == 0x09)                          /* TAB */
            f |= NLS_C1_SPACE | NLS_C1_BLANK;
        else if (c >= 0x0A && c <= 0x0D)        /* LF VT FF CR */
            f |= NLS_C1_SPACE;
        return f;
    }
    if (c == 0x20)                              /* SPACE */
        return NLS_C1_SPACE | NLS_C1_BLANK | NLS_C1_DEFINED;
    if (c == 0xA0)                              /* NBSP */
        return NLS_C1_SPACE | NLS_C1_BLANK | NLS_C1_DEFINED;
    if (c >= '0' && c <= '9')                   /* ASCII digits */
        return NLS_C1_DIGIT | NLS_C1_XDIGIT | NLS_C1_DEFINED;
    if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
        uint16_t f = NLS_C1_ALPHA | NLS_C1_XDIGIT | NLS_C1_DEFINED;
        f |= (c <= 'F') ? NLS_C1_UPPER : NLS_C1_LOWER;
        return f;
    }
    if (c >= 'G' && c <= 'Z')
        return NLS_C1_UPPER | NLS_C1_ALPHA | NLS_C1_DEFINED;
    if (c >= 'g' && c <= 'z')
        return NLS_C1_LOWER | NLS_C1_ALPHA | NLS_C1_DEFINED;

    /* Latin-1 letters. Uppercase 0xC0-0xD6, 0xD8-0xDE; lowercase 0xDF-0xF6,
     * 0xF8-0xFF, plus the ordinal/micro letters 0xAA/0xB5/0xBA. */
    if ((c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xDE))
        return NLS_C1_UPPER | NLS_C1_ALPHA | NLS_C1_DEFINED;
    if ((c >= 0xDF && c <= 0xF6) || (c >= 0xF8 && c <= 0xFF) ||
        c == 0xAA || c == 0xB5 || c == 0xBA)
        return NLS_C1_LOWER | NLS_C1_ALPHA | NLS_C1_DEFINED;

    /* Everything else in ASCII/Latin-1 printable space is punctuation/symbol
     * (includes 0xD7 multiply and 0xF7 divide). */
    return NLS_C1_PUNCT | NLS_C1_DEFINED;
}

/* ---- Pure format parser -------------------------------------------------- */

NTSTATUS nls_table_parse(const uint8_t *blob, uint32_t len, nls_published_t *out)
{
    nls_table_header_t hdr;
    nls_published_t r;
    uint32_t dir_bytes, body_start, seen_mask, i;
    uint32_t off_crc, computed, zero = 0;

    if (!blob || !out)
        return STATUS_INVALID_PARAMETER;
    if (len < sizeof(nls_table_header_t))
        return STATUS_INVALID_PARAMETER;

    /* Copy the header out of the (possibly unaligned) blob. */
    memcpy(&hdr, blob, sizeof(hdr));

    if (hdr.magic != NLS_TABLE_V1_MAGIC)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (hdr.version != NLS_TABLE_V1_VERSION)
        return STATUS_INVALID_IMAGE_FORMAT;
    /* total_size must describe exactly the bytes handed to us and stay capped. */
    if (hdr.total_size != len || hdr.total_size > NLS_TABLE_V1_MAX_SIZE)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (hdr.chunk_count > NLS_TABLE_V1_MAX_CHUNKS)
        return STATUS_INVALID_IMAGE_FORMAT;

    /* Directory must fit wholly after the header (chunk_count <= 32 so the
     * multiply cannot overflow a uint32). */
    dir_bytes  = hdr.chunk_count * (uint32_t)sizeof(nls_chunk_desc_t);
    body_start = (uint32_t)sizeof(nls_table_header_t) + dir_bytes;
    if (body_start > len)
        return STATUS_INVALID_IMAGE_FORMAT;

    /* CRC32 over the whole blob with the crc32 field treated as zero. */
    off_crc  = (uint32_t)__builtin_offsetof(nls_table_header_t, crc32);
    computed = kcrc32(blob, off_crc);
    computed = kcrc32_cont(computed, &zero, sizeof(zero));
    computed = kcrc32_cont(computed, blob + off_crc + 4, len - off_crc - 4);
    if (computed != hdr.crc32)
        return STATUS_INVALID_IMAGE_FORMAT;

    memset(&r, 0, sizeof(r));
    r.blob        = blob;
    r.lcid        = hdr.lcid;
    r.code_page   = hdr.code_page;
    r.nls_version = hdr.nls_version;

    seen_mask = 0;
    for (i = 0; i < hdr.chunk_count; i++) {
        nls_chunk_desc_t cd;
        const uint16_t *p;
        uint32_t elems;

        memcpy(&cd, blob + sizeof(nls_table_header_t) + i * sizeof(cd), sizeof(cd));

        if (cd.type == 0 || cd.type > NLS_CHUNK_TYPE_MAX)
            return STATUS_INVALID_IMAGE_FORMAT;
        if (seen_mask & (1u << cd.type))    /* duplicate chunk type */
            return STATUS_INVALID_IMAGE_FORMAT;
        seen_mask |= (1u << cd.type);

        /* Body must lie after the directory and wholly inside the blob; the
         * size test is written subtraction-first so it cannot overflow. */
        if (cd.offset < body_start || cd.offset > len)
            return STATUS_INVALID_IMAGE_FORMAT;
        if (cd.size > len - cd.offset)
            return STATUS_INVALID_IMAGE_FORMAT;
        /* Every v1 chunk is a uint16-element table (consumed CTYPE/UPCASE now,
         * reserved FOLD_* for section 7). Require even offset + size for ALL
         * defined types -- not just the consumed ones -- so a reserved chunk
         * cannot publish an unaligned or half-element payload a later section
         * would inherit and trust. */
        if ((cd.offset & 1u) || (cd.size & 1u))
            return STATUS_INVALID_IMAGE_FORMAT;

        switch (cd.type) {
        case NLS_CHUNK_UPCASE:
        case NLS_CHUNK_CTYPE1:
        case NLS_CHUNK_CTYPE2:
        case NLS_CHUNK_CTYPE3:
            p     = (const uint16_t *)(blob + cd.offset);
            elems = cd.size / 2u;
            if (cd.type == NLS_CHUNK_UPCASE)      { r.upcase = p; r.upcase_count = elems; }
            else if (cd.type == NLS_CHUNK_CTYPE1) { r.ctype1 = p; r.ctype1_count = elems; }
            else if (cd.type == NLS_CHUNK_CTYPE2) { r.ctype2 = p; r.ctype2_count = elems; }
            else                                  { r.ctype3 = p; r.ctype3_count = elems; }
            break;
        default:
            /* FOLD_* are reserved for section 7: validated + carried, not
             * interpreted by v1. */
            break;
        }
    }

    *out = r;
    return STATUS_SUCCESS;
}

/* ---- Query accessors ----------------------------------------------------- */

uint16_t nls_upcase_char(uint16_t c)
{
    const nls_published_t *d;

    /* < U+0100 is the compiled invariant authority -- never disk-sourced. */
    if (c < 0x0100u)
        return rtl_upcase_char(c);

    d = __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
    if (d && d->upcase && c < d->upcase_count)
        return d->upcase[c];
    return c;   /* matches rtl_upcase_char's >= U+0100 pass-through */
}

uint16_t nls_char_type(uint16_t c, int which)
{
    const nls_published_t *d = __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
    const uint16_t *tbl = 0;
    uint32_t cnt = 0;

    if (d) {
        switch (which) {
        case 1: tbl = d->ctype1; cnt = d->ctype1_count; break;
        case 2: tbl = d->ctype2; cnt = d->ctype2_count; break;
        case 3: tbl = d->ctype3; cnt = d->ctype3_count; break;
        default: return 0;
        }
    } else if (which < 1 || which > 3) {
        return 0;
    }

    if (tbl && c < cnt)
        return tbl[c];
    if (which == 1)
        return nls_compiled_ctype1(c);   /* compiled ASCII + Latin-1 fallback */
    return 0;                            /* no CTYPE2/CTYPE3 fallback */
}

uint32_t nls_get_version(void)
{
    const nls_published_t *d = __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
    return d ? d->nls_version : NLS_TABLE_V1_VERSION;
}

#ifdef KERNEL_TESTS
void nls_test_set_active(const nls_published_t *desc)
{
    __atomic_store_n(&s_active, desc, __ATOMIC_RELEASE);
}
#endif

/* ---- Boot loader --------------------------------------------------------- */

static void nls_free_blob(uintptr_t phys, uint32_t pages)
{
    uint32_t p;
    for (p = 0; p < pages; p++)
        pmm_free_frame(phys + (uintptr_t)p * NLS_PAGE_SIZE);
}

void nls_init(void)
{
    static uint8_t s_init_ran = 0;
    struct vfs_node *f;
    nls_table_header_t hdr;
    uintptr_t phys;
    uint8_t *buf;
    uint32_t pages;
    int rc;
    NTSTATUS st;

    /* One-shot: never re-parse into s_loaded while accessors may already be
     * reading a published descriptor. The atomic test-and-set makes this safe
     * even if two CPUs ever raced the call (the boot path calls it once on the
     * BSP; this is belt-and-suspenders). A losing caller returns and the winner
     * publishes; readers see the compiled fallback until the release store. */
    if (__atomic_exchange_n(&s_init_ran, 1u, __ATOMIC_ACQ_REL) != 0)
        return;

    /* No C: volume -> serve compiled fallback. */
    if (!vfs_is_mounted('C')) {
        klog(LOG_WARN, "nls", "C: not mounted -- serving compiled invariant fallback");
        kernel_subsystem_apply_result(SUBSYS_NLS, BOOT_DEGRADED);
        return;
    }

    f = vfs_open(NLS_TABLE_PATH, VFS_O_READ);
    if (!f) {
        klog(LOG_WARN, "nls", "%s absent -- compiled invariant fallback", NLS_TABLE_PATH);
        kernel_subsystem_apply_result(SUBSYS_NLS, BOOT_DEGRADED);
        return;
    }

    /* Read the fixed header FIRST so a hostile total_size cannot size the
     * allocation before magic/version/cap are trusted. */
    rc = vfs_read(f, 0, (uint32_t)sizeof(hdr), (uint8_t *)&hdr);
    if (rc < (int)sizeof(hdr) ||
        hdr.magic != NLS_TABLE_V1_MAGIC || hdr.version != NLS_TABLE_V1_VERSION ||
        hdr.total_size < sizeof(hdr) || hdr.total_size > NLS_TABLE_V1_MAX_SIZE) {
        klog(LOG_WARN, "nls", "invalid NLS header -- compiled invariant fallback");
        vfs_close(f);
        kernel_subsystem_apply_result(SUBSYS_NLS, BOOT_DEGRADED);
        return;
    }
    /* Identity guard: invariant.nls must declare the invariant locale. A table
     * claiming a different LCID is rejected before it can publish a fold, cutting
     * the attack surface a disk-swapped table would otherwise offer. CRC + the
     * system-volume ACL are the trust model (matches Windows loading l_intl.nls
     * unsigned); full-BMP disk folds still feed only >= U+0100 -- the
     * security-critical < U+0100 range stays compiled (see nls.h). */
    if (hdr.lcid != NLS_INVARIANT_LCID) {
        klog(LOG_WARN, "nls", "NLS table lcid=0x%x != invariant -- compiled fallback",
             (uint64_t)hdr.lcid);
        vfs_close(f);
        kernel_subsystem_apply_result(SUBSYS_NLS, BOOT_DEGRADED);
        return;
    }

    pages = (hdr.total_size + NLS_PAGE_SIZE - 1) / NLS_PAGE_SIZE;
    phys  = pmm_alloc_contiguous(pages);
    if (!phys) {
        klog(LOG_ERROR, "nls", "alloc %u pages failed -- compiled invariant fallback", pages);
        vfs_close(f);
        kernel_subsystem_apply_result(SUBSYS_NLS, BOOT_DEGRADED);
        return;
    }
    buf = (uint8_t *)phys;

    rc = vfs_read(f, 0, hdr.total_size, buf);
    vfs_close(f);
    if (rc < (int)hdr.total_size) {
        klog(LOG_WARN, "nls", "short NLS read (%d/%u) -- compiled invariant fallback",
             rc, hdr.total_size);
        nls_free_blob(phys, pages);
        kernel_subsystem_apply_result(SUBSYS_NLS, BOOT_DEGRADED);
        return;
    }

    st = nls_table_parse(buf, hdr.total_size, &s_loaded);
    if (st != STATUS_SUCCESS) {
        klog(LOG_WARN, "nls", "NLS table validation failed (0x%x) -- compiled fallback",
             (uint64_t)st);
        nls_free_blob(phys, pages);
        kernel_subsystem_apply_result(SUBSYS_NLS, BOOT_DEGRADED);
        return;
    }

    /* Authoritative identity gate on the PUBLISHED blob (not the preliminary
     * header read): a device that returned different bytes between the two reads
     * cannot publish a non-invariant table through the TOCTOU window. */
    if (s_loaded.lcid != NLS_INVARIANT_LCID) {
        klog(LOG_WARN, "nls", "parsed NLS lcid=0x%x != invariant -- compiled fallback",
             (uint64_t)s_loaded.lcid);
        nls_free_blob(phys, pages);
        kernel_subsystem_apply_result(SUBSYS_NLS, BOOT_DEGRADED);
        return;
    }

    s_loaded.blob_pages = pages;
    __atomic_store_n(&s_active, &s_loaded, __ATOMIC_RELEASE);
    klog(LOG_INFO, "nls", "NLS table loaded: lcid=0x%x cp=%u ver=%u (%u bytes)",
         (uint64_t)s_loaded.lcid, (uint64_t)s_loaded.code_page,
         (uint64_t)s_loaded.nls_version, (uint64_t)hdr.total_size);
    kernel_subsystem_apply_result(SUBSYS_NLS, BOOT_OK);
}
