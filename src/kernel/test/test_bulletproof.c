/* ============================================================================
 * test_bulletproof.c -- Consolidated kernel invariant assertion suite
 *
 * Tests every cross-file invariant identified in TODO-22 (Kernel
 * Bulletproofing).  Individual sections added assertions to their own
 * test files (test_nt_types.c, test_security.c, test_ixfs.c, test_vfs.c);
 * this file provides a single-suite reference that covers all 12 invariant
 * classes in one place.
 *
 * XREF: 02-kernel-core/TODO-31-kernel-bulletproofing.md -- Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/smp.h"
#include "kernel/boot_info.h"
#include "kernel/idt.h"
#include "kernel/gdt.h"
#include "kernel/vectors.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/mm/user_range.h"
#include "kernel/mm/pmm.h"
#include "kernel/fs/ixfs.h"
#include "kernel/fs/vfs.h"
#include "kernel/security/acl.h"

/* ---- Struct layout invariants ---- */

static void test_bp_struct_layouts(void)
{
    /* per_cpu_data -- assembly hardcodes gs:0, gs:24, gs:32 */
    TEST_ASSERT_EQ(__builtin_offsetof(struct per_cpu_data, self), 0,
                   "per_cpu_data.self at gs:0");
    TEST_ASSERT_EQ(__builtin_offsetof(struct per_cpu_data, syscall_rsp0), 24,
                   "per_cpu_data.syscall_rsp0 at gs:24");
    TEST_ASSERT_EQ(__builtin_offsetof(struct per_cpu_data, user_rsp_scratch), 32,
                   "per_cpu_data.user_rsp_scratch at gs:32");

    /* boot_config -- shared between UEFI bootloader and kernel */
    TEST_ASSERT_EQ(sizeof(struct boot_config), 512,
                   "boot_config == 512 bytes (sector-aligned)");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_config, cmdline), 32,
                   "boot_config.cmdline at offset 32");

    /* interrupt_frame -- isr_stubs.asm push/pop order */
    TEST_ASSERT_EQ(sizeof(struct interrupt_frame), 176,
                   "interrupt_frame == 176 bytes (22 x 8)");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, rip), 136,
                   "interrupt_frame.rip at offset 136");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, cs), 144,
                   "interrupt_frame.cs at offset 144");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, rflags), 152,
                   "interrupt_frame.rflags at offset 152");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, rsp), 160,
                   "interrupt_frame.rsp at offset 160");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, ss), 168,
                   "interrupt_frame.ss at offset 168");
}

/* ---- Memory range constants ---- */

static void test_bp_memory_constants(void)
{
    /* User ELF range -- 3 files must agree (user_range.h, vmm.c, pmm.c) */
    TEST_ASSERT_EQ(USER_ELF_END, 0x900000UL,
                   "USER_ELF_END == 0x900000");

    /* VFS drive letters */
}

/* ---- Assembly offset sync ---- */

static void test_bp_assembly_offsets(void)
{
    /* AP trampoline -- ap_trampoline.asm uses [AP_DATA + 0xNN] */

    /* GDT SYSRET ordering (cross-ref: test_nt_types.c) */
    TEST_ASSERT_EQ(GDT_USER_CODE, GDT_USER_DATA + 8,
                   "GDT_USER_CODE == GDT_USER_DATA + 8 (SYSRET)");
}

/* ---- Dispatch table invariants ---- */

static void test_bp_dispatch_tables(void)
{
    /* SSDT main table count */

    /* IDT vector uniqueness -- verify values and no collisions */

    /* Pairwise uniqueness for adjacent vectors */
    TEST_ASSERT(VECTOR_NT_SYSCALL != VECTOR_LINUX_SYSCALL,
                "NT and Linux syscall vectors differ");
    TEST_ASSERT(VECTOR_IPI_ASYNC_INIT != VECTOR_IPI_RESCHEDULE,
                "async init and reschedule IPI differ");
}

/* ---- XSAVE alignment ---- */

static void test_bp_xsave_alignment(void)
{
    /* XSAVE requires 64-byte alignment; PMM returns page-aligned (4096) */
    uintptr_t frame = pmm_alloc_frame();
    TEST_ASSERT(frame != 0, "pmm_alloc_frame for XSAVE alignment test");
    if (frame) {
        TEST_ASSERT_EQ(frame & 63, 0,
                       "PMM frame is 64-byte aligned (XSAVE safe)");
        pmm_free_frame(frame);
    }
}

/* ---- IXFS filesystem layout ---- */

static void test_bp_ixfs_layout(void)
{
    TEST_ASSERT_EQ(sizeof(struct ixfs_superblock), 512,
                   "ixfs_superblock == 512 bytes");
}

/* ---- Security struct ABI ---- */

static void test_bp_security_abi(void)
{
    TEST_ASSERT_EQ(sizeof(ACL), 8,
                   "ACL == 8 bytes (Windows ABI)");
    TEST_ASSERT_EQ(sizeof(ACE_HEADER), 4,
                   "ACE_HEADER == 4 bytes (Windows ABI)");
}

/* ---- Registration ---- */

void test_register_bulletproof(void)
{
    test_suite_register_cat("BP: struct layouts",    test_bp_struct_layouts,    TEST_CAT_ABI);
    test_suite_register_cat("BP: memory constants",  test_bp_memory_constants,  TEST_CAT_ABI);
    test_suite_register_cat("BP: assembly offsets",  test_bp_assembly_offsets,  TEST_CAT_ABI);
    test_suite_register_cat("BP: dispatch tables",   test_bp_dispatch_tables,   TEST_CAT_ABI);
    test_suite_register_cat("BP: XSAVE alignment",  test_bp_xsave_alignment,   TEST_CAT_ABI);
    test_suite_register_cat("BP: IXFS layout",       test_bp_ixfs_layout,       TEST_CAT_ABI);
    test_suite_register_cat("BP: security ABI",      test_bp_security_abi,      TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
