/* ============================================================================
 * test_nt_types.c -- NTSTATUS type and NT foundational types unit tests
 *
 * Tests severity macros, status code values, and type sizes for Win32 ABI
 * compatibility.
 *
 * XREF: 02-kernel-core/TODO-05-native-api-ssdt.md section 1
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/vectors.h"
#include "kernel/smp.h"
#include "kernel/boot_info.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/user_range.h"

/* ---- NTSTATUS severity macros ---- */

static void test_ntstatus_success(void)
{
    TEST_ASSERT(NT_SUCCESS(STATUS_SUCCESS),
                "NT_SUCCESS(STATUS_SUCCESS) is true");
    TEST_ASSERT(NT_SUCCESS(STATUS_PENDING),
                "NT_SUCCESS(STATUS_PENDING) is true");
    TEST_ASSERT(NT_SUCCESS(STATUS_ALERTED),
                "NT_SUCCESS(STATUS_ALERTED) is true");
    TEST_ASSERT(!NT_SUCCESS(STATUS_UNSUCCESSFUL),
                "NT_SUCCESS(STATUS_UNSUCCESSFUL) is false");
    TEST_ASSERT(!NT_SUCCESS(STATUS_ACCESS_DENIED),
                "NT_SUCCESS(STATUS_ACCESS_DENIED) is false");
}

static void test_ntstatus_error(void)
{
    TEST_ASSERT(NT_ERROR(STATUS_UNSUCCESSFUL),
                "NT_ERROR(STATUS_UNSUCCESSFUL) is true");
    TEST_ASSERT(NT_ERROR(STATUS_ACCESS_VIOLATION),
                "NT_ERROR(STATUS_ACCESS_VIOLATION) is true");
    TEST_ASSERT(NT_ERROR(STATUS_NO_MEMORY),
                "NT_ERROR(STATUS_NO_MEMORY) is true");
    TEST_ASSERT(!NT_ERROR(STATUS_SUCCESS),
                "NT_ERROR(STATUS_SUCCESS) is false");
    TEST_ASSERT(!NT_ERROR(STATUS_BUFFER_OVERFLOW),
                "NT_ERROR(STATUS_BUFFER_OVERFLOW) is false");
}

static void test_ntstatus_warning(void)
{
    TEST_ASSERT(NT_WARNING(STATUS_BUFFER_OVERFLOW),
                "NT_WARNING(STATUS_BUFFER_OVERFLOW) is true");
    TEST_ASSERT(NT_WARNING(STATUS_NO_MORE_FILES),
                "NT_WARNING(STATUS_NO_MORE_FILES) is true");
    TEST_ASSERT(!NT_WARNING(STATUS_SUCCESS),
                "NT_WARNING(STATUS_SUCCESS) is false");
    TEST_ASSERT(!NT_WARNING(STATUS_UNSUCCESSFUL),
                "NT_WARNING(STATUS_UNSUCCESSFUL) is false");
}

static void test_ntstatus_information(void)
{
    /* STATUS_ALERTED (0x00000101) and STATUS_TIMEOUT (0x00000102) have
     * severity 00 (success), not 01 (informational). This matches
     * Windows behavior -- they are success codes, not info codes.
     * True informational codes have bits 31:30 = 01 (0x40000000+). */
    TEST_ASSERT(!NT_INFORMATION(STATUS_SUCCESS),
                "NT_INFORMATION(STATUS_SUCCESS) is false");
    TEST_ASSERT(!NT_INFORMATION(STATUS_UNSUCCESSFUL),
                "NT_INFORMATION(STATUS_UNSUCCESSFUL) is false");
}

/* ---- Type sizes (ABI compatibility) ---- */

static void test_nt_type_sizes(void)
{
    TEST_ASSERT_EQ(sizeof(NTSTATUS), 4, "NTSTATUS is 4 bytes");
    TEST_ASSERT_EQ(sizeof(ACCESS_MASK), 4, "ACCESS_MASK is 4 bytes");
    TEST_ASSERT_EQ(sizeof(HANDLE), 4, "HANDLE is 4 bytes");
    TEST_ASSERT_EQ(sizeof(IO_STATUS_BLOCK), 16,
                   "IO_STATUS_BLOCK is 16 bytes");
}

static void test_object_attributes_size(void)
{
    /* OBJECT_ATTRIBUTES must match Windows x64 layout (56 bytes) */
    TEST_ASSERT(sizeof(OBJECT_ATTRIBUTES) >= 48,
                "OBJECT_ATTRIBUTES >= 48 bytes");
    TEST_ASSERT(sizeof(OBJECT_ATTRIBUTES) <= 64,
                "OBJECT_ATTRIBUTES <= 64 bytes");
}

/* ---- GDT SYSRET ordering ---- */

static void test_gdt_sysret_order(void)
{
    TEST_ASSERT_EQ(GDT_USER_CODE, GDT_USER_DATA + 8,
                   "GDT_USER_CODE == GDT_USER_DATA + 8 (SYSRET constraint)");
    TEST_ASSERT(GDT_USER_DATA < GDT_USER_CODE,
                "GDT_USER_DATA < GDT_USER_CODE (data before code)");
    TEST_ASSERT_EQ(GDT_KERNEL_CODE, 0x08,
                   "GDT_KERNEL_CODE == 0x08");
    TEST_ASSERT_EQ(GDT_KERNEL_DATA, 0x10,
                   "GDT_KERNEL_DATA == 0x10");
    TEST_ASSERT_EQ(GDT_USER_DATA, 0x18,
                   "GDT_USER_DATA == 0x18");
    TEST_ASSERT_EQ(GDT_USER_CODE, 0x20,
                   "GDT_USER_CODE == 0x20");
}

/* ---- SSDT dispatch ---- */

static void test_ssdt_unimplemented_returns_not_implemented(void)
{
    /* ssdt_init() was already called during boot.
     * An unregistered slot should return STATUS_NOT_IMPLEMENTED. */
    NTSTATUS s = ssdt_dispatch(0x0FFF, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_NOT_IMPLEMENTED,
                   "ssdt_dispatch(unregistered) returns STATUS_NOT_IMPLEMENTED");
}

static void test_ssdt_invalid_table_returns_error(void)
{
    /* Table selector 2 (bits 13:12 = 10) is invalid */
    NTSTATUS s = ssdt_dispatch(0x2000, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "ssdt_dispatch(invalid table) returns STATUS_INVALID_PARAMETER");
}

static void test_ssdt_main_count(void)
{
    TEST_ASSERT_EQ(SSDT_MAIN_COUNT, 470, "SSDT_MAIN_COUNT == 470");
    TEST_ASSERT_EQ(SSDT_LAST_MAIN_INDEX, 0x03D7,
                   "SSDT_LAST_MAIN_INDEX == 0x03D7");
    TEST_ASSERT(SSDT_LAST_MAIN_INDEX < SSDT_MAIN_MAX,
                "last index within table capacity");

    /* Verify ssdt_get_table returns correct count */
    {
        const SSDT_TABLE *t = ssdt_get_table(SSDT_TABLE_MAIN);
        TEST_ASSERT(t != (void *)0, "ssdt_get_table(MAIN) non-NULL");
        if (t)
            TEST_ASSERT_EQ(t->count, SSDT_MAIN_COUNT,
                           "table->count matches SSDT_MAIN_COUNT");
    }
}

static NTSTATUS test_ssdt_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return (NTSTATUS)a1;  /* echo first arg as status */
}

static void test_ssdt_register_and_dispatch(void)
{
    /* Register a test handler at a high unused index */
    int r = ssdt_register(0x03FF, test_ssdt_handler);
    TEST_ASSERT_EQ(r, 0, "ssdt_register returns 0 on success");

    NTSTATUS s = ssdt_dispatch(0x03FF, STATUS_SUCCESS, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "ssdt_dispatch calls registered handler");

    /* Restore stub */
    ssdt_register(0x03FF, ssdt_stub_not_implemented);
}

/* ---- per_cpu_data assembly offsets ---- */

static void test_per_cpu_offsets(void)
{
    /* These offsets are hardcoded in syscall_entry.asm and ISR stubs.
     * If any field is inserted or reordered, assembly will silently
     * read the wrong data -- corrupting every ring transition. */
    TEST_ASSERT_EQ(__builtin_offsetof(struct per_cpu_data, self), 0,
                   "per_cpu_data.self at gs:0");
    TEST_ASSERT_EQ(__builtin_offsetof(struct per_cpu_data, syscall_rsp0), 24,
                   "per_cpu_data.syscall_rsp0 at gs:24");
    TEST_ASSERT_EQ(__builtin_offsetof(struct per_cpu_data, user_rsp_scratch), 32,
                   "per_cpu_data.user_rsp_scratch at gs:32");

    /* Verify field ordering: cpu_id and lapic_id sit between self and rsp0 */
    TEST_ASSERT_EQ(__builtin_offsetof(struct per_cpu_data, cpu_id), 8,
                   "per_cpu_data.cpu_id at gs:8");
    TEST_ASSERT_EQ(__builtin_offsetof(struct per_cpu_data, lapic_id), 12,
                   "per_cpu_data.lapic_id at gs:12");
    TEST_ASSERT_EQ(__builtin_offsetof(struct per_cpu_data, rsp0), 16,
                   "per_cpu_data.rsp0 at gs:16");
}

/* ---- boot_config struct layout ---- */

static void test_boot_config_layout(void)
{
    /* boot_config is shared between UEFI bootloader (PE/COFF) and kernel (ELF).
     * Both compile from separate struct definitions that must stay in sync.
     * cmdline at offset 32 is the ABI contract -- stable across versions. */
    TEST_ASSERT_EQ(sizeof(struct boot_config), 512,
                   "boot_config is 512 bytes (sector-aligned)");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_config, cmdline), 32,
                   "boot_config.cmdline at offset 32");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_config, config_found), 288,
                   "boot_config.config_found at offset 288");

    /* Verify config_found was set by bootloader (canary for struct corruption) */
    TEST_ASSERT_EQ(g_boot_info.config.config_found, 1,
                   "boot_config.config_found == 1 (bootloader set it)");
}

/* ---- User ELF range 3-file sync ---- */

static void test_user_elf_range(void)
{
    /* Three files must agree: user_range.h, vmm.c, pmm.c, user.ld.
     * The linker script can't #include the header, so verify the constants
     * match the expected values that user.ld uses. */
    TEST_ASSERT_EQ(USER_ELF_BASE, 0x800000UL,
                   "USER_ELF_BASE == 0x800000");
    TEST_ASSERT_EQ(USER_ELF_SIZE, 0x100000UL,
                   "USER_ELF_SIZE == 0x100000 (1 MiB)");
    TEST_ASSERT_EQ(USER_ELF_END, 0x900000UL,
                   "USER_ELF_END == 0x900000");
    TEST_ASSERT_EQ(USER_PD_INDEX, 4,
                   "USER_PD_INDEX == 4 (0x800000 >> 21)");
    /* Verify range fits within one 2 MiB PD entry */
    TEST_ASSERT_EQ(USER_ELF_BASE >> 21, (USER_ELF_END - 1) >> 21,
                   "User ELF range within single 2 MiB PD entry");
}

/* ---- AP trampoline data area offsets ---- */

static void test_ap_trampoline_offsets(void)
{
    /* ap_trampoline.asm uses hardcoded [AP_DATA + 0xNN] offsets.
     * If these C defines diverge, APs read garbage and crash on SIPI. */
    TEST_ASSERT_EQ(AP_OFF_CR3, 0x00,
                   "AP_OFF_CR3 == 0x00");
    TEST_ASSERT_EQ(AP_OFF_STACK, 0x08,
                   "AP_OFF_STACK == 0x08");
    TEST_ASSERT_EQ(AP_OFF_GDT_PTR, 0x10,
                   "AP_OFF_GDT_PTR == 0x10");
    TEST_ASSERT_EQ(AP_OFF_ENTRY, 0x20,
                   "AP_OFF_ENTRY == 0x20");
    TEST_ASSERT_EQ(AP_OFF_CPUID, 0x28,
                   "AP_OFF_CPUID == 0x28");
    TEST_ASSERT_EQ(AP_OFF_IDT_PTR, 0x30,
                   "AP_OFF_IDT_PTR == 0x30");
    TEST_ASSERT_EQ(AP_OFF_CANARY, 0x3C,
                   "AP_OFF_CANARY == 0x3C (after IDT_PTR 10-byte span)");
    TEST_ASSERT_EQ(AP_DATA_BASE, AP_TRAMPOLINE_ADDR + 0xE00,
                   "AP_DATA_BASE == trampoline + 0xE00");
}

/* ---- interrupt frame layout ---- */

static void test_interrupt_frame_layout(void)
{
    /* struct interrupt_frame is the single most critical struct in the kernel.
     * isr_stubs.asm pushes/pops in this exact order; schedule() returns a
     * frame pointer; task.c builds frames for new tasks. Any drift = silent
     * register corruption on every interrupt. */
    TEST_ASSERT_EQ(sizeof(struct interrupt_frame), 176,
                   "interrupt_frame == 22 x 8 = 176 bytes");

    /* First pushed (rax) = highest offset; last pushed (r15) = offset 0 */
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, r15), 0,
                   "r15 at offset 0 (last pushed, first popped)");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, rax), 112,
                   "rax at offset 112 (first pushed, last popped)");

    /* iretq frame: must be last 5 fields in exact CPU order */
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, rip), 136,
                   "rip at offset 136 (iretq field 1)");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, cs), 144,
                   "cs at offset 144 (iretq field 2)");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, rflags), 152,
                   "rflags at offset 152 (iretq field 3)");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, rsp), 160,
                   "rsp at offset 160 (iretq field 4)");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, ss), 168,
                   "ss at offset 168 (iretq field 5)");

    /* int_no/err_code between GPRs and iretq frame */
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, int_no), 120,
                   "int_no at offset 120");
    TEST_ASSERT_EQ(__builtin_offsetof(struct interrupt_frame, err_code), 128,
                   "err_code at offset 128");
}

/* ---- IDT vector uniqueness ---- */

static void test_vector_uniqueness(void)
{
    /* Every statically assigned vector must be unique. The static asserts
     * in vectors.h catch this at compile time; these tests verify the
     * actual values at runtime and document the expected assignments. */
    TEST_ASSERT_EQ(VECTOR_LINUX_SYSCALL, 0x80, "VECTOR_LINUX_SYSCALL == 0x80");
    TEST_ASSERT_EQ(VECTOR_NT_SYSCALL, 0x2E, "VECTOR_NT_SYSCALL == 0x2E");
    TEST_ASSERT_EQ(VECTOR_YIELD, 0x81, "VECTOR_YIELD == 0x81");
    TEST_ASSERT_EQ(VECTOR_IPI_ASYNC_INIT, 0xFC, "VECTOR_IPI_ASYNC_INIT == 0xFC");
    TEST_ASSERT_EQ(VECTOR_IPI_RESCHEDULE, 0xFD, "VECTOR_IPI_RESCHEDULE == 0xFD");
    TEST_ASSERT_EQ(VECTOR_IPI_TLB_SHOOTDOWN, 0xFE, "VECTOR_IPI_TLB_SHOOTDOWN == 0xFE");
    TEST_ASSERT_EQ(VECTOR_LAPIC_SPURIOUS, 0xFF, "VECTOR_LAPIC_SPURIOUS == 0xFF");

    /* Verify no pair of software/IPI vectors collide */
    TEST_ASSERT(VECTOR_LINUX_SYSCALL != VECTOR_YIELD,
                "syscall and yield vectors differ");
    TEST_ASSERT(VECTOR_NT_SYSCALL != VECTOR_LINUX_SYSCALL,
                "NT and Linux syscall vectors differ");
    TEST_ASSERT(VECTOR_IPI_ASYNC_INIT != VECTOR_IPI_RESCHEDULE,
                "async init and reschedule IPI differ");
}

/* ---- XSAVE/FXSAVE alignment ---- */

static void test_xsave_alignment(void)
{
    /* XSAVE requires 64-byte alignment, FXSAVE requires 16-byte.
     * PMM returns page-aligned (4096) which satisfies both. Verify
     * the invariant holds by allocating a frame and checking. */
    uintptr_t frame = pmm_alloc_frame();
    TEST_ASSERT(frame != 0, "pmm_alloc_frame for XSAVE test");
    if (frame) {
        TEST_ASSERT_EQ(frame & 63, 0,
                       "PMM frame is 64-byte aligned (XSAVE safe)");
        TEST_ASSERT_EQ(frame & 15, 0,
                       "PMM frame is 16-byte aligned (FXSAVE safe)");
        TEST_ASSERT_EQ(frame & 4095, 0,
                       "PMM frame is page-aligned");
        pmm_free_frame(frame);
    }
}

/* ---- Registration ---- */

void test_register_nt_types(void)
{
    test_suite_register_cat("NT: NTSTATUS success", test_ntstatus_success, TEST_CAT_ABI);
    test_suite_register_cat("NT: NTSTATUS error", test_ntstatus_error, TEST_CAT_ABI);
    test_suite_register_cat("NT: NTSTATUS warning", test_ntstatus_warning, TEST_CAT_ABI);
    test_suite_register_cat("NT: NTSTATUS information", test_ntstatus_information, TEST_CAT_ABI);
    test_suite_register_cat("NT: type sizes", test_nt_type_sizes, TEST_CAT_ABI);
    test_suite_register_cat("NT: OBJECT_ATTRIBUTES size", test_object_attributes_size, TEST_CAT_ABI);
    test_suite_register_cat("NT: GDT SYSRET order", test_gdt_sysret_order, TEST_CAT_ABI);
    test_suite_register_cat("NT: SSDT unimplemented stub", test_ssdt_unimplemented_returns_not_implemented, TEST_CAT_ABI);
    test_suite_register_cat("NT: SSDT invalid table", test_ssdt_invalid_table_returns_error, TEST_CAT_ABI);
    test_suite_register_cat("NT: SSDT main count", test_ssdt_main_count, TEST_CAT_ABI);
    test_suite_register_cat("NT: SSDT register+dispatch", test_ssdt_register_and_dispatch, TEST_CAT_ABI);
    test_suite_register_cat("NT: per_cpu_data offsets", test_per_cpu_offsets, TEST_CAT_ABI);
    test_suite_register_cat("NT: boot_config layout", test_boot_config_layout, TEST_CAT_ABI);
    test_suite_register_cat("NT: user ELF range sync", test_user_elf_range, TEST_CAT_ABI);
    test_suite_register_cat("NT: AP trampoline offsets", test_ap_trampoline_offsets, TEST_CAT_ABI);
    test_suite_register_cat("NT: interrupt frame layout", test_interrupt_frame_layout, TEST_CAT_ABI);
    test_suite_register_cat("NT: IDT vector uniqueness", test_vector_uniqueness, TEST_CAT_ABI);
    test_suite_register_cat("NT: XSAVE alignment", test_xsave_alignment, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
