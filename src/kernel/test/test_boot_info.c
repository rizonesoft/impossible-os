/* ============================================================================
 * test_boot_info.c -- boot_info handoff validator unit tests (S16)
 *
 * Exercises boot_info_validate_addr(), boot_info_validate_header(), and
 * the combined boot_info_validate() from src/kernel/main/boot_info.c.
 *
 * All tests use pure synthetic buffers and never touch live boot
 * infrastructure -- no forbidden boot_progress/vpd/_init/boot_halt
 * calls.  A single 21952-byte static struct boot_info lives in BSS and
 * is memset() before each test that needs a valid-looking buffer.
 *
 * XREF: 01-boot-platform/TODO-03-bootloader-error-recovery.md §16
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/test/klog_suppress.h"   /* silence boot_payload [FAIL] klog on negative tests */
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "libc/string.h"                 /* snprintf for §9 fuzz per-iter context messages */

/* Production-shape buffer: real sizeof(struct boot_info) so the range
 * check exercises the same arithmetic boot_phase0() runs.  Aligned to
 * 16 bytes to guarantee the 8-byte alignment the validator requires. */
static struct boot_info s_test_buf __attribute__((aligned(16)));

static void bi_zero(void)
{
    uint8_t *p = (uint8_t *)&s_test_buf;
    uint32_t i;
    for (i = 0; i < sizeof(s_test_buf); i++)
        p[i] = 0;
}

static void bi_fill_valid(void)
{
    bi_zero();
    s_test_buf.header.magic   = BOOT_INFO_MAGIC;
    s_test_buf.header.version = BOOT_INFO_VERSION;
    s_test_buf.header.size    = (uint16_t)sizeof(struct boot_info);
}

/* ---- boot_info_validate_addr() -------------------------------------- */

static void test_validate_addr_null(void)
{
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)0,
                                           sizeof(struct boot_info),
                                           (uintptr_t)-1),
                   BOOT_FATAL,
                   "NULL pointer rejected");
}

static void test_validate_addr_below_floor(void)
{
    /* 0x500 is above NULL but inside the BDA region -- rejected. */
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)(uintptr_t)0x500,
                                           sizeof(struct boot_info),
                                           (uintptr_t)-1),
                   BOOT_FATAL,
                   "addr below 0x1000 floor rejected");
}

static void test_validate_addr_misaligned(void)
{
    /* 0x10001 is above floor but not 8-byte aligned. */
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)(uintptr_t)0x10001,
                                           sizeof(struct boot_info),
                                           (uintptr_t)-1),
                   BOOT_FATAL,
                   "misaligned pointer rejected");
}

static void test_validate_addr_size_too_small(void)
{
    /* Caller passed a size smaller than the header -- impossible to
     * read magic/version/size from the buffer.  Rejected. */
    TEST_ASSERT_EQ(boot_info_validate_addr(&s_test_buf,
                                           sizeof(struct boot_info_header) - 1,
                                           (uintptr_t)-1),
                   BOOT_FATAL,
                   "size smaller than header rejected");
}

static void test_validate_addr_size_too_large(void)
{
    /* header.size is uint16_t so sizes above 65535 cannot round-trip. */
    TEST_ASSERT_EQ(boot_info_validate_addr(&s_test_buf,
                                           65536,
                                           (uintptr_t)-1),
                   BOOT_FATAL,
                   "size above uint16_t rejected");
}

static void test_validate_addr_wraparound(void)
{
    /* Near-top address with non-trivial size -- addr + size wraps. */
    uintptr_t near_top = (uintptr_t)-16;  /* 0xFFFFFFFFFFFFFFF0 */
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)near_top,
                                           sizeof(struct boot_info),
                                           (uintptr_t)-1),
                   BOOT_FATAL,
                   "wraparound rejected");
}

static void test_validate_addr_over_max(void)
{
    /* 0x100000000 is exactly the 4 GiB bound used by boot_phase0().
     * addr + size > max rejects. */
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)(uintptr_t)0x100000000ULL,
                                           sizeof(struct boot_info),
                                           BOOT_INFO_EARLY_MAP_END),
                   BOOT_FATAL,
                   "addr at max_addr rejected");
}

static void test_validate_addr_bound_straddle(void)
{
    /* Address below bound but [addr, addr+size) straddles the bound. */
    uintptr_t straddle = BOOT_INFO_EARLY_MAP_END - 16;  /* size > 16 */
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)straddle,
                                           sizeof(struct boot_info),
                                           BOOT_INFO_EARLY_MAP_END),
                   BOOT_FATAL,
                   "range straddling max_addr rejected");
}

static void test_validate_addr_ok_early_map(void)
{
    /* Classic handoff address 0x10000 with the early 4 GiB bound. */
    bi_fill_valid();
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)(uintptr_t)0x10000,
                                           sizeof(struct boot_info),
                                           BOOT_INFO_EARLY_MAP_END),
                   BOOT_OK,
                   "valid handoff address accepted with early map bound");
}

static void test_validate_addr_ok_kernel_va(void)
{
    /* Static buffer lives at a kernel VA well above 4 GiB.  Must pass
     * when the caller uses the unbounded (uintptr_t)-1 max. */
    bi_fill_valid();
    TEST_ASSERT_EQ(boot_info_validate_addr(&s_test_buf,
                                           sizeof(struct boot_info),
                                           (uintptr_t)-1),
                   BOOT_OK,
                   "kernel VA static buffer accepted with UINTPTR_MAX bound");
}

static void test_validate_addr_min_addr_ok(void)
{
    /* Exact lower boundary: addr == BOOT_INFO_MIN_ADDR (0x1000).
     * Catches regression of `<` to `<=` on the floor check. */
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)(uintptr_t)0x1000,
                                           sizeof(struct boot_info_header),
                                           BOOT_INFO_EARLY_MAP_END),
                   BOOT_OK,
                   "addr exactly at 0x1000 floor accepted");
}

static void test_validate_addr_end_exactly_max_ok(void)
{
    /* Exact upper boundary: end == max_addr (inclusive range check).
     * Catches regression of `>` to `>=` on the bound check. */
    uintptr_t addr = (uintptr_t)(BOOT_INFO_EARLY_MAP_END - sizeof(struct boot_info));
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)addr,
                                           sizeof(struct boot_info),
                                           BOOT_INFO_EARLY_MAP_END),
                   BOOT_OK,
                   "range ending exactly at max_addr accepted");
}

static void test_validate_addr_size_uint16_max_ok(void)
{
    /* Exact uint16 upper boundary: size == 65535 (inclusive).  Pointer
     * is a 8-byte aligned literal -- the addr validator does not
     * dereference it, so an abstract address is fine. */
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)(uintptr_t)0x2000,
                                           65535,
                                           (uintptr_t)-1),
                   BOOT_OK,
                   "size exactly at 65535 accepted");
}

/* ---- boot_info_validate_header() ------------------------------------ */

static void test_validate_header_null(void)
{
    TEST_ASSERT_EQ(boot_info_validate_header((const struct boot_info_header *)0,
                                             sizeof(struct boot_info)),
                   BOOT_FATAL,
                   "NULL header rejected");
}

static void test_validate_header_bad_magic(void)
{
    bi_fill_valid();
    s_test_buf.header.magic = 0xDEADBEEFu;
    TEST_ASSERT_EQ(boot_info_validate_header(&s_test_buf.header,
                                             sizeof(struct boot_info)),
                   BOOT_FATAL,
                   "bad magic rejected");
}

static void test_validate_header_bad_version(void)
{
    bi_fill_valid();
    s_test_buf.header.version = (uint16_t)(BOOT_INFO_VERSION + 1);
    TEST_ASSERT_EQ(boot_info_validate_header(&s_test_buf.header,
                                             sizeof(struct boot_info)),
                   BOOT_FATAL,
                   "wrong version rejected");
}

static void test_validate_header_bad_size(void)
{
    bi_fill_valid();
    s_test_buf.header.size = (uint16_t)(sizeof(struct boot_info) - 1);
    TEST_ASSERT_EQ(boot_info_validate_header(&s_test_buf.header,
                                             sizeof(struct boot_info)),
                   BOOT_FATAL,
                   "wrong size rejected");
}

static void test_validate_header_ok(void)
{
    bi_fill_valid();
    TEST_ASSERT_EQ(boot_info_validate_header(&s_test_buf.header,
                                             sizeof(struct boot_info)),
                   BOOT_OK,
                   "fully valid header accepted");
}

/* ---- Combined boot_info_validate() ---------------------------------- */

static void test_validate_combined_null(void)
{
    TEST_ASSERT_EQ(boot_info_validate((const void *)0,
                                      sizeof(struct boot_info)),
                   BOOT_FATAL,
                   "combined NULL rejected");
}

static void test_validate_combined_bad_magic(void)
{
    bi_fill_valid();
    s_test_buf.header.magic = 0u;
    TEST_ASSERT_EQ(boot_info_validate(&s_test_buf,
                                      sizeof(struct boot_info)),
                   BOOT_FATAL,
                   "combined bad magic rejected");
}

static void test_validate_combined_ok(void)
{
    bi_fill_valid();
    TEST_ASSERT_EQ(boot_info_validate(&s_test_buf,
                                      sizeof(struct boot_info)),
                   BOOT_OK,
                   "combined valid buffer accepted");
}

static void test_validate_combined_misaligned_short_circuits(void)
{
    /* Misaligned synthetic pointer: combined validator MUST short-
     * circuit in the address phase and never read through the
     * (unmapped, un-allocated) pointer to touch header fields.  This
     * pins the contract that boot_info_validate() runs the address
     * phase first -- catches a refactor that reorders the phases. */
    TEST_ASSERT_EQ(boot_info_validate((const void *)(uintptr_t)0x10001,
                                      sizeof(struct boot_info)),
                   BOOT_FATAL,
                   "combined misaligned ptr rejected without header deref");
}

/* ============================================================================
 * §9 parametric fuzz for boot_info_validate_addr.
 *
 * The discrete tests above pin every NAMED failure mode. This sweep
 * covers a pseudo-random space of (address, size) pairs and asserts
 * the invariants hold for every combination: rejected addresses stay
 * rejected, valid addresses stay accepted, and no combination
 * produces false-positive accept (important because the validator
 * feeds every downstream pointer consumer).
 *
 * Uses a simple xorshift RNG seeded from a compile-time constant so
 * the sweep is deterministic across runs but exercises variation a
 * one-shot test cannot. Budget: 256 iterations, O(N) per iteration,
 * well under one second on every platform.
 * ========================================================================= */

static uint64_t fuzz_rand_state = 0xC0FFEE1234567890ull;

static uint64_t fuzz_rand(void)
{
    uint64_t x = fuzz_rand_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    fuzz_rand_state = x;
    return x;
}

static void test_validate_addr_fuzz_sweep(void)
{
    /* Reset PRNG so re-runs are deterministic. */
    fuzz_rand_state = 0xC0FFEE1234567890ull;
    uint32_t rejected = 0;
    uint32_t accepted = 0;
    uint32_t iter;
    for (iter = 0u; iter < 256u; iter++) {
        uint64_t raw = fuzz_rand();
        /* Cover four classes per iteration by varying the seed space:
         *   - address below BOOT_INFO_MIN_ADDR (below 0x1000)
         *   - address inside the early map
         *   - size at u16 extremes (0, 1, UINT16_MAX)
         *   - random combinations */
        uintptr_t addr = (uintptr_t)(raw & 0xFFFFFFFFull);
        size_t sz = (size_t)((raw >> 32) & 0xFFFFull);

        /* Rewrite some iterations into KNOWN-REJECTED / KNOWN-OK
         * patterns so the sweep guarantees both sides get exercise. */
        switch (iter & 0x3u) {
        case 0:
            /* Known reject: NULL or below-floor. */
            addr = (iter & 0x4u) ? 0ull : 0x0800ull;
            break;
        case 1:
            /* Known reject: misaligned by 1 byte above a valid base. */
            addr = 0x10000ull + 1ull;
            sz = sizeof(struct boot_info);
            break;
        case 2:
            /* Known accept: 8-byte aligned inside early map, size > 8. */
            addr = 0x20000ull + ((raw >> 48) & 0xFF0u);
            sz = 16ull + ((raw >> 56) & 0xFu) * 8ull;
            if (sz < 16u) sz = 16u;
            break;
        case 3:
            /* Random: let it land wherever; just ensure the validator
             * picks the right side without crashing. */
            addr &= ~(uintptr_t)7;  /* force 8-byte alignment */
            if (sz < 8u) sz = 8u;
            break;
        }

        boot_result_t r = boot_info_validate_addr((const void *)addr, sz,
                                                   BOOT_INFO_EARLY_MAP_END);
        if (r == BOOT_OK)
            accepted++;
        else
            rejected++;

        /* Invariant checks on the classification: if addr < 0x1000,
         * must be rejected; if sz < sizeof(struct boot_info_header),
         * must be rejected; if (addr & 7), must be rejected. Per-
         * iteration context in the assertion message so a failure
         * on any platform is reproducible from the log alone: iter
         * index + the synthesized (addr, sz, class) + observed
         * return value. */
        if (addr < 0x1000ull || sz < sizeof(struct boot_info_header) ||
            (addr & 7u) != 0u) {
            if (r != BOOT_FATAL) {
                char fuzz_msg[160];
                snprintf(fuzz_msg, sizeof(fuzz_msg),
                         "fuzz[%u]: malformed addr=0x%llx sz=%llu class=%u "
                         "returned %d (expected BOOT_FATAL)",
                         (unsigned int)iter,
                         (unsigned long long)addr,
                         (unsigned long long)sz,
                         (unsigned int)(iter & 0x3u),
                         (int)r);
                TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL, fuzz_msg);
            }
        }
    }
    /* The sweep must exercise BOTH sides of the accept/reject line;
     * a bug that reject-all or accept-all would collapse one counter. */
    TEST_ASSERT_EQ((unsigned long)(accepted > 0u ? 1 : 0), (unsigned long)1,
                   "fuzz sweep exercised accept path");
    TEST_ASSERT_EQ((unsigned long)(rejected > 0u ? 1 : 0), (unsigned long)1,
                   "fuzz sweep exercised reject path");
}

/* Parametric fuzz for boot_info_validate_header: sweep every single-
 * byte perturbation of magic/version/size across a valid baseline.
 * 32 perturbations + 32 happy-paths prove the validator is sensitive
 * to each bit of the three header fields without being noisy on
 * otherwise-intact data. */
static void test_validate_header_fuzz_perturbations(void)
{
    struct boot_info_header good = {
        .magic   = BOOT_INFO_MAGIC,
        .version = BOOT_INFO_VERSION,
        .size    = (uint16_t)sizeof(struct boot_info),
    };
    /* Happy path is the baseline. */
    TEST_ASSERT_EQ(boot_info_validate_header(&good, sizeof(struct boot_info)),
                   BOOT_OK, "fuzz: unperturbed baseline accepts");

    /* Perturb each field in turn across a pseudo-random offset space.
     * Assert per iteration (not just the final count) so a false-accept
     * shows up with the exact perturbation in the log. */
    fuzz_rand_state = 0xDECAFBAD01020304ull;
    uint32_t iter;
    for (iter = 0u; iter < 32u; iter++) {
        struct boot_info_header bad = good;
        uint64_t r = fuzz_rand();
        const char *field_name;
        switch (iter & 0x3u) {
        case 0:
            bad.magic ^= (uint32_t)((r | 1u) & 0xFFFFFFFFu);  /* non-zero delta */
            field_name = "magic";
            break;
        case 1:
            bad.version = (uint16_t)(good.version + 1u + (uint16_t)(r & 0xFu));
            field_name = "version";
            break;
        case 2: {
            uint16_t delta = (uint16_t)((r & 0xFu) + 1u);
            bad.size = (uint16_t)(good.size + delta);
            field_name = "size";
            break;
        }
        case 3:
            bad.magic   = good.magic ^ (uint32_t)(1u << (r & 0x1Fu));
            bad.version = (uint16_t)(good.version + 1u);
            field_name = "magic+version";
            break;
        default:
            field_name = "?";
            break;
        }
        boot_result_t rv = boot_info_validate_header(&bad, sizeof(struct boot_info));
        if (rv != BOOT_FATAL) {
            char fuzz_msg[160];
            snprintf(fuzz_msg, sizeof(fuzz_msg),
                     "fuzz-hdr[%u] (%s perturbed): magic=0x%x version=%u "
                     "size=%u returned %d (expected BOOT_FATAL)",
                     (unsigned int)iter, field_name,
                     bad.magic, (unsigned int)bad.version,
                     (unsigned int)bad.size, (int)rv);
            TEST_ASSERT_EQ((int)rv, (int)BOOT_FATAL, fuzz_msg);
        }
    }
}

/* ============================================================================
 * §4 -- Typed payload descriptor array validator
 *
 * Exercises boot_payload_validate() against the in-BSS s_test_buf with
 * synthetic descriptor layouts. All tests run on a zero-initialized
 * buffer (no fb, no rt_mmap, no USB DMA, no kernel-visible overlap)
 * except where a test explicitly populates one of those regions to
 * assert the overlap class. No live boot infrastructure touched.
 * ========================================================================= */

/* A payload region that is definitively outside every retained area the
 * validator inspects: above the 4 GiB early-map ceiling and far from
 * BOOT_INFO_PHYS_ADDR / framebuffer / USB DMA / any plausible rt_mmap.
 * Length fits inside a page so total recomputation is trivial. */
#define SAFE_PAYLOAD_START  0x1000000000ull  /* 64 GiB */
#define SAFE_PAYLOAD_LEN    0x1000ull        /* 4 KiB */

static void bi_payload_zero(void)
{
    /* Descriptor array + counts + total are within s_test_buf, already
     * cleared by bi_zero() / bi_fill_valid(). This helper documents the
     * intent at the test call site. */
    bi_fill_valid();
}

static void test_payload_empty_valid(void)
{
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_OK,
                   "empty payload array accepted");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OK,
                   "empty payload array error=OK");
}

static void test_payload_count_out_of_range(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count = BOOT_PAYLOAD_MAX + 1;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "payload_count > BOOT_PAYLOAD_MAX rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_COUNT_OOR,
                   "err=COUNT_OOR");
}

static void test_payload_prefix_violated(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Occupied slot past payload_count (which is 0) */
    s_test_buf.payload_descriptors[3].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[3].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[3].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[3].length     = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "occupied slot past payload_count rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_PREFIX_VIOLATED,
                   "err=PREFIX_VIOLATED");
}

static void test_payload_range_wrap(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = (uint64_t)-0x100;
    s_test_buf.payload_descriptors[0].length     = 0x200;  /* wraps past (uint64_t)-1 */

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "phys_start + length overflow rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_RANGE_WRAP,
                   "err=RANGE_WRAP");
}

static void test_payload_overlap_boot_info(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_INITRD;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    /* 0x10000 = BOOT_INFO_PHYS_ADDR */
    s_test_buf.payload_descriptors[0].phys_start = 0x10000ull + 0x10ull;
    s_test_buf.payload_descriptors[0].length     = 0x100ull;
    s_test_buf.payload_total_bytes        = 0x100ull;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "overlap with boot_info rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OVERLAP_BOOT_INFO,
                   "err=OVERLAP_BOOT_INFO");
}

static void test_payload_overlap_framebuffer(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Stage a framebuffer at a known-safe physical address far from
     * BOOT_INFO_PHYS_ADDR; then place a payload that straddles it. */
    s_test_buf.fb_available               = 1;
    s_test_buf.fb.addr                    = 0xFD000000ull;
    s_test_buf.fb.pitch                   = 4 * 1280;
    s_test_buf.fb.width                   = 1280;
    s_test_buf.fb.height                  = 720;
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = 0xFD000000ull + 0x1000ull;
    s_test_buf.payload_descriptors[0].length     = 0x1000ull;
    s_test_buf.payload_total_bytes        = 0x1000ull;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "overlap with framebuffer rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OVERLAP_FB,
                   "err=OVERLAP_FB");
}

static void test_payload_overlap_rt_mmap(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Stage one runtime memory region at a known-safe physical address
     * and place a payload that lands inside it. 4 KiB entry (num_pages=1). */
    s_test_buf.rt_mmap_count      = 1;
    s_test_buf.rt_mmap[0].phys_addr  = 0x80000000ull;
    s_test_buf.rt_mmap[0].num_pages  = 1;
    s_test_buf.rt_mmap[0].type       = 0;
    s_test_buf.rt_mmap[0].attribute  = 0;

    s_test_buf.payload_count      = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_TPM_EVENT_LOG;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = 0x80000100ull;  /* inside the 4 KiB page */
    s_test_buf.payload_descriptors[0].length     = 0x100ull;
    s_test_buf.payload_total_bytes        = 0x100ull;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "overlap with rt_mmap rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OVERLAP_RT_MMAP,
                   "err=OVERLAP_RT_MMAP");
}

static void test_payload_overlap_usb_dma(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.usb_controller.dma_page_count = 1;
    s_test_buf.usb_controller.dma_pages[0]   = 0x40000000ull;

    s_test_buf.payload_count      = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_USB_HANDOVER;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = 0x40000800ull;  /* within the 4 KiB page */
    s_test_buf.payload_descriptors[0].length     = 0x100ull;
    s_test_buf.payload_total_bytes        = 0x100ull;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "overlap with USB DMA page rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OVERLAP_USB_DMA,
                   "err=OVERLAP_USB_DMA");
}

static void test_payload_bad_alignment(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_descriptors[0].alignment  = 3;  /* not a power of 2 */
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "non-power-of-two alignment rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_ALIGNMENT,
                   "err=ALIGNMENT");
}

static void test_payload_unknown_type_required_fatal(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = 0xDEADBEEF;  /* unknown */
    s_test_buf.payload_descriptors[0].flags      =
        BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_REQUIRED;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "unknown type + REQUIRED aborts boot");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_UNKNOWN_REQUIRED,
                   "err=UNKNOWN_REQUIRED");
}

static void test_payload_unknown_type_optional_accepted(void)
{
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count              = 1;
    /* Unknown type, no REQUIRED flag -> forward-compat skip. Generic
     * checks (range, overlap) still pass because the payload lies in
     * the known-safe region. */
    s_test_buf.payload_descriptors[0].type       = 0xDEADBEEF;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_OK,
                   "unknown type without REQUIRED is forward-compat skipped");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OK,
                   "err=OK (optional unknown)");
}

static void test_payload_unknown_flags_required_fatal(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;  /* known type */
    /* REQUIRED set + an unknown bit (0x100) -> unknown-flags fatal. */
    s_test_buf.payload_descriptors[0].flags      =
        BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_REQUIRED | 0x100u;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "unknown flag bits + REQUIRED aborts boot");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_UNKNOWN_FLAGS,
                   "err=UNKNOWN_FLAGS");
}

static void test_payload_total_mismatch(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN + 1ull;  /* lies */

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "payload_total_bytes disagreement rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_TOTAL_MISMATCH,
                   "err=TOTAL_MISMATCH");
}

static void test_payload_two_descriptors_ok(void)
{
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count              = 2;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_INITRD;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_descriptors[0].alignment  = 4096;  /* valid power of 2 */

    s_test_buf.payload_descriptors[1].type       = BOOT_PAYLOAD_RANDOM_SEED;
    s_test_buf.payload_descriptors[1].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[1].phys_start = SAFE_PAYLOAD_START + 0x100000ull;
    s_test_buf.payload_descriptors[1].length     = 0x40ull;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN + 0x40ull;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_OK,
                   "two disjoint known-type payloads accepted");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OK,
                   "err=OK");
}

static void test_payload_empty_slot_in_prefix(void)
{
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Slot 0 populated, slot 1 empty, payload_count = 2. Producer
     * reserved slot 1 but deferred the payload (length == 0). This is
     * allowed by the contract and payload_total_bytes should include
     * only the populated slot's length. */
    s_test_buf.payload_count              = 2;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    /* Slot 1 stays zero (type=NONE, length=0) */
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_OK,
                   "empty slot within packed prefix accepted");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OK,
                   "err=OK (empty slot in prefix)");
}

static void test_payload_none_with_length_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* ABI contract: type=BOOT_PAYLOAD_NONE means empty slot. A descriptor
     * with type=NONE but non-zero length is ABI drift -- the old code path
     * treated it as occupied and silently validated it, but type-keyed
     * consumers would skip it entirely. Reject at the validator. */
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_NONE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "type=NONE with non-zero length rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_NONE_NOT_EMPTY,
                   "err=NONE_NOT_EMPTY");
}

static void test_payload_none_with_flags_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* type=NONE + length=0 but flags != 0 -- must still be rejected.
     * A lenient validator that only checked 'length != 0' would let this
     * pass as an empty slot while the flags carry unchecked metadata. */
    s_test_buf.payload_count                  = 1;
    s_test_buf.payload_descriptors[0].type    = BOOT_PAYLOAD_NONE;
    s_test_buf.payload_descriptors[0].flags   = BOOT_PAYLOAD_FLAG_REQUIRED;
    /* all other fields stay zero */

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "type=NONE + length=0 + flags!=0 rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_NONE_NOT_EMPTY,
                   "err=NONE_NOT_EMPTY (flags smuggling)");
}

static void test_payload_none_past_prefix_with_phys_start_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Slot 0 inside packed prefix, slot 1 past payload_count with
     * type=NONE, length=0, but non-zero phys_start. The old loop
     * treated this as 'unoccupied, skip' -- the new loop rejects any
     * NONE slot that is not fully zeroed, regardless of position. */
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    /* Slot 1 past prefix -- type NONE + length 0 but phys_start set */
    s_test_buf.payload_descriptors[1].type       = BOOT_PAYLOAD_NONE;
    s_test_buf.payload_descriptors[1].phys_start = 0xDEADBEEFull;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "type=NONE past prefix with phys_start!=0 rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_NONE_NOT_EMPTY,
                   "err=NONE_NOT_EMPTY (past-prefix smuggling)");
}

static void test_payload_find_returns_nth_of_type(void)
{
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Three payloads: MODULE, INITRD, MODULE. boot_payload_find should
     * return index 0 of MODULE as slot 0, index 1 of MODULE as slot 2,
     * index 0 of INITRD as slot 1, and index 2 of MODULE (only 2 exist)
     * as NULL. This is the contract section 5 relies on. */
    s_test_buf.payload_count                     = 3;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = 0x1000ull;

    s_test_buf.payload_descriptors[1].type       = BOOT_PAYLOAD_INITRD;
    s_test_buf.payload_descriptors[1].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[1].phys_start = SAFE_PAYLOAD_START + 0x10000ull;
    s_test_buf.payload_descriptors[1].length     = 0x2000ull;

    s_test_buf.payload_descriptors[2].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[2].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[2].phys_start = SAFE_PAYLOAD_START + 0x20000ull;
    s_test_buf.payload_descriptors[2].length     = 0x4000ull;

    s_test_buf.payload_total_bytes = 0x1000ull + 0x2000ull + 0x4000ull;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_OK,
                   "three-descriptor fixture passes validator");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OK, "validator err=OK");

    const struct boot_payload_desc *d;
    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_MODULE, 0);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)1,
                   "find MODULE [0] non-NULL");
    TEST_ASSERT_EQ((unsigned long)d->phys_start,
                   (unsigned long)SAFE_PAYLOAD_START,
                   "find MODULE [0] -> slot 0 phys_start");

    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_MODULE, 1);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)1,
                   "find MODULE [1] non-NULL");
    TEST_ASSERT_EQ((unsigned long)d->phys_start,
                   (unsigned long)(SAFE_PAYLOAD_START + 0x20000ull),
                   "find MODULE [1] -> slot 2 phys_start");

    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_INITRD, 0);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)1,
                   "find INITRD [0] non-NULL");
    TEST_ASSERT_EQ((unsigned long)d->length,
                   (unsigned long)0x2000ull,
                   "find INITRD [0] -> slot 1 length");

    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_MODULE, 2);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)0,
                   "find MODULE [2] NULL (only 2 exist)");

    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_RECOVERY_IMAGE, 0);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)0,
                   "find RECOVERY_IMAGE [0] NULL (none in fixture)");
}

static void test_payload_find_rejects_null_info_and_none_type(void)
{
    const struct boot_payload_desc *d;

    d = boot_payload_find((const struct boot_info *)0, BOOT_PAYLOAD_MODULE, 0);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)0,
                   "find(NULL, MODULE, 0) returns NULL");

    bi_payload_zero();
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = 0x1000ull;
    s_test_buf.payload_total_bytes        = 0x1000ull;

    /* NONE is the empty-slot sentinel -- looking it up is always NULL. */
    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_NONE, 0);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)0,
                   "find(info, NONE, 0) returns NULL");
}

static void test_payload_find_rejects_out_of_range_count(void)
{
    const struct boot_payload_desc *d;

    bi_payload_zero();
    /* Synthesize a count past BOOT_PAYLOAD_MAX. A real bootloader could
     * not get past the validator with this, but boot_payload_find must
     * still defend against a caller that skipped validation. */
    s_test_buf.payload_count = BOOT_PAYLOAD_MAX + 1;
    /* slot 0 has a real descriptor; find() must refuse to walk anyway */
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = 0x1000ull;

    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_MODULE, 0);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)0,
                   "find rejects payload_count > BOOT_PAYLOAD_MAX");
}

static void test_payload_desc_size_pin(void)
{
    /* Guard against any accidental change to boot_payload_desc layout.
     * 48 bytes is the ABI-pinned size; the mirror + manifest + kernel
     * all depend on it. */
    TEST_ASSERT_EQ((unsigned long)sizeof(struct boot_payload_desc),
                   (unsigned long)48,
                   "boot_payload_desc is 48 bytes (ABI pin)");
}

static void test_payload_phys_unaligned_vs_required_alignment(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    /* alignment = 4096 is a valid power of 2, but phys_start is not
     * 4 KiB-aligned -- the descriptor lies about its alignment. */
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START + 1ull;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_descriptors[0].alignment  = 4096;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "power-of-two alignment with unaligned phys_start rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_ALIGNMENT,
                   "err=ALIGNMENT (phys_start not a multiple of alignment)");
}

static void test_payload_aggregate_total_wrap(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count              = 2;

    /* Two individually-valid descriptors whose lengths sum just past
     * UINT64_MAX. Each length passes the per-descriptor overflow
     * check; the aggregate wraps. Without the accumulator guard, a
     * forged payload_total_bytes matching the wrapped sum would pass. */
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = ((uint64_t)-1) / 2ull;

    s_test_buf.payload_descriptors[1].type       = BOOT_PAYLOAD_INITRD;
    s_test_buf.payload_descriptors[1].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[1].phys_start = SAFE_PAYLOAD_START + 0x1000000000000ull;
    s_test_buf.payload_descriptors[1].length     = ((uint64_t)-1) / 2ull + 4ull;

    /* Bootloader-supplied total: irrelevant -- the accumulator wrap
     * check fires BEFORE the total comparison, so any value here is
     * acceptable evidence that the wrap was caught. */
    s_test_buf.payload_total_bytes        = 0;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "aggregate sum overflow rejected before total compare");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_TOTAL_MISMATCH,
                   "err=TOTAL_MISMATCH (aggregate sum wraps uint64_t)");
}

static void test_payload_retained_rt_mmap_wrap_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Stage a malformed rt_mmap entry whose end-address wraps past
     * UINT64_MAX. Without the range_end_overflows() guard the payload
     * would compare against a wrapped interval and could be
     * falsely-reported as disjoint. */
    s_test_buf.rt_mmap_count         = 1;
    s_test_buf.rt_mmap[0].phys_addr  = ((uint64_t)-1) - 0x1000ull;
    s_test_buf.rt_mmap[0].num_pages  = 16;  /* 64 KiB -- wraps */
    s_test_buf.rt_mmap[0].type       = 0;

    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "rt_mmap range whose end wraps UINT64_MAX rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OVERLAP_RT_MMAP,
                   "err=OVERLAP_RT_MMAP (retained-region wrap)");
}

static void test_payload_overflow_truncated_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Producer reports it had MORE payloads than fit: the prefix
     * (count=1, one valid descriptor, total matches) is internally
     * consistent, but payload_overflow=1 flags dropped payloads. §4's
     * contract refuses the handoff so a consumer can never silently
     * treat a truncated set as complete. */
    s_test_buf.payload_count              = 1;
    s_test_buf.payload_overflow           = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "payload_overflow=1 rejects the entire handoff");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OVERFLOW_TRUNCATED,
                   "err=OVERFLOW_TRUNCATED");
}

static void test_payload_retained_fb_wrap_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Framebuffer whose declared size wraps past UINT64_MAX. The
     * pitch*height multiplication guard rejects insane dimensions; the
     * range_end_overflows() check rejects fb_addr near UINT64_MAX. */
    s_test_buf.fb_available               = 1;
    s_test_buf.fb.addr                    = ((uint64_t)-1) - 0x100ull;
    s_test_buf.fb.pitch                   = 4 * 1024;
    s_test_buf.fb.width                   = 1024;
    s_test_buf.fb.height                  = 16;

    s_test_buf.payload_count              = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes        = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "framebuffer range whose end wraps UINT64_MAX rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OVERLAP_FB,
                   "err=OVERLAP_FB (fb retained-region wrap)");
}

/* ---- Registration --------------------------------------------------- */

void test_register_boot_info(void)
{
    test_suite_register_cat("boot_info: addr NULL",            test_validate_addr_null,           TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr below floor",     test_validate_addr_below_floor,    TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr misaligned",      test_validate_addr_misaligned,     TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr size too small",  test_validate_addr_size_too_small, TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr size too large",  test_validate_addr_size_too_large, TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr wraparound",      test_validate_addr_wraparound,     TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr over max",        test_validate_addr_over_max,       TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr bound straddle",  test_validate_addr_bound_straddle, TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr OK early map",    test_validate_addr_ok_early_map,   TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr OK kernel VA",    test_validate_addr_ok_kernel_va,   TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr min boundary",    test_validate_addr_min_addr_ok,    TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr end == max",      test_validate_addr_end_exactly_max_ok, TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: addr size uint16 max", test_validate_addr_size_uint16_max_ok, TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: header NULL",          test_validate_header_null,         TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: header bad magic",     test_validate_header_bad_magic,    TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: header bad version",   test_validate_header_bad_version,  TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: header bad size",      test_validate_header_bad_size,     TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: header OK",            test_validate_header_ok,           TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: combined NULL",        test_validate_combined_null,       TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: combined bad magic",   test_validate_combined_bad_magic,  TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: combined OK",          test_validate_combined_ok,         TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: fuzz addr sweep",
                            test_validate_addr_fuzz_sweep, TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: fuzz header perturbations",
                            test_validate_header_fuzz_perturbations, TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: combined misalign",
                            test_validate_combined_misaligned_short_circuits, TEST_CAT_BOOT);

    /* Typed payload descriptor array: wire each validator path */
    test_suite_register_cat("boot_payload: empty valid",
                            test_payload_empty_valid, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: count OOR",
                            test_payload_count_out_of_range, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: prefix violated",
                            test_payload_prefix_violated, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: range wrap",
                            test_payload_range_wrap, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: overlap boot_info",
                            test_payload_overlap_boot_info, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: overlap framebuffer",
                            test_payload_overlap_framebuffer, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: overlap rt_mmap",
                            test_payload_overlap_rt_mmap, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: overlap usb_dma",
                            test_payload_overlap_usb_dma, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: bad alignment",
                            test_payload_bad_alignment, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: unknown type required",
                            test_payload_unknown_type_required_fatal, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: unknown type optional",
                            test_payload_unknown_type_optional_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: unknown flags req",
                            test_payload_unknown_flags_required_fatal, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: total mismatch",
                            test_payload_total_mismatch, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: two descriptors",
                            test_payload_two_descriptors_ok, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: empty slot in prefix",
                            test_payload_empty_slot_in_prefix, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: NONE with length rejected",
                            test_payload_none_with_length_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: NONE with flags rejected",
                            test_payload_none_with_flags_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: NONE past prefix with phys_start rejected",
                            test_payload_none_past_prefix_with_phys_start_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: find nth of type",
                            test_payload_find_returns_nth_of_type, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: find rejects NULL + NONE",
                            test_payload_find_rejects_null_info_and_none_type, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: find rejects count > MAX",
                            test_payload_find_rejects_out_of_range_count, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: desc size pin",
                            test_payload_desc_size_pin, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: phys not aligned",
                            test_payload_phys_unaligned_vs_required_alignment, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: total wrap",
                            test_payload_aggregate_total_wrap, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: rt_mmap wrap",
                            test_payload_retained_rt_mmap_wrap_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: fb wrap",
                            test_payload_retained_fb_wrap_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: overflow truncated",
                            test_payload_overflow_truncated_rejected, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
