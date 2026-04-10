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
#include "kernel/nt/filetime.h"
#include "kernel/drivers/rtc.h"
#include "kernel/uefi_runtime.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/vectors.h"
#include "kernel/smp.h"
#include "kernel/boot_info.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/user_range.h"
#include "kernel/sched/syscall.h"
#include "kernel/nt/nt_file.h"
#include "kernel/nt/nt_process.h"
#include "kernel/nt/nt_sync.h"
#include "kernel/nt/nt_memory.h"
#include "kernel/nt/zw.h"

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
            TEST_ASSERT(t->count >= SSDT_MAIN_COUNT,
                        "table->count >= SSDT_MAIN_COUNT (grows as handlers register)");
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

/* ---- swapgs symmetry (GS self-pointer valid in kernel context) ---- */

static void test_gs_self_pointer(void)
{
    /* If swapgs symmetry is broken, gs:0 reads TEB or garbage instead
     * of the per_cpu_data self-pointer. This test runs in kernel context
     * (ring 0) where GS should always point to per-CPU data.
     * The fact that we got here means hundreds of interrupts already
     * verified swapgs symmetry, but test it explicitly. */
    struct per_cpu_data *gs_self;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(gs_self));
    TEST_ASSERT(gs_self != (void *)0,
                "gs:0 is non-NULL (GS_BASE set)");
    TEST_ASSERT(gs_self->self == gs_self,
                "gs:0 self-pointer matches (swapgs symmetry intact)");
    TEST_ASSERT(gs_self->cpu_id < 64,
                "gs:0->cpu_id is reasonable (< 64)");
}

/* ---- FILETIME epoch and conversion tests ---- */

static void test_filetime_epoch(void)
{
    /* Unix epoch (1970-01-01T00:00:00Z) in FILETIME must equal the offset constant */
    FILETIME ft = filetime_from_unix_seconds(0);
    TEST_ASSERT_EQ(ft, FILETIME_EPOCH_OFFSET_100NS,
                   "filetime_from_unix_seconds(0) == FILETIME_EPOCH_OFFSET_100NS");
}

static void test_filetime_roundtrip(void)
{
    /* Round-trip: Unix seconds -> FILETIME -> Unix seconds */
    uint64_t unix_sec = 1743811200ULL;  /* 2025-04-05T00:00:00Z */
    FILETIME ft = filetime_from_unix_seconds(unix_sec);
    uint64_t back = filetime_to_unix_seconds(ft);
    TEST_ASSERT_EQ(back, unix_sec, "FILETIME round-trip preserves Unix seconds");
}

static void test_filetime_dos_roundtrip(void)
{
    /* FAT32 DOS date/time round-trip (2-second resolution, no sub-second) */
    FILETIME ft = filetime_from_unix_seconds(1743811200ULL);  /* 2025-04-05 */
    uint16_t date, time;
    filetime_to_dos_datetime(ft, 0, &date, &time);  /* UTC, no bias */

    /* Year=2025-1980=45, month=4, day=5 */
    TEST_ASSERT_EQ((date >> 9) & 0x7F, 45, "DOS date year = 45 (2025-1980)");
    TEST_ASSERT_EQ((date >> 5) & 0x0F, 4, "DOS date month = 4");
    TEST_ASSERT_EQ(date & 0x1F, 5, "DOS date day = 5");
}

static void test_filetime_ticks_per_second(void)
{
    TEST_ASSERT_EQ(FILETIME_TICKS_PER_SECOND, 10000000ULL,
                   "FILETIME_TICKS_PER_SECOND == 10,000,000");
    TEST_ASSERT_EQ(FILETIME_TICKS_PER_MS, 10000ULL,
                   "FILETIME_TICKS_PER_MS == 10,000");
}

/* ---- NT syscall migration SSDT registration verification ---- */

static void test_nt_syscall_ssdt_registered(void)
{
    /* Verify that the 12 NtXxx migration handlers are registered (not stubs).
     * After nt_syscall_register_ssdt(), each slot should dispatch to a real
     * handler, not ssdt_stub_not_implemented. We test by checking dispatch
     * doesn't return STATUS_NOT_IMPLEMENTED. */
    NTSTATUS s;

    /* NtYieldExecution (0x0044) -- no args, always STATUS_SUCCESS */
    s = ssdt_dispatch(SSDT_NtYieldExecution, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "NtYieldExecution SSDT registered and callable");

    /* NtShutdownSystem with invalid action -- should return INVALID_PARAMETER,
     * NOT STATUS_NOT_IMPLEMENTED (proves handler is registered) */
    s = ssdt_dispatch(SSDT_NtShutdownSystem, 99, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtShutdownSystem SSDT registered (invalid action)");

    /* NtWriteFile with NULL buffer -- should return STATUS_SUCCESS (0 bytes) */
    s = ssdt_dispatch(SSDT_NtWriteFile, STDOUT_FD, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "NtWriteFile SSDT registered (NULL buf = 0 bytes)");

    /* NtReadFile with NULL buffer -- should return STATUS_SUCCESS (0 bytes) */
    s = ssdt_dispatch(SSDT_NtReadFile, STDIN_FD, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "NtReadFile SSDT registered (NULL buf = 0 bytes)");

    /* NtCreateNamedPipeFile with NULL handles -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateNamedPipeFile, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateNamedPipeFile SSDT registered (NULL handles)");

    /* NtCreateSection with NULL out handle -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateSection, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateSection SSDT registered (NULL out)");

    /* NtMapViewOfSection with NULL base_out -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtMapViewOfSection, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtMapViewOfSection SSDT registered (NULL base)");

    /* NtQuerySystemInformation with invalid class -- NOT_IMPLEMENTED (§10) */
    s = ssdt_dispatch(SSDT_NtQuerySystemInformation, 0xFF, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_NOT_IMPLEMENTED,
                   "NtQuerySystemInformation SSDT registered (bad class)");

    /* NtQueryDirectoryFile with NULL buffer -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtQueryDirectoryFile, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtQueryDirectoryFile SSDT registered (NULL buf)");
}

static void test_nt_syscall_ssdt_alias_values(void)
{
    /* Verify SYS_NT_* aliases match SSDT_NtXxx constants */
    TEST_ASSERT_EQ(SYS_NT_WRITE,        SSDT_NtWriteFile,
                   "SYS_NT_WRITE == SSDT_NtWriteFile");
    TEST_ASSERT_EQ(SYS_NT_READ,         SSDT_NtReadFile,
                   "SYS_NT_READ == SSDT_NtReadFile");
    TEST_ASSERT_EQ(SYS_NT_EXIT,         SSDT_NtTerminateProcess,
                   "SYS_NT_EXIT == SSDT_NtTerminateProcess");
    TEST_ASSERT_EQ(SYS_NT_YIELD,        SSDT_NtYieldExecution,
                   "SYS_NT_YIELD == SSDT_NtYieldExecution");
    TEST_ASSERT_EQ(SYS_NT_WAITPID,      SSDT_NtWaitForSingleObject,
                   "SYS_NT_WAITPID == SSDT_NtWaitForSingleObject");
    TEST_ASSERT_EQ(SYS_NT_READDIR,      SSDT_NtQueryDirectoryFile,
                   "SYS_NT_READDIR == SSDT_NtQueryDirectoryFile");
    TEST_ASSERT_EQ(SYS_NT_GETPROCS,     SSDT_NtQuerySystemInformation,
                   "SYS_NT_GETPROCS == SSDT_NtQuerySystemInformation");
    TEST_ASSERT_EQ(SYS_NT_SHUTDOWN,     SSDT_NtShutdownSystem,
                   "SYS_NT_SHUTDOWN == SSDT_NtShutdownSystem");
    TEST_ASSERT_EQ(SYS_NT_PIPE,         SSDT_NtCreateNamedPipeFile,
                   "SYS_NT_PIPE == SSDT_NtCreateNamedPipeFile");
    TEST_ASSERT_EQ(SYS_NT_SHMEM_CREATE, SSDT_NtCreateSection,
                   "SYS_NT_SHMEM_CREATE == SSDT_NtCreateSection");
    TEST_ASSERT_EQ(SYS_NT_SHMEM_MAP,    SSDT_NtMapViewOfSection,
                   "SYS_NT_SHMEM_MAP == SSDT_NtMapViewOfSection");
    TEST_ASSERT_EQ(SYS_NT_CLOSE,        SSDT_NtClose,
                   "SYS_NT_CLOSE == SSDT_NtClose");
}

/* ---- NT file I/O (§6) -- NtCreateFile / NtOpenFile SSDT tests ---- */

static void test_nt_create_file_ssdt_registered(void)
{
    NTSTATUS s;

    /* NtCreateFile (0x0010) with NULL out_handle -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateFile, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateFile SSDT registered (NULL out_handle)");

    /* NtOpenFile (0x0011) with NULL out_handle -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtOpenFile, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtOpenFile SSDT registered (NULL out_handle)");
}

static void test_nt_file_constants(void)
{
    /* CreateDisposition values match Windows NT convention */
    TEST_ASSERT_EQ(FILE_SUPERSEDE,    0, "FILE_SUPERSEDE == 0");
    TEST_ASSERT_EQ(FILE_OPEN,         1, "FILE_OPEN == 1");
    TEST_ASSERT_EQ(FILE_CREATE,       2, "FILE_CREATE == 2");
    TEST_ASSERT_EQ(FILE_OPEN_IF,      3, "FILE_OPEN_IF == 3");
    TEST_ASSERT_EQ(FILE_OVERWRITE,    4, "FILE_OVERWRITE == 4");
    TEST_ASSERT_EQ(FILE_OVERWRITE_IF, 5, "FILE_OVERWRITE_IF == 5");

    /* IoStatusBlock Information values */
    TEST_ASSERT_EQ(FILE_SUPERSEDED,     0, "FILE_SUPERSEDED == 0");
    TEST_ASSERT_EQ(FILE_OPENED,         1, "FILE_OPENED == 1");
    TEST_ASSERT_EQ(FILE_CREATED,        2, "FILE_CREATED == 2");
    TEST_ASSERT_EQ(FILE_OVERWRITTEN,    3, "FILE_OVERWRITTEN == 3");

    /* SSDT indices for NtCreateFile/NtOpenFile */
    TEST_ASSERT_EQ(SSDT_NtCreateFile, 0x0010, "SSDT_NtCreateFile == 0x0010");
    TEST_ASSERT_EQ(SSDT_NtOpenFile,   0x0011, "SSDT_NtOpenFile == 0x0011");
}

/* ---- NT process/thread lifecycle (§7) tests ---- */

static void test_nt_process_ssdt_registered(void)
{
    NTSTATUS s;

    /* NtCreateProcess (0x0030) with NULL out -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateProcess, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateProcess SSDT registered (NULL out)");

    /* NtOpenProcess (0x0032) with NULL args -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtOpenProcess, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtOpenProcess SSDT registered (NULL args)");

    /* NtCreateThread (0x0036) with NULL out -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateThread, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateThread SSDT registered (NULL out)");

    /* NtDelayExecution (0x0047) with NULL interval -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtDelayExecution, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtDelayExecution SSDT registered (NULL interval)");

    /* NtGetContextThread (0x003C) -- stub returns NOT_IMPLEMENTED */
    s = ssdt_dispatch(SSDT_NtGetContextThread, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_NOT_IMPLEMENTED,
                   "NtGetContextThread stub (needs TODO-10)");

    /* NtQueueApcThread (0x0043) -- stub returns NOT_IMPLEMENTED */
    s = ssdt_dispatch(SSDT_NtQueueApcThread, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_NOT_IMPLEMENTED,
                   "NtQueueApcThread stub (needs TODO-06)");

    /* NtTerminateProcess (0x0033) with PID 0 / CURRENT_PROCESS
     * can't test without killing ourselves, so test with invalid handle */
    s = ssdt_dispatch(SSDT_NtTerminateProcess, 999, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE,
                   "NtTerminateProcess SSDT registered (bad handle)");
}

static void test_nt_thread_info_classes(void)
{
    /* Verify info class constants match Windows NT convention */
    TEST_ASSERT_EQ(ThreadBasicInformation, 0, "ThreadBasicInformation == 0");
    TEST_ASSERT_EQ(ThreadTimes,            1, "ThreadTimes == 1");
    TEST_ASSERT_EQ(ThreadPriority,         2, "ThreadPriority == 2");
    TEST_ASSERT_EQ(ThreadBasePriority,     3, "ThreadBasePriority == 3");
    TEST_ASSERT_EQ(ThreadAffinityMask,     4, "ThreadAffinityMask == 4");
    TEST_ASSERT_EQ(ProcessBasicInformation, 0, "ProcessBasicInformation == 0");
    TEST_ASSERT_EQ(ProcessPriorityClass,   18, "ProcessPriorityClass == 18");

    /* NtQueryInformationThread with invalid class -- INVALID_INFO_CLASS */
    {
        NTSTATUS s = ssdt_dispatch(SSDT_NtQueryInformationThread,
                                   CURRENT_THREAD, 0xFF, 0, 0, 0, 0);
        /* NULL buffer returns INVALID_PARAMETER before class check */
        TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                    "NtQueryInformationThread is registered");
    }
}

/* ---- NT sync objects (§8) tests ---- */

static void test_nt_sync_ssdt_registered(void)
{
    NTSTATUS s;

    /* NtCreateEvent (0x0070) with NULL out -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateEvent, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateEvent SSDT registered (NULL out)");

    /* NtCreateMutant (0x0076) with NULL out -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateMutant, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateMutant SSDT registered (NULL out)");

    /* NtCreateSemaphore (0x007A) with NULL out -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtCreateSemaphore, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtCreateSemaphore SSDT registered (NULL out)");

    /* NtWaitForMultipleObjects (0x0007) with NULL handles -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtWaitForMultipleObjects, 1, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtWaitForMultipleObjects SSDT registered (NULL handles)");

    /* NtSetEvent (0x0072) with invalid handle -- INVALID_HANDLE */
    s = ssdt_dispatch(SSDT_NtSetEvent, 999, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE,
                   "NtSetEvent SSDT registered (bad handle)");

    /* Keyed event stub (0x0084) -- NOT_IMPLEMENTED */
    s = ssdt_dispatch(SSDT_NtCreateKeyedEvent, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_NOT_IMPLEMENTED,
                   "NtCreateKeyedEvent stub (deferred TODO-07)");
}

static void test_nt_sync_constants(void)
{
    TEST_ASSERT_EQ(NotificationEvent,     0, "NotificationEvent == 0");
    TEST_ASSERT_EQ(SynchronizationEvent,  1, "SynchronizationEvent == 1");
    TEST_ASSERT_EQ(WaitAll,               0, "WaitAll == 0");
    TEST_ASSERT_EQ(WaitAny,               1, "WaitAny == 1");
    TEST_ASSERT_EQ(MAXIMUM_WAIT_OBJECTS, 64, "MAXIMUM_WAIT_OBJECTS == 64");
    TEST_ASSERT_EQ(STATUS_WAIT_0,         0, "STATUS_WAIT_0 == 0");
    TEST_ASSERT_EQ(STATUS_ABANDONED,   0x80, "STATUS_ABANDONED == 0x80");
}

/* ---- NT virtual memory (§9) tests ---- */

static void test_nt_vm_ssdt_registered(void)
{
    NTSTATUS s;

    /* NtAllocateVirtualMemory (0x0050) with NULL args -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtAllocateVirtualMemory, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtAllocateVirtualMemory SSDT registered (NULL args)");

    /* NtQueryVirtualMemory (0x0053) with bad class -- INVALID_INFO_CLASS */
    s = ssdt_dispatch(SSDT_NtQueryVirtualMemory, 0, 0, 0xFF, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_INFO_CLASS,
                   "NtQueryVirtualMemory SSDT registered (bad class)");

    /* NtReadVirtualMemory (0x0057) with NULL buffer -- INVALID_PARAMETER */
    s = ssdt_dispatch(SSDT_NtReadVirtualMemory, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER,
                   "NtReadVirtualMemory SSDT registered (NULL buf)");

    /* AWE stub (0x0059) -- NOT_IMPLEMENTED */
    s = ssdt_dispatch(SSDT_NtAllocateUserPhysicalPages, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_NOT_IMPLEMENTED,
                   "NtAllocateUserPhysicalPages AWE stub");

    /* NtLockVirtualMemory (0x0054) -- always SUCCESS (no swap) */
    s = ssdt_dispatch(SSDT_NtLockVirtualMemory, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "NtLockVirtualMemory SSDT registered (no-op)");
}

static void test_nt_vm_constants(void)
{
    TEST_ASSERT_EQ(MEM_COMMIT,    0x1000, "MEM_COMMIT == 0x1000");
    TEST_ASSERT_EQ(MEM_RESERVE,   0x2000, "MEM_RESERVE == 0x2000");
    TEST_ASSERT_EQ(MEM_RELEASE,   0x8000, "MEM_RELEASE == 0x8000");
    TEST_ASSERT_EQ(MEM_DECOMMIT,  0x4000, "MEM_DECOMMIT == 0x4000");
    TEST_ASSERT_EQ(PAGE_NOACCESS,       0x01, "PAGE_NOACCESS == 0x01");
    TEST_ASSERT_EQ(PAGE_READONLY,       0x02, "PAGE_READONLY == 0x02");
    TEST_ASSERT_EQ(PAGE_READWRITE,      0x04, "PAGE_READWRITE == 0x04");
    TEST_ASSERT_EQ(PAGE_EXECUTE,        0x10, "PAGE_EXECUTE == 0x10");
    TEST_ASSERT_EQ(PAGE_EXECUTE_READ,   0x20, "PAGE_EXECUTE_READ == 0x20");
    TEST_ASSERT_EQ(PAGE_EXECUTE_READWRITE, 0x40, "PAGE_EXECUTE_READWRITE == 0x40");
    TEST_ASSERT_EQ(PAGE_GUARD,          0x100, "PAGE_GUARD == 0x100");

    /* MEMORY_BASIC_INFORMATION size */
    TEST_ASSERT_EQ(sizeof(MEMORY_BASIC_INFORMATION), 48,
                   "MEMORY_BASIC_INFORMATION size == 48");
}

/* ---- NT system/process information (§10) tests ---- */

static void test_nt_sysinfo_classes(void)
{
    NTSTATUS s;
    uint8_t buf[128];
    uint32_t ret_len = 0;

    /* SystemBasicInformation: should return page size and CPU count */
    s = ssdt_dispatch(SSDT_NtQuerySystemInformation,
                      0 /* SystemBasicInformation */,
                      (uint64_t)buf, 128, (uint64_t)&ret_len, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "SystemBasicInformation returns SUCCESS");
    /* Page size at offset 8 */
    {
        uint32_t *page_size = (uint32_t *)(buf + 8);
        TEST_ASSERT_EQ(*page_size, 4096,
                       "SystemBasicInformation.PageSize == 4096");
    }

    /* SystemPerformanceInformation */
    s = ssdt_dispatch(SSDT_NtQuerySystemInformation,
                      2 /* SystemPerformanceInformation */,
                      (uint64_t)buf, 128, (uint64_t)&ret_len, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "SystemPerformanceInformation returns SUCCESS");

    /* SystemProcessorInformation */
    s = ssdt_dispatch(SSDT_NtQuerySystemInformation,
                      1 /* SystemProcessorInformation */,
                      (uint64_t)buf, 128, (uint64_t)&ret_len, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "SystemProcessorInformation returns SUCCESS");

    /* NtSetSystemInformation -- stub returns NOT_IMPLEMENTED */
    s = ssdt_dispatch(SSDT_NtSetSystemInformation, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_NOT_IMPLEMENTED,
                   "NtSetSystemInformation stub");

    /* Unknown class returns NOT_IMPLEMENTED */
    s = ssdt_dispatch(SSDT_NtQuerySystemInformation,
                      999, (uint64_t)buf, 128, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_NOT_IMPLEMENTED,
                   "Unknown SystemInformationClass returns NOT_IMPLEMENTED");
}

static void test_nt_procinfo_classes(void)
{
    NTSTATUS s;
    uint8_t buf[128];
    uint32_t ret_len = 0;

    /* ProcessHandleCount for current process */
    s = ssdt_dispatch(SSDT_NtQueryInformationProcess,
                      CURRENT_PROCESS, ProcessHandleCount,
                      (uint64_t)buf, 4, (uint64_t)&ret_len, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "ProcessHandleCount returns SUCCESS");

    /* ProcessDebugPort -- should be 0 (not debugged) */
    s = ssdt_dispatch(SSDT_NtQueryInformationProcess,
                      CURRENT_PROCESS, ProcessDebugPort,
                      (uint64_t)buf, 8, (uint64_t)&ret_len, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "ProcessDebugPort returns SUCCESS");
    {
        uint64_t *port = (uint64_t *)buf;
        TEST_ASSERT_EQ(*port, 0,
                       "ProcessDebugPort == 0 (not debugged)");
    }

    /* ProcessWow64Information -- 0 for native 64-bit */
    s = ssdt_dispatch(SSDT_NtQueryInformationProcess,
                      CURRENT_PROCESS, ProcessWow64Information,
                      (uint64_t)buf, 8, (uint64_t)&ret_len, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "ProcessWow64Information returns SUCCESS");

    /* ProcessSessionInformation -- session 0 */
    s = ssdt_dispatch(SSDT_NtQueryInformationProcess,
                      CURRENT_PROCESS, ProcessSessionInformation,
                      (uint64_t)buf, 4, (uint64_t)&ret_len, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "ProcessSessionInformation returns SUCCESS");
}

/* ---- S12: ZwXxx alias layer ---- */

/* ZwClose dispatches to NtClose handler in kernel mode (previous mode = 0) */
static void test_zw_close_kernel_dispatch(void)
{
    /* Previous mode should be KernelMode when called from kernel test */
    TEST_ASSERT_EQ(ssdt_previous_mode(), SSDT_KERNEL_MODE,
                   "previous mode is KernelMode in test context");
    /* ZwClose with an invalid handle should return STATUS_INVALID_HANDLE,
     * proving it reached the NtClose handler (not STATUS_NOT_IMPLEMENTED). */
    NTSTATUS s = ZwClose(0xDEAD);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "ZwClose dispatches to registered handler");
}

/* ProbeForRead: valid user-range pointer should succeed */
static void test_probe_for_read_user_range(void)
{
    NTSTATUS s = ProbeForRead((const void *)0x800000, 0x100, 1);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "ProbeForRead accepts user-range pointer");
    /* Zero length always succeeds */
    s = ProbeForRead((const void *)0x800000, 0, 1);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "ProbeForRead zero length succeeds");
}

/* ProbeForRead: kernel-space pointer should fail */
static void test_probe_for_read_kernel_ptr(void)
{
    /* Address above MM_USER_PROBE_ADDRESS */
    NTSTATUS s = ProbeForRead((const void *)0x80000000ULL, 0x10, 1);
    TEST_ASSERT_EQ(s, STATUS_ACCESS_VIOLATION,
                   "ProbeForRead rejects kernel pointer");
    /* NULL pointer */
    s = ProbeForRead((const void *)0, 0x10, 1);
    TEST_ASSERT_EQ(s, STATUS_ACCESS_VIOLATION,
                   "ProbeForRead rejects NULL");
    /* Overflow: address near max + large length */
    s = ProbeForRead((const void *)0x7FFE0000ULL, 0x100000, 1);
    TEST_ASSERT_EQ(s, STATUS_ACCESS_VIOLATION,
                   "ProbeForRead rejects overflow");
}

/* ProbeForRead: alignment check */
static void test_probe_for_read_alignment(void)
{
    /* Misaligned address with alignment=4 */
    NTSTATUS s = ProbeForRead((const void *)0x800001, 4, 4);
    TEST_ASSERT_EQ(s, STATUS_DATATYPE_MISALIGNMENT,
                   "ProbeForRead rejects misaligned pointer");
    /* Aligned address with alignment=4 */
    s = ProbeForRead((const void *)0x800004, 4, 4);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "ProbeForRead accepts aligned pointer");
}

/* ASSERT_KERNEL_CALLER: in kernel mode (test context) should not fire */
static void test_assert_kernel_caller(void)
{
    /* We can't directly test the macro returning a status since it
     * uses 'return'. Test the underlying condition instead. */
    TEST_ASSERT_EQ(ssdt_previous_mode(), SSDT_KERNEL_MODE,
                   "ASSERT_KERNEL_CALLER would pass (kernel mode)");
    /* Set to user mode, check, restore */
    ssdt_set_previous_mode(SSDT_USER_MODE);
    TEST_ASSERT_EQ(ssdt_previous_mode(), SSDT_USER_MODE,
                   "previous mode set to UserMode");
    ssdt_set_previous_mode(SSDT_KERNEL_MODE);
    TEST_ASSERT_EQ(ssdt_previous_mode(), SSDT_KERNEL_MODE,
                   "previous mode restored to KernelMode");
}

/* Default previous mode should be KernelMode (0) */
static void test_previous_mode_default(void)
{
    TEST_ASSERT_EQ(ssdt_previous_mode(), SSDT_KERNEL_MODE,
                   "default previous mode is KernelMode");
}

/* ZwClose called from within a UserMode context should still see KernelMode */
static void test_zw_nested_in_usermode(void)
{
    /* Simulate being inside a user syscall dispatch */
    ssdt_set_previous_mode(SSDT_USER_MODE);
    /* ZwClose uses zw_dispatch which saves/restores previous mode */
    NTSTATUS s = ZwClose(0xDEAD);
    /* After ZwClose returns, previous mode should be restored to UserMode */
    TEST_ASSERT_EQ(ssdt_previous_mode(), SSDT_USER_MODE,
                   "Zw restores previous mode after nested dispatch");
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "ZwClose reached handler even in nested UserMode context");
    /* Restore */
    ssdt_set_previous_mode(SSDT_KERNEL_MODE);
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
    test_suite_register_cat("NT: SSDT unimp stub",
        test_ssdt_unimplemented_returns_not_implemented, TEST_CAT_ABI);
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
    test_suite_register_cat("NT: GS self-pointer (swapgs)", test_gs_self_pointer, TEST_CAT_ABI);

    /* FILETIME tests */
    test_suite_register_cat("NT: FILETIME epoch", test_filetime_epoch, TEST_CAT_ABI);
    test_suite_register_cat("NT: FILETIME roundtrip", test_filetime_roundtrip, TEST_CAT_ABI);
    test_suite_register_cat("NT: FILETIME DOS roundtrip", test_filetime_dos_roundtrip, TEST_CAT_ABI);
    test_suite_register_cat("NT: FILETIME ticks/sec", test_filetime_ticks_per_second, TEST_CAT_ABI);

    /* NT syscall migration tests */
    test_suite_register_cat("NT: syscall SSDT registered", test_nt_syscall_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: SYS_NT_* alias values", test_nt_syscall_ssdt_alias_values, TEST_CAT_ABI);

    /* NT file I/O tests (§6) */
    test_suite_register_cat("NT: NtCreateFile/NtOpenFile SSDT", test_nt_create_file_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: file I/O constants", test_nt_file_constants, TEST_CAT_ABI);

    /* NT process/thread lifecycle tests (§7) */
    test_suite_register_cat("NT: process/thread SSDT registered", test_nt_process_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: thread info classes", test_nt_thread_info_classes, TEST_CAT_ABI);

    /* NT sync objects tests (§8) */
    test_suite_register_cat("NT: sync SSDT registered", test_nt_sync_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: sync constants", test_nt_sync_constants, TEST_CAT_ABI);

    /* NT virtual memory tests (§9) */
    test_suite_register_cat("NT: VM SSDT registered", test_nt_vm_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: VM constants", test_nt_vm_constants, TEST_CAT_ABI);

    /* NT system information tests (§10) */
    test_suite_register_cat("NT: NtQuerySystemInfo classes", test_nt_sysinfo_classes, TEST_CAT_ABI);
    test_suite_register_cat("NT: NtQueryProcessInfo classes", test_nt_procinfo_classes, TEST_CAT_ABI);

    /* ZwXxx alias layer tests (§12) */
    test_suite_register_cat("NT: ZwClose kernel dispatch",
                            test_zw_close_kernel_dispatch, TEST_CAT_ABI);
    test_suite_register_cat("NT: ProbeForRead user range",
                            test_probe_for_read_user_range, TEST_CAT_ABI);
    test_suite_register_cat("NT: ProbeForRead kernel ptr rejected",
                            test_probe_for_read_kernel_ptr, TEST_CAT_ABI);
    test_suite_register_cat("NT: ProbeForRead alignment",
                            test_probe_for_read_alignment, TEST_CAT_ABI);
    test_suite_register_cat("NT: ASSERT_KERNEL_CALLER macro",
                            test_assert_kernel_caller, TEST_CAT_ABI);
    test_suite_register_cat("NT: previous mode default kernel",
                            test_previous_mode_default, TEST_CAT_ABI);
    test_suite_register_cat("NT: Zw nested in UserMode restores",
                            test_zw_nested_in_usermode, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
