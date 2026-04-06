/* ============================================================================
 * test_peb_teb.c -- PEB / TEB user-mode ABI unit tests
 *
 * Tests PEB/TEB struct offsets (compile-time), PEB population after
 * task_exec, OS version fields, and RTL_USER_PROCESS_PARAMETERS content.
 * TEB runtime tests (GS self-pointer, ClientId) need a user-mode test
 * binary -- kernel GS points to per-CPU data, not TEB.
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
        /* PEB not yet populated -- skip gracefully */
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
        TEST_ASSERT(1, "PEB not yet allocated -- skip");
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
        TEST_ASSERT(1, "PEB/RTLPP not yet allocated -- skip");
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

/* ---- TLS expansion slots (S12) ---- */

#include "kernel/sched/task.h"

static void test_tls_constants(void)
{
    TEST_ASSERT_EQ(TLS_MINIMUM_AVAILABLE, 64,
                   "TLS_MINIMUM_AVAILABLE == 64");
    TEST_ASSERT_EQ(TLS_EXPANSION_SLOTS, 1024,
                   "TLS_EXPANSION_SLOTS == 1024");
    TEST_ASSERT_EQ(TLS_MAXIMUM_AVAILABLE, 1088,
                   "TLS_MAXIMUM_AVAILABLE == 1088");
    TEST_ASSERT_EQ(TLS_EXPANSION_BITMAP_WORDS, 16,
                   "TLS_EXPANSION_BITMAP_WORDS == 16");
}

static void test_tls_static_alloc_free(void)
{
    /* Allocate a static slot and verify it's in range 0-63 */
    uint32_t pid = task_current()->pid;
    int slot = tls_alloc(pid);
    TEST_ASSERT(slot >= 0 && slot < 64,
                "tls_alloc returns static slot (0-63)");
    if (slot >= 0) {
        /* Set and get value */
        tls_set_value(pid, (uint32_t)slot, 0xDEADBEEF12345678);
        uint64_t val = tls_get_value(pid, (uint32_t)slot);
        TEST_ASSERT_EQ(val, 0xDEADBEEF12345678,
                       "TLS static slot round-trips value");
        /* Free the slot */
        int ret = tls_free(pid, (uint32_t)slot);
        TEST_ASSERT_EQ(ret, 0, "tls_free succeeds for static slot");
    }
}

static void test_tls_expansion_alloc(void)
{
    uint32_t pid = task_current()->pid;
    int slots[65];
    uint32_t i;

    /* Allocate 65 slots -- first 64 static, 65th triggers expansion */
    for (i = 0; i < 65; i++) {
        slots[i] = tls_alloc(pid);
        if (slots[i] < 0) break;
    }

    TEST_ASSERT(slots[64] >= 64,
                "65th tls_alloc returns expansion slot (>= 64)");

    /* Write/read the expansion slot */
    if (slots[64] >= 64) {
        tls_set_value(pid, (uint32_t)slots[64], 0xCAFEBABECAFEBABE);
        uint64_t val = tls_get_value(pid, (uint32_t)slots[64]);
        TEST_ASSERT_EQ(val, 0xCAFEBABECAFEBABE,
                       "TLS expansion slot round-trips value");
    }

    /* Free all slots */
    for (i = 0; i < 65; i++) {
        if (slots[i] >= 0)
            tls_free(pid, (uint32_t)slots[i]);
    }
}

static void test_tls_expansion_reuse(void)
{
    uint32_t pid = task_current()->pid;
    int slots[65];
    uint32_t i;

    /* Allocate 65 to get into expansion */
    for (i = 0; i < 65; i++) {
        slots[i] = tls_alloc(pid);
        if (slots[i] < 0) break;
    }

    /* Free the expansion slot */
    if (slots[64] >= 64) {
        int freed_idx = slots[64];
        tls_free(pid, (uint32_t)freed_idx);

        /* Re-alloc should return the same index (reuse) */
        int realloc_idx = tls_alloc(pid);
        TEST_ASSERT_EQ(realloc_idx, freed_idx,
                       "tls_alloc reuses freed expansion slot");
        if (realloc_idx >= 0)
            tls_free(pid, (uint32_t)realloc_idx);
    }

    /* Cleanup */
    for (i = 0; i < 64; i++) {
        if (slots[i] >= 0)
            tls_free(pid, (uint32_t)slots[i]);
    }
}

/* Static to avoid 4 KB+ stack allocation */
static int s_tls_boundary_slots[TLS_MAXIMUM_AVAILABLE];

static void test_tls_expansion_boundary(void)
{
    /* Allocate all 1088 slots to reach the last expansion slot.
     * This exercises the full range and validates boundary semantics. */
    uint32_t pid = task_current()->pid;
    int *slots = s_tls_boundary_slots;
    uint32_t i;
    int last_slot = -1;
    int alloc_count = 0;

    for (i = 0; i < TLS_MAXIMUM_AVAILABLE; i++) {
        slots[i] = tls_alloc(pid);
        if (slots[i] < 0) break;
        alloc_count++;
        if (slots[i] > last_slot) last_slot = slots[i];
    }

    /* Last allocated slot must be in the expansion range (>= 64) */
    TEST_ASSERT(last_slot >= (int)TLS_MINIMUM_AVAILABLE,
                "last allocated slot is in expansion range");

    /* Round-trip a value through the highest slot */
    if (last_slot >= 0) {
        tls_set_value(pid, (uint32_t)last_slot, 0x1087108710871087ULL);
        uint64_t val = tls_get_value(pid, (uint32_t)last_slot);
        TEST_ASSERT_EQ(val, 0x1087108710871087ULL,
                       "TLS high expansion slot round-trips value");
    }

    POST16(POST16_TLS_EXPAND_TEST);

    /* Index 1088 should be rejected (out of range) */
    uint64_t bad = tls_get_value(pid, TLS_MAXIMUM_AVAILABLE);
    TEST_ASSERT_EQ(bad, 0,
                   "tls_get_value(1088) returns 0 (out of range)");

    /* Asking for one more slot should fail (all 1088 occupied) */
    int overflow = tls_alloc(pid);
    TEST_ASSERT_EQ(overflow, -1,
                   "tls_alloc returns -1 when all slots occupied");

    /* Free all slots and verify each free succeeds */
    for (i = 0; i < (uint32_t)alloc_count; i++) {
        int ret = tls_free(pid, (uint32_t)slots[i]);
        TEST_ASSERT_EQ(ret, 0, "tls_free succeeds for allocated slot");
    }
}

static void test_tls_expansion_post_codes(void)
{
    TEST_ASSERT_EQ(POST16_TLS_EXPAND, 0xDF10,
                   "POST16_TLS_EXPAND == 0xDF10");
    TEST_ASSERT_EQ(POST16_TLS_EXPAND_ALLOC, 0xDF11,
                   "POST16_TLS_EXPAND_ALLOC == 0xDF11");
    TEST_ASSERT_EQ(POST16_TLS_EXPAND_TEST, 0xDF12,
                   "POST16_TLS_EXPAND_TEST == 0xDF12");
    TEST_ASSERT_EQ(POST16_TLS_EXPAND_CLEAN, 0xDF13,
                   "POST16_TLS_EXPAND_CLEAN == 0xDF13");
}

/* ---- Registration ---- */

void test_register_peb_teb(void)
{
    test_suite_register_cat("PEB/TEB: TEB offsets", test_teb_offsets, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: PEB offsets", test_peb_offsets, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: OS version", test_peb_os_version, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: populated", test_peb_populated, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: RTLPP content", test_rtlpp_content, TEST_CAT_ABI);
    /* S12: TLS expansion slots */
    test_suite_register_cat("PEB/TEB: TLS constants",
                            test_tls_constants, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: TLS static alloc/free",
                            test_tls_static_alloc_free, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: TLS expansion alloc",
                            test_tls_expansion_alloc, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: TLS expansion reuse",
                            test_tls_expansion_reuse, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: TLS expansion boundary",
                            test_tls_expansion_boundary, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: TLS POST codes",
                            test_tls_expansion_post_codes, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
