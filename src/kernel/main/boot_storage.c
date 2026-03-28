/* ============================================================================
 * boot_storage.c -- Phase 2: System Services
 *
 * PCI, peripherals, disk drivers, VFS, partition mount, registry, SMP,
 * network, symbol table, mmap. BOOT_FATAL only if VFS or registry are
 * completely broken; everything else degrades.
 *
 * Provides: boot_phase2() and the legacy boot_storage_init() wrapper.
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
#include "kernel/smp.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_init.h"
#include "kernel/boot_halt.h"
#include "kernel/boot_recovery.h"
#include "registry.h"
#include "kernel/symtab.h"
#include "kernel/cpuid_platform.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/xhci.h"
#include "kernel/drivers/rtl8139.h"
#include "kernel/net/net.h"
#include "kernel/drivers/virtio_input.h"
#include "kernel/drivers/vbox_mouse.h"
#include "kernel/drivers/ata.h"
#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/ahci.h"
#include "kernel/hv_bar.h"
#include "main/main_internal.h"

/* ---- Phase 2 ------------------------------------------------------------ */

void boot_phase2(void)
{
    uint32_t i;
    uint64_t total_ram = 0;

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Storage & Filesystem ---------------------------------------------------");

    /* --- PCI + peripherals (moved from Phase 1) --- */
    klog(LOG_DEBUG, "boot", "--- Phase: PCI & peripherals ---");
    boot_splash_tick();
    boot_splash_status("Scanning PCI bus...");
    pci_scan();
    xhci_init();
    boot_splash_status("Initializing network...");
    rtl8139_init();
    net_init();
    virtio_input_init();
    vbox_mouse_init();
    boot_progress(2, "PCI_NET", POSTCODE_PCI_INIT);

    /* DHCP fire-and-forget */
    dhcp_discover();
    boot_progress(2, "DHCP", POSTCODE_NET_INIT);

    /* --- Disk drivers (moved from Phase 0) --- */
    klog(LOG_DEBUG, "boot", "--- Phase: disk drivers ---");
    boot_splash_status("Initializing storage...");
    ata_init();
    virtio_blk_init();
    ahci_init();
    ahci_setup_interrupts();  /* MSI/INTx — needs LAPIC (Phase 1) + AHCI PCI device (just found) */
    blkdev_register_all();
    boot_progress(2, "STORAGE_DRV", POSTCODE_STORAGE_INIT);

    /* --- VFS: requires HEAP --- */
    if (!kernel_subsystem_ready(SUBSYS_HEAP)) {
        boot_recovery_info_t ri = { SUBSYS_VFS, POSTCODE_VFS_INIT, BOOT_FATAL, 2 };
        kernel_subsystem_dump();
        boot_recovery_show(&ri);
        boot_halt("HEAP not ready -- cannot init VFS");
    }
    klog(LOG_DEBUG, "boot", "--- Phase: storage & VFS ---");
    boot_splash_status("Initializing VFS...");
    vfs_init();
    kernel_subsystem_set_ready(SUBSYS_VFS, true);
    HV_BAR(11);  /* white: VFS done */
    boot_progress(2, "VFS", POSTCODE_VFS_INIT);

    /* --- Partition scan + filesystem mount --- */
    klog(LOG_DEBUG, "boot", "--- Phase: partition & filesystem mount ---");
    boot_splash_tick();
    boot_splash_status("Scanning partitions...");
    partition_scan_all();
    boot_splash_status("Mounting filesystems...");
    partition_mount_filesystems();
    boot_splash_status("Checking boot flags...");

    /* Check for debug boot flag on X: */
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

    /* --- klog disk enable: Phase 2 VFS-backed logging --- */
    if (!platform_is_tcg())
        klog_disk_enable();
    boot_splash_tick();

    /* --- System summary --- */
    boot_splash_status("Configuring system...");

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- System Summary ---------------------------------------------------------");

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
        if (g_boot_info.mmap[i].type == 1)
            total_ram += g_boot_info.mmap[i].length;
    }
    klog(LOG_INFO, "mm", "Total RAM: %u MiB (%u entries)",
         (uint64_t)(total_ram / (1024 * 1024)),
         (uint64_t)g_boot_info.mmap_count);

    /* --- SMP: bringup APs --- */
    if (g_boot_info.acpi_available) {
        boot_splash_status("Initializing SMP...");
        smp_init();
        kernel_subsystem_set_ready(SUBSYS_SMP, true);
        boot_progress(2, "SMP", POSTCODE_SMP_INIT);
    }

    /* Hardware dump (only in live debug mode) */
    if (klog_disk_live_active()) {
        boot_splash_status("Dumping hardware info...");
        hw_dump_to_log();
    }

    /* Heap self-test */
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

    /* --- Registry: requires VFS --- */
    if (!kernel_subsystem_ready(SUBSYS_VFS)) {
        boot_recovery_info_t ri = { SUBSYS_REGISTRY, POSTCODE_REGISTRY_INIT, BOOT_FATAL, 2 };
        kernel_subsystem_dump();
        boot_recovery_show(&ri);
        boot_halt("VFS not ready -- cannot init registry");
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
    kernel_subsystem_set_ready(SUBSYS_REGISTRY, true);
    boot_progress(2, "REGISTRY", POSTCODE_REGISTRY_INIT);

    /* --- Symbol table --- */
    symtab_init();

    /* --- mmap --- */
    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- System Services --------------------------------------------------------");

    boot_splash_status("Initializing mmap...");
    mmap_init();

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "[PHASE2] complete -- VFS, registry, SMP, network ready");
}

/* ---- Legacy wrapper ----------------------------------------------------- */

void boot_storage_init(uint64_t magic)
{
    (void)magic;
    boot_phase2();
}
