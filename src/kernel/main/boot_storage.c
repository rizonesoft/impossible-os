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
#include "kernel/fs/fat32.h"
#include "kernel/fs/partition.h"
#include "kernel/smp.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_init.h"
#include "kernel/boot_halt.h"
#include "kernel/boot_recovery.h"
#include "kernel/acpi.h"
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
#include "kernel/drivers/nvme.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/timer.h"
#include "kernel/ob/ob.h"
#include "main/main_internal.h"

/* ---- Deferred init wrappers --------------------------------------------- */

/* Network: rtl8139 + net stack + DHCP -- not needed for desktop */
static boot_result_t deferred_net_init(void)
{
    POST16(POST16_DEFERRED_NET);
    int nic = rtl8139_init();
    if (nic < 0) {
        POST16(POST16_DEFERRED_NET_OK);
        return BOOT_DEGRADED;  /* no NIC found -- not fatal */
    }
    net_init();
    dhcp_discover();
    POST16(POST16_DEFERRED_NET_OK);
    return BOOT_OK;
}

/* Optional input: VirtIO tablet + VBox absolute mouse -- PS/2 is sufficient */
static boot_result_t deferred_input_init(void)
{
    POST16(POST16_DEFERRED_INPUT);
    virtio_input_init();
    vbox_mouse_init();
    POST16(POST16_DEFERRED_INPUT_OK);
    return BOOT_OK;
}

/* ---- Async init wrappers (boot_result_t) -------------------------------- */

static boot_result_t async_ata_init(void)
{
    /* ata_init() is void -- legacy PIO driver; absence is not failure
     * (modern systems use AHCI/NVMe). Always BOOT_OK. */
    ata_init();
    return BOOT_OK;
}

static boot_result_t async_ahci_init(void)
{
    int rc = ahci_init();
    if (rc != 0) {
        klog(LOG_WARN, "boot", "AHCI init failed (rc=%d)", (uint64_t)rc);
        return BOOT_DEGRADED;
    }
    return BOOT_OK;
}

static boot_result_t async_nvme_init(void)
{
    int rc = nvme_init();
    if (rc != 0) {
        klog(LOG_WARN, "boot", "NVMe init failed (rc=%d)", (uint64_t)rc);
        return BOOT_DEGRADED;
    }
    return BOOT_OK;
}

static boot_result_t async_virtio_blk_init(void)
{
    int rc = virtio_blk_init();
    if (rc != 0) {
        klog(LOG_WARN, "boot", "VirtIO-blk init failed (rc=%d)", (uint64_t)rc);
        return BOOT_DEGRADED;
    }
    return BOOT_OK;
}

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
    if (g_boot_info.config.deferred) {
        /* Defer non-critical peripherals until after desktop is up */
        boot_defer("network", deferred_net_init);
        boot_defer("input",   deferred_input_init);
        boot_progress(2, "PCI_NET_DEFERRED", POST16_PCI_OK);
    } else {
        /* Legacy: all subsystems init in-phase */
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
    }

    /* --- SMP: moved before storage so async init can use APs --- */
    if (g_boot_info.acpi_available) {
        boot_splash_status("Initializing SMP...");
        POST16(POST16_SMP);
        smp_init();
        POST16(POST16_SMP_OK);
        kernel_subsystem_set_ready(SUBSYS_SMP, true);
        boot_progress(2, "SMP", POST16_SMP_OK);
    }

    /* Register async init IPI handler (safe even with 1 CPU) */
    boot_async_init();

    /* --- Disk drivers --- */
    klog(LOG_DEBUG, "boot", "--- Phase: disk drivers ---");
    boot_splash_status("Initializing storage...");

    if (g_boot_info.config.async_init && smp_cpu_count() > 1) {
        /* Async: probe storage drivers in parallel across CPUs */
        boot_async_step_t storage_steps[] = {
            { "ATA",       async_ata_init },
            { "AHCI",      async_ahci_init },
            { "NVMe",      async_nvme_init },
            { "VirtIO-blk", async_virtio_blk_init },
        };
        boot_result_t async_rc = boot_async_group("storage", storage_steps, 4);
        if (async_rc == BOOT_FATAL) {
            klog(LOG_ERROR, "boot",
                 "Async storage init FATAL -- falling back to sequential");
            /* Fall through to sequential path */
            ata_init();
            virtio_blk_init();
            ahci_init();
            nvme_init();
        } else if (async_rc == BOOT_DEGRADED) {
            klog(LOG_WARN, "boot",
                 "Async storage init degraded -- some drivers may be unavailable");
        }
    } else {
        /* Sequential: original order */
        POST16(POST16_ATA);
        ata_init();
        POST16(POST16_ATA_OK);
        virtio_blk_init();
        POST16(POST16_AHCI);
        ahci_init();
        POST16(POST16_AHCI_OK);
        POST16(POST16_NVME);
        nvme_init();
        POST16(POST16_NVME_OK);
    }

    ahci_setup_interrupts();
    xhci_setup_interrupts();  /* After enumeration -- ISR would steal events from polling loops */
    blkdev_register_all();
    boot_progress(2, "STORAGE_DRV", POST16_AHCI_OK);

    /* --- VFS: requires HEAP --- */
    if (!kernel_subsystem_ready(SUBSYS_HEAP)) {
        boot_recovery_info_t ri = { SUBSYS_VFS, POST16_VFS_OK, BOOT_FATAL, 2 };
        kernel_subsystem_dump();
        boot_recovery_action_t act = boot_recovery_show(&ri);
        if (act == RECOVERY_POWEROFF) { acpi_shutdown(); }
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

    /* --- BlackBox directory skeleton (X:\) --- */
    {
        struct vfs_node *probe = vfs_open("X:\\Logs", 0);
        if (probe) {
            vfs_close(probe);  /* already exists */
        } else {
            /* First boot or reformatted -- create directory skeleton */
            static const char *bb_dirs[] = {
                "X:\\Logs", "X:\\Boot", "X:\\Crash", "X:\\Crash\\WER",
                "X:\\Perf", "X:\\Diag", "X:\\Tools"
            };
            uint32_t d;
            for (d = 0; d < 7; d++) {
                if (vfs_create(bb_dirs[d], VFS_DIRECTORY) == 0)
                    klog(LOG_INFO, "boot", "BlackBox: created %s",
                         bb_dirs[d]);
            }
        }
    }

    /* --- BlackBox disk space management --- */
    if (vfs_is_mounted('X')) {
        struct vfs_node *x_root = vfs_get_drive_root('X');
        struct fat32_volume *bb_vol = x_root ?
            fat32_volume_from_root(x_root) : (struct fat32_volume *)0;

        if (bb_vol) {
            uint64_t free_bytes = fat32_get_free_bytes(bb_vol);
            uint64_t total_bytes = fat32_get_total_bytes(bb_vol);
            uint32_t pct = total_bytes > 0
                         ? (uint32_t)((free_bytes * 100) / total_bytes) : 0;
            if (pct > 100) pct = 100;  /* clamp: FSInfo can over-report */

            klog(LOG_INFO, "boot", "BlackBox: %u MiB free (%u%%)",
                 (uint32_t)(free_bytes / (1024 * 1024)), (uint64_t)pct);

            /* Cleanup if < 10% free */
            if (pct < 10) {
                uint32_t deleted = 0;
                uint32_t max_sessions = 10;  /* TODO: registry HKLM\SYSTEM\BlackBox\MaxBootSessions */
                struct vfs_node *boot_dir;

                /* Delete oldest files in Boot\ beyond max_sessions */
                boot_dir = vfs_open("X:\\Boot", VFS_O_READ);
                if (boot_dir) {
                    uint32_t file_count = 0;
                    struct vfs_dirent *de;
                    uint32_t idx = 0;

                    /* Count files */
                    while ((de = vfs_readdir(boot_dir, idx++)) != 0)
                        if (de->type == VFS_FILE) file_count++;

                    /* Delete oldest if over limit */
                    if (file_count > max_sessions) {
                        idx = 0;
                        while ((de = vfs_readdir(boot_dir, idx)) != 0
                               && file_count > max_sessions) {
                            if (de->type == VFS_FILE) {
                                char path[64];
                                int p = 0, j;
                                const char *pfx = "X:\\Boot\\";
                                for (j = 0; pfx[j]; j++) path[p++] = pfx[j];
                                for (j = 0; de->name[j] && p < 60; j++)
                                    path[p++] = de->name[j];
                                path[p] = '\0';
                                if (vfs_unlink(path) == 0) {
                                    deleted++;
                                    file_count--;
                                    continue;  /* re-read same index */
                                }
                            }
                            idx++;
                        }
                    }
                    vfs_close(boot_dir);
                }

                /* Delete rotated logs (.1, .2, .3) in Logs\ */
                {
                    struct vfs_node *logs_dir = vfs_open("X:\\Logs", VFS_O_READ);
                    if (logs_dir) {
                        uint32_t idx2 = 0;
                        struct vfs_dirent *de2;
                        while ((de2 = vfs_readdir(logs_dir, idx2)) != 0) {
                            /* Match *.N pattern (rotated files) */
                            int len = 0;
                            while (de2->name[len]) len++;
                            if (len >= 2 && de2->name[len-2] == '.'
                                && de2->name[len-1] >= '1'
                                && de2->name[len-1] <= '9') {
                                char path[64];
                                int p = 0, j;
                                const char *pfx = "X:\\Logs\\";
                                for (j = 0; pfx[j]; j++) path[p++] = pfx[j];
                                for (j = 0; de2->name[j] && p < 60; j++)
                                    path[p++] = de2->name[j];
                                path[p] = '\0';
                                if (vfs_unlink(path) == 0) {
                                    deleted++;
                                    continue;
                                }
                            }
                            idx2++;
                        }
                        vfs_close(logs_dir);
                    }
                }

                if (deleted > 0) {
                    uint64_t new_free = fat32_get_free_bytes(bb_vol);
                    uint64_t freed = new_free - free_bytes;
                    klog(LOG_WARN, "boot",
                         "BlackBox: cleanup freed %u KiB (%u files removed)",
                         (uint64_t)(freed / 1024), (uint64_t)deleted);
                }

                /* If still critically low, fall back to C:\ */
                {
                    uint64_t recheck = fat32_get_free_bytes(bb_vol);
                    uint32_t min_free_mib = 16;  /* TODO: registry HKLM\SYSTEM\BlackBox\MinFreeMiB */
                    if (recheck < (uint64_t)min_free_mib * 1024 * 1024) {
                        klog(LOG_ERROR, "boot",
                             "BlackBox: critically low (%u MiB free, min %u) "
                             "-- falling back to C:\\ for this session",
                             (uint32_t)(recheck / (1024*1024)),
                             (uint64_t)min_free_mib);
                        /* Force klog to C:\ by marking X:\ unavailable
                         * for log resolution */
                        extern int klog_using_blackbox;
                        extern const char *klog_dir;
                        klog_using_blackbox = 0;
                        klog_dir = KLOG_DIR_FALLBACK;
                    }
                }
            }
        }
    }

    POST16(POST16_PARTITION_OK);

    /* --- Debug diagnostic (shows storage state on serial + optionally splash) ---
     * diag_splash=1: render each message on the splash diag line and pause.
     *                If diag_delay=0, default to 5s so the text is readable
     *                on bare metal without serial access. */
    if (g_boot_info.config.debug) {
        char diag[256];
        int p = 0;
        int blk_n = blkdev_count();
        const char *s;
        uint8_t show_splash = g_boot_info.config.diag_splash;
        uint8_t delay = g_boot_info.config.diag_delay;
        if (show_splash && delay == 0)
            delay = 5;  /* bare metal default: 5s per screen */

        /* Diag 1: xHCI, USB devices, mount status */
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

            /* Scan PCI for all USB controllers: show prog-if codes */
            s = " USB:"; while (*s) diag[p++] = *s++;
            {
                uint16_t bus; uint8_t dv, fn;
                int usb_found = 0;
                for (bus = 0; bus < 256 && p < 230; bus++) {
                    for (dv = 0; dv < 32; dv++) {
                        for (fn = 0; fn < 8; fn++) {
                            uint16_t vid = pci_read16((uint8_t)bus, dv, fn, 0x00);
                            if (vid == 0xFFFF) continue;
                            uint8_t cls = pci_read8((uint8_t)bus, dv, fn, 0x0B);
                            uint8_t sub = pci_read8((uint8_t)bus, dv, fn, 0x0A);
                            if (cls == 0x0C && sub == 0x03) {
                                uint8_t pi = pci_read8((uint8_t)bus, dv, fn, 0x09);
                                if (usb_found > 0) diag[p++] = ',';
                                /* Show prog-if as hex: 00=UHCI 10=OHCI 20=EHCI 30=xHCI */
                                diag[p++] = "0123456789ABCDEF"[pi >> 4];
                                diag[p++] = "0123456789ABCDEF"[pi & 0xF];
                                usb_found++;
                            }
                            if (fn == 0) {
                                uint8_t hdr = pci_read8((uint8_t)bus, dv, fn, 0x0E);
                                if (!(hdr & 0x80)) break;
                            }
                        }
                    }
                }
                if (usb_found == 0) { s = "none"; while (*s) diag[p++] = *s++; }
            }
        }
        diag[p] = '\0';
        klog(LOG_WARN, "boot", "DIAG: %s", diag);
        if (show_splash) {
            boot_splash_diag(diag);
            boot_splash_delay(delay);
        }

        /* Diag 2: list block device names and sizes */
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
        klog(LOG_WARN, "boot", "DIAG: %s", diag);
        if (show_splash) {
            boot_splash_diag(diag);
            boot_splash_delay(delay);
            boot_splash_diag("");  /* clear diag line before resuming */
        }
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
    klog_resolve_dir();  /* always resolve X:\ vs C:\ -- even on TCG */
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

    /* (SMP moved earlier -- before disk drivers for async init support) */

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

    /* --- Object Manager: requires HEAP --- */
    boot_splash_status("Initializing Object Manager...");
    POST16(POST16_OB);
    {
        boot_result_t r = ob_init();
        kernel_subsystem_set_ready(SUBSYS_OB, r != BOOT_FATAL);
        if (r == BOOT_FATAL) {
            boot_recovery_info_t ri = { SUBSYS_OB, POST16_OB_OK, BOOT_FATAL, 2 };
            kernel_subsystem_dump();
            boot_recovery_action_t act = boot_recovery_show(&ri);
            if (act == RECOVERY_POWEROFF) { acpi_shutdown(); }
            boot_halt("Object Manager init failed");
        }
    }
    POST16(POST16_OB_OK);
    boot_progress(2, "OB", POST16_OB_OK);

    /* Wire OB handle event tracing from boot.conf (S15) */
    {
        extern int g_ob_handle_trace;
        if (g_boot_info.config.ob_handle_trace)
            g_ob_handle_trace = 1;
    }

    /* --- Registry: requires VFS --- */
    if (!kernel_subsystem_ready(SUBSYS_VFS)) {
        boot_recovery_info_t ri = { SUBSYS_REGISTRY, POST16_REGISTRY_OK, BOOT_FATAL, 2 };
        kernel_subsystem_dump();
        boot_recovery_action_t act = boot_recovery_show(&ri);
        if (act == RECOVERY_POWEROFF) { acpi_shutdown(); }
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

    /* --- Time subsystem: wall clock, timezone, KUSER_SHARED_DATA --- */
    {
        extern void mono_clock_init(void);
        extern void wall_clock_init(void);
        extern void timezone_init(void);
        extern void kusd_init(void);

        mono_clock_init();
        wall_clock_init();
        timezone_init();
        kusd_init();
    }

    /* --- ACPI power init: S-state discovery + SCI handler --- */
    {
        extern void acpi_power_init(void);
        extern void acpi_enable_fixed_events(void);
        extern void acpi_register_sci(void);
        acpi_power_init();
        acpi_enable_fixed_events();
        acpi_register_sci();
    }

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
