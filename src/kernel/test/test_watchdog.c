/* ============================================================================
 * test_watchdog.c -- ACPI WDAT validator unit tests (TODO-23 boot watchdog)
 *
 * Exercises the I/O-free hw_watchdog_wdat_validate() on crafted WDAT tables:
 * a valid table passes; every brick-safety guard (size/header bounds, entry
 * overflow, bad GAS, missing required actions, unknown instruction) rejects.
 * The arm/pet/disarm register I/O is bare-metal-only and validated on hardware
 * via serial; it is not unit-testable without real WDAT firmware.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/watchdog.h"
#include "kernel/types.h"

/* A contiguous valid WDAT: 68-byte fixed header + 5 instruction entries (all
 * the actions init/pet/disarm/verify require: SET_RUNNING_STATE,
 * SET_STOPPED_STATE, GET_RUNNING_STATE, SET_COUNTDOWN, RESET), each with a
 * usable I/O-space GAS. */
struct wdat_fixture {
    struct acpi_wdat hdr;
    struct acpi_wdat_entry e[5];
} __attribute__((packed));

static void wd_make_valid(struct wdat_fixture *f)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)f;
    for (i = 0; i < sizeof(*f); i++)
        p[i] = 0;
    f->hdr.header.signature[0] = 'W'; f->hdr.header.signature[1] = 'D';
    f->hdr.header.signature[2] = 'A'; f->hdr.header.signature[3] = 'T';
    f->hdr.header_length = (uint32_t)sizeof(struct acpi_wdat);  /* 68 */
    f->hdr.timer_period  = 100;        /* ms per count */
    f->hdr.min_count     = 1;
    f->hdr.max_count     = 1200;       /* 120 s at 100 ms */
    f->hdr.entries       = 5;
    {
        uint8_t actions[5] = { ACPI_WDAT_SET_RUNNING_STATE,
                               ACPI_WDAT_SET_STOPPED_STATE,
                               ACPI_WDAT_GET_RUNNING_STATE,
                               ACPI_WDAT_SET_COUNTDOWN,
                               ACPI_WDAT_RESET };
        uint8_t instrs[5] = { ACPI_WDAT_WRITE_VALUE, ACPI_WDAT_WRITE_VALUE,
                              ACPI_WDAT_READ_VALUE, ACPI_WDAT_WRITE_COUNTDOWN,
                              ACPI_WDAT_WRITE_VALUE };
        for (i = 0; i < 5; i++) {
            f->e[i].action = actions[i];
            f->e[i].instruction = instrs[i];
            f->e[i].register_region.address_space = ACPI_GAS_SPACE_IO;
            f->e[i].register_region.bit_width = 32;
            f->e[i].register_region.address = 0x400 + i * 4;
            f->e[i].value = 1;
            f->e[i].mask = 0xFFFFFFFFu;
        }
    }
}

static void test_wdat_valid(void)
{
    struct wdat_fixture f;
    wd_make_valid(&f);
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, (uint32_t)sizeof(f)), 1,
                   "well-formed WDAT with all required actions validates");
}

static void test_wdat_null(void)
{
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate((const struct acpi_wdat *)0, 100), 0,
                   "size smaller than the fixed header is rejected");
}

static void test_wdat_size_too_small(void)
{
    struct wdat_fixture f;
    wd_make_valid(&f);
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, 40), 0,
                   "size < sizeof(acpi_wdat) rejected");
}

static void test_wdat_bad_header_length(void)
{
    struct wdat_fixture f;
    wd_make_valid(&f);
    f.hdr.header_length = 40;          /* < 68 */
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, (uint32_t)sizeof(f)), 0,
                   "header_length below the fixed header rejected");
}

static void test_wdat_zero_timer_period(void)
{
    struct wdat_fixture f;
    wd_make_valid(&f);
    f.hdr.timer_period = 0;            /* would divide-by-zero on arm */
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, (uint32_t)sizeof(f)), 0,
                   "timer_period == 0 rejected");
}

static void test_wdat_bad_counts(void)
{
    struct wdat_fixture f;
    wd_make_valid(&f);
    f.hdr.min_count = 2000; f.hdr.max_count = 1000;  /* min > max */
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, (uint32_t)sizeof(f)), 0,
                   "min_count > max_count rejected");
}

static void test_wdat_entry_overflow(void)
{
    struct wdat_fixture f;
    wd_make_valid(&f);
    f.hdr.entries = 0x10000000u;       /* entries*24 overflows past size */
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, (uint32_t)sizeof(f)), 0,
                   "entry-count overflow past table size rejected");
}

static void test_wdat_zero_entries(void)
{
    struct wdat_fixture f;
    wd_make_valid(&f);
    f.hdr.entries = 0;
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, (uint32_t)sizeof(f)), 0,
                   "zero entries rejected");
}

static void test_wdat_bad_gas(void)
{
    struct wdat_fixture f;
    wd_make_valid(&f);
    f.e[0].register_region.address = 0;   /* unusable register region */
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, (uint32_t)sizeof(f)), 0,
                   "entry with address-0 GAS rejected");
    wd_make_valid(&f);
    f.e[1].register_region.bit_width = 7; /* not 8/16/32 */
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, (uint32_t)sizeof(f)), 0,
                   "entry with bad GAS bit_width rejected");
}

static void test_wdat_missing_action(void)
{
    struct wdat_fixture f;
    wd_make_valid(&f);
    f.e[1].action = ACPI_WDAT_RESET;   /* drop SET_STOPPED_STATE (no disarm) */
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, (uint32_t)sizeof(f)), 0,
                   "missing SET_STOPPED_STATE (no disarm path) rejected");
}

static void test_wdat_unknown_instruction(void)
{
    struct wdat_fixture f;
    wd_make_valid(&f);
    f.e[0].instruction = 5;            /* > WRITE_COUNTDOWN (3), no flag */
    TEST_ASSERT_EQ(hw_watchdog_wdat_validate(&f.hdr, (uint32_t)sizeof(f)), 0,
                   "unknown instruction code rejected");
}

static void test_wdat_kind_none_before_init(void)
{
    /* Without init (or on QEMU with no WDAT), the kind is NONE. */
    TEST_ASSERT_EQ((int)hw_watchdog_kind() == HW_WD_WDAT, 0,
                   "kind is not WDAT without a real armed WDAT");
}

void test_register_watchdog(void)
{
    test_suite_register_cat("WDAT: valid table", test_wdat_valid, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: null/too-small", test_wdat_null, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: size too small", test_wdat_size_too_small, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: bad header_length", test_wdat_bad_header_length, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: zero timer_period", test_wdat_zero_timer_period, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: min>max count", test_wdat_bad_counts, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: entry overflow", test_wdat_entry_overflow, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: zero entries", test_wdat_zero_entries, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: bad GAS", test_wdat_bad_gas, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: missing required action", test_wdat_missing_action, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: unknown instruction", test_wdat_unknown_instruction, TEST_CAT_BOOT);
    test_suite_register_cat("WDAT: kind none before arm", test_wdat_kind_none_before_init, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
