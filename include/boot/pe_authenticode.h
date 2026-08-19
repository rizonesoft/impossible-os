/* pe_authenticode.h -- the Authenticode PE image hash, over a caller-owned
 *                      immutable snapshot of the file bytes.
 *
 * WHY THIS EXISTS. Firmware measures a boot application into PCR 4 by hashing
 * the PE image the Authenticode way, not by hashing the file flat. A flat
 * SHA-256 can therefore NEVER equal the firmware value, so a loader that wants
 * to correlate its own on-disk bytes with what firmware executed has to
 * reproduce this exact algorithm first. The exclusion ranges are the whole
 * difficulty and they are what this header owns.
 *
 * THE ALGORITHM IS TRANSCRIBED, NOT INVENTED. Every step below is numbered
 * against "Windows Authenticode Portable Executable Signature Format" v1.0
 * (2008-03-21), section "Calculating the PE Image Hash", steps 1-15. EDK2's
 * SecurityPkg/Library/DxeImageVerificationLib HashPeImage() implements the same
 * steps and is used here as an ORACLE (BSD-2-Clause-Patent, permissive) rather
 * than vendored, per the repo's reference-implementation rule.
 *
 * WHY A PURE HEADER. Same reason as devpath_filepath.h beside it: the bytes are
 * hostile by assumption, the caller owns and measures the buffer, and
 * everything here is arithmetic over `img[0 .. img_size)`. That is also what
 * makes it testable from the kernel test suite (test_uefi_boot.c), where a
 * malformed section table can actually be constructed -- inside the loader it
 * cannot, because firmware only ever hands over a real image.
 *
 * WHY IT REFUSES INSTEAD OF COPING. The consumer is a measurement whose whole
 * value is attribution. A digest computed over a layout this code did not fully
 * understand would be published with exactly the confidence of a correct one,
 * and would then be compared against a firmware value it can only accidentally
 * match. So an overlapping section, a header that does not fit the file, an
 * arithmetic overflow and a certificate table larger than the bytes behind it
 * are all distinct refusals, never recoveries.
 *
 * Type discipline: plain C types rather than UEFI UINTN or kernel stdint,
 * following include/boot/devpath_filepath.h -- the bootloader uses UEFI types
 * and the kernel test suite uses C99 stdint, and this file must compile as-is
 * under both.
 */

#ifndef PE_AUTHENTICODE_H
#define PE_AUTHENTICODE_H

#include "sha256_boot.h"

_Static_assert(sizeof(unsigned int) == 4,
               "pe_authenticode.h needs a 32-bit unsigned int");
_Static_assert(sizeof(unsigned long long) == 8,
               "pe_authenticode.h needs a 64-bit unsigned long long");

#define PEAC_FN static __attribute__((unused))

/* Field offsets, counted from the start of the OPTIONAL header. SizeOfHeaders
 * and CheckSum land at the SAME place in PE32 and PE32+: the two layouts
 * diverge at ImageBase (4 vs 8 bytes) but re-converge because PE32 also carries
 * a BaseOfData that PE32+ drops.
 *
 * These are NOT pinned by a _Static_assert here, and saying so matters: this
 * header parses a byte buffer rather than declaring a struct, so there is no
 * layout for an assert to check. include/kernel/pe.h DOES assert the same two
 * offsets (0x3C, 0x40) on its own PE32+ struct, which is corroboration and not
 * a constraint on this file -- it is a kernel translation unit this header
 * cannot include. What actually holds these honest is tools/boot-header-tests,
 * which hashes the REAL staged BOOTX64.EFI and compares against an independent
 * transcription of the spec: a wrong offset there fails immediately, on every
 * smoke leg. */
#define PEAC_OPT_SIZEOFHEADERS      60u
#define PEAC_OPT_CHECKSUM           64u
#define PEAC_OPT_MAGIC_PE32     0x010Bu
#define PEAC_OPT_MAGIC_PE32PLUS 0x020Bu

/* Where the data-directory array starts, and therefore where the certificate
 * table entry sits. The spec states the resulting absolute offset of the
 * certificate SIZE field directly -- "32 bit: offset 132, 64 bit: offset 148" --
 * which is exactly 96 + 4*8 + 4 and 112 + 4*8 + 4, so these constants reproduce
 * the spec's own numbers rather than competing with them. */
#define PEAC_OPT_DATADIR_PE32       96u
#define PEAC_OPT_DATADIR_PE32PLUS  112u
#define PEAC_DIR_SECURITY            4u   /* IMAGE_DIRECTORY_ENTRY_SECURITY */
#define PEAC_DIR_ENTRY_SIZE          8u   /* {VirtualAddress, Size}, 4+4 */

#define PEAC_SECTION_HDR_SIZE       40u
#define PEAC_SECTION_SIZERAW_OFF    16u   /* SizeOfRawData */
#define PEAC_SECTION_PTRRAW_OFF     20u   /* PointerToRawData, spec step 10 */

/* An image with more sections than this is refused rather than hashed. The cap
 * is not a layout fact, it is a bound on the insertion sort below: the caller
 * is a boot loader with a fixed stack and the input is firmware-adjacent, so an
 * unbounded section count is an unbounded loop over hostile data. Real EFI
 * applications sit far below it. */
#define PEAC_MAX_SECTIONS           96u

enum peac_status {
    PEAC_OK = 0,
    PEAC_BAD_DOS,          /* no MZ, or e_lfanew outside the file */
    PEAC_BAD_PE_SIG,       /* no "PE\0\0" at e_lfanew */
    PEAC_BAD_COFF,         /* COFF header does not fit the file */
    PEAC_BAD_OPTIONAL,     /* optional header absent, short, or unknown magic */
    PEAC_BAD_HEADERS,      /* SizeOfHeaders outside the file, or below the cuts */
    PEAC_TOO_MANY_SECTIONS,
    PEAC_BAD_SECTION,      /* a section's raw extent leaves the file */
    PEAC_SECTION_OVERLAP,  /* two sections claim the same file bytes */
    PEAC_BAD_CERT,         /* certificate table larger than the bytes behind it */
    PEAC_OVERFLOW          /* the running byte count would wrap */
};

PEAC_FN const char *peac_status_name(int st)
{
    switch (st) {
    case PEAC_OK:                return "ok";
    case PEAC_BAD_DOS:           return "bad-dos";
    case PEAC_BAD_PE_SIG:        return "bad-pe-sig";
    case PEAC_BAD_COFF:          return "bad-coff";
    case PEAC_BAD_OPTIONAL:      return "bad-optional";
    case PEAC_BAD_HEADERS:       return "bad-headers";
    case PEAC_TOO_MANY_SECTIONS: return "too-many-sections";
    case PEAC_BAD_SECTION:       return "bad-section";
    case PEAC_SECTION_OVERLAP:   return "section-overlap";
    case PEAC_BAD_CERT:          return "bad-cert";
    case PEAC_OVERFLOW:          return "overflow";
    default:                     return "unknown";
    }
}

PEAC_FN unsigned int peac_rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

PEAC_FN unsigned int peac_rd32(const unsigned char *p)
{
    return (unsigned int)p[0]         | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* One section's file extent, kept apart from the raw header so the sort below
 * moves 8 bytes rather than 40. */
struct peac_extent {
    unsigned int ptr;   /* PointerToRawData */
    unsigned int size;  /* SizeOfRawData */
};

/* Compute the Authenticode PE image hash of `img[0 .. img_size)`.
 *
 * The caller owns the buffer and must have read it ONCE: deriving this digest
 * and any other digest of the same image from two separate reads would let the
 * two describe different snapshots of a file an attacker is free to rewrite
 * between them, which is the very substitution the measurement exists to catch.
 *
 * On PEAC_OK, `out` holds the 32-byte digest. On any other status `out` is
 * untouched, so there is no partial answer to misread.
 */
PEAC_FN int peac_hash(const unsigned char *img, unsigned long long img_size,
                      unsigned char out[SHA256B_DIGEST_LEN])
{
    struct sha256b_ctx ctx;
    struct peac_extent ext[PEAC_MAX_SECTIONS];
    unsigned int nsec_kept = 0;
    unsigned int i;

    if (!img || img_size < 64u)
        return PEAC_BAD_DOS;

    /* Step 1: the image header. e_lfanew lives at 0x3C of the DOS header.
     * Containment is subtraction-form throughout -- `off + n > img_size` would
     * wrap on a hostile offset and let the check pass. */
    if (img[0] != 'M' || img[1] != 'Z')
        return PEAC_BAD_DOS;
    unsigned int pe_off = peac_rd32(img + 0x3Cu);
    if ((unsigned long long)pe_off > img_size || (img_size - pe_off) < 24u)
        return PEAC_BAD_DOS;

    if (img[pe_off] != 'P' || img[pe_off + 1u] != 'E'
        || img[pe_off + 2u] != 0u || img[pe_off + 3u] != 0u)
        return PEAC_BAD_PE_SIG;

    /* COFF file header: 20 bytes at pe_off+4, NumberOfSections at +2 and
     * SizeOfOptionalHeader at +16. */
    /* 64-bit from here down. Every one of these is derived from a 32-bit field
     * the file itself declares, so on an input larger than 4 GiB a crafted
     * e_lfanew could wrap the adds below and produce a coherent-looking parse
     * of the wrong bytes. The only in-tree caller caps its input at 2 MiB, so
     * this is unreachable today -- but the header is documented as reusable and
     * unit-tested standalone, and an arithmetic bound that depends on who calls
     * you is not a bound. */
    unsigned long long coff_off = (unsigned long long)pe_off + 4u;
    if ((img_size - coff_off) < 20u)
        return PEAC_BAD_COFF;
    unsigned int nsec     = peac_rd16(img + coff_off + 2u);
    unsigned int opt_size = peac_rd16(img + coff_off + 16u);
    unsigned long long opt_off = coff_off + 20u;   /* <= img_size, checked above */

    if (opt_size == 0u || (unsigned long long)opt_off > img_size
        || (img_size - opt_off) < (unsigned long long)opt_size)
        return PEAC_BAD_OPTIONAL;
    if (opt_size < 2u)
        return PEAC_BAD_OPTIONAL;

    unsigned int magic = peac_rd16(img + opt_off);
    unsigned int datadir_off;
    if (magic == PEAC_OPT_MAGIC_PE32)
        datadir_off = PEAC_OPT_DATADIR_PE32;
    else if (magic == PEAC_OPT_MAGIC_PE32PLUS)
        datadir_off = PEAC_OPT_DATADIR_PE32PLUS;
    else
        return PEAC_BAD_OPTIONAL;

    /* The certificate table ENTRY must lie inside the optional header the COFF
     * header declared. A truncated optional header that stops before the
     * directory array is not an image whose exclusions we can honour. This one
     * test also bounds every optional-header read below, since it forces
     * opt_size past 132 (PE32) or 148 (PE32+). */
    unsigned int secdir_rel = datadir_off
                            + PEAC_DIR_SECURITY * PEAC_DIR_ENTRY_SIZE;
    if (opt_size < secdir_rel + PEAC_DIR_ENTRY_SIZE)
        return PEAC_BAD_OPTIONAL;

    unsigned long long checksum_off = opt_off + PEAC_OPT_CHECKSUM;
    unsigned long long secdir_off   = opt_off + secdir_rel;
    unsigned int size_of_headers = peac_rd32(img + opt_off + PEAC_OPT_SIZEOFHEADERS);

    /* NumberOfRvaAndSizes sits 4 bytes before the array in both layouts. The
     * certificate SIZE is only meaningful when the array actually reaches index
     * 4, and EDK2 makes exactly this test before reading it, taking zero
     * otherwise. The header CUTS below are taken regardless, which is also what
     * EDK2 does: the bytes are there to skip whether or not the directory count
     * claims them. */
    unsigned int nrva = peac_rd32(img + opt_off + datadir_off - 4u);
    unsigned int cert_size = 0u;
    if (nrva > PEAC_DIR_SECURITY)
        cert_size = peac_rd32(img + secdir_off + 4u);

    /* Step 7 hashes "to the end of image header", which is SizeOfHeaders. Both
     * cuts must land inside it, in order, or the ranges below run backwards and
     * the subtractions underflow. */
    if ((unsigned long long)size_of_headers > img_size)
        return PEAC_BAD_HEADERS;
    if (secdir_off < checksum_off + 4u
        || (unsigned long long)size_of_headers < secdir_off + PEAC_DIR_ENTRY_SIZE)
        return PEAC_BAD_HEADERS;

    /* Steps 9-10: the section table, minus every zero-length section, sorted by
     * PointerToRawData ascending. The spec drops the zero ones at TABLE BUILD
     * time (step 9), not at hash time, and that distinction matters: a dropped
     * section also contributes nothing to SUM_OF_BYTES_HASHED, so it cannot
     * shift where the trailing-data range begins. */
    if (nsec > PEAC_MAX_SECTIONS)
        return PEAC_TOO_MANY_SECTIONS;
    unsigned long long sectab_off = opt_off + opt_size;   /* <= img_size, checked */
    /* Bound the whole table before walking it, so `hdr` below cannot wrap. */
    if ((img_size - sectab_off)
            < (unsigned long long)nsec * PEAC_SECTION_HDR_SIZE)
        return PEAC_BAD_SECTION;
    for (i = 0; i < nsec; i++) {
        unsigned long long hdr = sectab_off + (unsigned long long)i * PEAC_SECTION_HDR_SIZE;
        unsigned int sraw = peac_rd32(img + hdr + PEAC_SECTION_SIZERAW_OFF);
        unsigned int praw = peac_rd32(img + hdr + PEAC_SECTION_PTRRAW_OFF);
        if (sraw == 0u)
            continue;                        /* step 9 */
        if ((unsigned long long)praw > img_size
            || (img_size - praw) < (unsigned long long)sraw)
            return PEAC_BAD_SECTION;
        ext[nsec_kept].ptr  = praw;
        ext[nsec_kept].size = sraw;
        nsec_kept++;
    }

    /* Insertion sort: bounded by PEAC_MAX_SECTIONS, stable, no allocation. */
    for (i = 1u; i < nsec_kept; i++) {
        struct peac_extent key = ext[i];
        unsigned int j = i;
        while (j > 0u && ext[j - 1u].ptr > key.ptr) {
            ext[j] = ext[j - 1u];
            j--;
        }
        ext[j] = key;
    }

    /* Adjacent-pair overlap is sufficient once sorted, and it is a refusal
     * rather than a de-duplication: two sections claiming the same bytes would
     * hash those bytes twice and produce a digest no conforming firmware can
     * reproduce, which would then read as DISAGREE -- a manufactured mismatch,
     * the one outcome worse than an honest refusal. */
    for (i = 1u; i < nsec_kept; i++) {
        unsigned long long prev_end =
            (unsigned long long)ext[i - 1u].ptr + (unsigned long long)ext[i - 1u].size;
        if (prev_end > (unsigned long long)ext[i].ptr)
            return PEAC_SECTION_OVERLAP;
    }

    /* Steps 2-7: the header, with the checksum and the certificate table entry
     * cut out of it. */
    sha256b_init(&ctx);
    sha256b_update(&ctx, img, checksum_off);
    sha256b_update(&ctx, img + checksum_off + 4u,
                   secdir_off - (checksum_off + 4u));
    sha256b_update(&ctx, img + secdir_off + PEAC_DIR_ENTRY_SIZE,
                   (unsigned long long)size_of_headers
                       - (secdir_off + PEAC_DIR_ENTRY_SIZE));

    /* Steps 8, 11-13: SUM_OF_BYTES_HASHED starts at SizeOfHeaders and grows by
     * each kept section's SizeOfRawData. */
    unsigned long long sum = (unsigned long long)size_of_headers;
    for (i = 0; i < nsec_kept; i++) {
        sha256b_update(&ctx, img + ext[i].ptr, (unsigned long long)ext[i].size);
        if (sum > ~(unsigned long long)0 - (unsigned long long)ext[i].size)
            return PEAC_OVERFLOW;
        sum += (unsigned long long)ext[i].size;
    }

    /* Step 14: whatever is left past SUM_OF_BYTES_HASHED is hashed too, EXCEPT
     * the attribute certificate table, which always sits at the very end. The
     * spec gives the length as (File Size) - ((Size of AttributeCertificateTable)
     * + SUM_OF_BYTES_HASHED). A file too small to hold both is malformed rather
     * than a zero-length remainder: reading it as zero would quietly hash a
     * DIFFERENT range than firmware did and still publish a digest. */
    if (img_size > sum) {
        unsigned long long cert = (unsigned long long)cert_size;
        if (cert > img_size - sum)
            return PEAC_BAD_CERT;
        unsigned long long extra = img_size - sum - cert;
        if (extra > 0u)
            sha256b_update(&ctx, img + sum, extra);
    } else if (cert_size != 0u) {
        /* A certificate table declared past the end of the file. */
        return PEAC_BAD_CERT;
    }

    sha256b_final(&ctx, out);   /* step 15 */
    return PEAC_OK;
}

#endif /* PE_AUTHENTICODE_H */
