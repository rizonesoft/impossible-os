/* ============================================================================
 * kusd_time.c -- KUSER_SHARED_DATA page allocation and ISR time updates
 *
 * Allocates a physical page, maps it at 0x7FFE0000 (user read-only) and
 * at a kernel-writable address. The timer ISR calls kusd_update_time()
 * on every tick to keep InterruptTime, SystemTime, TickCount, and
 * TimeZoneBias current for lock-free user-mode reads.
 *
 * Static field population (NtSystemRoot, ProcessorFeatures, Cookie, etc.)
 * is deferred to TODO-04 PEB/TEB §11 kusd_init().
 * ============================================================================ */

#include "kernel/nt/kusd.h"
#include "libc/string.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/time/wall_clock.h"
#include "kernel/time/timezone.h"
#include "kernel/nt/filetime.h"
#include "kernel/klog.h"
#include "build_info.h"

/* ---- State --------------------------------------------------------------- */

volatile uint8_t *g_kusd;          /* kernel-writable alias */
static uintptr_t  s_kusd_phys;    /* physical frame backing the page */
static int         s_kusd_ready;

/* ---- Helpers ------------------------------------------------------------- */

/* Type-punned volatile field access at a given byte offset */
#define KUSD_FIELD(type, off) \
    ((volatile type *)(g_kusd + (off)))

/* ---- Page allocation ----------------------------------------------------- */

void kusd_page_init(void)
{
    uintptr_t phys;
    int ret;

    /* Allocate one physical frame */
    phys = pmm_alloc_frame();
    if (!phys) {
        klog(LOG_ERROR, "kusd", "Failed to allocate physical frame for KUSD");
        return;
    }
    s_kusd_phys = phys;

    /* Zero the frame via temporary identity map (it's in the identity region) */
    memset((void *)phys, 0, 4096);

    /* Map at fixed user VA: read-only for user mode */
    ret = vmm_map_page(KUSD_USER_VA, phys,
                       VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NX);
    if (ret != 0) {
        klog(LOG_ERROR, "kusd", "Failed to map KUSD at user VA 0x%X",
             (uint64_t)KUSD_USER_VA);
        return;
    }

    /* Kernel alias: use the identity-mapped physical address directly
     * (kernel has full access to identity-mapped region) */
    g_kusd = (volatile uint8_t *)phys;

    /* Populate minimal static fields */
    *KUSD_FIELD(uint32_t, KUSD_OFF_TICK_COUNT_MULTIPLIER) = 0x0FA00000;
        /* 10 ms tick -> multiplier for GetTickCount(): (10ms * 2^24) / 1000 */
    *KUSD_FIELD(uint32_t, KUSD_OFF_NT_MAJOR_VERSION) = 10;
    *KUSD_FIELD(uint32_t, KUSD_OFF_NT_MINOR_VERSION) = 0;
    *KUSD_FIELD(uint32_t, KUSD_OFF_NT_BUILD_NUMBER) = BUILD_NUMBER;

    s_kusd_ready = 1;

    klog(LOG_INFO, "kusd",
         "KUSER_SHARED_DATA: user=0x%X kernel=0x%X (phys=0x%X)",
         (uint64_t)KUSD_USER_VA, (uint64_t)(uintptr_t)g_kusd,
         (uint64_t)phys);
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

    /* Timezone bias in 100 ns units (bias_minutes * 60 * 10000000) */
    {
        int32_t bias_min = timezone_total_bias();
        int64_t bias_ticks = (int64_t)bias_min * 60 * (int64_t)FILETIME_TICKS_PER_SECOND;
        tz_bias_100ns = (uint64_t)bias_ticks;
    }

    /* Write using KSYSTEM_TIME triple-write protocol.
     * All writes are volatile -- no locks needed at CLOCK_LEVEL IRQL. */
    ksystem_time_write(KUSD_FIELD(KSYSTEM_TIME, KUSD_OFF_INTERRUPT_TIME),
                       interrupt_time);
    ksystem_time_write(KUSD_FIELD(KSYSTEM_TIME, KUSD_OFF_SYSTEM_TIME),
                       system_time);
    ksystem_time_write(KUSD_FIELD(KSYSTEM_TIME, KUSD_OFF_TIMEZONE_BIAS),
                       tz_bias_100ns);
    ksystem_time_write(KUSD_FIELD(KSYSTEM_TIME, KUSD_OFF_TICK_COUNT),
                       tick_count);
}

int kusd_ready(void)
{
    return s_kusd_ready;
}
