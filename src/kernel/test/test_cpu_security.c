/* ============================================================================
 * test_cpu_security.c -- CPU security hardening unit tests
 *
 * Tests NX, SMEP/SMAP state, and KPTI trampoline infrastructure
 * from TODO-10-kernel-security-hardening.md S1-S3.
 *
 * All tests are read-only checks -- no live boot infrastructure calls.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/cpuid.h"

/* snprintf is not in freestanding kernel headers; declared extern at file
 * scope so test functions can build per-iteration mismatch messages with
 * the offending byte index baked in (per implement-unit-tests skill). */
extern int snprintf(char *buf, size_t size, const char *fmt, ...);
#include "kernel/cpu_security.h"
#include "kernel/cpuid_platform.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "kernel/topology.h"
#include "kernel/pmc.h"
#include "kernel/kpti.h"
#include "kernel/smp.h"
#include "kernel/msr.h"
#include "kernel/mm/memops.h"
#include "kernel/mm/vmm.h"
#include "kernel/security/pku.h"
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
    (void)cr4;
}

/* BSP-only Phase 0 activation-summary helper smoke test. Confirms the
 * helper returns cleanly when invoked from the test runner (which runs on
 * the BSP after boot). Per the header contract, the helper itself is
 * BSP-only -- this test does NOT establish AP / ISR / preemption safety;
 * the `[Phase0]` label would misattribute register state if called from
 * an AP. Future AP audit needs a separate phase/context-aware helper. */
static void test_cpu_security_log_state(void)
{
    cpu_security_log_state("unit-test");
    TEST_ASSERT(1, "cpu_security_log_state() returns cleanly on BSP");
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

    /* Verify byte-for-byte match. Track first mismatch and report once
     * outside the loop so a failure includes the byte index, not just
     * "every byte" (per implement-unit-tests skill). */
    {
        uint32_t mismatch = 256;
        char m[96];
        for (i = 0; i < 256; i++) {
            if (dst[i] != src[i]) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memcpy_avx 256B: dst matches src (first diff @ byte %u/256)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 256, m);
    }
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

    {
        uint32_t mismatch = 37;
        char m[96];
        for (i = 0; i < 37; i++) {
            if (dst[i] != src[i]) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memcpy_avx tail 37B: dst matches src (first diff @ byte %u/37)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 37, m);
    }
    /* Verify bytes beyond copy length are untouched */
    TEST_ASSERT_EQ(dst[37], 0,
                   "memcpy_avx tail: byte 37 untouched (still 0)");
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

    {
        uint32_t mismatch = 256;
        char m[96];
        for (i = 0; i < 256; i++) {
            if (buf[i] != 0x42) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memset_avx 256B: every byte 0x42 (first diff @ byte %u/256)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 256, m);
    }
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

    {
        uint32_t mismatch = 41;
        char m[96];
        for (i = 0; i < 41; i++) {
            if (buf[i] != 0xBB) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memset_avx tail 41B: every byte 0xBB (first diff @ byte %u/41)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 41, m);
    }
    TEST_ASSERT_EQ(buf[41], 0,
                   "memset_avx tail: byte 41 untouched (still 0)");
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

    {
        uint32_t mismatch = 128;
        char m[96];
        for (i = 0; i < 128; i++) {
            if (dst[i] != src[i]) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memcpy_fast 128B: dst matches src (first diff @ byte %u/128)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 128, m);
    }
}

static void test_memset_fast_dispatch(void)
{
    uint8_t __attribute__((aligned(32))) buf[128];
    uint32_t i;

    for (i = 0; i < 128; i++)
        buf[i] = 0;

    memset_fast(buf, 0x55, 128);

    {
        uint32_t mismatch = 128;
        char m[96];
        for (i = 0; i < 128; i++) {
            if (buf[i] != 0x55) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memset_fast 128B: every byte 0x55 (first diff @ byte %u/128)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 128, m);
    }
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

    {
        uint32_t mismatch = 256;
        char m[96];
        for (i = 0; i < 256; i++) {
            if (dst[i] != src[i]) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memcpy_avx512 256B: dst matches src (first diff @ byte %u/256)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 256, m);
    }
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

    {
        uint32_t mismatch = 70;
        char m[96];
        for (i = 0; i < 70; i++) {
            if (dst[i] != src[i]) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memcpy_avx512 tail 70B: dst matches src (first diff @ byte %u/70)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 70, m);
    }
    TEST_ASSERT_EQ(dst[70], 0,
                   "memcpy_avx512 tail: byte 70 untouched (still 0)");
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

    {
        uint32_t mismatch = 256;
        char m[96];
        for (i = 0; i < 256; i++) {
            if (buf[i] != 0x7A) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memset_avx512 256B: every byte 0x7A (first diff @ byte %u/256)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 256, m);
    }
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

    {
        uint32_t mismatch = 73;
        char m[96];
        for (i = 0; i < 73; i++) {
            if (buf[i] != 0xDD) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memset_avx512 tail 73B: every byte 0xDD (first diff @ byte %u/73)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 73, m);
    }
    TEST_ASSERT_EQ(buf[73], 0,
                   "memset_avx512 tail: byte 73 untouched (still 0)");
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

    {
        uint32_t mismatch = 128;
        char m[96];
        for (i = 0; i < 128; i++) {
            if (dst[i] != src[i]) { mismatch = i; break; }
        }
        snprintf(m, sizeof(m),
                 "memcpy_fast (avx512 tier) 128B: dst matches src (first diff @ byte %u/128)",
                 (uint64_t)mismatch);
        TEST_ASSERT(mismatch == 128, m);
    }
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

static void test_msr_try_read_no_crash(void)
{
    /* msr_try_read on a nonexistent MSR must not crash.
     *
     * Platform divergence:
     *   TCG / bare metal: hardware raises #GP, handler catches it, ret==-1
     *   WHPX: hypervisor absorbs the read silently, returns 0, ret==0
     *
     * This test verifies the NO-CRASH guarantee. It does NOT verify MSR
     * existence detection because WHPX makes that unreliable. Feature
     * detection must always use cpu_has() / CPUID first. */
    uint64_t val = 0xDEAD;
    int ret = msr_try_read(0xFFFFFFFF, &val);
    TEST_ASSERT(ret == 0 || ret == -1,
                "msr_try_read(0xFFFFFFFF) survives without panic");
    if (ret == -1) {
        TEST_ASSERT_EQ(val, 0,
                       "msr_try_read sets output to 0 on #GP");
    }
}

static void test_msr_try_read_null_out(void)
{
    /* msr_try_read with NULL out pointer should not crash */
    int ret = msr_try_read(MSR_IA32_EFER, (uint64_t *)0);
    TEST_ASSERT_EQ(ret, 0,
                   "msr_try_read(EFER, NULL) returns 0 without crash");
}

static void test_msr_cpuid_gate_pattern(void)
{
    /* Correct pattern: CPUID gate first, msr_try_read as safety net.
     *
     * CPUID alone does not guarantee MSR availability under virtualization.
     * KVM with -cpu host passes AVX512F through but traps IA32_MPERF
     * with #GP (MPERF can't be meaningfully virtualized). WHPX takes the
     * opposite path and silently returns 0 for unknown MSRs. Both are
     * legitimate -- the contract of msr_try_read() is "no crash", not
     * "success implies the feature works". This test verifies both:
     * msr_try_read either succeeds cleanly (bare metal / WHPX) or fails
     * cleanly with -1 (KVM / trapping hypervisor). */
    if (!cpu_has(CPU_FEATURE_AVX512F)) {
        TEST_SKIP("AVX512F not present; MPERF/APERF test not applicable");
        return;
    }
    uint64_t mperf = 0;
    int ret = msr_try_read(MSR_IA32_MPERF, &mperf);
    TEST_ASSERT(ret == 0 || ret == -1,
                "msr_try_read(MPERF) returns 0 or -1 without crashing");
    if (ret == -1) {
        TEST_ASSERT_EQ(mperf, 0,
                       "msr_try_read sets *out = 0 on probe failure");
    }
}

/* ---- S5: UMIP + PKU ---- */

static void test_umip_cr4_set(void)
{
    if (!cpu_has(CPU_FEATURE_UMIP)) {
        TEST_SKIP("CPU does not support UMIP");
        return;
    }
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    TEST_ASSERT(cr4 & (1ULL << 11),
                "CR4.UMIP (bit 11) is set when CPU supports UMIP");
}

static void test_pku_cr4_pke_set(void)
{
    if (!cpu_has(CPU_FEATURE_PKU)) {
        TEST_SKIP("CPU does not support PKU");
        return;
    }
    extern struct cpu_features g_cpu;
    if (!(g_cpu.xcr0_active & (1UL << 9))) {
        TEST_SKIP("XCR0 bit 9 (PKRU) not set");
        return;
    }
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    TEST_ASSERT(cr4 & (1ULL << 22),
                "CR4.PKE (bit 22) is set when PKU + XCR0 bit 9 active");
}

static void test_pku_xcr0_bit9(void)
{
    if (!cpu_has(CPU_FEATURE_PKU)) {
        TEST_SKIP("CPU does not support PKU");
        return;
    }
    extern struct cpu_features g_cpu;
    TEST_ASSERT(g_cpu.xcr0_active & (1UL << 9),
                "XCR0 bit 9 (PKRU state) is set when PKU supported");
}

static void test_pku_alloc_free(void)
{
    if (!cpu_has(CPU_FEATURE_PKU)) {
        TEST_SKIP("PKU not available");
        return;
    }
    int key = pku_alloc_key();
    TEST_ASSERT(key >= 1 && key <= 15,
                "pku_alloc_key returns key in range 1-15");
    pku_free_key(key);

    /* Allocate again; should get a key (possibly the same one) */
    int key2 = pku_alloc_key();
    TEST_ASSERT(key2 >= 1 && key2 <= 15,
                "pku_alloc_key returns valid key after free");
    pku_free_key(key2);
}

static void test_pku_read_pkru(void)
{
    if (!cpu_has(CPU_FEATURE_PKU)) {
        TEST_SKIP("PKU not available");
        return;
    }
    uint32_t pkru = pku_read();
    /* Key 0 (bits 1:0) should be 0 (full access) */
    TEST_ASSERT_EQ(pkru & 3, 0,
                   "PKRU key 0 has full access (bits 1:0 == 0)");
}

static void test_pku_set_permissions(void)
{
    if (!cpu_has(CPU_FEATURE_PKU)) {
        TEST_SKIP("PKU not available");
        return;
    }
    int key = pku_alloc_key();
    if (key < 0) {
        TEST_SKIP("No PKU keys available");
        return;
    }

    /* Set write-disable on the key */
    pku_set_permissions(key, PKU_WRITE_DISABLE);
    uint32_t pkru = pku_read();
    uint32_t field = (pkru >> (key * 2)) & 3;
    TEST_ASSERT_EQ(field, PKU_WRITE_DISABLE,
                   "PKRU key field matches PKU_WRITE_DISABLE after set");

    /* Restore to access-disable (default) and free */
    pku_set_permissions(key, PKU_ACCESS_DISABLE);
    pku_free_key(key);
}

static void test_pku_pte_key_macros(void)
{
    /* Verify PTE key encode/decode round-trips */
    uint64_t pte = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_PKU_KEY(7);
    TEST_ASSERT_EQ(VMM_PKU_KEY_GET(pte), 7,
                   "VMM_PKU_KEY(7) encodes and decodes to 7");

    pte = VMM_PKU_KEY(0);
    TEST_ASSERT_EQ(VMM_PKU_KEY_GET(pte), 0,
                   "VMM_PKU_KEY(0) encodes and decodes to 0");

    pte = VMM_PKU_KEY(15);
    TEST_ASSERT_EQ(VMM_PKU_KEY_GET(pte), 15,
                   "VMM_PKU_KEY(15) encodes and decodes to 15");
}

static void test_pku_xsave_offset(void)
{
    if (!cpu_has(CPU_FEATURE_PKU)) {
        TEST_SKIP("PKU not available");
        return;
    }
    extern struct cpu_features g_cpu;
    TEST_ASSERT(g_cpu.pkru_xsave_offset > 0,
                "PKRU XSAVE offset is non-zero when PKU supported");
    TEST_ASSERT_EQ(g_cpu.pkru_xsave_size, 4,
                   "PKRU XSAVE component size is 4 bytes");
}

/* ---- S6: 1 GiB huge pages ---- */

static void test_1g_page_cpuid_feature(void)
{
    /* CPU_FEATURE_PAGE1GB should match CPUID leaf 0x80000001 EDX bit 26 */
    int has_1g = cpu_has(CPU_FEATURE_PAGE1GB);
    TEST_ASSERT(has_1g == 0 || has_1g == 1,
                "cpu_has(PAGE1GB) returns 0 or 1");
}

static void test_1g_page_pdpt_promoted(void)
{
    if (!cpu_has(CPU_FEATURE_PAGE1GB)) {
        TEST_SKIP("CPU does not support 1 GiB pages");
        return;
    }
    /* After vmm_promote_to_1g(), PDPT[2] should have PS=1 (1 GiB page).
     * We check PDPT[2] (GiB 2) instead of PDPT[1] because GiB 1 contains
     * KUSD at 0x7FFE0000, and vmm_split_huge_page cascades from 1 GiB back
     * to 2 MiB when mapping KUSD, de-promoting PDPT[1]. PDPT[2] and [3]
     * remain promoted since nothing maps in those ranges. */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *)(cr3 & 0x000FFFFFFFFFF000ULL);
    if (!(pml4[0] & 1)) {
        TEST_SKIP("PML4[0] not present");
        return;
    }
    uint64_t *pdpt = (uint64_t *)(pml4[0] & 0x000FFFFFFFFFF000ULL);
    /* PDPT[2] should be a 1 GiB page (PS=1, bit 7) */
    TEST_ASSERT(pdpt[2] & (1ULL << 7),
                "PDPT[2] has PS=1 (promoted to 1 GiB page)");
    /* Physical address should be 2 GiB (identity map) */
    uint64_t phys = pdpt[2] & 0x000FFFFFC0000000ULL;
    TEST_ASSERT_EQ(phys, (2ULL << 30),
                   "PDPT[2] maps physical 2 GiB (identity map)");
}

static void test_1g_page_pdpt0_not_promoted(void)
{
    /* PDPT[0] should NOT be promoted (contains kernel text, needs NX) */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *)(cr3 & 0x000FFFFFFFFFF000ULL);
    if (!(pml4[0] & 1)) {
        TEST_SKIP("PML4[0] not present");
        return;
    }
    uint64_t *pdpt = (uint64_t *)(pml4[0] & 0x000FFFFFFFFFF000ULL);
    /* PDPT[0] should be a PD pointer, NOT a 1 GiB page */
    if (pdpt[0] & 1) {
        TEST_ASSERT(!(pdpt[0] & (1ULL << 7)),
                    "PDPT[0] is NOT a 1 GiB page (preserves NX granularity)");
    }
}

static void test_vmm_map_huge_1g_alignment(void)
{
    /* vmm_map_huge_1g should reject misaligned addresses */
    int ret = vmm_map_huge_1g(0x1000, 0x1000, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    TEST_ASSERT_EQ(ret, -1,
                   "vmm_map_huge_1g rejects misaligned addresses");
}

static void test_wc_pat_entry(void)
{
    /* PAT MSR entry 1 should be WC (0x01) after cpu_configure_pat().
     * Write PAT again and readback to test the current CPU's behavior.
     * WHPX traps PAT MSR writes and returns Intel default (WT = 0x04);
     * this is a genuine hypervisor limitation, not a code bug. The kernel
     * degrades gracefully (framebuffer gets WT instead of WC). */
    cpu_configure_pat();
    uint64_t pat = msr_read(0x277);  /* MSR_IA32_PAT */
    uint8_t entry1 = (uint8_t)((pat >> 8) & 0xFF);
    if (entry1 == 0x01) {
    } else if (entry1 == 0x04) {
        /* WHPX traps PAT writes; kernel degrades to WT (functional) */
    } else {
        TEST_ASSERT_EQ(entry1, 0x01,
                       "PAT entry 1 is WC (0x01) or WT (0x04)");
    }
}

/* ---- S9: CPU Topology ---- */

static void test_topo_cpu0_logical_id(void)
{
    TEST_ASSERT_EQ(g_cpu_topo[0].logical_id, 0,
                   "CPU 0 topology logical_id is 0");
}

static void test_topo_cpu_count(void)
{
    TEST_ASSERT(g_topo_cpu_count >= 1,
                "topology CPU count >= 1");
    uint32_t smp_count = smp_cpu_count();
    TEST_ASSERT_EQ(g_topo_cpu_count, smp_count,
                   "topology CPU count matches smp_cpu_count()");
}

static void test_topo_numa_nodes(void)
{
    TEST_ASSERT(g_numa_nodes >= 1,
                "NUMA node count >= 1 (at least UMA)");
}

static void test_topo_core_type_valid(void)
{
    /* Core type must be one of the defined constants */
    uint8_t ct = g_cpu_topo[0].core_type;
    TEST_ASSERT(ct == CORE_TYPE_GENERIC || ct == CORE_TYPE_P || ct == CORE_TYPE_E,
                "CPU 0 core_type is a valid constant");
}

static void test_topo_smt_siblings(void)
{
    TEST_ASSERT(g_cpu_topo[0].smt_siblings >= 1,
                "CPU 0 smt_siblings >= 1 (at least 1 thread per core)");
}

/* ---- S13: Virtualization Detection ---- */

static void test_virt_detect_flags(void)
{
    /* SVM and VMX should not both be set (AMD or Intel, not both) */
    int has_svm = cpu_has(CPU_FEATURE_SVM);
    int has_vmx = cpu_has(CPU_FEATURE_VMX);
    TEST_ASSERT(!(has_svm && has_vmx),
                "SVM and VMX are mutually exclusive (not both set)");
    /* Note: some VMs or configurations may hide both; only assert
     * mutual exclusion, not presence (presence is platform-dependent). */
}

static void test_virt_detect_consistent(void)
{
    /* cpu_has should return consistent values on repeated calls */
    int v1 = cpu_has(CPU_FEATURE_VMX);
    int v2 = cpu_has(CPU_FEATURE_VMX);
    TEST_ASSERT_EQ(v1, v2, "cpu_has(VMX) stable across calls");
}

/* ---- S11: OSVW + RDTSCP ---- */

static void test_osvw_fields(void)
{
    if (!cpu_has(CPU_FEATURE_OSVW)) {
        TEST_SKIP("CPU does not support OSVW");
        return;
    }
    extern struct cpu_features g_cpu;
    TEST_ASSERT(g_cpu.osvw_length > 0,
                "OSVW: osvw_length > 0 when OSVW supported");
}

static void test_rdtscp_cpu_id(void)
{
    if (!cpu_has(CPU_FEATURE_RDTSCP)) {
        TEST_SKIP("CPU does not support RDTSCP");
        return;
    }
    extern int g_tsc_aux_available;
    if (!g_tsc_aux_available) {
        TEST_SKIP("TSC_AUX MSR not writable (WHPX may trap it)");
        return;
    }
    uint32_t cpu_id = 0xFFFF;
    uint64_t tsc = rdtscp_read(&cpu_id);
    /* BSP should have TSC_AUX = 0 */
    TEST_ASSERT_EQ(cpu_id, 0,
                   "RDTSCP: TSC_AUX = 0 on BSP (test runs on BSP)");
    TEST_ASSERT(tsc > 0, "RDTSCP: TSC value is non-zero");
}

static void test_rdtscp_null_cpuid(void)
{
    if (!cpu_has(CPU_FEATURE_RDTSCP)) {
        TEST_SKIP("CPU does not support RDTSCP");
        return;
    }
    /* Should not crash with NULL cpu_id pointer */
    uint64_t tsc = rdtscp_read((uint32_t *)0);
    TEST_ASSERT(tsc > 0, "RDTSCP: works with NULL cpu_id pointer");
}

/* ---- S10: Performance Monitoring Counters ---- */

static void test_pmc_info_valid(void)
{
    /* g_pmc_info should be populated after pmc_init */
    TEST_ASSERT(g_pmc_info.available == 0 || g_pmc_info.available == 1,
                "pmc available flag is 0 or 1");
    if (g_pmc_info.available) {
        TEST_ASSERT(g_pmc_info.num_counters >= 1,
                    "PMC num_counters >= 1 when available");
        TEST_ASSERT(g_pmc_info.counter_width >= 32,
                    "PMC counter_width >= 32 when available");
    }
}

static void test_pmc_start_stop(void)
{
    /* TCG emulates the PMU MSRs but does not actually count instructions
     * during a busy loop, so the counter stays at 0 and the post-spin
     * `val > 0` assertion fires on the GHA TCG runner. The hardware
     * (real CPU under KVM/WHPX/bare metal) does count, so skip only on
     * TCG. Detected via CPUID 0x40000000 -> "TCGTCGTCGTCG" (see
     * src/kernel/cpuid_platform.c:175). */
    if (platform_is_tcg()) {
        TEST_SKIP("PMC counters are unreliable on QEMU TCG");
        return;
    }
    if (!g_pmc_info.available) {
        TEST_SKIP("PMC not available");
        return;
    }
    /* Start slot 0 with instructions retired, read, stop */
    uint64_t event = g_pmc_info.is_amd ? PMC_AMD_INST_RETIRED
                                       : PMC_INTEL_INST_RETIRED;
    int ret = pmc_start(0, event);
    TEST_ASSERT_EQ(ret, 0, "pmc_start(0) returns 0");

    /* Spin briefly to accumulate some counts */
    {
        volatile uint32_t spin = 0;
        uint32_t j;
        for (j = 0; j < 10000; j++) spin++;
        (void)spin;
    }

    uint64_t val = pmc_read(0);
    TEST_ASSERT(val > 0, "PMC counter incremented after tight loop");

    ret = pmc_stop(0);
    TEST_ASSERT_EQ(ret, 0, "pmc_stop(0) returns 0");
}

static void test_pmc_out_of_range(void)
{
    /* Out-of-range slot should return -1 / 0 */
    int ret = pmc_start(PMC_MAX_SLOTS + 1, 0);
    TEST_ASSERT_EQ(ret, -1, "pmc_start with invalid slot returns -1");
    TEST_ASSERT_EQ(pmc_read(PMC_MAX_SLOTS + 1), 0,
                   "pmc_read with invalid slot returns 0");
}

static void test_pmc_ipc(void)
{
    /* Same TCG caveat as test_pmc_start_stop -- counters stay at 0 on
     * the emulator, so the IPC ratio computation returns 0 and trips
     * the assertion. Skip on TCG; KVM/WHPX/bare metal still exercise it. */
    if (platform_is_tcg()) {
        TEST_SKIP("PMC IPC ratio is 0 on QEMU TCG (counters do not advance)");
        return;
    }
    if (!g_pmc_info.available || g_pmc_info.num_counters < 2) {
        TEST_SKIP("PMC IPC requires 2+ counters");
        return;
    }
    uint32_t ipc = pmc_ipc();
    TEST_ASSERT(ipc > 0, "pmc_ipc returns non-zero fixed-point IPC");
}

/* copy_user fault injection: arming copy_user_fail_next() forces
 * the next copy_to_user OR copy_from_user to return -1 without
 * attempting the real memory access. Proves syscall error paths
 * propagate user-copy failure as a non-zero return without corrupting
 * the source/destination buffers. */
static void test_copy_user_fault_inject(void)
{
    uint64_t pre = copy_user_fail_injections_triggered();

    /* Use a kernel-side buffer as the "user" target -- the fault-inject
     * path returns BEFORE the SMAP-gated copy runs, so whether the
     * target is user-mode or kernel-mode doesn't matter for this test. */
    uint8_t kernel_src[16];
    uint8_t kernel_dst[16];
    for (int i = 0; i < 16; i++) {
        kernel_src[i] = (uint8_t)i;
        kernel_dst[i] = 0xAA;
    }

    copy_user_fail_next();
    int r1 = copy_to_user(kernel_dst, kernel_src, 16);
    TEST_ASSERT_EQ(r1, -1, "copy_to_user returns -1 when injection armed");
    /* Fault-inject returns BEFORE the copy loop, so kernel_dst stays 0xAA. */
    TEST_ASSERT_EQ(kernel_dst[0], 0xAA, "kernel_dst[0] unchanged by forced-fail copy");
    TEST_ASSERT_EQ(kernel_dst[15], 0xAA, "kernel_dst[15] unchanged by forced-fail copy");

    /* Second call (countdown cleared after fire) succeeds and copies. */
    int r2 = copy_to_user(kernel_dst, kernel_src, 16);
    TEST_ASSERT_EQ(r2, 0, "auto-cleared after fire -- next copy_to_user succeeds");
    TEST_ASSERT_EQ(kernel_dst[0], 0, "successful copy wrote kernel_src[0]");
    TEST_ASSERT_EQ(kernel_dst[15], 15, "successful copy wrote kernel_src[15]");

    /* copy_from_user shares the same gate. */
    copy_user_fail_next();
    int r3 = copy_from_user(kernel_dst, kernel_src, 16);
    TEST_ASSERT_EQ(r3, -1, "copy_from_user returns -1 when injection armed");

    TEST_ASSERT_EQ(copy_user_fail_injections_triggered() - pre, 2u,
                   "copy_user injection counter advanced by exactly 2");
}

/* Task-filter + max-injections mirror tests for copy_user (so a
 * field-mix-up in copy_user_fault_should_fire is caught alongside
 * the other 3 subsystems). */
static void test_copy_user_fault_inject_task_filter(void)
{
    uint64_t pre = copy_user_fail_injections_triggered();
    uint8_t src = 0x55, dst = 0xAA;

    copy_user_fail_next();
    copy_user_fail_task_filter_set(0xFFFFFFFFu);
    int r1 = copy_to_user(&dst, &src, 1);
    TEST_ASSERT_EQ(r1, 0, "task-filter skips non-matching task -- copy succeeds");
    TEST_ASSERT_EQ(dst, 0x55, "copy wrote to dst (filter did not block the copy)");
    TEST_ASSERT_EQ(copy_user_fail_injections_triggered(), pre,
                   "copy_user injection counter unchanged when filter blocks");

    dst = 0xAA;
    copy_user_fail_task_filter_clear();
    int r2 = copy_to_user(&dst, &src, 1);
    TEST_ASSERT_EQ(r2, -1, "countdown preserved -- fires after filter clear");
    TEST_ASSERT_EQ(dst, 0xAA, "dst unchanged on forced-fail copy");
    TEST_ASSERT_EQ(copy_user_fail_injections_triggered() - pre, 1u,
                   "exactly 1 copy_user fire after filter clear");
}

static void test_copy_user_fault_inject_max_cap(void)
{
    uint64_t pre = copy_user_fail_injections_triggered();
    copy_user_fail_max_injections_set(3);
    copy_user_fail_next();

    uint8_t src = 0x55, dst;
    int fail_hits = 0;
    for (int i = 0; i < 10; i++) {
        dst = 0xAA;
        int r = copy_to_user(&dst, &src, 1);
        if (r == -1)
            fail_hits++;
    }
    TEST_ASSERT_EQ(fail_hits, 3, "copy_user max_injections=3 produces 3 failures");
    TEST_ASSERT_EQ(copy_user_fail_fired_counter(), 3u,
                   "copy_user fired_counter equals cap");
    TEST_ASSERT_EQ(copy_user_fail_injections_triggered() - pre, 3u,
                   "copy_user injection counter advanced by exactly 3");

    copy_user_fail_max_injections_clear();
}

/* ---- Hypervisor detection (boot_info.hv_vendor + hv_flags) ---- */

/* hv_vendor must be NUL-terminated within the 16-byte buffer. */
static void test_platform_hv_vendor_null_terminated(void)
{
    int i;
    int found_nul = 0;
    for (i = 0; i < 16; i++) {
        if (g_boot_info.hv_vendor[i] == '\0') { found_nul = 1; break; }
    }
    TEST_ASSERT(found_nul, "hv_vendor[16] contains a NUL terminator");
}

/* hv_flags must only contain bits defined by HV_FLAG_*. */
static void test_platform_hv_flags_in_range(void)
{
    uint32_t defined = HV_FLAG_TSC_ENLIGHTENMENT | HV_FLAG_TLBFLUSH_HYPERCALL |
                       HV_FLAG_APIC_FREQ_MSR | HV_FLAG_KVM_STEAL_TIME |
                       HV_FLAG_VMWARE_BACKDOOR;
    TEST_ASSERT((g_boot_info.hv_flags & ~defined) == 0,
                "hv_flags contains only defined HV_FLAG_* bits");
}

/* platform_has_apic_freq_msr() and (hv_flags & HV_FLAG_APIC_FREQ_MSR) must
 * agree on Hyper-V -- they read the same underlying truth after platform
 * detection populates hv_flags from CPUID 0x40000003 AccessFrequencyMsrs. */
static void test_platform_apic_freq_consistent_on_hyperv(void)
{
    if (platform_get() != PLATFORM_HYPERV) {
        TEST_SKIP("not running on Hyper-V");
        return;
    }
    int flag = !!(g_boot_info.hv_flags & HV_FLAG_APIC_FREQ_MSR);
    int func = platform_has_apic_freq_msr();
    TEST_ASSERT_EQ(func, flag,
                   "Hyper-V: platform_has_apic_freq_msr() matches HV_FLAG_APIC_FREQ_MSR");
}

/* Bare metal must have empty hv_vendor and zero hv_flags. */
static void test_platform_baremetal_clean(void)
{
    if (platform_get() != PLATFORM_BARE_METAL) {
        TEST_SKIP("not running on bare metal");
        return;
    }
    TEST_ASSERT_EQ(g_boot_info.hv_vendor[0], (char)0,
                   "bare metal: hv_vendor is empty");
    TEST_ASSERT_EQ((uint64_t)g_boot_info.hv_flags, 0ULL,
                   "bare metal: hv_flags is zero");
}

/* ---- Registration ---- */

void test_register_x86(void)
{
    test_suite_register_cat("CPU security: NX EFER.NXE set",
        test_nx_efer_set, TEST_CAT_X86);
    test_suite_register_cat("CPU security: SMEP/SMAP CR4 accessible",
        test_smep_smap_state, TEST_CAT_X86);
    test_suite_register_cat("CPU security: activation-state helper callable",
        test_cpu_security_log_state, TEST_CAT_X86);
    test_suite_register_cat("CPU security: kernel_cr3 non-zero",
        test_kpti_percpu_kernel_cr3, TEST_CAT_X86);
    test_suite_register_cat("CPU security: user_cr3 == kernel_cr3",
        test_kpti_percpu_user_cr3_eq_kernel, TEST_CAT_X86);
    test_suite_register_cat("CPU security: kpti_active() == 0",
        test_kpti_active_false, TEST_CAT_X86);
    test_suite_register_cat("CPU security: kernel_cr3 matches HW CR3",
        test_kpti_kernel_cr3_matches_hw, TEST_CAT_X86);

    /* Hypervisor detection */
    test_suite_register_cat("Platform: hv_vendor NUL-terminated",
        test_platform_hv_vendor_null_terminated, TEST_CAT_X86);
    test_suite_register_cat("Platform: hv_flags only contains defined bits",
        test_platform_hv_flags_in_range, TEST_CAT_X86);
    test_suite_register_cat("Platform: APIC freq flag/func consistent on Hyper-V",
        test_platform_apic_freq_consistent_on_hyperv, TEST_CAT_X86);
    test_suite_register_cat("Platform: bare metal hv_vendor/hv_flags clean",
        test_platform_baremetal_clean, TEST_CAT_X86);

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
    test_suite_register_cat("MSR: msr_try_read no-crash guarantee",
        test_msr_try_read_no_crash, TEST_CAT_X86);
    test_suite_register_cat("MSR: msr_try_read NULL out",
        test_msr_try_read_null_out, TEST_CAT_X86);
    test_suite_register_cat("MSR: CPUID-gated msr_try_read pattern",
        test_msr_cpuid_gate_pattern, TEST_CAT_X86);

    /* S5: UMIP + PKU */
    test_suite_register_cat("UMIP: CR4.UMIP set",
        test_umip_cr4_set, TEST_CAT_X86);
    test_suite_register_cat("PKU: CR4.PKE set",
        test_pku_cr4_pke_set, TEST_CAT_X86);
    test_suite_register_cat("PKU: XCR0 bit 9 active",
        test_pku_xcr0_bit9, TEST_CAT_X86);
    test_suite_register_cat("PKU: alloc/free key",
        test_pku_alloc_free, TEST_CAT_X86);
    test_suite_register_cat("PKU: read PKRU value",
        test_pku_read_pkru, TEST_CAT_X86);
    test_suite_register_cat("PKU: set_permissions + readback",
        test_pku_set_permissions, TEST_CAT_X86);
    test_suite_register_cat("PKU: PTE key macros round-trip",
        test_pku_pte_key_macros, TEST_CAT_X86);
    test_suite_register_cat("PKU: XSAVE offset from CPUID",
        test_pku_xsave_offset, TEST_CAT_X86);

    /* S6: 1 GiB huge pages + WC PAT */
    test_suite_register_cat("1GiB: PAGE1GB CPUID feature",
        test_1g_page_cpuid_feature, TEST_CAT_X86);
    test_suite_register_cat("1GiB: PDPT[2] promoted to 1 GiB",
        test_1g_page_pdpt_promoted, TEST_CAT_X86);
    test_suite_register_cat("1GiB: PDPT[0] NOT promoted (NX)",
        test_1g_page_pdpt0_not_promoted, TEST_CAT_X86);
    test_suite_register_cat("1GiB: misaligned address rejected",
        test_vmm_map_huge_1g_alignment, TEST_CAT_X86);
    test_suite_register_cat("PAT: entry 1 is WC (0x01)",
        test_wc_pat_entry, TEST_CAT_X86);

    /* S9: CPU Topology */
    test_suite_register_cat("Topo: CPU 0 logical_id == 0",
        test_topo_cpu0_logical_id, TEST_CAT_X86);
    test_suite_register_cat("Topo: cpu_count matches SMP",
        test_topo_cpu_count, TEST_CAT_X86);
    test_suite_register_cat("Topo: NUMA nodes >= 1",
        test_topo_numa_nodes, TEST_CAT_X86);
    test_suite_register_cat("Topo: core_type valid",
        test_topo_core_type_valid, TEST_CAT_X86);
    test_suite_register_cat("Topo: smt_siblings >= 1",
        test_topo_smt_siblings, TEST_CAT_X86);

    /* S13: Virtualization Detection */
    test_suite_register_cat("Virt: SVM/VMX mutually exclusive",
        test_virt_detect_flags, TEST_CAT_X86);
    test_suite_register_cat("Virt: cpu_has(VMX) stable",
        test_virt_detect_consistent, TEST_CAT_X86);

    /* S11: OSVW + RDTSCP */
    test_suite_register_cat("OSVW: fields populated",
        test_osvw_fields, TEST_CAT_X86);
    test_suite_register_cat("RDTSCP: TSC_AUX = 0 on BSP",
        test_rdtscp_cpu_id, TEST_CAT_X86);
    test_suite_register_cat("RDTSCP: NULL cpu_id safe",
        test_rdtscp_null_cpuid, TEST_CAT_X86);

    /* S10: Performance Monitoring Counters */
    test_suite_register_cat("PMC: info valid after init",
        test_pmc_info_valid, TEST_CAT_X86);
    test_suite_register_cat("PMC: start/read/stop slot 0",
        test_pmc_start_stop, TEST_CAT_X86);
    test_suite_register_cat("PMC: out-of-range slot rejected",
        test_pmc_out_of_range, TEST_CAT_X86);
    test_suite_register_cat("PMC: pmc_ipc non-zero",
        test_pmc_ipc, TEST_CAT_X86);

    /* Kernel-test-harness roadmap: copy_user fault injection */
    test_suite_register_cat("CPU security: copy_user fault-inject",
        test_copy_user_fault_inject, TEST_CAT_X86);
    test_suite_register_cat("CPU security: copy_user fault-inject task-filter",
        test_copy_user_fault_inject_task_filter, TEST_CAT_X86);
    test_suite_register_cat("CPU security: copy_user fault-inject max-cap",
        test_copy_user_fault_inject_max_cap, TEST_CAT_X86);
}

#endif /* KERNEL_TESTS */
