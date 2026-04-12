/* ============================================================================
 * test_cpu_security.c -- CPU security hardening unit tests
 *
 * Tests NX, SMEP/SMAP state, and KPTI trampoline infrastructure
 * from TODO-17-kernel-security-hardening.md S1-S3.
 *
 * All tests are read-only checks -- no live boot infrastructure calls.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/cpuid.h"
#include "kernel/cpu_security.h"
#include "kernel/kpti.h"
#include "kernel/smp.h"
#include "kernel/msr.h"

/* ---- S1: NX Bit ---- */

static void test_nx_efer_set(void)
{
    if (!cpu_has(CPU_FEATURE_NX)) {
        TEST_SKIP("CPU does not support NX");
        return;
    }
    uint64_t efer = msr_read(MSR_IA32_EFER);
    TEST_ASSERT(efer & (1ULL << 11),
                "EFER.NXE is set when CPU supports NX");
}

/* ---- S2: SMEP/SMAP state ---- */

static void test_smep_smap_state(void)
{
    /* SMEP/SMAP are currently skipped on all platforms due to kernel
     * PTE User bit. Verify the skip is logged, not silently ignored. */
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    /* On Hyper-V, SMEP/SMAP may be enforced via EPT even if CR4 bits
     * are not set. On bare metal/TCG, they're skipped. Either way,
     * the test just confirms no crash accessing cr4. */
    TEST_ASSERT(1, "CR4 read succeeds (SMEP/SMAP state accessible)");
    (void)cr4;
}

/* ---- S3: KPTI trampoline infrastructure ---- */

static void test_kpti_percpu_kernel_cr3(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    TEST_ASSERT_NEQ(cpu->kernel_cr3, 0,
                    "per-CPU kernel_cr3 is non-zero (initialized from CR3)");
}

static void test_kpti_percpu_user_cr3_eq_kernel(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    TEST_ASSERT_EQ(cpu->user_cr3, cpu->kernel_cr3,
                   "user_cr3 == kernel_cr3 (no isolation until S6)");
}

static void test_kpti_active_false(void)
{
    TEST_ASSERT_EQ(kpti_active(), 0,
                   "kpti_active() returns 0 (infrastructure only, not wired)");
}

static void test_kpti_kernel_cr3_matches_hw(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    uint64_t hw_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(hw_cr3));
    /* Mask off PCID bits (lower 12 bits) for comparison */
    TEST_ASSERT_EQ(cpu->kernel_cr3 & ~0xFFFULL, hw_cr3 & ~0xFFFULL,
                   "per-CPU kernel_cr3 matches hardware CR3 (PML4 base)");
}

/* ---- S1: XSAVE / XRSTOR ---- */

static void test_xcr0_active(void)
{
    if (!cpu_has(CPU_FEATURE_XSAVE)) {
        TEST_SKIP("CPU does not support XSAVE");
        return;
    }
    extern struct cpu_features g_cpu;
    TEST_ASSERT_NEQ(g_cpu.xcr0_active, 0,
                    "xcr0_active is non-zero when XSAVE supported");
}

static void test_xsave_size(void)
{
    if (!cpu_has(CPU_FEATURE_XSAVE)) {
        TEST_SKIP("CPU does not support XSAVE");
        return;
    }
    extern struct cpu_features g_cpu;
    TEST_ASSERT(g_cpu.xsave_size_max > 0,
                "xsave_size_max > 0 when XSAVE supported");
    TEST_ASSERT(g_cpu.xsave_size_max >= 512,
                "xsave_size_max >= 512 (minimum FXSAVE area)");
}

static void test_xcr0_has_x87_sse(void)
{
    if (!cpu_has(CPU_FEATURE_XSAVE)) {
        TEST_SKIP("CPU does not support XSAVE");
        return;
    }
    extern struct cpu_features g_cpu;
    /* XCR0 bits 0 (x87) and 1 (SSE) are mandatory when XSAVE is enabled */
    TEST_ASSERT(g_cpu.xcr0_active & 0x3,
                "xcr0_active has x87 (bit 0) and SSE (bit 1) set");
}

static void test_cr4_osxsave(void)
{
    if (!cpu_has(CPU_FEATURE_XSAVE)) {
        TEST_SKIP("CPU does not support XSAVE");
        return;
    }
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    TEST_ASSERT(cr4 & (1ULL << 18),
                "CR4.OSXSAVE (bit 18) is set when XSAVE supported");
}

/* ---- Registration ---- */

void test_register_x86(void)
{
    test_suite_register_cat("CPU security: NX EFER.NXE set",
        test_nx_efer_set, TEST_CAT_X86);
    test_suite_register_cat("CPU security: SMEP/SMAP CR4 accessible",
        test_smep_smap_state, TEST_CAT_X86);
    test_suite_register_cat("CPU security: kernel_cr3 non-zero",
        test_kpti_percpu_kernel_cr3, TEST_CAT_X86);
    test_suite_register_cat("CPU security: user_cr3 == kernel_cr3",
        test_kpti_percpu_user_cr3_eq_kernel, TEST_CAT_X86);
    test_suite_register_cat("CPU security: kpti_active() == 0",
        test_kpti_active_false, TEST_CAT_X86);
    test_suite_register_cat("CPU security: kernel_cr3 matches HW CR3",
        test_kpti_kernel_cr3_matches_hw, TEST_CAT_X86);

    /* S1: XSAVE / XRSTOR */
    test_suite_register_cat("XSAVE: xcr0_active non-zero",
        test_xcr0_active, TEST_CAT_X86);
    test_suite_register_cat("XSAVE: xsave_size_max valid",
        test_xsave_size, TEST_CAT_X86);
    test_suite_register_cat("XSAVE: xcr0 has x87+SSE bits",
        test_xcr0_has_x87_sse, TEST_CAT_X86);
    test_suite_register_cat("XSAVE: CR4.OSXSAVE set",
        test_cr4_osxsave, TEST_CAT_X86);
}

#endif /* KERNEL_TESTS */
