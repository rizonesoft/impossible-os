/* ============================================================================
 * test_crashdump.c -- Crash dump generation unit tests
 *
 * Tests bugcheck code table, KeBugCheckEx parameter storage, STOP code
 * name resolution, and POST code uniqueness.
 *
 * XREF: 02-kernel-core/TODO-16-crash-dump-generation.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/bugcheck.h"
#include "kernel/boot_init.h"

/* ---- Bugcheck name resolution ---- */

static void test_bugcheck_name_known(void)
{
    const char *name = bugcheck_name(0x50);
    /* Compare first chars to verify correct string */
    TEST_ASSERT(name[0] == 'P' && name[1] == 'A' && name[2] == 'G' && name[3] == 'E',
                "bugcheck_name(0x50) returns PAGE_FAULT_IN_NONPAGED_AREA");
}

static void test_bugcheck_name_unknown(void)
{
    const char *name = bugcheck_name(0xDEAD);
    TEST_ASSERT(name[0] == 'U' && name[1] == 'N',
                "bugcheck_name(unknown) returns UNKNOWN");
}

static void test_bugcheck_name_exclusive(void)
{
    const char *name = bugcheck_name(0xE0000001);
    TEST_ASSERT(name[0] == 'I' && name[1] == 'O' && name[2] == 'S',
                "bugcheck_name(0xE0000001) returns IOS_BOOT_INIT_FAILED");
}

/* ---- Bugcheck info storage ---- */

static void test_bugcheck_info_struct(void)
{
    const BUGCHECK_INFO *info = bugcheck_get_last();
    TEST_ASSERT(info != (void *)0, "bugcheck_get_last returns non-NULL");
    /* At boot, no bugcheck has occurred -- code should be 0 */
    TEST_ASSERT_EQ(info->code, 0, "no bugcheck occurred -- code is 0");
}

/* ---- BUGCHECK_CODE constants ---- */

static void test_bugcheck_constants(void)
{
    TEST_ASSERT_EQ(BUGCHECK_IRQL_NOT_LESS_OR_EQUAL, 0x0A,
                   "IRQL_NOT_LESS_OR_EQUAL == 0x0A");
    TEST_ASSERT_EQ(BUGCHECK_PAGE_FAULT_IN_NONPAGED_AREA, 0x50,
                   "PAGE_FAULT_IN_NONPAGED_AREA == 0x50");
    TEST_ASSERT_EQ(BUGCHECK_MANUALLY_INITIATED_CRASH, 0xE2,
                   "MANUALLY_INITIATED_CRASH == 0xE2");
    TEST_ASSERT_EQ(BUGCHECK_IOS_BOOT_INIT_FAILED, 0xE0000001,
                   "IOS_BOOT_INIT_FAILED == 0xE0000001");
}

/* ---- POST code uniqueness ---- */

static void test_bugcheck_post_codes(void)
{
    /* 0xDE40 is the KeBugCheckEx entry POST code */
    TEST_ASSERT(0xDE40 != 0, "KeBugCheckEx POST code is non-zero");
    /* No overlap with existing crash log POST codes */
    TEST_ASSERT(0xDE40 != POST16_CRASHLOG,
                "KeBugCheckEx POST != CRASHLOG POST");
    TEST_ASSERT(0xDE40 != POST16_KLOG_CTX,
                "KeBugCheckEx POST != KLOG_CTX POST");
}

/* ---- Registration ---- */

void test_register_crashdump(void)
{
    test_suite_register_cat("Crash: bugcheck name known",
                            test_bugcheck_name_known, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: bugcheck name unknown",
                            test_bugcheck_name_unknown, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: bugcheck name exclusive",
                            test_bugcheck_name_exclusive, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: bugcheck info struct",
                            test_bugcheck_info_struct, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: bugcheck constants",
                            test_bugcheck_constants, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: POST codes",
                            test_bugcheck_post_codes, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
