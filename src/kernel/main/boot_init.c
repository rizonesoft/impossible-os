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
    /* Write directly to serial — klog may not be fully initialised yet. */
    serial_write("[BOOT] REQUIRE failed: ");
    serial_write(subsys_name);
    serial_write(" not ready\n");
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
    if (!g_boot_info.config.postcode) return;

    /* I/O port 0x80: write high byte (hardware POST cards are 8-bit) */
    __asm__ volatile("outb %0, $0x80" :: "a"((uint8_t)(code >> 8)));

    /* UEFI NVRAM: store full 16-bit value (backward-compatible: old reader
     * sees high byte as the 1-byte variable, new reader sees both bytes) */
    {
        uint32_t lvt_saved = 0;
        int need_mask = lapic_available() &&
                        kernel_subsystem_ready(SUBSYS_TIMER);
        if (need_mask) {
            lvt_saved = lapic_read(LAPIC_REG_LVT_TIMER);
            lapic_write(LAPIC_REG_LVT_TIMER, lvt_saved | LVT_MASKED);
        }

        uefi_set_variable(&s_post_guid, s_post_name, POST_ATTRS,
                          sizeof(code), &code);

        if (need_mask)
            lapic_write(LAPIC_REG_LVT_TIMER, lvt_saved);
    }

    /* On-screen display if framebuffer is available */
    post_display16(code);
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
    serial_write("[PHASE");
    serial_putchar('0' + (phase & 0x0F));
    serial_write("] ");
    serial_write(step);
    serial_write(" (0x");
    serial_write_hex16(postcode);
    serial_write(")\n");

    boot_timing_record_step(phase, step, postcode);
    boot_post_write16(postcode);
    post_display16(postcode);

    boot_splash_status(step);
}
