/* test_tpm_pcr_alloc.c -- unit tests for the PCR allocation table + derived
 * policy masks. Pure data + pure derivation, so these assert the table content
 * and the mask membership directly. No live boot infrastructure (test policy).
 *
 * XREF: 01-boot-platform/TODO-13-tpm-measured-boot-attestation.md "PCR
 * Allocation and Policy Mask Table".
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/tpm_pcr_alloc.h"

#define BIT(n) (1u << (n))

static void test_pcr_owner_layers(void)
{
    TEST_ASSERT_EQ(tpm_pcr_owner(0), TPM_PCR_LAYER_FIRMWARE, "PCR 0 = firmware");
    TEST_ASSERT_EQ(tpm_pcr_owner(4), TPM_PCR_LAYER_BOOTLOADER, "PCR 4 = bootloader/IPL");
    TEST_ASSERT_EQ(tpm_pcr_owner(7), TPM_PCR_LAYER_SECUREBOOT, "PCR 7 = Secure Boot");
    /* PCR 11 has two events; tpm_pcr_owner returns the lowest-ordering layer
     * (UKI kernel-boot, ordering 0). */
    TEST_ASSERT_EQ(tpm_pcr_owner(11), TPM_PCR_LAYER_UKI_KERNEL,
                   "PCR 11 owner = UKI kernel-boot (lowest ordering)");
    /* An unallocated PCR has no owner. */
    TEST_ASSERT_EQ(tpm_pcr_owner(9), -1, "PCR 9 has no allocated owner");
    TEST_ASSERT_EQ(tpm_pcr_owner(99), -1, "out-of-range PCR -> -1");
}

static void test_pcr11_multiple_events(void)
{
    /* The table MUST carry both PCR-11 events so a manifest-only change is
     * distinguishable from a UKI kernel-boot change. */
    uint32_t n = tpm_pcr_alloc_count();
    int saw_uki = 0, saw_manifest = 0;
    for (uint32_t i = 0; i < n; i++) {
        const struct tpm_pcr_event_alloc *e = tpm_pcr_alloc_get(i);
        if (!e || e->pcr != TPM_PCR_MANIFEST_INDEX)
            continue;
        if (e->layer == TPM_PCR_LAYER_UKI_KERNEL)  saw_uki = 1;
        if (e->layer == TPM_PCR_LAYER_KERNEL_ABI)  saw_manifest = 1;
    }
    TEST_ASSERT_EQ(saw_uki, 1, "PCR 11 carries a UKI kernel-boot event");
    TEST_ASSERT_EQ(saw_manifest, 1, "PCR 11 carries a kernel-ABI-manifest event");
    TEST_ASSERT_EQ(tpm_pcr_alloc_get(tpm_pcr_alloc_count()), (const struct tpm_pcr_event_alloc *)0,
                   "out-of-range alloc index -> NULL");
}

static void test_pcr_masks(void)
{
    uint32_t seal = tpm_pcr_seal_mask();
    uint32_t quote = tpm_pcr_quote_mask();
    uint32_t baseline = tpm_pcr_baseline_mask();

    /* SEAL default = PCR 7 only. The load-bearing negative assertion: PCR 11 is
     * NOT in the seal default, so a kernel-ABI-manifest change (kernel rebuild)
     * never breaks FDE unseal. */
    TEST_ASSERT_EQ(seal, BIT(7), "seal default mask = PCR 7 only");
    TEST_ASSERT_EQ(seal & BIT(11), 0u, "PCR 11 excluded from the seal default");

    /* QUOTE + BASELINE = PCR 0-7 + PCR 11. */
    uint32_t expect = BIT(0)|BIT(1)|BIT(2)|BIT(3)|BIT(4)|BIT(5)|BIT(6)|BIT(7)|BIT(11);
    TEST_ASSERT_EQ(quote, expect, "quote mask = PCR 0-7 + 11");
    TEST_ASSERT_EQ(baseline, expect, "baseline mask = PCR 0-7 + 11");
    TEST_ASSERT_EQ(baseline & BIT(11), BIT(11), "PCR 11 IS in the baseline mask");

    /* Every mask bit must correspond to a PCR with an allocated owner -- no mask
     * references an ownerless PCR. */
    uint32_t all = seal | quote | baseline;
    for (uint32_t pcr = 0; pcr < 24u; pcr++) {
        if (all & BIT(pcr))
            TEST_ASSERT(tpm_pcr_owner(pcr) >= 0, "masked PCR has an allocated owner");
    }
}

/* The canonical measured-boot PCR list the baseline / replay / PCR-cache consumers
 * derive instead of each hard-coding {0-7,11}. Pins the exact set + order. */
static void test_pcr_baseline_list(void)
{
    static const uint8_t expect[] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 11u };
    uint8_t pcrs[24];
    uint8_t n = tpm_pcr_baseline_pcrs(pcrs, sizeof pcrs);
    uint8_t i;
    uint32_t m = 0;

    TEST_ASSERT_EQ((uint32_t)n, 9u, "baseline PCR list has 9 entries (0-7 + 11)");
    for (i = 0; i < n && i < (uint8_t)(sizeof expect); i++) {
        TEST_ASSERT_EQ((uint32_t)pcrs[i], (uint32_t)expect[i], "baseline PCR list ascending {0-7,11}");
        m |= (1u << pcrs[i]);
    }
    TEST_ASSERT_EQ(m, tpm_pcr_baseline_mask(), "enumerated list bits == baseline mask");

    /* Overflow is detectable: a too-small buffer writes only `cap` entries but
     * returns the FULL count (> cap), so a caller can fail closed instead of
     * silently dropping PCRs. */
    uint8_t small[3];
    uint8_t ns = tpm_pcr_baseline_pcrs(small, sizeof small);
    TEST_ASSERT_EQ((uint32_t)ns, 9u, "returns FULL count even when truncated (overflow detectable)");
    TEST_ASSERT(small[0] == 0u && small[1] == 1u && small[2] == 2u, "wrote the cap-bounded prefix");
}

void test_register_tpm_pcr_alloc(void)
{
    test_suite_register_cat("tpm: PCR owner layers", test_pcr_owner_layers, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PCR 11 multi-event", test_pcr11_multiple_events, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PCR policy masks", test_pcr_masks, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PCR baseline list", test_pcr_baseline_list, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
