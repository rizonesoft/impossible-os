/* ============================================================================
 * test_tpm_baseline.c -- measured-boot baseline pure-core tests
 *
 * Format / validate / compare / generation logic (MMIO-free, no live TPM). The
 * live snapshot / enroll / verify wrappers are validated against QEMU swtpm.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/tpm.h"
#include "kernel/tpm_baseline.h"
#include "kernel/boot_proto_descriptor.h"
#include "libc/string.h"

/* The measured PCR set the baseline canonically pins (matches tpm_baseline.c). */
static const uint8_t k_baseline_pcrs[TPM_BASELINE_MAX_PCRS] =
    { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 11u };

/* Build a CANONICAL well-formed golden baseline: the full measured PCR set in
 * the expected index order, all present, plus SB state + firmware hash. */
static void make_golden(struct tpm_baseline *b)
{
    uint32_t i;
    memset(b, 0, sizeof(*b));
    b->alg = TPM_ALG_SHA256;
    b->generation = 1u;
    b->pcr_count = TPM_BASELINE_MAX_PCRS;
    for (i = 0; i < TPM_BASELINE_MAX_PCRS; i++) {
        b->pcrs[i].index = k_baseline_pcrs[i];
        b->pcrs[i].present = 1u;
        memset(b->pcrs[i].digest, (int)(0xA0u + i), TPM_BASELINE_DIGEST);
    }
    b->secure_boot = 1u;
    b->secure_boot_valid = 1u;
    b->fw_hash_present = 1u;
    memset(b->fw_hash, 0xCC, TPM_BASELINE_DIGEST);
    tpm_baseline_finalize(b);
}

static void test_baseline_finalize_validate(void)
{
    struct tpm_baseline b;
    struct tpm_baseline out;

    make_golden(&b);
    TEST_ASSERT_EQ(tpm_baseline_finalize(&b), (uint32_t)sizeof(struct tpm_baseline),
                   "finalize returns blob size");
    TEST_ASSERT_EQ(b.magic, TPM_BASELINE_MAGIC, "finalize stamps magic");
    TEST_ASSERT_EQ((uint32_t)b.version, (uint32_t)TPM_BASELINE_VERSION, "finalize stamps version");
    TEST_ASSERT_EQ((uint32_t)b.size, (uint32_t)sizeof(struct tpm_baseline), "finalize stamps size");

    TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&b, sizeof(b), &out), 1,
                   "well-formed canonical baseline validates");
    TEST_ASSERT_EQ((uint32_t)out.generation, 1u, "validate copies fields into the aligned out");

    /* NULL out / truncated blob -> reject. */
    TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&b, sizeof(b), 0), 0,
                   "NULL out rejected");
    TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&b, sizeof(b) - 1u, &out), 0,
                   "truncated blob rejected");
    /* Corrupt magic -> reject. */
    {
        struct tpm_baseline c = b;
        c.magic ^= 0xFFu;
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "bad magic rejected");
    }
    /* Wrong version -> reject. */
    {
        struct tpm_baseline c = b;
        c.version = (uint16_t)(TPM_BASELINE_VERSION + 1u);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "bad version rejected");
    }
    /* Tampered body without recomputing crc -> reject (crc covers the body). */
    {
        struct tpm_baseline c = b;
        c.pcrs[0].digest[0] ^= 0xFFu;
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "body tamper without crc fixup rejected");
    }
    /* CANONICAL-STRUCTURE attacks (CRC-valid but hollow) -- all rejected: */
    /* pcr_count != full measured set. */
    {
        struct tpm_baseline c = b;
        c.pcr_count = 2u;
        tpm_baseline_finalize(&c);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "non-full pcr_count rejected");
    }
    /* Every PCR present=0 -> pins nothing -> reject (F-B1). */
    {
        struct tpm_baseline c = b;
        uint32_t i;
        for (i = 0; i < TPM_BASELINE_MAX_PCRS; i++) c.pcrs[i].present = 0u;
        tpm_baseline_finalize(&c);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "all-absent PCR set rejected (pins nothing)");
    }
    /* PARTIAL present-set (8 of 9) -> reject: a verifiable baseline must pin the
     * FULL measured set, else the unpinned PCR's changes go unverified (F-RV3). */
    {
        struct tpm_baseline c = b;
        c.pcrs[4].present = 0u;
        tpm_baseline_finalize(&c);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "partial present-set rejected (must pin all measured PCRs)");
    }
    /* Wrong PCR index order -> reject. */
    {
        struct tpm_baseline c = b;
        c.pcrs[0].index = 9u;
        tpm_baseline_finalize(&c);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "wrong PCR index order rejected");
    }
    /* Out-of-range boolean flag -> reject. */
    {
        struct tpm_baseline c = b;
        c.secure_boot = 2u;
        tpm_baseline_finalize(&c);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "non-boolean flag rejected");
    }
}

static void test_baseline_compare(void)
{
    struct tpm_baseline g, c;
    make_golden(&g);

    /* Identical current snapshot -> MATCH. */
    c = g;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MATCH,
                   "identical snapshot matches");

    /* A changed PCR digest -> MISMATCH. */
    c = g; c.pcrs[0].digest[5] ^= 0xFFu;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "changed PCR digest -> mismatch");

    /* A golden PCR no longer present in current -> MISMATCH. */
    c = g; c.pcrs[1].present = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "golden PCR absent in current -> mismatch");

    /* Secure Boot state change -> MISMATCH. */
    c = g; c.secure_boot = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "SB state change -> mismatch");
    /* SB readability change (valid->unknown) -> MISMATCH (never a silent match). */
    c = g; c.secure_boot_valid = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "SB readability change -> mismatch");

    /* Firmware-version hash change -> MISMATCH. */
    c = g; c.fw_hash[0] ^= 0xFFu;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "firmware-version hash change -> mismatch");

    /* Different hash bank -> MISMATCH. */
    c = g; c.alg = TPM_ALG_SHA384;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "different bank -> mismatch");

    /* NULL args -> CMP_BADARG. */
    TEST_ASSERT_EQ((int)tpm_baseline_compare(0, &c), (int)TPM_BASELINE_CMP_BADARG,
                   "NULL golden -> badarg");
}

static void test_baseline_rotation(void)
{
    TEST_ASSERT_EQ(tpm_baseline_rotation_ok(1u, 2u), 1, "higher generation accepted");
    TEST_ASSERT_EQ(tpm_baseline_rotation_ok(2u, 2u), 0, "equal generation rejected (no rollback)");
    TEST_ASSERT_EQ(tpm_baseline_rotation_ok(5u, 3u), 0, "lower generation rejected (rollback)");
}

/* The build-time ABI-manifest digest accessor. Pure: it reads only the const
 * `.bootproto` descriptor linked into this same kernel, so it is safe to call
 * from a test (no live boot infrastructure, no MMIO, no init). */
static void test_abi_manifest_digest(void)
{
    uint8_t d[BOOT_PROTO_ABI_DIGEST_LEN];
    uint8_t z[BOOT_PROTO_ABI_DIGEST_LEN];
    uint32_t i, nonzero = 0u;

    memset(z, 0, sizeof(z));
    memset(d, 0xEE, sizeof(d));

    TEST_ASSERT_EQ(boot_proto_abi_digest(d), 1, "digest available in a built kernel");
    for (i = 0; i < (uint32_t)BOOT_PROTO_ABI_DIGEST_LEN; i++)
        nonzero |= d[i];
    TEST_ASSERT_EQ((int)(nonzero != 0u), 1, "digest is not all-zero");
    TEST_ASSERT_EQ(memcmp(d, kernel_boot_proto.sha256, BOOT_PROTO_ABI_DIGEST_LEN), 0,
                   "digest equals the .bootproto descriptor sha256");
    /* Control: the comparison above would also pass if BOTH sides were zeroed,
     * which is exactly the corruption shape the accessor must refuse. */
    TEST_ASSERT_EQ((int)(memcmp(d, z, BOOT_PROTO_ABI_DIGEST_LEN) != 0), 1,
                   "control: the compared digest is genuinely non-zero");

    /* Two calls agree -- the descriptor is immutable, so the value is stable. */
    memset(z, 0, sizeof(z));
    TEST_ASSERT_EQ(boot_proto_abi_digest(z), 1, "second call succeeds");
    TEST_ASSERT_EQ(memcmp(d, z, BOOT_PROTO_ABI_DIGEST_LEN), 0, "digest is stable across calls");

    TEST_ASSERT_EQ(boot_proto_abi_digest(0), 0, "NULL out -> 0, no fault");
}

/* The fail-closed branches, driven through the pure seam. kernel_boot_proto is
 * const and linked into the image, so these are the ONLY way to prove that a
 * corrupt descriptor returns 0 AND leaves no stale bytes behind -- which is the
 * safety property the whole ABI binding rests on. */
static void test_abi_manifest_digest_corruption(void)
{
    struct boot_proto_descriptor d;
    uint8_t out[BOOT_PROTO_ABI_DIGEST_LEN];
    uint8_t zero[BOOT_PROTO_ABI_DIGEST_LEN];
    uint32_t i;

    memset(zero, 0, sizeof(zero));

    /* Control: a well-formed synthetic descriptor succeeds, so the failures
     * below are attributable to the corruption and not to the fixture. */
    memset(&d, 0, sizeof(d));
    d.magic = BOOT_PROTO_DESCRIPTOR_MAGIC;
    for (i = 0; i < (uint32_t)BOOT_PROTO_ABI_DIGEST_LEN; i++)
        d.sha256[i] = (uint8_t)(i + 1u);
    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQ(boot_proto_abi_digest_from(&d, out), 1, "control: well-formed descriptor -> 1");
    TEST_ASSERT_EQ(memcmp(out, d.sha256, BOOT_PROTO_ABI_DIGEST_LEN), 0,
                   "control: digest copied verbatim");

    /* Bad magic -> 0, and the prefilled sentinel must be wiped. */
    d.magic = BOOT_PROTO_DESCRIPTOR_MAGIC ^ 0xFFu;
    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQ(boot_proto_abi_digest_from(&d, out), 0, "bad magic -> 0");
    TEST_ASSERT_EQ(memcmp(out, zero, BOOT_PROTO_ABI_DIGEST_LEN), 0,
                   "bad magic zeroes the output (no stale bytes)");

    /* Valid magic but an all-zero digest -> 0, output still wiped. */
    d.magic = BOOT_PROTO_DESCRIPTOR_MAGIC;
    memset(d.sha256, 0, sizeof(d.sha256));
    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQ(boot_proto_abi_digest_from(&d, out), 0, "all-zero digest -> 0");
    TEST_ASSERT_EQ(memcmp(out, zero, BOOT_PROTO_ABI_DIGEST_LEN), 0,
                   "all-zero digest zeroes the output");

    /* A single non-zero byte anywhere is enough -- the scan must cover the
     * whole field, not just its head. */
    memset(out, 0xEE, sizeof(out));
    d.sha256[BOOT_PROTO_ABI_DIGEST_LEN - 1] = 0x01u;
    TEST_ASSERT_EQ(boot_proto_abi_digest_from(&d, out), 1,
                   "non-zero in the LAST byte is still a digest");

    /* NULL descriptor -> 0, output wiped. */
    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQ(boot_proto_abi_digest_from(0, out), 0, "NULL descriptor -> 0");
    TEST_ASSERT_EQ(memcmp(out, zero, BOOT_PROTO_ABI_DIGEST_LEN), 0,
                   "NULL descriptor zeroes the output");

    /* The live accessor is the seam applied to the linked descriptor. */
    {
        uint8_t live[BOOT_PROTO_ABI_DIGEST_LEN];
        uint8_t via[BOOT_PROTO_ABI_DIGEST_LEN];
        TEST_ASSERT_EQ(boot_proto_abi_digest(live), 1, "live accessor succeeds");
        TEST_ASSERT_EQ(boot_proto_abi_digest_from(&kernel_boot_proto, via), 1,
                       "seam over the linked descriptor succeeds");
        TEST_ASSERT_EQ(memcmp(live, via, BOOT_PROTO_ABI_DIGEST_LEN), 0,
                       "live accessor equals the seam over kernel_boot_proto");
    }
}

/* Presence semantics for the ABI-manifest digest. This is the distinction the
 * section exists to make: the manifest binds the kernel ABI identity into the
 * baseline, and a presence disagreement in EITHER direction must be a mismatch
 * (unlike the fw-hash, which is legitimately optional). */
static void test_baseline_abi_manifest_compare(void)
{
    struct tpm_baseline g, c;

    make_golden(&g);
    g.abi_manifest_present = 1u;
    memset(g.abi_manifest, 0x5A, TPM_BASELINE_DIGEST);
    tpm_baseline_finalize(&g);

    /* Control: two populated, EQUAL manifests still match. Without this the
     * mismatch assertions below would all pass against a compare that simply
     * rejected everything. */
    c = g;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MATCH,
                   "control: equal populated manifests match");

    /* A changed manifest digest -> MISMATCH (a different kernel ABI ran). */
    c = g; c.abi_manifest[3] ^= 0xFFu;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "changed ABI manifest -> mismatch");

    /* Golden bound, current absent -> MISMATCH (the binding cannot be dropped). */
    c = g; c.abi_manifest_present = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "golden bound + current absent -> mismatch");

    /* Golden UNBOUND, current bound -> MISMATCH. This is the asymmetric case a
     * one-way gate would silently pass: a pre-binding baseline would keep
     * reporting VERIFIED forever while attesting nothing about the ABI. */
    {
        struct tpm_baseline legacy;
        make_golden(&legacy);   /* leaves abi_manifest_present = 0 */
        TEST_ASSERT_EQ((int)legacy.abi_manifest_present, 0,
                       "control: the legacy fixture really is unbound");
        TEST_ASSERT_EQ((int)tpm_baseline_compare(&legacy, &g), (int)TPM_BASELINE_MISMATCH,
                       "legacy unbound golden + bound current -> mismatch");
    }

    /* The fw-hash gate stays ONE-WAY on purpose: SMBIOS can be genuinely
     * absent, so a golden without a firmware hash is a legitimate state. */
    c = g; g.fw_hash_present = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MATCH,
                   "fw-hash stays one-way: golden absent + current present matches");
}

/* A stored blob claiming a manifest it does not carry is a shape no honest
 * enroll can produce, so validate must refuse it rather than canonicalize it. */
static void test_baseline_validate_manifest_shape(void)
{
    struct tpm_baseline b, out;

    /* Control: present=1 WITH a real digest validates. */
    make_golden(&b);
    b.abi_manifest_present = 1u;
    memset(b.abi_manifest, 0x77, TPM_BASELINE_DIGEST);
    tpm_baseline_finalize(&b);
    TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&b, (uint32_t)sizeof(b), &out), 1,
                   "control: present manifest with a real digest validates");

    /* present=1 with an all-zero digest -> rejected, even with a valid CRC. */
    memset(b.abi_manifest, 0, TPM_BASELINE_DIGEST);
    tpm_baseline_finalize(&b);
    TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&b, (uint32_t)sizeof(b), &out), 0,
                   "CRC-valid blob claiming a present but all-zero manifest is rejected");

    /* present=0 with a zero digest is the legacy shape and still validates --
     * it must stay parseable so compare can report it as a mismatch rather
     * than the blob being read as corrupt. */
    b.abi_manifest_present = 0u;
    tpm_baseline_finalize(&b);
    TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&b, (uint32_t)sizeof(b), &out), 1,
                   "legacy unbound blob still validates (compare reports it, not validate)");
}

void test_register_tpm_baseline(void)
{
    test_suite_register_cat("tpm: baseline finalize/validate", test_baseline_finalize_validate, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline compare", test_baseline_compare, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline rotation guard", test_baseline_rotation, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: ABI manifest digest accessor", test_abi_manifest_digest, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: ABI manifest corruption paths",
                            test_abi_manifest_digest_corruption, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline ABI manifest compare",
                            test_baseline_abi_manifest_compare, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline manifest shape validation",
                            test_baseline_validate_manifest_shape, TEST_CAT_SECURITY);
}
