/* ============================================================================
 * boot_interrupts.c — Interrupt controller and ACPI/SMP initialization
 *
 * GDT, IDT, PIC, PIT, RTC, keyboard, mouse, framebuffer, boot splash,
 * PCI, NIC, VirtIO input, VBox mouse, DHCP, ACPI, LAPIC/IOAPIC, SMP.
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
#include "kernel/drivers/pci.h"
#include "kernel/drivers/rtl8139.h"
#include "kernel/net/net.h"
#include "kernel/drivers/virtio_input.h"
#include "kernel/drivers/vbox_mouse.h"
#include "kernel/acpi.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/smp.h"
#include "kernel/boot_splash.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/partition.h"
#include "main/main_internal.h"

void boot_interrupts_init(void)
{
    /* Hyper-V debug bars — read FB from boot_info at 0x10000 */
    volatile struct boot_info *_bi =
        (volatile struct boot_info *)(uintptr_t)0x10000;
    volatile uint32_t *_fb = (volatile uint32_t *)_bi->fb.addr;
    uint32_t _pitch = _bi->fb.pitch / 4;
    uint32_t _fbw = _bi->fb.width;
    uint32_t _fbh = _bi->fb.height;
#define HV_BAR(row, color) do { \
    if (_fb && _fbw > 0) { \
        uint32_t _r, _c; \
        for (_r = (row); _r < (row) + 8 && _r < _fbh; _r++) \
            for (_c = 0; _c < 100 && _c < _fbw; _c++) \
                _fb[_r * _pitch + _c] = (color); \
    } \
} while (0)

    HV_BAR(160, 0x00FF0000);  /* RED = entered boot_interrupts_init */

    klog(LOG_DEBUG, "boot", "--- Phase: interrupt controllers & timer ---");
    boot_splash_status("Setting up interrupts...");

    gdt_init();
    HV_BAR(172, 0x0000FF00);  /* GREEN = GDT OK */

    idt_init();
    irq_init();
    HV_BAR(184, 0x0000FFFF);  /* CYAN = IDT OK */

    /* ACPI MADT must be parsed before PIC/PIT so we know:
     *   (a) PCAT_COMPAT — whether a PIC exists at all
     *   (b) IOAPIC base — for routing IRQ0 through IOAPIC
     *   (c) CPU count   — for SMP decision
     * Prerequisites: only g_boot_info (available since boot_hw_init). */
    if (g_boot_info.acpi_available) {
        klog(LOG_INFO, "acpi", "RSDP v%u at %p",
               (uint64_t)g_boot_info.acpi_version,
               g_boot_info.acpi_rsdp_addr);
        boot_splash_status("Parsing ACPI tables...");
        acpi_init();
        klog(LOG_INFO, "smp", "CPUs discovered: %u",
             (uint64_t)acpi_get_cpu_count());
    }
    HV_BAR(190, 0x00FF4500);  /* DARK ORANGE = ACPI OK */

    /* ---- APIC-first boot: LAPIC + IOAPIC before PIC/PIT ----
     *
     * With the MADT parsed, we know the IOAPIC base address and whether
     * a PIC exists (PCAT_COMPAT flag).  Initialize LAPIC and IOAPIC NOW
     * so that pit_init() IRQ0 routes through the IOAPIC redirect table
     * instead of the 8259 PIC.  This fixes Hyper-V Gen 2 (no PIC). */
    if (g_boot_info.acpi_available && acpi_get_ioapic_base() != 0) {
        boot_splash_status("Initializing LAPIC...");
        lapic_init();

        if (lapic_available()) {
            ioapic_init();

            if (ioapic_available()) {
                /* Disable PIC if platform has one — IOAPIC takes over */
                if (acpi_pcat_compat()) {
                    pic_disable();
                    klog(LOG_INFO, "irq",
                         "Switched to LAPIC/IOAPIC (PIC disabled)");
                } else {
                    klog(LOG_INFO, "irq",
                         "LAPIC/IOAPIC active (APIC-only, no PIC)");
                }

                /* Drain stale ISR bits — may exist from firmware or
                 * PIC init leaking through before we disabled it. */
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
        HV_BAR(193, 0x0080FF00);  /* LIME = LAPIC/IOAPIC OK */
    }

    /* PIC init — only when a PIC exists AND IOAPIC has NOT taken over.
     *
     * Three states:
     *   1. No ACPI: assume legacy PIC → pic_init()
     *   2. PCAT_COMPAT=1, no IOAPIC: PIC is the IRQ controller → pic_init()
     *   3. PCAT_COMPAT=1, IOAPIC active: PIC already disabled above → skip
     *   4. PCAT_COMPAT=0: no PIC exists (APIC-only) → skip
     *
     * pic_unmask_irq/pic_mask_irq are self-guarding (no-op when pic_ready=0). */
    if (!ioapic_available()) {
        if (!g_boot_info.acpi_available || acpi_pcat_compat()) {
            pic_init();
        } else {
            klog(LOG_INFO, "irq",
                 "PIC: skipped (PCAT_COMPAT=0, APIC-only platform)");
        }
    }
    HV_BAR(196, 0x00FFFF00);  /* YELLOW = PIC OK */

    /* UTS: select timer backend (PIT for TCG, LAPIC for all else).
     * This MUST happen before boot_splash_init() so sleep_ms() works.
     * On non-TCG: calibrates LAPIC, starts timer, masks PIT IRQ0.
     * On TCG: initializes PIT as wall-clock timer. */
    timer_hal_init();
    HV_BAR(208, 0x000000FF);  /* BLUE = timer OK */

    rtc_init();
    HV_BAR(220, 0x00FF8000);  /* ORANGE = RTC OK */

    boot_splash_status("Initializing input...");
    keyboard_init();
    HV_BAR(232, 0x00800080);  /* PURPLE = keyboard OK */

    mouse_init();
    HV_BAR(244, 0x00FFFFFF);  /* WHITE = mouse OK */

    klog(LOG_DEBUG, "boot", "--- Phase: display & splash ---");
    fb_init();
    HV_BAR(256, 0x00FF00FF);  /* MAGENTA = fb_init OK */

    boot_splash_init();
    boot_splash_status("Setting up hardware...");

    klog(LOG_DEBUG, "boot", "--- Phase: PCI & network hardware ---");
    boot_splash_tick();
    boot_splash_status("Detecting hardware...");
    boot_splash_status("Scanning PCI bus...");
    pci_scan();
    boot_splash_status("Initializing network...");
    rtl8139_init();
    net_init();
    virtio_input_init();
    vbox_mouse_init();

    /* Enable interrupts */
    __asm__ volatile ("sti");

    /* Start timer-driven splash animation (needs PIT IRQs running) */
    boot_splash_start_animation();

    /* DHCP fire-and-forget */
    klog(LOG_DEBUG, "boot",
         "--- Phase: network (DHCP, async fire-and-forget) ---");
    dhcp_discover();

    HV_BAR(268, 0x0000FF00);  /* GREEN = boot_interrupts complete */
#undef HV_BAR
}
