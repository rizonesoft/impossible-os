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
#include "kernel/entropy.h"   /* boot_seed_length_reservable -- must agree with the table */
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

    s_br_buf.kstack_base       = BR_KS_BASE;
    s_br_buf.kstack_size       = (uint32_t)BR_KS_SIZE;
    s_br_buf.kstack_guard_size = 0x1000u;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
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
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
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
    s_br_buf.kstack_base       = BOOT_INFO_PHYS_ADDR;
    s_br_buf.kstack_size       = (uint32_t)BR_KS_SIZE;
    s_br_buf.kstack_guard_size = 0x1000u;

    enum boot_reserved_error err = BOOT_RESERVED_ERR_OK;
    boot_result_t r = boot_reserved_populate_from_info(&s_br_buf, &err);
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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 0;
    (void)boot_reserved_populate_from_info(&s_br_buf, &err);
    n = (uint32_t)boot_reserved_count();
    br_zero_fixture();
    return n;
}

/* Stage one RESERVED payload descriptor at slot `i`. */
static void br_stage_payload(uint32_t i, uint32_t type, uint64_t phys,
                             uint64_t len, uint32_t extra_flags)
{
    s_br_buf.payload_descriptors[i].type       = type;
    s_br_buf.payload_descriptors[i].flags      = BOOT_PAYLOAD_FLAG_VALID |
                                                 BOOT_PAYLOAD_FLAG_RESERVED |
                                                 extra_flags;
    s_br_buf.payload_descriptors[i].phys_start = phys;
    s_br_buf.payload_descriptors[i].length     = len;
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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 4;

    /* A module EXACTLY at its 256 MiB maximum is pinned; one byte over is
     * not. The boundary is the assertion that matters: an off-by-one would
     * leave the contract nominally present and actually one byte wrong. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435457ull, 0u);
    /* A type with NO declared bound refuses ANY length, including a
     * plausible one. Both of these are "not reservable" in the table. */
    br_stage_payload(2, BOOT_PAYLOAD_TPM_EVENT_LOG,  0x50000000ull, 0x1000ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_NETWORK_CONFIG, 0x51000000ull, 0x1000ull, 0u);

    r = boot_reserved_populate_from_info(&s_br_buf, &err);

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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 2;
    /* Two initrds. INITRD is a singleton by meaning, so the second is a
     * malformed handoff; pinning it would let one type claim twice its
     * declared maximum, which no per-descriptor bound can see. */
    br_stage_payload(0, BOOT_PAYLOAD_INITRD, 0x10000000ull, 0x1000ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_INITRD, 0x20000000ull, 0x1000ull, 0u);

    (void)boot_reserved_populate_from_info(&s_br_buf, &err);

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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 2;
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 0x1000ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x20000000ull, 0x1000ull, 0u);

    (void)boot_reserved_populate_from_info(&s_br_buf, &err);

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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 4;
    /* Four modules, each exactly at its 256 MiB per-type maximum, so every
     * one passes the per-descriptor contract. Three fit the 768 MiB
     * aggregate budget exactly and the fourth must not be pinned: bounding
     * each term does not bound the sum, and this is the case that proves
     * the sum is bounded too. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x50000000ull, 268435456ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_MODULE, 0x70000000ull, 268435456ull, 0u);

    (void)boot_reserved_populate_from_info(&s_br_buf, &err);

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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 4;
    /* Three optional at-maximum modules sit BEFORE a REQUIRED one and would
     * consume the whole budget in table order. The pass claims required
     * payloads first, so the boot outcome is decided by policy rather than
     * by where the producer happened to write the descriptor. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x50000000ull, 268435456ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_INITRD, 0x70000000ull, 0x1000ull,
                     BOOT_PAYLOAD_FLAG_REQUIRED);

    (void)boot_reserved_populate_from_info(&s_br_buf, &err);

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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 4;
    /* Three at-maximum modules exhaust the 768 MiB budget; the fourth is in
     * contract, carries FLAG_RESERVED, and is NOT pinned. Re-deriving its
     * status from the descriptor alone says "fine": the length passes, the
     * flag is set, the capability is negotiated. Only the pass knows. */
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x50000000ull, 268435456ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_MODULE, 0x70000000ull, 0x1000ull, 0u);

    (void)boot_reserved_populate_from_info(&s_br_buf, &err);

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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 1;
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 0x1000ull, 0u);
    (void)boot_reserved_populate_from_info(&s_br_buf, &err);

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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 5;
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

    (void)boot_reserved_populate_from_info(&s_br_buf, &err);

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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 2;
    br_stage_payload(0, BOOT_PAYLOAD_INITRD, 0x10000000ull, 0x1000ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_INITRD, 0x20000000ull, 0x1000ull,
                     BOOT_PAYLOAD_FLAG_REQUIRED);

    (void)boot_reserved_populate_from_info(&s_br_buf, &err);

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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.payload_count = 2;
    br_stage_payload(0, BOOT_PAYLOAD_RANDOM_SEED, 0x10000000ull, 4096ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_RANDOM_SEED, 0x20000000ull, 4096ull, 0u);

    (void)boot_reserved_populate_from_info(&s_br_buf, &err);

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
    base = br_baseline_count();
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.flags         = BOOT_FLAG_WARM_UPDATE;
    s_br_buf.payload_count = 3;
    /* Admissible: page-multiple start and length, no unknown continuation
     * bits. This is the control -- without it the refusals below would pass
     * against a rule that pins nothing. */
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x10000000ull, 0x1000ull, 0u);
    /* Not page-aligned: the consumer refuses it, so the pass must not pin it. */
    br_stage_payload(1, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x20000100ull, 0x1000ull, 0u);
    /* An unknown continuation bit: likewise refused by the consumer. */
    br_stage_payload(2, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x30000000ull, 0x1000ull,
                     0x80000000u);

    (void)boot_reserved_populate_from_info(&s_br_buf, &err);

    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       0u, 0x10000000ull, 0x1000ull), 1ul,
                   "an admissible warm-update descriptor IS pinned");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       1u, 0x20000100ull, 0x1000ull), 0ul,
                   "an unaligned one is not pinned -- the consumer refuses it");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_payload_is_pinned(
                       2u, 0x30000000ull, 0x1000ull), 0ul,
                   "nor is one carrying an unknown continuation bit");
    TEST_ASSERT_EQ((unsigned long)boot_reserved_count(),
                   (unsigned long)(base + 1u),
                   "exactly the admissible descriptor was pinned");
}

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
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.flags         = BOOT_FLAG_WARM_UPDATE;
    s_br_buf.payload_count = 4;
    br_stage_payload(0, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x08000000ull, 67108864ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_MODULE, 0x50000000ull, 268435456ull, 0u);
    (void)boot_reserved_populate_from_info(&s_br_buf, &err);
    warm_first = (uint32_t)boot_reserved_count() - base;

    /* The same four descriptors, warm LAST. */
    base = br_baseline_count();
    s_br_buf.caps_present  = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_br_buf.flags         = BOOT_FLAG_WARM_UPDATE;
    s_br_buf.payload_count = 4;
    br_stage_payload(0, BOOT_PAYLOAD_MODULE, 0x10000000ull, 268435456ull, 0u);
    br_stage_payload(1, BOOT_PAYLOAD_MODULE, 0x30000000ull, 268435456ull, 0u);
    br_stage_payload(2, BOOT_PAYLOAD_MODULE, 0x50000000ull, 268435456ull, 0u);
    br_stage_payload(3, BOOT_PAYLOAD_WARM_UPDATE_STATE, 0x08000000ull, 67108864ull, 0u);
    (void)boot_reserved_populate_from_info(&s_br_buf, &err);
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

void test_register_boot_reserved(void)
{
    test_suite_register_cat("boot_reserved: warm-update budget is order-independent",
                            test_boot_reserved_warm_update_order_independent,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_reserved: warm-update pinning matches the consumer",
                            test_boot_reserved_warm_update_matches_consumer,
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
}
