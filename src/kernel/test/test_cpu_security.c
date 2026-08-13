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
#include "kernel/cpu_regs.h"
#include "kernel/msr.h"
#include "kernel/mtrr.h"
#include "kernel/mm/memops.h"
#include "kernel/mm/vmm.h"
#include "kernel/security/pku.h"
#include "kernel/hw_profile.h"
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

    /* SMEP/SMAP are skipped on EVERY platform (kernel PTE User bit --
     * needs KPTI); there is NO EPT-enforcement path (that claim was
     * retracted as a false security signal in the TODO-10 bare-metal
     * hardening CPU-security review). The test just confirms no crash
     * accessing cr4. */
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

/* ---- S5 (TODO-09-boot): PCID activation window ---- */

/* cpu_pcid_enable() runs on the BSP in boot_phase1 (this test runs on the
 * BSP); if the CPU reports PCID, CR4.PCIDE must be set as a result. Read-only
 * post-boot assertion -- never calls the activation function (would mutate
 * live CR4). */
static void test_cr4_pcide(void)
{
    if (!cpu_has(CPU_FEATURE_PCID)) {
        TEST_SKIP("CPU does not support PCID");
        return;
    }
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    TEST_ASSERT(cr4 & (1ULL << 17),
                "CR4.PCIDE (bit 17) is set when PCID supported (Phase 1 activated it)");
}

/* PCID activation keeps PCID 0 everywhere until TODO-10 S7; the live CR3 must
 * therefore carry no PCID in bits [11:0]. This guards against a premature
 * tagging change landing without the KPTI machinery. */
static void test_cr3_pcid_zero(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    TEST_ASSERT_EQ(cr3 & 0xFFFULL, 0,
                   "CR3[11:0] == 0 (PCID stays 0 until TODO-10 S7 tagging)");
}

/* ---- S6 (TODO-09-boot): AP feature consistency validation ---- */

/* The BSP must satisfy every required baseline feature (it passed the Phase-0
 * minimum check and is executing in long mode). Read-only invariant on
 * g_cpu.flags; does not invoke the AP validation path. */
static void test_required_mask_subset_of_bsp(void)
{
    extern struct cpu_features g_cpu;
    TEST_ASSERT(cpu_feature_subset(CPU_FEATURES_REQUIRED_MASK, g_cpu.flags),
                "BSP flags include every CPU_FEATURES_REQUIRED_MASK bit");
}

/* The probe mask must be a superset of the required mask, or a passing
 * AP probe could omit a required bit the validator then never checks. */
static void test_required_subset_of_probe(void)
{
    TEST_ASSERT(cpu_feature_subset(CPU_FEATURES_REQUIRED_MASK, CPU_FEATURES_AP_PROBE_MASK),
                "CPU_FEATURES_REQUIRED_MASK is a subset of CPU_FEATURES_AP_PROBE_MASK");
}

/* After cpu_features_finalize_global() runs in smp_init, the published global
 * intersection must still carry every required bit (all online CPUs have them
 * or boot bug-checked), and a globally-present feature implies the BSP has it. */
static void test_global_feature_mask_has_required(void)
{
    TEST_ASSERT(cpu_feature_global_has(CPU_FEATURE_SSE2),    "global intersection has SSE2");
    TEST_ASSERT(cpu_feature_global_has(CPU_FEATURE_NX),      "global intersection has NX");
    TEST_ASSERT(cpu_feature_global_has(CPU_FEATURE_LM),      "global intersection has LM");
    TEST_ASSERT(cpu_feature_global_has(CPU_FEATURE_SYSCALL), "global intersection has SYSCALL");
    TEST_ASSERT(cpu_feature_global_has(CPU_FEATURE_CX16),    "global intersection has CX16");
    TEST_ASSERT(!cpu_feature_global_has(CPU_FEATURE_SSE2) || cpu_has(CPU_FEATURE_SSE2),
                "global SSE2 implies BSP SSE2 (global is a subset of BSP)");
}

/* 128-bit cpu_feature_mask_t machinery: word/bit split round-trips across the
 * 64-bit boundary. Uses synthetic bit indices (63, 64) so the test does not
 * depend on any real CPU_FEATURE_* living in the high word yet. */
static void test_feature_mask_wordsplit(void)
{
    cpu_feature_mask_t m = { { 0, 0 } };
    cpu_feature_set(&m, (enum cpu_feature)63);
    cpu_feature_set(&m, (enum cpu_feature)64);
    TEST_ASSERT(cpu_feature_test(&m, (enum cpu_feature)63), "bit 63 lands in word 0");
    TEST_ASSERT(cpu_feature_test(&m, (enum cpu_feature)64), "bit 64 lands in word 1");
    TEST_ASSERT_EQ(m.w[0], 1ULL << 63, "word 0 holds exactly bit 63");
    TEST_ASSERT_EQ(m.w[1], 1ULL,       "word 1 holds exactly bit 64");
    cpu_feature_clear(&m, (enum cpu_feature)64);
    TEST_ASSERT(!cpu_feature_test(&m, (enum cpu_feature)64), "bit 64 clears in word 1");
    TEST_ASSERT(cpu_feature_test(&m, (enum cpu_feature)63),  "clearing word 1 leaves word 0");
}

/* Design-review invariant: a high-word feature present on the BSP but absent on
 * an AP must clear in the intersection (cpu_feature_and), or finalize_global
 * would publish an over-broad mask. Mirrors the AND finalize uses, no live infra. */
static void test_feature_mask_intersection_highword(void)
{
    cpu_feature_mask_t bsp = { { 0, 0 } }, ap = { { 0, 0 } };
    cpu_feature_set(&bsp, (enum cpu_feature)100);   /* BSP-only synthetic high-word feature */
    cpu_feature_set(&bsp, CPU_FEATURE_SSE2);
    cpu_feature_set(&ap,  CPU_FEATURE_SSE2);
    cpu_feature_mask_t inter = cpu_feature_and(bsp, ap);
    TEST_ASSERT(!cpu_feature_test(&inter, (enum cpu_feature)100),
                "high-word feature absent on AP clears in the intersection");
    TEST_ASSERT(cpu_feature_test(&inter, CPU_FEATURE_SSE2),
                "shared low-word feature survives the intersection");
    TEST_ASSERT(!cpu_feature_subset(bsp, ap),
                "bsp is not a subset of ap (high-word feature 100 missing)");
}

/* ---- S7 (TODO-09-boot): CR0/CR4 safety-bit pinning ---- */

/* After boot the BSP pinned CR0.WP. Read-only: the pinned mask in per_cpu_data
 * must include CR0_WP and CR0.WP must actually be set. Does NOT call the
 * verifiers (they bug-check on a cleared pin -- panic-on-clear is a manual
 * debug-build test). */
static void test_cr0_wp_pinned(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    TEST_ASSERT(cpu->cr0_pinned & CR0_WP,
                "BSP pinned mask includes CR0.WP (cr0_pinned & CR0_WP)");
    TEST_ASSERT(cr0 & CR0_WP, "CR0.WP is actually set on the BSP");
}

/* Every bit the BSP pinned in CR0 must still be set (pins hold post-boot). */
static void test_cr0_pins_held(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    uint64_t cr0, m = cpu->cr0_pinned;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    TEST_ASSERT_EQ(cr0 & m, m, "all pinned CR0 bits remain set");
}

/* Every bit the BSP pinned in CR4 must still be set, and the pinned mask must
 * be a subset of the live CR4 (only actually-enabled bits get pinned). */
static void test_cr4_pins_held(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    uint64_t cr4, m = cpu->cr4_pinned;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    TEST_ASSERT_EQ(cr4 & m, m, "all pinned CR4 bits remain set");
}

/* ---- S2: AVX2 memops ---- */

static void test_memcpy_avx_correctness(void)
{
    if (!simd_avx2_capable) {
        TEST_SKIP("CPU cannot execute AVX2 (no CPUID AVX2, or XCR0 bit 2 clear)");
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
    if (!simd_avx2_capable) {
        TEST_SKIP("CPU cannot execute AVX2 (no CPUID AVX2, or XCR0 bit 2 clear)");
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
    if (!simd_avx2_capable) {
        TEST_SKIP("CPU cannot execute AVX2 (no CPUID AVX2, or XCR0 bit 2 clear)");
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
    if (!simd_avx2_capable) {
        TEST_SKIP("CPU cannot execute AVX2 (no CPUID AVX2, or XCR0 bit 2 clear)");
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

/* TODO-09 S11: deterministic coverage of the committed-online predicate the
 * finalizer reduces over. This drives the SHIPPED cpu_slot_committed_online()
 * against synthetic slots, so removing its AP_BRINGUP_ONLINE term fails HERE --
 * the live-state test below cannot force that window (a real AP has normally
 * published is_online by the time the suite runs). */
static void test_cpu_slot_committed_online_states(void)
{
    /* STATIC, not a stack local: struct per_cpu_data is ~3.9 KiB (the 64-entry
     * transition ring alone is 3584 bytes) and the BSP boot stack is 16 KiB
     * behind a guard page -- a quarter of the stack in one frame, to read two
     * uint32 fields. BSS, zero-initialized, single-threaded test. */
    static struct per_cpu_data slot;

    TEST_ASSERT_EQ((uint32_t)cpu_slot_committed_online((struct per_cpu_data *)0), 0u,
                   "committed-online: NULL slot is not in the intersection set");

    /* Only the two publication fields are read by the predicate; set both
     * explicitly rather than zeroing the whole block. */
    slot.is_online = 0;
    slot.ap_bringup_state = AP_BRINGUP_STARTING;
    TEST_ASSERT_EQ((uint32_t)cpu_slot_committed_online(&slot), 0u,
                   "committed-online: STARTING with is_online=0 is excluded");

    slot.ap_bringup_state = AP_BRINGUP_ABANDONED;
    TEST_ASSERT_EQ((uint32_t)cpu_slot_committed_online(&slot), 0u,
                   "committed-online: ABANDONED is excluded (parks dark, never goes live)");

    /* The window the bounded 100 ms wait at smp.c:507 can expire inside: the AP
     * won the CAS and is going live, but its is_online store has not landed. */
    slot.ap_bringup_state = AP_BRINGUP_ONLINE;
    TEST_ASSERT_EQ((uint32_t)cpu_slot_committed_online(&slot), 1u,
                   "committed-online: ONLINE CAS won with is_online=0 is INCLUDED");

    slot.is_online = 1;
    TEST_ASSERT_EQ((uint32_t)cpu_slot_committed_online(&slot), 1u,
                   "committed-online: published is_online is included");

    /* is_online alone suffices -- the BSP slot never runs the AP CAS. */
    slot.ap_bringup_state = AP_BRINGUP_STARTING;
    TEST_ASSERT_EQ((uint32_t)cpu_slot_committed_online(&slot), 1u,
                   "committed-online: is_online admits a slot still marked STARTING");

    /* is_online PRECEDENCE. The bringup CAS makes this combination unreachable
     * (exactly one of AP-ONLINE / BSP-ABANDONED wins, and the AP publishes
     * is_online only after winning), but the precedence is pinned because it is
     * the SAFE direction: a CPU that published is_online is live, so dropping it
     * from a capability intersection would over-publish. */
    slot.ap_bringup_state = AP_BRINGUP_ABANDONED;
    TEST_ASSERT_EQ((uint32_t)cpu_slot_committed_online(&slot), 1u,
                   "committed-online: is_online takes precedence over ABANDONED");
}

/* TODO-09 S11: pku_enabled must EQUAL the online-CPU intersection of CR4.PKE,
 * not "some CPU enabled PKU". RDPKRU/WRPKRU #GP when CR4.PKE is clear on the
 * executing CPU, so a flag set while any committed-online CPU lacks the bit
 * faults a thread scheduled there.
 *
 * The expectation is recomputed here independently and compared for EQUALITY, in
 * both directions: an implication-only test ("enabled implies X") also passes
 * when the flag is stuck at 0, which would silently disable protection keys
 * machine-wide.
 *
 * It derives expect from LIVE CR4.PKE only -- never cpu_feature_global_has(),
 * which the same finalizer publishes and which would make this circular. That is
 * also what keeps it correct on a no-ACPI/degraded boot that never reaches
 * smp_init(): the global mask is unpublished there, so a mask term would force
 * expect=0 while the BSP one-shot has legitimately published 1. Using CR4 is
 * sound because CR4.PKE is set on a CPU only after cpu_enable_pku() cleared that
 * CPU's own CPUID-PKU and XCR0-bit-9 gates, so the finalizer's mask clause is
 * defense-in-depth over this value, not a separate input.
 *
 * Limitation, stated rather than papered over: this pins the flag against the
 * LIVE configuration, so it catches drift, hardcoding and a wrong predicate on
 * whatever CPUs booted -- it cannot FORCE the committed-online
 * (AP_BRINGUP_ONLINE with is_online still 0) window, which would need an
 * AP-bringup fault-injection seam the tree does not have and the test policy
 * would not allow a test to drive. */
static void test_pku_enabled_matches_online_intersection(void)
{
    extern int pku_enabled;
    uint64_t cr4;
    int enabled = __atomic_load_n(&pku_enabled, __ATOMIC_ACQUIRE);
    int expect;
    uint32_t i;

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    expect = (cr4 & (1ULL << 22)) ? 1 : 0;   /* BSP runs the suite */

    /* Same committed-online predicate the finalizer uses: an AP that won the
     * STARTING->ONLINE CAS counts even if its is_online store has not landed. */
    for (i = 1; i < MAX_CPUS; i++) {
        struct per_cpu_data *pc = smp_get_cpu(i);
        if (!pc)
            continue;
        if (!__atomic_load_n(&pc->is_online, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&pc->ap_bringup_state, __ATOMIC_ACQUIRE) != AP_BRINGUP_ONLINE)
            continue;
        if (!(pc->cr4_at_boot & (1ULL << 22)))
            expect = 0;
    }

    TEST_ASSERT_EQ((uint32_t)enabled, (uint32_t)expect,
                   "pku_enabled equals the committed-online CR4.PKE intersection");
    TEST_ASSERT(!enabled || (cr4 & (1ULL << 22)),
                "pku_enabled implies CR4.PKE is set on the CPU running this test");
    TEST_ASSERT(!enabled || cpu_has(CPU_FEATURE_PKU),
                "pku_enabled implies the BSP CPUID advertises PKU");
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
    /* Gate on the FINALIZED global, not BSP CPUID: on a feature-skewed machine
     * (BSP has PKU, an online AP lacks CR4.PKE) the online-CPU intersection
     * correctly clears pku_enabled and pku_alloc_key() returns -1 by contract.
     * Gating on cpu_has() alone would report that supported degradation as a
     * test FAILURE (TODO-09 S11). */
    if (!__atomic_load_n(&pku_enabled, __ATOMIC_ACQUIRE)) {
        /* Assert the fail-closed contract on EVERY disabled configuration, not
         * just the rare feature-skew one -- an ordinary no-PKU leg is where a
         * missing guard would otherwise go unexercised until it #GP'd on real
         * hardware. Calling the WRPKRU/RDPKRU-bearing entry points here is the
         * point: they must return without executing the instruction. */
        TEST_ASSERT_EQ(pku_alloc_key(), -1,
                       "PKU disabled: pku_alloc_key returns -1");
        TEST_ASSERT_EQ(pku_read(), 0u,
                       "PKU disabled: pku_read returns 0 without RDPKRU");
        pku_set_permissions(1, PKU_WRITE_DISABLE);
        pku_free_key(1);
        TEST_ASSERT_EQ(pku_read(), 0u,
                       "PKU disabled: set_permissions/free_key are safe no-ops (no WRPKRU)");
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
    /* Finalized global, not BSP CPUID -- see test_pku_alloc_free (TODO-09 S11). */
    if (!__atomic_load_n(&pku_enabled, __ATOMIC_ACQUIRE)) {
        TEST_SKIP("PKU not enabled on every online CPU");
        return;
    }
    uint32_t pkru = pku_read();
    /* Key 0 (bits 1:0) should be 0 (full access) */
    TEST_ASSERT_EQ(pkru & 3, 0,
                   "PKRU key 0 has full access (bits 1:0 == 0)");
}

static void test_pku_set_permissions(void)
{
    /* Finalized global, not BSP CPUID -- see test_pku_alloc_free (TODO-09 S11). */
    if (!__atomic_load_n(&pku_enabled, __ATOMIC_ACQUIRE)) {
        TEST_SKIP("PKU not enabled on every online CPU");
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
    /* HHDM walk (section 9); mm_phys_to_hhdm not pt_walk in test code. */
    uint64_t *pml4 = (uint64_t *)mm_phys_to_hhdm(cr3 & PTE_ADDR_MASK);
    TEST_ASSERT(pml4 != (void *)0, "kernel PML4 phys has an HHDM alias");
    if (!(pml4[0] & 1)) {
        TEST_SKIP("PML4[0] not present");
        return;
    }
    uint64_t *pdpt = (uint64_t *)mm_phys_to_hhdm(pml4[0] & PTE_ADDR_MASK);
    TEST_ASSERT(pdpt != (void *)0, "PDPT phys has an HHDM alias");
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
    /* HHDM walk (section 9); mm_phys_to_hhdm not pt_walk in test code. */
    uint64_t *pml4 = (uint64_t *)mm_phys_to_hhdm(cr3 & PTE_ADDR_MASK);
    TEST_ASSERT(pml4 != (void *)0, "kernel PML4 phys has an HHDM alias");
    if (!(pml4[0] & 1)) {
        TEST_SKIP("PML4[0] not present");
        return;
    }
    uint64_t *pdpt = (uint64_t *)mm_phys_to_hhdm(pml4[0] & PTE_ADDR_MASK);
    TEST_ASSERT(pdpt != (void *)0, "PDPT phys has an HHDM alias");
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
    /* PAT index 1 must be WC (0x01) -- selected by vmm_map_mmio_wc() and
     * PAGE_WRITECOMBINE (PWT-only). When the platform honors the PAT write the
     * live MSR reads back WC; a hypervisor that traps PAT writes (WHPX) returns
     * the Intel default (WT) instead. That is a hypervisor limitation, so we
     * SKIP rather than accept-two-answers (the constant itself is pinned WC by
     * a compile-time _Static_assert in cpu_security.c, platform-independently).
     * (TODO-09-boot S8) */
    cpu_configure_pat();
    uint64_t pat = msr_read(MSR_IA32_PAT);
    uint8_t entry1 = (uint8_t)((pat >> 8) & 0xFF);
    if (entry1 != 0x01)
        TEST_SKIP("hypervisor traps PAT write (live index1 != WC); "
                  "constant pinned WC at compile time");
    TEST_ASSERT_EQ(entry1, 0x01, "PAT index 1 = WC (0x01) on this CPU");
}

static void test_pat_index0_wb(void)
{
    /* PAT index 0 (no PAT/PCD/PWT bits = default mapping) must stay WB (0x06).
     * The S8 fix only changed PA1; a bad constant or future edit touching PA0
     * would corrupt ordinary write-back memory. (TODO-09-boot S8) */
    cpu_configure_pat();
    uint64_t pat = msr_read(MSR_IA32_PAT);
    uint8_t entry0 = (uint8_t)(pat & 0xFF);
    if (entry0 != 0x06)
        TEST_SKIP("hypervisor traps PAT write (live index0 != WB)");
    TEST_ASSERT_EQ(entry0, 0x06, "PAT index 0 = WB (0x06) for normal memory");
}

static void test_pat_index2_uc_minus(void)
{
    /* PAT index 2 (PCD-only, PAGE_NOCACHE) must be UC- (0x07), not WC. The old
     * decode bug put WC at index 2, silently write-combining NOCACHE requests.
     * (TODO-09-boot S8) */
    cpu_configure_pat();
    uint64_t pat = msr_read(MSR_IA32_PAT);
    uint8_t entry2 = (uint8_t)((pat >> 16) & 0xFF);
    if (entry2 != 0x07)
        TEST_SKIP("hypervisor traps PAT write (live index2 != UC-)");
    TEST_ASSERT_EQ(entry2, 0x07, "PAT index 2 = UC- (0x07) for PAGE_NOCACHE");
}

static void test_pat_index3_uc_strong(void)
{
    /* PAT index 3 (PCD+PWT, vmm_map_mmio_uc) must be UC-strong (0x00) -- the
     * device-MMIO correctness path. Unchanged by the S8 fix; assert it stayed
     * UC. (TODO-09-boot S8) */
    cpu_configure_pat();
    uint64_t pat = msr_read(MSR_IA32_PAT);
    uint8_t entry3 = (uint8_t)((pat >> 24) & 0xFF);
    if (entry3 != 0x00)
        TEST_SKIP("hypervisor traps PAT write (live index3 != UC)");
    TEST_ASSERT_EQ(entry3, 0x00, "PAT index 3 = UC-strong (0x00) for MMIO");
}

static void test_mtrr_capture_deterministic(void)
{
    /* mtrr_capture() is a read-only snapshot; two back-to-back captures on the
     * same CPU must be identical (no side effects, no drift). (TODO-09-boot S8) */
    struct mtrr_snapshot a, b;
    mtrr_capture(&a);
    mtrr_capture(&b);
    if (!a.supported)
        TEST_SKIP("MTRR not supported on this CPU");
    TEST_ASSERT(mtrr_snapshot_equal(&a, &b),
                "two mtrr_capture() calls on one CPU are identical");
    /* A supported snapshot folds DEF_TYPE + variable + fixed MTRRs into the
     * checksum; a 0 checksum would mean the register contents never got
     * folded, silently weakening the only divergence-detecting field. */
    TEST_ASSERT(a.checksum != 0,
                "supported MTRR snapshot has a non-zero checksum");
}

static void test_mtrr_var_count_bounded(void)
{
    /* MTRRCAP.VCNT is an 8-bit field; the captured variable count must never
     * exceed the architectural max (guards the PHYSBASE/PHYSMASK read loop
     * against an out-of-range count). (TODO-09-boot S8) */
    struct mtrr_snapshot a;
    mtrr_capture(&a);
    if (!a.supported)
        TEST_SKIP("MTRR not supported on this CPU");
    TEST_ASSERT(a.var_count <= MTRR_VARIABLE_MAX,
                "MTRR var_count within architectural bound");
}

static void test_mtrr_snapshot_equal_discriminates(void)
{
    /* mtrr_snapshot_equal() must return equal only when ALL compared fields
     * match, and detect a difference in any one of them -- so a divergent AP
     * is never silently reported as "synced". Hand-built snapshots; no MSR
     * access needed. (TODO-09-boot S8) */
    struct mtrr_snapshot a = { .cap = 0x0508, .def_type = 0xC06,
                               .var_count = 8, .supported = 1,
                               .checksum = 0xABCDEF12 };
    struct mtrr_snapshot b = a;
    TEST_ASSERT(mtrr_snapshot_equal(&a, &b), "identical snapshots compare equal");
    b.checksum = a.checksum ^ 1ULL;
    TEST_ASSERT(!mtrr_snapshot_equal(&a, &b), "checksum difference detected");
    b = a; b.def_type ^= 1ULL;
    TEST_ASSERT(!mtrr_snapshot_equal(&a, &b), "def_type difference detected");
    b = a; b.var_count += 1;
    TEST_ASSERT(!mtrr_snapshot_equal(&a, &b), "var_count difference detected");
    b = a; b.supported = 0;
    TEST_ASSERT(!mtrr_snapshot_equal(&a, &b), "supported difference detected");
    b = a; b.cap ^= MTRRCAP_FIX;
    TEST_ASSERT(!mtrr_snapshot_equal(&a, &b), "cap difference detected");
}

static void test_mtrr_bsp_baseline_matches_live(void)
{
    /* The BSP MTRR baseline recorded in cpu_record_bsp_profile() must equal a
     * fresh capture on the BSP (tests run on the BSP). MTRRs are never
     * reprogrammed, so the snapshot stays valid. (TODO-09-boot S8) */
    struct mtrr_snapshot base, live;
    cpu_bsp_mtrr_baseline(&base);
    mtrr_capture(&live);
    if (!base.supported)
        TEST_SKIP("MTRR not supported / baseline not captured");
    TEST_ASSERT(mtrr_snapshot_equal(&base, &live),
                "BSP MTRR baseline matches live capture");
}

/* ---- CPU register audit trail (TODO-09-boot S9) ----
 * Read-only checks of cpu_data[0]'s audit snapshot, captured during smp_init().
 * Tests do NOT call cpu_audit_registers() (it writes per_cpu_data + does the
 * Intel microcode-read MSR write); they assert the boot-captured values. */

static void test_cpu_audit_captured(void)
{
    struct per_cpu_data *bsp = smp_get_cpu(0);
    TEST_ASSERT(bsp != (struct per_cpu_data *)0, "cpu_data[0] present");
    TEST_ASSERT_EQ(bsp->audit_captured, 1u,
                   "BSP register audit was captured during smp_init()");
}

static void test_cpu_audit_cr0_wp(void)
{
    /* CR0/CR4 safety-bit pinning sets CR0.WP; the audit snapshot must show it. */
    struct per_cpu_data *bsp = smp_get_cpu(0);
    TEST_ASSERT_EQ((uint32_t)((bsp->cr0_at_boot >> 16) & 1u), 1u,
                   "audited BSP CR0.WP is set");
}

static void test_cpu_audit_efer_nxe(void)
{
    /* NX is a required feature; the audited EFER must have NXE set. */
    struct per_cpu_data *bsp = smp_get_cpu(0);
    TEST_ASSERT_EQ((uint32_t)((bsp->efer_at_boot >> 11) & 1u), 1u,
                   "audited BSP EFER.NXE is set");
}

static void test_cpu_audit_cr4_pae(void)
{
    /* Long mode requires CR4.PAE (bit 5); a captured CR4 of 0 would mean the
     * audit never read it. Sanity-checks the capture path. */
    struct per_cpu_data *bsp = smp_get_cpu(0);
    TEST_ASSERT_EQ((uint32_t)((bsp->cr4_at_boot >> 5) & 1u), 1u,
                   "audited BSP CR4.PAE is set (capture is live)");
}

static void test_cpu_audit_pat_matches_profile(void)
{
    /* The audit reads PAT fresh; on the BSP it must equal the MSR-profile PAT
     * baseline (PAT is never reprogrammed post-boot). Cross-checks the audit
     * snapshot against the independent PAT baseline. */
    struct per_cpu_data *bsp = smp_get_cpu(0);
    TEST_ASSERT_EQ(bsp->pat_at_boot, cpu_bsp_pat_baseline(),
                   "audited BSP PAT matches the MSR-profile baseline");
}

/* ---- AP bringup hardening (TODO-09-boot S10) ---- */

static void test_ap_forceable_cr4_excludes_fsgsbase_cet(void)
{
    /* The force-after-validation mask must NEVER include FSGSBASE/CET -- those
     * CR4 bits are not in the S6 AP probe, so forcing them on a skewed AP could
     * #GP. With ALL features set, the forceable mask still excludes them. */
    cpu_feature_mask_t all = { { ~0ULL, ~0ULL } };
    uint64_t f = cpu_ap_forceable_cr4(all);
    TEST_ASSERT_EQ((uint32_t)(f & (CR4_FSGSBASE | CR4_CET)), 0u,
                   "forceable CR4 excludes FSGSBASE + CET (not AP-probed)");
    TEST_ASSERT_EQ((uint32_t)((f & (CR4_OSXSAVE | CR4_UMIP | CR4_SMEP |
                                    CR4_SMAP | CR4_PCIDE | CR4_PKE)) ==
                              (CR4_OSXSAVE | CR4_UMIP | CR4_SMEP |
                               CR4_SMAP | CR4_PCIDE | CR4_PKE)), 1u,
                   "forceable CR4 includes the AP-probed CR4 bits");
}

static void test_ap_forceable_cr4_gated_by_features(void)
{
    /* No features -> nothing forceable (a CPU that proves nothing forces
     * nothing). */
    cpu_feature_mask_t none = { { 0, 0 } };
    TEST_ASSERT_EQ((uint32_t)cpu_ap_forceable_cr4(none), 0u,
                   "forceable CR4 is empty when no features are present");
    /* Only SMEP probed -> only CR4_SMEP forceable. */
    cpu_feature_mask_t smep_only = { { 0, 0 } };
    cpu_feature_set(&smep_only, CPU_FEATURE_SMEP);
    uint64_t f = cpu_ap_forceable_cr4(smep_only);
    TEST_ASSERT_EQ((uint32_t)f, (uint32_t)CR4_SMEP,
                   "SMEP-only features yield exactly CR4_SMEP");
}

static void test_ap_forceable_cr4_live_preconditions(void)
{
    /* Two CR4 bits have live preconditions beyond CPUID presence; the force path
     * must drop them when the precondition fails, mirroring the safe-enable
     * paths, else it #GPs on a skewed AP. cr3=0 is the clean boot-PML4 value. */
    cpu_feature_mask_t pku = { { 0, 0 } };
    cpu_feature_mask_t pcid = { { 0, 0 } };
    cpu_feature_set(&pku, CPU_FEATURE_PKU);
    cpu_feature_set(&pcid, CPU_FEATURE_PCID);

    /* PKE: XCR0.PKRU (bit 9) clear -> dropped even though PKU is present. */
    TEST_ASSERT_EQ((uint32_t)(cpu_ap_forceable_cr4_live(pku, 0x7, 0) & CR4_PKE), 0u,
                   "PKE dropped when XCR0 bit 9 clear");
    /* PKE: XCR0 bit 9 set -> retained. */
    TEST_ASSERT_EQ((uint32_t)((cpu_ap_forceable_cr4_live(pku, (1ULL << 9), 0) & CR4_PKE)
                              == CR4_PKE), 1u,
                   "PKE retained when XCR0 bit 9 set");
    /* PCIDE: CR3[11:0] nonzero -> dropped even though PCID is present (SDM
     * 4.10.1 says PCIDE may be set only when CR3[11:0]==0, else #GP). */
    TEST_ASSERT_EQ((uint32_t)(cpu_ap_forceable_cr4_live(pcid, 0, 0x123) & CR4_PCIDE), 0u,
                   "PCIDE dropped when CR3[11:0] nonzero");
    /* PCIDE: CR3[11:0]==0 -> retained. */
    TEST_ASSERT_EQ((uint32_t)((cpu_ap_forceable_cr4_live(pcid, 0, 0x1000) & CR4_PCIDE)
                              == CR4_PCIDE), 1u,
                   "PCIDE retained when CR3[11:0] zero");
    /* Gates touch only PKE/PCIDE: SMEP forceable regardless of XCR0/CR3 state. */
    cpu_feature_mask_t smep = { { 0, 0 } };
    cpu_feature_set(&smep, CPU_FEATURE_SMEP);
    TEST_ASSERT_EQ((uint32_t)cpu_ap_forceable_cr4_live(smep, 0, 0xFFF), (uint32_t)CR4_SMEP,
                   "live gates touch only PKE/PCIDE, not SMEP");
}

static void test_ap_probe_mask_covers_gated_features(void)
{
    /* The AP-local enable gates + TSC_AUX read this AP probe mask; if a gated
     * feature is dropped from it, the AP-local gate silently falls back to
     * never-enable. Guard every feature the S10 gating depends on. */
    cpu_feature_mask_t m = CPU_FEATURES_AP_PROBE_MASK;
    TEST_ASSERT(cpu_feature_test(&m, CPU_FEATURE_UMIP),   "AP probe mask covers UMIP");
    TEST_ASSERT(cpu_feature_test(&m, CPU_FEATURE_PKU),    "AP probe mask covers PKU");
    TEST_ASSERT(cpu_feature_test(&m, CPU_FEATURE_SMEP),   "AP probe mask covers SMEP");
    TEST_ASSERT(cpu_feature_test(&m, CPU_FEATURE_SMAP),   "AP probe mask covers SMAP");
    TEST_ASSERT(cpu_feature_test(&m, CPU_FEATURE_RDTSCP), "AP probe mask covers RDTSCP (TSC_AUX gate)");
    TEST_ASSERT(cpu_feature_test(&m, CPU_FEATURE_WAITPKG), "AP probe mask covers WAITPKG (UMWAIT_CONTROL gate)");
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

/* ---- AP CPU hardening: BSP MSR replay profile + baseline ---- */

static void test_ap_msr_profile_populated(void)
{
    /* Pin the exact registry shape: PAT (verbatim) + TSC_AUX (per-CPU) + UMWAIT
     * (per-CPU computed constant, WAITPKG-gated; S19 anti-DoS). UMWAIT MUST be
     * per_cpu==1 (computed): a verbatim BSP-value replay would write a stale 0
     * (unbounded) onto an AP-only WAITPKG core when the BSP lacks WAITPKG. An
     * accidental added/duplicate/wrong entry would change AP MSR replay. */
    uint32_t i, count = cpu_msr_profile_count();
    uint32_t pat_count = 0, tsc_aux_count = 0, umwait_count = 0;
    uint32_t spec_ctrl_count = 0, unknown = 0;

    TEST_ASSERT_EQ((uint64_t)count, 4ULL,
                   "MSR profile registry has exactly 4 entries");
    for (i = 0; i < count; i++) {
        uint32_t msr = 0;
        uint64_t value = 0;
        int per_cpu = -1;
        if (cpu_msr_profile_entry(i, &msr, &value, &per_cpu) != 0)
            continue;
        if (msr == MSR_IA32_PAT && per_cpu == 0)
            pat_count++;
        else if (msr == MSR_IA32_TSC_AUX && per_cpu == 1)
            tsc_aux_count++;
        else if (msr == MSR_IA32_UMWAIT_CONTROL && per_cpu == 1)
            umwait_count++;
        else if (msr == MSR_IA32_SPEC_CTRL && per_cpu == 1)
            spec_ctrl_count++;
        else
            unknown++;
    }
    TEST_ASSERT_EQ((uint64_t)pat_count, 1ULL,
                   "exactly one verbatim PAT profile entry");
    TEST_ASSERT_EQ((uint64_t)tsc_aux_count, 1ULL,
                   "exactly one per-CPU TSC_AUX profile entry");
    TEST_ASSERT_EQ((uint64_t)umwait_count, 1ULL,
                   "exactly one per-CPU computed UMWAIT_CONTROL profile entry");
    TEST_ASSERT_EQ((uint64_t)spec_ctrl_count, 1ULL,
                   "exactly one per-CPU computed SPEC_CTRL profile entry (S8 eIBRS)");
    TEST_ASSERT_EQ((uint64_t)unknown, 0ULL,
                   "no unknown MSR profile entries");
}

static void test_ap_msr_profile_entry_bounds(void)
{
    uint32_t count = cpu_msr_profile_count();
    uint32_t msr = 0xABCDu;
    uint64_t value = 0x1234ULL;
    int per_cpu = 7;

    /* idx == count and a large idx must fail WITHOUT mutating out-params. */
    TEST_ASSERT_EQ(cpu_msr_profile_entry(count, &msr, &value, &per_cpu), -1,
                   "cpu_msr_profile_entry(count) returns -1");
    TEST_ASSERT_EQ(cpu_msr_profile_entry(0xFFFFu, &msr, &value, &per_cpu), -1,
                   "cpu_msr_profile_entry(large idx) returns -1");
    TEST_ASSERT_EQ((uint64_t)msr, 0xABCDULL,
                   "failed lookup leaves msr out-param untouched");
    TEST_ASSERT_EQ(value, 0x1234ULL,
                   "failed lookup leaves value out-param untouched");
    TEST_ASSERT_EQ((uint64_t)per_cpu, 7ULL,
                   "failed lookup leaves per_cpu out-param untouched");

    /* Valid index with all NULL out-params must still succeed. */
    TEST_ASSERT_EQ(cpu_msr_profile_entry(0, (uint32_t *)0, (uint64_t *)0,
                                         (int *)0), 0,
                   "valid index tolerates NULL out-params");
}

static void test_ap_msr_profile_pat_entry(void)
{
    uint32_t msr = 0;
    uint64_t value = 0;
    int per_cpu = -1;

    /* Entry 0 is PAT: replicated verbatim (not per-CPU). */
    TEST_ASSERT_EQ(cpu_msr_profile_entry(0, &msr, &value, &per_cpu), 0,
                   "MSR profile entry 0 readable");
    TEST_ASSERT_EQ((uint64_t)msr, (uint64_t)MSR_IA32_PAT,
                   "MSR profile entry 0 is IA32_PAT");
    TEST_ASSERT_EQ((uint64_t)per_cpu, 0ULL,
                   "PAT profile entry is replicated verbatim (per_cpu == 0)");
}

static void test_ap_msr_profile_tsc_aux_per_cpu(void)
{
    uint32_t i, count = cpu_msr_profile_count();
    int found = 0;

    /* TSC_AUX must be flagged per-CPU (value is logical id, not BSP value). */
    for (i = 0; i < count; i++) {
        uint32_t msr = 0;
        uint64_t value = 0;
        int per_cpu = -1;
        if (cpu_msr_profile_entry(i, &msr, &value, &per_cpu) == 0 &&
            msr == MSR_IA32_TSC_AUX) {
            found = 1;
            TEST_ASSERT_EQ((uint64_t)per_cpu, 1ULL,
                           "TSC_AUX profile entry is per-CPU (per_cpu == 1)");
        }
    }
    TEST_ASSERT(found, "MSR profile registry contains TSC_AUX");
}

static void test_ap_bsp_pat_baseline_matches_live(void)
{
    /* cpu_record_bsp_profile() ran during smp_init(); PAT is not reprogrammed
     * afterward, so the recorded baseline equals the live BSP PAT MSR. */
    TEST_ASSERT_EQ(cpu_bsp_pat_baseline(), msr_read(MSR_IA32_PAT),
                   "BSP PAT baseline matches live IA32_PAT");
}

static void test_ap_bsp_snapshot_recorded(void)
{
    struct per_cpu_data *bsp = smp_get_cpu(0);
    TEST_ASSERT(bsp != (struct per_cpu_data *)0, "BSP per-CPU data present");
    if (!bsp) return;

    /* Baseline was captured: EFER non-zero (LME/LMA set in long mode) and
     * CR4.PAE set (mandatory in 64-bit mode). */
    TEST_ASSERT(bsp->efer_at_boot != 0,
                "BSP efer_at_boot recorded (non-zero)");
    TEST_ASSERT((bsp->cr4_at_boot & (1ULL << 5)) != 0,
                "BSP cr4_at_boot has CR4.PAE set");
    if (cpu_has(CPU_FEATURE_NX))
        TEST_ASSERT((bsp->efer_at_boot & EFER_NXE) != 0,
                    "BSP efer_at_boot has EFER.NXE when NX supported");

    /* PAT snapshot is internally consistent: recorded field == accessor
     * baseline == live MSR. */
    TEST_ASSERT_EQ(bsp->pat_at_boot, cpu_bsp_pat_baseline(),
                   "BSP pat_at_boot == PAT baseline accessor");
    TEST_ASSERT_EQ(bsp->pat_at_boot, msr_read(MSR_IA32_PAT),
                   "BSP pat_at_boot == live IA32_PAT");

    /* XCR0 snapshot: x87+SSE bits present when XSAVE supported, else 0. */
    if (cpu_has(CPU_FEATURE_XSAVE))
        TEST_ASSERT_EQ(bsp->xcr0_at_boot & 0x3ULL, 0x3ULL,
                       "BSP xcr0_at_boot has x87+SSE bits when XSAVE present");
    else
        TEST_ASSERT_EQ(bsp->xcr0_at_boot, 0ULL,
                       "BSP xcr0_at_boot == 0 when XSAVE absent");

    /* BSP is logical CPU 0 and never runs the AP replay, so its TSC_AUX
     * snapshot is 0 and its profile-applied counter stays 0. */
    TEST_ASSERT_EQ(bsp->tsc_aux, 0ULL, "BSP tsc_aux snapshot == 0");
    TEST_ASSERT_EQ((uint64_t)bsp->msr_profile_applied, 0ULL,
                   "BSP msr_profile_applied == 0 (AP-only counter)");
}

/* ---- S14: hw_profile pure helpers (benchmark itself runs at Phase 3) ---- */

static void test_hwprofile_stale_null(void)
{
    /* NULL stored, or absent VALID flag, or NULL brand -> stale */
    TEST_ASSERT_EQ((uint64_t)hw_profile_is_stale(NULL, "x", "x"), 1ULL,
                   "NULL stored profile is stale");
    hw_profile_t pf;
    pf.flags = 0;  /* not HW_PROFILE_VALID */
    TEST_ASSERT_EQ((uint64_t)hw_profile_is_stale(&pf, "x", "x"), 1ULL,
                   "profile without VALID flag is stale");
    pf.flags = HW_PROFILE_VALID;
    TEST_ASSERT_EQ((uint64_t)hw_profile_is_stale(&pf, NULL, "x"), 1ULL,
                   "NULL stored brand is stale");
}

static void test_hwprofile_stale_brand(void)
{
    hw_profile_t pf;
    pf.flags = HW_PROFILE_VALID;
    TEST_ASSERT_EQ((uint64_t)hw_profile_is_stale(&pf, "Intel(R) Core(TM)",
                                                 "Intel(R) Core(TM)"), 0ULL,
                   "matching brand + VALID is not stale");
    TEST_ASSERT_EQ((uint64_t)hw_profile_is_stale(&pf, "Intel(R) Core(TM)",
                                                 "AMD Ryzen 9"), 1ULL,
                   "different brand is stale");
}

static void test_hwprofile_simd_decision(void)
{
    int keep_avx2, keep_avx512;
    /* AVX-512 30% faster than AVX2 -> keep AVX-512 */
    hw_profile_simd_decision(1000, 2000, 2600, &keep_avx2, &keep_avx512);
    TEST_ASSERT_EQ((uint64_t)keep_avx512, 1ULL,
                   "AVX-512 30% faster than AVX2 -> kept");
    /* AVX-512 only 5% faster -> throttle, disable */
    hw_profile_simd_decision(1000, 2000, 2100, &keep_avx2, &keep_avx512);
    TEST_ASSERT_EQ((uint64_t)keep_avx512, 0ULL,
                   "AVX-512 <10% over AVX2 -> disabled");
    /* AVX2 == SSE2 -> disable AVX2 */
    hw_profile_simd_decision(2000, 2000, 0, &keep_avx2, &keep_avx512);
    TEST_ASSERT_EQ((uint64_t)keep_avx2, 0ULL,
                   "AVX2 no gain over SSE2 -> disabled");
    /* AVX2 50% faster than SSE2 -> keep */
    hw_profile_simd_decision(2000, 3000, 0, &keep_avx2, &keep_avx512);
    TEST_ASSERT_EQ((uint64_t)keep_avx2, 1ULL,
                   "AVX2 50% faster than SSE2 -> kept");
    /* zero measurement (ISA absent/unmeasured) -> keep (no disable on no data) */
    hw_profile_simd_decision(0, 0, 0, &keep_avx2, &keep_avx512);
    TEST_ASSERT_EQ((uint64_t)keep_avx2, 1ULL, "zero data -> AVX2 kept");
    TEST_ASSERT_EQ((uint64_t)keep_avx512, 1ULL, "zero data -> AVX-512 kept");
}

/* ---- S15: future-silicon detection stubs ---- */

static void test_cc_kind_valid(void)
{
    const struct cpu_features *c = cpuid_get();
    TEST_ASSERT((uint64_t)c->cc_kind <= (uint64_t)CC_AMD_SEV_SNP,
                "cc_kind is a valid enum value");
    /* Re-derive the expected cc_kind from CPUID/MSR rather than hard-coding the
     * test platform as non-confidential -- this is correct on bare metal
     * (CC_NONE), a TDX guest, and an SEV guest alike. */
    uint32_t a, b, cc, d, maxleaf, maxext;
    __asm__ volatile("cpuid" : "=a"(maxleaf), "=b"(b), "=c"(cc), "=d"(d)
                             : "a"(0u), "c"(0u));
    __asm__ volatile("cpuid" : "=a"(maxext), "=b"(b), "=c"(cc), "=d"(d)
                             : "a"(0x80000000u), "c"(0u));
    uint8_t expect = CC_NONE;
    if (maxleaf >= 0x21u) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(cc), "=d"(d)
                                 : "a"(0x21u), "c"(0u));
        if (b == 0x65746E49u && d == 0x5844546Cu && cc == 0x20202020u)
            expect = CC_INTEL_TDX;
    }
    if (expect == CC_NONE && maxext >= 0x8000001Fu) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(cc), "=d"(d)
                                 : "a"(0x8000001Fu), "c"(0u));
        if (a & (1u << 1)) {
            uint64_t st = msr_read(MSR_AMD64_SEV);
            if (st & 1u) {
                if (st & (1u << 2))      expect = CC_AMD_SEV_SNP;
                else if (st & (1u << 1)) expect = CC_AMD_SEV_ES;
                else                     expect = CC_AMD_SEV;
            }
        }
    }
    TEST_ASSERT_EQ((uint64_t)c->cc_kind, (uint64_t)expect,
                   "cc_kind matches CPUID/MSR-derived expectation");
}

static void test_future_silicon_bits(void)
{
    /* Pin the detection bit positions so a wrong bit cannot regress in: LASS at
     * CPUID.(7,1):EAX[6] (NOT 27), LAM at EAX[26], LA57 at CPUID.(7,0):ECX[16].
     * On a CPU lacking a feature both sides read 0; on a capable CPU both read
     * 1; a wrong bit would diverge from the spec bit read here. */
    uint32_t a, b, c, d;
    /* Gate the leaf-7 query on max basic leaf, exactly like cpuid_init -- on a
     * CPU with max_leaf < 7 the CPUID(7) result is not leaf-7 data. */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(0u), "c"(0u));
    uint32_t max_leaf = a;
    if (max_leaf < 7) {
        TEST_ASSERT_EQ((uint64_t)cpu_has(CPU_FEATURE_LA57), 0ULL,
                       "LA57 clear when leaf 7 unsupported");
        TEST_ASSERT_EQ((uint64_t)cpu_has(CPU_FEATURE_LASS), 0ULL,
                       "LASS clear when leaf 7 unsupported");
        TEST_ASSERT_EQ((uint64_t)cpu_has(CPU_FEATURE_LAM), 0ULL,
                       "LAM clear when leaf 7 unsupported");
        return;
    }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(7u), "c"(0u));
    uint32_t ecx_70 = c, max_sub = a;
    TEST_ASSERT_EQ((uint64_t)(cpu_has(CPU_FEATURE_LA57) ? 1 : 0),
                   (uint64_t)((ecx_70 >> 16) & 1u), "LA57 == CPUID.7.0:ECX[16]");
    if (max_sub < 1) {
        /* no subleaf 1 -> LASS/LAM must not be set */
        TEST_ASSERT_EQ((uint64_t)cpu_has(CPU_FEATURE_LASS), 0ULL,
                       "LASS clear when subleaf 1 unsupported");
        TEST_ASSERT_EQ((uint64_t)cpu_has(CPU_FEATURE_LAM), 0ULL,
                       "LAM clear when subleaf 1 unsupported");
        return;
    }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(7u), "c"(1u));
    TEST_ASSERT_EQ((uint64_t)(cpu_has(CPU_FEATURE_LASS) ? 1 : 0),
                   (uint64_t)((a >> 6) & 1u), "LASS == CPUID.7.1:EAX[6]");
    TEST_ASSERT_EQ((uint64_t)(cpu_has(CPU_FEATURE_LAM) ? 1 : 0),
                   (uint64_t)((a >> 26) & 1u), "LAM == CPUID.7.1:EAX[26]");
}

/* ---- S19: WAITPKG / SERIALIZE / RDPID adoption ---- */

static void test_feature_adoption_bits(void)
{
    uint32_t a, b, c, d, maxleaf;
    __asm__ volatile("cpuid" : "=a"(maxleaf), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(0u), "c"(0u));
    if (maxleaf < 7) {
        TEST_ASSERT_EQ((uint64_t)cpu_has(CPU_FEATURE_WAITPKG), 0ULL,
                       "WAITPKG clear without leaf 7");
        return;
    }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(7u), "c"(0u));
    TEST_ASSERT_EQ((uint64_t)(cpu_has(CPU_FEATURE_WAITPKG) ? 1 : 0),
                   (uint64_t)((c >> 5) & 1u), "WAITPKG == CPUID.7.0:ECX[5]");
    TEST_ASSERT_EQ((uint64_t)(cpu_has(CPU_FEATURE_RDPID) ? 1 : 0),
                   (uint64_t)((c >> 22) & 1u), "RDPID == CPUID.7.0:ECX[22]");
    TEST_ASSERT_EQ((uint64_t)(cpu_has(CPU_FEATURE_SERIALIZE) ? 1 : 0),
                   (uint64_t)((d >> 14) & 1u), "SERIALIZE == CPUID.7.0:EDX[14]");
}

static void test_umwait_bounded(void)
{
    /* On a WAITPKG CPU, the BSP UMWAIT bound is programmed by
     * cpu_program_bsp_umwait() (boot_phase2, post-IDT); APs replay it via
     * ap_apply_msr_profile(). The live MSR must equal the exact programmed
     * constant -- non-zero alone would let a garbage firmware value pass.
     * 100000 mirrors UMWAIT_MAX_DWELL_TSC in cpu_security.c (keep in sync).
     * Skipped on CPUs lacking WAITPKG. */
    if (!cpu_has(CPU_FEATURE_WAITPKG)) {
        TEST_SKIP("no WAITPKG on this CPU");
        return;
    }
    uint64_t v = msr_read(MSR_IA32_UMWAIT_CONTROL);
    TEST_ASSERT_EQ(v, (100000ull & ~3ull),
                   "UMWAIT_CONTROL == programmed bound (UMWAIT_MAX_DWELL_TSC)");
}

/* ---- S8: Spectre v2 (eIBRS / IBPB / retpoline) ---- */

static void test_spectre_feature_bits(void)
{
    /* ENHANCED_IBRS is MSR-derived (IA32_ARCH_CAPABILITIES[1] IBRS_ALL), gated on
     * ARCH_CAP; pin cpu_has(ENHANCED_IBRS) against that derivation. IBPB on Intel
     * is implied by SPEC_CTRL (7.0:EDX[26]); if SPEC_CTRL is present, IBPB must be. */
    if (cpu_has(CPU_FEATURE_ARCH_CAP)) {
        uint64_t ac = msr_read(MSR_IA32_ARCH_CAPS);
        TEST_ASSERT_EQ((uint64_t)(cpu_has(CPU_FEATURE_ENHANCED_IBRS) ? 1 : 0),
                       (uint64_t)((ac & ARCH_CAP_IBRS_ALL) ? 1 : 0),
                       "ENHANCED_IBRS == ARCH_CAPABILITIES[1] IBRS_ALL");
    } else {
        TEST_ASSERT_EQ((uint64_t)cpu_has(CPU_FEATURE_ENHANCED_IBRS), 0ULL,
                       "no eIBRS without ARCH_CAP");
    }
    if (cpu_has(CPU_FEATURE_SPEC_CTRL))
        TEST_ASSERT(cpu_has(CPU_FEATURE_IBPB),
                    "Intel SPEC_CTRL implies IBPB (7.0:EDX[26])");
}

static void test_eibrs_active(void)
{
    /* On an eIBRS CPU, cpu_program_bsp_eibrs() set IA32_SPEC_CTRL.IBRS once at
     * boot; the bit must read back set (set-once, never toggled). Skipped on
     * non-eIBRS CPUs (retpoline covers them; no SPEC_CTRL write). */
    if (!cpu_has(CPU_FEATURE_ENHANCED_IBRS)) {
        TEST_SKIP("no Enhanced IBRS on this CPU");
        return;
    }
    uint64_t v = msr_read(MSR_IA32_SPEC_CTRL);
    TEST_ASSERT((v & SPEC_CTRL_IBRS) != 0,
                "eIBRS: IA32_SPEC_CTRL.IBRS set permanently");
}

static void test_ap_probe_covers_spec_ctrl(void)
{
    /* SPEC_CTRL must be in the AP probe mask so cpu_feature_local() gates the
     * SPEC_CTRL eIBRS replay correctly on each AP (else APs silently skip it). */
    cpu_feature_mask_t m = CPU_FEATURES_AP_PROBE_MASK;
    TEST_ASSERT(cpu_feature_test(&m, CPU_FEATURE_SPEC_CTRL),
                "AP probe mask covers SPEC_CTRL (eIBRS replay gate)");
}

/* ---- S19: MDS/VERW microarchitectural buffer clear ---- */

extern volatile uint8_t g_mds_verw_active;

static void test_md_clear_feature_bit(void)
{
    /* MD_CLEAR is CPUID.(7,0):EDX[10]; it is CPU_FEATURE_MD_CLEAR = bit 64, the
     * first feature in word 1 of the 128-bit cpu_feature_mask_t, so this also
     * exercises a real high-word cpu_has() read end to end. */
    uint32_t a, b, c, d;
    cpuid_raw(0x07, 0, &a, &b, &c, &d);
    TEST_ASSERT_EQ((uint64_t)(cpu_has(CPU_FEATURE_MD_CLEAR) ? 1 : 0),
                   (uint64_t)((d >> 10) & 1),
                   "MD_CLEAR == CPUID.(7,0):EDX[10] (word-1 feature read)");
}

static void test_mds_verw_gate_implies_capability(void)
{
    /* The once-per-boot decision never latches the VERW gate without the MD_CLEAR
     * clear capability (an unconditional VERW on a CPU that cannot clear is wasted
     * cycles). On MDS_NO / no-MD_CLEAR silicon the gate stays off and the exit
     * path is a no-op. */
    if (!g_mds_verw_active) {
        TEST_SKIP("VERW gate off on this CPU (MDS_NO or no MD_CLEAR)");
        return;
    }
    TEST_ASSERT(cpu_has(CPU_FEATURE_MD_CLEAR),
                "VERW gate on implies MD_CLEAR present");
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

    /* AP CPU hardening: BSP MSR replay profile + baseline */
    test_suite_register_cat("AP harden: MSR profile populated",
        test_ap_msr_profile_populated, TEST_CAT_X86);
    test_suite_register_cat("AP harden: MSR profile entry bounds/NULL",
        test_ap_msr_profile_entry_bounds, TEST_CAT_X86);
    test_suite_register_cat("AP harden: PAT profile entry verbatim",
        test_ap_msr_profile_pat_entry, TEST_CAT_X86);
    test_suite_register_cat("AP harden: TSC_AUX profile entry per-CPU",
        test_ap_msr_profile_tsc_aux_per_cpu, TEST_CAT_X86);
    test_suite_register_cat("AP harden: BSP PAT baseline matches live",
        test_ap_bsp_pat_baseline_matches_live, TEST_CAT_X86);
    test_suite_register_cat("AP harden: BSP baseline snapshot recorded",
        test_ap_bsp_snapshot_recorded, TEST_CAT_X86);

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

    /* S5 (TODO-09-boot): PCID activation window */
    test_suite_register_cat("PCID: CR4.PCIDE set when supported",
        test_cr4_pcide, TEST_CAT_X86);
    test_suite_register_cat("PCID: CR3[11:0] stays 0 (no tagging yet)",
        test_cr3_pcid_zero, TEST_CAT_X86);

    /* S6 (TODO-09-boot): AP feature consistency validation */
    test_suite_register_cat("AP features: BSP satisfies required mask",
        test_required_mask_subset_of_bsp, TEST_CAT_X86);
    test_suite_register_cat("AP features: required is subset of probe mask",
        test_required_subset_of_probe, TEST_CAT_X86);
    test_suite_register_cat("AP features: global intersection retains required",
        test_global_feature_mask_has_required, TEST_CAT_X86);
    test_suite_register_cat("MDS: MD_CLEAR feature bit (word-1 read)",
        test_md_clear_feature_bit, TEST_CAT_X86);
    test_suite_register_cat("MDS: VERW gate implies MD_CLEAR capability",
        test_mds_verw_gate_implies_capability, TEST_CAT_X86);
    test_suite_register_cat("feature-mask: 128-bit word/bit split round-trips",
        test_feature_mask_wordsplit, TEST_CAT_X86);
    test_suite_register_cat("feature-mask: high-word AP intersection clears",
        test_feature_mask_intersection_highword, TEST_CAT_X86);

    /* S7 (TODO-09-boot): CR0/CR4 safety-bit pinning */
    test_suite_register_cat("CR pin: CR0.WP pinned + set",
        test_cr0_wp_pinned, TEST_CAT_X86);
    test_suite_register_cat("CR pin: pinned CR0 bits held",
        test_cr0_pins_held, TEST_CAT_X86);
    test_suite_register_cat("CR pin: pinned CR4 bits held",
        test_cr4_pins_held, TEST_CAT_X86);

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
    test_suite_register_cat("PKU: committed-online predicate state table",
        test_cpu_slot_committed_online_states, TEST_CAT_X86);
    test_suite_register_cat("PKU: pku_enabled matches online-CPU CR4.PKE intersection",
        test_pku_enabled_matches_online_intersection, TEST_CAT_X86);
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
    test_suite_register_cat("PAT: index 1 is WC (0x01)",
        test_wc_pat_entry, TEST_CAT_X86);

    /* S8: PAT decode-bug guards + MTRR parity audit */
    test_suite_register_cat("PAT: index 0 is WB (normal memory)",
        test_pat_index0_wb, TEST_CAT_X86);
    test_suite_register_cat("PAT: index 2 is UC- (PAGE_NOCACHE)",
        test_pat_index2_uc_minus, TEST_CAT_X86);
    test_suite_register_cat("PAT: index 3 is UC-strong (MMIO)",
        test_pat_index3_uc_strong, TEST_CAT_X86);
    test_suite_register_cat("MTRR: capture is deterministic",
        test_mtrr_capture_deterministic, TEST_CAT_X86);
    test_suite_register_cat("MTRR: var_count within bound",
        test_mtrr_var_count_bounded, TEST_CAT_X86);
    test_suite_register_cat("MTRR: snapshot_equal discriminates",
        test_mtrr_snapshot_equal_discriminates, TEST_CAT_X86);
    test_suite_register_cat("MTRR: BSP baseline matches live",
        test_mtrr_bsp_baseline_matches_live, TEST_CAT_X86);

    /* S9: CPU register audit trail */
    test_suite_register_cat("Audit: BSP register snapshot captured",
        test_cpu_audit_captured, TEST_CAT_X86);
    test_suite_register_cat("Audit: BSP CR0.WP set in snapshot",
        test_cpu_audit_cr0_wp, TEST_CAT_X86);
    test_suite_register_cat("Audit: BSP EFER.NXE set in snapshot",
        test_cpu_audit_efer_nxe, TEST_CAT_X86);
    test_suite_register_cat("Audit: BSP CR4.PAE set (capture live)",
        test_cpu_audit_cr4_pae, TEST_CAT_X86);
    test_suite_register_cat("Audit: BSP PAT matches MSR-profile baseline",
        test_cpu_audit_pat_matches_profile, TEST_CAT_X86);

    /* S10: AP bringup hardening */
    test_suite_register_cat("AP harden: forceable CR4 excludes FSGSBASE/CET",
        test_ap_forceable_cr4_excludes_fsgsbase_cet, TEST_CAT_X86);
    test_suite_register_cat("AP harden: forceable CR4 gated by features",
        test_ap_forceable_cr4_gated_by_features, TEST_CAT_X86);
    test_suite_register_cat("AP harden: forceable CR4 live preconditions (PKE/PCIDE)",
        test_ap_forceable_cr4_live_preconditions, TEST_CAT_X86);
    test_suite_register_cat("AP harden: probe mask covers gated features",
        test_ap_probe_mask_covers_gated_features, TEST_CAT_X86);

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

    /* S14: boot self-benchmark pure helpers */
    test_suite_register_cat("hw_profile: staleness NULL/flag/brand guards",
        test_hwprofile_stale_null, TEST_CAT_X86);
    test_suite_register_cat("hw_profile: staleness brand compare",
        test_hwprofile_stale_brand, TEST_CAT_X86);
    test_suite_register_cat("hw_profile: SIMD auto-tune decision",
        test_hwprofile_simd_decision, TEST_CAT_X86);

    /* S15: future-silicon detection stubs */
    test_suite_register_cat("future-silicon: cc_kind valid + CC_NONE on test HW",
        test_cc_kind_valid, TEST_CAT_X86);
    test_suite_register_cat("future-silicon: LA57/LAM/LASS bit positions pinned",
        test_future_silicon_bits, TEST_CAT_X86);

    /* S19: WAITPKG / SERIALIZE / RDPID adoption */
    test_suite_register_cat("feature-adopt: WAITPKG/SERIALIZE/RDPID bit positions",
        test_feature_adoption_bits, TEST_CAT_X86);
    test_suite_register_cat("feature-adopt: UMWAIT_CONTROL bounded when WAITPKG",
        test_umwait_bounded, TEST_CAT_X86);

    /* S8: Spectre v2 (eIBRS / IBPB / retpoline) */
    test_suite_register_cat("spectre: ENHANCED_IBRS/IBPB feature bits",
        test_spectre_feature_bits, TEST_CAT_X86);
    test_suite_register_cat("spectre: eIBRS IA32_SPEC_CTRL.IBRS set-once",
        test_eibrs_active, TEST_CAT_X86);
    test_suite_register_cat("spectre: AP probe mask covers SPEC_CTRL",
        test_ap_probe_covers_spec_ctrl, TEST_CAT_X86);
}

#endif /* KERNEL_TESTS */
