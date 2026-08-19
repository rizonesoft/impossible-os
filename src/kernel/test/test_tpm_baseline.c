/* ============================================================================
 * test_tpm_baseline.c -- measured-boot baseline pure-core tests
 *
 * Format / validate / compare / generation logic (MMIO-free, no live TPM). The
 * live snapshot / enroll / verify wrappers are validated against QEMU swtpm.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/tpm.h"
#include "kernel/klog.h"   /* klog_entry_t.message cap -- the guidance lines must fit */
#include "kernel/tpm_baseline.h"
#include "kernel/tpm_nv.h"          /* TPM_NV_INDEX_BASELINE */
#include "kernel/tpm_pcr_alloc.h"   /* tpm_pcr_baseline_pcrs (canonical set) */
#include "kernel/tpm_transport.h"   /* tpm_t_test_install/restore (no-transport path) */
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

/* Per-PCR comparison detail. tpm_baseline_compare answers "does this boot
 * match" and stops at the first difference, so it cannot say WHICH PCR moved --
 * and on a scalar difference it never looks at a PCR at all. These assert that
 * compare_pcrs answers that question independently of the scalars. */
static void test_baseline_compare_pcrs(void)
{
    struct tpm_baseline g, c;
    uint8_t st[TPM_BASELINE_MAX_PCRS];
    uint8_t i, n;

    /* Control: an identical snapshot verifies every slot. Without this a
     * detector that returned VERIFIED unconditionally would pass the
     * attribution cases below. */
    make_golden(&g);
    make_golden(&c);
    n = tpm_baseline_compare_pcrs(&g, &c, st, TPM_BASELINE_MAX_PCRS);
    TEST_ASSERT_EQ(n, (uint8_t)TPM_BASELINE_MAX_PCRS,
                   "one status per golden PCR slot");
    for (i = 0; i < n; i++)
        TEST_ASSERT_EQ(st[i], (uint8_t)BOOT_INTEGRITY_VERIFIED,
                       "identical snapshot verifies every PCR");

    /* PCR 4 alone moved: slot 4 is MISMATCH and NOTHING else is. This is the
     * attribution the report needs -- the overall verdict already said
     * MISMATCH, the per-PCR detail is what names the culprit. */
    make_golden(&c);
    memset(c.pcrs[4].digest, 0x5A, TPM_BASELINE_DIGEST);
    n = tpm_baseline_compare_pcrs(&g, &c, st, TPM_BASELINE_MAX_PCRS);
    TEST_ASSERT_EQ(n, (uint8_t)TPM_BASELINE_MAX_PCRS, "count unchanged");
    for (i = 0; i < n; i++)
        TEST_ASSERT_EQ(st[i],
                       (i == 4u) ? (uint8_t)BOOT_INTEGRITY_MISMATCH
                                 : (uint8_t)BOOT_INTEGRITY_VERIFIED,
                       "only the PCR that differs is reported as mismatched");
    /* Attribution must follow the PCR INDEX, not the array position. Asserting
     * the fixture's own `g.pcrs[4].index == 4` proved nothing: every snapshot
     * shares make_golden's ordering, so a positional `pcrs[i]` comparison would
     * pass the whole suite. PERMUTING the current entries while preserving each
     * index/digest pair is what distinguishes the two. */
    make_golden(&c);
    {
        struct tpm_baseline_pcr tmp = c.pcrs[1];
        c.pcrs[1] = c.pcrs[7];
        c.pcrs[7] = tmp;
    }
    n = tpm_baseline_compare_pcrs(&g, &c, st, TPM_BASELINE_MAX_PCRS);
    for (i = 0; i < n; i++)
        TEST_ASSERT_EQ(st[i], (uint8_t)BOOT_INTEGRITY_VERIFIED,
                       "matching by index, a permuted current snapshot verifies");

    /* And when an index genuinely disappears from the current snapshot, ONLY
     * the golden PCR that lost its match is blamed. */
    make_golden(&c);
    c.pcrs[3].index = 20u;      /* PCR 3 no longer present; PCR 20 appears */
    n = tpm_baseline_compare_pcrs(&g, &c, st, TPM_BASELINE_MAX_PCRS);
    for (i = 0; i < n; i++)
        TEST_ASSERT_EQ(st[i],
                       (i == 3u) ? (uint8_t)BOOT_INTEGRITY_MISMATCH
                                 : (uint8_t)BOOT_INTEGRITY_VERIFIED,
                       "a golden index absent from the current snapshot is the "
                       "only one blamed");

    /* A golden PCR that the current boot can no longer produce is a mismatch,
     * not a pass -- the measured state changed even though no digest differs. */
    make_golden(&c);
    c.pcrs[6].present = 0u;
    n = tpm_baseline_compare_pcrs(&g, &c, st, TPM_BASELINE_MAX_PCRS);
    TEST_ASSERT_EQ(st[6], (uint8_t)BOOT_INTEGRITY_MISMATCH,
                   "golden PCR now unreadable is a mismatch");
    TEST_ASSERT_EQ(st[5], (uint8_t)BOOT_INTEGRITY_VERIFIED,
                   "its neighbours are untouched");

    /* A golden that pins nothing for a slot leaves that PCR UNVERIFIED, never
     * mismatched -- reporting a mismatch there would invent a culprit. */
    make_golden(&g);
    g.pcrs[2].present = 0u;
    make_golden(&c);
    n = tpm_baseline_compare_pcrs(&g, &c, st, TPM_BASELINE_MAX_PCRS);
    TEST_ASSERT_EQ(st[2], (uint8_t)BOOT_INTEGRITY_NO_BASELINE,
                   "unpinned golden slot reports no-baseline, not mismatch");

    /* THE SCALAR CASE, which is why this function exists. A Secure Boot change
     * makes tpm_baseline_compare return MISMATCH before it inspects a single
     * PCR; the per-PCR detail must still show the PCRs as they actually
     * compared, so the report cannot read as PCR tampering. */
    make_golden(&g);
    make_golden(&c);
    c.secure_boot = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "control: a Secure Boot change is an overall mismatch");
    n = tpm_baseline_compare_pcrs(&g, &c, st, TPM_BASELINE_MAX_PCRS);
    TEST_ASSERT_EQ(n, (uint8_t)TPM_BASELINE_MAX_PCRS,
                   "per-PCR detail is produced even on a scalar mismatch");
    for (i = 0; i < n; i++)
        TEST_ASSERT_EQ(st[i], (uint8_t)BOOT_INTEGRITY_VERIFIED,
                       "a scalar-caused mismatch does not blame any PCR");

    /* Different banks make no digest comparable, so nothing may look verified. */
    make_golden(&c);
    c.alg = TPM_ALG_SHA1;
    n = tpm_baseline_compare_pcrs(&g, &c, st, TPM_BASELINE_MAX_PCRS);
    for (i = 0; i < n; i++)
        TEST_ASSERT_EQ(st[i], (uint8_t)BOOT_INTEGRITY_MISMATCH,
                       "a bank disagreement leaves no PCR verified");

    /* Bad arguments write nothing and report 0 -- which the caller must read as
     * NOT EVALUATED, never as a clean pass. Asserted with a canary rather than
     * by return value alone, so "returns 0 but scribbled anyway" still fails. */
    make_golden(&c);
    memset(st, 0xEE, sizeof(st));
    TEST_ASSERT_EQ(tpm_baseline_compare_pcrs((const struct tpm_baseline *)0, &c,
                                             st, TPM_BASELINE_MAX_PCRS), 0u,
                   "NULL golden reports nothing evaluated");
    TEST_ASSERT_EQ(st[0], 0xEEu, "NULL golden writes no status");
    TEST_ASSERT_EQ(tpm_baseline_compare_pcrs(&g, (const struct tpm_baseline *)0,
                                             st, TPM_BASELINE_MAX_PCRS), 0u,
                   "NULL current reports nothing evaluated");
    TEST_ASSERT_EQ(st[0], 0xEEu, "NULL current writes no status");
    TEST_ASSERT_EQ(tpm_baseline_compare_pcrs(&g, &c, (uint8_t *)0,
                                             TPM_BASELINE_MAX_PCRS), 0u,
                   "NULL out buffer writes no status");
    TEST_ASSERT_EQ(tpm_baseline_compare_pcrs(&g, &c, st, 0u), 0u,
                   "zero capacity evaluates nothing");
    TEST_ASSERT_EQ(st[0], 0xEEu, "zero capacity writes no status");

    /* An oversized pcr_count is rejected rather than walked. */
    {
        struct tpm_baseline bad;
        make_golden(&bad);
        bad.pcr_count = (uint8_t)(TPM_BASELINE_MAX_PCRS + 1u);
        TEST_ASSERT_EQ(tpm_baseline_compare_pcrs(&bad, &c, st,
                                                 TPM_BASELINE_MAX_PCRS), 0u,
                       "oversized golden pcr_count is refused");
        TEST_ASSERT_EQ(st[0], 0xEEu, "oversized golden writes no status");
        make_golden(&bad);
        bad.pcr_count = (uint8_t)(TPM_BASELINE_MAX_PCRS + 1u);
        TEST_ASSERT_EQ(tpm_baseline_compare_pcrs(&g, &bad, st,
                                                 TPM_BASELINE_MAX_PCRS), 0u,
                       "oversized current pcr_count is refused");
    }

    /* A short buffer is honoured rather than overrun. The count alone cannot
     * show this -- an implementation that wrote all 9 entries and returned 3
     * would pass -- so the assertion is on GUARD BYTES past the stated
     * capacity, which must survive untouched. */
    {
        uint8_t guarded[TPM_BASELINE_MAX_PCRS];
        uint8_t k;
        memset(guarded, 0xEE, sizeof(guarded));
        TEST_ASSERT_EQ(tpm_baseline_compare_pcrs(&g, &c, guarded, 3u), 3u,
                       "count is clamped to the caller's capacity");
        for (k = 3u; k < TPM_BASELINE_MAX_PCRS; k++)
            TEST_ASSERT_EQ(guarded[k], 0xEEu,
                           "nothing is written past the stated capacity");
        TEST_ASSERT(guarded[0] != 0xEEu, "control: the in-capacity slots WERE written");
    }
}

/* The verify WRAPPER's per-PCR out-params, on the path this suite can reach
 * without a full NV-blob fake: with no transport installed the NV read fails,
 * verify returns NO_TPM, and the contract is that it publishes NO verdict and
 * reports NOTHING evaluated. The sentinel must be zeroed by verify itself, so
 * the buffers are POISONED first -- a wrapper that simply never touched them
 * would otherwise pass. */
static void test_baseline_verify_no_transport(void)
{
    struct tpm_t_test_state prev;
    uint8_t overall = 0xEEu;
    uint8_t pcr_status[TPM_BASELINE_MAX_PCRS];
    uint8_t pcr_n = 0xEEu;
    tpm_baseline_status_t st;

    memset(pcr_status, 0xEE, sizeof(pcr_status));
    prev = tpm_t_test_install((const struct tpm_t_io *)0, TPM_T_IFACE_NONE, 0);

    st = tpm_baseline_verify(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256, &overall,
                             pcr_status, (uint8_t)TPM_BASELINE_MAX_PCRS, &pcr_n);

    TEST_ASSERT_EQ((int)st, (int)TPM_BASELINE_NO_TPM,
                   "no transport verifies to NO_TPM");
    TEST_ASSERT_EQ(pcr_n, 0u,
                   "a path that never compared reports nothing evaluated");
    TEST_ASSERT_EQ(pcr_status[0], 0xEEu,
                   "a non-comparing path writes no per-PCR status");
    TEST_ASSERT_EQ(overall, 0xEEu,
                   "a non-verdict return leaves the overall status untouched");

    /* NULL out-params on the same path must not fault. Declining the detail is
     * always legal; it is only an UNDERSIZED buffer that is refused. */
    st = tpm_baseline_verify(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256,
                             (uint8_t *)0, (uint8_t *)0, 0u, (uint8_t *)0);
    TEST_ASSERT_EQ((int)st, (int)TPM_BASELINE_NO_TPM,
                   "NULL out-params are accepted on the no-transport path");

    /* The out-params are INDEPENDENTLY optional, per the header. A status buffer
     * WITHOUT a count pointer is legal and must not be refused for that reason;
     * the capacity rule still applies to it. */
    memset(pcr_status, 0xEE, sizeof(pcr_status));
    st = tpm_baseline_verify(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256, &overall,
                             pcr_status, (uint8_t)TPM_BASELINE_MAX_PCRS,
                             (uint8_t *)0);
    TEST_ASSERT_EQ((int)st, (int)TPM_BASELINE_NO_TPM,
                   "a status buffer without a count pointer is accepted");
    st = tpm_baseline_verify(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256, &overall,
                             pcr_status, 1u, (uint8_t *)0);
    TEST_ASSERT_EQ((int)st, (int)TPM_BASELINE_BADARG,
                   "the capacity rule still applies without a count pointer");
    /* And a count pointer WITHOUT a status buffer is legal, reporting 0. */
    pcr_n = 0xEEu;
    st = tpm_baseline_verify(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256, &overall,
                             (uint8_t *)0, 0u, &pcr_n);
    TEST_ASSERT_EQ((int)st, (int)TPM_BASELINE_NO_TPM,
                   "a count pointer without a status buffer is accepted");
    TEST_ASSERT_EQ(pcr_n, 0u, "the count is zeroed even with no status buffer");

    /* An UNDERSIZED detail buffer is BADARG, refused as an argument error
     * before any TPM work -- never a verdict beside a truncated detail array.
     * It is checked ahead of the transport, so it outranks NO_TPM here, which
     * is what proves it happens before the NV read rather than after it.
     *
     * The OTHER half of that ordering -- that the kernel self-identity check
     * still outranks this BADARG -- cannot be asserted from a unit test: the
     * real `boot_proto_abi_digest()` reads the linked-in read-only `.bootproto`
     * descriptor, and the only corruption seam is the pure
     * `boot_proto_abi_digest_from(&d, out)` variant that takes a caller's
     * descriptor. Adding a seam to force the live self-check to fail would let
     * any caller spoof the fail-closed identity gate, which is a worse trade
     * than the missing assertion. The precedence is held by statement order in
     * tpm_baseline_verify and documented at the site. */
    overall = 0xEEu;
    pcr_n = 0xEEu;
    memset(pcr_status, 0xEE, sizeof(pcr_status));
    st = tpm_baseline_verify(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256, &overall,
                             pcr_status,
                             (uint8_t)(TPM_BASELINE_MAX_PCRS - 1u), &pcr_n);
    TEST_ASSERT_EQ((int)st, (int)TPM_BASELINE_BADARG,
                   "a detail buffer smaller than the measured set is refused");
    TEST_ASSERT_EQ(pcr_n, 0u, "a refused call reports nothing evaluated");
    TEST_ASSERT_EQ(overall, 0xEEu, "a refused call publishes no verdict");
    TEST_ASSERT_EQ(pcr_status[0], 0xEEu, "a refused call writes no detail");

    /* Control: the SAME call with an exactly-sized buffer gets past the
     * argument check and reaches the transport. Without this the assertion
     * above would also pass against a function that refused everything. */
    st = tpm_baseline_verify(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256, &overall,
                             pcr_status, (uint8_t)TPM_BASELINE_MAX_PCRS, &pcr_n);
    TEST_ASSERT_EQ((int)st, (int)TPM_BASELINE_NO_TPM,
                   "control: an exactly-sized buffer is accepted");

    tpm_t_test_restore(prev);
}

/* Publication: the report is an immutable snapshot swapped in whole, so a
 * reader's copy can never be mutated underneath it and the overall verdict can
 * never be published apart from its per-PCR detail. */
static void test_integrity_publication(void)
{
    struct boot_integrity_report saved, a, b;
    uint8_t st[BOOT_INTEGRITY_MAX_PCRS];
    uint8_t i;

    /* Save the boot's real verdict; every path below restores it. */
    tpm_integrity_report_copy(&saved);

    /* Seed a fixture with the full measured set, then publish VERIFIED with
     * per-PCR detail to match. */
    memset(&a, 0, sizeof(a));
    a.pcr_count = (uint8_t)BOOT_INTEGRITY_MAX_PCRS;
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++) {
        a.pcrs[i].pcr_index = (i < 8u) ? i : 11u;
        a.pcrs[i].status = (uint8_t)BOOT_INTEGRITY_NO_CRYPTO;
    }
    tpm_integrity_test_republish(&a);

    /* PCR 11 has a SLOT at all -- the report was sized 8 and could not carry
     * it. The claim under test is the SIZING, so it is asserted against the
     * canonical measured set rather than against the fixture's own injected
     * value (which would only prove memcpy works): the report must hold every
     * PCR that tpm_pcr_baseline_pcrs() enumerates, and 11 must be among them. */
    {
        uint8_t set[TPM_BASELINE_MAX_PCRS];
        uint8_t n = tpm_pcr_baseline_pcrs(set, (uint8_t)TPM_BASELINE_MAX_PCRS);
        uint8_t k, found11 = 0;
        /* EXACT equality, matching the guard tpm_integrity_build_report ships.
         * `<=` would still pass if the canonical set LOST a PCR, which is one
         * of the two drift directions the guard exists to refuse. */
        TEST_ASSERT_EQ(n, (uint8_t)BOOT_INTEGRITY_MAX_PCRS,
                       "the report is sized to exactly the measured-boot set");
        for (k = 0; k < n; k++)
            if (set[k] == 11u)
                found11 = 1u;
        TEST_ASSERT_EQ(found11, 1u, "PCR 11 is in the measured-boot set");
    }

    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        st[i] = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, st,
                                   (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_VERIFIED,
                   "verified verdict published");
    for (i = 0; i < b.pcr_count; i++)
        TEST_ASSERT(b.pcrs[i].status != (uint8_t)BOOT_INTEGRITY_NO_CRYPTO,
                    "no slot still reads NO_CRYPTO behind a VERIFIED verdict");

    /* A path that never compared publishes NOT-EVALUATED per-PCR rather than
     * leaving the stale VERIFIED detail standing beside a NO_BASELINE verdict. */
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_NO_BASELINE, (const uint8_t *)0, 0u);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_NO_BASELINE,
                   "no-baseline verdict published");
    for (i = 0; i < b.pcr_count; i++)
        TEST_ASSERT_EQ(b.pcrs[i].status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                       "a path that never compared reports not-evaluated");

    /* A reader's copy is a SNAPSHOT: publishing again cannot mutate it. That is
     * the property a naked pointer into the live slot could not provide, and it
     * is what makes acquisition safe rather than merely well-timed. */
    tpm_integrity_report_copy(&a);
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        st[i] = (uint8_t)BOOT_INTEGRITY_MISMATCH;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_MISMATCH, st,
                                   (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    TEST_ASSERT_EQ(a.overall_status, (uint8_t)BOOT_INTEGRITY_NO_BASELINE,
                   "an earlier copy is unaffected by a later publication");
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_MISMATCH,
                   "a fresh copy sees the new snapshot");

    /* Many publications in a row: the slots are recycled, and a copy taken
     * after each one is complete and self-consistent -- it never spins and
     * never observes a half-written mixture. A seqlock reader could stall here
     * on a writer stuck mid-update; the copy-out cannot. */
    for (i = 0; i < 8u; i++) {
        uint8_t want = (uint8_t)((i & 1u) ? BOOT_INTEGRITY_VERIFIED
                                          : BOOT_INTEGRITY_MISMATCH);
        uint8_t j;
        for (j = 0; j < BOOT_INTEGRITY_MAX_PCRS; j++)
            st[j] = want;
        tpm_integrity_publish_baseline(want, st, (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
        tpm_integrity_report_copy(&b);
        TEST_ASSERT_EQ(b.overall_status, want,
                       "each publication is observed whole");
        for (j = 0; j < b.pcr_count; j++)
            TEST_ASSERT_EQ(b.pcrs[j].status, want,
                           "per-PCR detail matches the verdict it shipped with");
    }

    /* TAMPER pins MISMATCH: a later baseline VERIFIED must NOT downgrade it,
     * and the per-PCR detail is still refreshed. A pinned verdict is a
     * statement about the verdict, not a licence to report stale PCR values. */
    memset(&a, 0, sizeof(a));
    a.pcr_count = (uint8_t)BOOT_INTEGRITY_MAX_PCRS;
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++) {
        a.pcrs[i].pcr_index = (i < 8u) ? i : 11u;
        a.pcrs[i].status = (uint8_t)BOOT_INTEGRITY_NO_CRYPTO;
    }
    a.replay_verdict = 1u;                       /* TPM_REPLAY_TAMPER */
    a.overall_status = (uint8_t)BOOT_INTEGRITY_MISMATCH;
    tpm_integrity_test_republish(&a);
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        st[i] = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, st,
                                   (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_MISMATCH,
                   "a baseline VERIFIED never overwrites a replay TAMPER");
    for (i = 0; i < b.pcr_count; i++)
        TEST_ASSERT_EQ(b.pcrs[i].status, (uint8_t)BOOT_INTEGRITY_VERIFIED,
                       "per-PCR detail is refreshed even while the verdict is pinned");

    /* An oversized count must be clamped by the INDEPENDENT
     * BOOT_INTEGRITY_MAX_PCRS write bound, not merely by pcr_count. The earlier
     * shape could not test that: it seeded pcr_count=9, so n=255 executed only
     * nine iterations through the pcr_count bound and removing the clamp
     * changed nothing. Seeding an OVERSIZED pcr_count is what challenges the
     * real bound, and the canary is event_count -- the field that actually
     * follows pcrs[] in the struct (replay_verdict is last, several fields
     * further on, so it was the wrong canary). */
    memset(&a, 0, sizeof(a));
    a.pcr_count = 255u;                    /* deliberately impossible */
    a.event_count = 0xA5A5A5A5u;           /* overrun canary, sits after pcrs[] */
    a.tpm_version = 2u;
    a.secure_boot = 1u;
    a.secure_boot_valid = 1u;
    a.tpm_rng_available = 1u;
    a.replay_verdict = 0u;
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        a.pcrs[i].pcr_index = (i < 8u) ? i : 11u;
    tpm_integrity_test_republish(&a);

    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        st[i] = (uint8_t)BOOT_INTEGRITY_NO_TPM;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_MISMATCH, st, 255u);
    tpm_integrity_report_copy(&b);
    /* pcr_count is NORMALIZED at publication, so no reader can be handed a
     * count larger than pcrs[] holds -- a reader's natural
     * `for (i = 0; i < r.pcr_count; i++)` over its own copy would otherwise
     * over-read its stack struct by 246 entries. */
    TEST_ASSERT_EQ(b.pcr_count, (uint8_t)BOOT_INTEGRITY_MAX_PCRS,
                   "an oversized pcr_count is clamped at publication");
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        TEST_ASSERT_EQ(b.pcrs[i].status, (uint8_t)BOOT_INTEGRITY_NO_TPM,
                       "an oversized count writes every real slot");
    TEST_ASSERT_EQ(b.event_count, 0xA5A5A5A5u,
                   "the field immediately after pcrs[] survives the overrun");
    TEST_ASSERT_EQ(b.tpm_version, 2u, "later fields survive the overrun");
    TEST_ASSERT_EQ(b.secure_boot_valid, 1u, "later fields survive the overrun");
    TEST_ASSERT_EQ(b.tpm_rng_available, 1u, "later fields survive the overrun");

    /* BOTH real writer orderings, through the REAL setters. The fixture above
     * pre-set replay_verdict via the restore seam, which cannot catch a broken
     * tpm_integrity_set_replay_verdict or a lost update between the two. */
    memset(&a, 0, sizeof(a));
    a.pcr_count = (uint8_t)BOOT_INTEGRITY_MAX_PCRS;
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++) {
        a.pcrs[i].pcr_index = (i < 8u) ? i : 11u;
        a.pcrs[i].status = (uint8_t)BOOT_INTEGRITY_NO_CRYPTO;
    }
    tpm_integrity_test_republish(&a);

    /* Order 1: TAMPER lands FIRST, then a baseline VERIFIED arrives. */
    tpm_integrity_set_replay_verdict(1u);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_MISMATCH,
                   "the replay setter escalates to MISMATCH by itself");
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        st[i] = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, st,
                                   (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_MISMATCH,
                   "tamper-then-verified: the tamper verdict stands");

    /* Order 2: baseline VERIFIED lands FIRST, then TAMPER arrives. The verdict
     * must END at MISMATCH -- a report that settled on VERIFIED because the
     * tamper signal was late is the failure this ordering exists to catch. */
    tpm_integrity_test_republish(&a);
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, st,
                                   (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_VERIFIED,
                   "control: with no tamper the baseline verdict is published");
    tpm_integrity_set_replay_verdict(1u);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_MISMATCH,
                   "verified-then-tamper: the tamper verdict wins");
    /* And a later baseline verdict still cannot undo it. */
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, st,
                                   (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_MISMATCH,
                   "the pin holds against a repeat baseline publication");

    /* NULL-safety on the reader and the restore seam: neither may fault, and
     * neither may disturb the published snapshot. */
    tpm_integrity_report_copy((struct boot_integrity_report *)0);
    tpm_integrity_test_republish((const struct boot_integrity_report *)0);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_MISMATCH,
                   "NULL arguments leave the published report untouched");

    /* A NULL status array with a nonzero count must not be dereferenced; the
     * detail falls back to NOT-EVALUATED. */
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_MISMATCH,
                                   (const uint8_t *)0,
                                   (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    tpm_integrity_report_copy(&b);
    for (i = 0; i < b.pcr_count; i++)
        TEST_ASSERT_EQ(b.pcrs[i].status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                       "a NULL status array with a nonzero count is not read");

    /* A PARTIAL count fills what it covers and marks the rest not-evaluated,
     * rather than leaving stale values in the uncovered tail. */
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        st[i] = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_MISMATCH, st, 4u);
    tpm_integrity_report_copy(&b);
    for (i = 0; i < b.pcr_count; i++)
        TEST_ASSERT_EQ(b.pcrs[i].status,
                       (i < 4u) ? (uint8_t)BOOT_INTEGRITY_VERIFIED
                                : (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                       "a partial count covers its prefix and blanks the tail");
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_MISMATCH,
                   "a non-VERIFIED verdict is unaffected by partial detail");

    /* COHERENCE: a partial detail array may NOT carry a VERIFIED verdict. The
     * earlier version of this test asserted only the prefix/tail split and so
     * legitimized publishing VERIFIED beside five UNKNOWN slots -- the very
     * verdict-vs-detail contradiction the section exists to remove.
     *
     * Reset to an UNPINNED base first: earlier blocks raised a replay TAMPER,
     * which pins MISMATCH and would mask the coherence guard entirely -- every
     * assertion below would pass for the wrong reason. */
    memset(&a, 0, sizeof(a));
    a.pcr_count = (uint8_t)BOOT_INTEGRITY_MAX_PCRS;
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        a.pcrs[i].pcr_index = (i < 8u) ? i : 11u;
    tpm_integrity_test_republish(&a);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.replay_verdict, 0u,
                   "control: the coherence checks run on an unpinned report");

    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        st[i] = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, st, 4u);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                   "VERIFIED is refused when the detail does not cover every PCR");
    /* A NULL detail array cannot back a VERIFIED verdict either. */
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, (const uint8_t *)0, 0u);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                   "VERIFIED is refused when no detail was evaluated at all");
    /* Control: FULL coverage still publishes VERIFIED, so the guard rejects
     * incoherence rather than rejecting the verdict outright. */
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        st[i] = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, st,
                                   (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_VERIFIED,
                   "control: full VERIFIED detail publishes a VERIFIED verdict");
    /* And a single non-VERIFIED slot is enough to withhold the claim. */
    st[6] = (uint8_t)BOOT_INTEGRITY_MISMATCH;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, st,
                                   (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                   "one non-verified PCR withholds the VERIFIED verdict");

    /* A ZERO-SLOT report cannot carry VERIFIED either. A guard bounded only by
     * pcr_count passes VACUOUSLY here -- it finds no offending slot precisely
     * BECAUSE there is no detail at all, which is the strongest case for
     * withholding the claim, not the weakest. */
    memset(&a, 0, sizeof(a));
    a.pcr_count = 0u;
    tpm_integrity_test_republish(&a);
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        st[i] = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, st,
                                   (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.pcr_count, 0u, "control: the base really carries no slots");
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                   "a zero-slot report cannot claim VERIFIED");

    /* A TRUNCATED count is equally unacceptable, and a non-zero test would miss
     * it: 4 slots all marked VERIFIED says nothing about the other five
     * measured PCRs, so the claim must still be withheld. */
    memset(&a, 0, sizeof(a));
    a.pcr_count = 4u;
    for (i = 0; i < 4u; i++) {
        a.pcrs[i].pcr_index = i;
        a.pcrs[i].status = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    }
    tpm_integrity_test_republish(&a);
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++)
        st[i] = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    tpm_integrity_publish_baseline(BOOT_INTEGRITY_VERIFIED, st, 4u);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.pcr_count, 4u, "control: the base really is truncated");
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                   "a truncated report cannot claim VERIFIED even if all its "
                   "slots verified");

    /* An OVERSIZED producer count must not be laundered into a valid-looking
     * VERIFIED by the publication-boundary clamp: the slot contents surviving
     * truncation is no reason to trust the claim attached to them. */
    memset(&a, 0, sizeof(a));
    a.pcr_count = 255u;
    a.overall_status = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    for (i = 0; i < BOOT_INTEGRITY_MAX_PCRS; i++) {
        a.pcrs[i].pcr_index = (i < 8u) ? i : 11u;
        a.pcrs[i].status = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    }
    tpm_integrity_test_republish(&a);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.pcr_count, (uint8_t)BOOT_INTEGRITY_MAX_PCRS,
                   "the out-of-range count is clamped");
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                   "a clamped count invalidates the VERIFIED claim it carried");

    /* Control: the SAME slot contents with an in-range count DO verify, so the
     * rule above rejects the corrupt count rather than the contents. */
    a.pcr_count = (uint8_t)BOOT_INTEGRITY_MAX_PCRS;
    a.overall_status = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    tpm_integrity_test_republish(&a);
    tpm_integrity_report_copy(&b);
    TEST_ASSERT_EQ(b.overall_status, (uint8_t)BOOT_INTEGRITY_VERIFIED,
                   "control: an in-range count with the same slots verifies");

    tpm_integrity_test_republish(&saved);
}

/* The PURE initial-report builder. tpm_integrity_init() itself is boot
 * infrastructure a test may not call, so the construction RULES live here where
 * they can be exercised directly: no-TPM handling, PCR enumeration and PCR 11
 * placement, NO_CRYPTO seeding, Secure Boot normalization, and measured-set
 * drift in BOTH directions. */
static void test_integrity_build_report(void)
{
    struct boot_integrity_report r;
    uint8_t set[BOOT_INTEGRITY_MAX_PCRS];
    uint8_t canonical[BOOT_INTEGRITY_MAX_PCRS];
    uint8_t n, i;

    n = tpm_pcr_baseline_pcrs(canonical, (uint8_t)BOOT_INTEGRITY_MAX_PCRS);
    TEST_ASSERT_EQ(n, (uint8_t)BOOT_INTEGRITY_MAX_PCRS,
                   "control: the canonical set fits the report exactly");

    /* No TPM, in the PRODUCTION input shape: tpm_integrity_init only enumerates
     * the measured set when a TPM is present, so its no-TPM call reaches the
     * builder with set == NULL and n == 0. Passing a populated set here would
     * have hidden a regression that moved the exact-set guard ahead of the
     * no-TPM branch -- production would report UNKNOWN while the test stayed
     * green on inputs the real caller never supplies. */
    TEST_ASSERT_EQ(tpm_integrity_build_report(&r, 0, 0u, 42u, 1, 1,
                                              (const uint8_t *)0, 0u),
                   1, "no-TPM is a well-formed report on the production inputs");
    TEST_ASSERT_EQ(r.overall_status, (uint8_t)BOOT_INTEGRITY_NO_TPM, "status NO_TPM");
    TEST_ASSERT_EQ(r.pcr_count, 0u, "no-TPM reports no PCRs");
    TEST_ASSERT_EQ(r.event_count, 42u, "event count is carried through");

    /* A stray set with no TPM is still NO_TPM, never a PCR report. */
    TEST_ASSERT_EQ(tpm_integrity_build_report(&r, 0, 0u, 0u, 1, 1, canonical, n),
                   1, "no-TPM ignores a supplied set");
    TEST_ASSERT_EQ(r.overall_status, (uint8_t)BOOT_INTEGRITY_NO_TPM,
                   "no-TPM outranks a valid measured set");
    TEST_ASSERT_EQ(r.pcr_count, 0u, "no-TPM still reports no PCRs");

    /* TPM present with the canonical set: every measured PCR gets a slot, in
     * order, seeded NO_CRYPTO -- and PCR 11 is the one that used not to fit. */
    TEST_ASSERT_EQ(tpm_integrity_build_report(&r, 1, 2u, 7u, 1, 1, canonical, n),
                   1, "the canonical set builds a full report");
    TEST_ASSERT_EQ(r.overall_status, (uint8_t)BOOT_INTEGRITY_NO_CRYPTO,
                   "Phase 0 cannot verify anything yet");
    TEST_ASSERT_EQ(r.pcr_count, n, "every measured PCR is reported");
    TEST_ASSERT_EQ(r.tpm_version, 2u, "TPM version is carried through");
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQ(r.pcrs[i].pcr_index, canonical[i],
                       "slots follow the canonical measured set");
        TEST_ASSERT_EQ(r.pcrs[i].status, (uint8_t)BOOT_INTEGRITY_NO_CRYPTO,
                       "each slot starts NO_CRYPTO");
    }
    TEST_ASSERT_EQ(r.pcrs[n - 1u].pcr_index, 11u, "PCR 11 has a slot");

    /* Secure Boot normalization: an UNREADABLE state never collapses to "off". */
    tpm_integrity_build_report(&r, 1, 2u, 0u, 0, 1, canonical, n);
    TEST_ASSERT_EQ(r.secure_boot_valid, 0u, "unreadable SB state is not valid");
    TEST_ASSERT_EQ(r.secure_boot, 0u, "unreadable SB never reports as enabled");
    tpm_integrity_build_report(&r, 1, 2u, 0u, 1, 0, canonical, n);
    TEST_ASSERT_EQ(r.secure_boot_valid, 1u, "readable SB state is valid");
    TEST_ASSERT_EQ(r.secure_boot, 0u, "readable-and-off reports off");
    tpm_integrity_build_report(&r, 1, 2u, 0u, 1, 1, canonical, n);
    TEST_ASSERT_EQ(r.secure_boot, 1u, "readable-and-on reports on");

    /* Measured-set DRIFT, both directions, each failing closed. */
    memcpy(set, canonical, sizeof(set));
    TEST_ASSERT_EQ(tpm_integrity_build_report(&r, 1, 2u, 0u, 1, 1, set,
                                              (uint8_t)(n - 1u)), 0,
                   "an undersized measured set is refused");
    TEST_ASSERT_EQ(r.pcr_count, 0u, "undersized drift reports no PCRs");
    TEST_ASSERT_EQ(r.overall_status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                   "undersized drift never reads as verified");
    TEST_ASSERT_EQ(tpm_integrity_build_report(&r, 1, 2u, 0u, 1, 1, set,
                                              (uint8_t)(n + 1u)), 0,
                   "an oversized measured set is refused");
    TEST_ASSERT_EQ(r.pcr_count, 0u, "oversized drift reports no PCRs");
    TEST_ASSERT_EQ(r.overall_status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                   "oversized drift never reads as verified");
    TEST_ASSERT_EQ(tpm_integrity_build_report(&r, 1, 2u, 0u, 1, 1,
                                              (const uint8_t *)0, n), 0,
                   "a NULL set with a TPM present is refused");
    TEST_ASSERT_EQ(r.overall_status, (uint8_t)BOOT_INTEGRITY_UNKNOWN,
                   "a NULL set fails closed too");

    /* NULL out is rejected without faulting. */
    TEST_ASSERT_EQ(tpm_integrity_build_report((struct boot_integrity_report *)0,
                                              1, 2u, 0u, 1, 1, canonical, n), 0,
                   "NULL out is refused");
}

/* FIRST-MISMATCH ATTRIBUTION: every MISMATCH branch of the comparison must name
 * a DIFFERENT field, because the whole point is that an operator can tell a
 * firmware update from a Secure Boot toggle from a genuine tamper. A test that
 * only checked "some cause was set" would pass with every branch reporting the
 * same one, which is the state this section exists to fix. */
static void test_baseline_compare_cause(void)
{
    struct tpm_baseline g, c;
    struct tpm_baseline_mismatch m;

    /* Control FIRST: a matching pair must report MATCH and NO cause. Without
     * this, an implementation that reported a cause unconditionally would pass
     * every assertion below. */
    make_golden(&g);
    make_golden(&c);
    memset(&m, 0xEE, sizeof(m));
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MATCH,
                   "control: identical baselines match");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_NONE,
                   "a MATCH clears the cause rather than leaving the poison");
    TEST_ASSERT_EQ((uint32_t)m.pcr_valid, 0u, "a MATCH marks no PCR index valid");
    TEST_ASSERT_EQ((uint32_t)m.pcr_index, 0u, "a MATCH leaves no PCR index behind");

    /* Bank: checked before everything else, since no digest is comparable. */
    make_golden(&c);
    c.alg = TPM_ALG_SHA1;
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MISMATCH,
                   "a different hash bank mismatches");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_BANK,
                   "a bank difference is attributed to the bank");

    /* Secure Boot readability and Secure Boot state are DISTINCT causes: a
     * platform that stopped reporting the state and one that turned it off
     * need different operator actions. */
    make_golden(&c);
    c.secure_boot_valid = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MISMATCH,
                   "Secure Boot becoming unreadable mismatches");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_SB_VALIDITY,
                   "unreadable Secure Boot is attributed to readability");

    make_golden(&c);
    c.secure_boot = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MISMATCH,
                   "Secure Boot turning off mismatches");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_SB_STATE,
                   "a Secure Boot state change is attributed to the state");

    /* Firmware hash: absent and differing are separate causes. */
    make_golden(&c);
    c.fw_hash_present = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MISMATCH,
                   "a vanished firmware hash mismatches");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_FW_HASH_ABSENT,
                   "an absent firmware hash is named as absent, not as differing");

    make_golden(&c);
    memset(c.fw_hash, 0xDD, TPM_BASELINE_DIGEST);
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MISMATCH,
                   "a changed firmware hash mismatches");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_FW_HASH,
                   "a firmware update is attributed to the firmware hash");

    /* ABI manifest: presence disagreement, then content. */
    make_golden(&c);
    c.abi_manifest_present = 1u;
    memset(c.abi_manifest, 0x11, TPM_BASELINE_DIGEST);
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MISMATCH,
                   "an ABI-manifest presence disagreement mismatches");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_ABI_PRESENCE,
                   "presence disagreement is attributed to presence");

    make_golden(&g);
    g.abi_manifest_present = 1u;
    memset(g.abi_manifest, 0x11, TPM_BASELINE_DIGEST);
    make_golden(&c);
    c.abi_manifest_present = 1u;
    memset(c.abi_manifest, 0x22, TPM_BASELINE_DIGEST);
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MISMATCH,
                   "a differing ABI manifest mismatches");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_ABI_CONTENT,
                   "a differing ABI manifest is attributed to its content");

    /* A golden pinning no PCR at all. */
    make_golden(&g);
    make_golden(&c);
    {
        uint32_t i;
        for (i = 0; i < TPM_BASELINE_MAX_PCRS; i++)
            g.pcrs[i].present = 0u;
    }
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MISMATCH,
                   "a golden pinning no PCR cannot match");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_NO_PCR_PINNED,
                   "an unpinned golden is named as such, not as a PCR difference");
    TEST_ASSERT_EQ((uint32_t)m.pcr_valid, 0u,
                   "no-PCR-pinned carries no PCR index (there is no culprit slot)");

    /* A PCR whose digest moved: the index is part of the attribution. Slot 2
     * holds PCR 2 in the canonical set, so the reported index proves the
     * culprit slot is identified rather than defaulted. */
    make_golden(&g);
    make_golden(&c);
    memset(c.pcrs[2].digest, 0x5A, TPM_BASELINE_DIGEST);
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MISMATCH,
                   "a changed PCR digest mismatches");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_PCR_DIGEST,
                   "a changed digest is attributed to the PCR digest");
    TEST_ASSERT_EQ((uint32_t)m.pcr_valid, 1u, "a PCR cause marks its index valid");
    TEST_ASSERT_EQ((uint32_t)m.pcr_index, (uint32_t)k_baseline_pcrs[2],
                   "the reported PCR index is the culprit slot, not slot 0");

    /* A pinned PCR that is no longer readable. Slot 4 -> PCR 4. */
    make_golden(&c);
    c.pcrs[4].present = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m), (int)TPM_BASELINE_MISMATCH,
                   "a pinned PCR that vanished mismatches");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_PCR_ABSENT,
                   "a vanished PCR is named absent, not as a digest difference");
    TEST_ASSERT_EQ((uint32_t)m.pcr_index, (uint32_t)k_baseline_pcrs[4],
                   "the absent PCR reports its own index");

    /* Precedence: when a scalar AND a PCR both moved, the scalar wins, because
     * it is compared first. This is the documented first-mismatch contract, and
     * asserting it is what stops a later reorder changing the reported cause
     * silently. */
    make_golden(&c);
    c.secure_boot = 0u;
    memset(c.pcrs[2].digest, 0x5A, TPM_BASELINE_DIGEST);
    TEST_ASSERT_EQ((uint32_t)tpm_baseline_compare_detail(&g, &c, &m),
                   (uint32_t)TPM_BASELINE_MISMATCH,
                   "a simultaneous scalar+PCR change mismatches");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_SB_STATE,
                   "the earlier scalar cause wins over a later PCR difference");

    /* BADARG is attributed too, so a caller cannot read NONE beside a
     * non-verdict return. */
    memset(&m, 0xEE, sizeof(m));
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail((const struct tpm_baseline *)0, &c, &m),
                   (int)TPM_BASELINE_CMP_BADARG, "NULL golden is BADARG");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_BADARG,
                   "BADARG is attributed rather than left as poison");

    /* An oversized pcr_count is the other BADARG route. */
    make_golden(&c);
    c.pcr_count = (uint8_t)(TPM_BASELINE_MAX_PCRS + 1u);
    TEST_ASSERT_EQ((int)tpm_baseline_compare_detail(&g, &c, &m),
                   (int)TPM_BASELINE_CMP_BADARG, "an oversized pcr_count is BADARG");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_BADARG,
                   "the oversized-count BADARG is attributed too");

    /* Declining the detail is legal and must not fault, and the wrapper must
     * agree with the detail function on the verdict. */
    make_golden(&c);
    c.secure_boot = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c),
                   (int)tpm_baseline_compare_detail(&g, &c,
                                                    (struct tpm_baseline_mismatch *)0),
                   "the wrapper and the detail function return the same verdict");
}

/* Every cause renders a DISTINCT name, and an out-of-range value renders
 * "unknown" rather than reading past a table. Distinctness is the property that
 * matters: identical strings would make the attribution useless while every
 * per-branch test above still passed. */
static void test_baseline_cause_label(void)
{
    const char *names[12];
    uint32_t i, j;

    for (i = 0; i < 12u; i++) {
        names[i] = tpm_baseline_cause_label((uint8_t)i);
        TEST_ASSERT(names[i] != (const char *)0, "every in-range cause has a name");
    }
    for (i = 0; i < 12u; i++) {
        for (j = i + 1u; j < 12u; j++) {
            TEST_ASSERT(strcmp(names[i], names[j]) != 0,
                        "no two causes share a name");
        }
    }
    TEST_ASSERT(strcmp(tpm_baseline_cause_label((uint8_t)TPM_BASELINE_CAUSE_NONE),
                       "none") == 0, "the no-cause value renders none");
    TEST_ASSERT(strcmp(tpm_baseline_cause_label((uint8_t)TPM_BASELINE_CAUSE_SB_STATE),
                       "secure-boot-state") == 0, "the Secure Boot state name is stable");
    TEST_ASSERT(strcmp(tpm_baseline_cause_label(12u), "unknown") == 0,
                "the first out-of-range value renders unknown");
    TEST_ASSERT(strcmp(tpm_baseline_cause_label(255u), "unknown") == 0,
                "a far out-of-range value renders unknown");
}

/* The verify WRAPPER's fail-closed attribution contract, on the path this suite
 * can reach without a full NV-blob fake. No transport means no comparison ever
 * runs, so a caller reusing its struct across boots must read NONE and not the
 * previous call's cause -- which is why the struct is POISONED first. A wrapper
 * that simply never touched the out-param would otherwise pass. */
static void test_baseline_verify_detail_no_transport(void)
{
    struct tpm_t_test_state prev;
    struct tpm_baseline_mismatch m;
    uint8_t overall = 0xEEu;
    uint8_t pcr_status[TPM_BASELINE_MAX_PCRS];
    uint8_t pcr_n = 0xEEu;
    tpm_baseline_status_t st;

    memset(pcr_status, 0xEE, sizeof(pcr_status));
    memset(&m, 0xEE, sizeof(m));
    prev = tpm_t_test_install((const struct tpm_t_io *)0, TPM_T_IFACE_NONE, 0);

    st = tpm_baseline_verify_detail(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256, &overall,
                                    pcr_status, (uint8_t)TPM_BASELINE_MAX_PCRS,
                                    &pcr_n, &m);

    TEST_ASSERT_EQ((int)st, (int)TPM_BASELINE_NO_TPM,
                   "no transport still verifies to NO_TPM through the detail wrapper");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_NONE,
                   "a path that never compared reports NO cause, not stale poison");
    TEST_ASSERT_EQ((uint32_t)m.pcr_valid, 0u,
                   "a path that never compared marks no PCR index valid");
    TEST_ASSERT_EQ((uint32_t)m.pcr_index, 0u,
                   "a path that never compared leaves no PCR index behind");
    TEST_ASSERT_EQ(pcr_n, 0u,
                   "the detail wrapper keeps the not-evaluated per-PCR contract");
    TEST_ASSERT_EQ(overall, 0xEEu,
                   "the detail wrapper still publishes no verdict on a non-verdict return");

    /* A NULL cause pointer is "no detail wanted" and must not fault. */
    st = tpm_baseline_verify_detail(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256,
                                    (uint8_t *)0, (uint8_t *)0, 0u, (uint8_t *)0,
                                    (struct tpm_baseline_mismatch *)0);
    TEST_ASSERT_EQ((int)st, (int)TPM_BASELINE_NO_TPM,
                   "a NULL cause pointer is accepted on the no-transport path");

    /* The UNDERSIZED-buffer refusal is a SECOND, earlier return, and it is the
     * one that would go quiet: it sits BELOW the cause reset today, so moving
     * the reset under it would let a reused struct carry a previous call's PCR
     * cause beside BADARG while every other assertion here stayed green. Poison
     * everything and prove the whole out-param set is either written or left
     * alone, as each one's contract says. */
    memset(&m, 0xEE, sizeof(m));
    memset(pcr_status, 0xEE, sizeof(pcr_status));
    overall = 0xEEu;
    pcr_n = 0xEEu;
    st = tpm_baseline_verify_detail(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256, &overall,
                                    pcr_status,
                                    (uint8_t)(TPM_BASELINE_MAX_PCRS - 1u), &pcr_n, &m);
    TEST_ASSERT_EQ((int)st, (int)TPM_BASELINE_BADARG,
                   "a buffer too small for the measured set is refused outright");
    TEST_ASSERT_EQ((uint32_t)m.cause, (uint32_t)TPM_BASELINE_CAUSE_NONE,
                   "the undersized refusal still clears the cause");
    TEST_ASSERT_EQ((uint32_t)m.pcr_valid, 0u,
                   "the undersized refusal marks no PCR index valid");
    TEST_ASSERT_EQ((uint32_t)m.pcr_index, 0u,
                   "the undersized refusal leaves no PCR index behind");
    TEST_ASSERT_EQ((uint32_t)m.pad, 0u,
                   "the undersized refusal zeroes the padding byte too");
    TEST_ASSERT_EQ(pcr_n, 0u,
                   "the undersized refusal reports nothing evaluated");
    TEST_ASSERT_EQ(overall, 0xEEu,
                   "a BADARG return publishes no verdict");
    TEST_ASSERT_EQ(pcr_status[0], 0xEEu,
                   "a refused call writes no per-PCR status");

    tpm_t_test_restore(prev);
}

/* The shared status-to-guidance helper. Two properties matter and neither is
 * the wording: EVERY failure status that can publish MISMATCH must have a line
 * (a status without one reaches the operator as a bare number, which is what
 * the enrollment path used to do), and the CORRUPT and SELF_CORRUPT lines must
 * stay DISTINCT, because they route to opposite recovery paths -- bad stored
 * bytes against an untrustworthy kernel -- even though enrollment refuses both
 * today. */
static void test_baseline_status_repair(void)
{
    static const tpm_baseline_status_t failures[6] = {
        TPM_BASELINE_TORN, TPM_BASELINE_RELABELED, TPM_BASELINE_UNBOUND,
        TPM_BASELINE_IDENTITY, TPM_BASELINE_CORRUPT, TPM_BASELINE_SELF_CORRUPT
    };
    const char *texts[6];
    uint32_t i, j;

    for (i = 0; i < 6u; i++) {
        texts[i] = tpm_baseline_status_repair((uint8_t)failures[i]);
        TEST_ASSERT(texts[i] != (const char *)0,
                    "every MISMATCH-publishing failure status has guidance");
        TEST_ASSERT(tpm_baseline_status_is_failure(failures[i]) != 0,
                    "control: each of these really is a failure status");
    }
    for (i = 0; i < 6u; i++)
        for (j = i + 1u; j < 6u; j++)
            TEST_ASSERT(strcmp(texts[i], texts[j]) != 0,
                        "no two failure statuses share guidance");

    /* The two that must not converge. SELF_CORRUPT must warn the operator off
     * looking for a way past the refusal; CORRUPT must not promise that
     * re-enrolling repairs it, because the enroll path validates the readback
     * first and refuses on the same bytes. */
    TEST_ASSERT(strstr(tpm_baseline_status_repair((uint8_t)TPM_BASELINE_SELF_CORRUPT),
                       "do NOT") != (char *)0,
                "a corrupt kernel identity forbids enrolling past the refusal");
    TEST_ASSERT(strstr(tpm_baseline_status_repair((uint8_t)TPM_BASELINE_SELF_CORRUPT),
                       "reinstall the kernel") != (char *)0,
                "and names the one action that actually resolves it");
    TEST_ASSERT(strstr(tpm_baseline_status_repair((uint8_t)TPM_BASELINE_CORRUPT),
                       "REFUSE") != (char *)0,
                "a corrupt stored blob warns that re-enrolling refuses");

    /* Non-failure statuses have nothing to advise, and must say so with NULL
     * rather than a plausible-looking line. */
    TEST_ASSERT(tpm_baseline_status_repair((uint8_t)TPM_BASELINE_OK)
                == (const char *)0, "OK has no repair guidance");
    TEST_ASSERT(tpm_baseline_status_repair((uint8_t)TPM_BASELINE_NO_BASELINE)
                == (const char *)0, "an unenrolled machine has nothing to repair");
    TEST_ASSERT(tpm_baseline_status_repair((uint8_t)TPM_BASELINE_NO_TPM)
                == (const char *)0, "a machine that could not measure has nothing to repair");
    TEST_ASSERT(tpm_baseline_status_repair(200u) == (const char *)0,
                "an out-of-range status yields no guidance rather than a stray pointer");

    /* Both call sites render these into a klog entry that TRUNCATES at
     * message[256], and the enrollment path wraps the longest prefix around
     * them. A truncated safety instruction is worse than none -- "do NOT" can
     * be exactly what gets cut -- so the composite is bounded here. */
    for (i = 0; i < 6u; i++) {
        const uint32_t rendered =
            (uint32_t)strlen("Baseline enroll integrity failure (status 99): "
                             "not a clean first install -- ") +
            (uint32_t)strlen(texts[i]);
        TEST_ASSERT(rendered < (uint32_t)sizeof(((klog_entry_t *)0)->message),
                    "the longest rendered enroll-failure line fits a klog entry");
    }
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
    test_suite_register_cat("tpm: baseline per-PCR compare detail",
                            test_baseline_compare_pcrs, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline verify per-PCR out-params (no transport)",
                            test_baseline_verify_no_transport, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline first-mismatch attribution",
                            test_baseline_compare_cause, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline mismatch cause labels",
                            test_baseline_cause_label, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline failure-status repair guidance",
                            test_baseline_status_repair, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline verify cause fail-closed (no transport)",
                            test_baseline_verify_detail_no_transport, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: boot-integrity report publication",
                            test_integrity_publication, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: boot-integrity initial-report builder",
                            test_integrity_build_report, TEST_CAT_SECURITY);
}
