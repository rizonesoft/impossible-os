/* Unit tests for the TPM-rooted boot attestation report export -- currently the
 * immutable bootloader-to-kernel handoff snapshot. */

#include "kernel/test/test.h"
#include "kernel/tpm_attest_report.h"
#include "kernel/boot_info.h"
#include "libc/string.h"

/* Static fixture: struct boot_info is large, so keep it off the test stack. */
static struct boot_info s_fixture_bi;

static void test_attest_handoff_capture(void)
{
    struct boot_attest_handoff h;

    memset(&s_fixture_bi, 0, sizeof s_fixture_bi);
    s_fixture_bi.caps_required       = 0x1234u;
    s_fixture_bi.caps_present        = 0x5678u;
    s_fixture_bi.caps_degraded       = 0x9ABCu;
    s_fixture_bi.boot_path           = 3u;
    s_fixture_bi.boot_reason         = 7u;
    s_fixture_bi.boot_source_flags   = 0xF0u;
    s_fixture_bi.boot_fallback_depth = 2u;

    tpm_attest_handoff_capture(&s_fixture_bi, &h);
    TEST_ASSERT_EQ((uint32_t)h.valid, 1u, "capture marks the snapshot valid");
    TEST_ASSERT_EQ((uint32_t)h.caps_required, 0x1234u, "caps_required copied");
    TEST_ASSERT_EQ((uint32_t)h.caps_present, 0x5678u, "caps_present copied");
    TEST_ASSERT_EQ((uint32_t)h.caps_degraded, 0x9ABCu, "caps_degraded copied");
    TEST_ASSERT_EQ(h.boot_path, 3u, "boot_path copied");
    TEST_ASSERT_EQ(h.boot_reason, 7u, "boot_reason copied");
    TEST_ASSERT_EQ(h.boot_source_flags, 0xF0u, "boot_source_flags copied");
    TEST_ASSERT_EQ(h.boot_fallback_depth, 2u, "boot_fallback_depth copied");

    /* The load-bearing property: the snapshot is a COPY. A later kernel refinement
     * of the live boot_info (e.g. boot_caps_mark_present) must NOT change what the
     * attestation report attributes to the loader handoff. */
    s_fixture_bi.caps_present = 0xFFFFu;
    TEST_ASSERT_EQ((uint32_t)h.caps_present, 0x5678u,
                   "snapshot immutable to a later boot_info caps change");

    /* NULL boot_info -> invalid (fail closed), never a stale/garbage snapshot. */
    memset(&h, 0xAA, sizeof h);
    tpm_attest_handoff_capture(0, &h);
    TEST_ASSERT_EQ((uint32_t)h.valid, 0u, "NULL boot_info -> invalid snapshot");

    /* snapshot_init is WRITE-ONCE, enforced by a latch (no public reset). boot_phase0
     * already captured the real Phase-0 snapshot, so a later init with a fixture MUST
     * no-op -- the getter keeps the loader-time boot snapshot, never kernel-refined
     * caps. This proves a later accidental call cannot replace loader evidence. */
    {
        const struct boot_attest_handoff *g = tpm_attest_handoff_get();
        uint64_t boot_caps;
        TEST_ASSERT(g != 0, "handoff getter is never NULL");
        TEST_ASSERT_EQ((uint32_t)g->valid, 1u, "boot_phase0 captured a valid snapshot");
        boot_caps = g->caps_present;
        memset(&s_fixture_bi, 0, sizeof s_fixture_bi);
        s_fixture_bi.caps_present = ~boot_caps;        /* a value the boot snapshot is not */
        tpm_attest_handoff_snapshot_init(&s_fixture_bi);   /* latched -> no-op */
        TEST_ASSERT_EQ((uint32_t)g->caps_present, (uint32_t)boot_caps,
                       "write-once: a later init does not overwrite the boot snapshot");
    }
}

void test_register_tpm_attest_report(void)
{
    test_suite_register_cat("tpm: attest handoff snapshot capture",
                            test_attest_handoff_capture, TEST_CAT_SECURITY);
}
