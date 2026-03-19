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
#include "kernel/drivers/pic.h"
#include "kernel/drivers/pit.h"
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
    klog(LOG_DEBUG, "boot", "--- Phase: interrupt controllers & timer ---");
    /* GDT, IDT, PIC, PIT — must happen before boot splash
     * so that sleep_ms() works correctly during the fade-in animation. */
    boot_splash_status("Setting up interrupts...");
    gdt_init();
    idt_init();
    pic_init();   /* Always remap PIC early — PIT needs IRQ0 for boot splash.
                   * On APIC-only platforms (Hyper-V Gen 2), these writes are
                   * harmlessly dropped.  pic_disable() is called later when
                   * LAPIC/IOAPIC takes over (only if PCAT_COMPAT=1). */
    pit_init();
    rtc_init();
    boot_splash_status("Initializing input...");
    keyboard_init();
    mouse_init();

    klog(LOG_DEBUG, "boot", "--- Phase: display & splash ---");
    /* Initialize framebuffer */
    fb_init();

    /* Boot splash: black screen + icon + animated dots + status text. */
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

    /* DHCP fire-and-forget — overlaps with disk scanning */
    klog(LOG_DEBUG, "boot",
         "--- Phase: network (DHCP, async fire-and-forget) ---");
    dhcp_discover();

    /* ACPI and SMP init — deferred to boot_storage_init via the
     * main sequence, since ACPI parsing needs memory but not FS. */
}
