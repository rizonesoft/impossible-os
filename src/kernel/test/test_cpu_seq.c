/* ============================================================================
 * test_cpu_seq.c -- CPU boot-sequencing invariants (TODO-09-boot Unit Tests)
 *
 * Cross-section invariants that no per-feature test owns. Coverage already in
 * test_cpu_security.c (EFER.NXE, CR4.OSXSAVE, MSR-profile size, CR0/CR4 pin
 * masks, PAT-vs-baseline, audit EFER/CR0/CR4, global-mask-retains-required) is
 * cited by the TODO, never duplicated here.
 *
 * Every suite here must be able to FAIL on a real boot. Three neighbouring
 * assertions are deliberately ABSENT because they cannot:
 *   - `feature_mismatch == 0` contradicts the shipped policy. Optional feature
 *     skew is TOLERATED and narrows the global intersection, so a legitimately
 *     heterogeneous machine boots correctly and would fail such a suite.
 *   - a "defined core_type" check proves nothing: CORE_TYPE_GENERIC is 0x00,
 *     so a never-populated slot satisfies it.
 *   - `ucode_rev != 0` reads a PAYLOAD as a capture signal, but
 *     cpu_read_microcode_rev() returns 0 for a legitimately unavailable MSR.
 *
 * Ordering invariants (hypervisor detection before timer selection) are NOT
 * testable from here -- the probes below read final state, which a reordered
 * boot would still satisfy. That claim is carried by the serial-log bullets in
 * the roadmap's Verification block; this file is not coverage of it.
 *
 * Read-only: every test reads state the boot path already published. Nothing
 * here calls a boot-mutating helper (same discipline as test_cpu_security.c).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/cpuid.h"
#include "kernel/cpuid_platform.h"
#include "kernel/cpu_security.h"
#include "kernel/msr.h"
#include "kernel/smp.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "kernel/acpi.h"

extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* cpuid.h declares g_cpu only inside cpu_has(); test_cpu_security.c uses the
 * same file-scope extern to reach the cached snapshot directly. */
extern struct cpu_features g_cpu;

/* CPUID feature bits cross-checked against the cached g_cpu mask. */
#define CPUID_1_EDX_SSE2_BIT       26   /* CPUID.01H:EDX[26]       -- SSE2 */
#define CPUID_EXT1_EDX_NX_BIT      20   /* CPUID.80000001H:EDX[20] -- NX   */
#define CPUID_LEAF_EXT_FEATURES    0x80000001u

/* CET state components 11 (user) and 12 (supervisor) are SUPERVISOR state:
 * Intel SDM Vol. 1, 13.1 places them under IA32_XSS, never XCR0, where both
 * bits are reserved. Their absence from XCR0 is therefore a PERMANENT
 * invariant, not a deferred contract -- a correct CET implementation (owned by
 * 02-kernel-core/TODO-10) still leaves XCR0 clear here, which is why this is a
 * plain assert rather than a pending marker that could never flip. */
#define XCR0_CET_U_RESERVED   (1ULL << 11)
#define XCR0_CET_S_RESERVED   (1ULL << 12)

/* ---- Cached CPU features match the live CPU (S1) ---- */

static void test_cached_minimums_match_live_cpuid(void)
{
    /* g_cpu is a Phase-0 snapshot; a stale or mis-shifted capture makes every
     * cpu_has() consumer wrong while boot still succeeds. The cross-check
     * against live CPUID is what makes this discriminating -- "NX and SSE2 are
     * set" on its own would pass on any machine that booted at all, since the
     * boot path already halts a CPU missing the required feature mask. */
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint32_t ext_edx = 0;

    cpuid_raw(1, 0, &eax, &ebx, &ecx, &edx);
    cpuid_raw(CPUID_LEAF_EXT_FEATURES, 0, &eax, &ebx, &ecx, &ext_edx);

    TEST_ASSERT_EQ((uint32_t)(cpu_has(CPU_FEATURE_SSE2) ? 1u : 0u),
                   (edx >> CPUID_1_EDX_SSE2_BIT) & 1u,
                   "cached SSE2 matches live CPUID.01H:EDX[26]");
    TEST_ASSERT_EQ((uint32_t)(cpu_has(CPU_FEATURE_NX) ? 1u : 0u),
                   (ext_edx >> CPUID_EXT1_EDX_NX_BIT) & 1u,
                   "cached NX matches live CPUID.80000001H:EDX[20]");
    TEST_ASSERT(cpu_has(CPU_FEATURE_SSE2), "SSE2 present (boot minimum)");
    TEST_ASSERT(cpu_has(CPU_FEATURE_NX), "NX present (boot minimum)");
}

/* ---- Hypervisor detection published a coherent result (S3) ---- */

/* Bounded compare against the fixed-size hv_vendor[16] field. */
static int hv_vendor_is(const char *want)
{
    uint32_t i;
    for (i = 0; i < sizeof(g_boot_info.hv_vendor); i++) {
        if (g_boot_info.hv_vendor[i] != want[i])
            return 0;
        if (want[i] == '\0')
            return 1;
    }
    return 1;
}

static void test_hv_vendor_recognized(void)
{
    /* platform_detect() writes hv_vendor only on the four branches that
     * recognize a vendor string (cpuid_platform.c:109, :137, :152, :189); TCG
     * and unknown-HV deliberately leave it empty. So the field is either empty
     * or one of exactly those four strings -- anything else is a corrupted or
     * partially-written capture. */
    char shown[sizeof(g_boot_info.hv_vendor) + 1];
    char msg[96];
    uint32_t i;
    int ok = (g_boot_info.hv_vendor[0] == '\0') ||
             hv_vendor_is("Microsoft Hv") ||
             hv_vendor_is("VMwareVMware") ||
             hv_vendor_is("VBoxVBoxVBox") ||
             hv_vendor_is("KVMKVMKVM");

    /* Copy exactly 16 bytes and terminate. Formatting the raw field with %s
     * would run past it on precisely the missing-NUL corruption this test
     * exists to report. */
    for (i = 0; i < sizeof(g_boot_info.hv_vendor); i++)
        shown[i] = g_boot_info.hv_vendor[i];
    shown[sizeof(g_boot_info.hv_vendor)] = '\0';

    snprintf(msg, sizeof(msg), "hv_vendor empty or recognized (got \"%s\")",
             shown);
    TEST_ASSERT(ok, msg);
}

static void test_hv_vendor_implies_hypervisor(void)
{
    /* A written vendor string means a vendor branch ran, and every one of them
     * set cached_platform away from PLATFORM_BARE_METAL. The converse does NOT
     * hold: TCG (cpuid_platform.c:199) sets the platform and leaves the vendor
     * empty, so this implication is deliberately one-directional. */
    if (g_boot_info.hv_vendor[0] == '\0') {
        TEST_SKIP("no hv_vendor written (bare metal, TCG, or unknown HV)");
        return;
    }
    TEST_ASSERT(platform_get() != PLATFORM_BARE_METAL,
                "non-empty hv_vendor implies a detected hypervisor platform");
}

static void test_tsc_enlightenment_only_on_hyperv(void)
{
    /* HV_FLAG_TSC_ENLIGHTENMENT gates the Hyper-V reference-TSC MSR; it is set
     * only inside the "Microsoft Hv" branch. Seeing it anywhere else means the
     * timer HAL could select a driver for a page the platform never provides. */
    TEST_ASSERT(!(g_boot_info.hv_flags & HV_FLAG_TSC_ENLIGHTENMENT) ||
                    platform_get() == PLATFORM_HYPERV,
                "TSC-enlightenment flag set only under Hyper-V");
}

/* ---- XCR0 carries no supervisor state components (S5) ---- */

static void test_cet_components_absent_from_xcr0(void)
{
    /* Enabling a supervisor state component through XCR0 is architecturally
     * invalid; this pins that CET work never reaches for the wrong register. */
    if (!cpu_has(CPU_FEATURE_XSAVE)) {
        TEST_SKIP("no XSAVE, XCR0 not in use");
        return;
    }
    TEST_ASSERT(!(g_cpu.xcr0_active &
                  (XCR0_CET_U_RESERVED | XCR0_CET_S_RESERVED)),
                "XCR0 carries no CET (IA32_XSS) state components");
}

/* ---- Audit trail respects its CPUID gates (S9) ---- */

static void test_audit_arch_caps_respects_cpuid_gate(void)
{
    /* The repo rule is that msr_try_read() is a no-crash guarantee, NOT an
     * existence probe -- CPUID gates first. This asserts the direction that
     * catches an ungated read: with the MSR unadvertised, the captured value
     * must be exactly 0. The other direction is deliberately NOT asserted --
     * IA32_ARCH_CAPABILITIES enumerates that the MSR EXISTS, not that any bit
     * is set, so a part reporting 0 is legitimate. */
    struct per_cpu_data *bsp = smp_get_cpu(0);
    TEST_ASSERT_NOT_NULL(bsp, "cpu_data[0] present");
    TEST_ASSERT_EQ(bsp->audit_captured, 1u, "BSP audit ran");
    if (cpu_has(CPU_FEATURE_ARCH_CAP)) {
        TEST_SKIP("ARCH_CAPABILITIES advertised; payload is not an invariant");
        return;
    }
    TEST_ASSERT_EQ(bsp->arch_caps, 0,
                   "unadvertised ARCH_CAPABILITIES captured as 0 (gate held)");
}

static void test_audit_ucode_rev_uniform(void)
{
    /* cpu_audit_consistency_check() compares EFER.NXE, required CR4, PAT, XCR0
     * and ARCH_CAPABILITIES across CPUs but NOT the microcode revision, so a
     * core running a different patch level than the BSP is invisible today.
     * A captured 0 is a legitimate "no revision exposed" value, so the
     * invariant asserted is UNIFORMITY, not a non-zero payload. */
    struct per_cpu_data *bsp = smp_get_cpu(0);
    uint32_t i;
    uint32_t diff_at = MAX_CPUS;    /* sentinel: all uniform */
    uint32_t covered = 0;
    char msg[96];

    TEST_ASSERT_NOT_NULL(bsp, "cpu_data[0] present");

    /* cpu_slot_committed_online() is the published predicate -- it honours the
     * STARTING->ONLINE CAS as well as the is_online release edge, so a raw
     * is_online read would miss a committed AP mid-publication. */
    for (i = 1; i < MAX_CPUS; i++) {
        struct per_cpu_data *pc = smp_get_cpu(i);
        if (!pc || !cpu_slot_committed_online(pc))
            continue;
        covered++;
        if (pc->ucode_rev != bsp->ucode_rev && diff_at == MAX_CPUS)
            diff_at = i;
    }
    snprintf(msg, sizeof(msg),
             "%u online AP(s) match BSP ucode 0x%x (first differing slot %u)",
             (uint64_t)covered, (uint64_t)bsp->ucode_rev, (uint64_t)diff_at);
    TEST_ASSERT(diff_at == MAX_CPUS, msg);
}

/* ---- Registration ---- */

void test_register_cpu_seq(void)
{
    test_suite_register_cat("cpu_seq: cached NX/SSE2 match live CPUID",
        test_cached_minimums_match_live_cpuid, TEST_CAT_X86);
    test_suite_register_cat("cpu_seq: hv_vendor empty or recognized",
        test_hv_vendor_recognized, TEST_CAT_X86);
    test_suite_register_cat("cpu_seq: hv_vendor implies hypervisor platform",
        test_hv_vendor_implies_hypervisor, TEST_CAT_X86);
    test_suite_register_cat("cpu_seq: TSC enlightenment only on Hyper-V",
        test_tsc_enlightenment_only_on_hyperv, TEST_CAT_X86);
    test_suite_register_cat("cpu_seq: XCR0 carries no CET components",
        test_cet_components_absent_from_xcr0, TEST_CAT_X86);
    test_suite_register_cat("cpu_seq: audit ARCH_CAPABILITIES CPUID gate",
        test_audit_arch_caps_respects_cpuid_gate, TEST_CAT_X86);
    test_suite_register_cat("cpu_seq: audit microcode revision uniform",
        test_audit_ucode_rev_uniform, TEST_CAT_X86);
}

#endif /* KERNEL_TESTS */
