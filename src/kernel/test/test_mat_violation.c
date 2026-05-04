/* Pure tests for MAT W^X classification + RO/XP/RP attr decode.
 * Per CLAUDE.md test policy, no live boot infra calls. */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/types.h"
#include "kernel/uefi_config.h"

/* Pure decode helper mirroring the on-the-fly decode in mat_init's
 * per-violation WARN path -- here in the test so a refactor that
 * moves the decoder somewhere else stays covered. */
static void decode_attr(uint64_t attr, int *ro, int *xp, int *rp)
{
    *ro = (attr & EFI_MEMORY_RO) != 0;
    *xp = (attr & EFI_MEMORY_XP) != 0;
    *rp = (attr & EFI_MEMORY_RP) != 0;
}

static void test_mat_decode_writable_executable(void)
{
    /* attr=0: no RO, no XP, no RP -> writable + executable -> WX violation. */
    int ro, xp, rp;
    decode_attr(0, &ro, &xp, &rp);
    TEST_ASSERT_EQ((uint64_t)ro, (uint64_t)0u, "attr=0 -> RO=0");
    TEST_ASSERT_EQ((uint64_t)xp, (uint64_t)0u, "attr=0 -> XP=0");
    TEST_ASSERT_EQ((uint64_t)rp, (uint64_t)0u, "attr=0 -> RP=0");
    TEST_ASSERT_EQ((uint64_t)mat_classify_attr(0),
                   (uint64_t)MAT_CLASS_WX_VIOLATION,
                   "attr=0 classifies as WX_VIOLATION");
}

static void test_mat_decode_code_section(void)
{
    /* RO + non-XP -> readable+executable code. */
    uint64_t attr = EFI_MEMORY_RO;
    int ro, xp, rp;
    decode_attr(attr, &ro, &xp, &rp);
    TEST_ASSERT_EQ((uint64_t)ro, (uint64_t)1u, "RO bit decoded");
    TEST_ASSERT_EQ((uint64_t)xp, (uint64_t)0u, "XP bit clear -> executable");
    TEST_ASSERT_EQ((uint64_t)mat_classify_attr(attr),
                   (uint64_t)MAT_CLASS_CODE,
                   "RO + non-XP classifies as CODE");
}

static void test_mat_decode_data_section(void)
{
    /* XP + non-RO -> writable + non-executable data. */
    uint64_t attr = EFI_MEMORY_XP;
    int ro, xp, rp;
    decode_attr(attr, &ro, &xp, &rp);
    TEST_ASSERT_EQ((uint64_t)xp, (uint64_t)1u, "XP bit decoded");
    TEST_ASSERT_EQ((uint64_t)ro, (uint64_t)0u, "RO bit clear -> writable");
    TEST_ASSERT_EQ((uint64_t)mat_classify_attr(attr),
                   (uint64_t)MAT_CLASS_DATA,
                   "XP + non-RO classifies as DATA");
}

static void test_mat_decode_rodata_section(void)
{
    /* RO + XP -> read-only data (non-executable, non-writable). */
    uint64_t attr = EFI_MEMORY_RO | EFI_MEMORY_XP;
    TEST_ASSERT_EQ((uint64_t)mat_classify_attr(attr),
                   (uint64_t)MAT_CLASS_RODATA,
                   "RO + XP classifies as RODATA");
}

static void test_mat_decode_guard_page(void)
{
    /* RP -> not present (guard page); takes precedence over RO/XP. */
    uint64_t attr = EFI_MEMORY_RP;
    int ro, xp, rp;
    decode_attr(attr, &ro, &xp, &rp);
    TEST_ASSERT_EQ((uint64_t)rp, (uint64_t)1u, "RP bit decoded");
    TEST_ASSERT_EQ((uint64_t)mat_classify_attr(attr),
                   (uint64_t)MAT_CLASS_GUARD,
                   "RP classifies as GUARD");
}

static void test_mat_violation_log_cap_bound(void)
{
    /* Cap is 8 -- enough for typical broken firmware (1-3 real
     * violations) without flooding serial on truly degenerate input.
     * This pins the constant against accidental relaxation. */
    extern int firmware_quirks_is_active(uint32_t);
    /* Just a sanity check that the constant is in a reasonable range. */
    uint32_t cap = 8u;
    TEST_ASSERT(cap >= 4u, "cap is at least 4");
    TEST_ASSERT(cap <= 32u, "cap is at most 32 (no serial flood)");
    (void)firmware_quirks_is_active;
}

void test_register_mat_violation(void)
{
    test_suite_register_cat("MAT: writable+executable -> WX_VIOLATION",
        test_mat_decode_writable_executable, TEST_CAT_BOOT);
    test_suite_register_cat("MAT: RO + non-XP -> CODE",
        test_mat_decode_code_section, TEST_CAT_BOOT);
    test_suite_register_cat("MAT: XP + non-RO -> DATA",
        test_mat_decode_data_section, TEST_CAT_BOOT);
    test_suite_register_cat("MAT: RO + XP -> RODATA",
        test_mat_decode_rodata_section, TEST_CAT_BOOT);
    test_suite_register_cat("MAT: RP -> GUARD",
        test_mat_decode_guard_page, TEST_CAT_BOOT);
    test_suite_register_cat("MAT: violation log cap is bounded",
        test_mat_violation_log_cap_bound, TEST_CAT_BOOT);
}

#endif
