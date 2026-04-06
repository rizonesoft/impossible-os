/* ============================================================================
 * test_boot_init.c -- Boot init sequencing unit tests
 *
 * Tests boot_result_t values, subsystem readiness tracking, BOOT_REQUIRE
 * macro, boot_progress() null safety, and POST code uniqueness.
 *
 * XREF: 02-kernel-core/TODO-01-kernel-init-sequencing.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/boot_init.h"
#include "kernel/boot_timing.h"
#include "kernel/exec.h"
#include "kernel/eif.h"
#include "kernel/errno.h"

/* ---- boot_result_t values ---- */

static void test_boot_result_values(void)
{
    TEST_ASSERT(BOOT_OK == 0, "BOOT_OK == 0");
    TEST_ASSERT(BOOT_DEGRADED == 1, "BOOT_DEGRADED == 1");
    TEST_ASSERT(BOOT_FATAL == 2, "BOOT_FATAL == 2");
}

/* ---- Subsystem readiness ---- */

static void test_subsys_set_ready_true(void)
{
    /* Save and restore state to avoid side effects */
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == true,
                "set_ready(PMM, true) then ready(PMM) returns true");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

static void test_subsys_set_ready_false(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, false);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == false,
                "set_ready(PMM, false) then ready(PMM) returns false");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

static void test_subsys_out_of_range(void)
{
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_COUNT) == false,
                "ready(SUBSYS_COUNT) returns false (out of range)");
}

/* ---- BOOT_REQUIRE macro ----
 * BOOT_REQUIRE uses `return BOOT_FATAL`, so we test it via a wrapper
 * function that has the matching return type. */

static boot_result_t require_pmm_wrapper(void)
{
    BOOT_REQUIRE(SUBSYS_PMM);
    return BOOT_OK;
}

static void test_boot_require_fails_when_not_ready(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    /* Note: BOOT_REQUIRE writes "[BOOT] REQUIRE failed: SUBSYS_PMM not ready"
     * directly to serial -- this is expected test output, not a real failure. */
    kernel_subsystem_set_ready(SUBSYS_PMM, false);
    boot_result_t r = require_pmm_wrapper();
    TEST_ASSERT(r == BOOT_FATAL,
                "BOOT_REQUIRE returns BOOT_FATAL when PMM not ready");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

static void test_boot_require_passes_when_ready(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    boot_result_t r = require_pmm_wrapper();
    TEST_ASSERT(r == BOOT_OK,
                "BOOT_REQUIRE passes (returns BOOT_OK) when PMM ready");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

/* ---- boot_progress() null safety ---- */

static void test_boot_progress_null_step(void)
{
    /* Must not crash -- just call it and survive */
    boot_progress(0, (const char *)0, 0x0000);
    TEST_ASSERT(1, "boot_progress(NULL step) does not crash");
}

/* ---- POST code constants ---- */

static void test_post_codes_nonzero_and_unique(void)
{
    /* Spot-check representative codes from each phase are non-zero */
    TEST_ASSERT(POST16_SERIAL_OK != 0, "POST16_SERIAL_OK is non-zero");
    TEST_ASSERT(POST16_PMM_OK != 0, "POST16_PMM_OK is non-zero");
    TEST_ASSERT(POST16_GDT_OK != 0, "POST16_GDT_OK is non-zero");
    TEST_ASSERT(POST16_TIMER_OK != 0, "POST16_TIMER_OK is non-zero");
    TEST_ASSERT(POST16_VFS_OK != 0, "POST16_VFS_OK is non-zero");
    TEST_ASSERT(POST16_SCHED_OK != 0, "POST16_SCHED_OK is non-zero");

    /* Verify uniqueness across phases */
    TEST_ASSERT(POST16_PMM_OK != POST16_VMM_OK, "PMM_OK != VMM_OK");
    TEST_ASSERT(POST16_GDT_OK != POST16_IDT_OK, "GDT_OK != IDT_OK");
    TEST_ASSERT(POST16_SCHED_OK != POST16_DESKTOP_OK, "SCHED_OK != DESKTOP_OK");
    TEST_ASSERT(POST16_BOOT_OK != POST16_BOOT_FAILED, "BOOT_OK != BOOT_FAILED");
}

/* ---- BOOT_DEFERRED value ---- */

static void test_boot_deferred_value(void)
{
    TEST_ASSERT_EQ(BOOT_DEFERRED, 3, "BOOT_DEFERRED == 3");
}

/* ---- Deferred init registration ---- */

static boot_result_t deferred_test_fn(void)
{
    return BOOT_OK;
}

static void test_boot_defer_register(void)
{
    /* boot_defer returns 0 on success.
     * Note: this adds to the real deferred array, but boot_run_deferred()
     * has already run by the time tests execute, so these entries won't
     * cause problems -- they'd run only if boot_run_deferred() is called
     * again (which it won't be). */
    int r = boot_defer("test_deferred", deferred_test_fn);
    TEST_ASSERT_EQ(r, 0, "boot_defer returns 0 on success");
}

/* ---- Deferred POST codes ---- */

static void test_deferred_post_codes(void)
{
    TEST_ASSERT(POST16_DEFERRED != 0, "POST16_DEFERRED is non-zero");
    TEST_ASSERT(POST16_DEFERRED_OK != 0, "POST16_DEFERRED_OK is non-zero");
    TEST_ASSERT(POST16_DEFERRED != POST16_DEFERRED_OK,
                "DEFERRED != DEFERRED_OK");
    TEST_ASSERT(POST16_DEFERRED_NET != POST16_DEFERRED_INPUT,
                "DEFERRED_NET != DEFERRED_INPUT");
}

/* ---- Boot perf record struct ---- */

static void test_boot_perf_record_size(void)
{
    /* boot_perf_record_t must be 24 bytes for NVRAM layout stability */
    TEST_ASSERT_EQ(sizeof(boot_perf_record_t), 24,
                   "boot_perf_record_t is 24 bytes");
}

static void test_boot_perf_header_magic(void)
{
    TEST_ASSERT_EQ(BOOT_PERF_MAGIC, 0x50455246, "BOOT_PERF_MAGIC == 'PERF'");
}

/* ---- Boot perf POST codes ---- */

static void test_bootperf_post_codes(void)
{
    TEST_ASSERT(POST16_BOOTPERF != 0, "POST16_BOOTPERF is non-zero");
    TEST_ASSERT(POST16_BOOTPERF_WRITE != 0, "POST16_BOOTPERF_WRITE is non-zero");
    TEST_ASSERT(POST16_BOOTPERF != POST16_BOOTPERF_READ,
                "BOOTPERF != BOOTPERF_READ");
    TEST_ASSERT(POST16_BOOTPERF_CMP != POST16_BOOTPERF_WRITE,
                "BOOTPERF_CMP != BOOTPERF_WRITE");
    /* Confirm no overlap with deferred POST range */
    TEST_ASSERT(POST16_BOOTPERF != POST16_DEFERRED,
                "BOOTPERF != DEFERRED");
}

/* ---- Async init POST codes and IPI vector ---- */

static void test_async_post_codes(void)
{
    TEST_ASSERT(POST16_ASYNC != 0, "POST16_ASYNC is non-zero");
    TEST_ASSERT(POST16_ASYNC_DONE != 0, "POST16_ASYNC_DONE is non-zero");
    TEST_ASSERT(POST16_ASYNC != POST16_ASYNC_AP,
                "ASYNC != ASYNC_AP");
    TEST_ASSERT(POST16_ASYNC_BARRIER != POST16_ASYNC_DONE,
                "ASYNC_BARRIER != ASYNC_DONE");
    /* No overlap with other debug ranges */
    TEST_ASSERT(POST16_ASYNC != POST16_DEFERRED,
                "ASYNC != DEFERRED");
    TEST_ASSERT(POST16_ASYNC != POST16_BOOTPERF,
                "ASYNC != BOOTPERF");
}

static void test_async_ipi_vector(void)
{
    TEST_ASSERT_EQ(IPI_VECTOR_ASYNC_INIT, 0xFC, "IPI_VECTOR_ASYNC_INIT == 0xFC");
    /* Must not collide with existing IPI vectors */
    TEST_ASSERT(IPI_VECTOR_ASYNC_INIT != 0xFD,
                "ASYNC_INIT != RESCHEDULE (0xFD)");
    TEST_ASSERT(IPI_VECTOR_ASYNC_INIT != 0xFE,
                "ASYNC_INIT != TLB_SHOOTDOWN (0xFE)");
}

/* ---- Exec dispatcher tests (TODO-08 §1) ---- */

static void test_exec_bad_magic(void)
{
    int err = 0;
    uint8_t bad_data[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0 };
    uint64_t entry = exec_load(bad_data, sizeof(bad_data), &err);
    TEST_ASSERT_EQ(entry, 0, "exec_load rejects unknown magic");
    TEST_ASSERT_EQ(err, ENOEXEC, "exec_load sets ENOEXEC for bad magic");
}

static void test_exec_null_data(void)
{
    int err = 0;
    uint64_t entry = exec_load((const uint8_t *)0, 0, &err);
    TEST_ASSERT_EQ(entry, 0, "exec_load rejects NULL data");
    TEST_ASSERT_EQ(err, ENOEXEC, "exec_load sets ENOEXEC for NULL");
}

static void test_exec_errno(void)
{
    TEST_ASSERT_EQ(ENOEXEC, 8, "ENOEXEC == 8");
    TEST_ASSERT_EQ(ENOENT,  2, "ENOENT == 2");
    TEST_ASSERT_EQ(ENOMEM, 12, "ENOMEM == 12");
    TEST_ASSERT_EQ(EINVAL, 22, "EINVAL == 22");
}

/* ---- Module registration tests (TODO-08 §6) ---- */

static void test_module_struct_size(void)
{
    TEST_ASSERT_EQ(sizeof(loaded_module_t), 368, "loaded_module_t == 368 bytes");
    TEST_ASSERT_EQ(EXEC_MAX_MODULES, 64, "EXEC_MAX_MODULES == 64");
    TEST_ASSERT_EQ(EXEC_MODULE_NAME_MAX, 64, "EXEC_MODULE_NAME_MAX == 64");
    TEST_ASSERT_EQ(EXEC_MODULE_PATH_MAX, 256, "EXEC_MODULE_PATH_MAX == 256");
}

static void test_module_register_and_find(void)
{
    loaded_module_t mod;
    loaded_module_t found;
    uint8_t *p = (uint8_t *)&mod;
    uint32_t i;
    for (i = 0; i < sizeof(mod); i++) p[i] = 0;

    mod.base_address = 0xA00000;
    mod.size_of_image = 0x20000;
    mod.entry_point = 0xA00100;
    mod.format = EXEC_FMT_ELF;
    mod.name[0] = 'h'; mod.name[1] = 'e'; mod.name[2] = 'l';
    mod.name[3] = 'l'; mod.name[4] = 'o'; mod.name[5] = 0;

    int ret = exec_register_module((process_t *)0, &mod);
    TEST_ASSERT_EQ(ret, 0, "exec_register_module succeeds");

    /* Find by entry point (middle of module) */
    ret = exec_find_module_by_pc(0xA00100, &found);
    TEST_ASSERT_EQ(ret, 0, "find_by_pc finds module at entry");
    TEST_ASSERT_EQ(found.base_address, 0xA00000, "found module has correct base");
    TEST_ASSERT_EQ(found.size_of_image, 0x20000, "found module has correct size");

    /* Find at exact base address */
    ret = exec_find_module_by_pc(0xA00000, &found);
    TEST_ASSERT_EQ(ret, 0, "find_by_pc finds module at exact base");

    /* Find at last valid byte (base + size - 1) */
    ret = exec_find_module_by_pc(0xA1FFFF, &found);
    TEST_ASSERT_EQ(ret, 0, "find_by_pc finds module at last byte");

    /* Address at exact end (base + size) should NOT match */
    ret = exec_find_module_by_pc(0xA20000, &found);
    TEST_ASSERT_EQ(ret, -1, "find_by_pc misses at exact end");

    /* Address past end of module should not match */
    ret = exec_find_module_by_pc(0xB00000, &found);
    TEST_ASSERT_EQ(ret, -1, "find_by_pc returns -1 for miss");
}

static void test_module_register_null(void)
{
    int ret = exec_register_module((process_t *)0, (const loaded_module_t *)0);
    TEST_ASSERT_EQ(ret, -1, "exec_register_module rejects NULL");
}

static void test_module_register_invalid(void)
{
    loaded_module_t mod;
    uint8_t *p = (uint8_t *)&mod;
    uint32_t i;
    for (i = 0; i < sizeof(mod); i++) p[i] = 0;

    /* Zero base_address should be rejected */
    mod.base_address = 0;
    mod.size_of_image = 0x1000;
    int ret = exec_register_module((process_t *)0, &mod);
    TEST_ASSERT_EQ(ret, -1, "rejects zero base_address");

    /* Zero size_of_image should be rejected */
    mod.base_address = 0xC00000;
    mod.size_of_image = 0;
    ret = exec_register_module((process_t *)0, &mod);
    TEST_ASSERT_EQ(ret, -1, "rejects zero size_of_image");
}

static void test_module_find_null_out(void)
{
    int ret = exec_find_module_by_pc(0xA00000, (loaded_module_t *)0);
    TEST_ASSERT_EQ(ret, -1, "find_by_pc rejects NULL out buffer");
}

static void test_module_count(void)
{
    uint32_t count = exec_module_count();
    TEST_ASSERT_NEQ((uint64_t)count, 0, "module count > 0 after registration");
}

/* ---- EIF loader tests (TODO-08 §5) ---- */

static void test_eif_bad_magic(void)
{
    uint8_t bad[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0,0,0,0,0,0,0,0,0,0,0,0,
                      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
    uint64_t entry = eif_load(bad, sizeof(bad));
    TEST_ASSERT_EQ(entry, 0, "eif_load rejects bad magic");
}

static void test_eif_too_small(void)
{
    uint8_t small[] = { 'E', 'I', 'F', '!' };
    uint64_t entry = eif_load(small, sizeof(small));
    TEST_ASSERT_EQ(entry, 0, "eif_load rejects data smaller than header");
}

static void test_eif_struct_sizes(void)
{
    TEST_ASSERT_EQ(sizeof(eif_header_t), 64, "eif_header_t == 64 bytes");
    TEST_ASSERT_EQ(sizeof(eif_segment_t), 32, "eif_segment_t == 32 bytes");
    TEST_ASSERT_EQ(sizeof(eif_import_t), 8, "eif_import_t == 8 bytes");
}

static void test_eif_constants(void)
{
    TEST_ASSERT_EQ(EIF_MAGIC, 0x45494621, "EIF_MAGIC == 0x45494621");
    TEST_ASSERT_EQ(EIF_VERSION, 1, "EIF_VERSION == 1");
    TEST_ASSERT_EQ(EIF_ARCH_X86_64, 1, "EIF_ARCH_X86_64 == 1");
    TEST_ASSERT_EQ(EIF_FLAG_SIGNED, 8, "EIF_FLAG_SIGNED == 8");
}

/* ---- Registration ---- */

void test_register_boot_init(void)
{
    test_suite_register_cat("Boot init: result values", test_boot_result_values, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: set_ready true", test_subsys_set_ready_true, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: set_ready false", test_subsys_set_ready_false, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: out of range", test_subsys_out_of_range, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: REQUIRE fails", test_boot_require_fails_when_not_ready, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: REQUIRE passes", test_boot_require_passes_when_ready, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: progress null", test_boot_progress_null_step, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: POST codes", test_post_codes_nonzero_and_unique, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: BOOT_DEFERRED value", test_boot_deferred_value, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: defer register", test_boot_defer_register, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: deferred POST codes", test_deferred_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: perf record size", test_boot_perf_record_size, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: perf magic", test_boot_perf_header_magic, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: bootperf POST codes", test_bootperf_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: async POST codes", test_async_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: async IPI vector", test_async_ipi_vector, TEST_CAT_BOOT);

    /* Exec dispatcher tests (TODO-08 §1) */
    test_suite_register_cat("Exec: bad magic", test_exec_bad_magic, TEST_CAT_BOOT);
    test_suite_register_cat("Exec: null data", test_exec_null_data, TEST_CAT_BOOT);
    test_suite_register_cat("Exec: errno constants", test_exec_errno, TEST_CAT_BOOT);

    /* Module registration tests (TODO-08 §6) */
    test_suite_register_cat("Exec: module struct size", test_module_struct_size, TEST_CAT_BOOT);
    test_suite_register_cat("Exec: module register+find", test_module_register_and_find, TEST_CAT_BOOT);
    test_suite_register_cat("Exec: module register NULL", test_module_register_null, TEST_CAT_BOOT);
    test_suite_register_cat("Exec: module register invalid", test_module_register_invalid, TEST_CAT_BOOT);
    test_suite_register_cat("Exec: module find NULL out", test_module_find_null_out, TEST_CAT_BOOT);
    test_suite_register_cat("Exec: module count", test_module_count, TEST_CAT_BOOT);

    /* EIF loader tests (TODO-08 §5) */
    test_suite_register_cat("EIF: bad magic", test_eif_bad_magic, TEST_CAT_BOOT);
    test_suite_register_cat("EIF: too small", test_eif_too_small, TEST_CAT_BOOT);
    test_suite_register_cat("EIF: struct sizes", test_eif_struct_sizes, TEST_CAT_BOOT);
    test_suite_register_cat("EIF: constants", test_eif_constants, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
