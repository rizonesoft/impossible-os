/* ============================================================================
 * kusd_time.c -- KUSER_SHARED_DATA page allocation, static init, ISR updates
 *
 * Allocates a physical page, maps it at 0x7FFE0000 (user read-only) and
 * at a kernel-writable address. Populates all static fields (version,
 * processor features, QPC frequency, cookie, etc.) and provides the
 * kusd_update_time() function called from the timer ISR on every tick.
 * ============================================================================ */

#include "kernel/nt/kusd.h"
#include "kernel/config.h"     /* kernel_config_get() for the policy mirror */
#include "kernel/abi_hash.h"   /* IMPOSSIBLE_OS_ABI_HASH for -18 AbiLayoutHash */
#include "libc/string.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/time/wall_clock.h"
#include "kernel/time/timezone.h"
#include "kernel/nt/filetime.h"
#include "kernel/cpuid.h"
#include "kernel/random.h"
#include "kernel/smp.h"
#include "kernel/klog.h"
#include "build_info.h"

/* ---- State --------------------------------------------------------------- */

volatile KUSER_SHARED_DATA *g_kusd;
static int s_kusd_ready;

/* ---- RDRAND helper -------------------------------------------------------
 * Wraps the shared rdrand_bytes() helper to deliver a single uint32_t for
 * the KUSER_SHARED_DATA Cookie field. Falls back to a TSC-derived value if
 * RDRAND is unavailable (TCG, very old CPUs). The Cookie is not security-
 * critical -- it is a cheap per-boot identifier exposed to user-mode at
 * 0x7FFE0330. */

static uint32_t kusd_rdrand32(void)
{
    uint8_t buf[4];
    if (rdrand_bytes(buf, 4)) {
        return (uint32_t)buf[0]
             | ((uint32_t)buf[1] << 8)
             | ((uint32_t)buf[2] << 16)
             | ((uint32_t)buf[3] << 24);
    }
    /* Fallback: TSC-derived */
    {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        return lo ^ (hi << 16) ^ 0xBAADF00D;
    }
}

/* ---- NtSystemRoot: ASCII to WCHAR --------------------------------------- */

static void kusd_set_system_root(volatile uint16_t *dst, const char *src,
                                  int max_wchars)
{
    int i;
    for (i = 0; i < max_wchars - 1 && src[i]; i++)
        dst[i] = (uint16_t)(uint8_t)src[i];
    dst[i] = 0;
}

/* ---- Processor features ------------------------------------------------- */

static void kusd_populate_processor_features(volatile uint8_t *pf)
{
    /* Map CPU_FEATURE_* -> Windows PF_* indices */
    pf[PF_FLOATING_POINT_PRECISION_ERRATA] = 0;
    pf[PF_FLOATING_POINT_EMULATED]         = 0;
    pf[PF_COMPARE_EXCHANGE_DOUBLE]         = 1;  /* x86-64 always has CMPXCHG8B */
    pf[PF_MMX_INSTRUCTIONS_AVAILABLE]      = cpu_has(CPU_FEATURE_MMX)   ? 1 : 0;
    pf[PF_XMMI_INSTRUCTIONS_AVAILABLE]     = cpu_has(CPU_FEATURE_SSE)   ? 1 : 0;
    pf[PF_RDTSC_INSTRUCTION_AVAILABLE]     = cpu_has(CPU_FEATURE_TSC)   ? 1 : 0;
    pf[PF_XMMI64_INSTRUCTIONS_AVAILABLE]   = cpu_has(CPU_FEATURE_SSE2)  ? 1 : 0;
    pf[PF_SSE3_INSTRUCTIONS_AVAILABLE]     = cpu_has(CPU_FEATURE_SSE3)  ? 1 : 0;
    pf[PF_COMPARE_EXCHANGE128]             = cpu_has(CPU_FEATURE_CX16) ? 1 : 0;
    pf[PF_RDRAND_INSTRUCTION_AVAILABLE]    = cpu_has(CPU_FEATURE_RDRAND)? 1 : 0;
    pf[PF_SSSE3_INSTRUCTIONS_AVAILABLE]    = cpu_has(CPU_FEATURE_SSSE3) ? 1 : 0;
    pf[PF_SSE4_1_INSTRUCTIONS_AVAILABLE]   = cpu_has(CPU_FEATURE_SSE4_1)? 1 : 0;
    pf[PF_SSE4_2_INSTRUCTIONS_AVAILABLE]   = cpu_has(CPU_FEATURE_SSE4_2)? 1 : 0;
    pf[PF_AVX_INSTRUCTIONS_AVAILABLE]      = cpu_has(CPU_FEATURE_AVX)   ? 1 : 0;
    pf[PF_AVX2_INSTRUCTIONS_AVAILABLE]     = cpu_has(CPU_FEATURE_AVX2)  ? 1 : 0;
}

/* ---- Init ---------------------------------------------------------------- */

void kusd_init(void)
{
    uintptr_t phys;
    int ret;

    /* Allocate one physical frame */
    phys = pmm_alloc_frame();
    if (!phys) {
        klog(LOG_ERROR, "kusd", "Failed to allocate physical frame for KUSD");
        return;
    }

    /* Zero the frame via identity-mapped address */
    memset((void *)phys, 0, 4096);

    /* Split the 2 MiB huge page containing 0x7FFE0000 so we can map
     * a single 4 KiB page with different flags (user read-only + NX). A failed
     * split (PMM exhaustion) leaves the huge PD entry, which makes the
     * vmm_map_page below fail-closed -- check it explicitly so the failure is
     * attributed, not silently degraded. */
    if (vmm_split_huge_page(KUSD_USER_VA) != 0) {
        klog(LOG_ERROR, "kusd", "Failed to split huge page for KUSD at 0x%X",
             (uint64_t)KUSD_USER_VA);
        return;
    }

    /* Map at fixed user VA: read-only for user mode, NX */
    ret = vmm_map_page(KUSD_USER_VA, phys,
                       VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NX);
    if (ret != 0) {
        klog(LOG_ERROR, "kusd", "Failed to map KUSD at user VA 0x%X",
             (uint64_t)KUSD_USER_VA);
        return;
    }

    /* Kernel alias: use the identity-mapped physical address directly */
    g_kusd = (volatile KUSER_SHARED_DATA *)phys;

    /* ---- Populate static fields ---- */

    /* Time multiplier: (tick_ms * 2^24) / 1000 for GetTickCount() */
    /* (TickCountQuad * TickCountMultiplier) >> 24 == milliseconds. TickCount is
     * published in 10 ms units (interrupt_time / 100000), so the multiplier is
     * 10 << 24. (Was 0x0FA00000 = 15.625 ms, which mismatched the 10 ms unit.) */
    g_kusd->TickCountMultiplier = 0x0A000000;  /* 10 ms per tick (10 << 24) */

    /* Image number: x86-64 = 0x8664 (IMAGE_FILE_MACHINE_AMD64) */
    g_kusd->ImageNumberLow  = 0x8664;
    g_kusd->ImageNumberHigh = 0x8664;

    /* NtSystemRoot: L"C:\\Impossible" */
    kusd_set_system_root(g_kusd->NtSystemRoot, "C:\\Impossible", 260);

    /* Large page: 2 MiB on x86-64 */
    g_kusd->LargePageMinimum = 0x200000;

    /* Version info */
    g_kusd->NtBuildNumber      = BUILD_NUMBER;
    g_kusd->NtProductType      = NtProductWinNt;
    g_kusd->ProductTypeIsValid = 1;
    g_kusd->NtMajorVersion     = 10;
    g_kusd->NtMinorVersion     = 0;

    /* Processor architecture: AMD64 = 9 */
    g_kusd->NativeProcessorArchitecture = 9;

    /* Processor features */
    kusd_populate_processor_features(g_kusd->ProcessorFeatures);

    /* Physical pages */
    g_kusd->NumberOfPhysicalPages = (uint32_t)pmm_get_total_frames();

    /* QPC frequency: fixed 10 MHz */
    g_kusd->QpcFrequency = (int64_t)FILETIME_TICKS_PER_SECOND;

    /* SystemCall: 0 = SYSCALL, 1 = INT 2E fallback */
    g_kusd->SystemCall = 0;

    /* Active processor count */
    g_kusd->ActiveConsoleId = 0;

    /* Security cookie (RDRAND-seeded) */
    g_kusd->Cookie = kusd_rdrand32();

    /* Kernel policy mirror: SafeBootMode + debugger + mitigation summary from
     * the immutable kernel_config snapshot, for user-mode policy consumers.
     * Written before AbiMagic so a crt_init that gates on the magic sees them. */
    {
        const kernel_config_t *kc = kernel_config_get();
        if (kc) {
            g_kusd->SafeBootMode      = kc->safe_mode;             /* 0..3 */
            g_kusd->KdDebuggerEnabled = kc->debug_enabled ? 1 : 0;
            /* Publish EFFECTIVE CI policy, not raw boot args: safe mode gates CI
             * relaxation off, so a safe boot must not advertise CI-disabled /
             * test-signing-allowed to user-mode (the single gating contract). */
            int relax_ok = safe_mode_component_allowed(
                (safe_mode_t)kc->safe_mode, SAFE_COMP_CI_RELAX);
            uint8_t eff_noci   = relax_ok ? kc->nointegritychecks : 0;
            uint8_t eff_tsign  = relax_ok ? kc->testsigning : 0;
            /* bit0 = code-integrity enforced; bit1 = test-signing allowed. */
            g_kusd->MitigationPolicies =
                (uint8_t)((eff_noci ? 0 : 0x1) | (eff_tsign ? 0x2 : 0));
        }
    }

    /* -18 self-describing ABI header at 0x340. Written AFTER every
     * Windows-compatible field above so user-mode readers see a
     * fully-initialized page when AbiMagic turns non-zero. Writes
     * from high offset to low so the magic lands last -- crt_init
     * can treat magic==KUSD_ABI_MAGIC as "header is complete"
     * without a separate ready flag. */
    g_kusd->AbiBuildTimestamp = 0;  /* future: plumb from build.sh; 0 today */
    g_kusd->AbiReserved0      = 0;
    g_kusd->AbiReserved1      = 0;
    g_kusd->AbiLayoutHash     = IMPOSSIBLE_OS_ABI_HASH;
    g_kusd->AbiStructSize     = 0x340;  /* size of Win-compat portion */
    g_kusd->AbiVersion        = KUSD_ABI_VERSION;
    g_kusd->AbiMagic          = KUSD_ABI_MAGIC;  /* WRITE LAST: makes header complete */

    s_kusd_ready = 1;

    klog(LOG_INFO, "kusd",
         "KUSER_SHARED_DATA: user=0x%X kernel=0x%X (phys=0x%X)",
         (uint64_t)KUSD_USER_VA, (uint64_t)(uintptr_t)g_kusd,
         (uint64_t)phys);
    klog(LOG_INFO, "kusd",
         "  Version: %u.%u build %u, arch=%u, cookie=0x%08X",
         (uint64_t)g_kusd->NtMajorVersion,
         (uint64_t)g_kusd->NtMinorVersion,
         (uint64_t)g_kusd->NtBuildNumber,
         (uint64_t)g_kusd->NativeProcessorArchitecture,
         (uint64_t)g_kusd->Cookie);
    klog(LOG_INFO, "kusd",
         "  PhysPages=%u, QpcFreq=%u Hz, PF[SSE]=%u PF[AVX]=%u",
         (uint64_t)g_kusd->NumberOfPhysicalPages,
         (uint64_t)g_kusd->QpcFrequency,
         (uint64_t)g_kusd->ProcessorFeatures[PF_XMMI_INSTRUCTIONS_AVAILABLE],
         (uint64_t)g_kusd->ProcessorFeatures[PF_AVX_INSTRUCTIONS_AVAILABLE]);
}

/* ---- ISR time update ----------------------------------------------------- */

void kusd_update_time(void)
{
    uint64_t interrupt_time;
    uint64_t system_time;
    uint64_t tz_bias_100ns;
    uint64_t tick_count;

    FILETIME system_ft;

    /* BSP-only: the per-tick time update (coarse cache publish + KUSD page
     * triple-writes) has a SINGLE publisher, so coarse interrupt time stays
     * monotonic and the KUSD InterruptTime/SystemTime/TickCount fields never
     * tear. The tick ISR is BSP-only today; this guard keeps the invariant when
     * AP per-CPU tick delivery lands (an AP tick must publish neither the coarse
     * cache nor the KUSD time). */
    {
        struct per_cpu_data *cpu = smp_this_cpu();
        if (cpu && cpu->cpu_id != 0)
            return;
    }

    /* Publish the coarse cache every tick BEFORE the KUSD-readiness gate, so the
     * lock-free Ke*Coarse() readers (klog/crash timestamps) go live as soon as
     * the monotonic clock is up -- not gated on kusd_init(). Before
     * mono_clock_init() mono_ns() returns 0, so the cache holds 0 then (no
     * monotonic source exists yet -- the only honest early value). One precise
     * mono_ns() sample feeds both the cache and (below) the KUSD page, so the
     * per-tick clocksource read is amortized across KUSD + every coarse reader.
     * Precise sub-us callers still use KeQuerySystemTime()/...Precise(). */
    wall_clock_tick_cache(&system_ft, &interrupt_time);

    if (!s_kusd_ready)
        return;

    system_time = (uint64_t)system_ft;
    tick_count  = interrupt_time / 100000;  /* 10 ms units */

    /* KUSD TimeZoneBias is the Win32 positive-WEST Bias (LocalTime =
     * SystemTime - TimeZoneBias). Our internal timezone_total_bias() is
     * west-NEGATIVE (Local = UTC + bias), so the ABI field is its negation. */
    {
        int32_t bias_min = timezone_total_bias();
        int64_t bias_ticks = -(int64_t)bias_min * 60 * (int64_t)FILETIME_TICKS_PER_SECOND;
        tz_bias_100ns = (uint64_t)bias_ticks;
    }

    /* Write using KSYSTEM_TIME triple-write protocol.
     * All writes are volatile -- no locks needed at CLOCK_LEVEL IRQL. */
    /* Cast through (void *) to avoid packed-member-address warning.
     * These fields are naturally aligned at their Windows-defined offsets. */
    ksystem_time_write((volatile KSYSTEM_TIME *)(void *)&g_kusd->InterruptTime,
                       interrupt_time);
    ksystem_time_write((volatile KSYSTEM_TIME *)(void *)&g_kusd->SystemTime,
                       system_time);
    ksystem_time_write((volatile KSYSTEM_TIME *)(void *)&g_kusd->TimeZoneBias,
                       tz_bias_100ns);
    ksystem_time_write((volatile KSYSTEM_TIME *)(void *)&g_kusd->TickCount,
                       tick_count);

    /* TickCountLowDeprecated: legacy field for 32-bit GetTickCount() */
    g_kusd->TickCountLowDeprecated = (uint32_t)tick_count;
}

int kusd_ready(void)
{
    return s_kusd_ready;
}
