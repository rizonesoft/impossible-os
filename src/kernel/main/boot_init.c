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
