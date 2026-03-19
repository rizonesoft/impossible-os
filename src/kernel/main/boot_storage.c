/* ============================================================================
 * boot_storage.c — Storage, filesystem, ACPI/SMP, registry, services
 *
 * VFS init, partition scanning, filesystem mount, disk log setup,
 * system summary, ACPI/SMP, heap test, registry, symbol table, mmap.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/hw_dump.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/mmap.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/partition.h"
#include "kernel/acpi.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/drivers/pic.h"
#include "kernel/smp.h"
#include "kernel/drivers/hyperv/vmbus.h"
#include "kernel/drivers/hyperv/storvsc.h"
#include "kernel/drivers/hyperv/hv_input.h"
#include "kernel/boot_splash.h"
#include "registry.h"
#include "kernel/symtab.h"
#include "main/main_internal.h"

void boot_storage_init(uint64_t magic)
{
    uint32_t i;
    uint64_t total_ram = 0;

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Storage & Filesystem ---------------------------------------------------");

    klog(LOG_DEBUG, "boot", "--- Phase: storage & VFS ---");
    boot_splash_status("Initializing VFS...");
    vfs_init();

    klog(LOG_DEBUG, "boot", "--- Phase: partition & filesystem mount ---");
    boot_splash_tick();
    boot_splash_status("Scanning partitions...");
    partition_scan_all();
    boot_splash_status("Mounting filesystems...");
    partition_mount_filesystems();

    /* Check for debug boot flag */
    if (vfs_is_mounted('X')) {
        struct vfs_node *x_root = vfs_get_drive_root('X');
        if (x_root && x_root->ops && x_root->ops->finddir) {
            struct vfs_node *dbg = x_root->ops->finddir(x_root, "DEBUG");
            if (dbg) {
                klog(LOG_INFO, "boot",
                     "DEBUG flag found on X: -- disabling splash");
                boot_splash_abort();
                printk("\n=== DEBUG BOOT MODE ===\n");
                printk("Splash disabled. Showing live boot output.\n\n");
                klog_disk_set_live(1);
            }
        }
    }

    klog_disk_flush();
    boot_splash_tick();

    boot_splash_status("Configuring network...");
    boot_splash_tick();

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- System Summary ---------------------------------------------------------");

    /* Hardware summary */
    klog(LOG_INFO, "boot", "Multiboot2 magic verified: %x", magic);
    klog(LOG_INFO, "boot", "Running in 64-bit Long Mode");

    if (g_boot_info.fb_available) {
        klog(LOG_INFO, "gfx", "Framebuffer: %ux%ux%u at %p",
               (uint64_t)g_boot_info.fb.width,
               (uint64_t)g_boot_info.fb.height,
               (uint64_t)g_boot_info.fb.bpp,
               (uint64_t)g_boot_info.fb.addr);
    }

    /* Memory summary */
    for (i = 0; i < g_boot_info.mmap_count; i++) {
        if (g_boot_info.mmap[i].type == 1) {
            total_ram += g_boot_info.mmap[i].length;
        }
    }
    klog(LOG_INFO, "mm", "Total RAM: %u MiB (%u entries)",
         (uint64_t)(total_ram / (1024 * 1024)),
         (uint64_t)g_boot_info.mmap_count);

    /* ACPI */
    if (g_boot_info.acpi_available) {
        klog(LOG_INFO, "acpi", "RSDP v%u at %p",
               (uint64_t)g_boot_info.acpi_version,
               g_boot_info.acpi_rsdp_addr);

        boot_splash_status("Parsing ACPI tables...");
        acpi_init();

        klog(LOG_INFO, "smp", "CPUs discovered: %u",
             (uint64_t)acpi_get_cpu_count());

        /* Switch to LAPIC/IOAPIC when:
         *   (a) SMP (cpu_count > 1) — need LAPIC for inter-CPU IPI, or
         *   (b) APIC-only (PCAT_COMPAT=0) — no PIC exists (Hyper-V Gen 2).
         *
         * Single-CPU with PIC (QEMU default) stays on PIT timer to avoid
         * LAPIC timer frequency issues on TCG emulation. */
        if ((acpi_get_cpu_count() > 1 || !acpi_pcat_compat()) &&
            acpi_get_ioapic_base() != 0) {
            boot_splash_status("Initializing LAPIC...");
            lapic_init();

            if (lapic_available()) {
                ioapic_init();

                if (ioapic_available()) {
                    /* Only touch the PIC if the platform actually has one */
                    if (acpi_pcat_compat()) {
                        pic_disable();
                        klog(LOG_INFO, "irq",
                             "Switched to LAPIC/IOAPIC (PIC disabled)");
                    } else {
                        klog(LOG_INFO, "irq",
                             "LAPIC/IOAPIC active (APIC-only, no PIC)");
                    }

                    /* Drain stale ISR bits from PIC→LAPIC transition */
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

                    /* Start LAPIC timer as the primary tick source */
                    lapic_timer_init(100);
                }

                boot_splash_status("Initializing SMP...");
                smp_init();
            }
        } else {
            /* No IOAPIC — single CPU with PIC routing only */
            smp_init();
        }
    }

    /* Hyper-V VMBus — discover and connect if running on Hyper-V.
     * Must be after LAPIC init (SynIC depends on LAPIC).
     * On non-Hyper-V platforms, this returns immediately. */
    boot_splash_status("Probing Hyper-V VMBus...");
    if (vmbus_init() == 0) {
        /* VMBus connected — initialize synthetic devices */
        boot_splash_status("Initializing Hyper-V storage...");
        storvsc_init();

        boot_splash_status("Initializing Hyper-V input...");
        hv_kbd_init();
        hv_mouse_init();
    }

    /* Dump hardware info (only when live debug is active) */
    if (klog_disk_live_active()) {
        boot_splash_status("Dumping hardware info...");
        hw_dump_to_log();
    }

    /* Heap test */
    boot_splash_status("Heap self-test...");
    {
        uint8_t *a = (uint8_t *)kmalloc(64);
        uint8_t *b = (uint8_t *)kmalloc(128);
        uint8_t *c = (uint8_t *)kmalloc(256);
        uint32_t ok = 1;

        if (a) { uint32_t j; for (j = 0; j < 64; j++) a[j] = (uint8_t)j; }
        if (b) { uint32_t j; for (j = 0; j < 128; j++) b[j] = (uint8_t)(j ^ 0xAA); }
        if (c) { uint32_t j; for (j = 0; j < 256; j++) c[j] = (uint8_t)(j ^ 0x55); }

        if (a) { uint32_t j; for (j = 0; j < 64; j++) if (a[j] != (uint8_t)j) ok = 0; }
        if (b) { uint32_t j; for (j = 0; j < 128; j++) if (b[j] != (uint8_t)(j ^ 0xAA)) ok = 0; }

        kfree(b);
        b = (uint8_t *)krealloc(a, 512);
        if (b) { uint32_t j; for (j = 0; j < 64; j++) if (b[j] != (uint8_t)j) ok = 0; }

        kfree(b);
        kfree(c);

        klog(ok ? LOG_INFO : LOG_ERROR, "mm",
             "Heap: %s (used: %u, free: %u bytes)",
             ok ? "OK" : "FAIL", heap_get_used(), heap_get_free());
    }

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Registry ---------------------------------------------------------------");

    boot_splash_status("Loading registry...");
    boot_splash_tick();
    klog_disk_flush();

    boot_splash_status("Initializing registry...");
    registry_init();
    boot_splash_status("Populating registry defaults...");
    registry_populate_defaults();

    /* Load kernel symbol map for symbolic stack traces */
    symtab_init();

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- System Services --------------------------------------------------------");

    boot_splash_status("Initializing mmap...");
    mmap_init();
}
