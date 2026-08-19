/* test_boot_headers.c -- host-side tests for the pure headers under include/boot/.
 *
 * WHY THESE LIVE ON THE HOST AND NOT IN THE KERNEL TEST SUITE. The headers in
 * include/boot/ are deliberately plain C over plain types so that both the UEFI
 * loader and a test can compile them unchanged; sha256_boot.h and
 * devpath_filepath.h are tested from src/kernel/test/test_uefi_boot.c for that
 * reason. That home has a hard ceiling: every test function's code lands in
 * kernel.exe, and the image's BSS end must stay below the user base at
 * 0x800000. Measured 2026-08-19 at commit 6e7ea8c9a: __kernel_end sits at
 * 0x7fe000, leaving 8192 bytes, and this suite alone is 8664 -- it does not
 * fit, and would not fit tomorrow either.
 *
 * A host binary costs the kernel image nothing, which is the whole point. It
 * also gives these particular tests something the kernel suite cannot: the
 * Authenticode expectation is computed by an INDEPENDENT implementation of the
 * spec's numbered steps (tools/boot-header-tests/authenticode_oracle.py), so
 * the digest asserted below is not this code agreeing with itself.
 *
 * Build + run: bash tools/boot-header-tests/run.sh
 */

#include <stdio.h>
#include <string.h>

#include "../../include/boot/pe_authenticode.h"
#include "../../include/boot/tcg_evlog.h"

static int g_failures;
static int g_checks;

static void check(int cond, const char *what)
{
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("  [FAIL] %s\n", what);
    }
}

static void check_eq(int got, int want, const char *what)
{
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("  [FAIL] %s (got %d, want %d)\n", what, got, want);
    }
}

/* ---------------------------------------------------------------------------
 * Authenticode PE image hash
 *
 * The fixture is a minimal but deliberately awkward PE32+: three sections of
 * which one has SizeOfRawData 0 (dropped at table-build time, spec step 9), the
 * other two laid out in the FILE in the opposite order to the section table (so
 * a missing sort changes the answer), 64 bytes of trailing data past the last
 * section, and a 32-byte attribute certificate table at the very end that must
 * NOT be hashed.
 * ------------------------------------------------------------------------- */
#define PE_FILE_SIZE 1632u

static void wr16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void wr32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* Byte-for-byte the construction authenticode_oracle.py uses. */
static void build_pe(unsigned char *b)
{
    unsigned int i;
    const unsigned int pe = 64u, coff = 68u, opt = 88u, sectab = 328u;

    for (i = 0; i < PE_FILE_SIZE; i++)
        b[i] = (unsigned char)((i * 7u + 3u) & 0xFFu);
    b[0] = 'M'; b[1] = 'Z';
    wr32(b + 0x3Cu, pe);
    b[pe] = 'P'; b[pe + 1u] = 'E'; b[pe + 2u] = 0u; b[pe + 3u] = 0u;
    wr16(b + coff + 0u, 0x8664u);      /* Machine */
    wr16(b + coff + 2u, 3u);           /* NumberOfSections */
    wr16(b + coff + 16u, 240u);        /* SizeOfOptionalHeader */
    wr16(b + opt, 0x20Bu);             /* PE32+ */
    wr32(b + opt + 60u, 512u);         /* SizeOfHeaders */
    wr32(b + opt + 64u, 0xDEADBEEFu);  /* CheckSum -- excluded */
    wr32(b + opt + 108u, 16u);         /* NumberOfRvaAndSizes */
    wr32(b + 232u, 1600u);             /* certificate table VA */
    wr32(b + 236u, 32u);               /* certificate table Size -- excluded */
    /* section 0 lies LATER in the file than section 1: the sort is load-bearing */
    wr32(b + sectab + 0u * 40u + 16u, 512u);
    wr32(b + sectab + 0u * 40u + 20u, 1024u);
    wr32(b + sectab + 1u * 40u + 16u, 512u);
    wr32(b + sectab + 1u * 40u + 20u, 512u);
    /* section 2 has SizeOfRawData 0 -- never enters the table at all (step 9) */
    wr32(b + sectab + 2u * 40u + 16u, 0u);
    wr32(b + sectab + 2u * 40u + 20u, 2048u);
}

/* Produced by tools/boot-header-tests/authenticode_oracle.py, which transcribes
 * the spec's numbered steps independently of peac_hash(). */
static const unsigned char expect_authenticode[SHA256B_DIGEST_LEN] = {
    0x2b,0x4c,0x89,0xcc,0xbc,0x32,0x81,0x66,0xef,0x3f,0x0f,0x95,0xa0,0x37,0xd4,0x08,
    0x1b,0x99,0xcc,0xf3,0x6e,0x6d,0x78,0x97,0xff,0x8c,0x11,0xd7,0x46,0x84,0x8c,0x2f
};

static void t_authenticode_matches_oracle(void)
{
    unsigned char img[PE_FILE_SIZE];
    unsigned char got[SHA256B_DIGEST_LEN], flat[SHA256B_DIGEST_LEN];

    build_pe(img);
    check_eq(peac_hash(img, PE_FILE_SIZE, got), PEAC_OK,
             "Authenticode hash of a well-formed PE succeeds");
    check(memcmp(got, expect_authenticode, SHA256B_DIGEST_LEN) == 0,
          "digest equals the independently computed value");

    /* Control: a flat hash of the same bytes must NOT match, or the exclusions
     * and the section sort are not doing anything at all. */
    sha256b(img, PE_FILE_SIZE, flat);
    check(memcmp(got, flat, SHA256B_DIGEST_LEN) != 0,
          "Authenticode digest differs from the flat file digest");
}

static void t_excluded_fields_do_not_move_digest(void)
{
    unsigned char a[PE_FILE_SIZE], b[PE_FILE_SIZE];
    unsigned char da[SHA256B_DIGEST_LEN], db[SHA256B_DIGEST_LEN];

    build_pe(a);
    build_pe(b);
    /* Exactly the bytes the spec excludes: the 4-byte CheckSum and the 8-byte
     * certificate directory ENTRY. A conforming hash cannot notice either. */
    wr32(b + 88u + 64u, 0x12345678u);
    wr32(b + 232u, 4242u);
    wr32(b + 236u, 32u);   /* the size itself is unchanged: it bounds the cut */

    check_eq(peac_hash(a, PE_FILE_SIZE, da), PEAC_OK, "baseline hashes");
    check_eq(peac_hash(b, PE_FILE_SIZE, db), PEAC_OK, "mutated hashes");
    check(memcmp(da, db, SHA256B_DIGEST_LEN) == 0,
          "checksum and certificate-entry bytes are excluded");
}

static void t_hashed_regions_do_move_digest(void)
{
    unsigned char a[PE_FILE_SIZE], b[PE_FILE_SIZE];
    unsigned char da[SHA256B_DIGEST_LEN], db[SHA256B_DIGEST_LEN];

    /* A byte inside a section. */
    build_pe(a); build_pe(b);
    b[1024u] ^= 0xFFu;
    check_eq(peac_hash(a, PE_FILE_SIZE, da), PEAC_OK, "baseline hashes");
    check_eq(peac_hash(b, PE_FILE_SIZE, db), PEAC_OK, "section-mutated hashes");
    check(memcmp(da, db, SHA256B_DIGEST_LEN) != 0,
          "a byte inside a section DOES change the digest");

    /* A byte in the trailing data between the last section and the cert table. */
    build_pe(b);
    b[1536u] ^= 0xFFu;
    check_eq(peac_hash(b, PE_FILE_SIZE, db), PEAC_OK, "trailing-mutated hashes");
    check(memcmp(da, db, SHA256B_DIGEST_LEN) != 0,
          "trailing data past the last section IS hashed");

    /* The attribute certificate table itself, which must not be hashed. */
    build_pe(b);
    {
        unsigned int i;
        for (i = 0; i < 32u; i++)
            b[1600u + i] ^= 0xFFu;
    }
    check_eq(peac_hash(b, PE_FILE_SIZE, db), PEAC_OK, "cert-mutated hashes");
    check(memcmp(da, db, SHA256B_DIGEST_LEN) == 0,
          "the attribute certificate table is NOT hashed");
}

static void t_refuses_malformed_images(void)
{
    unsigned char img[PE_FILE_SIZE];
    unsigned char d[SHA256B_DIGEST_LEN];

    build_pe(img); img[0] = 'X';
    check_eq(peac_hash(img, PE_FILE_SIZE, d), PEAC_BAD_DOS,
             "missing MZ signature refused");

    build_pe(img); wr32(img + 0x3Cu, PE_FILE_SIZE);
    check_eq(peac_hash(img, PE_FILE_SIZE, d), PEAC_BAD_DOS,
             "e_lfanew past end of file refused");

    build_pe(img); img[65] = 'X';
    check_eq(peac_hash(img, PE_FILE_SIZE, d), PEAC_BAD_PE_SIG,
             "missing PE signature refused");

    build_pe(img); wr16(img + 88u, 0x0107u);
    check_eq(peac_hash(img, PE_FILE_SIZE, d), PEAC_BAD_OPTIONAL,
             "unknown optional-header magic refused");

    build_pe(img); wr16(img + 68u + 16u, 64u);
    check_eq(peac_hash(img, PE_FILE_SIZE, d), PEAC_BAD_OPTIONAL,
             "optional header too short for the certificate entry refused");

    build_pe(img); wr32(img + 88u + 60u, PE_FILE_SIZE + 1u);
    check_eq(peac_hash(img, PE_FILE_SIZE, d), PEAC_BAD_HEADERS,
             "SizeOfHeaders past end of file refused");

    /* Two sections claiming the same bytes would be hashed twice, producing a
     * digest no conforming firmware can reproduce -- which would read as
     * DISAGREE, a manufactured mismatch. */
    build_pe(img); wr32(img + 328u + 1u * 40u + 20u, 768u);
    check_eq(peac_hash(img, PE_FILE_SIZE, d), PEAC_SECTION_OVERLAP,
             "overlapping sections refused");

    build_pe(img); wr32(img + 328u + 0u * 40u + 20u, PE_FILE_SIZE - 16u);
    check_eq(peac_hash(img, PE_FILE_SIZE, d), PEAC_BAD_SECTION,
             "section raw extent past end of file refused");

    build_pe(img); wr32(img + 236u, PE_FILE_SIZE);
    check_eq(peac_hash(img, PE_FILE_SIZE, d), PEAC_BAD_CERT,
             "certificate table larger than the bytes behind it refused");
}

/* ---------------------------------------------------------------------------
 * TCG event-log walk
 *
 * The SpecID checks below are exactly what src/kernel/tpm.c does NOT do, so
 * these are the tests that would fail if this walker were swapped for the
 * permissive one.
 * ------------------------------------------------------------------------- */
#define LOG_CAP 1024u

static unsigned int log_specid(unsigned char *b, unsigned int nalg,
                               const unsigned int *algs, const unsigned int *sizes,
                               int good_signature)
{
    static const char sig[16] = "Spec ID Event03";
    /* +1 for the vendorInfoSize byte (zero vendor bytes). The parser now
     * requires the payload to account for itself exactly. */
    unsigned int i, ev_size = 28u + nalg * 4u + 1u;

    wr32(b + 0u, 0u);                      /* PCRIndex */
    wr32(b + 4u, TCGL_EV_NO_ACTION);       /* EventType */
    for (i = 8u; i < 28u; i++)
        b[i] = 0u;                         /* legacy SHA-1 digest field */
    wr32(b + 28u, ev_size);                /* EventSize */
    for (i = 0; i < 16u; i++)
        b[32u + i] = (unsigned char)sig[i];
    if (!good_signature)
        b[32u] = 'X';
    wr32(b + 32u + 24u, nalg);             /* numberOfAlgorithms */
    for (i = 0; i < nalg; i++) {
        wr16(b + 32u + 28u + i * 4u, algs[i]);
        wr16(b + 32u + 28u + i * 4u + 2u, sizes[i]);
    }
    b[32u + 28u + nalg * 4u] = 0u;    /* vendorInfoSize */
    return 32u + ev_size;
}

/* Append one EVENT2 image-load record carrying `nalg` digests. Pass the same
 * bank set the log advertised for a conforming record; pass a subset to build
 * the incomplete-digest-list case. */
static unsigned int log_image_load_banks(unsigned char *b, unsigned int off,
                                         unsigned int pcr,
                                         const unsigned int *algs,
                                         const unsigned int *sizes,
                                         unsigned int nalg,
                                         unsigned char dp_tag, unsigned char fill)
{
    unsigned int i, j, p = off;
    const unsigned int dp_len = 8u, esize = 32u + 8u;

    wr32(b + p, pcr); p += 4u;
    wr32(b + p, TCGL_EV_EFI_BOOT_SERVICES_APPLICATION); p += 4u;
    wr32(b + p, nalg); p += 4u;
    for (j = 0; j < nalg; j++) {
        wr16(b + p, algs[j]); p += 2u;
        for (i = 0; i < sizes[j]; i++) b[p + i] = fill;
        p += sizes[j];
    }
    wr32(b + p, esize); p += 4u;
    for (i = 0; i < 32u; i++) b[p + i] = 0u;
    wr32(b + p, 0x1000u);            /* ImageLocationInMemory, low half */
    wr32(b + p + 8u, 0x2000u);       /* ImageLengthInMemory */
    wr32(b + p + 24u, dp_len);       /* LengthOfDevicePath */
    p += 32u;
    for (i = 0; i < dp_len; i++)
        b[p + i] = (unsigned char)(dp_tag + i);
    return p + dp_len;
}

static unsigned int log_image_load(unsigned char *b, unsigned int off,
                                   unsigned int pcr, unsigned int with_sha256,
                                   unsigned char dp_tag, unsigned char fill)
{
    const unsigned int sha256_alg[1] = { TCGL_ALG_SHA256 };
    const unsigned int sha256_sz[1]  = { 32u };
    const unsigned int sha1_alg[1]   = { TCGL_ALG_SHA1 };
    const unsigned int sha1_sz[1]    = { 20u };

    if (with_sha256)
        return log_image_load_banks(b, off, pcr, sha256_alg, sha256_sz, 1u,
                                    dp_tag, fill);
    return log_image_load_banks(b, off, pcr, sha1_alg, sha1_sz, 1u, dp_tag, fill);
}

static void mk_dp(unsigned char *dp, unsigned char tag)
{
    unsigned int i;
    for (i = 0; i < 8u; i++)
        dp[i] = (unsigned char)(tag + i);
}

static void t_open_accepts_and_records_banks(void)
{
    unsigned char b[LOG_CAP];
    const unsigned int algs[2]  = { TCGL_ALG_SHA1, TCGL_ALG_SHA256 };
    const unsigned int sizes[2] = { 20u, 32u };
    struct tcgl_log lg;
    unsigned int end, sz = 0;

    end = log_specid(b, 2u, algs, sizes, 1);
    check_eq(tcgl_open(b, end, &lg), TCGL_OK, "well-formed SpecID accepted");
    check_eq((int)lg.nbanks, 2, "both advertised banks recorded");
    check(tcgl_bank_size(&lg, TCGL_ALG_SHA256, &sz), "SHA-256 bank present");
    check_eq((int)sz, 32, "SHA-256 digest size taken from the log itself");
}

static void t_open_refuses_unvalidated_logs(void)
{
    unsigned char b[LOG_CAP];
    const unsigned int algs[1]  = { TCGL_ALG_SHA256 };
    const unsigned int sizes[1] = { 32u };
    const unsigned int bad_sizes[1] = { 20u };
    struct tcgl_log lg;
    unsigned int end;

    check_eq(tcgl_open(NULL, 0u, &lg), TCGL_ABSENT,
             "an absent log is ABSENT, not malformed");

    /* The signature check the kernel walker never performs. Without it a
     * TPM 1.2 log is walked as though every later record were an EVENT2. */
    end = log_specid(b, 1u, algs, sizes, 0);
    check_eq(tcgl_open(b, end, &lg), TCGL_NOT_SPECID,
             "wrong SpecID signature refused");

    end = log_specid(b, 1u, algs, sizes, 1);
    wr32(b + 4u, 0x80000001u);
    check_eq(tcgl_open(b, end, &lg), TCGL_NOT_SPECID,
             "first record that is not EV_NO_ACTION refused");

    end = log_specid(b, 1u, algs, bad_sizes, 1);
    check_eq(tcgl_open(b, end, &lg), TCGL_BAD_SPECID,
             "SHA-256 advertised with a 20-byte digest refused");

    end = log_specid(b, 1u, algs, sizes, 1);
    check_eq(tcgl_open(b, end - 1u, &lg), TCGL_TRUNCATED,
             "SpecID payload running past the buffer refused");

    /* The payload must account for itself EXACTLY. Declaring fewer algorithms
     * than the bytes present leaves reader and writer disagreeing about where
     * the table ends while every bounds check still passes. */
    end = log_specid(b, 1u, algs, sizes, 1);
    wr32(b + 28u, 28u + 1u * 4u);          /* EventSize omitting the vendor byte */
    check_eq(tcgl_open(b, end, &lg), TCGL_BAD_SPECID,
             "SpecID payload that does not account for its vendor tail refused");

    end = log_specid(b, 1u, algs, sizes, 1);
    b[32u + 28u + 4u] = 7u;                /* vendorInfoSize the size cannot hold */
    check_eq(tcgl_open(b, end, &lg), TCGL_BAD_SPECID,
             "SpecID vendorInfoSize inconsistent with EventSize refused");

    /* The SpecID record's own header is part of the contract: PCR index 0 and
     * an all-zero legacy digest. Skipping either let a nonconforming log reach
     * AGREE through an otherwise well-formed record. */
    end = log_specid(b, 1u, algs, sizes, 1);
    wr32(b + 0u, 1u);
    check_eq(tcgl_open(b, end, &lg), TCGL_BAD_SPECID_HDR,
             "SpecID record at a PCR index other than 0 refused");

    end = log_specid(b, 1u, algs, sizes, 1);
    b[8u] = 0x01u;
    check_eq(tcgl_open(b, end, &lg), TCGL_BAD_SPECID_HDR,
             "SpecID record with a non-zero legacy digest refused");

    /* A duplicate advertised bank makes "the SHA-256 bank" ambiguous. */
    {
        const unsigned int dup[2]   = { TCGL_ALG_SHA256, TCGL_ALG_SHA256 };
        const unsigned int dupsz[2] = { 32u, 32u };
        end = log_specid(b, 2u, dup, dupsz, 1);
        check_eq(tcgl_open(b, end, &lg), TCGL_DUPLICATE_ALG,
                 "duplicate advertised algorithm refused");
    }
}

static void t_finds_own_image_load_event(void)
{
    unsigned char b[LOG_CAP], want[8];
    const unsigned int algs[1]  = { TCGL_ALG_SHA256 };
    const unsigned int sizes[1] = { 32u };
    struct tcgl_log lg;
    struct tcgl_match m;
    unsigned int end, cand = 0, i;
    int allfill = 1;

    end = log_specid(b, 1u, algs, sizes, 1);
    end = log_image_load(b, end, 4u, 1u, 0x40u, 0xAAu);   /* a different app */
    end = log_image_load(b, end, 4u, 1u, 0x10u, 0xBBu);   /* ours */
    check_eq(tcgl_open(b, end, &lg), TCGL_OK, "log opens");

    mk_dp(want, 0x10u);
    check_eq(tcgl_find_image_load(&lg, 4u, want, 8u, &m, &cand), TCGL_OK,
             "our own image-load event found among several");
    check_eq((int)cand, 1, "exactly one candidate matched");
    for (i = 0; i < 32u; i++)
        if (m.sha256[i] != 0xBBu) allfill = 0;
    check(allfill, "the digest returned is OUR entry's, not the first one's");
}

static void t_refuses_ambiguous_and_absent(void)
{
    unsigned char b[LOG_CAP], want[8];
    const unsigned int algs[1]  = { TCGL_ALG_SHA256 };
    const unsigned int sizes[1] = { 32u };
    struct tcgl_log lg;
    struct tcgl_match m;
    unsigned int end, cand = 0;

    /* Two entries with the SAME device path: reporting the first would be a
     * confident wrong answer. */
    end = log_specid(b, 1u, algs, sizes, 1);
    end = log_image_load(b, end, 4u, 1u, 0x10u, 0xAAu);
    end = log_image_load(b, end, 4u, 1u, 0x10u, 0xBBu);
    check_eq(tcgl_open(b, end, &lg), TCGL_OK, "log opens");

    mk_dp(want, 0x10u);
    check_eq(tcgl_find_image_load(&lg, 4u, want, 8u, &m, &cand), TCGL_AMBIGUOUS,
             "two matching entries refuse rather than guess");
    check_eq((int)cand, 2, "both candidates counted for the report");

    mk_dp(want, 0x77u);
    check_eq(tcgl_find_image_load(&lg, 4u, want, 8u, &m, &cand), TCGL_NO_MATCH,
             "an unmatched device path reports absence");

    mk_dp(want, 0x10u);
    check_eq(tcgl_find_image_load(&lg, 7u, want, 8u, &m, &cand), TCGL_NO_MATCH,
             "a match in another PCR does not answer for PCR 4");
}

static void t_refuses_unusable_records(void)
{
    unsigned char b[LOG_CAP], want[8];
    const unsigned int algs[1]  = { TCGL_ALG_SHA256 };
    const unsigned int sizes[1] = { 32u };
    const unsigned int algs2[2]  = { TCGL_ALG_SHA1, TCGL_ALG_SHA256 };
    const unsigned int sizes2[2] = { 20u, 32u };
    struct tcgl_log lg;
    struct tcgl_match m;
    unsigned int end, cand = 0;

    mk_dp(want, 0x10u);

    /* A record using a bank this log never advertised: the stride is undefined
     * from here on, so the walk refuses instead of guessing. */
    end = log_specid(b, 1u, algs, sizes, 1);
    end = log_image_load(b, end, 4u, 0u, 0x10u, 0xCCu);
    check_eq(tcgl_open(b, end, &lg), TCGL_OK, "log opens");
    check_eq(tcgl_find_image_load(&lg, 4u, want, 8u, &m, &cand), TCGL_UNKNOWN_ALG,
             "a record using an undeclared bank is refused");

    /* Both banks advertised, but OUR record carries only SHA-1. A digest list
     * is not a free-form subset: accepting one would let a record present only
     * the bank we compare against, which is how a partial log becomes AGREE. */
    end = log_specid(b, 2u, algs2, sizes2, 1);
    end = log_image_load(b, end, 4u, 0u, 0x10u, 0xEEu);
    check_eq(tcgl_open(b, end, &lg), TCGL_OK, "log opens");
    check_eq(tcgl_find_image_load(&lg, 4u, want, 8u, &m, &cand),
             TCGL_INCOMPLETE_DIGESTS,
             "a record omitting an advertised bank is refused");

    /* A log that advertises only SHA-1: the record is complete and identified,
     * there is simply no SHA-256 digest to compare. Named, not a mismatch. */
    {
        const unsigned int only1[1]  = { TCGL_ALG_SHA1 };
        const unsigned int only1sz[1] = { 20u };
        end = log_specid(b, 1u, only1, only1sz, 1);
        end = log_image_load_banks(b, end, 4u, only1, only1sz, 1u, 0x10u, 0xEEu);
        check_eq(tcgl_open(b, end, &lg), TCGL_OK, "SHA-1-only log opens");
        check_eq(tcgl_find_image_load(&lg, 4u, want, 8u, &m, &cand),
                 TCGL_NO_SHA256_BANK,
                 "an identified entry with no SHA-256 bank is named, not a mismatch");
        check_eq((int)cand, 1, "the entry was still identified");
    }

    /* A record listing the same advertised bank twice satisfies a naive
     * completeness count with one digest. */
    {
        const unsigned int dup[2]   = { TCGL_ALG_SHA256, TCGL_ALG_SHA256 };
        const unsigned int dupsz[2] = { 32u, 32u };
        end = log_specid(b, 1u, algs, sizes, 1);
        end = log_image_load_banks(b, end, 4u, dup, dupsz, 2u, 0x10u, 0xEEu);
        check_eq(tcgl_open(b, end, &lg), TCGL_OK, "log opens");
        check_eq(tcgl_find_image_load(&lg, 4u, want, 8u, &m, &cand),
                 TCGL_DUPLICATE_ALG,
                 "a record listing one bank twice is refused");
    }

    /* A record whose event data runs past the buffer. */
    end = log_specid(b, 1u, algs, sizes, 1);
    end = log_image_load(b, end, 4u, 1u, 0x10u, 0xDDu);
    check_eq(tcgl_open(b, end, &lg), TCGL_OK, "log opens");
    lg.size = end - 4u;
    check_eq(tcgl_find_image_load(&lg, 4u, want, 8u, &m, &cand), TCGL_TRUNCATED,
             "a record running past the buffer is refused");
}

int main(void)
{
    printf("boot-header-tests: pe_authenticode.h + tcg_evlog.h\n");

    t_authenticode_matches_oracle();
    t_excluded_fields_do_not_move_digest();
    t_hashed_regions_do_move_digest();
    t_refuses_malformed_images();
    t_open_accepts_and_records_banks();
    t_open_refuses_unvalidated_logs();
    t_finds_own_image_load_event();
    t_refuses_ambiguous_and_absent();
    t_refuses_unusable_records();

    printf("boot-header-tests: %d/%d checks passed, %d failed\n",
           g_checks - g_failures, g_checks, g_failures);
    return g_failures ? 1 : 0;
}
