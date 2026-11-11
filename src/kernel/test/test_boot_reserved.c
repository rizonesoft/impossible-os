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
#include "kernel/test/boot_info_fixture.h" /* TEST_BOOT_INFO: the shared fixture */
#include "kernel/test/klog_suppress.h"
#include "kernel/mm/boot_reserved.h"
#include "kernel/boot_info.h"
#include "boot/uki_payload_limits.h" /* UKI_PAYLOAD_SLOT_*, UKI_PAYLOAD_MAX_BYTES */
#include "kernel/entropy.h"   /* boot_seed_length_reservable -- must agree with the table */
/* libc/string.h not needed; fixture is zeroed by scalar loop. */

/* BSS-resident fixture: struct boot_info is ~23 KiB. Allocating on the
 * test stack would blow past IST sizes on some configurations.  */

/* The RAM every fixture declares: one conventional entry from 0 to here. The
 * reservation pass now asks the memory map whether a payload is readable RAM,
 * so a fixture states its own memory and an "out of range" address is past
 * THIS end, the same on every host (rather than past the host's frame count). */
#define BR_FIXTURE_RAM_END  0xF0000000ull

static void br_set_map(uint32_t i, uint64_t base, uint64_t len, uint32_t type)
{
    TEST_BOOT_INFO.mmap[i].base_addr        = base;
    TEST_BOOT_INFO.mmap[i].length           = len;
    TEST_BOOT_INFO.mmap[i].uefi_memory_type = type;
    if (TEST_BOOT_INFO.mmap_count < i + 1u)
        TEST_BOOT_INFO.mmap_count = i + 1u;
}

static void br_zero_fixture(void)
{
    uint64_t *p = (uint64_t *)&TEST_BOOT_INFO;
    uint32_t i;
    for (i = 0u; i < sizeof(TEST_BOOT_INFO) / sizeof(uint64_t); i++)
        p[i] = 0u;
    /* Header magic + size must be valid so populate doesn't misread,
     * but boot_reserved doesn't consult them directly; zero is fine. */
    boot_reserved_reset_for_test();
    /* The warm-update selection is SEALED by whichever phase reads it first
     * and is deliberately immutable after that, so a fixture that did not
     * drop it would be adjudicated against the PREVIOUS fixture's handoff
     * and refused as a mismatch. */
    boot_warm_update_selection_reset_for_test();
    br_set_map(0u, 0u, BR_FIXTURE_RAM_END, UEFI_MMAP_CONVENTIONAL);
}

static void test_boot_reserved_populate_happy_path(void)
{
    br_zero_fixture();

    /* TPM event log at 1 MiB, 4 KiB. Gate: caps_present must carry
     * BOOT_CAP_TPM_EVENT_LOG for the reservation path to enter. */
    TEST_BOOT_INFO.tpm_event_log      = 0x100000u;
    TEST_BOOT_INFO.tpm_event_log_size = 0x1000u;
    TEST_BOOT_INFO.caps_present       = BOOT_CAP_TPM_EVENT_LOG;

    /* Framebuffer at 0xE0000000, pitch=1920*4, height=1080 -> 8 MiB. */
    TEST_BOOT_INFO.fb.addr        = 0xE0000000u;
    TEST_BOOT_INFO.fb.pitch       = 1920u * 4u;
    TEST_BOOT_INFO.fb.height      = 1080u;
    TEST_BOOT_INFO.fb_available   = 1;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

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
    TEST_BOOT_INFO.caps_present                        = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count                       = 2;
    TEST_BOOT_INFO.payload_descriptors[0].type         = BOOT_PAYLOAD_MODULE;
    TEST_BOOT_INFO.payload_descriptors[0].flags        = BOOT_PAYLOAD_FLAG_VALID |
                                                   BOOT_PAYLOAD_FLAG_RESERVED;
    TEST_BOOT_INFO.payload_descriptors[0].phys_start   = 0x2000000ull;
    TEST_BOOT_INFO.payload_descriptors[0].length       = 0x1000ull;

    /* One payload without FLAG_RESERVED -- must NOT be added. */
    TEST_BOOT_INFO.payload_descriptors[1].type         = BOOT_PAYLOAD_INITRD;
    TEST_BOOT_INFO.payload_descriptors[1].flags        = BOOT_PAYLOAD_FLAG_VALID;
    TEST_BOOT_INFO.payload_descriptors[1].phys_start   = 0x3000000ull;
    TEST_BOOT_INFO.payload_descriptors[1].length       = 0x1000ull;
    TEST_BOOT_INFO.payload_total_bytes                 = 0x2000ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
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
    TEST_BOOT_INFO.tpm_event_log      = BOOT_INFO_PHYS_ADDR;
    TEST_BOOT_INFO.tpm_event_log_size = 0x1000u;
    TEST_BOOT_INFO.caps_present       = BOOT_CAP_TPM_EVENT_LOG;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
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
    TEST_BOOT_INFO.tpm_event_log      = 0xFFFFFFFFFFFFF000ull;
    TEST_BOOT_INFO.tpm_event_log_size = 0x4000u;  /* wraps past UINT64_MAX */
    TEST_BOOT_INFO.caps_present       = BOOT_CAP_TPM_EVENT_LOG;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
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
    TEST_BOOT_INFO.usb_controller.active                = 1;
    TEST_BOOT_INFO.usb_controller.dma_page_count        = 3u;
    TEST_BOOT_INFO.usb_controller.dma_pages[0]          = 0x200000ull;  /* DCBAA */
    TEST_BOOT_INFO.usb_controller.dma_pages[1]          = 0x201000ull;  /* cmd ring */
    TEST_BOOT_INFO.usb_controller.dma_pages[2]          = 0x202000ull;  /* sp_base also here */
    TEST_BOOT_INFO.usb_controller.scratchpad_base_phys  = 0x202000ull;
    TEST_BOOT_INFO.usb_controller.scratchpad_page_count = 2u;           /* 8 KiB total */

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

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
    TEST_BOOT_INFO.usb_controller.active                = 1;
    TEST_BOOT_INFO.usb_controller.dma_page_count        = 1u;
    TEST_BOOT_INFO.usb_controller.dma_pages[0]          = 0x300000ull; /* sp_array base */
    TEST_BOOT_INFO.usb_controller.scratchpad_array_phys = 0x300000ull;
    TEST_BOOT_INFO.usb_controller.scratchpad_base_phys  = 0x400000ull; /* far away */
    TEST_BOOT_INFO.usb_controller.scratchpad_page_count = 600u;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
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
    TEST_BOOT_INFO.caps_present = BOOT_CAP_RUNTIME_SERVICES;
    TEST_BOOT_INFO.rt_mmap_count = 1u;
    TEST_BOOT_INFO.rt_mmap[0].phys_addr = 0x100000000ull;
    TEST_BOOT_INFO.rt_mmap[0].num_pages = ((uint64_t)-1 / 4096ull) + 1ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
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
    TEST_BOOT_INFO.rt_mmap_count = BOOT_RT_MMAP_MAX + 1u;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
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
    TEST_BOOT_INFO.caps_present                        = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count                       = 1;
    TEST_BOOT_INFO.payload_descriptors[0].type         = BOOT_PAYLOAD_MODULE;
    TEST_BOOT_INFO.payload_descriptors[0].flags        = BOOT_PAYLOAD_FLAG_VALID |
                                                   BOOT_PAYLOAD_FLAG_RESERVED;
    TEST_BOOT_INFO.payload_descriptors[0].phys_start   = 0x2000000ull;
    TEST_BOOT_INFO.payload_descriptors[0].length       = 0x1000ull;
    TEST_BOOT_INFO.payload_total_bytes                 = 0x1000ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    TEST_ASSERT_EQ((int)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err),
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
    TEST_BOOT_INFO.caps_present  = 0u;
    TEST_BOOT_INFO.caps_degraded = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = BOOT_PAYLOAD_MAX + 1u;  /* corrupt */

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
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
    TEST_BOOT_INFO.caps_present  = 0u;  /* RUNTIME_SERVICES not advertised */
    TEST_BOOT_INFO.caps_degraded = BOOT_CAP_RUNTIME_SERVICES;
    /* Populate rt_mmap as if the producer DID write it (stale or
     * companion-field-without-cap pattern). */
    TEST_BOOT_INFO.rt_mmap_count       = 1u;
    TEST_BOOT_INFO.rt_mmap[0].phys_addr = 0x200000000ull;
    TEST_BOOT_INFO.rt_mmap[0].num_pages = 4ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
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
    TEST_BOOT_INFO.caps_present                        = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags                               = 0u;  /* BOOT_FLAG_WARM_UPDATE clear */
    TEST_BOOT_INFO.payload_count                       = 1;
    TEST_BOOT_INFO.payload_descriptors[0].type         = BOOT_PAYLOAD_WARM_UPDATE_STATE;
    TEST_BOOT_INFO.payload_descriptors[0].flags        = BOOT_PAYLOAD_FLAG_VALID
                                                 | BOOT_PAYLOAD_FLAG_RESERVED;
    TEST_BOOT_INFO.payload_descriptors[0].phys_start   = 0x4000000ull;
    TEST_BOOT_INFO.payload_descriptors[0].length       = 0x1000ull;
    TEST_BOOT_INFO.payload_total_bytes                 = 0x1000ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
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
    TEST_BOOT_INFO.caps_present                        = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags                               = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count                       = 1;
    TEST_BOOT_INFO.payload_descriptors[0].type         = BOOT_PAYLOAD_WARM_UPDATE_STATE;
    TEST_BOOT_INFO.payload_descriptors[0].flags        = BOOT_PAYLOAD_FLAG_VALID
                                                 | BOOT_PAYLOAD_FLAG_RESERVED;
    TEST_BOOT_INFO.payload_descriptors[0].phys_start   = 0x4000000ull;
    TEST_BOOT_INFO.payload_descriptors[0].length       = 0x1000ull;
    TEST_BOOT_INFO.payload_total_bytes                 = 0x1000ull;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
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
    TEST_BOOT_INFO.tpm_event_log      = 0x100000u;
    TEST_BOOT_INFO.tpm_event_log_size = 0x1000u;
    /* caps_present does NOT carry BOOT_CAP_TPM_EVENT_LOG */
    TEST_BOOT_INFO.caps_present       = 0u;
    TEST_BOOT_INFO.caps_degraded      = BOOT_CAP_TPM_EVENT_LOG;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((int)r,   (int)BOOT_OK,                    "degraded TPM -> populate OK");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_OK,       "err=OK");
    /* Expected: only boot_info entry; TPM skipped. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)1,
                   "degraded TPM caps -> no reservation");
}


/* ---- Loader-owned kernel boot stack entry (TODO-10 sec32) ------------------
 *
 * Every other fixture in this file leaves kstack_base/kstack_size zero, so the
 * new populate block was skipped by all of them -- the entry that keeps the
 * PMM off the running kernel stack was covered only by a live boot. These
 * fixtures drive it directly.
 */

#define BR_KS_BASE  0x40000000ull
#define BR_KS_SIZE  0x00040000ull   /* 256 KiB */

static void test_boot_reserved_boot_stack_entry(void)
{
    uint32_t i;
    uint32_t found = 0u;

    br_zero_fixture();

    TEST_BOOT_INFO.kstack_base       = BR_KS_BASE;
    TEST_BOOT_INFO.kstack_size       = (uint32_t)BR_KS_SIZE;
    TEST_BOOT_INFO.kstack_guard_size = 0x1000u;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((int)r, (int)BOOT_OK, "a lone boot-stack run must populate");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_OK, "err=OK");

    for (i = 0u; i < boot_reserved_count(); i++) {
        const struct boot_reserved_region *e = boot_reserved_get(i);
        if (e->kind != (uint32_t)BOOT_RESERVED_BOOT_STACK)
            continue;
        found++;
        /* The WHOLE run is reserved, guard included: the guard page is inside
         * the allocation, and handing it to the allocator would give the next
         * owner a page the fault handler reports as a stack overflow. */
        TEST_ASSERT_EQ(e->phys_start, BR_KS_BASE, "entry base must be kstack_base");
        TEST_ASSERT_EQ(e->length, BR_KS_SIZE,
                       "entry length must be the FULL run, guard included");
        TEST_ASSERT_EQ((unsigned long)e->source_index, 0ul,
                       "the boot stack is a singleton, so source_index is 0");
    }
    TEST_ASSERT_EQ((unsigned long)found, 1ul,
                   "exactly one BOOT_RESERVED_BOOT_STACK entry must exist");
}

static void test_boot_reserved_boot_stack_absent_when_unpublished(void)
{
    uint32_t i;

    br_zero_fixture();

    /* A producer that publishes nothing must add no entry at all -- not a
     * zero-length one, which add_or_fatal would reject and which would turn
     * an older bootloader into a boot failure inside the PMM instead of the
     * explicit refusal boot_stack_init already gives. */
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((int)r, (int)BOOT_OK, "an unpublished stack must not fail populate");
    for (i = 0u; i < boot_reserved_count(); i++)
        TEST_ASSERT(boot_reserved_get(i)->kind != (uint32_t)BOOT_RESERVED_BOOT_STACK,
                    "no boot-stack entry may exist when kstack_base is 0");
}

static void test_boot_reserved_boot_stack_overlap_rejected(void)
{
    TEST_KLOG_SUPPRESS("mm");
    br_zero_fixture();

    /* Point the stack at the struct boot_info copy. Two retained regions
     * claiming the same frames is a producer bug, and the whole reason the
     * stack goes through this table instead of a private
     * pmm_mark_region_used() call: a direct mark would have made this
     * collision two successful bitmap writes and no complaint. */
    TEST_BOOT_INFO.kstack_base       = BOOT_INFO_PHYS_ADDR;
    TEST_BOOT_INFO.kstack_size       = (uint32_t)BR_KS_SIZE;
    TEST_BOOT_INFO.kstack_guard_size = 0x1000u;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL,
                   "a boot stack overlapping boot_info must be rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_RESERVED_ERR_OVERLAP, "err=OVERLAP");
}

/* ---------------------------------------------------------------------------
 * Per-type length contract, cardinality and aggregate budget
 * (the per-type payload length contract, boot-protocol ABI handoff).
 *
 * These assert the reservation pass's OWN ACCOUNTING -- what landed in the
 * table -- rather than a predicate's return value, because the defect being
 * guarded is memory getting PINNED, and a predicate can be right while the
 * pass that calls it is wrong. The pure helpers are asserted separately
 * below; both layers matter and neither implies the other.
 * ------------------------------------------------------------------------- */

/* How many regions a zeroed fixture pins before any payload is staged.
 *
 * MEASURED rather than hardcoded: the pass always reserves the struct
 * boot_info handoff region itself (phys 0x10000), and a future baseline
 * entry would silently shift every index below. A test that hardcoded the
 * count would then fail for a reason unrelated to what it asserts.
 * Leaves the table reset for the caller. */
static uint32_t br_baseline_count(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t n;

    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 0;
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    n = (uint32_t)boot_reserved_count();
    br_zero_fixture();
    return n;
}

/* Stage one RESERVED payload descriptor at slot `i`. */
static void br_stage_payload(uint32_t i, uint32_t type, uint64_t phys,
                             uint64_t len, uint32_t extra_flags)
{
    TEST_BOOT_INFO.payload_descriptors[i].type       = type;
    TEST_BOOT_INFO.payload_descriptors[i].flags      = BOOT_PAYLOAD_FLAG_VALID |
                                                 BOOT_PAYLOAD_FLAG_RESERVED |
                                                 extra_flags;
    TEST_BOOT_INFO.payload_descriptors[i].phys_start = phys;
    TEST_BOOT_INFO.payload_descriptors[i].length     = len;
}

static void test_boot_reserved_payload_length_contract(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r;
    uint32_t base;

    /* Every refusal this section adds logs a LOG_WARN under "mm", and these
     * tests exist to prove those refusals FIRE -- suppress the expected
     * warnings so the boot log does not read as failing. */
    TEST_KLOG_SUPPRESS("mm");
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 4;

    /* A module EXACTLY at its 256 MiB maximum is pinned; one byte over is
     * not. The boundary is the assertion that matters: an off-by-one would
     * leave the contract nominally present and actually one byte wrong. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435457ull, 0u);
    /* A type with NO declared bound refuses ANY length, including a
     * plausible one. Both of these are "not reservable" in the table. */
    br_stage_payload(2, BOOT_PAYLOAD_TPM_EVENT_LOG,  0x50000000ull, 0x1000ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_NETWORK_CONFIG, 0x51000000ull, 0x1000ull, 0u);

    r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

    TEST_ASSERT_EQ((unsigned long)r, (unsigned long)BOOT_OK,
                   "an over-contract descriptor DEGRADES the boot, never halts it");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 1u),
                   "only the exactly-at-maximum module is pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_get((int)base)->phys_start,
                   (unsigned long)0x10000000ull,
                   "the pinned region is the at-maximum module, not a later one");
}

static void test_boot_reserved_payload_singleton(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 2;
    /* Two initrds. INITRD is a singleton by meaning, so the second is a
     * malformed handoff; pinning it would let one type claim twice its
     * declared maximum, which no per-descriptor bound can see. */
    br_stage_payload(0, BOOT_PAYLOAD_INITRD, 0x10000000ull, 0x1000ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_INITRD, 0x20000000ull, 0x1000ull, 0u);

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 1u),
                   "a duplicate singleton payload is not pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_get((int)base)->phys_start,
                   (unsigned long)0x10000000ull,
                   "the FIRST occurrence is the one kept");

    /* MODULE is the one repeatable type: a boot loading three drivers is
     * the normal case, so the same shape must NOT be rejected there. This
     * is the control -- without it the assertions above pass just as
     * happily against a rule that refuses every repeat. */
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 2;
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 0x1000ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x20000000ull, 0x1000ull, 0u);

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 2u),
                   "repeated MODULE payloads are both pinned");
}

static void test_boot_reserved_payload_budget(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 4;
    /* Four modules, each exactly at its 256 MiB per-type maximum, so every
     * one passes the per-descriptor contract. Three fit the 768 MiB
     * aggregate budget exactly and the fourth must not be pinned: bounding
     * each term does not bound the sum, and this is the case that proves
     * the sum is bounded too. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x50000000ull, 268435456ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_MODULE, 0x70000000ull, 268435456ull, 0u);

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 3u),
                   "the aggregate budget admits exactly three at-maximum modules");
}

static void test_boot_reserved_payload_required_first(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 4;
    /* Three optional at-maximum modules sit BEFORE a REQUIRED one and would
     * consume the whole budget in table order. The pass claims required
     * payloads first, so the boot outcome is decided by policy rather than
     * by where the producer happened to write the descriptor. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x50000000ull, 268435456ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_INITRD, 0x70000000ull, 0x1000ull,
                     BOOT_PAYLOAD_FLAG_REQUIRED);

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_get((int)base)->phys_start,
                   (unsigned long)0x70000000ull,
                   "the REQUIRED payload is claimed first, whatever its slot");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 3u),
                   "it is the trailing OPTIONAL payload that the budget drops");
}

static void test_boot_reserved_payload_is_pinned_query(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;

    TEST_KLOG_SUPPRESS("mm");
    (void)br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 4;
    /* Three at-maximum modules exhaust the 768 MiB budget; the fourth is in
     * contract, carries FLAG_RESERVED, and is NOT pinned. Re-deriving its
     * status from the descriptor alone says "fine": the length passes, the
     * flag is set, the capability is negotiated. Only the pass knows. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x50000000ull, 268435456ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_MODULE, 0x70000000ull, 0x1000ull, 0u);

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(0u, 0x10000000ull, 268435456ull), 1ul,
                   "an admitted descriptor reads as pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(3u, 0x70000000ull, 0x1000ull), 0ul,
                   "a budget-skipped descriptor reads as NOT pinned, though "
                   "its length, flag and capability all look fine");
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_MODULE, 0x1000ull), 1ul,
                   "and the length predicate alone would have admitted it -- "
                   "which is exactly why consumers must ask the pass");
}

/* The admission answer must be bound to the descriptor's IDENTITY, not to its
 * slot number.
 *
 * This is the case the [high] was about and the one the other tests do NOT
 * reach: within a single populate, each index carries at most one reservation,
 * so an index-only match still answers correctly. The aliasing appears when a
 * reservation from an EARLIER handoff is still in the table -- which is exactly
 * how a live caller reaches it, since boot_headless_authz_take_from() accepts a
 * caller-supplied boot_info, and exactly how one test came to pass on another
 * test's leftovers. Reverting the match to source_index alone must fail here. */
static void test_boot_reserved_pinned_is_identity_bound(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;

    TEST_KLOG_SUPPRESS("mm");
    (void)br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 0x1000ull, 0u);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x10000000ull, 0x1000ull), 1ul,
                   "the range that was actually pinned reads as pinned");
    /* Same slot, DIFFERENT range: a descriptor from another handoff. Nothing
     * pinned this, and answering by slot number alone would say otherwise. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x40000000ull, 0x1000ull), 0ul,
                   "a different range at the same slot is NOT pinned");
    /* Same slot and start, different length -- a truncated or extended claim
     * over memory whose real extent was pinned at something else. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x10000000ull, 0x2000ull), 0ul,
                   "a different length at the same slot and start is NOT pinned");
}

/* A singleton rejected by the BUDGET must not block a later occurrence that
 * would fit: the type is "seen" only once one is really pinned.
 *
 * THE LATER OCCURRENCE IS THE WHOLE TEST. An earlier version of this staged
 * only the rejected one, so reverting the exact defect it names -- recording
 * type_seen before the budget check instead of after -- would have left
 * every assertion green. A regression test that cannot fail on the
 * regression is not one. (Re-adversarial finding.) */
static void test_boot_reserved_singleton_after_budget_reject(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 5;
    /* Two at-maximum modules plus one at 256 MiB minus 8 KiB leaves exactly
     * 8 KiB of the 768 MiB budget. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x50000000ull,
                     268435456ull - 8192ull, 0u);
    /* A 16 KiB initrd does not fit the 8 KiB remainder -- budget-rejected. */
    br_stage_payload(3, BOOT_PAYLOAD_INITRD, 0x70000000ull, 16384ull, 0u);
    /* ...but this 4 KiB one does, and must be pinned. If the rejected
     * occurrence above had marked INITRD as seen, this would be refused as a
     * duplicate instead. */
    br_stage_payload(4, BOOT_PAYLOAD_INITRD, 0x71000000ull, 4096ull, 0u);

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       3u, 0x70000000ull, 16384ull), 0ul,
                   "the budget-rejected initrd is not pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       4u, 0x71000000ull, 4096ull), 1ul,
                   "a LATER initrd that fits IS pinned -- a budget reject must "
                   "not mark the singleton type as seen");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 4u),
                   "three modules plus the admissible initrd");
}

/* The same rule across the REQUIRED/optional pass boundary: a required
 * occurrence pinned in pass 0 must block an optional duplicate in pass 1. */
static void test_boot_reserved_singleton_across_passes(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 2;
    br_stage_payload(0, BOOT_PAYLOAD_INITRD, 0x10000000ull, 0x1000ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_INITRD, 0x20000000ull, 0x1000ull,
                     BOOT_PAYLOAD_FLAG_REQUIRED);

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 1u),
                   "only one occurrence of the singleton is pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       1u, 0x20000000ull, 0x1000ull), 1ul,
                   "and it is the REQUIRED one, claimed in pass 0");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x10000000ull, 0x1000ull), 0ul,
                   "the optional duplicate seen later in pass 1 is refused");
}

/* RANDOM_SEED is multi-descriptor by DOCUMENTED CONTRACT, so the singleton
 * rule must not touch it. bootx64.c appends its own seed beside an
 * earlier-stage one and boot_seed.c digest-chains every occurrence; pinning
 * only the first would have silently dropped the loader's fresh firmware and
 * CPU entropy and degraded the CSPRNG with nothing in the log to explain it.
 * (Round-4 re-adversarial, on a rule this section added.) */
static void test_boot_reserved_seed_is_repeatable(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 2;
    br_stage_payload(0, BOOT_PAYLOAD_RANDOM_SEED, 0x10000000ull, 4096ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_RANDOM_SEED, 0x20000000ull, 4096ull, 0u);

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 2u),
                   "BOTH seed descriptors are pinned -- the seed is not a singleton");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       1u, 0x20000000ull, 4096ull), 1ul,
                   "including the appended one, which carries the fresh entropy");
}

/* The reservation pass must pin a warm-update descriptor on EXACTLY the terms
 * the Phase-0 consumer accepts it. The pass once applied a strict subset --
 * no page-alignment check, no continuation-bit check -- so a descriptor that
 * cold-fell-back at Phase 0 was pinned for the life of the machine anyway.
 * (Round-6 re-adversarial.) */
static void test_boot_reserved_warm_update_matches_consumer(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    TEST_KLOG_SUPPRESS("boot");
    /* ONE candidate per fixture. Duplicates are refused wholesale now (see
     * the cardinality tests below), so predicate parity has to be asserted
     * one descriptor at a time -- which is also the only shape that isolates
     * WHICH rule refused. */
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    /* Admissible: page-multiple start and length, no unknown continuation
     * bits. This is the control -- without it the refusals below would pass
     * against a rule that pins nothing. */
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x10000000ull, 0x1000ull), 1ul,
                   "admissible warm pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 1u),
                   "only entry added");

    /* Not page-aligned: the consumer refuses it, so the pass must not pin it. */
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x20000100ull, 0x1000ull, 0u);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x20000100ull, 0x1000ull), 0ul,
                   "unaligned: not pinned");

    /* An unknown continuation bit: likewise refused by the consumer. */
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x30000000ull, 0x1000ull,
                     0x80000000u);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x30000000ull, 0x1000ull), 0ul,
                   "unknown cont bit: not pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)base,
                   "refused adds no entry");
}

/* Two admissible warm-update descriptors is a MALFORMED handoff, not a choice
 * to arbitrate: the incoming kernel cannot know which preserved region is
 * authoritative. Both phases refuse both, and cold init is the safe outcome.
 * The optional-first / REQUIRED-second arrangement is the specific shape that
 * defeated a first-admissible-wins rule -- required-first pinning would take
 * slot 1 while a table-order consumer takes slot 0. */
static void test_boot_reserved_warm_update_duplicates_refused(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    const struct boot_warm_update_sel *sel;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    TEST_KLOG_SUPPRESS("boot");
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 2;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x20000000ull, 0x1000ull,
                     BOOT_PAYLOAD_FLAG_REQUIRED);

    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_AMBIGUOUS,
                   "two candidates is AMBIGUOUS");
    TEST_ASSERT_EQ((unsigned long)sel->candidate_count, 2ul,
                   "count is recorded");

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x10000000ull, 0x1000ull), 0ul,
                   "optional dup not pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       1u, 0x20000000ull, 0x1000ull), 0ul,
                   "REQUIRED not pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)base,
                   "ambiguous pins nothing");


    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 2;
    /* Perfectly admissible on its own -- proven by the parity test above. */
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);
    /* Unknown continuation bit: refused by the per-descriptor validator. */
    br_stage_payload(1, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x20000000ull, 0x1000ull,
                     0x80000000u);

    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_AMBIGUOUS,
                   "admissible+malformed=two");

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x10000000ull, 0x1000ull), 0ul,
                   "fail closed, none pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)base,
                   "ambiguous pins nothing");


    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = 0u;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);

    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_NONE,
                   "no BOOT_FLAG_WARM_UPDATE means no selection");
    TEST_ASSERT_EQ((unsigned long)sel->candidate_count, 0ul,
                   "and the scan stops before counting candidates");

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)base,
                   "so nothing of that type is pinned");

}

/* Cardinality is counted BEFORE admissibility. A malformed second type-9
 * descriptor still means the producer published two preserved-state regions,
 * and boot_payload_validate() lets an OPTIONAL one through with unknown flag
 * bits by design (src/kernel/main/boot_payload.c), so this input really does
 * reach the selection. */

/* The integration assertion the section exists for: both phases agree on the
 * descriptor's IDENTITY, not merely on a count. The selection names an index
 * and a range; the reservation table must hold that exact index and range and
 * no other warm-update entry. */
static void test_boot_reserved_warm_update_identity_agrees(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    const struct boot_warm_update_sel *sel;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    TEST_KLOG_SUPPRESS("boot");
    /* ADDRESSES INSIDE THE FIXTURE'S OWN DECLARED RAM, never the host's. The
     * warm descriptor pins only when the fixture's memory map backs it, so an
     * address derived from the host frame count would leave the fixture's RAM
     * on a large guest and fail for a reason unrelated to descriptor identity
     * (the same host dependence a fixed 1.25 GiB address once had at 1 GiB). */
    uint64_t mid = BR_FIXTURE_RAM_END / 2ull;
    uint64_t p0 = mid;
    uint64_t p1 = mid + 0x10000ull;
    uint64_t p2 = mid + 0x20000ull;

    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 3;
    /* A normal payload before and after, so the selected index is not 0 and a
     * test that merely counted entries could not tell the difference. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, p0, 0x1000ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_WARM_UPDATE_STATE, p1, 0x2000ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, p2, 0x1000ull, 0u);

    /* Phase 0's view, sealed. */
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_ONE,
                   "one admissible candidate");
    TEST_ASSERT_EQ((unsigned long)sel->index, 1ul,
                   "selection names index");
    TEST_ASSERT_EQ((unsigned long)sel->phys_start, (unsigned long)p1,
                   "base");
    TEST_ASSERT_EQ((unsigned long)sel->length, 0x2000ul,
                   "extent");

    /* The reservation pass's view, later, over the same handoff. */
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       sel->index, sel->phys_start, sel->length), 1ul,
                   "pass pinned the selected");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 3u),
                   "2 mod + 1 warm pinned");


    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);

    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_ONE,
                   "sealed SELECT_ONE");

    /* Someone moves the preserved region after the seal. */
    TEST_BOOT_INFO.payload_descriptors[0].phys_start = 0x70000000ull;

    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->mismatch, 1ul,
                   "mutation detected");
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_ONE,
                   "verdict not revoked");

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x10000000ull, 0x1000ull), 1ul,
                   "sealed range is pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x70000000ull, 0x1000ull), 0ul,
                   "mutated range is not");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 1u),
                   "one entry from seal");


    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);

    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_ONE,
                   "sealed with caps");

    /* The capability bit disappears between the two phases. */
    TEST_BOOT_INFO.caps_present = 0u;

    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x10000000ull, 0x1000ull), 1ul,
                   "caps loss keeps the pin");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 1u),
                   "nothing else pinned");

}

/* A handoff that MOVES after Phase 0 read it is detected -- but the
 * authorization is NOT withdrawn. By the time the reservation pass looks,
 * Phase 0 has already acted on SELECT_ONE, so revoking the verdict would leave
 * preserved state reattached and its pages returned to PMM. The pass pins the
 * SEALED range (memory is the only thing that costs) and `mismatch` is what
 * tells a reattach consumer to refuse. */

/* BOOT_CAP_PAYLOAD_DESCRIPTORS is one of the conditions the SELECTION already
 * evaluated, so the reservation of a sealed warm region must not sit behind a
 * second, live test of it: clearing the bit after Phase 0 would otherwise skip
 * the pin for a region Phase 0 was authorized to reattach. That is the
 * cross-phase split this section removes, reintroduced by a gate that looked
 * safe. (Codex test-coverage finding on this section.) */

/* SELECT_ONE is a decision; `pinned` is the commit. The reservation can refuse
 * the sealed range for a reason that has nothing to do with warm update -- an
 * overlap with the kernel boot stack is a fatal handoff error -- and a restore
 * consumer acting on the verdict alone would reattach over memory PMM was
 * never told to keep. (Round-2 adversarial finding.) */
static void test_boot_reserved_warm_update_pinned_is_the_commit(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    const struct boot_warm_update_sel *sel;
    boot_result_t r;

    TEST_KLOG_SUPPRESS("mm");
    TEST_KLOG_SUPPRESS("boot");

    /* Happy path: selected, then admitted. */
    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->pinned, 0ul,
                   "not yet a commit");
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)sel->pinned, 1ul,
                   "the pass commits it");

    /* The range collides with the kernel boot stack: the pass fatals and the
     * selection must never read as committed. */
    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    TEST_BOOT_INFO.kstack_base   = 0x10000000ull;
    TEST_BOOT_INFO.kstack_size   = 0x4000u;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10001000ull, 0x1000ull, 0u);
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_ONE,
                   "well-formed, unambiguous");
    r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)r, (unsigned long)BOOT_FATAL,
                   "kstack overlap fatal");
    TEST_ASSERT_EQ((unsigned long)err, (unsigned long)BOOT_RESERVED_ERR_OVERLAP,
                   "err: overlap");
    TEST_ASSERT_EQ((unsigned long)sel->pinned, 0ul,
                   "unadmitted: not committed");
    br_zero_fixture();
    {
        uint64_t past_end;


    past_end = BR_FIXTURE_RAM_END;   /* past the fixture's declared RAM: a gap */

    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, past_end, 0x1000ull, 0u);

    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_ONE,
                   "well-formed at Phase 0");

    r = boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)r, (unsigned long)BOOT_OK,
                   "unbacked: not fatal");
    TEST_ASSERT_EQ((unsigned long)sel->pinned, 0ul,
                   "never committed");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, past_end, 0x1000ull), 0ul,
                   "not in the table");
    br_zero_fixture();
    }

}

/* The continuation bits pick which subsystem restores run, so swapping one
 * recognized bit for another changes what a consumer would do while leaving
 * the range identical. The seal covers them, and the mutation check compares
 * them. (Round-2 adversarial finding.) */
static void test_boot_reserved_warm_update_seals_continuation_bits(void)
{
    const struct boot_warm_update_sel *sel;

    TEST_KLOG_SUPPRESS("mm");
    TEST_KLOG_SUPPRESS("boot");
    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull,
                     BOOT_WARM_UPDATE_CONT_FD_TABLE);

    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)(sel->flags
                                   & (uint32_t)BOOT_WARM_UPDATE_CONT_MASK_KNOWN),
                   (unsigned long)BOOT_WARM_UPDATE_CONT_FD_TABLE,
                   "cont bits sealed");

    /* Same range, same index, different recognized continuation bit. */
    TEST_BOOT_INFO.payload_descriptors[0].flags =
        (TEST_BOOT_INFO.payload_descriptors[0].flags
         & ~(uint32_t)BOOT_WARM_UPDATE_CONT_FD_TABLE)
        | (uint32_t)BOOT_WARM_UPDATE_CONT_HW_QUEUES;

    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->mismatch, 1ul,
                   "cont-bit swap: mutation");
    TEST_ASSERT_EQ((unsigned long)(sel->flags
                                   & (uint32_t)BOOT_WARM_UPDATE_CONT_MASK_KNOWN),
                   (unsigned long)BOOT_WARM_UPDATE_CONT_FD_TABLE,
                   "sealed bits unchanged");
    br_zero_fixture();



    /* Value-only mutation: same range, same flags, different CRC. */
    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull,
                     BOOT_PAYLOAD_FLAG_CHECKSUMMED);
    TEST_BOOT_INFO.payload_descriptors[0].checksum = 0xC0FFEEull;
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->checksum, 0xC0FFEEul,
                   "CRC sealed");
    TEST_BOOT_INFO.payload_descriptors[0].checksum = 0xDEADBEEFull;
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->mismatch, 1ul,
                   "CRC-only swap: mutation");
    TEST_ASSERT_EQ((unsigned long)sel->checksum, 0xC0FFEEul,
                   "sealed CRC unchanged");

    /* Flag-only mutation: clearing CHECKSUMMED would skip verification. */
    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull,
                     BOOT_PAYLOAD_FLAG_CHECKSUMMED);
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)(sel->flags
                                   & (uint32_t)BOOT_PAYLOAD_FLAG_CHECKSUMMED),
                   (unsigned long)BOOT_PAYLOAD_FLAG_CHECKSUMMED,
                   "CHECKSUMMED bit sealed");
    TEST_BOOT_INFO.payload_descriptors[0].flags &=
        ~(uint32_t)BOOT_PAYLOAD_FLAG_CHECKSUMMED;
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->mismatch, 1ul,
                   "clearing it: mutation");
    br_zero_fixture();

}

/* The checksum decides whether preserved bytes are verified at all, so leaving
 * it in the live descriptor let a mutation change what a consumer DOES while
 * every compared field stayed identical. Both the CHECKSUMMED bit and the
 * value itself are sealed and compared. (Round-3 adversarial finding.) */

/* `pinned` is a positive claim about memory, so it must not certify a range
 * PMM never touches. pmm_mark_region_used() clamps only the END of a range,
 * so one starting past the last tracked frame marks nothing and reports
 * nothing. The out-of-range address is DERIVED from the live frame count
 * rather than hardcoded, so the assertion means the same thing on any host.
 * (Round-3 adversarial finding.) */

/* Every sole-candidate refusal class, asserted at the SELECTION rather than
 * at the reservation pass. The pass has its own prefilters for several of
 * these (FLAG_RESERVED, zero length, the length contract), so a selector that
 * wrongly returned ONE would authorize Phase 0 while the pass still skipped
 * the range -- and every "not pinned" assertion elsewhere would stay green.
 * (Codex test-coverage finding on this section.) */
static void test_boot_warm_update_selection_refusal_classes(void)
{
    struct wu_case {
        uint64_t phys;
        uint64_t len;
        uint32_t extra_flags;
        uint32_t clear_flags;
        uint32_t expect_err;
        const char *what;
    };
    static const struct wu_case cases[] = {
        { 0x10000000ull, 0ull,     0u,          0u,
          (uint32_t)BOOT_WARM_UPDATE_ERR_EMPTY,             "zero length" },
        { 0x10000000ull, 0x1000ull, 0u,
          (uint32_t)BOOT_PAYLOAD_FLAG_RESERVED,
          (uint32_t)BOOT_WARM_UPDATE_ERR_MISSING_FLAGS,     "no RESERVED flag" },
        { 0x10000000ull, 0x1000ull, 0u,
          (uint32_t)BOOT_PAYLOAD_FLAG_VALID,
          (uint32_t)BOOT_WARM_UPDATE_ERR_MISSING_FLAGS,     "no VALID flag" },
        /* Page-aligned and inside the type contract, so every per-FIELD rule
         * passes and only the whole-RANGE rule catches it. Occupies the slot
         * the unaligned-base case had: alignment is already asserted against
         * the predicate by test_boot_wu_unaligned_phys, and the selection only
         * calls that predicate, while the wrap rule was covered nowhere.
         * (Kernel-quality audit.) */
        { 0xFFFFFFFFFFFFE000ull, 0x4000ull, 0u, 0u,
          (uint32_t)BOOT_WARM_UPDATE_ERR_RANGE_WRAP,        "range wraps" },
        { 0x10000000ull, 0x1800ull, 0u,          0u,
          (uint32_t)BOOT_WARM_UPDATE_ERR_UNALIGNED,         "bad length granularity" },
        { 0x10000000ull, 0x8000000ull, 0u,       0u,
          (uint32_t)BOOT_WARM_UPDATE_ERR_LENGTH_CONTRACT,   "past the contract" },
        { 0x10000000ull, 0x1000ull, 0x80000000u, 0u,
          (uint32_t)BOOT_WARM_UPDATE_ERR_UNKNOWN_CONT_FLAG, "unknown cont bit" },
    };
    uint32_t c;

    TEST_KLOG_SUPPRESS("mm");
    TEST_KLOG_SUPPRESS("boot");
    for (c = 0u; c < (uint32_t)(sizeof(cases) / sizeof(cases[0])); c++) {
        enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
        const struct boot_warm_update_sel *sel;
        uint32_t base = br_baseline_count();

        TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
        TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
        TEST_BOOT_INFO.payload_count = 1;
        br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, cases[c].phys,
                         cases[c].len, cases[c].extra_flags);
        TEST_BOOT_INFO.payload_descriptors[0].flags &= ~cases[c].clear_flags;

        sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
        TEST_ASSERT_EQ((unsigned long)sel->verdict,
                       (unsigned long)BOOT_WARM_UPDATE_SELECT_NONE,
                       cases[c].what);
        TEST_ASSERT_EQ((unsigned long)sel->candidate_count, 1ul,
                       "counted then refused");
        TEST_ASSERT_EQ((unsigned long)sel->error,
                       (unsigned long)cases[c].expect_err,
                       "err names the rule");

        (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
        TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                       (unsigned long)base,
                       "refused pins nothing");
    }
}

/* The selection's own input boundaries: NULL, absent capability, an empty
 * table, a count past the array bound, and a candidate in the LAST slot. */
static void test_boot_warm_update_selection_input_bounds(void)
{
    const struct boot_warm_update_sel *sel;

    TEST_KLOG_SUPPRESS("mm");
    TEST_KLOG_SUPPRESS("boot");

    br_zero_fixture();
    sel = boot_warm_update_selection_get((const struct boot_info *)0);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_NONE,
                   "NULL selects nothing");

    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = 0u;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_NONE,
                   "no caps: skipped");

    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 0;
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_NONE,
                   "empty: no candidate");
    TEST_ASSERT_EQ((unsigned long)sel->candidate_count, 0ul,
                   "says so");

    /* A count past the array bound is a malformed handoff. The scan must
     * refuse rather than read off the end of payload_descriptors[]. */
    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = (uint32_t)BOOT_PAYLOAD_MAX + 1u;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_NONE,
                   "count>MAX refuses");
    TEST_ASSERT_EQ((unsigned long)sel->candidate_count, 0ul,
                   "walked no slot");

    /* The last legal slot is reachable: an off-by-one in the scan bound would
     * report zero candidates here and nothing else would notice. */
    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = (uint32_t)BOOT_PAYLOAD_MAX;
    br_stage_payload((uint32_t)BOOT_PAYLOAD_MAX - 1u,
                     BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)sel->verdict,
                   (unsigned long)BOOT_WARM_UPDATE_SELECT_ONE,
                   "last slot reachable");
    TEST_ASSERT_EQ((unsigned long)sel->index,
                   (unsigned long)((uint32_t)BOOT_PAYLOAD_MAX - 1u),
                   "real index");
    br_zero_fixture();
}

/* With the flag clear there is no warm update to select, however well-formed
 * the descriptor is -- the gate lives inside the shared selection now, so this
 * also proves the pass did not keep a second copy of it. */

/* Warm-update bypasses the aggregate budget, so it must not be CHARGED to it
 * either -- otherwise normal payload admission depends on where the warm
 * descriptor happens to sit in the table. The two arrangements below hold the
 * identical set of descriptors and must admit the identical normal payloads.
 * (Round-7 re-adversarial.) */
static void test_boot_reserved_warm_update_order_independent(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base, warm_first, warm_last;

    TEST_KLOG_SUPPRESS("mm");
    TEST_KLOG_SUPPRESS("boot");

    /* Warm descriptor FIRST, then exactly the full 768 MiB of modules. */
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 4;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x08000000ull, 67108864ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_MODULE, 0x50000000ull, 268435456ull, 0u);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    warm_first = (uint32_t)boot_reserved_count() - base;

    /* The same four descriptors, warm LAST. */
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 4;
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x50000000ull, 268435456ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x08000000ull, 67108864ull, 0u);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    warm_last = (uint32_t)boot_reserved_count() - base;

    TEST_ASSERT_EQ((unsigned long)warm_first, (unsigned long)warm_last,
                   "descriptor ORDER does not change what is admitted");
    TEST_ASSERT_EQ((unsigned long)warm_first, (unsigned long)4u,
                   "and all four are admitted -- warm bytes never consumed the "
                   "normal payload budget");
}

static void test_boot_payload_length_contract_matrix(void)
{
    /* The pure predicate, at every boundary that carries a decision. */
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_MODULE, 268435456ull), 1ul,
                   "a module exactly at its maximum is reservable");
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_MODULE, 268435457ull), 0ul,
                   "one byte over the maximum is not");
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_MODULE, 0ull), 0ul,
                   "a zero length is never reservable");

    /* The seed keeps a MINIMUM as well, and it is the header size. A
     * shorter payload cannot even carry the header the consumer parses. */
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_RANDOM_SEED, 32ull), 1ul,
                   "a seed exactly at the header size is reservable");
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_RANDOM_SEED, 31ull), 0ul,
                   "one byte under the seed minimum is not");
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_RANDOM_SEED, 16384ull), 1ul,
                   "a seed at BOOT_SEED_PAYLOAD_CAP is reservable");
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_RANDOM_SEED, 16385ull), 0ul,
                   "one byte over the seed cap is not");

    /* The seed's own named predicate must agree with the table exactly --
     * it delegates, and this is what would catch it being forked back into
     * a second copy of the bound. */
    TEST_ASSERT_EQ((unsigned long)boot_seed_length_reservable(16384ull), 1ul,
                   "boot_seed_length_reservable agrees with the table at the cap");
    TEST_ASSERT_EQ((unsigned long)boot_seed_length_reservable(16385ull), 0ul,
                   "and agrees one byte past it");

    /* An EXACT-length type accepts only that length. */
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_HEADLESS_AUTHZ, 152ull), 1ul,
                   "the authorization blob is reservable at its exact length");
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_HEADLESS_AUTHZ, 151ull), 0ul,
                   "a short authorization is not a weaker one, it is malformed");

    /* NOT-RESERVABLE types refuse every length, including plausible ones --
     * asserting only a huge length here would pass against a rule that
     * merely capped them. */
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_TPM_EVENT_LOG, 4096ull), 0ul,
                   "the TPM event log is not reservable as a descriptor at all");
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_USB_HANDOVER, 1ull), 0ul,
                   "nor is USB handover state, at any length");

    /* Wire values outside the enum are refused rather than cast. */
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       (uint32_t)BOOT_PAYLOAD_NONE, 4096ull), 0ul,
                   "the empty-slot sentinel is never a reservable payload");
    TEST_ASSERT_EQ((unsigned long)boot_payload_length_reservable(
                       0xFFFFFFFFu, 4096ull), 0ul,
                   "an unknown wire type is refused, not cast to an enum");
}

static void test_boot_payload_budget_admits_bounds(void)
{
    TEST_ASSERT_EQ((unsigned long)boot_payload_budget_admits(0ull, 805306368ull),
                   1ul, "an empty budget admits exactly the whole ceiling");
    TEST_ASSERT_EQ((unsigned long)boot_payload_budget_admits(0ull, 805306369ull),
                   0ul, "but not one byte more");
    TEST_ASSERT_EQ((unsigned long)boot_payload_budget_admits(805306368ull, 1ull),
                   0ul, "a full budget admits nothing further");
    /* Overflow safety: the rule is a subtraction against the remaining
     * budget precisely so a hostile length near UINT64_MAX cannot wrap the
     * addition and read as an accept. */
    TEST_ASSERT_EQ((unsigned long)boot_payload_budget_admits(
                       1ull, 0xFFFFFFFFFFFFFFFFull), 0ul,
                   "a near-UINT64_MAX length cannot wrap into an accept");
}

/* The addressability predicate itself, one verdict per shape. */
static void test_boot_reserved_range_backing_verdicts(void)
{
    uint32_t t = 99u;

    br_zero_fixture();
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x10000000ull, 0x1000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_BACKED, "inside declared RAM");
    TEST_ASSERT_EQ((unsigned long)t, 0ul, "no offending type on success");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, BR_FIXTURE_RAM_END - 0x1000ull, 0x2000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_GAP, "runs off the end of RAM");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, BOOT_INFO_EARLY_MAP_END - 0x1000ull, 0x2000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_ABOVE_MAP, "crosses the 4 GiB identity map");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x1000ull, 0u, 0, &t),
                   (unsigned long)BOOT_RANGE_GAP, "zero length is never backed");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, ~0ull - 0xFFFull, 0x2000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_GAP, "a wrapping range is never backed");

    /* A forbidden entry laid OVER readable RAM vetoes it, whatever the order. */
    br_set_map(1u, 0x40000000ull, 0x1000ull, UEFI_MMAP_MMIO);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x40000000ull, 0x1000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_FORBIDDEN, "MMIO below the RAM end");
    TEST_ASSERT_EQ((unsigned long)t, (unsigned long)UEFI_MMAP_MMIO, "names the type");
    br_set_map(2u, 0x41000000ull, 0x1000ull, UEFI_MMAP_RESERVED);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x40FFF000ull, 0x2000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_FORBIDDEN, "reserved memory is not on the allowlist");
    br_set_map(3u, 0x42000000ull, 0x1000ull, BOOT_MMAP_WARM_UPDATE);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x42000000ull, 0x1000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_FORBIDDEN, "type 15 refused for an ordinary payload");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x42000000ull, 0x1000ull, 1, &t),
                   (unsigned long)BOOT_RANGE_BACKED, "type 15 admitted for the warm extent");

    /* Unsorted, adjacent entries of two readable types cover a range together. */
    br_zero_fixture();
    TEST_BOOT_INFO.mmap_count = 0u;
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x30000000ull, 0x1000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_NO_MAP, "no map refuses everything");
    br_set_map(0u, 0x30001000ull, 0x1000ull, UEFI_MMAP_LOADER_DATA);
    br_set_map(1u, 0x30000000ull, 0x1000ull, UEFI_MMAP_CONVENTIONAL);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x30000000ull, 0x2000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_BACKED, "unsorted adjacent entries cover it");
    br_set_map(1u, 0x2FFFF000ull, 0x1000ull, UEFI_MMAP_CONVENTIONAL);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x30000000ull, 0x2000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_GAP, "a hole at the start is a gap");

    /* A one-page hole BETWEEN backed endpoints, in both entry orders: a check
     * of the endpoints alone, or a jump across the gap, would pass this. */
    br_zero_fixture();
    TEST_BOOT_INFO.mmap_count = 0u;
    br_set_map(0u, 0x30000000ull, 0x1000ull, UEFI_MMAP_CONVENTIONAL);
    br_set_map(1u, 0x30002000ull, 0x1000ull, UEFI_MMAP_CONVENTIONAL);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x30000000ull, 0x3000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_GAP, "interior hole, ascending entries");
    TEST_BOOT_INFO.mmap_count = 0u;
    br_set_map(0u, 0x30002000ull, 0x1000ull, UEFI_MMAP_CONVENTIONAL);
    br_set_map(1u, 0x30000000ull, 0x1000ull, UEFI_MMAP_CONVENTIONAL);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x30000000ull, 0x3000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_GAP, "interior hole, descending entries");
    br_set_map(2u, 0x30001000ull, 0x1000ull, UEFI_MMAP_LOADER_CODE);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x30000000ull, 0x3000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_BACKED, "the hole closed: backed");
    /* Forbidden entries that only TOUCH either exclusive bound do not overlap. */
    br_set_map(3u, 0x2FFFF000ull, 0x1000ull, UEFI_MMAP_MMIO);
    br_set_map(4u, 0x30003000ull, 0x1000ull, UEFI_MMAP_UNUSABLE);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x30000000ull, 0x3000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_BACKED, "adjacent forbidden entries do not veto");

    /* A readable entry whose base + length wraps covers nothing: neither as
     * the sole backing nor by bridging a gap between two good entries. */
    br_zero_fixture();
    TEST_BOOT_INFO.mmap_count = 0u;
    br_set_map(0u, 0x100000ull, ~0ull, UEFI_MMAP_CONVENTIONAL);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x200000ull, 0x1000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_GAP, "a wrapping entry is not backing");
    br_set_map(1u, 0x30000000ull, 0x1000ull, UEFI_MMAP_CONVENTIONAL);
    br_set_map(2u, 0x30002000ull, 0x1000ull, UEFI_MMAP_CONVENTIONAL);
    br_set_map(3u, 0x30001000ull, ~0ull, UEFI_MMAP_CONVENTIONAL);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x30000000ull, 0x3000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_GAP, "a wrapping entry does not bridge a gap");
    /* ...but it still vetoes when its type is forbidden. */
    br_set_map(4u, 0x2FFFF000ull, ~0ull, UEFI_MMAP_MMIO);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_range_backing(
                       &TEST_BOOT_INFO, 0x30000000ull, 0x1000ull, 0, &t),
                   (unsigned long)BOOT_RANGE_FORBIDDEN, "a wrapping forbidden entry vetoes");
    br_zero_fixture();
}

/* The reservation pass applies the predicate to every admitted payload, and a
 * refusal is a decline (the boot continues) that leaves the slot unpinned. */
static void test_boot_reserved_payload_must_be_backed(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    base = br_baseline_count();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    br_set_map(1u, 0x40000000ull, 0x1000ull, UEFI_MMAP_MMIO);
    TEST_BOOT_INFO.payload_count = 4;
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x40000000ull, 0x1000ull, 0u);   /* MMIO */
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, BR_FIXTURE_RAM_END, 0x1000ull, 0u); /* past RAM */
    /* A refused first INITRD must not block a valid later one. */
    br_stage_payload(2, BOOT_PAYLOAD_INITRD, BR_FIXTURE_RAM_END + 0x1000ull, 0x1000ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_INITRD, 0x20000000ull, 0x1000ull, 0u);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err),
                   (unsigned long)BOOT_OK, "an unbacked payload is declined, not fatal");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x40000000ull, 0x1000ull), 0ul, "MMIO payload not pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       1u, BR_FIXTURE_RAM_END, 0x1000ull), 0ul, "payload past RAM not pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       3u, 0x20000000ull, 0x1000ull), 1ul,
                   "a later backed INITRD is pinned: the refused one was not marked seen");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)(base + 1u),
                   "only the backed payload entered the table");

    /* A truncated map is checked against the entries it carries: an omitted
     * interval below the RAM end is a gap, not a pass. */
    br_zero_fixture();
    TEST_BOOT_INFO.mmap_count = 0u;
    br_set_map(0u, 0u, 0x10000000ull, UEFI_MMAP_CONVENTIONAL);
    br_set_map(1u, 0x30000000ull, 0x10000000ull, UEFI_MMAP_CONVENTIONAL);
    TEST_BOOT_INFO.mmap_truncated = 1u;
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x20000000ull, 0x1000ull, 0u);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x20000000ull, 0x1000ull), 0ul,
                   "truncated map: an omitted interval is refused");
    /* ...while a range the retained entries DO cover is still admitted. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x30000000ull, 0x1000ull, 0u);
    boot_reserved_reset_for_test();
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x30000000ull, 0x1000ull), 1ul,
                   "truncated map: a covered range is pinned");

    /* A payload whose only backing is a wrapping entry is refused. */
    br_zero_fixture();
    TEST_BOOT_INFO.mmap_count = 0u;
    br_set_map(0u, 0u, 0x10000000ull, UEFI_MMAP_CONVENTIONAL);
    br_set_map(1u, 0x20000000ull, ~0ull, UEFI_MMAP_CONVENTIONAL);
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x20001000ull, 0x1000ull, 0u);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x20001000ull, 0x1000ull), 0ul,
                   "wrapping-entry backing is refused at admission");
    br_zero_fixture();
}

/* The warm extent goes through the same predicate in the reservation pass,
 * with its one exception: memory the producer tagged BOOT_MMAP_WARM_UPDATE. */
static void test_boot_reserved_warm_extent_backing(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    const struct boot_warm_update_sel *sel;
    uint32_t i, found;

    TEST_KLOG_SUPPRESS("mm");
    br_zero_fixture();
    /* Backed ONLY by the type-15 entry: no conventional RAM under it. */
    TEST_BOOT_INFO.mmap_count = 0u;
    br_set_map(0u, 0u, 0x42000000ull, UEFI_MMAP_CONVENTIONAL);
    br_set_map(1u, 0x42000000ull, 0x1000ull, BOOT_MMAP_WARM_UPDATE);
    br_set_map(2u, 0x42001000ull, BR_FIXTURE_RAM_END - 0x42001000ull, UEFI_MMAP_CONVENTIONAL);
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x42000000ull, 0x1000ull, 0u);
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err),
                   (unsigned long)BOOT_OK, "warm extent on type-15 memory: ok");
    TEST_ASSERT_EQ((unsigned long)sel->pinned, 1ul, "committed");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x42000000ull, 0x1000ull), 1ul, "pinned");
    for (i = 0u, found = 0u; i < boot_reserved_count(); i++)
        if (boot_reserved_get(i)->phys_start == 0x42000000ull
            && boot_reserved_get(i)->length == 0x1000ull)
            found++;
    TEST_ASSERT_EQ((unsigned long)found, 1ul, "exactly one table entry");

    /* The exception admits type 15 only: MMIO laid over it still vetoes. */
    br_zero_fixture();
    TEST_BOOT_INFO.mmap_count = 0u;
    br_set_map(0u, 0u, 0x42000000ull, UEFI_MMAP_CONVENTIONAL);
    br_set_map(1u, 0x42000000ull, 0x1000ull, BOOT_MMAP_WARM_UPDATE);
    br_set_map(2u, 0x42000000ull, 0x1000ull, UEFI_MMAP_MMIO);
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x42000000ull, 0x1000ull, 0u);
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err),
                   (unsigned long)BOOT_OK, "MMIO under the warm extent: declined, not fatal");
    TEST_ASSERT_EQ((unsigned long)sel->pinned, 0ul, "never committed");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x42000000ull, 0x1000ull), 0ul, "not pinned");

    /* A wrapping type-15 entry is not backing for the warm extent either. */
    br_zero_fixture();
    TEST_BOOT_INFO.mmap_count = 0u;
    br_set_map(0u, 0u, 0x42000000ull, UEFI_MMAP_CONVENTIONAL);
    br_set_map(1u, 0x42000000ull, ~0ull, BOOT_MMAP_WARM_UPDATE);
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.flags         = BOOT_FLAG_WARM_UPDATE;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x42000000ull, 0x1000ull, 0u);
    sel = boot_warm_update_selection_get(&TEST_BOOT_INFO);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)sel->pinned, 0ul, "wrapping warm backing: never committed");
    br_zero_fixture();
}

/* The dump's per-entry tracking verdict, and the copy the freeze is built on. */
static void test_boot_reserved_tracked_and_copy(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    struct boot_reserved_region copy[4];
    struct boot_reserved_region saved[4];
    uint32_t n, total, i, k;
    uint8_t *raw;

    TEST_ASSERT_EQ((unsigned long)boot_reserved_tracked_class(0x1000ull, 0x2000ull, 16u),
                   (unsigned long)BOOT_RESERVED_TRACKED_FULL, "inside the ceiling");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_tracked_class(0xF000ull, 0x2000ull, 16u),
                   (unsigned long)BOOT_RESERVED_TRACKED_PARTIAL, "straddles the ceiling");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_tracked_class(0x10000ull, 0x1000ull, 16u),
                   (unsigned long)BOOT_RESERVED_TRACKED_NONE, "starts at the ceiling");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_tracked_class(0x1000ull, 0u, 16u),
                   (unsigned long)BOOT_RESERVED_TRACKED_NONE, "zero length tracks nothing");

    br_zero_fixture();
    TEST_BOOT_INFO.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    TEST_BOOT_INFO.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x20000000ull, 0x1000ull, 0u);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    total = boot_reserved_count();
    TEST_ASSERT(total >= 2u, "baseline plus the payload");
    /* Capacity: nothing past `cap` is written. */
    raw = (uint8_t *)copy;
    for (k = 0u; k < sizeof(copy); k++)
        raw[k] = 0xA5u;
    TEST_ASSERT_EQ((unsigned long)boot_reserved_copy_table(copy, 0u), 0ul, "cap 0 copies nothing");
    for (k = 0u; k < sizeof(copy) && raw[k] == 0xA5u; k++)
        ;
    TEST_ASSERT_EQ((unsigned long)k, (unsigned long)sizeof(copy), "cap 0 writes nothing");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_copy_table(copy, 1u), 1ul, "copy honours cap");
    for (k = sizeof(copy[0]); k < sizeof(copy) && raw[k] == 0xA5u; k++)
        ;
    TEST_ASSERT_EQ((unsigned long)k, (unsigned long)sizeof(copy), "nothing written past cap");
    n = boot_reserved_copy_table(copy, 4u);
    TEST_ASSERT_EQ((unsigned long)n, (unsigned long)(total < 4u ? total : 4u), "copies the table");
    for (i = 0u; i < n; i++) {
        const struct boot_reserved_region *e = boot_reserved_get(i);
        TEST_ASSERT_EQ(copy[i].phys_start, e->phys_start, "copy: phys_start");
        TEST_ASSERT_EQ(copy[i].length, e->length, "copy: length");
        TEST_ASSERT_EQ((unsigned long)copy[i].kind, (unsigned long)e->kind, "copy: kind");
        TEST_ASSERT_EQ((unsigned long)copy[i].source_index, (unsigned long)e->source_index,
                       "copy: source_index");
        saved[i] = copy[i];
    }
    /* The copy is independent of the live table: a wipe leaves it intact. */
    boot_reserved_reset_for_test();
    TEST_ASSERT_EQ((unsigned long)boot_reserved_copy_table(saved, 4u), 0ul,
                   "an empty table copies nothing");
    for (i = 0u; i < n; i++)
        TEST_ASSERT_EQ(copy[i].phys_start, saved[i].phys_start, "copy survives the wipe");

    /* load_table puts a saved table back exactly and zeroes the tail. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_load_table(saved, n), (unsigned long)n,
                   "load returns the count");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)n, "count restored");
    for (i = 0u; i < n; i++) {
        TEST_ASSERT_EQ(boot_reserved_get(i)->phys_start, saved[i].phys_start, "load: phys_start");
        TEST_ASSERT_EQ(boot_reserved_get(i)->length, saved[i].length, "load: length");
        TEST_ASSERT_EQ((unsigned long)boot_reserved_get(i)->kind, (unsigned long)saved[i].kind,
                       "load: kind");
        TEST_ASSERT_EQ((unsigned long)boot_reserved_get(i)->source_index,
                       (unsigned long)saved[i].source_index, "load: source_index");
    }
    TEST_ASSERT(boot_reserved_get(n) == (const struct boot_reserved_region *)0,
                "nothing past the loaded count");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_load_table(
                       (const struct boot_reserved_region *)0, 4u), 0ul,
                   "NULL loads an empty table");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), 0ul, "empty after NULL load");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_copy_table((struct boot_reserved_region *)0, 4u),
                   0ul, "NULL destination copies nothing");
    br_zero_fixture();
}

/* Stage the three boot_info v14 UKI payload fields on the fixture. */
static void br_stage_uki(uint64_t ia, uint64_t is, uint64_t ra, uint64_t rs,
                         uint64_t ma, uint64_t ms)
{
    TEST_BOOT_INFO.uki_initrd_addr   = ia;
    TEST_BOOT_INFO.uki_initrd_size   = is;
    TEST_BOOT_INFO.uki_recovery_addr = ra;
    TEST_BOOT_INFO.uki_recovery_size = rs;
    TEST_BOOT_INFO.uki_modules_addr  = ma;
    TEST_BOOT_INFO.uki_modules_size  = ms;
}

/* UKI signed-payload sections: the signed UKI payload copies live in EfiLoaderData,
 * which pmm_init frees, so the pass must pin them -- and only them, only on a
 * UKI handoff, only when readable RAM inside the identity map. */
static void test_boot_reserved_uki_payloads_pinned(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    base = br_baseline_count();
    TEST_BOOT_INFO.flags = BOOT_FLAG_INVOKED_VIA_UKI;
    br_stage_uki(0x20000000ull, 4096ull, 0x20100000ull, 8192ull,
                 0x20200000ull, 512ull);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err),
                   (unsigned long)BOOT_OK, "three backed UKI payloads populate");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)(base + 3u),
                   "one table entry per UKI payload");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_INITRD, 0x20000000ull, 4096ull), 1ul,
                   ".initrd pinned at its exact range");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_RECOVERY, 0x20100000ull, 8192ull), 1ul,
                   ".recovery pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_MODULES, 0x20200000ull, 512ull), 1ul,
                   ".modules pinned");
    /* Identity is slot AND range: another slot's range, a shifted range, an
     * out-of-range slot and a zero length all answer no. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_RECOVERY, 0x20000000ull, 4096ull), 0ul,
                   "the .initrd range does not answer for .recovery");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_INITRD, 0x20000000ull, 4095ull), 0ul,
                   "a different length is a different range");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_COUNT, 0x20000000ull, 4096ull), 0ul,
                   "slot past the last is never pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_INITRD, 0x20000000ull, 0ull), 0ul,
                   "zero length is never pinned");
    /* A typed-payload query does not see a UKI entry, and vice versa. */
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x20000000ull, 4096ull), 0ul,
                   "a UKI entry is not a typed payload");
    /* PMM-internal disjointness covers the UKI kind: a bitmap placed over the
     * .modules copy is found (and pmm_init halts on it). */
    TEST_ASSERT(boot_reserved_check_payloads_disjoint(0x20200000ull, 0x1000ull,
                                                      "fake_bitmap") != 0u,
                "a UKI payload under a PMM-internal region is detected");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_check_payloads_disjoint(
                       0x30000000ull, 0x1000ull, "fake_bitmap"), 0ul,
                   "disjoint PMM-internal region passes");
    br_zero_fixture();
}

static void test_boot_reserved_uki_payloads_refused(void)
{
    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    uint32_t base;

    TEST_KLOG_SUPPRESS("mm");
    base = br_baseline_count();

    /* Fields on a handoff that did not come through the UKI path. */
    br_stage_uki(0x20000000ull, 4096ull, 0u, 0u, 0u, 0u);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err),
                   (unsigned long)BOOT_OK, "split-path UKI fields declined, not fatal");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_INITRD, 0x20000000ull, 4096ull), 0ul,
                   "UKI fields without BOOT_FLAG_INVOKED_VIA_UKI are not pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)base,
                   "nothing entered the table");

    /* Half-set fields and an over-cap size. */
    br_zero_fixture();
    TEST_BOOT_INFO.flags = BOOT_FLAG_INVOKED_VIA_UKI;
    br_stage_uki(0x20000000ull, 0u, 0u, 8192ull,
                 0x20200000ull, UKI_PAYLOAD_MAX_BYTES + 1ull);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err),
                   (unsigned long)BOOT_OK, "malformed UKI fields declined");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)base,
                   "address-only, size-only and over-cap entries all refused");
    /* The cap is inclusive: exactly UKI_PAYLOAD_MAX_BYTES is admitted. */
    br_zero_fixture();
    TEST_BOOT_INFO.flags = BOOT_FLAG_INVOKED_VIA_UKI;
    br_stage_uki(0x20000000ull, UKI_PAYLOAD_MAX_BYTES, 0u, 0u, 0u, 0u);
    (void)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_INITRD, 0x20000000ull, UKI_PAYLOAD_MAX_BYTES), 1ul,
                   "a payload of exactly the cap is pinned");

    /* Unbacked ranges: MMIO, past the fixture RAM, and above the 4 GiB
     * identity map even where the map calls it RAM. One refusal does not
     * cost the others: the valid .modules copy is still pinned. */
    br_zero_fixture();
    TEST_BOOT_INFO.flags = BOOT_FLAG_INVOKED_VIA_UKI;
    br_set_map(1u, 0x40000000ull, 0x1000ull, UEFI_MMAP_MMIO);
    br_set_map(2u, BOOT_INFO_EARLY_MAP_END, 0x10000000ull, UEFI_MMAP_CONVENTIONAL);
    br_stage_uki(0x40000000ull, 0x1000ull, BOOT_INFO_EARLY_MAP_END, 0x1000ull,
                 0x20200000ull, 512ull);
    TEST_ASSERT_EQ((unsigned long)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err),
                   (unsigned long)BOOT_OK, "unbacked UKI payloads declined");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_INITRD, 0x40000000ull, 0x1000ull), 0ul,
                   "MMIO-backed .initrd not pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_RECOVERY, BOOT_INFO_EARLY_MAP_END, 0x1000ull), 0ul,
                   ".recovery above the identity map not pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_uki_payload_is_pinned(
                       UKI_PAYLOAD_SLOT_MODULES, 0x20200000ull, 512ull), 1ul,
                   "the backed .modules copy is still pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(), (unsigned long)(base + 1u),
                   "only the backed copy entered the table");

    /* A UKI copy that overlaps another retained region is a fatal handoff
     * error, like every other entry: the boot stack is retained first. */
    br_zero_fixture();
    TEST_BOOT_INFO.flags       = BOOT_FLAG_INVOKED_VIA_UKI;
    TEST_BOOT_INFO.kstack_base = 0x20000000ull;
    TEST_BOOT_INFO.kstack_size = 0x10000u;
    br_stage_uki(0x20008000ull, 0x1000ull, 0u, 0u, 0u, 0u);
    err = BOOT_RESERVED_ERR_OK;
    TEST_ASSERT_EQ((unsigned long)boot_reserved_populate_from_info(&TEST_BOOT_INFO, &err),
                   (unsigned long)BOOT_FATAL, "UKI copy over the boot stack is fatal");
    TEST_ASSERT_EQ((unsigned long)err, (unsigned long)BOOT_RESERVED_ERR_OVERLAP,
                   "and reported as an overlap");
    br_zero_fixture();
}

static int br_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* The digest the kernel logs for each received UKI payload: FIPS 180-2
 * known-answer vectors, including the empty input and a two-block message. */
static void test_boot_reserved_digest_hex_vectors(void)
{
    char out[BOOT_RESERVED_DIGEST_HEX_LEN + 1u];
    static const char abc[] = "abc";
    static const char two[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

    boot_reserved_digest_hex(abc, 3u, out);
    TEST_ASSERT(br_streq(out,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "sha256(\"abc\") as lowercase hex");
    boot_reserved_digest_hex(abc, 0u, out);
    TEST_ASSERT(br_streq(out,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        "sha256 of the empty input");
    boot_reserved_digest_hex(two, sizeof(two) - 1u, out);
    TEST_ASSERT(br_streq(out,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
        "two-block message digest");
    TEST_ASSERT_EQ((unsigned long)out[BOOT_RESERVED_DIGEST_HEX_LEN], 0ul,
                   "digest string is NUL-terminated at 64 characters");
}

void test_register_boot_reserved(void)
{
    test_suite_register_cat("boot_reserved: warm-update budget is order-independent",
                            test_boot_reserved_warm_update_order_independent,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: warm-update pinning matches the consumer",
                            test_boot_reserved_warm_update_matches_consumer,
                            TEST_CAT_BOOT);
    test_suite_register_cat("wu: cardinality refusals",
                            test_boot_reserved_warm_update_duplicates_refused,
                            TEST_CAT_BOOT);
    test_suite_register_cat("wu: identity + seal integrity",
                            test_boot_reserved_warm_update_identity_agrees,
                            TEST_CAT_BOOT);
    test_suite_register_cat("wu: commit is not the verdict",
                            test_boot_reserved_warm_update_pinned_is_the_commit,
                            TEST_CAT_BOOT);
    test_suite_register_cat("wu: consumer fields are sealed",
                            test_boot_reserved_warm_update_seals_continuation_bits,
                            TEST_CAT_BOOT);
    test_suite_register_cat("wu: refusal classes",
                            test_boot_warm_update_selection_refusal_classes,
                            TEST_CAT_BOOT);
    test_suite_register_cat("wu: selection input bounds",
                            test_boot_warm_update_selection_input_bounds,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: RANDOM_SEED is repeatable",
                            test_boot_reserved_seed_is_repeatable, TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: pinned admission is identity-bound",
                            test_boot_reserved_pinned_is_identity_bound,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: payload pinned-admission query",
                            test_boot_reserved_payload_is_pinned_query,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: singleton across the REQUIRED pass boundary",
                            test_boot_reserved_singleton_across_passes,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: singleton after a budget reject",
                            test_boot_reserved_singleton_after_budget_reject,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: per-type payload length contract",
                            test_boot_reserved_payload_length_contract,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: singleton payload cardinality",
                            test_boot_reserved_payload_singleton,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: aggregate payload budget",
                            test_boot_reserved_payload_budget,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: REQUIRED payloads claim budget first",
                            test_boot_reserved_payload_required_first,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: per-type length contract matrix",
                            test_boot_payload_length_contract_matrix,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: aggregate budget bounds",
                            test_boot_payload_budget_admits_bounds,
                            TEST_CAT_BOOT);
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
    test_suite_register_cat("boot_reserved: boot stack entry",
                            test_boot_reserved_boot_stack_entry,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: boot stack absent when unpublished",
                            test_boot_reserved_boot_stack_absent_when_unpublished,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: boot stack overlap rejected",
                            test_boot_reserved_boot_stack_overlap_rejected,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: payload vs PMM-internal",
                            test_boot_reserved_payload_vs_pmm_internal,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: range backing verdicts",
                            test_boot_reserved_range_backing_verdicts,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: payload must be backed",
                            test_boot_reserved_payload_must_be_backed,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: warm extent backing",
                            test_boot_reserved_warm_extent_backing,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: tracked class and table copy",
                            test_boot_reserved_tracked_and_copy,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: UKI payloads pinned",
                            test_boot_reserved_uki_payloads_pinned,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: UKI payloads refused",
                            test_boot_reserved_uki_payloads_refused,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: received-payload digest",
                            test_boot_reserved_digest_hex_vectors,
                            TEST_CAT_BOOT);
}
