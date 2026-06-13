/* test_boot_seed.c -- boot_seed.c credit trust-boundary suites.
 *
 * boot_seed.c's pure helpers (boot_seed_desc_classify, boot_seed_release_payload,
 * the entropy_seed_parse routing) are exercised from test_entropy.c alongside the
 * entropy model. This file owns the tests that bind boot_seed.c's recording trust
 * boundary directly -- the gate that decides which parsed sources earn entropy
 * credit. Pure helpers + the documented global-record snapshot/restore pattern
 * only; no live boot infrastructure (test policy).
 */

#include "kernel/test/test.h"
#include "kernel/entropy.h"
#include "libc/string.h"

static void test_boot_seed_record_sources_gate(void)
{
    /* boot_seed_record_sources is the production credit trust boundary: it
     * records ONLY sources in records_mask (re-derived from accepted records)
     * and never the advisory hdr_mask. Bind it directly so a regression that
     * credited the all-HIGH advisory header on a rejected/cloned carryover is
     * caught. The global record is plain in-memory state: snapshot, run on a
     * clean slate, restore (the test_entropy_record_clamp pattern). */
    uint32_t saved_mask = entropy_source_mask();
    uint32_t saved_q = entropy_source_quality();
    struct entropy_seed_parse_result r;
    uint32_t src;

    for (src = 0; src < (uint32_t)ENTROPY_SRC_COUNT; src++)
        entropy_record_source((entropy_src_t)src, ENTROPY_Q_NONE);

    /* Cloned-only: no accepted records, adversarial all-HIGH advisory header. */
    memset(&r, 0, sizeof(r));
    r.records_mask = 0u;
    r.hdr_mask     = 0xFFu;
    r.hdr_quality  = 0xFFFFFFFFu;
    boot_seed_record_sources(&r);
    TEST_ASSERT_EQ(entropy_source_mask(), 0u,
                   "all-HIGH advisory credits nothing when records_mask is empty");
    TEST_ASSERT_EQ(entropy_classify(entropy_source_mask(),
                                    entropy_source_quality()),
                   ENTROPY_CLASS_DEGRADED, "cloned-only payload stays degraded");

    /* Positive control: an accepted FW_RNG record keeps its advisory HIGH; an
     * accepted SEED_FILE record is force-clamped to LOW despite HIGH advisory. */
    for (src = 0; src < (uint32_t)ENTROPY_SRC_COUNT; src++)
        entropy_record_source((entropy_src_t)src, ENTROPY_Q_NONE);
    memset(&r, 0, sizeof(r));
    r.records_mask = ENTROPY_SRC_BIT(ENTROPY_SRC_FW_RNG) |
                     ENTROPY_SRC_BIT(ENTROPY_SRC_SEED_FILE);
    r.hdr_quality  = entropy_quality_set(0, ENTROPY_SRC_FW_RNG, ENTROPY_Q_HIGH);
    r.hdr_quality  = entropy_quality_set(r.hdr_quality, ENTROPY_SRC_SEED_FILE,
                                         ENTROPY_Q_HIGH);
    boot_seed_record_sources(&r);
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_FW_RNG) & 1u, 1u,
                   "accepted FW_RNG record is credited");
    TEST_ASSERT_EQ(entropy_quality_get(entropy_source_quality(),
                                       ENTROPY_SRC_FW_RNG),
                   ENTROPY_Q_HIGH, "accepted FW_RNG keeps advisory HIGH quality");
    TEST_ASSERT_EQ(entropy_quality_get(entropy_source_quality(),
                                       ENTROPY_SRC_SEED_FILE),
                   ENTROPY_Q_LOW,
                   "accepted SEED_FILE force-clamped to LOW despite HIGH advisory");

    /* Restore the boot-time record exactly. */
    for (src = 0; src < (uint32_t)ENTROPY_SRC_COUNT; src++) {
        entropy_quality_t q = (saved_mask & ENTROPY_SRC_BIT(src))
            ? entropy_quality_get(saved_q, (entropy_src_t)src)
            : ENTROPY_Q_NONE;
        entropy_record_source((entropy_src_t)src, q);
    }
}

void test_register_boot_seed(void)
{
    test_suite_register_cat("boot seed: record sources credit gate",
        test_boot_seed_record_sources_gate, TEST_CAT_SECURITY);
}
