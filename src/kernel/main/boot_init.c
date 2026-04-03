/* ============================================================================
 * boot_init.c -- Subsystem readiness oracle and boot progress tracker
 *
 * Provides:
 *   - g_subsys_ready[] table with get/set/dump helpers
 *   - boot_progress() serial progress emitter + TSC step recorder
 *   - _boot_require_failed() helper used by BOOT_REQUIRE macro
 * ============================================================================ */

#include "kernel/boot_init.h"
#include "kernel/boot_timing.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_progress.h"
#include "kernel/klog.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/lapic.h"
#include "kernel/uefi_runtime.h"
#include "kernel/boot_info.h"

/* ---- Subsystem readiness table ------------------------------------------ */

static bool g_subsys_ready[SUBSYS_COUNT];

static const char *const s_subsys_names[SUBSYS_COUNT] = {
    "SERIAL",   /* 0  */
    "PMM",      /* 1  */
    "VMM",      /* 2  */
    "HEAP",     /* 3  */
    "KLOG",     /* 4  */
    "GDT",      /* 5  */
    "IDT",      /* 6  */
    "ACPI",     /* 7  */
    "LAPIC",    /* 8  */
    "IOAPIC",   /* 9  */
    "TIMER",    /* 10 */
    "RTC",      /* 11 */
    "FB",       /* 12 */
    "VFS",      /* 13 */
    "REGISTRY", /* 14 */
    "SCHED",    /* 15 */
    "IPC",      /* 16 */
    "SMP",      /* 17 */
    "EXEC",     /* 18 */
    "DESKTOP",  /* 19 */
};

bool kernel_subsystem_ready(kernel_subsys_t subsys)
{
    if ((uint32_t)subsys >= SUBSYS_COUNT) return false;
    return g_subsys_ready[subsys];
}

void kernel_subsystem_set_ready(kernel_subsys_t subsys, bool ok)
{
    if ((uint32_t)subsys >= SUBSYS_COUNT) return;
    g_subsys_ready[subsys] = ok;
}

void kernel_subsystem_dump(void)
{
    klog(LOG_INFO, "BOOT", "--- Subsystem readiness ---");
    for (uint32_t i = 0; i < SUBSYS_COUNT; i++) {
        klog(LOG_INFO, "BOOT", "  [%s] %s",
             g_subsys_ready[i] ? "OK  " : "FAIL",
             s_subsys_names[i]);
    }
}

/* ---- BOOT_REQUIRE helper ------------------------------------------------- */

void _boot_require_failed(const char *subsys_name)
{
    /* Write directly to serial -- klog may not be fully initialised yet. */
    serial_write("[BOOT] REQUIRE failed: ");
    serial_write(subsys_name);
    serial_write(" not ready\n");
}

/* ---- Deferred init registration ----------------------------------------- */

typedef struct {
    const char    *name;
    boot_result_t (*fn)(void);
} deferred_entry_t;

static deferred_entry_t g_deferred[BOOT_DEFERRED_MAX];
static uint32_t         g_deferred_count;

int boot_defer(const char *name, boot_result_t (*fn)(void))
{
    if (g_deferred_count >= BOOT_DEFERRED_MAX) {
        klog(LOG_ERROR, "boot", "deferred init full (%u/%u) -- cannot defer %s",
             g_deferred_count, (uint32_t)BOOT_DEFERRED_MAX, name ? name : "?");
        return -1;
    }
    g_deferred[g_deferred_count].name = name;
    g_deferred[g_deferred_count].fn   = fn;
    g_deferred_count++;
    klog(LOG_DEBUG, "boot", "deferred: registered %s (slot %u)",
         name ? name : "?", g_deferred_count - 1);
    return 0;
}

void boot_run_deferred(void)
{
    extern uint64_t system_get_ticks(void);
    uint32_t i;

    if (g_deferred_count == 0)
        return;

    POST16(POST16_DEFERRED);
    klog(LOG_INFO, "boot", "--- Running %u deferred init(s) ---",
         g_deferred_count);

    for (i = 0; i < g_deferred_count; i++) {
        const char *name = g_deferred[i].name ? g_deferred[i].name : "?";
        uint64_t t0 = system_get_ticks();
        boot_result_t r = g_deferred[i].fn();
        uint64_t elapsed_ms = (system_get_ticks() - t0) * 10;

        if (r == BOOT_OK || r == BOOT_DEGRADED) {
            klog(LOG_INFO, "boot", "[DEFERRED] %s +%ums", name,
                 (uint32_t)elapsed_ms);
        } else {
            klog(LOG_WARN, "boot", "[DEFERRED] %s FAILED +%ums", name,
                 (uint32_t)elapsed_ms);
        }
    }

    POST16(POST16_DEFERRED_OK);
    boot_progress(3, "DEFERRED", POST16_DEFERRED_OK);
}

/* ---- Async subsystem init (SMP parallel) -------------------------------- */

#include "kernel/smp.h"
#include "kernel/idt.h"
#include "kernel/timer.h"
#include "kernel/barrier.h"

/* IPI handler: runs on AP when it receives IPI_VECTOR_ASYNC_INIT.
 * Reads the work function from per-CPU data, executes it, writes result. */
static uint64_t async_ipi_handler(struct interrupt_frame *frame)
{
    struct per_cpu_data *pcpu = smp_this_cpu();

    if (!pcpu || !pcpu->async_fn) {
        /* Spurious -- no work assigned */
        extern void lapic_eoi(void);
        lapic_eoi();
        return (uint64_t)frame;
    }

    pcpu->in_async_work = 1;
    smp_mb();

    boot_result_t (*fn)(void) = (boot_result_t (*)(void))pcpu->async_fn;
    const char *name = pcpu->async_name ? pcpu->async_name : "?";

    klog(LOG_INFO, "ASYNC", "[ASYNC] %s started on CPU%u", name, pcpu->cpu_id);
    POST16(POST16_ASYNC_AP);

    uint64_t t0 = system_get_ticks();
    boot_result_t r = fn();
    uint64_t elapsed_ms = (system_get_ticks() - t0) * 10;

    pcpu->async_result = (uint8_t)r;
    pcpu->in_async_work = 0;
    smp_mb();
    pcpu->async_done = 1;
    smp_mb();

    if (r == BOOT_OK || r == BOOT_DEGRADED) {
        klog(LOG_INFO, "ASYNC", "[ASYNC] %s completed on CPU%u in %ums",
             name, pcpu->cpu_id, (uint32_t)elapsed_ms);
    } else {
        klog(LOG_WARN, "ASYNC", "[ASYNC] %s FAILED on CPU%u in %ums",
             name, pcpu->cpu_id, (uint32_t)elapsed_ms);
    }

    extern void lapic_eoi(void);
    lapic_eoi();
    return (uint64_t)frame;
}

void boot_async_init(void)
{
    idt_register_handler(IPI_VECTOR_ASYNC_INIT, async_ipi_handler);
    klog(LOG_DEBUG, "ASYNC", "Async init IPI handler registered (vector 0x%x)",
         (uint32_t)IPI_VECTOR_ASYNC_INIT);
}

boot_result_t boot_async_group(const char *group_name,
                               boot_async_step_t *steps, uint32_t count)
{
    extern void lapic_send_ipi(uint8_t target_apic_id, uint8_t vector);

    if (count == 0) return BOOT_OK;

    uint32_t ncpus = smp_cpu_count();
    uint32_t bsp_id = smp_cpu_id();

    POST16(POST16_ASYNC);
    klog(LOG_INFO, "ASYNC", "--- Async group '%s': %u step(s) on %u CPU(s) ---",
         group_name, count, ncpus);

    /* If only 1 CPU, run everything sequentially on BSP */
    if (ncpus <= 1) {
        boot_result_t worst = BOOT_OK;
        for (uint32_t i = 0; i < count; i++) {
            klog(LOG_INFO, "ASYNC", "[ASYNC] %s (sequential, 1 CPU)",
                 steps[i].name);
            uint64_t t0 = system_get_ticks();
            boot_result_t r = steps[i].fn();
            uint64_t elapsed_ms = (system_get_ticks() - t0) * 10;
            klog(LOG_INFO, "ASYNC", "[ASYNC] %s completed in %ums",
                 steps[i].name, (uint32_t)elapsed_ms);
            if (r > worst) worst = r;
        }
        POST16(POST16_ASYNC_DONE);
        return worst;
    }

    /* Dispatch steps across APs, BSP takes one step too.
     * Round-robin: step 0 -> BSP, step 1 -> AP1, step 2 -> AP2, etc. */
    /* Clear async state on all APs */
    for (uint32_t i = 0; i < ncpus; i++) {
        struct per_cpu_data *pcpu = smp_get_cpu(i);
        if (pcpu) {
            pcpu->async_done = 0;
            pcpu->async_result = (uint8_t)BOOT_OK;
            pcpu->async_fn = (void *)0;
            pcpu->async_name = (void *)0;
            pcpu->in_async_work = 0;
        }
    }
    smp_mb();

    /* Assign work to APs first (skip BSP = cpu 0) */
    uint32_t ap_idx = 1;  /* start with AP1 */
    for (uint32_t i = 1; i < count && ap_idx < ncpus; i++, ap_idx++) {
        struct per_cpu_data *ap = smp_get_cpu(ap_idx);
        if (!ap || !ap->is_online) continue;

        ap->async_name = steps[i].name;
        ap->async_fn   = (void *)steps[i].fn;
        smp_mb();

        /* Send IPI to wake the AP */
        lapic_send_ipi(ap->lapic_id, IPI_VECTOR_ASYNC_INIT);
    }

    /* BSP runs step 0 directly */
    klog(LOG_INFO, "ASYNC", "[ASYNC] %s started on CPU%u (BSP)",
         steps[0].name, bsp_id);
    uint64_t t0 = system_get_ticks();
    boot_result_t bsp_result = steps[0].fn();
    uint64_t bsp_ms = (system_get_ticks() - t0) * 10;
    klog(LOG_INFO, "ASYNC", "[ASYNC] %s completed on CPU%u (BSP) in %ums",
         steps[0].name, bsp_id, (uint32_t)bsp_ms);

    /* If more steps than CPUs, BSP runs the remaining ones sequentially */
    for (uint32_t i = ncpus; i < count; i++) {
        klog(LOG_INFO, "ASYNC", "[ASYNC] %s (overflow, BSP)", steps[i].name);
        boot_result_t r = steps[i].fn();
        if (r > bsp_result) bsp_result = r;
    }

    /* Barrier: wait for all APs to complete */
    POST16(POST16_ASYNC_BARRIER);
    uint32_t timeout_ms = 10000;  /* 10 second timeout */
    uint64_t deadline = system_get_ticks() + timeout_ms / 10;

    for (uint32_t i = 1; i < count && i < ncpus; i++) {
        struct per_cpu_data *ap = smp_get_cpu(i);
        if (!ap) continue;

        while (!ap->async_done) {
            if (system_get_ticks() > deadline) {
                klog(LOG_WARN, "ASYNC",
                     "[ASYNC] TIMEOUT: %s on CPU%u did not complete in %ums",
                     ap->async_name ? ap->async_name : "?",
                     ap->cpu_id, timeout_ms);
                ap->async_result = (uint8_t)BOOT_FATAL;
                ap->async_done = 1;
                break;
            }
            __asm__ volatile("pause");
        }
    }

    /* Collect results */
    boot_result_t worst = bsp_result;
    for (uint32_t i = 1; i < count && i < ncpus; i++) {
        struct per_cpu_data *ap = smp_get_cpu(i);
        if (ap && (boot_result_t)ap->async_result > worst)
            worst = (boot_result_t)ap->async_result;
    }

    POST16(POST16_ASYNC_DONE);
    klog(LOG_INFO, "ASYNC", "--- Async group '%s' complete (worst=%u) ---",
         group_name, (uint32_t)worst);

    return worst;
}

/* ---- UEFI NVRAM POST code persistence ----------------------------------- */

/* Impossible OS POST GUID: {494D504F-5354-4F53-504F-535447554944} */
static const struct boot_uefi_guid s_post_guid = {
    0x494D504F, 0x5354, 0x4F53,
    { 0x50, 0x4F, 0x53, 0x54, 0x47, 0x55, 0x49, 0x44 }
};

/* UCS-2 variable name: "ImpossiblePOST" */
static const uint16_t s_post_name[] = {
    'I','m','p','o','s','s','i','b','l','e','P','O','S','T', 0
};

/* Attributes for persistent boot-visible variable */
#define POST_ATTRS (EFI_VARIABLE_NON_VOLATILE | \
                    EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                    EFI_VARIABLE_RUNTIME_ACCESS)

/* ---- 16-bit POST code system --------------------------------------------- */

void boot_post_write16(uint16_t code)
{
    /* I/O port 0x80: always write -- zero cost, works before anything */
    __asm__ volatile("outb %0, $0x80" :: "a"((uint8_t)(code >> 8)));

    /* On-screen display -- direct VRAM, works before fb_init */
    post_display16(code);

    /* No NVRAM write here -- flash has limited endurance (~100K cycles).
     * NVRAM is written only twice per boot via boot_post_nvram_write16():
     *   1. boot_phase0: mark "booting" (entry POST code)
     *   2. boot_phase3: mark "succeeded" (POST16_BOOT_OK)
     * If the OS crashes between those two writes, next boot reads the
     * entry code and knows the previous boot failed. */
}

void boot_post_nvram_write16(uint16_t code)
{
    uint32_t lvt_saved = 0;
    int need_mask = lapic_available() &&
                    kernel_subsystem_ready(SUBSYS_TIMER);
    if (need_mask) {
        lvt_saved = lapic_read(LAPIC_REG_LVT_TIMER);
        lapic_write(LAPIC_REG_LVT_TIMER, lvt_saved | LVT_MASKED);
    }

    uefi_set_variable(&s_post_guid, s_post_name,
                       POST_ATTRS, sizeof(code), &code);

    if (need_mask)
        lapic_write(LAPIC_REG_LVT_TIMER, lvt_saved);
}

int boot_post_read16(void)
{
    uint16_t val = 0;
    uint64_t sz = sizeof(val);
    uint32_t attrs = 0;

    uint64_t status = uefi_get_variable(&s_post_guid, s_post_name,
                                        &attrs, &sz, &val);
    /* Backward-compatible: if only 1 byte was stored, treat as high byte */
    if (status == 0 && sz == 1)
        return (int)((uint16_t)(*(uint8_t *)&val) << 8);
    if (status == 0 && sz == 2)
        return (int)val;
    return -1;
}

/* ---- Boot progress emitter ----------------------------------------------- */

static void serial_write_hex16(uint16_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_putchar(hex[(v >> 12) & 0xF]);
    serial_putchar(hex[(v >>  8) & 0xF]);
    serial_putchar(hex[(v >>  4) & 0xF]);
    serial_putchar(hex[ v        & 0xF]);
}

void boot_progress(uint8_t phase, const char *step, uint16_t postcode)
{
    /* "[PHASEn] step (0xNNNN)\n" */
    const char *safe_step = step ? step : "(null)";
    serial_write("[PHASE");
    serial_putchar('0' + (phase & 0x0F));
    serial_write("] ");
    serial_write(safe_step);
    serial_write(" (0x");
    serial_write_hex16(postcode);
    serial_write(")\n");

    boot_timing_record_step(phase, safe_step, postcode);
    boot_post_write16(postcode);
    post_display16(postcode);

    /* VPD Tier 1: show named stage with status indicator */
    {
        extern void vpd_stage_begin(uint8_t, const char *, uint16_t);
        extern int vpd_is_active(void);
        if (vpd_is_active())
            vpd_stage_begin(phase, safe_step, postcode);
    }

    /* NOTE: do NOT call boot_splash_status() here -- the step name is an
     * internal identifier (e.g. "EXEC", "PCI_NET"), not a user-friendly
     * message.  Friendly status is set by explicit boot_splash_status()
     * calls before each boot_progress() in the boot sequence. */
}
