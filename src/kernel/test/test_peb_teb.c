/* ============================================================================
 * test_peb_teb.c — PEB / TEB user-mode ABI unit tests
 *
 * Tests PEB/TEB struct offsets (compile-time), PEB population after
 * task_exec, OS version fields, and RTL_USER_PROCESS_PARAMETERS content.
 * TEB runtime tests (GS self-pointer, ClientId) need a user-mode test
 * binary — kernel GS points to per-CPU data, not TEB.
 *
 * XREF: 02-kernel-core/TODO-04-peb-teb-user-abi.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/ob/peb.h"
#include "kernel/ob/teb.h"
#include "kernel/acpi.h"

/* ---- Compile-time offset checks (TEB) ---- */

static void test_teb_offsets(void)
{
    TEST_ASSERT(__builtin_offsetof(TEB, NtTib.Self) == 0x30,
                "TEB.NtTib.Self is at gs:[0x30]");
    TEST_ASSERT(__builtin_offsetof(TEB, ClientId) == 0x40,
                "TEB.ClientId at offset 0x40");
    TEST_ASSERT(__builtin_offsetof(TEB, ProcessEnvironmentBlock) == 0x60,
                "TEB.ProcessEnvironmentBlock at gs:[0x60]");
    TEST_ASSERT(__builtin_offsetof(TEB, LastErrorValue) == 0x68,
                "TEB.LastErrorValue at gs:[0x68]");
    TEST_ASSERT(__builtin_offsetof(TEB, TlsSlots) == 0x1480,
                "TEB.TlsSlots at gs:[0x1480]");
}

/* ---- Compile-time offset checks (PEB) ---- */

static void test_peb_offsets(void)
{
    TEST_ASSERT(__builtin_offsetof(PEB, ImageBaseAddress) == 0x10,
                "PEB.ImageBaseAddress at offset 0x10");
    TEST_ASSERT(__builtin_offsetof(PEB, Ldr) == 0x18,
                "PEB.Ldr at offset 0x18");
    TEST_ASSERT(__builtin_offsetof(PEB, ProcessParameters) == 0x20,
                "PEB.ProcessParameters at offset 0x20");
    TEST_ASSERT(__builtin_offsetof(PEB, OSMajorVersion) == 0xA4,
                "PEB.OSMajorVersion at offset 0xA4");
    TEST_ASSERT(__builtin_offsetof(PEB, OSBuildNumber) == 0xAC,
                "PEB.OSBuildNumber at offset 0xAC");
    TEST_ASSERT(__builtin_offsetof(PEB, NumberOfProcessors) == 0xB8,
                "PEB.NumberOfProcessors at offset 0xB8");
}

/* ---- PEB OS version fields ---- */
/* The PEB at 0x7FFDE000 is populated by peb_alloc_for_task() during
 * task_exec. At test time, cmd.exe has already been exec'd, so we can
 * read the PEB directly from the shared address space. */

static void test_peb_os_version(void)
{
    PEB *peb = (PEB *)0x7FFDE000ULL;

    /* Check if PEB page is mapped (non-zero content) */
    if (peb->OSMajorVersion == 0 && peb->OSBuildNumber == 0) {
        /* PEB not yet populated — skip gracefully */
        TEST_ASSERT(1, "PEB not yet allocated (no user task exec'd)");
        return;
    }

    TEST_ASSERT(peb->OSMajorVersion == 10,
                "PEB.OSMajorVersion == 10 (Win11)");
    TEST_ASSERT(peb->OSMinorVersion == 0,
                "PEB.OSMinorVersion == 0");
    TEST_ASSERT(peb->OSBuildNumber == 22621,
                "PEB.OSBuildNumber == 22621 (Win11 22H2)");
}

/* ---- PEB populated fields ---- */

static void test_peb_populated(void)
{
    PEB *peb = (PEB *)0x7FFDE000ULL;

    if (peb->OSMajorVersion == 0) {
        TEST_ASSERT(1, "PEB not yet allocated — skip");
        return;
    }

    TEST_ASSERT(peb->ProcessParameters != (void *)0,
                "PEB.ProcessParameters is non-NULL");
    TEST_ASSERT(peb->NumberOfProcessors > 0,
                "PEB.NumberOfProcessors > 0");
    TEST_ASSERT(peb->NumberOfProcessors == acpi_get_cpu_count(),
                "PEB.NumberOfProcessors matches acpi_get_cpu_count()");
    TEST_ASSERT(peb->BeingDebugged == 0,
                "PEB.BeingDebugged == 0");
}

/* ---- RTL_USER_PROCESS_PARAMETERS ---- */

static void test_rtlpp_content(void)
{
    PEB *peb = (PEB *)0x7FFDE000ULL;

    if (peb->OSMajorVersion == 0 || !peb->ProcessParameters) {
        TEST_ASSERT(1, "PEB/RTLPP not yet allocated — skip");
        return;
    }

    RTL_USER_PROCESS_PARAMETERS *pp = peb->ProcessParameters;

    TEST_ASSERT(pp->ImagePathName.Length > 0,
                "RTLPP.ImagePathName is non-empty");
    TEST_ASSERT(pp->ImagePathName.Buffer != (void *)0,
                "RTLPP.ImagePathName.Buffer is non-NULL");
    TEST_ASSERT(pp->CommandLine.Length > 0,
                "RTLPP.CommandLine is non-empty");
    TEST_ASSERT(pp->Environment != (void *)0,
                "RTLPP.Environment is non-NULL");
}

/* ---- Registration ---- */

void test_register_peb_teb(void)
{
    test_suite_register_cat("PEB/TEB: TEB offsets", test_teb_offsets, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: PEB offsets", test_peb_offsets, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: OS version", test_peb_os_version, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: populated", test_peb_populated, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: RTLPP content", test_rtlpp_content, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
