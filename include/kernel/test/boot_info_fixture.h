/* boot_info_fixture.h -- the one boot_info the kernel unit tests build on.
 *
 * struct boot_info is about 28 KiB, and six suites each kept a private static
 * copy (s_br_buf, s_caps_buf, s_dec_buf, s_fixture_bi, s_rb_buf, s_test_buf):
 * 168 KiB of .bss in an image that must end below USER_BASE (TODO-33). The
 * runner executes one suite at a time on one thread, and every one of those
 * suites rebuilt its copy from scratch before each use, so they now share
 * this single fixture. TEST_BOOT_INFO is an lvalue of type struct boot_info:
 * `TEST_BOOT_INFO.flags`, `&TEST_BOOT_INFO` and `sizeof(TEST_BOOT_INFO)` all
 * read as they did on the private copies.
 *
 * A suite must not keep a pointer into it past its own body, and must reset
 * it before relying on any field: another suite wrote it last. */
#pragma once

#ifdef KERNEL_TESTS

#include "kernel/boot_info.h"

struct boot_info *test_boot_info_fixture(void);

#define TEST_BOOT_INFO (*test_boot_info_fixture())

#endif /* KERNEL_TESTS */
