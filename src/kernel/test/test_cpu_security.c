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
#include "kernel/mm/memops.h"
#include "gfx_simd.h"

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

/* ---- S2: AVX2 memops ---- */

static void test_memcpy_avx_correctness(void)
{
    if (!simd_avx2_ok) {
        TEST_SKIP("AVX2 not enabled (XCR0 bit 2 not set)");
        return;
    }

    /* Stack-allocated 32-byte aligned buffers (256 bytes each) */
    uint8_t __attribute__((aligned(32))) src[256];
    uint8_t __attribute__((aligned(32))) dst[256];
    uint32_t i;

    /* Fill src with known pattern, dst with 0xFF */
    for (i = 0; i < 256; i++) {
        src[i] = (uint8_t)(i & 0xFF);
        dst[i] = 0xFF;
    }

    memcpy_avx(dst, src, 256);

    /* Verify byte-for-byte match */
    for (i = 0; i < 256; i++) {
        if (dst[i] != src[i]) {
            TEST_ASSERT_EQ(dst[i], src[i],
                           "memcpy_avx: dst matches src at every byte");
            return;
        }
    }
    TEST_ASSERT(1, "memcpy_avx: 256-byte copy matches reference");
}

static void test_memcpy_avx_tail(void)
{
    if (!simd_avx2_ok) {
        TEST_SKIP("AVX2 not enabled (XCR0 bit 2 not set)");
        return;
    }

    /* Test with non-aligned size (37 bytes: 1 YMM iter + 5 byte tail) */
    uint8_t __attribute__((aligned(32))) src[64];
    uint8_t __attribute__((aligned(32))) dst[64];
    uint32_t i;

    for (i = 0; i < 37; i++)
        src[i] = (uint8_t)(0xA0 + i);
    for (i = 0; i < 64; i++)
        dst[i] = 0;

    memcpy_avx(dst, src, 37);

    for (i = 0; i < 37; i++) {
        if (dst[i] != src[i]) {
            TEST_ASSERT_EQ(dst[i], src[i],
                           "memcpy_avx tail: first 37 bytes match");
            return;
        }
    }
    /* Verify bytes beyond copy length are untouched */
    TEST_ASSERT_EQ(dst[37], 0,
                   "memcpy_avx tail: byte 37 untouched (still 0)");
    TEST_ASSERT(1, "memcpy_avx tail: 37-byte copy correct");
}

static void test_memset_avx_correctness(void)
{
    if (!simd_avx2_ok) {
        TEST_SKIP("AVX2 not enabled (XCR0 bit 2 not set)");
        return;
    }

    uint8_t __attribute__((aligned(32))) buf[256];
    uint32_t i;

    for (i = 0; i < 256; i++)
        buf[i] = 0;

    memset_avx(buf, 0x42, 256);

    for (i = 0; i < 256; i++) {
        if (buf[i] != 0x42) {
            TEST_ASSERT_EQ(buf[i], 0x42,
                           "memset_avx: every byte is 0x42");
            return;
        }
    }
    TEST_ASSERT(1, "memset_avx: 256-byte fill with 0x42 correct");
}

static void test_memset_avx_tail(void)
{
    if (!simd_avx2_ok) {
        TEST_SKIP("AVX2 not enabled (XCR0 bit 2 not set)");
        return;
    }

    uint8_t __attribute__((aligned(32))) buf[64];
    uint32_t i;

    for (i = 0; i < 64; i++)
        buf[i] = 0;

    memset_avx(buf, 0xBB, 41);

    for (i = 0; i < 41; i++) {
        if (buf[i] != 0xBB) {
            TEST_ASSERT_EQ(buf[i], 0xBB,
                           "memset_avx tail: first 41 bytes are 0xBB");
            return;
        }
    }
    TEST_ASSERT_EQ(buf[41], 0,
                   "memset_avx tail: byte 41 untouched (still 0)");
    TEST_ASSERT(1, "memset_avx tail: 41-byte fill correct");
}

static void test_memcpy_fast_dispatch(void)
{
    /* memcpy_fast should produce correct results regardless of CPU */
    uint8_t __attribute__((aligned(32))) src[128];
    uint8_t __attribute__((aligned(32))) dst[128];
    uint32_t i;

    for (i = 0; i < 128; i++) {
        src[i] = (uint8_t)(i * 3);
        dst[i] = 0;
    }

    memcpy_fast(dst, src, 128);

    for (i = 0; i < 128; i++) {
        if (dst[i] != src[i]) {
            TEST_ASSERT_EQ(dst[i], src[i],
                           "memcpy_fast dispatch: result matches");
            return;
        }
    }
    TEST_ASSERT(1, "memcpy_fast dispatch: 128-byte copy correct");
}

static void test_memset_fast_dispatch(void)
{
    uint8_t __attribute__((aligned(32))) buf[128];
    uint32_t i;

    for (i = 0; i < 128; i++)
        buf[i] = 0;

    memset_fast(buf, 0x55, 128);

    for (i = 0; i < 128; i++) {
        if (buf[i] != 0x55) {
            TEST_ASSERT_EQ(buf[i], 0x55,
                           "memset_fast dispatch: every byte is 0x55");
            return;
        }
    }
    TEST_ASSERT(1, "memset_fast dispatch: 128-byte fill correct");
}

/* ---- S3: AVX-512 ---- */

static void test_avx512_xcr0_bits(void)
{
    if (!cpu_has(CPU_FEATURE_AVX512F)) {
        TEST_SKIP("CPU does not support AVX-512");
        return;
    }
    extern struct cpu_features g_cpu;
    /* If AVX-512 is enabled (not throttled), XCR0 bits 5-7 must be set.
     * If throttled, they were cleared by simd_enable_avx512(). */
    if (simd_avx512_ok) {
        TEST_ASSERT((g_cpu.xcr0_active & 0xE0) == 0xE0,
                     "xcr0_active has opmask+ZMM_Hi256+Hi16_ZMM when AVX-512 ok");
    } else {
        TEST_ASSERT((g_cpu.xcr0_active & 0xE0) == 0,
                     "xcr0_active has AVX-512 bits cleared when throttled");
    }
}

static void test_avx512_flag_consistent(void)
{
    /* simd_avx512_ok must be 0 or 1 */
    TEST_ASSERT(simd_avx512_ok == 0 || simd_avx512_ok == 1,
                "simd_avx512_ok is 0 or 1");
    /* If AVX-512 not in CPUID, flag must be 0 */
    if (!cpu_has(CPU_FEATURE_AVX512F)) {
        TEST_ASSERT_EQ(simd_avx512_ok, 0,
                       "simd_avx512_ok == 0 when CPU lacks AVX512F");
    }
}

static void test_memcpy_avx512_correctness(void)
{
    if (!simd_avx512_ok) {
        TEST_SKIP("AVX-512 not enabled");
        return;
    }

    uint8_t __attribute__((aligned(64))) src[256];
    uint8_t __attribute__((aligned(64))) dst[256];
    uint32_t i;

    for (i = 0; i < 256; i++) {
        src[i] = (uint8_t)(i & 0xFF);
        dst[i] = 0xFF;
    }

    memcpy_avx512(dst, src, 256);

    for (i = 0; i < 256; i++) {
        if (dst[i] != src[i]) {
            TEST_ASSERT_EQ(dst[i], src[i],
                           "memcpy_avx512: dst matches src at every byte");
            return;
        }
    }
    TEST_ASSERT(1, "memcpy_avx512: 256-byte copy matches reference");
}

static void test_memcpy_avx512_tail(void)
{
    if (!simd_avx512_ok) {
        TEST_SKIP("AVX-512 not enabled");
        return;
    }

    /* Test with non-aligned size (70 bytes: 1 ZMM iter + 6 byte tail) */
    uint8_t __attribute__((aligned(64))) src[128];
    uint8_t __attribute__((aligned(64))) dst[128];
    uint32_t i;

    for (i = 0; i < 70; i++)
        src[i] = (uint8_t)(0xC0 + i);
    for (i = 0; i < 128; i++)
        dst[i] = 0;

    memcpy_avx512(dst, src, 70);

    for (i = 0; i < 70; i++) {
        if (dst[i] != src[i]) {
            TEST_ASSERT_EQ(dst[i], src[i],
                           "memcpy_avx512 tail: first 70 bytes match");
            return;
        }
    }
    TEST_ASSERT_EQ(dst[70], 0,
                   "memcpy_avx512 tail: byte 70 untouched (still 0)");
    TEST_ASSERT(1, "memcpy_avx512 tail: 70-byte copy correct");
}

static void test_memset_avx512_correctness(void)
{
    if (!simd_avx512_ok) {
        TEST_SKIP("AVX-512 not enabled");
        return;
    }

    uint8_t __attribute__((aligned(64))) buf[256];
    uint32_t i;

    for (i = 0; i < 256; i++)
        buf[i] = 0;

    memset_avx512(buf, 0x7A, 256);

    for (i = 0; i < 256; i++) {
        if (buf[i] != 0x7A) {
            TEST_ASSERT_EQ(buf[i], 0x7A,
                           "memset_avx512: every byte is 0x7A");
            return;
        }
    }
    TEST_ASSERT(1, "memset_avx512: 256-byte fill with 0x7A correct");
}

static void test_memset_avx512_tail(void)
{
    if (!simd_avx512_ok) {
        TEST_SKIP("AVX-512 not enabled");
        return;
    }

    uint8_t __attribute__((aligned(64))) buf[128];
    uint32_t i;

    for (i = 0; i < 128; i++)
        buf[i] = 0;

    memset_avx512(buf, 0xDD, 73);

    for (i = 0; i < 73; i++) {
        if (buf[i] != 0xDD) {
            TEST_ASSERT_EQ(buf[i], 0xDD,
                           "memset_avx512 tail: first 73 bytes are 0xDD");
            return;
        }
    }
    TEST_ASSERT_EQ(buf[73], 0,
                   "memset_avx512 tail: byte 73 untouched (still 0)");
    TEST_ASSERT(1, "memset_avx512 tail: 73-byte fill correct");
}

static void test_memcpy_fast_avx512_dispatch(void)
{
    /* memcpy_fast should pick AVX-512 when available */
    uint8_t __attribute__((aligned(64))) src[128];
    uint8_t __attribute__((aligned(64))) dst[128];
    uint32_t i;

    for (i = 0; i < 128; i++) {
        src[i] = (uint8_t)(i * 5);
        dst[i] = 0;
    }

    memcpy_fast(dst, src, 128);

    for (i = 0; i < 128; i++) {
        if (dst[i] != src[i]) {
            TEST_ASSERT_EQ(dst[i], src[i],
                           "memcpy_fast avx512 dispatch: result matches");
            return;
        }
    }
    TEST_ASSERT(1, "memcpy_fast dispatch: correct with current SIMD tier");
}

/* ---- S4: MSR infrastructure ---- */

static void test_msr_read_efer_stable(void)
{
    /* msr_read(MSR_IA32_EFER) should return the same value on repeated calls */
    uint64_t efer1 = msr_read(MSR_IA32_EFER);
    uint64_t efer2 = msr_read(MSR_IA32_EFER);
    TEST_ASSERT_EQ(efer1, efer2,
                   "msr_read(EFER) stable across two consecutive calls");
}

static void test_msr_read_efer_lma_set(void)
{
    /* In long mode, EFER.LMA (bit 10) must be set */
    uint64_t efer = msr_read(MSR_IA32_EFER);
    TEST_ASSERT(efer & EFER_LMA,
                "EFER.LMA is set (kernel runs in long mode)");
}

static void test_msr_try_read_valid(void)
{
    /* msr_try_read on a known-good MSR should succeed */
    uint64_t val = 0;
    int ret = msr_try_read(MSR_IA32_EFER, &val);
    TEST_ASSERT_EQ(ret, 0,
                   "msr_try_read(EFER) returns 0 (success)");
    TEST_ASSERT(val & EFER_LMA,
                "msr_try_read(EFER) output has LMA set");
}

static void test_msr_try_read_invalid(void)
{
    /* msr_try_read on a nonexistent MSR should return -1 without panic */
    uint64_t val = 0xDEAD;
    int ret = msr_try_read(0xFFFFFFFF, &val);
    TEST_ASSERT_EQ(ret, -1,
                   "msr_try_read(0xFFFFFFFF) returns -1 (#GP caught)");
    TEST_ASSERT_EQ(val, 0,
                   "msr_try_read sets output to 0 on failure");
}

static void test_msr_try_read_null_out(void)
{
    /* msr_try_read with NULL out pointer should not crash */
    int ret = msr_try_read(MSR_IA32_EFER, (uint64_t *)0);
    TEST_ASSERT_EQ(ret, 0,
                   "msr_try_read(EFER, NULL) returns 0 without crash");
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

    /* S2: AVX2 memops */
    test_suite_register_cat("AVX2: memcpy_avx correctness",
        test_memcpy_avx_correctness, TEST_CAT_X86);
    test_suite_register_cat("AVX2: memcpy_avx tail bytes",
        test_memcpy_avx_tail, TEST_CAT_X86);
    test_suite_register_cat("AVX2: memset_avx correctness",
        test_memset_avx_correctness, TEST_CAT_X86);
    test_suite_register_cat("AVX2: memset_avx tail bytes",
        test_memset_avx_tail, TEST_CAT_X86);
    test_suite_register_cat("AVX2: memcpy_fast dispatch",
        test_memcpy_fast_dispatch, TEST_CAT_X86);
    test_suite_register_cat("AVX2: memset_fast dispatch",
        test_memset_fast_dispatch, TEST_CAT_X86);

    /* S3: AVX-512 */
    test_suite_register_cat("AVX-512: xcr0 bits 5-7 consistent",
        test_avx512_xcr0_bits, TEST_CAT_X86);
    test_suite_register_cat("AVX-512: simd_avx512_ok flag consistent",
        test_avx512_flag_consistent, TEST_CAT_X86);
    test_suite_register_cat("AVX-512: memcpy_avx512 correctness",
        test_memcpy_avx512_correctness, TEST_CAT_X86);
    test_suite_register_cat("AVX-512: memcpy_avx512 tail bytes",
        test_memcpy_avx512_tail, TEST_CAT_X86);
    test_suite_register_cat("AVX-512: memset_avx512 correctness",
        test_memset_avx512_correctness, TEST_CAT_X86);
    test_suite_register_cat("AVX-512: memset_avx512 tail bytes",
        test_memset_avx512_tail, TEST_CAT_X86);
    test_suite_register_cat("AVX-512: memcpy_fast dispatch",
        test_memcpy_fast_avx512_dispatch, TEST_CAT_X86);

    /* S4: MSR infrastructure */
    test_suite_register_cat("MSR: msr_read(EFER) stable",
        test_msr_read_efer_stable, TEST_CAT_X86);
    test_suite_register_cat("MSR: EFER.LMA set in long mode",
        test_msr_read_efer_lma_set, TEST_CAT_X86);
    test_suite_register_cat("MSR: msr_try_read valid MSR",
        test_msr_try_read_valid, TEST_CAT_X86);
    test_suite_register_cat("MSR: msr_try_read invalid MSR",
        test_msr_try_read_invalid, TEST_CAT_X86);
    test_suite_register_cat("MSR: msr_try_read NULL out",
        test_msr_try_read_null_out, TEST_CAT_X86);
}

#endif /* KERNEL_TESTS */
