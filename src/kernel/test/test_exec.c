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
#include "kernel/elf.h"
#include "kernel/errno.h"
#include "kernel/mm/user_range.h"
#include "kernel/test/klog_suppress.h"

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
    /* Behavioral errno path: a buffer shorter than the 4-byte minimum magic
     * must be rejected with ENOEXEC before any format match runs (exec.c
     * size < 4 guard) -- distinct from the null-data and full-length
     * bad-magic paths above. */
    int err = 99;
    uint8_t tiny[3] = { 0x7F, 'E', 'L' };  /* valid ELF prefix but sub-magic-length */
    uint64_t entry = exec_load(tiny, sizeof(tiny), &err);
    TEST_ASSERT_EQ(entry, 0, "exec_load rejects sub-magic-length buffer");
    TEST_ASSERT_EQ(err, ENOEXEC, "exec_load sets ENOEXEC for short buffer");
}

/* ---- ELF loader malformed-input rejection tests ----
 * Each test corrupts one field of an otherwise-valid ELF64 image so that
 * elf_load() rejects it in the pass-1 validation walk, BEFORE the pass-2
 * segment copy. Rejected images never touch user memory (that is the whole
 * point of the two-pass split), so these are safe to run in the kernel test
 * harness. They lock in the ELF bounds hardening: attacker-controlled uint64
 * wrap in the phdr table / segment file bounds / user-range check, p_filesz >
 * p_memsz, overlapping PT_LOAD ranges, and e_entry not inside an executable
 * segment. The fixture is a genuinely valid single-PF_X-segment ELF whose
 * e_entry lands in that segment, so each test isolates exactly one defect. */

static uint8_t s_elf_buf[256];

static void elf_test_build_valid(void)
{
    struct elf64_header *h = (struct elf64_header *)s_elf_buf;
    struct elf64_phdr *p = (struct elf64_phdr *)(s_elf_buf + sizeof(struct elf64_header));
    uint32_t i;
    for (i = 0; i < sizeof(s_elf_buf); i++) s_elf_buf[i] = 0;
    h->e_ident[0] = 0x7F; h->e_ident[1] = 'E'; h->e_ident[2] = 'L'; h->e_ident[3] = 'F';
    h->e_ident[4] = ELFCLASS64; h->e_ident[5] = ELFDATA2LSB;
    h->e_type = ET_EXEC; h->e_machine = EM_X86_64;
    h->e_entry = USER_ELF_BASE;  /* inside the single PF_X segment below */
    h->e_phoff = sizeof(struct elf64_header);
    h->e_phentsize = sizeof(struct elf64_phdr);
    h->e_phnum = 1;
    p->p_type = PT_LOAD;
    p->p_flags = PF_R | PF_X;
    p->p_offset = 0; p->p_filesz = 0; p->p_memsz = 16;
    p->p_vaddr = USER_ELF_BASE;
}

static void test_elf_reject_bad_phentsize(void)
{
    struct elf64_header *h = (struct elf64_header *)s_elf_buf;
    struct elf_load_result r;
    elf_test_build_valid();
    h->e_phentsize = 8;  /* != sizeof(elf64_phdr) */
    r = elf_load(s_elf_buf, sizeof(s_elf_buf));
    TEST_ASSERT_EQ(r.success, 0, "elf_load rejects bad e_phentsize");
}

static void test_elf_reject_phoff_overflow(void)
{
    struct elf64_header *h = (struct elf64_header *)s_elf_buf;
    struct elf_load_result r;
    elf_test_build_valid();
    h->e_phoff = 0xFFFFFFFFFFFFFF00ULL;  /* would wrap in phoff+table */
    r = elf_load(s_elf_buf, sizeof(s_elf_buf));
    TEST_ASSERT_EQ(r.success, 0, "elf_load rejects wrapping e_phoff");
}

static void test_elf_reject_offset_overflow(void)
{
    struct elf64_phdr *p = (struct elf64_phdr *)(s_elf_buf + sizeof(struct elf64_header));
    struct elf_load_result r;
    elf_test_build_valid();
    p->p_offset = 0xFFFFFFFFFFFFFF00ULL;  /* p_offset + p_filesz would wrap */
    p->p_filesz = 0x200;
    r = elf_load(s_elf_buf, sizeof(s_elf_buf));
    TEST_ASSERT_EQ(r.success, 0, "elf_load rejects wrapping p_offset");
}

static void test_elf_reject_filesz_gt_memsz(void)
{
    struct elf64_phdr *p = (struct elf64_phdr *)(s_elf_buf + sizeof(struct elf64_header));
    struct elf_load_result r;
    elf_test_build_valid();
    TEST_KLOG_SUPPRESS("elf");  /* reject path logs LOG_ERROR -- silence [FAIL]-lookalike */
    p->p_offset = 0; p->p_filesz = 128; p->p_memsz = 64;  /* dest overflow attempt */
    r = elf_load(s_elf_buf, sizeof(s_elf_buf));
    TEST_ASSERT_EQ(r.success, 0, "elf_load rejects p_filesz > p_memsz");
}

static void test_elf_reject_vaddr_overflow(void)
{
    struct elf64_phdr *p = (struct elf64_phdr *)(s_elf_buf + sizeof(struct elf64_header));
    struct elf_load_result r;
    elf_test_build_valid();
    TEST_KLOG_SUPPRESS("elf");  /* reject path logs LOG_ERROR -- silence [FAIL]-lookalike */
    p->p_vaddr = 0xFFFFFFFFFFFFFF00ULL;  /* high vaddr + small memsz wraps below END */
    p->p_memsz = 16; p->p_filesz = 0;
    r = elf_load(s_elf_buf, sizeof(s_elf_buf));
    TEST_ASSERT_EQ(r.success, 0, "elf_load rejects wrapping p_vaddr");
}

/* e_entry in an inter-segment gap: the single valid segment stays, but e_entry
 * points past it. Pass 1 rejects (no PF_X segment covers e_entry) before any
 * copy, so the stale-residue-exec path is blocked non-destructively. */
static void test_elf_reject_entry_outside_segment(void)
{
    struct elf64_header *h = (struct elf64_header *)s_elf_buf;
    struct elf_load_result r;
    elf_test_build_valid();
    h->e_entry = USER_ELF_BASE + 0x1000;  /* in user range, outside the 16-byte segment */
    r = elf_load(s_elf_buf, sizeof(s_elf_buf));
    TEST_ASSERT_EQ(r.success, 0, "elf_load rejects e_entry outside all segments");
}

/* e_entry inside a non-executable segment: clearing PF_X must reject even
 * though e_entry is inside the segment's address range. */
static void test_elf_reject_entry_non_exec(void)
{
    struct elf64_phdr *p = (struct elf64_phdr *)(s_elf_buf + sizeof(struct elf64_header));
    struct elf_load_result r;
    elf_test_build_valid();
    p->p_flags = PF_R;  /* no PF_X; e_entry still at USER_ELF_BASE */
    r = elf_load(s_elf_buf, sizeof(s_elf_buf));
    TEST_ASSERT_EQ(r.success, 0, "elf_load rejects e_entry in a non-exec segment");
}

/* Excessive program-header count: e_phnum above ELF_MAX_PHNUM must be rejected
 * in elf_validate BEFORE the O(n^2) overlap walk, closing the crafted-ELF
 * exec-path DoS (tens of thousands of zero-sized PT_LOADs forcing ~2 billion
 * comparisons). Rejection happens on header validation, so no memory is touched. */
static void test_elf_reject_excessive_phnum(void)
{
    struct elf64_header *h = (struct elf64_header *)s_elf_buf;
    struct elf_load_result r;
    elf_test_build_valid();
    h->e_phnum = ELF_MAX_PHNUM + 1;  /* over the cap; table need not even fit */
    r = elf_load(s_elf_buf, sizeof(s_elf_buf));
    TEST_ASSERT_EQ(r.success, 0, "elf_load rejects e_phnum over ELF_MAX_PHNUM");
}

/* Two overlapping PT_LOAD segments: the spec forbids overlap, and allowing it
 * would let a dummy PF_X segment satisfy the entry check while a later non-exec
 * segment overwrites the bytes. Pass 1 rejects the overlap before any copy. */
static void test_elf_reject_overlapping_segments(void)
{
    struct elf64_header *h = (struct elf64_header *)s_elf_buf;
    struct elf64_phdr *p = (struct elf64_phdr *)(s_elf_buf + sizeof(struct elf64_header));
    struct elf_load_result r;
    elf_test_build_valid();
    TEST_KLOG_SUPPRESS("elf");  /* overlap reject logs LOG_ERROR -- silence [FAIL]-lookalike */
    h->e_phnum = 2;
    p[1].p_type = PT_LOAD;
    p[1].p_flags = PF_R | PF_W;
    p[1].p_offset = 0; p[1].p_filesz = 0; p[1].p_memsz = 16;
    p[1].p_vaddr = USER_ELF_BASE + 8;  /* overlaps [BASE, BASE+16) */
    r = elf_load(s_elf_buf, sizeof(s_elf_buf));
    TEST_ASSERT_EQ(r.success, 0, "elf_load rejects overlapping PT_LOAD segments");
}

/* ---- Module registration tests ---- */

static void test_module_struct_size(void)
{
    TEST_ASSERT_EQ(sizeof(loaded_module_t), 368, "loaded_module_t == 368 bytes");
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
    TEST_ASSERT_EQ(EIF_FLAG_SIGNED, 8, "EIF_FLAG_SIGNED == 8");
}

/* ---- Registration ---- */

void test_register_exec(void)
{
    /* Exec dispatcher tests */
    test_suite_register_cat("Exec: bad magic", test_exec_bad_magic, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: null data", test_exec_null_data, TEST_CAT_EXEC);
    test_suite_register_cat("Exec: short buffer ENOEXEC", test_exec_errno, TEST_CAT_EXEC);

    /* ELF loader malformed-input rejection */
    test_suite_register_cat("ELF: reject bad phentsize", test_elf_reject_bad_phentsize, TEST_CAT_EXEC);
    test_suite_register_cat("ELF: reject phoff overflow", test_elf_reject_phoff_overflow, TEST_CAT_EXEC);
    test_suite_register_cat("ELF: reject offset overflow", test_elf_reject_offset_overflow, TEST_CAT_EXEC);
    test_suite_register_cat("ELF: reject filesz>memsz", test_elf_reject_filesz_gt_memsz, TEST_CAT_EXEC);
    test_suite_register_cat("ELF: reject vaddr overflow", test_elf_reject_vaddr_overflow, TEST_CAT_EXEC);
    test_suite_register_cat("ELF: reject entry outside segment", test_elf_reject_entry_outside_segment, TEST_CAT_EXEC);
    test_suite_register_cat("ELF: reject entry non-exec", test_elf_reject_entry_non_exec, TEST_CAT_EXEC);
    test_suite_register_cat("ELF: reject excessive phnum", test_elf_reject_excessive_phnum, TEST_CAT_EXEC);
    test_suite_register_cat("ELF: reject overlapping segments", test_elf_reject_overlapping_segments, TEST_CAT_EXEC);

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
