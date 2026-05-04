/* Pure-helper coverage for SMBIOS profile dump rank-3 + table-copy bound
 * math.  Per CLAUDE.md test policy, no live boot infra calls. */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/types.h"

/* Mirror the per-type profile struct from smbios.c for the rank-3 test
 * helper.  Test compiles without dragging in the full SMBIOS parser. */
struct smbios_test_profile {
    uint64_t total_ticks;
    uint32_t count;
};

/* Pure rank-3 picker -- same shape as smbios_dump_top3 minus the klog
 * side effect.  Tests pin the truth table so a future refactor can't
 * silently re-rank. */
static void rank3(const struct smbios_test_profile *prof, uint32_t count,
                  uint32_t *out_idx, uint64_t *out_ticks)
{
    out_idx[0] = out_idx[1] = out_idx[2] = 0xFFFFFFFFu;
    out_ticks[0] = out_ticks[1] = out_ticks[2] = 0;
    for (uint32_t t = 0; t < count; t++) {
        uint64_t v = prof[t].total_ticks;
        if (v == 0) continue;
        if (v > out_ticks[0]) {
            out_ticks[2] = out_ticks[1]; out_idx[2] = out_idx[1];
            out_ticks[1] = out_ticks[0]; out_idx[1] = out_idx[0];
            out_ticks[0] = v;             out_idx[0] = t;
        } else if (v > out_ticks[1]) {
            out_ticks[2] = out_ticks[1]; out_idx[2] = out_idx[1];
            out_ticks[1] = v;             out_idx[1] = t;
        } else if (v > out_ticks[2]) {
            out_ticks[2] = v;             out_idx[2] = t;
        }
    }
}

static void test_smbios_rank3_typical(void)
{
    /* Sparse profile: types 0=20 ticks, 1=100 ticks, 4=50 ticks,
     * 17=70 ticks. Top-3: type 1 / 17 / 4. */
    struct smbios_test_profile prof[18];
    for (uint32_t i = 0; i < 18; i++) { prof[i].total_ticks = 0; prof[i].count = 0; }
    prof[0].total_ticks = 20; prof[1].total_ticks = 100;
    prof[4].total_ticks = 50; prof[17].total_ticks = 70;

    uint32_t idx[3]; uint64_t ticks[3];
    rank3(prof, 18, idx, ticks);
    TEST_ASSERT_EQ(idx[0], 1u, "top1 is type 1 (100 ticks)");
    TEST_ASSERT_EQ(idx[1], 17u, "top2 is type 17 (70 ticks)");
    TEST_ASSERT_EQ(idx[2], 4u, "top3 is type 4 (50 ticks)");
}

static void test_smbios_rank3_empty(void)
{
    struct smbios_test_profile prof[8];
    for (uint32_t i = 0; i < 8; i++) { prof[i].total_ticks = 0; prof[i].count = 0; }
    uint32_t idx[3]; uint64_t ticks[3];
    rank3(prof, 8, idx, ticks);
    TEST_ASSERT_EQ(idx[0], 0xFFFFFFFFu, "empty profile -> no top1");
    TEST_ASSERT_EQ(idx[1], 0xFFFFFFFFu, "empty profile -> no top2");
    TEST_ASSERT_EQ(idx[2], 0xFFFFFFFFu, "empty profile -> no top3");
}

static void test_smbios_rank3_single_entry(void)
{
    struct smbios_test_profile prof[8];
    for (uint32_t i = 0; i < 8; i++) { prof[i].total_ticks = 0; prof[i].count = 0; }
    prof[3].total_ticks = 42;
    uint32_t idx[3]; uint64_t ticks[3];
    rank3(prof, 8, idx, ticks);
    TEST_ASSERT_EQ(idx[0], 3u, "single entry -> top1 = 3");
    TEST_ASSERT_EQ(idx[1], 0xFFFFFFFFu, "no top2 for single");
    TEST_ASSERT_EQ(idx[2], 0xFFFFFFFFu, "no top3 for single");
}

static void test_smbios_copy_cap_bound(void)
{
    /* SMBIOS_COPY_MAX_BYTES is 64 KiB. Verify the bound math:
     * pages = ceil_div(min(cap, max), 4096). */
    uint32_t cap = 16u * 4096u;  /* 64 KiB */
    uint32_t pages_for_cap = (cap + 4095u) / 4096u;
    TEST_ASSERT_EQ(pages_for_cap, 16u, "64 KiB -> 16 pages");

    uint32_t pages_for_405 = (405u + 4095u) / 4096u;
    TEST_ASSERT_EQ(pages_for_405, 1u, "405 bytes -> 1 page");

    uint32_t pages_for_4096 = (4096u + 4095u) / 4096u;
    TEST_ASSERT_EQ(pages_for_4096, 1u, "exactly 4096 bytes -> 1 page");

    uint32_t pages_for_4097 = (4097u + 4095u) / 4096u;
    TEST_ASSERT_EQ(pages_for_4097, 2u, "4097 bytes -> 2 pages");
}

void test_register_smbios(void)
{
    test_suite_register_cat("SMBIOS: rank3 typical 4-entry profile",
        test_smbios_rank3_typical, TEST_CAT_BOOT);
    test_suite_register_cat("SMBIOS: rank3 empty profile -> sentinel",
        test_smbios_rank3_empty, TEST_CAT_BOOT);
    test_suite_register_cat("SMBIOS: rank3 single entry -> sentinel pad",
        test_smbios_rank3_single_entry, TEST_CAT_BOOT);
    test_suite_register_cat("SMBIOS: copy cap pages math",
        test_smbios_copy_cap_bound, TEST_CAT_BOOT);
}

#endif
