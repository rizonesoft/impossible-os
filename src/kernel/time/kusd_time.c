/* ============================================================================
 * kusd_time.c -- KUSER_SHARED_DATA page allocation, static init, ISR updates
 *
 * Allocates a physical page, maps it at 0x7FFE0000 (user read-only) and
 * at a kernel-writable address. Populates all static fields (version,
 * processor features, QPC frequency, cookie, etc.) and provides the
 * kusd_update_time() function called from the timer ISR on every tick.
 * ============================================================================ */

#include "kernel/nt/kusd.h"
#include "libc/string.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/time/wall_clock.h"
#include "kernel/time/timezone.h"
#include "kernel/nt/filetime.h"
#include "kernel/cpuid.h"
#include "kernel/smp.h"
#include "kernel/klog.h"
#include "build_info.h"

/* ---- State --------------------------------------------------------------- */

volatile KUSER_SHARED_DATA *g_kusd;
static int s_kusd_ready;

/* ---- RDRAND helper ------------------------------------------------------- */

static uint32_t kusd_rdrand32(void)
{
    uint32_t val = 0;
    if (cpu_has(CPU_FEATURE_RDRAND)) {
        int ok = 0;
        __asm__ volatile (
            "rdrand %0\n\t"
            "setc   %1\n\t"
            : "=r"(val), "=qm"(ok)
            :
            : "cc"
        );
        if (ok) return val;
    }
    /* Fallback: TSC-based */
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
    pf[PF_COMPARE_EXCHANGE128]             = 1;  /* x86-64 has CMPXCHG16B */
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
     * a single 4 KiB page with different flags (user read-only + NX) */
    vmm_split_huge_page(KUSD_USER_VA);

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
    g_kusd->TickCountMultiplier = 0x0FA00000;  /* 10 ms tick */

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

    if (!s_kusd_ready)
        return;

    /* Gather current values */
    interrupt_time = KeQueryInterruptTime();
    system_time    = (uint64_t)KeQuerySystemTime();
    tick_count     = interrupt_time / 100000;  /* 10 ms units */

    /* Timezone bias in 100 ns units */
    {
        int32_t bias_min = timezone_total_bias();
        int64_t bias_ticks = (int64_t)bias_min * 60 * (int64_t)FILETIME_TICKS_PER_SECOND;
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
