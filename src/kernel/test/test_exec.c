/* ============================================================================
 * test_exec.c -- Binary format system unit tests
 *
 * Tests exec dispatcher (§1), EIF loader (§5), and module registration (§6).
 *
 * XREF: 02-kernel-core/TODO-08-binary-system.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/exec.h"
#include "kernel/eif.h"
#include "kernel/errno.h"

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

void test_register_exec(void)
{
    /* Exec dispatcher tests (TODO-08 §1) */
    test_suite_register_cat("Exec: bad magic", test_exec_bad_magic, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: null data", test_exec_null_data, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: errno constants", test_exec_errno, TEST_CAT_EXEC);

    /* Module registration tests (TODO-08 §6) */
    test_suite_register_cat("Exec: module struct size", test_module_struct_size, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: module register+find", test_module_register_and_find, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: module register NULL", test_module_register_null, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: module register invalid", test_module_register_invalid, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: module find NULL out", test_module_find_null_out, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: module count", test_module_count, TEST_CAT_EXEC);

    /* EIF loader tests (TODO-08 §5) */
    test_suite_register_cat("EIF: bad magic", test_eif_bad_magic, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: too small", test_eif_too_small, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: struct sizes", test_eif_struct_sizes, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: constants", test_eif_constants, TEST_CAT_EXEC);
}

#endif /* KERNEL_TESTS */
