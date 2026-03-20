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
#include "kernel/drivers/pit.h"
#include "kernel/smp.h"
#include "kernel/drivers/hyperv/vmbus.h"
#include "kernel/drivers/hyperv/storvsc.h"
#include "kernel/drivers/hyperv/hv_input.h"
#include "kernel/boot_splash.h"
#include "registry.h"
#include "kernel/symtab.h"
#include "kernel/cpuid_platform.h"
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
    boot_splash_status("Checking boot flags...");

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

    /* Flush accumulated log to disk — but skip on TCG because writing
     * 100+ entries via software-emulated AHCI DMA stalls for minutes. */
    if (!platform_is_tcg())
        klog_disk_flush();
    boot_splash_tick();

    boot_splash_status("Configuring system...");

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

    /* Timer init already happened in boot_interrupts_init() → timer_hal_init().
     * By this point g_system_timer is assigned and sleep_ms() works.
     * We just need to bring up secondary CPUs via SMP init. */
    if (g_boot_info.acpi_available) {
        boot_splash_status("Initializing SMP...");
        smp_init();
    }

    /* Hyper-V VMBus — discover and connect if running on Hyper-V.
     * Must be after LAPIC init (SynIC depends on LAPIC).
     * On non-Hyper-V platforms, this returns immediately. */
    boot_splash_status("Probing Hyper-V VMBus...");
    if (vmbus_init() == 0) {
        /* VMBus connected — initialize synthetic devices */
        boot_splash_status("Initializing Hyper-V storage...");
        if (storvsc_init() == 0) {
            /* StorVSC registered "hyperv0" as a new block device.
             * Re-scan partitions and mount filesystems — this is needed
             * on Hyper-V Gen 2 where there is no AHCI, only StorVSC. */
            boot_splash_status("Scanning Hyper-V partitions...");
            partition_scan_all();
            boot_splash_status("Mounting Hyper-V filesystems...");
            partition_mount_filesystems();
        }

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
