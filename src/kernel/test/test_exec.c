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
#include "kernel/nt/ssdt.h"
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
    TEST_ASSERT_EQ(PE_DOS_MAGIC, 0x5A4D, "PE_DOS_MAGIC == 'MZ'");
    TEST_ASSERT_EQ(PE_SIGNATURE, 0x00004550, "PE_SIGNATURE == 'PE\\0\\0'");
    TEST_ASSERT_EQ(PE_SIGNATURE_SIZE, 4, "PE_SIGNATURE_SIZE == 4");
    TEST_ASSERT_EQ(PE_MACHINE_AMD64, 0x8664, "PE_MACHINE_AMD64 == 0x8664");
    TEST_ASSERT_EQ(PE_MACHINE_I386, 0x014C, "PE_MACHINE_I386 == 0x014C");
    TEST_ASSERT_EQ(PE_OPT_MAGIC_PE32, 0x10B, "PE_OPT_MAGIC_PE32 == 0x10B");
    TEST_ASSERT_EQ(PE_OPT_MAGIC_PE32PLUS, 0x20B, "PE_OPT_MAGIC_PE32PLUS == 0x20B");
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
    TEST_ASSERT_EQ(r.err, ENOEXEC, "pe_validate sets ENOEXEC for truncated PE");
}

static void test_pe_validate_bad_magic(void)
{
    uint8_t buf[64];
    uint32_t i;
    for (i = 0; i < sizeof(buf); i++) buf[i] = 0;
    buf[0] = 0xDE; buf[1] = 0xAD;

    pe_validate_result_t r = pe_validate(buf, sizeof(buf));
    TEST_ASSERT_EQ(r.ok, 0, "pe_validate rejects non-MZ magic");
    TEST_ASSERT_EQ(r.err, ENOEXEC, "pe_validate sets ENOEXEC for bad magic");
}

static void test_pe_validate_null(void)
{
    pe_validate_result_t r = pe_validate((const uint8_t *)0, 0);
    TEST_ASSERT_EQ(r.ok, 0, "pe_validate rejects NULL data");
    TEST_ASSERT_EQ(r.err, ENOEXEC, "pe_validate sets ENOEXEC for NULL data");
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
    TEST_ASSERT_EQ(EIF_MAX_IMPORTS, EIF_DISPATCH_TABLE_MAX,
                   "EIF_MAX_IMPORTS == EIF_DISPATCH_TABLE_MAX");
}

/* Lay down a minimal valid EIF header (magic/version/arch, everything else
 * zeroed) into buf. Each rule-6/7/8 rejection test below corrupts exactly one
 * field; every one rejects during eif_validate or pass 1 of the segment walk,
 * so none reach the pass-2 copy -- no user frame or PMM state is mutated. */
static void eif_test_build_header(uint8_t *buf, uint64_t size)
{
    eif_header_t *h = (eif_header_t *)buf;
    uint64_t k;
    for (k = 0; k < size; k++)
        buf[k] = 0;
    h->magic = EIF_MAGIC;
    h->version = EIF_VERSION;
    h->arch = EIF_ARCH_X86_64;
    h->load_base = 0;
}

/* Rule 6: segment_count above the loader cap is rejected before the table is
 * walked. */
static void test_eif_reject_too_many_segments(void)
{
    uint8_t buf[64];
    eif_header_t *h = (eif_header_t *)buf;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->segment_count = EIF_MAX_SEGMENTS + 1;
    h->segment_offset = sizeof(eif_header_t);
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects segment_count over EIF_MAX_SEGMENTS");
}

/* Rule 6: import_count above the loader cap is rejected before the table is
 * walked (and before it could overrun the dispatch table it writes). */
static void test_eif_reject_too_many_imports(void)
{
    uint8_t buf[64];
    eif_header_t *h = (eif_header_t *)buf;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->segment_count = 0;
    h->import_count = EIF_MAX_IMPORTS + 1;
    h->import_offset = sizeof(eif_header_t);
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects import_count over EIF_MAX_IMPORTS");
}

/* Rule 8: a metadata_offset pointing past the end of an unsigned file is
 * rejected (its range [metadata_offset, file_size) would be empty/OOB). */
static void test_eif_reject_metadata_out_of_bounds(void)
{
    uint8_t buf[64];
    eif_header_t *h = (eif_header_t *)buf;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->metadata_offset = sizeof(buf) + 0x1000;  /* past EOF, unsigned file */
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects metadata_offset past end of file");
}

/* Rule 7: segments must be sorted by ascending, non-overlapping vaddr. Two
 * BSS-only segments (file_size 0) at the same vaddr are individually valid but
 * overlap; rejected in pass 1 before any copy. */
static void test_eif_reject_unsorted_segments(void)
{
    uint8_t buf[128];  /* 64-byte header + two 32-byte segment entries */
    eif_header_t *h = (eif_header_t *)buf;
    eif_segment_t *seg;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->segment_count = 2;
    h->segment_offset = sizeof(eif_header_t);
    seg = (eif_segment_t *)(buf + sizeof(eif_header_t));
    seg[0].vaddr = USER_ELF_BASE; seg[0].file_offset = 0;
    seg[0].file_size = 0; seg[0].mem_size = 0x1000; seg[0].flags = EIF_SEG_READ;
    seg[1].vaddr = USER_ELF_BASE; seg[1].file_offset = 0;
    seg[1].file_size = 0; seg[1].mem_size = 0x1000; seg[1].flags = EIF_SEG_READ;
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects non-ascending/overlapping segments");
}

/* Atomicity (validation precedes mutation): a file with a VALID segment but an
 * out-of-range entry point is rejected in the validation phase, before any
 * segment is copied -- so a failed exec cannot overwrite the prior image. */
static void test_eif_reject_bad_entry_valid_segments(void)
{
    uint8_t buf[96];  /* 64-byte header + one 32-byte segment */
    eif_header_t *h = (eif_header_t *)buf;
    eif_segment_t *seg;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->segment_count = 1;
    h->segment_offset = sizeof(eif_header_t);
    h->entry_point = USER_ELF_END + 0x1000;  /* out of user range */
    seg = (eif_segment_t *)(buf + sizeof(eif_header_t));
    seg->vaddr = USER_ELF_BASE; seg->file_offset = 0;
    seg->file_size = 0; seg->mem_size = 0x1000; seg->flags = EIF_SEG_READ;
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects out-of-range entry point with valid segments");
}

/* Atomicity: a file with a VALID segment but an out-of-range REQUIRED import is
 * rejected in the validation phase (imports are validated before the copy). */
static void test_eif_reject_bad_import_valid_segments(void)
{
    uint8_t buf[104];  /* header + one segment + one import */
    eif_header_t *h = (eif_header_t *)buf;
    eif_segment_t *seg;
    eif_import_t *imp;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->segment_count = 1;
    h->segment_offset = sizeof(eif_header_t);
    h->import_count = 1;
    h->import_offset = sizeof(eif_header_t) + sizeof(eif_segment_t);
    h->entry_point = USER_ELF_BASE;  /* entry_va valid; isolates the import fail */
    seg = (eif_segment_t *)(buf + sizeof(eif_header_t));
    seg->vaddr = USER_ELF_BASE; seg->file_offset = 0;
    seg->file_size = 0; seg->mem_size = 0x1000; seg->flags = EIF_SEG_READ;
    imp = (eif_import_t *)(buf + sizeof(eif_header_t) + sizeof(eif_segment_t));
    /* SSDT_MAIN_MAX (0x400) is the FIRST id past the main table -- ssdt_dispatch
     * cannot reach it, so a required import here must be rejected before mutation
     * (the index-mask 0x0FFF bound would have wrongly accepted it). */
    imp->syscall_id = SSDT_MAIN_MAX;
    imp->flags = 0;                          /* required (not OPTIONAL) */
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects out-of-range required import with valid segments");
}

/* Rule 3 canonical order: an unsigned file (SIGNED flag clear) must have
 * signature_offset == 0. A non-zero signature_offset on an unsigned file is
 * rejected so it cannot truncate its own metadata range. */
static void test_eif_reject_unsigned_with_signature(void)
{
    uint8_t buf[64];
    eif_header_t *h = (eif_header_t *)buf;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    /* EIF_FLAG_SIGNED intentionally NOT set */
    h->signature_offset = 32;  /* non-zero, in-file */
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects unsigned file with non-zero signature_offset");
}

/* Fail-closed: a COMPRESSED EIF must be rejected -- the loader copies segment
 * bytes verbatim into the executable range and has no decompressor, so loading
 * one would execute the raw LZ4 stream as code. */
static void test_eif_reject_compressed(void)
{
    uint8_t buf[64];
    eif_header_t *h = (eif_header_t *)buf;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->flags = EIF_FLAG_COMPRESSED;
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects COMPRESSED EIF (no decompressor yet)");
}

/* Entry point must land inside a loaded EXECUTABLE segment, not merely the user
 * window (shared identity-mapped range may hold stale residue). A read-only
 * segment covering the entry VA is rejected. */
static void test_eif_reject_entry_not_in_exec_segment(void)
{
    uint8_t buf[96];  /* header + one segment */
    eif_header_t *h = (eif_header_t *)buf;
    eif_segment_t *seg;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->segment_count = 1;
    h->segment_offset = sizeof(eif_header_t);
    h->entry_point = USER_ELF_BASE;  /* lands in the segment below */
    seg = (eif_segment_t *)(buf + sizeof(eif_header_t));
    seg->vaddr = USER_ELF_BASE; seg->file_offset = 0;
    seg->file_size = 0; seg->mem_size = 0x1000;
    seg->flags = EIF_SEG_READ;  /* NOT executable */
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects entry point not inside an executable segment");
}

/* A required import to an in-range SSDT slot that is still the not-implemented
 * stub is rejected (availability means registered, not merely in range). */
static void test_eif_reject_unregistered_import(void)
{
    uint8_t buf[104];  /* header + segment + import */
    eif_header_t *h = (eif_header_t *)buf;
    eif_segment_t *seg;
    eif_import_t *imp;
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    uint32_t unreg = SSDT_MAIN_MAX;  /* sentinel: none found */
    uint32_t k;

    /* Find an in-range main-SSDT slot that is still the stub. */
    if (tbl && tbl->handlers) {
        for (k = SSDT_MAIN_MAX; k-- > 0; ) {
            if (tbl->handlers[k] == ssdt_stub_not_implemented) { unreg = k; break; }
        }
    }
    if (unreg == SSDT_MAIN_MAX) {
        TEST_SKIP("no unregistered main-SSDT slot available");
        return;
    }

    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->segment_count = 1;
    h->segment_offset = sizeof(eif_header_t);
    h->import_count = 1;
    h->import_offset = sizeof(eif_header_t) + sizeof(eif_segment_t);
    h->entry_point = USER_ELF_BASE;
    seg = (eif_segment_t *)(buf + sizeof(eif_header_t));
    seg->vaddr = USER_ELF_BASE; seg->file_offset = 0;
    seg->file_size = 0; seg->mem_size = 0x1000; seg->flags = EIF_SEG_EXEC;
    imp = (eif_import_t *)(buf + sizeof(eif_header_t) + sizeof(eif_segment_t));
    imp->syscall_id = unreg;  /* in range but unregistered */
    imp->flags = 0;           /* required */
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects required import to an unregistered in-range slot");
}

/* Canonical section order (spec rule 2): placing the import table before the
 * segment table is rejected even though each table is individually in-bounds. */
static void test_eif_reject_out_of_order_sections(void)
{
    uint8_t buf[104];  /* header + import table + segment table */
    eif_header_t *h = (eif_header_t *)buf;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->import_count = 1;
    h->segment_count = 1;
    h->import_offset = sizeof(eif_header_t);                          /* 64 */
    h->segment_offset = sizeof(eif_header_t) + sizeof(eif_import_t);  /* 72 */
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects sections in non-canonical order");
}

/* Canonical order (spec rule 2): segment DATA must sit after all tables. A
 * segment whose file range points back into the segment table is rejected even
 * though the range is in-bounds against EOF. */
static void test_eif_reject_segment_data_over_table(void)
{
    uint8_t buf[96];  /* header + one 32-byte segment (tables_end = 96) */
    eif_header_t *h = (eif_header_t *)buf;
    eif_segment_t *seg;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->segment_count = 1;
    h->segment_offset = sizeof(eif_header_t);
    h->entry_point = USER_ELF_BASE;
    seg = (eif_segment_t *)(buf + sizeof(eif_header_t));
    seg->vaddr = USER_ELF_BASE;
    seg->file_offset = sizeof(eif_header_t);  /* 64 -- inside the segment table */
    seg->file_size = 8;                       /* non-empty file data */
    seg->mem_size = 0x1000; seg->flags = EIF_SEG_EXEC;
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects segment data overlapping a table");
}

/* Rule 2: an ABSENT table (count 0) must carry offset 0. A zero-count import
 * table with a non-zero offset would skip the canonical-order cursor -- reject. */
static void test_eif_reject_absent_table_nonzero_offset(void)
{
    uint8_t buf[96];  /* header + one 32-byte segment */
    eif_header_t *h = (eif_header_t *)buf;
    eif_segment_t *seg;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->segment_count = 1;
    h->segment_offset = sizeof(eif_header_t);
    h->entry_point = USER_ELF_BASE;
    h->import_count = 0;                       /* absent import table ... */
    h->import_offset = sizeof(eif_header_t);   /* ... but non-zero offset -> reject */
    seg = (eif_segment_t *)(buf + sizeof(eif_header_t));
    seg->vaddr = USER_ELF_BASE;
    seg->file_size = 0; seg->mem_size = 0x1000; seg->flags = EIF_SEG_EXEC;
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects a zero-count table with a non-zero offset");
}

/* The segment `reserved` field must be 0 -- a crafted binary that sets it is
 * rejected (spec compliance on untrusted disk-sourced data). */
static void test_eif_reject_segment_reserved_nonzero(void)
{
    uint8_t buf[96];
    eif_header_t *h = (eif_header_t *)buf;
    eif_segment_t *seg;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->segment_count = 1;
    h->segment_offset = sizeof(eif_header_t);
    h->entry_point = USER_ELF_BASE;
    seg = (eif_segment_t *)(buf + sizeof(eif_header_t));
    seg->vaddr = USER_ELF_BASE;
    seg->file_size = 0; seg->mem_size = 0x1000; seg->flags = EIF_SEG_EXEC;
    seg->reserved = 1;                          /* must be 0 -> reject */
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects a segment with a non-zero reserved field");
}

/* API-version gate (spec rule 4): an EIF requiring a NEWER OS API version than
 * the loader provides is rejected. The fixture is otherwise VALID (a page-aligned
 * executable segment), so ONLY the too-new api_version causes rejection -- remove
 * the gate and this fixture would proceed to load. Rejection fires at the early
 * api_version check, before any segment copy, so no real user memory is touched.
 * (The accept path api_version<=current is not unit-tested: a successful load
 * memcpys into the identity-mapped 0x800000 user range, which this harness
 * avoids -- all EIF tests are rejection-only.) */
static void test_eif_reject_api_version_too_new(void)
{
    uint8_t buf[104];  /* header(64) + one 32-byte segment + 8 bytes file data */
    eif_header_t *h = (eif_header_t *)buf;
    eif_segment_t *seg;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->api_version = EIF_CURRENT_API_VERSION + 1;   /* needs a newer OS -> reject */
    h->segment_count = 1;
    h->segment_offset = sizeof(eif_header_t);
    h->entry_point = USER_ELF_BASE;
    seg = (eif_segment_t *)(buf + sizeof(eif_header_t));
    seg->vaddr = USER_ELF_BASE;
    seg->file_offset = sizeof(eif_header_t) + sizeof(eif_segment_t);  /* 96 */
    seg->file_size = 8; seg->mem_size = 0x1000; seg->flags = EIF_SEG_READ | EIF_SEG_EXEC;
    TEST_ASSERT_EQ(eif_load(buf, sizeof(buf)), 0,
                   "eif_load rejects an otherwise-valid binary requiring a newer API version");
}

/* ---- EIF metadata parser -- pure, no mutation, safe to call directly ---- */

static int meta_streq(const char *a, const char *b)
{
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i])
            return 0;
        i++;
    }
    return a[i] == b[i];
}

static void meta_put_u32(uint8_t *buf, uint32_t pos, uint32_t v)
{
    buf[pos]     = (uint8_t)v;
    buf[pos + 1] = (uint8_t)(v >> 8);
    buf[pos + 2] = (uint8_t)(v >> 16);
    buf[pos + 3] = (uint8_t)(v >> 24);
}

/* Append one [key_len][key][val_len][val] record; return the new position. */
static uint32_t meta_put_kv(uint8_t *buf, uint32_t pos,
                            const char *key, const char *val)
{
    uint32_t kl = 0, vl = 0, i;
    while (key[kl]) kl++;
    while (val[vl]) vl++;
    meta_put_u32(buf, pos, kl); pos += 4;
    for (i = 0; i < kl; i++) buf[pos++] = (uint8_t)key[i];
    meta_put_u32(buf, pos, vl); pos += 4;
    for (i = 0; i < vl; i++) buf[pos++] = (uint8_t)val[i];
    return pos;
}

/* Well-formed name/version/author records populate the decode struct. */
static void test_eif_metadata_parsed(void)
{
    uint8_t buf[256];
    eif_header_t *h = (eif_header_t *)buf;
    eif_metadata_t meta;
    uint32_t pos;
    eif_test_build_header(buf, sizeof(buf));
    h->metadata_offset = sizeof(eif_header_t);
    pos = sizeof(eif_header_t);
    pos = meta_put_kv(buf, pos, "name", "hello");
    pos = meta_put_kv(buf, pos, "version", "1.0");
    pos = meta_put_kv(buf, pos, "author", "acme");
    meta_put_u32(buf, pos, 0);   /* key_len==0 terminator */
    TEST_ASSERT_EQ(eif_parse_metadata(buf, sizeof(buf), h, &meta), 1,
                   "eif_parse_metadata accepts well-formed metadata");
    TEST_ASSERT(meta_streq(meta.name, "hello"), "metadata name parsed");
    TEST_ASSERT(meta_streq(meta.version, "1.0"), "metadata version parsed");
    TEST_ASSERT(meta_streq(meta.author, "acme"), "metadata author parsed");
}

/* A value length that runs past the metadata range end is rejected. */
static void test_eif_metadata_reject_truncated(void)
{
    uint8_t buf[128];
    eif_header_t *h = (eif_header_t *)buf;
    eif_metadata_t meta;
    uint32_t pos = sizeof(eif_header_t), i;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->metadata_offset = sizeof(eif_header_t);
    meta_put_u32(buf, pos, 4); pos += 4;                 /* key_len = 4 */
    for (i = 0; i < 4; i++) buf[pos++] = (uint8_t)"name"[i];
    meta_put_u32(buf, pos, 100); pos += 4;               /* val_len past end */
    TEST_ASSERT_EQ(eif_parse_metadata(buf, pos + 8, h, &meta), 0,
                   "eif_parse_metadata rejects value length past range end");
}

/* metadata_offset==0 is a valid no-metadata binary; out is all-zero. */
static void test_eif_metadata_absent_ok(void)
{
    uint8_t buf[64];
    eif_header_t *h = (eif_header_t *)buf;
    eif_metadata_t meta;
    eif_test_build_header(buf, sizeof(buf));
    h->metadata_offset = 0;
    TEST_ASSERT_EQ(eif_parse_metadata(buf, sizeof(buf), h, &meta), 1,
                   "eif_parse_metadata accepts metadata_offset==0");
    TEST_ASSERT_EQ(meta.name[0], 0, "absent metadata yields empty name");
    TEST_ASSERT_EQ(meta.build_id_len, 0, "absent metadata yields no build_id");
}

/* More than EIF_MAX_METADATA_RECORDS records is rejected before mutation. */
static void test_eif_metadata_reject_over_cap(void)
{
    uint8_t buf[1024];
    eif_header_t *h = (eif_header_t *)buf;
    eif_metadata_t meta;
    uint32_t pos = sizeof(eif_header_t), i;
    eif_test_build_header(buf, sizeof(buf));
    TEST_KLOG_SUPPRESS("eif");
    h->metadata_offset = sizeof(eif_header_t);
    for (i = 0; i < EIF_MAX_METADATA_RECORDS + 1; i++) {
        meta_put_u32(buf, pos, 1); pos += 4;   /* key_len = 1 (unknown key) */
        buf[pos++] = 'x';
        meta_put_u32(buf, pos, 0); pos += 4;   /* val_len = 0 */
    }
    TEST_ASSERT_EQ(eif_parse_metadata(buf, pos + 8, h, &meta), 0,
                   "eif_parse_metadata rejects over-cap record count");
}

/* build_id is a raw byte blob: length captured, bytes copied verbatim. */
static void test_eif_metadata_build_id(void)
{
    uint8_t buf[128];
    eif_header_t *h = (eif_header_t *)buf;
    eif_metadata_t meta;
    uint32_t pos = sizeof(eif_header_t), i;
    static const uint8_t bid[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    eif_test_build_header(buf, sizeof(buf));
    h->metadata_offset = sizeof(eif_header_t);
    meta_put_u32(buf, pos, 8); pos += 4;
    for (i = 0; i < 8; i++) buf[pos++] = (uint8_t)"build_id"[i];
    meta_put_u32(buf, pos, 4); pos += 4;
    for (i = 0; i < 4; i++) buf[pos++] = bid[i];
    meta_put_u32(buf, pos, 0);   /* terminator */
    TEST_ASSERT_EQ(eif_parse_metadata(buf, sizeof(buf), h, &meta), 1,
                   "eif_parse_metadata accepts a build_id record");
    TEST_ASSERT_EQ(meta.build_id_len, 4, "build_id length captured");
    TEST_ASSERT_EQ(meta.build_id[0], 0xDE, "build_id byte 0 copied");
    TEST_ASSERT_EQ(meta.build_id[3], 0xEF, "build_id byte 3 copied");
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
    test_suite_register_cat("EIF: reject too many segments", test_eif_reject_too_many_segments, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject too many imports", test_eif_reject_too_many_imports, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject metadata OOB", test_eif_reject_metadata_out_of_bounds, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject unsorted segments", test_eif_reject_unsorted_segments, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject bad entry", test_eif_reject_bad_entry_valid_segments, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject bad import", test_eif_reject_bad_import_valid_segments, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject unsigned+sig", test_eif_reject_unsigned_with_signature, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject entry non-exec", test_eif_reject_entry_not_in_exec_segment, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject unreg import", test_eif_reject_unregistered_import, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject out-of-order", test_eif_reject_out_of_order_sections, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject seg data over table", test_eif_reject_segment_data_over_table, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: absent-table offset", test_eif_reject_absent_table_nonzero_offset, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: segment reserved!=0", test_eif_reject_segment_reserved_nonzero, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: api_version too new", test_eif_reject_api_version_too_new, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: reject compressed", test_eif_reject_compressed, TEST_CAT_EXEC);

    /* EIF metadata key-value parser */
    test_suite_register_cat("EIF: metadata parsed", test_eif_metadata_parsed, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: metadata truncated", test_eif_metadata_reject_truncated, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: metadata absent OK", test_eif_metadata_absent_ok, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: metadata over cap", test_eif_metadata_reject_over_cap, TEST_CAT_EXEC);
    test_suite_register_cat("EIF: metadata build_id", test_eif_metadata_build_id, TEST_CAT_EXEC);
}

#endif /* KERNEL_TESTS */
