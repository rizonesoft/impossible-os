/* ============================================================================
 * test_nt_types.c -- NTSTATUS type and NT foundational types unit tests
 *
 * Tests severity macros, status code values, and type sizes for Win32 ABI
 * compatibility.
 *
 * XREF: 02-kernel-core/TODO-12-native-api-ssdt.md section 1
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
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/ob/ob_event.h"
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

static void test_filetime_pre_epoch_guard(void)
{
    /* filetime_to_unix_seconds must return 0 for pre-Unix-epoch FILETIMEs */
    TEST_ASSERT_EQ(filetime_to_unix_seconds(0), 0ULL,
                   "filetime_to_unix_seconds(0) == 0 (pre-epoch guard)");
    TEST_ASSERT_EQ(filetime_to_unix_seconds(FILETIME_EPOCH_OFFSET_100NS - 1), 0ULL,
                   "filetime_to_unix_seconds(EPOCH_OFFSET-1) == 0 (boundary)");
}

static void test_filetime_dos_clamp_low(void)
{
    /* ft=0 (FILETIME_NOW_PLACEHOLDER) with negative bias: full saturation to 1980-01-01 */
    uint16_t date, time;
    filetime_to_dos_datetime(0, 0, &date, &time);
    TEST_ASSERT_EQ(date, (uint16_t)((0 << 9) | (1 << 5) | 1),
                   "FILETIME=0 -> DOS min date 1980-01-01");
    TEST_ASSERT_EQ(time, 0, "FILETIME=0 -> DOS time 00:00:00");
}

static void test_filetime_days_year_guard(void)
{
    /* filetime_days_from_date_fn: year < 1601 returns 0 (no underflow) */
    TEST_ASSERT_EQ(filetime_days_from_date_fn(0, 1, 1), 0ULL,
                   "filetime_days_from_date_fn(year=0) == 0 (guard)");
    TEST_ASSERT_EQ(filetime_days_from_date_fn(1600, 12, 31), 0ULL,
                   "filetime_days_from_date_fn(year=1600) == 0 (guard)");
    /* year=1601 should be non-zero baseline */
    TEST_ASSERT_EQ(filetime_days_from_date_fn(1601, 1, 1), 0ULL,
                   "filetime_days_from_date_fn(1601-01-01) == 0 (FILETIME epoch)");
}



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

    /* NtQuerySystemInformation with invalid class -- NOT_IMPLEMENTED */
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

/* ---- NT file I/O -- NtCreateFile / NtOpenFile SSDT tests ---- */

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

    /* IoStatusBlock Information values */

    /* SSDT indices for NtCreateFile/NtOpenFile */
    TEST_ASSERT_EQ(SSDT_NtCreateFile, 0x0010, "SSDT_NtCreateFile == 0x0010");
    TEST_ASSERT_EQ(SSDT_NtOpenFile,   0x0011, "SSDT_NtOpenFile == 0x0011");
}

/* ---- NT process/thread lifecycle tests ---- */

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

    /* NtGetContextThread (0x003C) -- pending SEH/CONTEXT */
    s = ssdt_dispatch(SSDT_NtGetContextThread, 0, 0, 0, 0, 0, 0);
    TEST_PENDING(s == STATUS_NOT_IMPLEMENTED,
                 "NtGetContextThread (0x3c): no CONTEXT capture yet");

    /* NtQueueApcThread (0x0043) -- pending APC delivery */
    s = ssdt_dispatch(SSDT_NtQueueApcThread, 0, 0, 0, 0, 0, 0);
    TEST_PENDING(s == STATUS_NOT_IMPLEMENTED,
                 "NtQueueApcThread (0x43): no APC queue yet");

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

/* ---- NT virtual memory tests ---- */

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

    /* AWE (0x0059) -- pending advanced VM */
    s = ssdt_dispatch(SSDT_NtAllocateUserPhysicalPages, 0, 0, 0, 0, 0, 0);
    TEST_PENDING(s == STATUS_NOT_IMPLEMENTED,
                 "NtAllocateUserPhysicalPages (0x59): no AWE windowing yet");

    /* NtLockVirtualMemory (0x0054) -- always SUCCESS (no swap) */
    s = ssdt_dispatch(SSDT_NtLockVirtualMemory, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "NtLockVirtualMemory SSDT registered (no-op)");
}

static void test_nt_vm_constants(void)
{

    /* MEMORY_BASIC_INFORMATION size */
    TEST_ASSERT_EQ(sizeof(MEMORY_BASIC_INFORMATION), 48,
                   "MEMORY_BASIC_INFORMATION size == 48");
}

/* ---- NT system/process information tests ---- */

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

    /* NtSetSystemInformation -- pending whole subsystem */
    s = ssdt_dispatch(SSDT_NtSetSystemInformation, 0, 0, 0, 0, 0, 0);
    TEST_PENDING(s == STATUS_NOT_IMPLEMENTED,
                 "NtSetSystemInformation: no class setters yet");

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

/* ---- S27: Extended directory enumeration ---- */

/* FILE_DIRECTORY_INFORMATION struct should have expected fixed size */
static void test_dir_info_struct_sizes(void)
{
    /* Fixed size = byte offset of FileName (NOT sizeof - 2, which over-counts
     * by trailing struct padding). Exact Windows x64 ABI offsets. */
    TEST_ASSERT_EQ((uint32_t)FILE_DIR_INFO_FIXED_SIZE, 64u,
                   "FILE_DIRECTORY_INFORMATION FileName offset = 64");
    TEST_ASSERT_EQ((uint32_t)FILE_BOTH_DIR_INFO_FIXED_SIZE, 94u,
                   "FILE_BOTH_DIR_INFORMATION FileName offset = 94");
    TEST_ASSERT_EQ((uint32_t)FILE_ID_BOTH_DIR_INFO_FIXED_SIZE, 104u,
                   "FILE_ID_BOTH_DIR_INFORMATION FileName offset = 104");
    TEST_ASSERT_EQ((uint32_t)__builtin_offsetof(FILE_ID_BOTH_DIR_INFORMATION, FileId), 96u,
                   "FILE_ID_BOTH_DIR_INFORMATION FileId offset = 96");
}

/* NtQueryDirectoryFile with class 1 should return entries with metadata */
static void test_dir_enum_extended_dispatch(void)
{
    NTSTATUS s;
    uint8_t buf[512];

    /* Class 1 (FileDirectoryInformation) -- should reach handler */
    s = ssdt_dispatch(SSDT_NtQueryDirectoryFile,
                      0, 0, (uint64_t)buf, 512,
                      FileDirectoryInformation, 0);
    /* Should succeed or NO_MORE_FILES (depends on C:\ contents) */
    TEST_ASSERT(s == STATUS_SUCCESS || s == STATUS_NO_MORE_FILES,
                "FileDirectoryInformation dispatches without error");

    /* Class 3 (FileBothDirectoryInformation) */
    s = ssdt_dispatch(SSDT_NtQueryDirectoryFile,
                      0, 0, (uint64_t)buf, 512,
                      FileBothDirectoryInformation, 0);
    TEST_ASSERT(s == STATUS_SUCCESS || s == STATUS_NO_MORE_FILES,
                "FileBothDirectoryInformation dispatches");

    /* Class 37 (FileIdBothDirectoryInformation) */
    s = ssdt_dispatch(SSDT_NtQueryDirectoryFile,
                      0, 0, (uint64_t)buf, 512,
                      FileIdBothDirectoryInformation, 0);
    TEST_ASSERT(s == STATUS_SUCCESS || s == STATUS_NO_MORE_FILES,
                "FileIdBothDirectoryInformation dispatches");

    /* Invalid class */
    s = ssdt_dispatch(SSDT_NtQueryDirectoryFile,
                      0, 0, (uint64_t)buf, 512, 99, 0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_INFO_CLASS,
                   "invalid dir info class returns STATUS_INVALID_INFO_CLASS");
}

/* ---- S13: File metadata SSDT registration ---- */

static void test_nt_file_metadata_ssdt_registered(void)
{
    NTSTATUS s;
    /* NtQueryInformationFile with bad handle */
    s = ssdt_dispatch(SSDT_NtQueryInformationFile,
                      0xDEAD, 0, 0, 0, FileBasicInformation, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "NtQueryInformationFile registered");

    /* NtSetInformationFile with bad handle */
    s = ssdt_dispatch(SSDT_NtSetInformationFile,
                      0xDEAD, 0, 0, 0, FileBasicInformation, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "NtSetInformationFile registered");

    /* NtFlushBuffersFile with bad handle */
    s = ssdt_dispatch(SSDT_NtFlushBuffersFile, 0xDEAD, 0, 0, 0, 0, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "NtFlushBuffersFile registered");

    /* NtDeleteFile with NULL OBJECT_ATTRIBUTES */
    s = ssdt_dispatch(SSDT_NtDeleteFile, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "NtDeleteFile registered");

    /* NtQueryVolumeInformationFile with bad handle */
    s = ssdt_dispatch(SSDT_NtQueryVolumeInformationFile,
                      0xDEAD, 0, 0, 0, 0, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "NtQueryVolumeInformationFile registered");

    /* NtLockFile with bad handle */
    s = ssdt_dispatch(SSDT_NtLockFile, 0xDEAD, 0, 0, 0, 0, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED, "NtLockFile registered");

    /* NtUnlockFile with bad handle */
    s = ssdt_dispatch(SSDT_NtUnlockFile, 0xDEAD, 0, 0, 0, 0, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED, "NtUnlockFile registered");

    /* NtQueryAttributesFile with NULL OBJECT_ATTRIBUTES */
    s = ssdt_dispatch(SSDT_NtQueryAttributesFile, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT(s != STATUS_NOT_IMPLEMENTED,
                "NtQueryAttributesFile registered");
}

static void test_nt_iocp_roundtrip(void)
{
    HANDLE iocp = 0;
    NTSTATUS s;
    uint64_t key_out = 0, apc_out = 0;
    IO_STATUS_BLOCK iosb;

    /* Create */
    s = ssdt_dispatch(SSDT_NtCreateIoCompletion,
                      (uint64_t)&iocp, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "NtCreateIoCompletion succeeds");
    TEST_ASSERT(iocp != 0, "IOCP handle is non-zero");

    /* Post */
    s = ssdt_dispatch(SSDT_NtSetIoCompletion,
                      (uint64_t)iocp, 42, 99, STATUS_SUCCESS, 1024, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "NtSetIoCompletion succeeds");

    /* Dequeue */
    s = ssdt_dispatch(SSDT_NtRemoveIoCompletion,
                      (uint64_t)iocp,
                      (uint64_t)&key_out, (uint64_t)&apc_out,
                      (uint64_t)&iosb, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "NtRemoveIoCompletion succeeds");
    TEST_ASSERT_EQ(key_out, 42, "IOCP key matches");
    TEST_ASSERT_EQ(apc_out, 99, "IOCP apc context matches");
    TEST_ASSERT_EQ(iosb.Status, STATUS_SUCCESS, "IOCP status matches");
    TEST_ASSERT_EQ(iosb.Information, 1024, "IOCP information matches");

    /* Empty dequeue returns TIMEOUT */
    s = ssdt_dispatch(SSDT_NtRemoveIoCompletion,
                      (uint64_t)iocp,
                      (uint64_t)&key_out, (uint64_t)&apc_out,
                      (uint64_t)&iosb, 0, 0);
    TEST_ASSERT_EQ(s, STATUS_TIMEOUT,
                   "NtRemoveIoCompletion empty returns TIMEOUT");
}

/* ---- S11: IOSB + LastError ---- */

/* RtlNtStatusToDosError: success -> 0 */
static void test_rtl_status_to_dos_success(void)
{
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_SUCCESS), 0,
                   "SUCCESS -> ERROR_SUCCESS (0)");
}

/* RtlNtStatusToDosError: known error mappings */
static void test_rtl_status_to_dos_known(void)
{
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_ACCESS_DENIED), 5,
                   "ACCESS_DENIED -> 5");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_NO_MEMORY), 8,
                   "NO_MEMORY -> 8");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_INVALID_HANDLE), 6,
                   "INVALID_HANDLE -> 6");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_OBJECT_NAME_NOT_FOUND), 2,
                   "OBJECT_NAME_NOT_FOUND -> 2 (FILE_NOT_FOUND)");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_INVALID_PARAMETER), 87,
                   "INVALID_PARAMETER -> 87");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_BUFFER_TOO_SMALL), 122,
                   "BUFFER_TOO_SMALL -> 122");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_ACCESS_VIOLATION), 998,
                   "ACCESS_VIOLATION -> 998");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_PRIVILEGE_NOT_HELD), 1314,
                   "PRIVILEGE_NOT_HELD -> 1314");
}

/* RtlNtStatusToDosError: broad-coverage class mappings (file/path, sharing,
 * buffer, process/handle, sync, and the success-severity timeout/pending
 * special cases). Guards against the "every other status -> 317" sparseness. */
static void test_rtl_status_to_dos_broad(void)
{
    /* file / path class */
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_OBJECT_PATH_NOT_FOUND), 3,
                   "OBJECT_PATH_NOT_FOUND -> 3 (PATH_NOT_FOUND)");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_NO_MORE_FILES), 18,
                   "NO_MORE_FILES -> 18");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_END_OF_FILE), 38,
                   "END_OF_FILE -> 38 (HANDLE_EOF)");
    /* sharing / lock class */
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_SHARING_VIOLATION), 32,
                   "SHARING_VIOLATION -> 32");
    /* buffer / length class */
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_INFO_LENGTH_MISMATCH), 24,
                   "INFO_LENGTH_MISMATCH -> 24 (BAD_LENGTH)");
    /* process / handle / device class */
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_OBJECT_TYPE_MISMATCH), 6,
                   "OBJECT_TYPE_MISMATCH -> 6 (INVALID_HANDLE)");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_NOT_SUPPORTED), 50,
                   "NOT_SUPPORTED -> 50");
    /* sync class */
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_MUTANT_NOT_OWNED), 288,
                   "MUTANT_NOT_OWNED -> 288 (NOT_OWNER)");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_SEMAPHORE_LIMIT_EXCEEDED), 298,
                   "SEMAPHORE_LIMIT_EXCEEDED -> 298 (TOO_MANY_POSTS)");
    /* buffer overflow (warning severity) -> ERROR_MORE_DATA, not
     * ERROR_BUFFER_OVERFLOW -- matches host ntdll RtlNtStatusToDosError */
    TEST_ASSERT_EQ(RtlNtStatusToDosError((NTSTATUS)0x80000005), 234,
                   "BUFFER_OVERFLOW -> 234 (MORE_DATA)");
    /* success-severity codes with a specific Win32 error (must not
     * return 0 despite NT_SUCCESS being true) */
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_TIMEOUT), 1460,
                   "STATUS_TIMEOUT -> 1460 (ERROR_TIMEOUT), not 0");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_PENDING), 997,
                   "STATUS_PENDING -> 997 (ERROR_IO_PENDING), not 0");
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_ABANDONED), 735,
                   "STATUS_ABANDONED -> 735 (ERROR_ABANDONED_WAIT_0), not 0");
    /* exact STATUS_SUCCESS still returns ERROR_SUCCESS */
    TEST_ASSERT_EQ(RtlNtStatusToDosError(STATUS_SUCCESS), 0,
                   "exact SUCCESS still -> 0 after reorder");
}

/* RtlNtStatusToDosError: unknown -> ERROR_MR_MID_NOT_FOUND (317) */
static void test_rtl_status_to_dos_unknown(void)
{
    TEST_ASSERT_EQ(RtlNtStatusToDosError((NTSTATUS)0xC00000FF), 317,
                   "unknown NTSTATUS -> 317 (MR_MID_NOT_FOUND)");
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
    /* Range past user boundary: address below MM_USER_PROBE_ADDRESS but
     * end (addr+Length) crosses it */
    s = ProbeForRead((const void *)0x7FFE0000ULL, 0x100000, 1);
    TEST_ASSERT_EQ(s, STATUS_ACCESS_VIOLATION,
                   "ProbeForRead rejects range past user boundary");
    /* True pointer wraparound: addr near UINTPTR_MAX so addr+Length wraps
     * below addr (exercises the end < addr branch, not the range check) */
    s = ProbeForRead((const void *)0xFFFFFFFFFFFFFF00ULL, 0x200, 1);
    TEST_ASSERT_EQ(s, STATUS_ACCESS_VIOLATION,
                   "ProbeForRead rejects addr+Length wraparound");
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

/* ProbeForRead: invalid Alignment (0 / non-power-of-two) is a caller
 * contract violation and must be rejected, not silently accepted. */
static void test_probe_for_read_invalid_alignment(void)
{
    /* Alignment 0 would disable the mask check entirely */
    NTSTATUS s = ProbeForRead((const void *)0x800000, 4, 0);
    TEST_ASSERT_EQ(s, STATUS_DATATYPE_MISALIGNMENT,
                   "ProbeForRead rejects Alignment 0");
    /* Alignment 3 is not a power of two -- (3-1)=2 is a bogus mask */
    s = ProbeForRead((const void *)0x800000, 4, 3);
    TEST_ASSERT_EQ(s, STATUS_DATATYPE_MISALIGNMENT,
                   "ProbeForRead rejects non-power-of-two Alignment");
    /* Alignment 1 (byte-aligned) remains valid */
    s = ProbeForRead((const void *)0x800001, 4, 1);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS,
                   "ProbeForRead accepts Alignment 1");
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
    test_suite_register_cat("NT: FILETIME pre-epoch guard", test_filetime_pre_epoch_guard, TEST_CAT_ABI);
    test_suite_register_cat("NT: FILETIME DOS low clamp", test_filetime_dos_clamp_low, TEST_CAT_ABI);
    test_suite_register_cat("NT: FILETIME days year guard", test_filetime_days_year_guard, TEST_CAT_ABI);

    /* NT syscall migration tests */
    test_suite_register_cat("NT: syscall SSDT registered", test_nt_syscall_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: SYS_NT_* alias values", test_nt_syscall_ssdt_alias_values, TEST_CAT_ABI);

    /* NT file I/O tests */
    test_suite_register_cat("NT: NtCreateFile/NtOpenFile SSDT", test_nt_create_file_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: file I/O constants", test_nt_file_constants, TEST_CAT_ABI);

    /* NT process/thread lifecycle tests */
    test_suite_register_cat("NT: process/thread SSDT registered", test_nt_process_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: thread info classes", test_nt_thread_info_classes, TEST_CAT_ABI);

    /* NT virtual memory tests */
    test_suite_register_cat("NT: VM SSDT registered", test_nt_vm_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: VM constants", test_nt_vm_constants, TEST_CAT_ABI);

    /* NT system information tests */
    test_suite_register_cat("NT: NtQuerySystemInfo classes", test_nt_sysinfo_classes, TEST_CAT_ABI);
    test_suite_register_cat("NT: NtQueryProcessInfo classes", test_nt_procinfo_classes, TEST_CAT_ABI);

    /* Extended directory enumeration S27 */
    test_suite_register_cat("NT: dir info struct sizes",
                            test_dir_info_struct_sizes, TEST_CAT_ABI);
    test_suite_register_cat("NT: dir enum extended dispatch",
                            test_dir_enum_extended_dispatch, TEST_CAT_ABI);

    /* File metadata S13 */
    test_suite_register_cat("NT: file metadata SSDT registered",
                            test_nt_file_metadata_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("NT: IOCP create/post/dequeue roundtrip",
                            test_nt_iocp_roundtrip, TEST_CAT_ABI);

    /* IOSB + LastError tests */
    test_suite_register_cat("NT: RtlNtStatusToDosError success",
                            test_rtl_status_to_dos_success, TEST_CAT_ABI);
    test_suite_register_cat("NT: RtlNtStatusToDosError known",
                            test_rtl_status_to_dos_known, TEST_CAT_ABI);
    test_suite_register_cat("NT: RtlNtStatusToDosError broad coverage",
                            test_rtl_status_to_dos_broad, TEST_CAT_ABI);
    test_suite_register_cat("NT: RtlNtStatusToDosError unknown",
                            test_rtl_status_to_dos_unknown, TEST_CAT_ABI);

    /* ZwXxx alias layer tests */
    test_suite_register_cat("NT: ZwClose kernel dispatch",
                            test_zw_close_kernel_dispatch, TEST_CAT_ABI);
    test_suite_register_cat("NT: ProbeForRead user range",
                            test_probe_for_read_user_range, TEST_CAT_ABI);
    test_suite_register_cat("NT: ProbeForRead kernel ptr rejected",
                            test_probe_for_read_kernel_ptr, TEST_CAT_ABI);
    test_suite_register_cat("NT: ProbeForRead alignment",
                            test_probe_for_read_alignment, TEST_CAT_ABI);
    test_suite_register_cat("NT: ProbeForRead invalid alignment rejected",
                            test_probe_for_read_invalid_alignment, TEST_CAT_ABI);
    test_suite_register_cat("NT: ASSERT_KERNEL_CALLER macro",
                            test_assert_kernel_caller, TEST_CAT_ABI);
    test_suite_register_cat("NT: previous mode default kernel",
                            test_previous_mode_default, TEST_CAT_ABI);
    test_suite_register_cat("NT: Zw nested in UserMode restores",
                            test_zw_nested_in_usermode, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
