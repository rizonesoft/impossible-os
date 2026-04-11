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
 * XREF: 01-boot-platform/TODO-02-bootloader-error-recovery.md §16
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"

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
    test_suite_register_cat("boot_info: combined misalign",    test_validate_combined_misaligned_short_circuits, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
