/* ============================================================================
 * boot_init.c — Subsystem readiness oracle and boot progress tracker
 *
 * Provides:
 *   - g_subsys_ready[] table with get/set/dump helpers
 *   - boot_progress() serial progress emitter + TSC step recorder
 *   - _boot_require_failed() helper used by BOOT_REQUIRE macro
 * ============================================================================ */

#include "kernel/boot_init.h"
#include "kernel/boot_timing.h"
#include "kernel/klog.h"
#include "kernel/drivers/serial.h"

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
    /* Write directly to serial — klog may not be fully initialised yet. */
    serial_write("[BOOT] REQUIRE failed: ");
    serial_write(subsys_name);
    serial_write(" not ready\n");
}

/* ---- Boot progress emitter ----------------------------------------------- */

static void serial_write_hex8(uint8_t v)
{
    static const char hex[] = "0123456789abcdef";
    serial_putchar(hex[v >> 4]);
    serial_putchar(hex[v & 0xF]);
}

void boot_progress(uint8_t phase, const char *step, uint8_t postcode)
{
    /* "[PHASEn] step (0xNN)\n" */
    serial_write("[PHASE");
    serial_putchar('0' + (phase & 0x0F));
    serial_write("] ");
    serial_write(step);
    serial_write(" (0x");
    serial_write_hex8(postcode);
    serial_write(")\n");

    boot_timing_record_step(phase, step, postcode);
}
