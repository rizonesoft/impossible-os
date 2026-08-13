/* ============================================================================
 * test_boot_reserved.c -- unit tests for the boot-protocol handoff
 * memory ownership / PMM reservation table (boot_reserved module).
 *
 * Covers:
 *   - populate_from_info happy path (3 typical regions: boot_info, TPM,
 *     framebuffer) -> count + ordering match boot_info.
 *   - payload descriptors with BOOT_PAYLOAD_FLAG_RESERVED are added;
 *     descriptors without RESERVED are skipped.
 *   - overlap between two retained regions returns BOOT_RESERVED_ERR_OVERLAP.
 *   - range wrap returns BOOT_RESERVED_ERR_RANGE_WRAP.
 *   - rt_mmap_count > BOOT_RT_MMAP_MAX returns BOOT_RESERVED_ERR_COUNT_OOR.
 *   - payload_count > BOOT_PAYLOAD_MAX returns BOOT_RESERVED_ERR_COUNT_OOR.
 *   - NULL info returns BOOT_RESERVED_ERR_NULL_INFO.
 *
 * All tests use a stack-allocated struct boot_info fixture and call
 * boot_reserved_reset_for_test() before each scenario. pmm_mark_region
 * _used is live and safe (writes a bitmap byte); tests never call
 * boot_reserved_apply() because that would mutate real PMM state.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/test/klog_suppress.h"
#include "kernel/mm/boot_reserved.h"
#include "kernel/boot_info.h"
/* libc/string.h not needed; fixture is zeroed by scalar loop. */

/* BSS-resident fixture: struct boot_info is ~23 KiB. Allocating on the
 * test stack would blow past IST sizes on some configurations.  */
static struct boot_info s_br_buf;

static void br_zero_fixture(void)
{
    uint64_t *p = (uint64_t *)&s_br_buf;
    uint32_t i;
    for (i = 0u; i < sizeof(s_br_buf) / sizeof(uint64_t); i++)
        p[i] = 0u;
    /* Header magic + size must be valid so populate doesn't misread,
     * but boot_reserved doesn't consult them directly; zero is fine. */
    boot_reserved_reset_for_test();
}

static void test_boot_reserved_populate_happy_path(void)
{
    br_zero_fixture();

    /* TPM event log at 1 MiB, 4 KiB. Gate: caps_present must carry
     * BOOT_CAP_TPM_EVENT_LOG for the reservation path to enter. */
    s_br_buf.tpm_event_log      = 0x100000u;
    s_br_buf.tpm_event_log_size = 0x1000u;
    s_br_buf.caps_present       = BOOT_CAP_TPM_EVENT_LOG;

    /* Framebuffer at 0xE0000000, pitch=1920*4, height=1080 -> 8 MiB. */
    s_br_buf.fb.addr        = 0xE0000000u;
    s_br_buf.fb.pitch       = 1920u * 4u;
    s_br_buf.fb.height      = 1080u;
    s_br_buf.fb_available   = 1;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);

    TEST_ASSERT_EQ((int)r, (int)BOOT_OK, "populate happy path -> BOOT_OK");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_OK, "err=OK");
    /* Expect 3 entries: boot_info, TPM, framebuffer. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)3,
                   "3 regions registered");

    const struct boot_reserved_region *b = boot_reserved_get(0);
    TEST_ASSERT_EQ((unsigned long)b->kind,
                   (unsigned long)BOOT_RESERVED_BOOT_INFO,
                   "entry 0 is boot_info");

    const struct boot_reserved_region *t = boot_reserved_get(1);
    TEST_ASSERT_EQ((unsigned long)t->kind,
                   (unsigned long)BOOT_RESERVED_TPM_EVENT_LOG,
                   "entry 1 is tpm_event_log");
    TEST_ASSERT_EQ((unsigned long)t->phys_start, (unsigned long)0x100000ul,
                   "TPM phys_start matches");
    TEST_ASSERT_EQ((unsigned long)t->length, (unsigned long)0x1000ul,
                   "TPM length matches");

    const struct boot_reserved_region *f = boot_reserved_get(2);
    TEST_ASSERT_EQ((unsigned long)f->kind,
                   (unsigned long)BOOT_RESERVED_FRAMEBUFFER,
                   "entry 2 is framebuffer");
    TEST_ASSERT_EQ((unsigned long)f->length,
                   (unsigned long)(1920ul * 4ul * 1080ul),
                   "fb length = pitch * height");
}

static void test_boot_reserved_payload_reserved_flag(void)
{
    br_zero_fixture();

    /* One payload with FLAG_RESERVED set (section 5 bootloader). */
    s_br_buf.caps_present                        = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count                       = 2;
    s_br_buf.payload_descriptors[0].type         = BOOT_PAYLOAD_MODULE;
    s_br_buf.payload_descriptors[0].flags        = BOOT_PAYLOAD_FLAG_VALID |
                                                   BOOT_PAYLOAD_FLAG_RESERVED;
    s_br_buf.payload_descriptors[0].phys_start   = 0x2000000ull;
    s_br_buf.payload_descriptors[0].length       = 0x1000ull;

    /* One payload without FLAG_RESERVED -- must NOT be added. */
    s_br_buf.payload_descriptors[1].type         = BOOT_PAYLOAD_INITRD;
    s_br_buf.payload_descriptors[1].flags        = BOOT_PAYLOAD_FLAG_VALID;
    s_br_buf.payload_descriptors[1].phys_start   = 0x3000000ull;
    s_br_buf.payload_descriptors[1].length       = 0x1000ull;
    s_br_buf.payload_total_bytes                 = 0x2000ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r, (int)BOOT_OK, "two-payload fixture populates OK");

    /* Expected: 1 boot_info + 1 RESERVED payload = 2 entries (INITRD
     * without RESERVED must not appear). */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)2,
                   "only FLAG_RESERVED payloads counted");

    const struct boot_reserved_region *p = boot_reserved_get(1);
    TEST_ASSERT_EQ((unsigned long)p->kind,
                   (unsigned long)BOOT_RESERVED_PAYLOAD,
                   "entry 1 is payload");
    TEST_ASSERT_EQ((unsigned long)p->phys_start,
                   (unsigned long)0x2000000ul,
                   "reserved-payload phys_start matches");
    TEST_ASSERT_EQ((unsigned long)p->source_index, (unsigned long)0,
                   "source_index is the descriptor array index");
}

static void test_boot_reserved_overlap_rejected(void)
{
    TEST_KLOG_SUPPRESS("mm");
    br_zero_fixture();

    /* Place the TPM event log at the same physical address as the
     * struct boot_info copy. Bound to BOOT_INFO_PHYS_ADDR, not a literal,
     * so the case keeps constructing a real overlap if the handoff base
     * ever moves. populate_from_info adds boot_info first, then the TPM
     * entry overlaps -> error. */
    s_br_buf.tpm_event_log      = BOOT_INFO_PHYS_ADDR;
    s_br_buf.tpm_event_log_size = 0x1000u;
    s_br_buf.caps_present       = BOOT_CAP_TPM_EVENT_LOG;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL,
                   "overlap with boot_info rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_OVERLAP,
                   "err=OVERLAP");
}

static void test_boot_reserved_range_wrap_rejected(void)
{
    TEST_KLOG_SUPPRESS("mm");
    br_zero_fixture();

    /* TPM event log near UINT64_MAX with length that wraps. */
    s_br_buf.tpm_event_log      = 0xFFFFFFFFFFFFF000ull;
    s_br_buf.tpm_event_log_size = 0x4000u;  /* wraps past UINT64_MAX */
    s_br_buf.caps_present       = BOOT_CAP_TPM_EVENT_LOG;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL, "range-wrap rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_RANGE_WRAP,
                   "err=RANGE_WRAP");
}

static void test_boot_reserved_xhci_scratchpad_no_self_overlap(void)
{
    br_zero_fixture();

    /* Model the real bootloader handoff: scratchpad_base_phys is
     * ALSO recorded in dma_pages[] (src/boot/uefi/bootx64.c near
     * line 4491). Without the de-dup guard, populate would register
     * sp_base as a 4 KiB DMA page AND as the first 4 KiB of the
     * scratchpad contiguous region -> overlap -> fatal on every
     * xHCI system. This test locks the correct behavior: both
     * entries are accepted and the scratchpad entry covers
     * sp_base alone. */
    s_br_buf.usb_controller.active                = 1;
    s_br_buf.usb_controller.dma_page_count        = 3u;
    s_br_buf.usb_controller.dma_pages[0]          = 0x200000ull;  /* DCBAA */
    s_br_buf.usb_controller.dma_pages[1]          = 0x201000ull;  /* cmd ring */
    s_br_buf.usb_controller.dma_pages[2]          = 0x202000ull;  /* sp_base also here */
    s_br_buf.usb_controller.scratchpad_base_phys  = 0x202000ull;
    s_br_buf.usb_controller.scratchpad_page_count = 2u;           /* 8 KiB total */

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);

    TEST_ASSERT_EQ((int)r, (int)BOOT_OK,
                   "xHCI scratchpad self-overlap de-duped");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_OK, "err=OK");
    /* Expect: boot_info + 2 DMA pages (DCBAA, cmd ring; sp_base skipped)
     * + 1 scratchpad entry = 4 regions. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)4,
                   "sp_base de-duped from DMA loop");
}

static void test_boot_reserved_scratchpad_array_multi_page(void)
{
    br_zero_fixture();

    /* xHCI HCSPARAMS2 advertises sp_count = 600 (high-end vendor extension);
     * pointer array = 600 * 8 = 4800 bytes -> 2 pages. The bootloader
     * allocates a contiguous 2-page region for the array and records the
     * BASE in dma_pages[]. Without the multi-page reservation fix, the
     * second array page would land back in PMM as free memory while the
     * controller still references it via the loaded ERST -- silent DMA
     * corruption. This test pins the per-base length: dma_pages[0] entry
     * covering scratchpad_array_phys must reserve ((600*8 + 4095) & ~4095)
     * = 8192 bytes, not 4096. */
    s_br_buf.usb_controller.active                = 1;
    s_br_buf.usb_controller.dma_page_count        = 1u;
    s_br_buf.usb_controller.dma_pages[0]          = 0x300000ull; /* sp_array base */
    s_br_buf.usb_controller.scratchpad_array_phys = 0x300000ull;
    s_br_buf.usb_controller.scratchpad_base_phys  = 0x400000ull; /* far away */
    s_br_buf.usb_controller.scratchpad_page_count = 600u;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r, (int)BOOT_OK, "multi-page scratchpad array accepted");

    /* Walk the published table: find the entry whose phys_start matches
     * scratchpad_array_phys; assert its length is 2 pages, not 1. */
    uint32_t i;
    int found = 0;
    uint64_t expected_len = 0x2000ull;  /* (600*8 + 4095) rounded up = 8192 */
    for (i = 0u; i < boot_reserved_count(); i++) {
        const struct boot_reserved_region *e = boot_reserved_get(i);
        if (e->phys_start == 0x300000ull) {
            TEST_ASSERT_EQ((unsigned long)e->length, (unsigned long)expected_len,
                           "scratchpad-array entry covers full multi-page extent");
            found = 1;
            break;
        }
    }
    TEST_ASSERT_EQ(found, 1, "scratchpad-array entry present in table");
}

static void test_boot_reserved_rt_mmap_num_pages_wrap(void)
{
    TEST_KLOG_SUPPRESS("mm");
    br_zero_fixture();

    /* num_pages above UINT64_MAX / 4096 wraps on multiplication. A
     * naive implementation would happily record a tiny length and
     * leave the rest of the runtime range free. The validator must
     * catch this before the multiply.
     *
     * Codex 2026-04-30: rt_mmap loop is now gated on
     * BOOT_CAP_RUNTIME_SERVICES; set the cap so the loop runs and
     * the wrap check fires. */
    s_br_buf.caps_present = BOOT_CAP_RUNTIME_SERVICES;
    s_br_buf.rt_mmap_count = 1u;
    s_br_buf.rt_mmap[0].phys_addr = 0x100000000ull;
    s_br_buf.rt_mmap[0].num_pages = ((uint64_t)-1 / 4096ull) + 1ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL, "num_pages wrap rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_RANGE_WRAP,
                   "err=RANGE_WRAP (pre-multiply)");
}

static void test_boot_reserved_rt_mmap_count_oor(void)
{
    TEST_KLOG_SUPPRESS("mm");
    br_zero_fixture();

    /* rt_mmap_count above the table cap -- producer bug; refuse rather
     * than truncate. */
    s_br_buf.rt_mmap_count = BOOT_RT_MMAP_MAX + 1u;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL, "rt_mmap_count > MAX rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_COUNT_OOR,
                   "err=COUNT_OOR");
}

static void test_boot_reserved_null_info(void)
{
    boot_reserved_reset_for_test();

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(NULL, &err);
    TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL, "NULL info rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_NULL_INFO,
                   "err=NULL_INFO");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)0,
                   "table untouched on NULL");
}

static void test_boot_reserved_payload_vs_pmm_internal(void)
{
    /* The overlap branch inside boot_reserved_check_payloads_disjoint
     * emits a LOG_ERROR naming both offenders when a payload collides
     * with a PMM-internal range. That is EXACTLY what this test is
     * proving fires -- suppress the expected [FAIL]-tagged klog so
     * the boot log does not look like an actual test failure. */
    TEST_KLOG_SUPPRESS("mm");
    br_zero_fixture();

    /* Reserved payload at 0x2000000 (32 MiB), 4 KiB long. */
    s_br_buf.caps_present                        = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count                       = 1;
    s_br_buf.payload_descriptors[0].type         = BOOT_PAYLOAD_MODULE;
    s_br_buf.payload_descriptors[0].flags        = BOOT_PAYLOAD_FLAG_VALID |
                                                   BOOT_PAYLOAD_FLAG_RESERVED;
    s_br_buf.payload_descriptors[0].phys_start   = 0x2000000ull;
    s_br_buf.payload_descriptors[0].length       = 0x1000ull;
    s_br_buf.payload_total_bytes                 = 0x1000ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    TEST_ASSERT_EQ((int)boot_reserved_populate_from_info(&s_br_buf, &err),
                   (int)BOOT_OK, "payload fixture populates OK");

    /* Disjoint PMM-internal range returns 0 (no collision). */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_check_payloads_disjoint(
                       0ull, 0x100000ull, "low_mem_1mb"),
                   (unsigned long)0,
                   "payload disjoint from first 1 MiB");

    /* Overlapping PMM-internal range returns (s_table_idx + 1) of
     * colliding entry. boot_info occupies s_table[0]; the payload we
     * added is s_table[1], so hit value is 2 (1-based). */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_check_payloads_disjoint(
                       0x2000000ull, 0x2000ull, "fake_bitmap"),
                   (unsigned long)2,
                   "payload overlap returns s_table_idx+1 (colliding entry)");
}

static void test_boot_reserved_payload_count_oor_still_rejected_with_degraded_caps(void)
{
    /* Structural validation (payload_count > BOOT_PAYLOAD_MAX) MUST
     * fire unconditionally, even when BOOT_CAP_PAYLOAD_DESCRIPTORS is
     * degraded. A corrupt boot_info cannot hide behind a missing cap
     * bit. */
    TEST_KLOG_SUPPRESS("mm");
    br_zero_fixture();
    s_br_buf.caps_present  = 0u;
    s_br_buf.caps_degraded = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = BOOT_PAYLOAD_MAX + 1u;  /* corrupt */

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r,   (int)BOOT_FATAL,                       "corrupt count rejected with degraded caps");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_COUNT_OOR,      "err=COUNT_OOR");
}

static void test_boot_reserved_rt_mmap_runtime_services_degraded_skipped(void)
{
    /* Codex 2026-04-30 H regression: when BOOT_CAP_RUNTIME_SERVICES is
     * NOT set in caps_present (degraded -- RT init failed or producer
     * never advertised), boot_reserved_populate_from_info MUST skip
     * the rt_mmap reservation loop. Pinning rt_mmap regions for an
     * unavailable runtime would waste physical memory on regions the
     * kernel has explicitly decided it will never call into. */
    TEST_KLOG_SUPPRESS("mm");
    br_zero_fixture();
    s_br_buf.caps_present  = 0u;  /* RUNTIME_SERVICES not advertised */
    s_br_buf.caps_degraded = BOOT_CAP_RUNTIME_SERVICES;
    /* Populate rt_mmap as if the producer DID write it (stale or
     * companion-field-without-cap pattern). */
    s_br_buf.rt_mmap_count       = 1u;
    s_br_buf.rt_mmap[0].phys_addr = 0x200000000ull;
    s_br_buf.rt_mmap[0].num_pages = 4ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r,   (int)BOOT_OK,                    "degraded RT -> populate OK");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_OK,       "err=OK");
    /* Expected: only boot_info entry; rt_mmap skipped. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)1,
                   "degraded RT caps -> no rt_mmap reservation");
}

static void test_boot_reserved_warm_update_no_flag_skipped(void)
{
    /* Codex 2026-04-30 re-adversarial regression: a type-9 warm-update
     * descriptor with BOOT_PAYLOAD_FLAG_RESERVED set MUST NOT be pinned
     * by boot_reserved_populate when BOOT_FLAG_WARM_UPDATE is clear.
     * Without this gate, a stale or malformed type-9 descriptor would
     * pin arbitrary payload pages even after the warm-update consume
     * cold-fallbacked it. */
    TEST_KLOG_SUPPRESS("mm");
    br_zero_fixture();
    s_br_buf.caps_present                        = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.flags                               = 0u;  /* BOOT_FLAG_WARM_UPDATE clear */
    s_br_buf.payload_count                       = 1;
    s_br_buf.payload_descriptors[0].type         = BOOT_PAYLOAD_WARM_UPDATE_STATE;
    s_br_buf.payload_descriptors[0].flags        = BOOT_PAYLOAD_FLAG_VALID
                                                 | BOOT_PAYLOAD_FLAG_RESERVED;
    s_br_buf.payload_descriptors[0].phys_start   = 0x4000000ull;
    s_br_buf.payload_descriptors[0].length       = 0x1000ull;
    s_br_buf.payload_total_bytes                 = 0x1000ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r,   (int)BOOT_OK,                    "warm-update no-flag -> populate OK");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_OK,       "err=OK");
    /* Expected: only boot_info entry; warm-update descriptor skipped. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)1,
                   "warm-update + RESERVED + flag clear -> not pinned");
}

static void test_boot_reserved_warm_update_with_flag_pinned(void)
{
    /* Inverse of the above: when BOOT_FLAG_WARM_UPDATE is set, the
     * type-9 RESERVED descriptor MUST be pinned (the warm-update
     * runtime expects the preserved memory range to survive PMM
     * handoff). */
    br_zero_fixture();
    s_br_buf.caps_present                        = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.flags                               = BOOT_FLAG_WARM_UPDATE;
    s_br_buf.payload_count                       = 1;
    s_br_buf.payload_descriptors[0].type         = BOOT_PAYLOAD_WARM_UPDATE_STATE;
    s_br_buf.payload_descriptors[0].flags        = BOOT_PAYLOAD_FLAG_VALID
                                                 | BOOT_PAYLOAD_FLAG_RESERVED;
    s_br_buf.payload_descriptors[0].phys_start   = 0x4000000ull;
    s_br_buf.payload_descriptors[0].length       = 0x1000ull;
    s_br_buf.payload_total_bytes                 = 0x1000ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r,   (int)BOOT_OK,                    "warm-update with flag -> populate OK");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_OK,       "err=OK");
    /* Expected: boot_info + warm-update RESERVED entry = 2. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)2,
                   "warm-update + RESERVED + flag set -> pinned");
}

static void test_boot_reserved_tpm_log_degraded_skipped(void)
{
    /* Capability-gated retrofit regression: when BOOT_CAP_TPM_EVENT_LOG
     * is NOT in caps_present, populate_from_info MUST skip the TPM
     * event-log reservation even when tpm_event_log +
     * tpm_event_log_size are populated. Prevents a loader from
     * forcing the kernel to pin a degraded region. */
    br_zero_fixture();
    s_br_buf.tpm_event_log      = 0x100000u;
    s_br_buf.tpm_event_log_size = 0x1000u;
    /* caps_present does NOT carry BOOT_CAP_TPM_EVENT_LOG */
    s_br_buf.caps_present       = 0u;
    s_br_buf.caps_degraded      = BOOT_CAP_TPM_EVENT_LOG;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
    TEST_ASSERT_EQ((int)r,   (int)BOOT_OK,                    "degraded TPM -> populate OK");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_OK,       "err=OK");
    /* Expected: only boot_info entry; TPM skipped. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)1,
                   "degraded TPM caps -> no reservation");
}

void test_register_boot_reserved(void)
{
    test_suite_register_cat("boot_reserved: populate happy path",
                            test_boot_reserved_populate_happy_path,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: payload FLAG_RESERVED filter",
                            test_boot_reserved_payload_reserved_flag,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: overlap rejected",
                            test_boot_reserved_overlap_rejected,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: range wrap rejected",
                            test_boot_reserved_range_wrap_rejected,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: rt_mmap count OOR",
                            test_boot_reserved_rt_mmap_count_oor,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: rt_mmap num_pages wrap",
                            test_boot_reserved_rt_mmap_num_pages_wrap,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: xHCI scratchpad self-overlap",
                            test_boot_reserved_xhci_scratchpad_no_self_overlap,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: scratchpad array multi-page",
                            test_boot_reserved_scratchpad_array_multi_page,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: NULL info rejected",
                            test_boot_reserved_null_info, TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: degraded RUNTIME_SERVICES caps skips rt_mmap reservation",
                            test_boot_reserved_rt_mmap_runtime_services_degraded_skipped, TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: warm-update RESERVED skipped when WARM_UPDATE flag clear",
                            test_boot_reserved_warm_update_no_flag_skipped, TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: warm-update RESERVED pinned when WARM_UPDATE flag set",
                            test_boot_reserved_warm_update_with_flag_pinned, TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: degraded TPM caps skips reservation",
                            test_boot_reserved_tpm_log_degraded_skipped, TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: payload count OOR still rejected with degraded caps",
                            test_boot_reserved_payload_count_oor_still_rejected_with_degraded_caps,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: payload vs PMM-internal",
                            test_boot_reserved_payload_vs_pmm_internal,
                            TEST_CAT_BOOT);
}
