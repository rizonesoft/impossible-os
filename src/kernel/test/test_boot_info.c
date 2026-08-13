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
 * XREF: 01-boot-platform/TODO-03-bootloader-error-recovery.md
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/test/klog_suppress.h"   /* silence boot_payload [FAIL] klog on negative tests */
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "libc/string.h" /* snprintf for fuzz per-iter context messages */

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
    /* The real handoff address with the early 4 GiB bound -- bound to the
     * macro so this stays the production case if the base ever moves. */
    bi_fill_valid();
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)(uintptr_t)BOOT_INFO_PHYS_ADDR,
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
    /* Exact lower boundary: addr == BOOT_INFO_MIN_ADDR.  Bound to the macro,
     * not a copied literal, so the case keeps testing the floor if the floor
     * ever moves -- a hardcoded 0x1000 would silently become an interior
     * value and stop probing the boundary at all.
     * Catches regression of `<` to `<=` on the floor check. */
    TEST_ASSERT_EQ(boot_info_validate_addr((const void *)(uintptr_t)BOOT_INFO_MIN_ADDR,
                                           sizeof(struct boot_info_header),
                                           BOOT_INFO_EARLY_MAP_END),
                   BOOT_OK,
                   "addr exactly at BOOT_INFO_MIN_ADDR floor accepted");
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
 * parametric fuzz for boot_info_validate_addr.
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
        /* Per-iteration class cycles through every documented reject
         * path plus a known-accept + a random case. class=2
         * (known-accept) is asserted as BOOT_OK per iteration so a
         * regression that rejects the valid space surfaces
         * immediately instead of blending into the aggregate
         * counter. */
        uintptr_t addr = (uintptr_t)(raw & 0xFFFFFFFFull);
        size_t sz = (size_t)((raw >> 32) & 0xFFFFull);
        uint32_t class_id = (uint32_t)(iter % 7u);
        /* Forced-reject flag: true if this class is known-rejected
         * (asserted below). Forced-accept flag: true if known-OK.
         * Non-forced class (6) is random; invariant check covers it. */
        int forced_reject = 0;
        int forced_accept = 0;
        switch (class_id) {
        case 0:
            /* Reject: NULL or below-floor. */
            addr = (iter & 0x4u) ? 0ull : 0x0800ull;
            sz   = sizeof(struct boot_info);
            forced_reject = 1;
            break;
        case 1:
            /* Reject: misaligned by 1 byte above the valid handoff base. */
            addr = BOOT_INFO_PHYS_ADDR + 1ull;
            sz   = sizeof(struct boot_info);
            forced_reject = 1;
            break;
        case 2:
            /* Accept: 8-byte aligned inside early map, size >=
             * sizeof(header), end within early-map ceiling. */
            addr = 0x20000ull + ((raw >> 48) & 0xFF0u);
            sz   = 16u + (size_t)((raw >> 56) & 0xFu) * 8u;
            if (sz < sizeof(struct boot_info_header))
                sz = sizeof(struct boot_info_header);
            forced_accept = 1;
            break;
        case 3:
            /* Reject: size > UINT16_MAX. boot_info_validate_addr
             * requires size fits uint16_t so the header-size field
             * can round-trip it. 65536 is the smallest overflow. */
            addr = 0x40000ull;
            sz   = 65536u + (size_t)((raw >> 48) & 0x3FFu);
            forced_reject = 1;
            break;
        case 4: {
            /* Reject: range wraparound. addr near UINT64_MAX with
             * size that makes start + size wrap past zero. */
            addr = (uintptr_t)((uint64_t)-8);
            sz   = 256u;
            forced_reject = 1;
            break;
        }
        case 5:
            /* Reject: addr beyond BOOT_INFO_EARLY_MAP_END. The caller
             * passes BOOT_INFO_EARLY_MAP_END as max_addr, so any
             * address at or above the 4 GiB ceiling must reject. */
            addr = (uintptr_t)(BOOT_INFO_EARLY_MAP_END +
                               ((raw >> 56) & 0xFF0u));
            sz   = sizeof(struct boot_info);
            forced_reject = 1;
            break;
        case 6:
            /* Random: let it land wherever, 8-byte aligned, size at
             * least sizeof(header). The invariant check below still
             * catches any floor/alignment/size violation the PRNG
             * produces. */
            addr &= ~(uintptr_t)7;
            if (sz < sizeof(struct boot_info_header))
                sz = sizeof(struct boot_info_header);
            break;
        }

        boot_result_t r = boot_info_validate_addr((const void *)addr, sz,
                                                   BOOT_INFO_EARLY_MAP_END);
        if (r == BOOT_OK)
            accepted++;
        else
            rejected++;

        /* Per-iteration assertion. forced_reject class MUST reject;
         * forced_accept class MUST accept; random class obeys the
         * documented invariants (floor / alignment / size). */
        if (forced_reject && r != BOOT_FATAL) {
            char fuzz_msg[160];
            snprintf(fuzz_msg, sizeof(fuzz_msg),
                     "fuzz[%u class=%u]: addr=0x%llx sz=%llu "
                     "returned %d (expected BOOT_FATAL)",
                     (unsigned int)iter, (unsigned int)class_id,
                     (unsigned long long)addr,
                     (unsigned long long)sz, (int)r);
            TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL, fuzz_msg);
        }
        if (forced_accept && r != BOOT_OK) {
            char fuzz_msg[160];
            snprintf(fuzz_msg, sizeof(fuzz_msg),
                     "fuzz[%u class=2 known-good]: addr=0x%llx sz=%llu "
                     "returned %d (expected BOOT_OK)",
                     (unsigned int)iter,
                     (unsigned long long)addr,
                     (unsigned long long)sz, (int)r);
            TEST_ASSERT_EQ((int)r, (int)BOOT_OK, fuzz_msg);
        }
        if (!forced_reject && !forced_accept) {
            if ((addr < 0x1000ull ||
                 sz < sizeof(struct boot_info_header) ||
                 (addr & 7u) != 0u) && r != BOOT_FATAL) {
                char fuzz_msg[160];
                snprintf(fuzz_msg, sizeof(fuzz_msg),
                         "fuzz[%u random]: malformed addr=0x%llx sz=%llu "
                         "returned %d (expected BOOT_FATAL)",
                         (unsigned int)iter,
                         (unsigned long long)addr,
                         (unsigned long long)sz, (int)r);
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
 * -- Typed payload descriptor array validator
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
    /* Straddle the real handoff base, not a hand-copied literal: the case
     * must follow BOOT_INFO_PHYS_ADDR if it ever moves. */
    s_test_buf.payload_descriptors[0].phys_start = BOOT_INFO_PHYS_ADDR + 0x10ull;
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

static void test_payload_descriptor_overlap_exact_duplicate(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count                     = 2;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_descriptors[1].type       = BOOT_PAYLOAD_INITRD;
    s_test_buf.payload_descriptors[1].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[1].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[1].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes               = 2ull * SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "exact-duplicate phys range rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_DESCRIPTOR_OVERLAP,
                   "err=DESCRIPTOR_OVERLAP");
}

static void test_payload_descriptor_overlap_partial(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* MODULE@X..X+0x2000; INITRD@X+0x1000..X+0x3000 -- second straddles
     * the upper half of first by 0x1000 bytes. Neither hits a retained
     * boot region (SAFE_PAYLOAD_START is 64 GiB, far from any). */
    s_test_buf.payload_count                     = 2;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = 0x2000ull;
    s_test_buf.payload_descriptors[1].type       = BOOT_PAYLOAD_INITRD;
    s_test_buf.payload_descriptors[1].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[1].phys_start = SAFE_PAYLOAD_START + 0x1000ull;
    s_test_buf.payload_descriptors[1].length     = 0x2000ull;
    s_test_buf.payload_total_bytes               = 0x4000ull;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "partial-overlap phys ranges rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_DESCRIPTOR_OVERLAP,
                   "err=DESCRIPTOR_OVERLAP partial");
}

/* Codex 2026-04-30 H2 regression: REQUIRED warm-update descriptor with
 * a known continuation bit (positions 8..13) must NOT trigger the
 * payload-validator unknown-flags fatal -- the validator's REQUIRED +
 * unknown-flags mask is type-aware and accepts
 * BOOT_WARM_UPDATE_CONT_MASK_KNOWN bits in addition to
 * BOOT_PAYLOAD_FLAG_MASK_KNOWN for type =
 * BOOT_PAYLOAD_WARM_UPDATE_STATE descriptors. */
static void test_payload_warm_update_required_with_cont_accepted(void)
{
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_WARM_UPDATE_STATE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID
                                                 | BOOT_PAYLOAD_FLAG_RESERVED
                                                 | BOOT_PAYLOAD_FLAG_REQUIRED
                                                 | BOOT_WARM_UPDATE_CONT_PAGE_TABLES;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes               = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_OK,
                   "REQUIRED warm-update + known cont bit accepted (type-aware mask)");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OK, "err=OK");
}

static void test_payload_warm_update_required_with_unknown_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    s_test_buf.payload_count = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_WARM_UPDATE_STATE;
    /* Bit 14: outside both BOOT_PAYLOAD_FLAG_MASK_KNOWN and
     * BOOT_WARM_UPDATE_CONT_MASK_KNOWN; must still trip the
     * REQUIRED+unknown-flags check. */
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID
                                                 | BOOT_PAYLOAD_FLAG_RESERVED
                                                 | BOOT_PAYLOAD_FLAG_REQUIRED
                                                 | (1u << 14);
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes               = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "REQUIRED warm-update + truly unknown bit still rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_UNKNOWN_FLAGS, "err=UNKNOWN_FLAGS");
}

static void test_payload_missing_valid_flag(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Occupied descriptor (type != NONE) with flags=0 (no FLAG_VALID).
     * Validator MUST reject before any downstream consumer sees it,
     * regardless of whether the range/alignment/overlap fields are
     * sound. This protects the FLAG_VALID gate from becoming
     * informational. */
    s_test_buf.payload_count                     = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = 0u;  /* missing FLAG_VALID */
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes               = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "occupied slot without FLAG_VALID rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_MISSING_VALID_FLAG,
                   "err=MISSING_VALID_FLAG");
}

static void test_payload_warm_update_cold_fallback_accepted(void)
{
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Warm-update descriptor with NO FLAG_VALID and NO FLAG_RESERVED:
     * the documented cold-fallback shape from the warm-kernel-update
     * handoff contract. Validator MUST accept (boot_warm_update_consume
     * handles the cold-fallback decision later); halting here would
     * turn a soft fallback into a fatal halt. */
    s_test_buf.payload_count                     = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_WARM_UPDATE_STATE;
    s_test_buf.payload_descriptors[0].flags      = 0u;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes               = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_OK,
                   "warm-update cold-fallback shape (no flags) accepted");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OK,
                   "err=OK warm-update cold-fallback");
}

static void test_payload_warm_update_reserved_only_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Warm-update descriptor with FLAG_RESERVED but NOT FLAG_VALID:
     * this would tell PMM to reserve a range while telling consumers
     * to cold-fallback. Memory leak on cold boot. Validator MUST
     * reject as malformed; the cold-fallback exemption is only for
     * the no-flags shape. */
    s_test_buf.payload_count                     = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_WARM_UPDATE_STATE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_RESERVED;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes               = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "warm-update RESERVED-without-VALID rejected as malformed");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_MISSING_VALID_FLAG,
                   "err=MISSING_VALID_FLAG malformed warm-update");
}

static void test_payload_warm_update_checksummed_only_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Warm-update descriptor with FLAG_CHECKSUMMED but NOT FLAG_VALID:
     * the exemption only covers d->flags == 0 exactly. Any non-zero
     * flag bit without FLAG_VALID is producer drift, not a documented
     * cold-fallback case. */
    s_test_buf.payload_count                     = 1;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_WARM_UPDATE_STATE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_CHECKSUMMED;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = SAFE_PAYLOAD_LEN;
    s_test_buf.payload_total_bytes               = SAFE_PAYLOAD_LEN;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "warm-update CHECKSUMMED-without-VALID rejected");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_MISSING_VALID_FLAG,
                   "err=MISSING_VALID_FLAG nonzero-flags-without-VALID");
}

static void test_payload_descriptor_overlap_adjacent_ok(void)
{
    enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;

    bi_payload_zero();
    /* Adjacent (touching) ranges MUST NOT overlap: ranges_overlap uses
     * half-open [start, start+len) semantics so X+len == Y is allowed. */
    s_test_buf.payload_count                     = 2;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = 0x1000ull;
    s_test_buf.payload_descriptors[1].type       = BOOT_PAYLOAD_INITRD;
    s_test_buf.payload_descriptors[1].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[1].phys_start = SAFE_PAYLOAD_START + 0x1000ull;
    s_test_buf.payload_descriptors[1].length     = 0x1000ull;
    s_test_buf.payload_total_bytes               = 0x2000ull;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_OK,
                   "adjacent non-overlapping ranges accepted");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_OK,
                   "err=OK adjacent");
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

static void test_payload_find_skips_retired_descriptors(void)
{
    const struct boot_payload_desc *d;

    bi_payload_zero();
    /* Two RANDOM_SEED descriptors. Retiring slot 0 (consumer clears
     * FLAG_VALID after wipe/free) must make it undiscoverable, and the
     * surviving descriptor must pop into occurrence 0 -- the contract
     * boot_seed_consume's find(type, 0) retire loop relies on. */
    s_test_buf.payload_count                     = 2;
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_RANDOM_SEED;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = 0x1000ull;
    s_test_buf.payload_descriptors[1].type       = BOOT_PAYLOAD_RANDOM_SEED;
    s_test_buf.payload_descriptors[1].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[1].phys_start = SAFE_PAYLOAD_START + 0x10000ull;
    s_test_buf.payload_descriptors[1].length     = 0x1000ull;
    s_test_buf.payload_total_bytes               = 0x2000ull;

    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_RANDOM_SEED, 1);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)1,
                   "both seed descriptors discoverable pre-retire");

    s_test_buf.payload_descriptors[0].flags &=
        ~(uint32_t)BOOT_PAYLOAD_FLAG_VALID;

    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_RANDOM_SEED, 0);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)1,
                   "survivor pops into occurrence 0");
    TEST_ASSERT_EQ((unsigned long)d->phys_start,
                   (unsigned long)(SAFE_PAYLOAD_START + 0x10000ull),
                   "occurrence 0 is the still-valid slot 1");

    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_RANDOM_SEED, 1);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)0,
                   "retired descriptor is not rediscoverable");

    s_test_buf.payload_descriptors[1].flags &=
        ~(uint32_t)BOOT_PAYLOAD_FLAG_VALID;
    d = boot_payload_find(&s_test_buf, BOOT_PAYLOAD_RANDOM_SEED, 0);
    TEST_ASSERT_EQ((unsigned long)(d != (void *)0 ? 1 : 0), (unsigned long)0,
                   "all retired -> find returns NULL (loop terminates)");
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

    /* Two individually-valid descriptors whose lengths sum past
     * UINT64_MAX. Each length passes the per-descriptor overflow
     * check; the aggregate would wrap if they were ever summed. Note:
     * after the pairwise descriptor-overlap check landed, two ranges
     * each spanning >= UINT64_MAX/2 bytes cannot avoid overlapping in
     * the 64-bit physical address space (it is mathematically
     * impossible to fit two disjoint ranges of that size in u64), so
     * the validator now rejects via DESCRIPTOR_OVERLAP before reaching
     * the aggregate-wrap accumulator. The accumulator check remains as
     * defense in depth for any future code path that bypasses the
     * pairwise loop, but the user-visible failure for this fixture
     * is the overlap rejection. */
    s_test_buf.payload_descriptors[0].type       = BOOT_PAYLOAD_MODULE;
    s_test_buf.payload_descriptors[0].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[0].phys_start = SAFE_PAYLOAD_START;
    s_test_buf.payload_descriptors[0].length     = ((uint64_t)-1) / 2ull;

    s_test_buf.payload_descriptors[1].type       = BOOT_PAYLOAD_INITRD;
    s_test_buf.payload_descriptors[1].flags      = BOOT_PAYLOAD_FLAG_VALID;
    s_test_buf.payload_descriptors[1].phys_start = SAFE_PAYLOAD_START + 0x1000000000000ull;
    s_test_buf.payload_descriptors[1].length     = ((uint64_t)-1) / 2ull + 4ull;

    s_test_buf.payload_total_bytes        = 0;

    TEST_ASSERT_EQ(boot_payload_validate(&s_test_buf, &err), BOOT_FATAL,
                   "aggregate sum overflow rejected (now via pairwise overlap)");
    TEST_ASSERT_EQ((int)err, (int)BOOT_PAYLOAD_ERR_DESCRIPTOR_OVERLAP,
                   "err=DESCRIPTOR_OVERLAP (overlap fires before aggregate wrap)");
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
     * consistent, but payload_overflow=1 flags dropped payloads.'s
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

/* ---- Bootloader build identity (boot_loader_identity) -------------- */

/* Layout pin: struct boot_loader_identity must be exactly 64 bytes
 * with offsets 0/20/28/52 (packed). The header carries _Static_assert
 * for these but a runtime test catches malformed builds where the
 * static assert was somehow bypassed (e.g., a future packed-attribute
 * regression that the compiler accepts). */
static void test_boot_loader_identity_layout_pinned(void)
{
    TEST_ASSERT_EQ((unsigned long)sizeof(struct boot_loader_identity),
                   64UL, "boot_loader_identity sizeof == 64");
    TEST_ASSERT_EQ((unsigned long)__builtin_offsetof(
                       struct boot_loader_identity, git_sha),
                   0UL, "git_sha at offset 0");
    TEST_ASSERT_EQ((unsigned long)__builtin_offsetof(
                       struct boot_loader_identity, build_unix_time),
                   20UL, "build_unix_time at offset 20");
    TEST_ASSERT_EQ((unsigned long)__builtin_offsetof(
                       struct boot_loader_identity, build_label),
                   28UL, "build_label at offset 28");
    TEST_ASSERT_EQ((unsigned long)__builtin_offsetof(
                       struct boot_loader_identity, _pad),
                   52UL, "_pad at offset 52");
}

/* Tail-field pin: the LAST appended field group must end at
 * sizeof(struct boot_info) -- inserting a field in the middle would
 * shift all subsequent offsets and silently break ABI back-compat.
 * v16 (local boot device path detail) is the current tail; v15
 * (extended boot-variable capability surface) was the prior tail;
 * v14 (UKI signed-payload addresses) before that; before v14,
 * loader_identity was. When the next ABI bump appends new fields,
 * update both the offset and the field name here so the tail-pin
 * test continues to enforce the end-of-struct invariant. */
static void test_boot_info_v20_tail(void)
{
    /* _audit_pad is the last field in the v20 tail block. v20 appended
     * the policy audit surface (audit_degraded + sticky_present +
     * sticky_recovery_trigger + sticky_watchdog_rollback_request +
     * sticky_last_outcome + sticky_audit_degraded_last_boot +
     * sticky_last_event_code + sticky_last_boot_seq +
     * sticky_consumed_trigger_seq + _audit_pad) after the v19 boot
     * policy selection ABI. The compiler adds up to 7 bytes of
     * trailing padding to round the struct to an 8-byte alignment
     * boundary. The tail invariant is therefore end-of-_audit_pad +
     * <=7-byte tail-pad == struct size. The pinned struct size
     * catches accidental drift in either direction.
     *
     * History: v22 pinned 28528 with _ab_meta_pad as tail; v19 pinned
     * 28488 with rejected_entry_overflow as tail; v18 pinned 24056 with
     * degraded_trust_flags as tail; v17 pinned an earlier size with
     * boot_media_role* as tail. v23 appended the multi-GPU GOP handle
     * array (gop_handles[] + gop_handle_count + _gop_handle_pad) at the
     * tail, so _gop_handle_pad is now the tail field. */
    uint64_t off = (uint64_t)__builtin_offsetof(
        struct boot_info, _gop_handle_pad);
    uint64_t end = off + (uint64_t)sizeof(
        ((struct boot_info *)0)->_gop_handle_pad);
    TEST_ASSERT_EQ((uint64_t)(sizeof(struct boot_info) - end < 8u), 1u,
                   "_gop_handle_pad must be the v23 tail field "
                   "(<= 7-byte alignment pad to next struct boundary)");
    TEST_ASSERT_EQ((uint64_t)sizeof(struct boot_info), (uint64_t)28664u,
                   "v23 struct size pinned at 28664 bytes "
                   "(multi-GPU GOP handle array appended to the v22 tail)");
}

/* loader_identity is no longer the tail (v14 appended UKI payload
 * fields after it), but its offset must remain stable: it sits
 * exactly between the v13 ESP integrity fields and the v14 tail
 * block. Stale-bootloader detection still hinges on the all-zero
 * git_sha[] sentinel at this offset. */
static void test_boot_info_loader_identity_offset_stable(void)
{
    uint64_t off = (uint64_t)__builtin_offsetof(
        struct boot_info, loader_identity);
    TEST_ASSERT_EQ(off, (uint64_t)23760,
                   "loader_identity must stay at offset 23760 "
                   "(v13 layout pinned; v14 fields append after it)");
}

/* Zero-default sentinel: a brand-new struct boot_info (or one
 * populated by a stale bootloader) must have all-zero git_sha[],
 * which is the "loader did not populate" signal consumers check.
 * A real SHA-1 cannot be all-zero (collision-resistant; even
 * git's empty-tree hash 4b825... is nonzero), so zero is a safe
 * sentinel. */
/* sec6 producer-valid gate: the kernel VPD must render detailed A/B status
 * ONLY when the bootloader published the snapshot. The dangerous case is an
 * older same-version (v22) bootloader that sets ab_meta_lba but leaves
 * ab_status_valid zero -- rendering that as a healthy zero-state would mask a
 * real rollback. ab_boot_status_published() is the gate. */
static void test_ab_status_published_gate(void)
{
    TEST_ASSERT_EQ((uint64_t)ab_boot_status_published(0, 0), 0UL,
                   "non-A/B disk (no meta, no marker) -> not published");
    TEST_ASSERT_EQ((uint64_t)ab_boot_status_published(0, AB_STATUS_VALID_MAGIC),
                   0UL, "marker set but no metadata partition -> not published");
    TEST_ASSERT_EQ((uint64_t)ab_boot_status_published(0x800, 0), 0UL,
                   "SKEW: A/B meta present but older loader left marker zero "
                   "-> not published (must not synthesize healthy zero-state)");
    TEST_ASSERT_EQ((uint64_t)ab_boot_status_published(0x800, 0x01), 0UL,
                   "wrong marker value -> not published");
    TEST_ASSERT_EQ((uint64_t)ab_boot_status_published(0x800,
                                                      AB_STATUS_VALID_MAGIC),
                   1UL, "A/B meta + correct marker -> published");
}

static void test_boot_loader_identity_zero_default(void)
{
    struct boot_info bi;
    uint32_t i;
    for (i = 0; i < sizeof(bi); i++)
        ((uint8_t *)&bi)[i] = 0;
    int sha_zero = 1;
    for (i = 0; i < 20; i++) {
        if (bi.loader_identity.git_sha[i] != 0) {
            sha_zero = 0;
            break;
        }
    }
    TEST_ASSERT_EQ((unsigned long)sha_zero, 1UL,
                   "zero-init boot_info has all-zero git_sha "
                   "(loader-did-not-populate sentinel)");
    TEST_ASSERT_EQ((uint64_t)bi.loader_identity.build_unix_time,
                   (uint64_t)0,
                   "zero-init build_unix_time == 0");
    TEST_ASSERT_EQ((uint64_t)bi.loader_identity.build_label[0],
                   (uint64_t)0,
                   "zero-init build_label[0] == NUL");
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
    test_suite_register_cat("boot_payload: descriptor overlap exact",
                            test_payload_descriptor_overlap_exact_duplicate, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: descriptor overlap partial",
                            test_payload_descriptor_overlap_partial, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: descriptor adjacent ok",
                            test_payload_descriptor_overlap_adjacent_ok, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: REQUIRED warm-update + known cont bit accepted",
                            test_payload_warm_update_required_with_cont_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: REQUIRED warm-update + truly unknown bit rejected",
                            test_payload_warm_update_required_with_unknown_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: missing FLAG_VALID",
                            test_payload_missing_valid_flag, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: warm-update cold-fallback shape",
                            test_payload_warm_update_cold_fallback_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: warm-update RESERVED-only rejected",
                            test_payload_warm_update_reserved_only_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_payload: warm-update CHECKSUMMED-only rejected",
                            test_payload_warm_update_checksummed_only_rejected, TEST_CAT_BOOT);
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
    test_suite_register_cat("boot_payload: find skips retired",
        test_payload_find_skips_retired_descriptors, TEST_CAT_BOOT);
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

    /* Bootloader build identity. */
    test_suite_register_cat("boot_loader_identity: layout pinned (64B, packed)",
                            test_boot_loader_identity_layout_pinned, TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: v20 tail field pin (_audit_pad)",
                            test_boot_info_v20_tail, TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: loader_identity offset stable (v13 layout)",
                            test_boot_info_loader_identity_offset_stable, TEST_CAT_BOOT);
    test_suite_register_cat("boot_loader_identity: zero is loader-did-not-populate",
                            test_boot_loader_identity_zero_default, TEST_CAT_BOOT);
    test_suite_register_cat("boot_info: A/B status-published gate (sec6 skew)",
                            test_ab_status_published_gate, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
