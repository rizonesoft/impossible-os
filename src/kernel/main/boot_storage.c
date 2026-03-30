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
#include "kernel/drivers/xhci_dev.h"
#include "kernel/drivers/rtl8139.h"
#include "kernel/net/net.h"
#include "kernel/drivers/virtio_input.h"
#include "kernel/drivers/vbox_mouse.h"
#include "kernel/drivers/ata.h"
#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/ahci.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/timer.h"
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
    POST16(POST16_PCI);
    pci_scan();
    POST16(POST16_PCI_OK);
    POST16(POST16_XHCI);
    xhci_init();
    POST16(POST16_XHCI_OK);
    boot_splash_status("Initializing network...");
    POST16(POST16_NIC);
    rtl8139_init();
    POST16(POST16_NIC_OK);
    POST16(POST16_NET);
    net_init();
    POST16(POST16_NET_OK);
    virtio_input_init();
    vbox_mouse_init();
    boot_progress(2, "PCI_NET", POST16_PCI_OK);

    /* DHCP fire-and-forget */
    dhcp_discover();
    boot_progress(2, "DHCP", POST16_NET_OK);

    /* --- Disk drivers (moved from Phase 0) --- */
    klog(LOG_DEBUG, "boot", "--- Phase: disk drivers ---");
    boot_splash_status("Initializing storage...");
    POST16(POST16_ATA);
    ata_init();
    POST16(POST16_ATA_OK);
    virtio_blk_init();
    POST16(POST16_AHCI);
    ahci_init();
    POST16(POST16_AHCI_OK);
    /* Bare metal AHCI skip REMOVED — root cause was clac #UD, fixed 2026-03-29. */
    ahci_setup_interrupts();
    blkdev_register_all();
    boot_progress(2, "STORAGE_DRV", POST16_AHCI_OK);

    /* --- VFS: requires HEAP --- */
    if (!kernel_subsystem_ready(SUBSYS_HEAP)) {
        boot_recovery_info_t ri = { SUBSYS_VFS, POST16_VFS_OK, BOOT_FATAL, 2 };
        kernel_subsystem_dump();
        boot_recovery_show(&ri);
        boot_halt("HEAP not ready -- cannot init VFS");
    }
    klog(LOG_DEBUG, "boot", "--- Phase: storage & VFS ---");
    boot_splash_status("Initializing VFS...");
    POST16(POST16_VFS);
    vfs_init();
    POST16(POST16_VFS_OK);
    kernel_subsystem_set_ready(SUBSYS_VFS, true);
    boot_progress(2, "VFS", POST16_VFS_OK);

    /* --- Partition scan + filesystem mount --- */
    klog(LOG_DEBUG, "boot", "--- Phase: partition & filesystem mount ---");
    boot_splash_tick();
    boot_splash_status("Scanning partitions...");
    POST16(POST16_PARTITION);
    partition_scan_all();
    boot_splash_status("Mounting filesystems...");
    partition_mount_filesystems();
    POST16(POST16_PARTITION_OK);

    /* --- Debug diagnostic pause (shows storage state on splash screen) --- */
    if (g_boot_info.config.debug) {
        char diag[256];
        int p = 0;
        int blk_n = blkdev_count();
        const char *s;

        /* Line 1: xHCI, USB devices, mount status */
        {
            int usb_ports = 0, usb_msc = 0;
            if (xhci_controller_count() > 0) {
                const struct xhci_controller *hc = xhci_get_controller(0);
                if (hc) usb_ports = (int)hc->max_ports;
            }
            usb_msc = xhci_msc_device_count();

            s = "xHCI="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)xhci_controller_count();
            s = " ports="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)(usb_ports / 10);
            diag[p++] = '0' + (char)(usb_ports % 10);
            s = " msc="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)usb_msc;
            s = " blk="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)(blk_n > 9 ? 9 : blk_n);
            s = " C:="; while (*s) diag[p++] = *s++;
            s = vfs_is_mounted('C') ? "Y" : "N"; while (*s) diag[p++] = *s++;
            s = " ccs="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)xhci_get_ccs_count();
            s = " stg="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)xhci_get_enum_stage();
            s = " C:="; while (*s) diag[p++] = *s++;
            s = vfs_is_mounted('C') ? "Y" : "N"; while (*s) diag[p++] = *s++;
        }
        diag[p] = '\0';
        boot_splash_status(diag);
        klog(LOG_WARN, "boot", "DIAG: %s", diag);
        sleep_ms(30000);

        /* Line 2: list block device names */
        p = 0;
        {
            int i;
            for (i = 0; i < blk_n && i < 6; i++) {
                const struct blkdev *bd = blkdev_get_by_index(i);
                if (!bd) continue;
                if (i > 0) diag[p++] = ' ';
                int j;
                for (j = 0; bd->name[j] && p < 240; j++)
                    diag[p++] = bd->name[j];
                diag[p++] = ':';
                /* Show size in MB */
                uint32_t mb = (uint32_t)(bd->sector_count * bd->sector_size / (1024*1024));
                if (mb >= 1000) {
                    diag[p++] = '0' + (char)(mb/1000 % 10);
                    diag[p++] = '0' + (char)(mb/100 % 10);
                    diag[p++] = '0' + (char)(mb/10 % 10);
                    diag[p++] = '0' + (char)(mb % 10);
                } else if (mb >= 100) {
                    diag[p++] = '0' + (char)(mb/100 % 10);
                    diag[p++] = '0' + (char)(mb/10 % 10);
                    diag[p++] = '0' + (char)(mb % 10);
                } else {
                    diag[p++] = '0' + (char)(mb/10 % 10);
                    diag[p++] = '0' + (char)(mb % 10);
                }
                diag[p++] = 'M';
            }
        }
        diag[p] = '\0';
        boot_splash_status(diag);
        klog(LOG_WARN, "boot", "DIAG: %s", diag);
        sleep_ms(30000);
    }

    boot_splash_status("Checking boot flags...");

    /* Check for debug boot flag on C:\ */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->finddir) {
            struct vfs_node *dbg = c_root->ops->finddir(c_root, "DEBUG");
            if (dbg) {
                klog(LOG_INFO, "boot",
                     "DEBUG flag found on C:\\ -- disabling splash");
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
        POST16(POST16_SMP);
        smp_init();
        POST16(POST16_SMP_OK);
        kernel_subsystem_set_ready(SUBSYS_SMP, true);
        boot_progress(2, "SMP", POST16_SMP_OK);
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
        boot_recovery_info_t ri = { SUBSYS_REGISTRY, POST16_REGISTRY_OK, BOOT_FATAL, 2 };
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
    POST16(POST16_REGISTRY);
    registry_init();
    POST16(POST16_REGISTRY_OK);
    boot_splash_status("Populating registry defaults...");
    registry_populate_defaults();
    kernel_subsystem_set_ready(SUBSYS_REGISTRY, true);
    boot_progress(2, "REGISTRY", POST16_REGISTRY_OK);

    /* --- Symbol table --- */
    symtab_init();

    /* --- mmap --- */
    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- System Services --------------------------------------------------------");

    boot_splash_status("Initializing mmap...");
    mmap_init();

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "[PHASE2] complete -- VFS, registry, SMP, network ready");

    /* NVRAM write: Phase 2 complete */
    boot_post_nvram_write16(POST16_REGISTRY_OK);
}

/* ---- Legacy wrapper ----------------------------------------------------- */

void boot_storage_init(uint64_t magic)
{
    (void)magic;
    boot_phase2();
}
