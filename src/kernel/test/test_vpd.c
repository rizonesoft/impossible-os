/* Unit tests for the VPD (Visual POST Display) data model -- TODO-15.
 *
 * VPD is a visual/boot-level subsystem: vpd_init / vpd_stage_begin / done /
 * fail write directly to the GOP framebuffer and are forbidden in tests
 * (live boot infrastructure). The pure, side-effect-free surface is the
 * POST16-code -> stage-name lookup table behind vpd_post16_name(), which is
 * what these tests cover. Pixel output is validated via scripts/test-smoke.sh
 * serial matching + manual inspection, per the section Unit Tests note. */

#include "kernel/test/test.h"
#include "kernel/vpd.h"
#include "libc/string.h"

/* Known POST16 codes resolve to their stage names (table in vpd.c). */
static void test_vpd_post16_name_known(void)
{
    TEST_ASSERT(strcmp(vpd_post16_name(0x0020), "PMM") == 0,
                "0x0020 -> PMM");
    TEST_ASSERT(strcmp(vpd_post16_name(0x0030), "VMM") == 0,
                "0x0030 -> VMM");
    TEST_ASSERT(strcmp(vpd_post16_name(0x0018), "TPM") == 0,
                "0x0018 -> TPM");
    TEST_ASSERT(strcmp(vpd_post16_name(0xB001), "BL_ENTRY") == 0,
                "0xB001 -> BL_ENTRY (bootloader range)");
    TEST_ASSERT(strcmp(vpd_post16_name(0xB050), "BL_EXIT_BS") == 0,
                "0xB050 -> BL_EXIT_BS");

    /* Bootloader milestones that emit real POST writes must resolve to their
     * stage name, not UNKNOWN, so the crash banner stays actionable. These
     * cover the entropy / UKI-detect / boot-var-ext / boot-policy / counter /
     * menu / kind-validate ranges the bootloader emits (bootx64.c). */
    TEST_ASSERT(strcmp(vpd_post16_name(0xB034), "BL_ENTROPY") == 0,
                "0xB034 -> BL_ENTROPY");
    TEST_ASSERT(strcmp(vpd_post16_name(0xB0A0), "BL_UKI_DETECT") == 0,
                "0xB0A0 -> BL_UKI_DETECT");
    TEST_ASSERT(strcmp(vpd_post16_name(0xB0A2), "BL_BOOT_VAR_EXT") == 0,
                "0xB0A2 -> BL_BOOT_VAR_EXT");
    TEST_ASSERT(strcmp(vpd_post16_name(0xB0B0), "BL_BOOT_POLICY") == 0,
                "0xB0B0 -> BL_BOOT_POLICY");
    TEST_ASSERT(strcmp(vpd_post16_name(0xB0B2), "BL_BOOT_POLICY_DECIDE") == 0,
                "0xB0B2 -> BL_BOOT_POLICY_DECIDE");
    TEST_ASSERT(strcmp(vpd_post16_name(0xB0B6), "BL_MENU") == 0,
                "0xB0B6 -> BL_MENU");
    TEST_ASSERT(strcmp(vpd_post16_name(0xB0B8), "BL_KIND_VALIDATE_OK") == 0,
                "0xB0B8 -> BL_KIND_VALIDATE_OK");

    /* Phase-2 OB + Executive support runtime markers must resolve so a crash
     * in those windows names the stage instead of UNKNOWN (TODO-06 S1). */
    TEST_ASSERT(strcmp(vpd_post16_name(0x20B0), "OB") == 0,
                "0x20B0 -> OB");
    TEST_ASSERT(strcmp(vpd_post16_name(0x20C0), "EX") == 0,
                "0x20C0 -> EX");
    TEST_ASSERT(strcmp(vpd_post16_name(0x20C1), "EX") == 0,
                "0x20C1 -> EX (exit marker)");
}

/* Unmapped codes fall through to "UNKNOWN"; the lookup never returns NULL,
 * so a banner draw for any stored NVRAM code cannot deref a null name. */
static void test_vpd_post16_name_unmapped(void)
{
    TEST_ASSERT(strcmp(vpd_post16_name(0xFFFF), "UNKNOWN") == 0,
                "0xFFFF (unmapped high) -> UNKNOWN");
    TEST_ASSERT(strcmp(vpd_post16_name(0x0000), "UNKNOWN") == 0,
                "0x0000 (unmapped low) -> UNKNOWN");
    TEST_ASSERT(strcmp(vpd_post16_name(0x7FFF), "UNKNOWN") == 0,
                "0x7FFF (unmapped mid) -> UNKNOWN");

    /* Full 16-bit sweep: the fallback guarantees a non-NULL, non-empty
     * string for every possible code (no crash, no null deref). */
    uint32_t c;
    int all_valid = 1;
    for (c = 0; c <= 0xFFFFu; c++) {
        const char *n = vpd_post16_name((uint16_t)c);
        if (n == (const char *)0 || n[0] == '\0') { all_valid = 0; break; }
    }
    TEST_ASSERT(all_valid, "every 16-bit code returns a non-NULL non-empty name");
}

void test_register_vpd(void)
{
    test_suite_register_cat("vpd: post16 name lookup (known codes)",
                            test_vpd_post16_name_known, TEST_CAT_BOOT);
    test_suite_register_cat("vpd: post16 name lookup (unmapped + sweep)",
                            test_vpd_post16_name_unmapped, TEST_CAT_BOOT);
}
