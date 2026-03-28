/* ============================================================================
 * boot_interrupts.c -- Phase 1: Platform Services (Interrupts Enabled at End)
 *
 * GDT, IDT, ACPI, LAPIC/IOAPIC, PIC, timer, RTC, keyboard, mouse,
 * SMBIOS, UEFI info gathering, framebuffer, boot splash.
 * Interrupts enabled with STI only after LAPIC/timer are ready.
 *
 * Provides: boot_phase1() and the legacy boot_interrupts_init() wrapper.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/irq.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/pit.h"
#include "kernel/timer.h"
#include "kernel/drivers/rtc.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/mouse.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/acpi.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_init.h"
#include "kernel/boot_halt.h"
#include "kernel/uefi_config.h"
#include "kernel/uefi_runtime.h"
#include "kernel/smbios.h"
#include "kernel/boot_timing.h"
#include "kernel/smp.h"
#include "main/main_internal.h"

/* ---- Phase 1 ------------------------------------------------------------ */

void boot_phase1(void)
{
    klog(LOG_DEBUG, "boot", "--- Phase: interrupt controllers & timer ---");
    boot_splash_status("Setting up interrupts...");

    /* --- GDT: no prerequisites --- */
    POST16(POST16_GDT);
    gdt_init();
    POST16(POST16_GDT_OK);
    kernel_subsystem_set_ready(SUBSYS_GDT, true);
    boot_progress(1, "GDT", POSTCODE_GDT_INIT);

    /* --- IDT + IRQ: requires GDT --- */
    if (!kernel_subsystem_ready(SUBSYS_GDT)) {
        kernel_subsystem_dump();
        boot_halt("GDT not ready -- cannot init IDT");
    }
    POST16(POST16_IDT);
    idt_init();
    irq_init();
    POST16(POST16_IDT_OK);
    kernel_subsystem_set_ready(SUBSYS_IDT, true);
    boot_progress(1, "IDT", POSTCODE_IDT_INIT);

    /* --- ACPI: requires IDT --- */
    if (g_boot_info.acpi_available) {
        klog(LOG_INFO, "acpi", "RSDP v%u at %p",
               (uint64_t)g_boot_info.acpi_version,
               g_boot_info.acpi_rsdp_addr);
        boot_splash_status("Parsing ACPI tables...");
        POST16(POST16_ACPI);
        acpi_init();
        POST16(POST16_ACPI_OK);
        kernel_subsystem_set_ready(SUBSYS_ACPI, true);
        klog(LOG_INFO, "smp", "CPUs discovered: %u",
             (uint64_t)acpi_get_cpu_count());
        boot_progress(1, "ACPI", POSTCODE_ACPI_INIT);
    }

    /* --- LAPIC + IOAPIC: APIC-first boot ---
     *
     * With the MADT parsed, we know the IOAPIC base address and whether
     * a PIC exists (PCAT_COMPAT flag).  Initialize LAPIC and IOAPIC NOW
     * so that pit_init() IRQ0 routes through the IOAPIC redirect table
     * instead of the 8259 PIC.  This fixes Hyper-V Gen 2 (no PIC). */
    if (g_boot_info.acpi_available && acpi_get_ioapic_base() != 0) {
        boot_splash_status("Initializing LAPIC...");
        POST16(POST16_LAPIC);
        lapic_init();

        if (lapic_available()) {
            kernel_subsystem_set_ready(SUBSYS_LAPIC, true);
            ioapic_init();

            if (ioapic_available()) {
                kernel_subsystem_set_ready(SUBSYS_IOAPIC, true);

                /* Disable PIC if platform has one -- IOAPIC takes over */
                if (acpi_pcat_compat()) {
                    pic_disable();
                    klog(LOG_INFO, "irq",
                         "Switched to LAPIC/IOAPIC (PIC disabled)");
                } else {
                    klog(LOG_INFO, "irq",
                         "LAPIC/IOAPIC active (APIC-only, no PIC)");
                }

                /* Drain stale ISR bits */
                {
                    uint32_t isr_dirty = 1;
                    uint32_t drain_rounds = 0;
                    while (isr_dirty && drain_rounds < 256) {
                        isr_dirty = 0;
                        uint32_t ri;
                        for (ri = 0; ri < 8; ri++) {
                            uint32_t isr_val =
                                lapic_read(0x100 + ri * 0x10);
                            if (isr_val) {
                                isr_dirty = 1;
                                if (drain_rounds == 0)
                                    klog(LOG_DEBUG, "lapic",
                                         "ISR[%u]=0x%x (stale)",
                                         (uint64_t)ri,
                                         (uint64_t)isr_val);
                            }
                        }
                        if (isr_dirty) {
                            lapic_eoi();
                            drain_rounds++;
                        }
                    }
                    if (drain_rounds > 0)
                        klog(LOG_INFO, "lapic",
                             "ISR drain: %u EOIs sent",
                             (uint64_t)drain_rounds);
                }
            }
        }
        boot_progress(1, "LAPIC_IOAPIC", POSTCODE_LAPIC_INIT);

        /* NOTE: ahci_setup_interrupts() moved to Phase 2, after ahci_init()
         * has discovered the PCI device.  LAPIC is ready here, so MSI will
         * work when called from Phase 2. */
    }

    /* --- PIC init: only when PIC exists AND IOAPIC has NOT taken over --- */
    if (!ioapic_available()) {
        if (!g_boot_info.acpi_available || acpi_pcat_compat()) {
            pic_init();
        } else {
            klog(LOG_INFO, "irq",
                 "PIC: skipped (PCAT_COMPAT=0, APIC-only platform)");
        }
    }

    /* --- RTC --- */
    POST16(POST16_RTC);
    rtc_init();
    POST16(POST16_RTC_OK);
    kernel_subsystem_set_ready(SUBSYS_RTC, true);
    boot_progress(1, "RTC", POSTCODE_RTC_INIT);

    /* --- Input devices --- */
    boot_splash_status("Initializing input...");
    POST16(POST16_KBD);
    keyboard_init();
    POST16(POST16_KBD_OK);
    boot_progress(1, "KEYBOARD", POSTCODE_KBD_INIT);
    /* mouse_init() skipped — crashes on i5-11600K laptop (touchpad, no PS/2 mouse) */
    /* POST16(POST16_MOUSE); mouse_init(); POST16(POST16_MOUSE_OK); */

    /* --- Framebuffer + boot splash --- */
    klog(LOG_DEBUG, "boot", "--- Phase: display & splash ---");
    POST16(POST16_FB);
    fb_init();
    POST16(POST16_FB_OK);
    kernel_subsystem_set_ready(SUBSYS_FB, true);
    boot_progress(1, "FB", POSTCODE_FB_INIT);
    POST16(POST16_SPLASH);
    boot_splash_init();
    POST16(POST16_SPLASH_OK);

    /* --- Phase 1 info gathering --- */
    uefi_config_init();
    POST16(POST16_SMBIOS);
    smbios_init();
    POST16(POST16_SMBIOS_OK);
    boot_progress(1, "SMBIOS", POSTCODE_SMBIOS_INIT);
    esrt_init();
    mat_init();
    uefi_conformance_init();
    uefi_capsule_init();
    uefi_crypto_agility_init();
    secureboot_keys_init();
    POST16(POST16_BOOT_TIMING);
    boot_timing_init();
    POST16(POST16_BOOT_TIMING_OK);

    /* GOP mode enumeration report */
    {
        static const char *pf_names[] = { "RGBX", "BGRX", "BitMask" };
        uint32_t mc = g_boot_info.gop_mode_count;
        uint32_t sel = g_boot_info.gop_mode_selected;
        if (mc > 0) {
            const char *pf = (g_boot_info.fb.pixel_format < 3) ?
                pf_names[g_boot_info.fb.pixel_format] : "Unknown";
            klog(LOG_INFO, "GOP",
                 "%ux%u %s (mode %u of %u available)",
                 g_boot_info.fb.width, g_boot_info.fb.height,
                 pf, sel, mc);
        }
    }

    /* --- Timer + enable interrupts ---
     * Start LAPIC timer LAST, right before sti.  The timer generates
     * pending interrupts immediately — if started earlier, any code
     * that might enable interrupts (UEFI runtime, firmware SMI) will
     * crash on bare metal because the timer fires into the wrong context. */
    if (!kernel_subsystem_ready(SUBSYS_IDT)) {
        kernel_subsystem_dump();
        boot_halt("IDT not ready -- cannot init timer");
    }
    POST16(POST16_TIMER);
    timer_hal_init();
    POST16(POST16_TIMER_OK);
    kernel_subsystem_set_ready(SUBSYS_TIMER, true);
    boot_progress(1, "TIMER", POSTCODE_TIMER_INIT);

    smp_early_bsp_init();

    __asm__ volatile ("sti");
    boot_splash_start_animation();
    boot_splash_status("Setting up hardware...");

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "[PHASE1] complete -- interrupts, timer, display ready");
}

/* ---- Legacy wrapper (until main.c switches to boot_phase1) -------------- */

void boot_interrupts_init(void)
{
    boot_phase1();
}
