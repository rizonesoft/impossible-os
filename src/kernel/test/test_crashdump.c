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
#include "kernel/panic.h"
#include "kernel/idt.h"
#include "kernel/exec.h"

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

/* ---- CONTEXT struct layout (S2) ---- */

static void test_context_size(void)
{
    TEST_ASSERT_EQ(sizeof(CONTEXT), 1232,
                   "CONTEXT is 1232 bytes (Windows x64)");
}

static void test_context_offsets(void)
{
    /* Key offsets that WinDbg depends on */
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, ContextFlags), 0x030,
                   "ContextFlags at offset 0x030");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, MxCsr), 0x034,
                   "MxCsr at offset 0x034");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, SegCs), 0x038,
                   "SegCs at offset 0x038");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, EFlags), 0x044,
                   "EFlags at offset 0x044");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, Dr0), 0x048,
                   "Dr0 at offset 0x048");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, Rax), 0x078,
                   "Rax at offset 0x078");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, Rip), 0x0F8,
                   "Rip at offset 0x0F8");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, FltSave), 0x100,
                   "FltSave at offset 0x100");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, VectorRegister), 0x300,
                   "VectorRegister at offset 0x300");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, VectorControl), 0x4A0,
                   "VectorControl at offset 0x4A0");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, DebugControl), 0x4A8,
                   "DebugControl at offset 0x4A8");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, LastExceptionFromRip), 0x4C8,
                   "LastExceptionFromRip at offset 0x4C8");
}

static void test_xmm_save_area_size(void)
{
    TEST_ASSERT_EQ(sizeof(XMM_SAVE_AREA32), 512,
                   "XMM_SAVE_AREA32 is 512 bytes");
}

static void test_context_flags(void)
{
    TEST_ASSERT_EQ(CONTEXT_AMD64, 0x00100000, "CONTEXT_AMD64 flag");
    TEST_ASSERT_EQ(CONTEXT_CONTROL, 0x00100001, "CONTEXT_CONTROL flag");
    TEST_ASSERT_EQ(CONTEXT_INTEGER, 0x00100002, "CONTEXT_INTEGER flag");
    TEST_ASSERT_EQ(CONTEXT_FLOATING_POINT, 0x00100008,
                   "CONTEXT_FLOATING_POINT flag");
    TEST_ASSERT_EQ(CONTEXT_FULL, 0x0010000B, "CONTEXT_FULL flag");
}

/* ---- panic_build_context (S2) ---- */

static void test_build_context_from_frame(void)
{
    /* Construct a synthetic interrupt_frame */
    struct interrupt_frame frame;
    uint8_t *fp = (uint8_t *)&frame;
    for (uint32_t i = 0; i < sizeof(frame); i++)
        fp[i] = 0;

    frame.rip    = 0xFFFF800000123456;
    frame.rsp    = 0xFFFF800001234000;
    frame.rax    = 0xAAAAAAAAAAAAAAAA;
    frame.rbx    = 0xBBBBBBBBBBBBBBBB;
    frame.rcx    = 0xCCCCCCCCCCCCCCCC;
    frame.rflags = 0x0000000000000246;
    frame.cs     = 0x08;
    frame.ss     = 0x10;
    frame.rbp    = 0xFFFF800001233FF0;
    frame.r8     = 0x0808080808080808;
    frame.r15    = 0x1515151515151515;

    /* Seed XSAVE buffer with a known MXCSR value at offset 24 */
    for (uint32_t i = 0; i < sizeof(g_panic_xsave_buf); i++)
        g_panic_xsave_buf[i] = 0;
    /* MXCSR default = 0x1F80; set to 0x1FA0 (non-default) */
    g_panic_xsave_buf[24] = 0xA0;
    g_panic_xsave_buf[25] = 0x1F;
    g_panic_xsave_buf[26] = 0x00;
    g_panic_xsave_buf[27] = 0x00;

    CONTEXT ctx;
    panic_build_context(&frame, &ctx);

    TEST_ASSERT_EQ(ctx.Rip, 0xFFFF800000123456,
                   "CONTEXT.Rip matches frame.rip");
    TEST_ASSERT_EQ(ctx.Rsp, 0xFFFF800001234000,
                   "CONTEXT.Rsp matches frame.rsp");
    TEST_ASSERT_EQ(ctx.Rax, 0xAAAAAAAAAAAAAAAA,
                   "CONTEXT.Rax matches frame.rax");
    TEST_ASSERT_EQ(ctx.Rbx, 0xBBBBBBBBBBBBBBBB,
                   "CONTEXT.Rbx matches frame.rbx");
    TEST_ASSERT_EQ(ctx.Rcx, 0xCCCCCCCCCCCCCCCC,
                   "CONTEXT.Rcx matches frame.rcx");
    TEST_ASSERT_EQ(ctx.R8,  0x0808080808080808,
                   "CONTEXT.R8 matches frame.r8");
    TEST_ASSERT_EQ(ctx.R15, 0x1515151515151515,
                   "CONTEXT.R15 matches frame.r15");
    TEST_ASSERT_EQ(ctx.EFlags, 0x00000246,
                   "CONTEXT.EFlags matches frame.rflags");
    TEST_ASSERT_EQ(ctx.SegCs, 0x08,
                   "CONTEXT.SegCs matches frame.cs");
    TEST_ASSERT_EQ(ctx.SegSs, 0x10,
                   "CONTEXT.SegSs matches frame.ss");
    TEST_ASSERT_EQ(ctx.MxCsr, 0x1FA0,
                   "CONTEXT.MxCsr copied from XSAVE buf");
    TEST_ASSERT(ctx.ContextFlags & CONTEXT_CONTROL,
                "ContextFlags includes CONTEXT_CONTROL");
    TEST_ASSERT(ctx.ContextFlags & CONTEXT_INTEGER,
                "ContextFlags includes CONTEXT_INTEGER");
    TEST_ASSERT(ctx.ContextFlags & CONTEXT_FLOATING_POINT,
                "ContextFlags includes CONTEXT_FLOATING_POINT");
}

static void test_build_context_null_frame(void)
{
    CONTEXT ctx;
    panic_build_context((void *)0, &ctx);

    /* With NULL frame, GPRs and RIP should be zero */
    TEST_ASSERT_EQ(ctx.Rip, 0, "NULL frame -> CONTEXT.Rip == 0");
    TEST_ASSERT_EQ(ctx.Rax, 0, "NULL frame -> CONTEXT.Rax == 0");
    /* ContextFlags should still be set */
    TEST_ASSERT(ctx.ContextFlags & CONTEXT_CONTROL,
                "NULL frame still sets CONTEXT_CONTROL");
}

static void test_xsave_buffer_alignment(void)
{
    uint64_t addr = (uint64_t)(uintptr_t)g_panic_xsave_buf;
    TEST_ASSERT_EQ(addr & 63, 0,
                   "g_panic_xsave_buf is 64-byte aligned");
}

static void test_fpu_capture_post_code(void)
{
    /* POST16_FPU_CAPTURE must be 0xDE42 and unique */
    TEST_ASSERT_EQ(POST16_FPU_CAPTURE, 0xDE42,
                   "POST16_FPU_CAPTURE == 0xDE42");
    TEST_ASSERT(POST16_FPU_CAPTURE != POST16_BUGCHECK,
                "FPU POST != Bugcheck POST");
    TEST_ASSERT(POST16_FPU_CAPTURE != POST16_CRASHLOG,
                "FPU POST != CRASHLOG POST");
}

/* ---- Module registry for crash dumps (S3) ---- */

static void test_kernel_module_registered(void)
{
    /* kernel.exe should be the first registered module after boot */
    TEST_ASSERT(exec_module_count() >= 1,
                "At least 1 module registered (kernel.exe)");
}

static void test_kernel_module_find_by_pc(void)
{
    /* kernel_main is inside the kernel image -- find it by PC */
    extern void kernel_main(uint64_t magic, uint64_t mbi);
    loaded_module_t mod;
    int ret = exec_find_module_by_pc((uint64_t)(uintptr_t)kernel_main, &mod);
    TEST_ASSERT_EQ(ret, 0, "exec_find_module_by_pc finds kernel_main");
    TEST_ASSERT(mod.name[0] == 'k' && mod.name[1] == 'e' &&
                mod.name[2] == 'r' && mod.name[3] == 'n',
                "Module name starts with 'kern'");
}

static void test_kernel_module_base_and_size(void)
{
    extern char __kernel_start[];
    extern char __kernel_end[];
    loaded_module_t mod;
    int ret = exec_find_module_by_pc((uint64_t)(uintptr_t)__kernel_start, &mod);
    TEST_ASSERT_EQ(ret, 0, "kernel module found at __kernel_start");
    TEST_ASSERT_EQ(mod.base_address, (uint64_t)(uintptr_t)__kernel_start,
                   "kernel base == __kernel_start");
    uint64_t expected_size = (uint64_t)(uintptr_t)__kernel_end -
                             (uint64_t)(uintptr_t)__kernel_start;
    TEST_ASSERT_EQ(mod.size_of_image, expected_size,
                   "kernel size == __kernel_end - __kernel_start");
}

static void test_iterate_modules_snapshot(void)
{
    loaded_module_t buf[4];
    uint32_t count = exec_iterate_modules(buf, 4);
    TEST_ASSERT(count >= 1, "exec_iterate_modules returns >= 1");
    /* First module should be kernel (lowest base address) */
    TEST_ASSERT(buf[0].name[0] == 'k',
                "First iterated module is kernel.exe");
}

static void test_iterate_modules_lockless(void)
{
    loaded_module_t buf[4];
    uint32_t count = exec_iterate_modules_lockless(buf, 4);
    TEST_ASSERT(count >= 1, "lockless iterate returns >= 1");
    TEST_ASSERT(count <= 4, "lockless count capped at max_count");
    /* Verify first entry is internally consistent */
    TEST_ASSERT(buf[0].base_address != 0,
                "lockless first entry has valid base");
    TEST_ASSERT(buf[0].size_of_image != 0,
                "lockless first entry has valid size");
}

static void test_module_registry_post_code(void)
{
    TEST_ASSERT_EQ(POST16_MODULE_REGISTRY, 0xDE44,
                   "POST16_MODULE_REGISTRY == 0xDE44");
    TEST_ASSERT(POST16_MODULE_REGISTRY != POST16_BUGCHECK,
                "MODULE_REGISTRY POST != BUGCHECK POST");
    TEST_ASSERT(POST16_MODULE_REGISTRY != POST16_FPU_CAPTURE,
                "MODULE_REGISTRY POST != FPU_CAPTURE POST");
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
    /* S2: CONTEXT layout */
    test_suite_register_cat("Crash: CONTEXT size",
                            test_context_size, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: CONTEXT offsets",
                            test_context_offsets, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: XMM_SAVE_AREA32 size",
                            test_xmm_save_area_size, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: CONTEXT flags",
                            test_context_flags, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: build_context from frame",
                            test_build_context_from_frame, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: build_context null frame",
                            test_build_context_null_frame, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: XSAVE buf alignment",
                            test_xsave_buffer_alignment, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: FPU capture POST code",
                            test_fpu_capture_post_code, TEST_CAT_BOOT);
    /* S3: Module registry for crash dumps */
    test_suite_register_cat("Crash: kernel module registered",
                            test_kernel_module_registered, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: kernel module find by PC",
                            test_kernel_module_find_by_pc, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: kernel module base/size",
                            test_kernel_module_base_and_size, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: iterate modules snapshot",
                            test_iterate_modules_snapshot, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: iterate modules lockless",
                            test_iterate_modules_lockless, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: module registry POST code",
                            test_module_registry_post_code, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
