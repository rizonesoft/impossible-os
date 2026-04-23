/* ============================================================================
 * test_exec.c -- Binary format system unit tests
 *
 * Tests exec dispatcher, EIF loader, and module registration.
 *
 * XREF: 02-kernel-core/TODO-17-binary-system.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/exec.h"
#include "kernel/eif.h"
#include "kernel/pe.h"
#include "kernel/errno.h"

/* ---- Exec dispatcher tests ---- */

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

/* ---- Module registration tests ---- */

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

/* ---- PE32+ parser tests ---- */

/* Helper: build a minimal valid PE32+ header in a buffer.
 * Returns the total size written. Buffer must be >= 512 bytes. */
static uint32_t build_minimal_pe32plus(uint8_t *buf)
{
    uint32_t i;
    for (i = 0; i < 512; i++) buf[i] = 0;

    /* DOS header: MZ magic + e_lfanew at 0x3C pointing to offset 0x80 */
    buf[0] = 'M'; buf[1] = 'Z';
    buf[0x3C] = 0x80; /* e_lfanew = 0x80 */

    /* PE signature at 0x80 */
    buf[0x80] = 'P'; buf[0x81] = 'E'; buf[0x82] = 0; buf[0x83] = 0;

    /* COFF header at 0x84: Machine=0x8664, NumberOfSections=1,
     * SizeOfOptionalHeader=240 */
    buf[0x84] = 0x64; buf[0x85] = 0x86;  /* Machine = AMD64 */
    buf[0x86] = 0x01; buf[0x87] = 0x00;  /* NumberOfSections = 1 */
    buf[0x94] = 0xF0; buf[0x95] = 0x00;  /* SizeOfOptionalHeader = 240 */

    /* Optional header at 0x98: Magic=0x20B, AddressOfEntryPoint=0x1000,
     * ImageBase=0x140000000, SizeOfImage=0x2000, SizeOfHeaders=0x200,
     * NumberOfRvaAndSizes=16 */
    buf[0x98] = 0x0B; buf[0x99] = 0x02;  /* Magic = PE32+ */
    buf[0xA8] = 0x00; buf[0xA9] = 0x10;  /* AddressOfEntryPoint = 0x1000 */
    buf[0xB0] = 0x00; buf[0xB1] = 0x00;
    buf[0xB2] = 0x00; buf[0xB3] = 0x40;
    buf[0xB4] = 0x01; buf[0xB5] = 0x00;  /* ImageBase = 0x140000000 */
    /* SizeOfImage at Optional Header offset 0x38 = file offset 0x98+0x38=0xD0 */
    buf[0xD0] = 0x00; buf[0xD1] = 0x20;  /* SizeOfImage = 0x2000 */
    /* SizeOfHeaders at Optional Header offset 0x3C = file offset 0xD4 */
    buf[0xD4] = 0x00; buf[0xD5] = 0x02;  /* SizeOfHeaders = 0x200 */
    /* NumberOfRvaAndSizes at Optional Header offset 0x6C = file offset 0x98+0x6C=0x104 */
    buf[0x104] = 0x10;  /* NumberOfRvaAndSizes = 16 */

    /* Section header at 0x98 + 240 = 0x188 (fits in 512 bytes) */
    /* .text section: VirtualSize=0x100, VirtualAddress=0x1000,
     * SizeOfRawData=0x100, PointerToRawData=0x200,
     * Characteristics=0x60000020 (CODE | EXECUTE | READ) */
    buf[0x188] = '.'; buf[0x189] = 't'; buf[0x18A] = 'e';
    buf[0x18B] = 'x'; buf[0x18C] = 't';
    buf[0x190] = 0x00; buf[0x191] = 0x01;  /* VirtualSize = 0x100 */
    buf[0x194] = 0x00; buf[0x195] = 0x10;  /* VirtualAddress = 0x1000 */
    buf[0x198] = 0x00; buf[0x199] = 0x01;  /* SizeOfRawData = 0x100 */
    buf[0x19C] = 0x00; buf[0x19D] = 0x02;  /* PointerToRawData = 0x200 */
    buf[0x1AC] = 0x20; buf[0x1AD] = 0x00;
    buf[0x1AE] = 0x00; buf[0x1AF] = 0x60;  /* Characteristics = 0x60000020 */

    return 512;
}

static void test_pe_struct_sizes(void)
{
    TEST_ASSERT_EQ(sizeof(pe_dos_header_t), 64, "pe_dos_header_t == 64 bytes");
    TEST_ASSERT_EQ(sizeof(pe_coff_header_t), 20, "pe_coff_header_t == 20 bytes");
    TEST_ASSERT_EQ(sizeof(pe_optional_header64_t), 240, "pe_optional_header64_t == 240 bytes");
    TEST_ASSERT_EQ(sizeof(pe_section_header_t), 40, "pe_section_header_t == 40 bytes");
    TEST_ASSERT_EQ(sizeof(pe_data_directory_t), 8, "pe_data_directory_t == 8 bytes");
}

static void test_pe_constants(void)
{
    TEST_ASSERT_EQ(PE_DOS_MAGIC, 0x5A4D, "PE_DOS_MAGIC == 0x5A4D");
    TEST_ASSERT_EQ(PE_SIGNATURE, 0x00004550, "PE_SIGNATURE == 0x00004550");
    TEST_ASSERT_EQ(PE_MACHINE_AMD64, 0x8664, "PE_MACHINE_AMD64 == 0x8664");
    TEST_ASSERT_EQ(PE_OPT_MAGIC_PE32PLUS, 0x20B, "PE_OPT_MAGIC_PE32PLUS == 0x20B");
    TEST_ASSERT_EQ(PE_OPT_MAGIC_PE32, 0x10B, "PE_OPT_MAGIC_PE32 == 0x10B");
}

static void test_pe_validate_valid(void)
{
    uint8_t buf[512];
    build_minimal_pe32plus(buf);

    pe_validate_result_t r = pe_validate(buf, sizeof(buf));
    TEST_ASSERT_EQ(r.ok, 1, "pe_validate accepts valid PE32+");
    TEST_ASSERT_EQ(r.err, 0, "pe_validate sets err=0 on success");
    TEST_ASSERT_EQ(r.num_sections, 1, "pe_validate sees 1 section");
}

static void test_pe_validate_32bit(void)
{
    uint8_t buf[512];
    build_minimal_pe32plus(buf);

    /* Change Optional Header Magic from 0x20B to 0x10B (PE32) */
    buf[0x98] = 0x0B; buf[0x99] = 0x01;

    pe_validate_result_t r = pe_validate(buf, sizeof(buf));
    TEST_ASSERT_EQ(r.ok, 0, "pe_validate rejects 32-bit PE");
    TEST_ASSERT_EQ(r.err, ENOEXEC, "pe_validate sets ENOEXEC for 32-bit PE");
}

static void test_pe_validate_truncated(void)
{
    uint8_t buf[32];
    uint32_t i;
    for (i = 0; i < sizeof(buf); i++) buf[i] = 0;
    buf[0] = 'M'; buf[1] = 'Z';

    pe_validate_result_t r = pe_validate(buf, sizeof(buf));
    TEST_ASSERT_EQ(r.ok, 0, "pe_validate rejects truncated PE");
}

static void test_pe_validate_bad_magic(void)
{
    uint8_t buf[64];
    uint32_t i;
    for (i = 0; i < sizeof(buf); i++) buf[i] = 0;
    buf[0] = 0xDE; buf[1] = 0xAD;

    pe_validate_result_t r = pe_validate(buf, sizeof(buf));
    TEST_ASSERT_EQ(r.ok, 0, "pe_validate rejects non-MZ magic");
}

static void test_pe_validate_null(void)
{
    pe_validate_result_t r = pe_validate((const uint8_t *)0, 0);
    TEST_ASSERT_EQ(r.ok, 0, "pe_validate rejects NULL data");
}

/* ---- PE32+ section loader tests ---- */

static void test_pe_load_maps_and_returns_entry(void)
{
    uint8_t buf[1024];
    uint32_t i;
    for (i = 0; i < sizeof(buf); i++) buf[i] = 0;
    build_minimal_pe32plus(buf);

    /* pe_load should validate, map sections, and return entry VA */
    uint64_t entry = pe_load(buf, sizeof(buf));
    /* Entry = ImageBase(0x140000000) + AddressOfEntryPoint(0x1000) */
    TEST_ASSERT_EQ(entry, 0x140001000ULL, "pe_load returns correct entry VA");
}

static void test_pe_load_registers_module(void)
{
    /* After pe_load, exec_find_module_by_pc should find the module */
    loaded_module_t mod;
    int ret = exec_find_module_by_pc(0x140001000ULL, &mod);
    TEST_ASSERT_EQ(ret, 0, "pe_load registered module in crash registry");
    TEST_ASSERT_EQ(mod.base_address, 0x140000000ULL, "PE module base correct");
    TEST_ASSERT_EQ(mod.format, (uint64_t)EXEC_FMT_PE, "PE module format correct");
}

static void test_pe_load_rejects_low_imagebase(void)
{
    uint8_t buf[512];
    build_minimal_pe32plus(buf);

    /* Set ImageBase to 0x1000 (below PE_MIN_IMAGE_BASE) */
    buf[0xB0] = 0x00; buf[0xB1] = 0x10;
    buf[0xB2] = 0x00; buf[0xB3] = 0x00;
    buf[0xB4] = 0x00; buf[0xB5] = 0x00;

    uint64_t entry = pe_load(buf, sizeof(buf));
    TEST_ASSERT_EQ(entry, 0, "pe_load rejects low ImageBase");
}

/* ---- PE32+ import resolver tests ---- */

static void test_pe_import_struct_sizes(void)
{
    TEST_ASSERT_EQ(sizeof(pe_import_descriptor_t), 20, "pe_import_descriptor_t == 20 bytes");
    TEST_ASSERT_EQ(sizeof(pe_export_entry_t), 16, "pe_export_entry_t == 16 bytes");
}

/* ---- EIF loader tests ---- */

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
    TEST_ASSERT_EQ(EIF_MAGIC, 0x21464945, "EIF_MAGIC == 0x21464945 (file-order LE 'EIF!')");
    TEST_ASSERT_EQ(EIF_VERSION, 1, "EIF_VERSION == 1");
    TEST_ASSERT_EQ(EIF_ARCH_X86_64, 1, "EIF_ARCH_X86_64 == 1");
    TEST_ASSERT_EQ(EIF_FLAG_SIGNED, 8, "EIF_FLAG_SIGNED == 8");
}

/* ---- Registration ---- */

void test_register_exec(void)
{
    /* Exec dispatcher tests */
    test_suite_register_cat("Exec: bad magic", test_exec_bad_magic, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: null data", test_exec_null_data, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: errno constants", test_exec_errno, TEST_CAT_EXEC);

    /* Module registration tests */
    test_suite_register_cat("Exec: module struct size", test_module_struct_size, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: module register+find", test_module_register_and_find, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: module register NULL", test_module_register_null, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: module register invalid", test_module_register_invalid, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: module find NULL out", test_module_find_null_out, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: module count", test_module_count, TEST_CAT_EXEC);

    /* PE32+ parser tests */
    test_suite_register_cat("PE: struct sizes", test_pe_struct_sizes, TEST_CAT_EXEC);
    test_suite_register_cat("PE: constants", test_pe_constants, TEST_CAT_EXEC);
    test_suite_register_cat("PE: validate valid PE32+", test_pe_validate_valid, TEST_CAT_EXEC);
    test_suite_register_cat("PE: validate rejects 32-bit", test_pe_validate_32bit, TEST_CAT_EXEC);
    test_suite_register_cat("PE: validate rejects truncated", test_pe_validate_truncated, TEST_CAT_EXEC);
    test_suite_register_cat("PE: validate rejects bad magic", test_pe_validate_bad_magic, TEST_CAT_EXEC);
    test_suite_register_cat("PE: validate rejects NULL", test_pe_validate_null, TEST_CAT_EXEC);

    /* PE32+ section loader tests */
    test_suite_register_cat("PE: load maps+returns entry", test_pe_load_maps_and_returns_entry, TEST_CAT_EXEC);
    test_suite_register_cat("PE: load registers module", test_pe_load_registers_module, TEST_CAT_EXEC);
    test_suite_register_cat("PE: load rejects low ImageBase", test_pe_load_rejects_low_imagebase, TEST_CAT_EXEC);

    /* PE32+ import resolver tests */
    test_suite_register_cat("PE: import struct sizes", test_pe_import_struct_sizes, TEST_CAT_EXEC);

    /* EIF loader tests */
    test_suite_register_cat("EIF: bad magic", test_eif_bad_magic, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: too small", test_eif_too_small, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: struct sizes", test_eif_struct_sizes, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: constants", test_eif_constants, TEST_CAT_EXEC);
}

#endif /* KERNEL_TESTS */
