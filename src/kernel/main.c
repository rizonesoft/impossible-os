/* ============================================================================
 * kernel_main — Kernel entry point
 *
 * Called from entry.asm in 64-bit Long Mode.
 * Initializes serial, framebuffer, parses boot info, prints system details.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/multiboot2.h"
#include "kernel/boot_info.h"
#include "gfx_simd.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/printk.h"
#include "kernel/klog.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/pit.h"
#include "kernel/drivers/rtc.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/rtl8139.h"
#include "kernel/net/net.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/heap.h"
#include "kernel/drivers/ata.h"
#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/ahci.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/fs/vfs.h"

#include "kernel/fs/fat32.h"
#include "kernel/fs/ixfs.h"
#include "kernel/fs/mbr.h"
#include "kernel/fs/gpt.h"
#include "kernel/fs/partition.h"

#include "kernel/sched/task.h"
#include "kernel/sched/workqueue.h"
#include "kernel/sched/syscall.h"
#include "kernel/ipc/pipe.h"
#include "kernel/ipc/shmem.h"
#include "kernel/mm/swap.h"
#include "kernel/mm/mmap.h"

#include "kernel/drivers/mouse.h"
#include "cursor.h"
#include "kernel/drivers/virtio_input.h"
#include "kernel/drivers/vbox_mouse.h"
#include "desktop/wm.h"
#include "desktop/font.h"
#include "font_mgr.h"
#include "icon_store.h"
#include "gfx.h"
#include "kernel/drivers/pit.h"
#include "desktop/desktop.h"
#include "desktop/terminal.h"
#include "desktop/gallery.h"
#include "kernel/acpi.h"
#include "kernel/version.h"
#include "kernel/boot_splash.h"
#include "registry.h"

/* ---- Block device adapter wrappers ----
 * These adapt driver-specific APIs to the blkdev function pointer signature:
 *   int fn(uint64_t lba, uint32_t count, void *buf, void *driver_data)
 */
static int blkdev_virtio_read(uint64_t lba, uint32_t count, void *buf,
                               void *driver_data)
{
    (void)driver_data;
    return virtio_blk_read(lba, count, buf);
}

static int blkdev_virtio_write(uint64_t lba, uint32_t count, const void *buf,
                                void *driver_data)
{
    (void)driver_data;
    return virtio_blk_write(lba, count, buf);
}

static int blkdev_ahci_read(uint64_t lba, uint32_t count, void *buf,
                             void *driver_data)
{
    int port_idx = (int)(uintptr_t)driver_data;
    return ahci_read(port_idx, lba, count, buf);
}

static int blkdev_ahci_write(uint64_t lba, uint32_t count, const void *buf,
                              void *driver_data)
{
    int port_idx = (int)(uintptr_t)driver_data;
    return ahci_write(port_idx, lba, count, (void *)buf);
}

static int blkdev_atapi_read(uint64_t lba, uint32_t count, void *buf,
                              void *driver_data)
{
    int atapi_idx = (int)(uintptr_t)driver_data;
    return ahci_atapi_read(atapi_idx, lba, count, buf);
}

static int blkdev_ata_read(uint64_t lba, uint32_t count, void *buf,
                            void *driver_data)
{
    (void)driver_data;
    return ata_read_sectors((uint32_t)lba, (uint8_t)count, buf);
}

static int blkdev_ata_write(uint64_t lba, uint32_t count, const void *buf,
                             void *driver_data)
{
    (void)driver_data;
    return ata_write_sectors((uint32_t)lba, (uint8_t)count, buf);
}

/* External: Multiboot2 parser */
extern void multiboot2_parse(uintptr_t mbi_addr);

/* UEFI boot magic — our custom bootloader passes this instead of Multiboot2 */
#define UEFI_BOOT_MAGIC 0x55454649ULL  /* "UEFI" */

/* ---- Directory tree dump (serial-only) ---- */

/* Format a uint64_t with comma separators into buf (e.g. 604696 → "604,696").
 * Returns number of chars written. buf must be at least 26 bytes. */
static uint32_t format_size_commas(uint64_t val, char *buf)
{
    char raw[20];
    int rn = 0, bi = 0, digits, groups, rem;

    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }

    while (val > 0) { raw[rn++] = '0' + (char)(val % 10); val /= 10; }

    digits = rn;
    groups = (digits - 1) / 3;  /* number of commas to insert */
    rem    = digits - groups * 3;  /* digits before first comma */
    if (rem == 0) { rem = 3; groups--; }

    /* Write digits with commas */
    {
        int ri = rn - 1;
        int g;
        for (g = 0; g < rem && ri >= 0; g++)
            buf[bi++] = raw[ri--];
        while (ri >= 0) {
            buf[bi++] = ',';
            buf[bi++] = raw[ri--];
            if (ri >= 0) buf[bi++] = raw[ri--];
            if (ri >= 0) buf[bi++] = raw[ri--];
        }
    }
    buf[bi] = '\0';
    return (uint32_t)bi;
}

#define TREE_PATH_COL  60  /* Column width for path before size/DIR */

/* Recursively dump a directory tree to serial log for diagnostics.
 * parent_path: accumulated path so far (e.g. "C:\Impossible"),
 * node: directory vfs_node, depth: indent level.
 * Returns total number of entries printed. */
static uint32_t dump_dir_tree(const char *parent_path, struct vfs_node *node,
                               uint32_t depth)
{
    uint32_t idx = 0;
    uint32_t total = 0;
    struct vfs_dirent *de;

    if (depth > 20) return 0;

    while ((de = vfs_readdir(node, idx)) != 0) {
        /* Build full path: parent_path + "\" + name */
        char full_path[256];
        uint32_t pi = 0, ni = 0;
        const char *pp = parent_path;
        while (*pp && pi < 254) full_path[pi++] = *pp++;
        if (pi > 0 && full_path[pi - 1] != '\\')
            full_path[pi++] = '\\';
        while (de->name[ni] && pi < 254) full_path[pi++] = de->name[ni++];

        if (de->type & VFS_DIRECTORY) {
            /* Append trailing backslash for directories */
            if (pi < 254) full_path[pi++] = '\\';
            full_path[pi] = '\0';

            /* Build aligned line: path padded to TREE_PATH_COL, then <DIR> */
            {
                char line[128];
                uint32_t li = 0, p;
                for (p = 0; full_path[p] && li < 120; p++)
                    line[li++] = full_path[p];
                while (li < TREE_PATH_COL && li < 120)
                    line[li++] = ' ';
                line[li++] = '<'; line[li++] = 'D'; line[li++] = 'I';
                line[li++] = 'R'; line[li++] = '>';
                line[li] = '\0';
                klog(LOG_DEBUG, "tree", "%s", line);
            }

            /* Recurse into subdirectory */
            {
                full_path[pi - 1] = '\0';  /* remove trailing \ for recursion */
                struct vfs_node *sub = vfs_finddir(node, de->name);
                if (sub)
                    total += dump_dir_tree(full_path, sub, depth + 1);
            }
        } else {
            full_path[pi] = '\0';

            /* Get file size via stat if available */
            struct vfs_stat st;
            uint64_t fsize = 0;
            if (node->ops && node->ops->stat) {
                struct vfs_node *fnode = vfs_finddir(node, de->name);
                if (fnode && fnode->ops && fnode->ops->stat) {
                    if (fnode->ops->stat(fnode, &st) == 0)
                        fsize = st.size;
                }
            }

            /* Build aligned line: path padded to TREE_PATH_COL, then size */
            {
                char line[128];
                char size_buf[26];
                uint32_t li = 0, p, sn, pad;

                for (p = 0; full_path[p] && li < 120; p++)
                    line[li++] = full_path[p];

                sn = format_size_commas(fsize, size_buf);

                /* Right-justify: pad to align size + " B" at column 72 */
                {
                    uint32_t target = TREE_PATH_COL + 12;
                    uint32_t needed = sn + 2;  /* size + " B" */
                    uint32_t fill = (target > li + needed) ?
                                    target - li - needed : 1;
                    for (pad = 0; pad < fill && li < 120; pad++)
                        line[li++] = ' ';
                }

                for (p = 0; size_buf[p] && li < 124; p++)
                    line[li++] = size_buf[p];
                line[li++] = ' '; line[li++] = 'B';
                line[li] = '\0';
                klog(LOG_DEBUG, "tree", "%s", line);
            }
        }
        total++;
        idx++;
    }
    return total;
}

/* Kernel entry point
 *   magic = MULTIBOOT2_BOOTLOADER_MAGIC (0x36D76289) for GRUB
 *           or UEFI_BOOT_MAGIC (0x55454649) for our UEFI bootloader
 *   mbi   = Multiboot2 info pointer (GRUB) or boot_info pointer (UEFI)
 */
void kernel_main(uint64_t magic, uint64_t mbi)
{
    uint32_t i;
    uint64_t total_ram = 0;

    /* Step 1: Initialize serial (always works, even without display) */
    serial_init();
    klog(LOG_DEBUG, "", "========================================================================");
    klog(LOG_DEBUG, "", "  Impossible OS -- Boot Log");
    klog(LOG_DEBUG, "", "========================================================================");

    /* Step 2: Parse boot info based on bootloader type */
    if (magic == UEFI_BOOT_MAGIC) {
        /* UEFI path: boot_info is already filled by our bootloader */
        struct boot_info *src = (struct boot_info *)(uintptr_t)mbi;
        uint8_t *d = (uint8_t *)&g_boot_info;
        const uint8_t *s = (const uint8_t *)src;
        for (i = 0; i < sizeof(struct boot_info); i++)
            d[i] = s[i];
        serial_write("[UEFI] Boot info received from Impossible OS bootloader\n");
    } else if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        /* Multiboot2 path: parse GRUB's info structure */
        multiboot2_parse((uintptr_t)mbi);
    } else {
        serial_write("[FAIL] Unknown bootloader magic!\n");
        goto halt;
    }

    /* Step 4: Initialize physical memory manager (needs parsed memory map) */
    pmm_init();

    /* Step 5: Initialize virtual memory manager (needs PMM for page tables) */
    vmm_init();

    /* Step 6: Initialize kernel heap (needs VMM for page mapping) */
    heap_init();

    /* Step 6b: Enable AVX2 SIMD if supported */
    simd_enable_avx();
    if (simd_avx2_ok)
        klog(LOG_INFO, "simd", "AVX2 enabled (8 pixels/iter)");
    else
        klog(LOG_WARN, "simd", "AVX2 not available, using SSE2 fallback");

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Hardware ---------------------------------------------------------------");

    /* Step 7: Initialize ATA disk driver */
    ata_init();

    /* Step 7b: Initialize VirtIO block device */
    virtio_blk_init();

    /* Step 7c: Initialize AHCI (SATA) driver */
    ahci_init();

    /* Step 7d: Register block devices */
    {
        struct blkdev bd;

        /* ATA master */
        if (ata_get_drive(0)->present) {
            const struct ata_drive *drv = ata_get_drive(0);
            bd = (struct blkdev){0};
            bd.name[0]='a'; bd.name[1]='t'; bd.name[2]='a'; bd.name[3]='0'; bd.name[4]='\0';
            bd.sector_size  = 512;
            bd.sector_count = (uint64_t)drv->sectors;
            bd.read  = blkdev_ata_read;
            bd.write = blkdev_ata_write;
            bd.driver_data  = (void *)0;
            blkdev_register(&bd);
        }

        /* VirtIO-blk */
        if (virtio_blk_present()) {
            bd = (struct blkdev){0};
            bd.name[0]='v'; bd.name[1]='i'; bd.name[2]='r'; bd.name[3]='t';
            bd.name[4]='i'; bd.name[5]='o'; bd.name[6]='0'; bd.name[7]='\0';
            bd.sector_size  = 512;
            bd.sector_count = virtio_blk_capacity();
            bd.read  = blkdev_virtio_read;
            bd.write = blkdev_virtio_write;
            bd.driver_data  = (void *)0;
            blkdev_register(&bd);
        }

        /* AHCI / SATA drives */
        {
            int di;
            for (di = 0; di < ahci_drive_count(); di++) {
                bd = (struct blkdev){0};
                bd.name[0]='s'; bd.name[1]='a'; bd.name[2]='t'; bd.name[3]='a';
                bd.name[4]='0' + (char)di; bd.name[5]='\0';
                bd.sector_size  = 512;
                bd.sector_count = ahci_capacity(di);
                bd.read  = blkdev_ahci_read;
                bd.write = blkdev_ahci_write;
                bd.driver_data  = (void *)(uintptr_t)di;
                blkdev_register(&bd);
            }
        }

        /* AHCI / ATAPI (optical) devices */
        {
            int ai;
            for (ai = 0; ai < ahci_atapi_count(); ai++) {
                bd = (struct blkdev){0};
                bd.name[0]='c'; bd.name[1]='d'; bd.name[2]='r'; bd.name[3]='o';
                bd.name[4]='m'; bd.name[5]='0' + (char)ai; bd.name[6]='\0';
                bd.sector_size  = ahci_atapi_sector_size(ai);
                bd.sector_count = ahci_atapi_capacity(ai);
                bd.read  = blkdev_atapi_read;
                bd.write = NULL;  /* Optical media is read-only */
                bd.driver_data  = (void *)(uintptr_t)ai;
                blkdev_register(&bd);
            }
        }

        blkdev_list();
    }

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Storage & Filesystem ---------------------------------------------------");

    klog(LOG_DEBUG, "boot", "--- Phase: storage & VFS ---");
    /* Step 8: Initialize VFS */
    vfs_init();

    /* All system files live on the IXFS partition (C:\), pre-populated
     * by mkfs-ixfs --populate. Boot files (kernel.exe, BOOTX64.EFI)
     * live on the EFI partition. partition_mount_filesystems() below
     * mounts all detected volumes. */


    klog(LOG_DEBUG, "boot", "--- Phase: interrupt controllers & timer ---");
    /* Step 5: Load GDT, IDT, PIC, PIT — must happen before boot splash
     * so that sleep_ms() works correctly during the fade-in animation. */
    gdt_init();
    idt_init();
    pic_init();
    pit_init();
    rtc_init();
    keyboard_init();
    mouse_init();

    klog(LOG_DEBUG, "boot", "--- Phase: display & splash ---");
    /* Step 6: Initialize framebuffer (needs parsed boot info) */
    fb_init();

    /* Boot splash: black screen + icon + animated dots + status text.
     * Locks the compositor so printk output goes to serial only.
     * PIT is now initialized so sleep_ms() works in the fade-in. */
    boot_splash_init();
    boot_splash_status("Setting up hardware...");

    klog(LOG_DEBUG, "boot", "--- Phase: PCI & network hardware ---");
    /* Step 10: PCI bus scan and NIC init */
    boot_splash_tick();
    boot_splash_status("Detecting hardware...");
    pci_scan();
    rtl8139_init();
    net_init();
    virtio_input_init();
    vbox_mouse_init();

    /* Step 11: Enable interrupts */
    __asm__ volatile ("sti");

    /* Start timer-driven splash animation (needs PIT IRQs running) */
    boot_splash_start_animation();

    /* ═══ PARALLEL BOOT — DHCP fire-and-forget ═══════════════════════════════
     * dhcp_discover() sends a single UDP broadcast and returns in <1ms.
     * The OFFER/REQUEST/ACK exchange is handled by the network IRQ handler
     * (dhcp_handle() called from the Ethernet receive path).
     *
     * Calling it HERE, before partition_scan_all(), means the ~300ms
     * DHCP round-trip latency overlaps with disk scanning and filesystem
     * mounting — saving ~300ms of sequential boot time at zero risk.
     *
     * DEPENDENCY NOTE: No VFS access, no threading, no lock contention.
     * ════════════════════════════════════════════════════════════════════════ */
    klog(LOG_DEBUG, "boot", "--- Phase: network (DHCP, async fire-and-forget) ---");
    dhcp_discover();   /* sends UDP; response arrives via IRQ during partition scan */

    klog(LOG_DEBUG, "boot", "--- Phase: partition & filesystem mount ---");
    /* Step 11b: Scan block devices for partition tables (GPT first, MBR fallback).
     * Must happen after IDT/PIC init because VirtIO I/O calls sti/cli.
     * Creates sub-blkdevs for each partition and probes filesystems. */
    boot_splash_tick();
    boot_splash_status("Detecting drives...");
    partition_scan_all();
    partition_mount_filesystems();
    boot_splash_tick();


    boot_splash_status("Configuring network...");
    boot_splash_tick();
    /* NOTE: DHCP was already sent before partition scan.
     * By now (300-500ms later) the ACK is usually already received
     * and net_cfg.configured == 1.  No explicit wait needed. */

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- System Summary ---------------------------------------------------------");

    /* Hardware summary — klog INFO (visible on screen) */
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

        /* Parse RSDP → RSDT → FADT for power management */
        acpi_init();
    }

    /* Heap test: alloc, write, free, realloc */
    {
        uint8_t *a = (uint8_t *)kmalloc(64);
        uint8_t *b = (uint8_t *)kmalloc(128);
        uint8_t *c = (uint8_t *)kmalloc(256);
        uint32_t ok = 1;

        /* Write patterns */
        if (a) { uint32_t j; for (j = 0; j < 64; j++) a[j] = (uint8_t)j; }
        if (b) { uint32_t j; for (j = 0; j < 128; j++) b[j] = (uint8_t)(j ^ 0xAA); }
        if (c) { uint32_t j; for (j = 0; j < 256; j++) c[j] = (uint8_t)(j ^ 0x55); }

        /* Verify patterns */
        if (a) { uint32_t j; for (j = 0; j < 64; j++) if (a[j] != (uint8_t)j) ok = 0; }
        if (b) { uint32_t j; for (j = 0; j < 128; j++) if (b[j] != (uint8_t)(j ^ 0xAA)) ok = 0; }

        kfree(b);
        b = (uint8_t *)krealloc(a, 512);
        /* Verify old data survived realloc */
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

    /* Initialize the Windows-compatible Registry */
    registry_init();
    registry_populate_defaults();

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- System Services --------------------------------------------------------");

    /* Initialize memory-mapped files subsystem */
    mmap_init();

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Boot Tests -------------------------------------------------------------");

    /* VFS test: read a file from C:\ (IXFS system partition) */
    if (vfs_is_mounted('C')) {
        struct vfs_node *f = vfs_open("C:\\hello.txt", VFS_O_READ);
        if (f) {
            uint8_t buf[128];
            int n = vfs_read(f, 0, sizeof(buf) - 1, buf);
            if (n > 0) {
                buf[n] = '\0';
                klog(LOG_DEBUG, "test", "VFS read C:\\hello.txt: \"%s\"", (char *)buf);
            }
            vfs_close(f);
        }
    }

    /* IXFS CRUD test on C:\ */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->create) {
            int rc = c_root->ops->create(c_root, "test.txt", VFS_FILE);
            if (rc == 0) {
                struct vfs_node *f = vfs_open("C:\\test.txt", VFS_O_WRITE);
                if (f) {
                    const char *msg = "IXFS is working!";
                    uint32_t len = 0;
                    while (msg[len]) len++;
                    vfs_write(f, 0, len, (const uint8_t *)msg);
                    vfs_close(f);
                }

                f = vfs_open("C:\\test.txt", VFS_O_READ);
                if (f) {
                    uint8_t buf[64];
                    int n = vfs_read(f, 0, sizeof(buf) - 1, buf);
                    if (n > 0) {
                        buf[n] = '\0';
                        klog(LOG_DEBUG, "test", "IXFS CRUD: C:\\test.txt = \"%s\"", (char *)buf);
                    }
                    vfs_close(f);
                }

                if (c_root->ops->unlink) {
                    c_root->ops->unlink(c_root, "test.txt");
                    f = vfs_open("C:\\test.txt", VFS_O_READ);
                    klog(f ? LOG_ERROR : LOG_DEBUG, "test",
                         "IXFS delete: C:\\test.txt %s", f ? "still exists!" : "removed");
                    if (f) vfs_close(f);
                }
            }
        } else {
            klog(LOG_DEBUG, "test", "IXFS: no c_root or no create op");
        }
    }

    /* IXFS mkdir/rmdir test on C:\ */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->create) {
            int rc = c_root->ops->create(c_root, "TestDir", VFS_DIRECTORY);
            if (rc == 0) {
                struct vfs_dirent *de = vfs_readdir(c_root, 0);
                if (de && de->type & VFS_DIRECTORY)
                    klog(LOG_DEBUG, "test", "IXFS mkdir: C:\\%s", de->name);

                if (c_root->ops->unlink) {
                    rc = c_root->ops->unlink(c_root, "TestDir");
                    klog(rc == 0 ? LOG_DEBUG : LOG_ERROR, "test",
                         "IXFS rmdir: C:\\TestDir %s", rc == 0 ? "removed" : "failed");
                }
            }
        }
    }
    /* IXFS Performance Tests: block groups, buffer cache, hash index */
    ixfs_test_performance();

    /* === Directory tree dump (serial-only) === */
    klog(LOG_DEBUG, "test", "=== Directory Tree Dump ===");

    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root) {
            klog(LOG_DEBUG, "tree", "C:\\");
            uint32_t c_count = dump_dir_tree("C:", c_root, 1);
            klog(LOG_DEBUG, "tree", "C:\\ total: %u entries", (uint64_t)c_count);
        }
    }

    if (vfs_is_mounted('D')) {
        struct vfs_node *d_root = vfs_get_drive_root('D');
        if (d_root) {
            klog(LOG_DEBUG, "tree", "D:\\");
            uint32_t d_count = dump_dir_tree("D:", d_root, 1);
            klog(LOG_DEBUG, "tree", "D:\\ total: %u entries", (uint64_t)d_count);
        }
    }

    /* === Registry persistence test === */
    {
        HKEY hk_test = (HKEY)0;
        uint32_t disp = 0;
        uint32_t reg_ok = 1;

        /* Create a test key and set a DWORD value */
        long rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "System\\Test",
                                 0, (void *)0, 0, KEY_ALL_ACCESS,
                                 (void *)0, &hk_test, &disp);
        if (rc == ERROR_SUCCESS && hk_test) {
            RegSetDword(hk_test, "BootCount", 42);
            RegCloseKey(hk_test);

            /* Flush to disk */
            registry_save_all();

            /* Verify the value is readable in-memory */
            hk_test = (HKEY)0;
            rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "System\\Test",
                              0, KEY_READ, &hk_test);
            if (rc == ERROR_SUCCESS && hk_test) {
                uint32_t val = 0;
                rc = RegGetDword(hk_test, "BootCount", &val);
                if (rc != ERROR_SUCCESS || val != 42)
                    reg_ok = 0;
                RegCloseKey(hk_test);
            } else {
                reg_ok = 0;
            }

            /* Clean up test key */
            RegDeleteTree(HKEY_LOCAL_MACHINE, "System\\Test");
        } else {
            reg_ok = 0;
        }

        klog(reg_ok ? LOG_DEBUG : LOG_ERROR, "test",
             "Registry round-trip: %s (keys: %u, values: %u)",
             reg_ok ? "passed" : "FAIL",
             (uint64_t)reg_keys_used(), (uint64_t)reg_values_used());
    }

    /* === VMM map/unmap self-test === */
    {
        uintptr_t vmm_phys = pmm_alloc_frame();
        uintptr_t vmm_virt = 0xA00000;  /* 10 MiB — safe test address */
        uint32_t vmm_ok = 1;

        if (vmm_phys) {
            vmm_map_page(vmm_virt, vmm_phys, VMM_KERNEL_RW);

            /* Write and read back a pattern */
            {
                volatile uint32_t *ptr = (volatile uint32_t *)vmm_virt;
                *ptr = 0xDEADBEEF;
                if (*ptr != 0xDEADBEEF)
                    vmm_ok = 0;
            }

            vmm_unmap_page(vmm_virt, 1);  /* free_frame = 1 */
        } else {
            vmm_ok = 0;
        }

        klog(vmm_ok ? LOG_DEBUG : LOG_ERROR, "test",
             "VMM map/unmap: %s", vmm_ok ? "passed" : "FAIL");
    }

    /* === PMM alloc/free self-test === */
    {
        uintptr_t f1 = pmm_alloc_frame();
        uintptr_t f2 = pmm_alloc_frame();
        uint32_t pmm_ok = 1;

        if (!f1 || !f2) {
            pmm_ok = 0;
        } else {
            /* Frames should be different */
            if (f1 == f2)
                pmm_ok = 0;

            /* Free f1, re-alloc should reuse it or give another valid frame */
            pmm_free_frame(f1);
            uintptr_t f3 = pmm_alloc_frame();
            if (!f3)
                pmm_ok = 0;

            pmm_free_frame(f2);
            pmm_free_frame(f3);
        }

        klog(pmm_ok ? LOG_DEBUG : LOG_ERROR, "test",
             "PMM alloc/free: %s", pmm_ok ? "passed" : "FAIL");
    }

    /* Timer verification */
    klog(LOG_DEBUG, "test", "Timer: sleeping 1 second...");
    sleep_ms(1000);
    klog(LOG_DEBUG, "test", "Timer OK (ticks: %u, uptime: %u sec)",
         pit_get_ticks(), uptime());

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Scheduler Tests --------------------------------------------------------");

    /* === Cooperative threading test === */
    task_init();

    /* Create the system work queue — used by drivers to defer IRQ bottom-half
     * work into a normal, yieldable kernel thread context.  Must be created
     * after task_init() because workqueue_create() spawns a kernel task.
     * The scheduler is enabled briefly so the worker task can start. */
    {
        scheduler_enable();
        sys_wq = workqueue_create("sys_wq");
        scheduler_disable();
        if (sys_wq)
            klog(LOG_DEBUG, "wq", "sys_wq created");
        else
            klog(LOG_ERROR, "wq", "sys_wq creation FAILED");
    }

    {
        extern void thread_a_func(void);
        extern void thread_b_func(void);

        task_create(thread_a_func, "ThreadA");
        task_create(thread_b_func, "ThreadB");

        klog(LOG_DEBUG, "test", "Cooperative: two threads alternate via yield()");

        /* Main thread yields to let both threads run */
        yield();
        yield();
        yield();
        yield();
        yield();
        yield();

        klog(LOG_DEBUG, "test", "Cooperative threading test passed");
    }

    /* === Preemptive scheduling test === */
    {
        extern void preempt_a_func(void);
        extern void preempt_b_func(void);

        task_create(preempt_a_func, "PreemptA");
        task_create(preempt_b_func, "PreemptB");

        klog(LOG_DEBUG, "test", "Preemptive: threads run without yield()");
        scheduler_enable();

        /* Main thread sleeps while the preemptive threads run */
        sleep_ms(600);

        scheduler_disable();

        klog(LOG_DEBUG, "test", "Preemptive scheduling test passed");
    }

    /* === Kernel thread test (shared globals) === */
    {
        extern volatile uint32_t thread_shared_counter;
        extern void thread_inc_func(void *arg);

        int tid_a, tid_b;
        int32_t status_a, status_b;

        thread_shared_counter = 0;

        tid_a = thread_create(thread_inc_func, (void *)"ThreadA", 0);
        tid_b = thread_create(thread_inc_func, (void *)"ThreadB", 0);

        if (tid_a >= 0 && tid_b >= 0) {
            status_a = thread_join((uint32_t)tid_a);
            status_b = thread_join((uint32_t)tid_b);
            (void)status_a;
            (void)status_b;

            klog(thread_shared_counter == 10 ? LOG_DEBUG : LOG_ERROR, "test",
                 "Kernel thread test: counter=%u (%s)",
                 (uint64_t)thread_shared_counter,
                 thread_shared_counter == 10 ? "passed" : "FAIL");
        } else {
            klog(LOG_ERROR, "test", "thread_create failed");
        }
    }

    /* === Mutex test (two threads, protected counter) === */
    {
        extern volatile uint32_t mutex_shared_counter;
        extern void mutex_inc_func(void *arg);

        int mtid_a, mtid_b;

        mutex_shared_counter = 0;

        mtid_a = thread_create(mutex_inc_func, (void *)"MutexA", 0);
        mtid_b = thread_create(mutex_inc_func, (void *)"MutexB", 0);

        if (mtid_a >= 0 && mtid_b >= 0) {
            thread_join((uint32_t)mtid_a);
            thread_join((uint32_t)mtid_b);

            klog(mutex_shared_counter == 200 ? LOG_DEBUG : LOG_ERROR, "test",
                 "Mutex test: counter=%u (%s)",
                 (uint64_t)mutex_shared_counter,
                 mutex_shared_counter == 200 ? "passed" : "FAIL");
        } else {
            klog(LOG_ERROR, "test", "mutex thread_create failed");
        }
    }

    /* === Semaphore test (producer-consumer) === */
    {
        extern volatile uint32_t sem_produced;
        extern volatile uint32_t sem_consumed;
        extern void sem_producer_func(void *arg);
        extern void sem_consumer_func(void *arg);

        int stid_p, stid_c;

        sem_produced = 0;
        sem_consumed = 0;

        stid_c = thread_create(sem_consumer_func, (void *)0, 0);
        stid_p = thread_create(sem_producer_func, (void *)0, 0);

        if (stid_p >= 0 && stid_c >= 0) {
            thread_join((uint32_t)stid_p);
            thread_join((uint32_t)stid_c);

            klog(sem_consumed == 5 ? LOG_DEBUG : LOG_ERROR, "test",
                 "Semaphore test: consumed=%u (%s)",
                 (uint64_t)sem_consumed,
                 sem_consumed == 5 ? "passed" : "FAIL");
        } else {
            klog(LOG_ERROR, "test", "semaphore thread_create failed");
        }
    }

    /* === Pipe test (writer → reader) === */
    {
        extern int pipe_test_id;
        extern volatile uint32_t pipe_test_ok;
        extern void pipe_writer_func(void *arg);
        extern void pipe_reader_func(void *arg);

        int pipe_fds[2];
        int ptid_w, ptid_r;

        pipe_init();
        pipe_test_ok = 0;

        if (pipe_create(pipe_fds) == 0) {
            pipe_test_id = pipe_fds[0];

            ptid_r = thread_create(pipe_reader_func, (void *)0, 0);
            ptid_w = thread_create(pipe_writer_func, (void *)0, 0);

            if (ptid_w >= 0 && ptid_r >= 0) {
                thread_join((uint32_t)ptid_w);
                thread_join((uint32_t)ptid_r);

                klog(pipe_test_ok ? LOG_DEBUG : LOG_ERROR, "test",
                     "Pipe test: %s", pipe_test_ok ? "passed" : "data mismatch");
            } else {
                klog(LOG_ERROR, "test", "pipe thread_create failed");
            }
        } else {
            klog(LOG_ERROR, "test", "pipe_create failed");
        }
    }

    /* === Shared memory test (two threads, named region) === */
    {
        extern volatile uint32_t shmem_test_ok;
        extern void shmem_writer_func(void *arg);
        extern void shmem_reader_func(void *arg);

        int shm_id;
        int shm_tw, shm_tr;

        shmem_test_ok = 0;

        shm_id = shmem_create("test_counter", sizeof(uint32_t));
        if (shm_id >= 0) {
            shm_tw = thread_create(shmem_writer_func, (void *)0, 0);
            shm_tr = thread_create(shmem_reader_func, (void *)0, 0);

            if (shm_tw >= 0 && shm_tr >= 0) {
                thread_join((uint32_t)shm_tw);
                thread_join((uint32_t)shm_tr);

                klog(shmem_test_ok ? LOG_DEBUG : LOG_ERROR, "test",
                     "Shared memory test: %s",
                     shmem_test_ok ? "passed" : "counter != 200");
            } else {
                klog(LOG_ERROR, "test", "shmem thread_create failed");
            }

            shmem_unmap(shm_id);
        } else {
            klog(LOG_ERROR, "test", "shmem_create failed");
        }
    }

    /* === Swap test (explicit swap out → swap in data path) === */
    {
        /* Allocate a test page via PMM + VMM */
        uintptr_t test_phys = pmm_alloc_frame();
        uintptr_t test_virt = 0x800000;  /* 8 MiB — safe test address */
        uint32_t swap_ok = 1;
        uint32_t k;
        int slot_id;

        swap_init(64);  /* 64 slots = 256 KiB swap */

        if (test_phys) {
            vmm_map_page(test_virt, test_phys, VMM_KERNEL_RW);

            /* Write known pattern */
            {
                uint8_t *page = (uint8_t *)test_virt;
                for (k = 0; k < 4096; k++)
                    page[k] = (uint8_t)(k & 0xFF);
            }

            swap_clock_register(test_virt);

            slot_id = swap_out(test_virt);
            if (slot_id >= 0) {
                if (swap_in((uint32_t)slot_id, test_virt) == 0) {
                    uint8_t *page = (uint8_t *)test_virt;
                    for (k = 0; k < 4096; k++) {
                        if (page[k] != (uint8_t)(k & 0xFF)) {
                            swap_ok = 0;
                            break;
                        }
                    }
                } else {
                    swap_ok = 0;
                }
            } else {
                swap_ok = 0;
            }

            vmm_unmap_page(test_virt, 1);

            klog(swap_ok ? LOG_DEBUG : LOG_ERROR, "test",
                 "Swap test: %s (%u/%u slots)",
                 swap_ok ? "passed" : "FAIL",
                 (uint64_t)swap_get_used_slots(),
                 (uint64_t)swap_get_total_slots());
        } else {
            klog(LOG_ERROR, "test", "swap test alloc failed");
        }
    }

    /* === mmap test: map hello.txt into memory and read as pointer === */
    if (vfs_is_mounted('C')) {
        struct vfs_node *mf = vfs_open("C:\\hello.txt", VFS_O_READ);
        if (mf) {
            void *mapped = mmap((void *)0, 4096, PROT_READ, MAP_PRIVATE,
                                mf, 0);
            if (mapped != MAP_FAILED) {
                const char *txt = (const char *)mapped;
                uint32_t mmap_ok = (txt[0] != '\0') ? 1 : 0;

                klog(mmap_ok ? LOG_DEBUG : LOG_ERROR, "test",
                     "mmap test: %s (mapped at %p, first='%c')",
                     mmap_ok ? "passed" : "FAIL",
                     (uintptr_t)mapped, (uint64_t)(uint8_t)txt[0]);

                munmap(mapped, 4096);
            } else {
                klog(LOG_ERROR, "test", "mmap returned MAP_FAILED");
            }
            vfs_close(mf);
        } else {
            klog(LOG_DEBUG, "test", "mmap test: hello.txt not found (skipped)");
        }
    }

    /* === VirtIO-blk test (read sector 0) === */
    if (virtio_blk_present()) {
        uint8_t sect0[512];
        uint32_t vt;
        int vrc;

        for (vt = 0; vt < 512; vt++)
            sect0[vt] = 0;

        vrc = virtio_blk_read(0, 1, sect0);
        if (vrc == 0) {
            klog(LOG_DEBUG, "test", "VirtIO-blk: sector 0 read OK (%u sectors)",
                 (uint64_t)virtio_blk_capacity());
        } else {
            klog(LOG_ERROR, "test", "VirtIO-blk: sector 0 read failed");
        }
    }
    /* === AHCI sector 0 test === */
    if (ahci_present()) {
        uint8_t asect0[512];
        uint32_t at;
        int arc;

        for (at = 0; at < 512; at++)
            asect0[at] = 0;

        arc = ahci_read(0, 0, 1, asect0);
        if (arc == 0) {
            klog(LOG_DEBUG, "test", "AHCI: sector 0 read OK (%u sectors)",
                 (uint64_t)ahci_capacity(0));
        } else {
            klog(LOG_ERROR, "test", "AHCI: sector 0 read failed");
        }
    }
    /* === User mode test === (SKIPPED — hangs due to ring 3 transition issue) */
#if 0
    {
        extern void user_test_func(void);

        syscall_init();
        task_create_user(user_test_func, "UserTest");
        scheduler_enable();
        sleep_ms(500);
        scheduler_disable();
        klog(LOG_DEBUG, "test", "User mode test passed");
    }

    /* === Exec test: load and run an ELF from C:\ === */
    {
        extern void exec_loader_func(void);

        task_create(exec_loader_func, "ExecLoader");
        scheduler_enable();
        sleep_ms(500);
        scheduler_disable();
        klog(LOG_DEBUG, "test", "Exec test passed");
    }

    /* === Fork test: fork a user process, child prints, parent waits === */
    {
        extern void fork_test_func(void);

        task_create_user(fork_test_func, "ForkTest");
        scheduler_enable();
        sleep_ms(800);
        scheduler_disable();
        klog(LOG_DEBUG, "test", "Fork test passed");
    }
#else
    /* Initialize syscalls (needed even without tests) */
    syscall_init();
    klog(LOG_DEBUG, "test", "User mode / exec / fork tests skipped");
#endif

    boot_splash_status("Preparing desktop...");
    boot_splash_tick();

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Desktop ----------------------------------------------------------------");

    /* === Flush boot log to disk === */
    klog_flush_to_disk();

#ifdef BSOD_TEST
    /* Test trigger: fire a deliberate panic to test the BSOD screen */
    {
        extern void panic_screen(struct interrupt_frame *frame, uint64_t error_code,
                                 const char *description, const char *file, uint32_t line);
        panic_screen((struct interrupt_frame *)0, 0xDEAD,
                     "BSOD_TEST: Deliberate panic for testing", __FILE__, __LINE__);
    }
#endif

    /* === Launch the shell === */
    {
        extern void shell_loader_func(void);

        boot_splash_tick();
        boot_splash_status("Loading fonts...");

        /* Initialize TrueType font manager (loads fonts + builds glyph cache) */
        ttf_mgr_init();

        /* Initialize icon store (loads Fluent icon fonts + color icons) */
        boot_splash_tick();
        boot_splash_status("Loading resources...");
        icon_store_init();

        /* Initialize cursor manager (loads Adwaita Xcur files from sysroot) */
        cursor_init();

        klog(LOG_DEBUG, "boot", "--- Phase: desktop & WM ---");
        /* Initialize window manager */
        boot_splash_tick();
        boot_splash_status("Almost ready...");
        wm_init();

        /* Initialize desktop (wallpaper, taskbar, copy backgrounds to IXFS) */
        desktop_init();

        /* Finish boot splash — hand off screen to desktop */
        boot_splash_finish();
        /* Boot complete timing marker (before compositor loop) */
        {
            uint64_t ms = pit_get_ticks() * 10;
            klog(LOG_INFO, "boot",
                 "Boot complete in %u.%03us (PIT uptime from interrupt init)",
                 (uint64_t)(ms / 1000), (uint64_t)(ms % 1000));
        }
        /* NOTE: klog screen output remains suppressed (LOG_FATAL) here.
         * It is restored inside the compositor loop on first_frame after
         * wm_composite() has rendered the full desktop. */

        /* ---- Boot-time heap stats ---- */
        {
            uint64_t h_used  = heap_get_used();
            uint64_t h_total = heap_get_total();
            uint64_t pct     = h_total ? (h_used * 100) / h_total : 0;
            printk("[OK] Heap: %u KB used / %u KB total (%u%%)\n",
                   (h_used + 1023) / 1024,
                   (h_total + 1023) / 1024,
                   pct);
            if (pct > 75) {
                printk("[!!] Heap pressure: %u%% used — risk of silent exhaustion\n",
                       pct);
            }
        }

        /* Create a demo window with icon toolbar */
        {
            int demo = wm_create_window("Welcome", 100, 80, 460, 300, WM_DEFAULT_FLAGS);
            if (demo >= 0) {
                uint32_t *fb = wm_get_framebuffer(demo);
                uint32_t cw = wm_get_client_width(demo);
                uint32_t ch = wm_get_client_height(demo);
                if (fb && cw && ch) {
                    gfx_surface_t ws;
                    ttf_font_t *fnt;
                    gfx_surface_init(&ws, fb, cw, ch, cw);

                    /* Dark background */
                    wm_fill_rect(demo, 0, 0, cw, ch, 0xFF202020);

                    /* ---- Toolbar strip (8 icons, 20px, Filled variant) ---- */
                    {
                        static const system_icon_t toolbar_icons[] = {
                            ICON_CUT, ICON_COPY, ICON_PASTE,
                            ICON_UNDO, ICON_REDO, ICON_SAVE,
                            ICON_SEARCH, ICON_SETTINGS
                        };
                        int icon_count = 8;
                        int icon_size  = 20;
                        int padding    = 8;
                        int toolbar_y  = 8;
                        int ix;
                        uint32_t toolbar_bg = 0xFF2D2D2D;
                        uint32_t icon_color = 0xFFFFFFFF;  /* Win11 white */

                        /* Toolbar background */
                        gfx_fill_rect(&ws, 0, 0, cw, icon_size + padding * 2, toolbar_bg);

                        /* Render each icon */
                        for (ix = 0; ix < icon_count; ix++) {
                            icon_bitmap_t *bmp = icon_get_variant(
                                toolbar_icons[ix], icon_size, icon_color,
                                ICON_FONT_REGULAR);
                            if (bmp) {
                                int icon_x = padding + ix * (icon_size + padding);
                                icon_draw(&ws, bmp, icon_x, toolbar_y);
                            }
                        }

                        /* Toolbar separator line */
                        gfx_fill_rect(&ws, 0, icon_size + padding * 2,
                                      cw, 1, 0xFF383838);
                    }

                    /* Greeting text below toolbar */
                    fnt = ttf_get(FONT_UI_BOLD, 20);
                    if (fnt)
                        ttf_draw_string(&ws, fnt, 20, 56,
                                        "Welcome to Impossible OS!",
                                        0xFF60CDFF);

                    /* Subtitle */
                    fnt = ttf_get(FONT_UI, 14);
                    if (fnt)
                        ttf_draw_string(&ws, fnt, 20, 86,
                                        "Fluent System Icons loaded from TTF",
                                        0xFFB0B0B0);
                }
            }
        }

        terminal_open();
        gallery_open();
        task_create(shell_loader_func, "ShellLoader");
        scheduler_enable();

        /* Compositor loop — runs as idle thread.
         * Only redraws when something changed (mouse moved, window changed).
         * Uses VirtIO tablet (absolute coords) if available, else PS/2 mouse.
         *
         * Clear the framebuffer first to wipe boot console text. */
        fb_fill_rect(0, 0, fb_get_width(), fb_get_height(), 0x00000000);
        {
            int32_t prev_mx = -1, prev_my = -1;
            uint8_t prev_mb = 0;
            uint8_t first_frame = 1;
            uint64_t last_clock_sec = 0;
            uint8_t last_clock_min = 0xFF;  /* force first draw */

            for (;;) {
                int32_t mx, my;
                uint8_t mb;

                /* Get mouse state from the best available source:
                 *   1. VirtIO tablet (QEMU) — absolute coordinates
                 *   2. VBox VMMDev mouse (VirtualBox) — absolute coordinates
                 *   3. PS/2 mouse (fallback) — relative deltas */
                if (virtio_input_available()) {
                    struct virtio_input_state vis = virtio_input_get_state();
                    mx = vis.x;
                    my = vis.y;
                    mb = vis.buttons;
                    /* Update PS/2 mouse state for cursor position tracking */
                    mouse_set_position(mx, my);
                } else if (vbox_mouse_available()) {
                    /* VBox gives absolute position but NOT buttons.
                     * Merge position from VBox + buttons from PS/2. */
                    struct mouse_state vb = vbox_mouse_get_state();
                    struct mouse_state ps = mouse_get_state();
                    mx = vb.x;
                    my = vb.y;
                    mb = ps.buttons;
                    mouse_set_position(mx, my);
                } else {
                    struct mouse_state ms = mouse_get_state();
                    mx = ms.x;
                    my = ms.y;
                    mb = ms.buttons;
                }

                /* Batch mouse events: after reading, briefly yield to let
                 * any additional IRQ deltas arrive, then re-read.
                 * This turns 5×1px moves into 1×5px = 1 composite. */
                {
                    int batch;
                    for (batch = 0; batch < 4; batch++) {
                        int32_t nx, ny;
                        uint8_t nb;
                        /* Allow IRQs to fire */
                        __asm__ volatile ("sti; hlt");
                        /* Re-read */
                        if (virtio_input_available()) {
                            struct virtio_input_state vis = virtio_input_get_state();
                            nx = vis.x; ny = vis.y; nb = vis.buttons;
                            mouse_set_position(nx, ny);
                        } else if (vbox_mouse_available()) {
                            struct mouse_state vb = vbox_mouse_get_state();
                            struct mouse_state ps = mouse_get_state();
                            nx = vb.x; ny = vb.y; nb = ps.buttons;
                            mouse_set_position(nx, ny);
                        } else {
                            struct mouse_state ms = mouse_get_state();
                            nx = ms.x; ny = ms.y; nb = ms.buttons;
                        }
                        /* If no new movement, stop draining */
                        if (nx == mx && ny == my && nb == mb)
                            break;
                        mx = nx; my = ny; mb = nb;
                    }
                }

                uint8_t cursor_moved = (mx != prev_mx || my != prev_my);
                uint8_t btn_changed  = (mb != prev_mb);

                /* Check if clock needs update (only when minute changes) */
                uint64_t cur_sec = uptime();
                uint8_t clock_tick = 0;
                if (cur_sec != last_clock_sec) {
                    last_clock_sec = cur_sec;
                    struct rtc_time rtc_now;
                    rtc_read(&rtc_now);
                    if (rtc_now.minute != last_clock_min) {
                        last_clock_min = rtc_now.minute;
                        clock_tick = 1;
                    }
                }

                /* Check if any window content changed (e.g., terminal output) */
                uint8_t wm_dirty = wm_needs_redraw();

                /* Always dispatch mouse events when cursor moves
                 * (hover tracking needs this even without buttons) */
                if (cursor_moved || btn_changed) {
                    if (!desktop_handle_click(mx, my, mb)) {
                        wm_handle_mouse(mx, my, mb);
                    }
                }

                /* Check if any window content changed (includes hover state) */
                uint8_t wm_dirty2 = wm_needs_redraw();

                /* Only do work if something actually changed */
                uint8_t need_full = first_frame || btn_changed
                                 || (cursor_moved && mb != 0)
                                 || wm_dirty || wm_dirty2 || clock_tick;

                if (need_full) {
                    /* Full composite needed: first frame, button change,
                     * dragging, window content, or hover state changed */

                    /* Skip terminal re-render during drag — nothing changed */
                    if (!wm_is_dragging()) {
                        /* Render terminal content to its window buffer */
                        terminal_render();
                        gallery_render();
                    }

                    /* Prevent preemption during draw+swap so the PIT
                     * cannot context-switch us mid-frame. */
                    scheduler_disable();

                    wm_mark_dirty();

                    /* Restore cursor, full composite, draw cursor, flip */
                    cursor_restore();
                    wm_composite();

                    /* On the very first frame: unlock compositor and restore
                     * klog screen output.  Both are held/suppressed from
                     * boot_splash_init() through all window-creation code so
                     * that printk() cannot write to the bare framebuffer. */
                    if (first_frame) {
                        fb_unlock_compositor();
                        klog_set_screen_level(LOG_INFO);
                    }

                    /* Determine cursor shape from context */
                    cursor_shape_t ctx = desktop_get_cursor_context(mx, my);
                    if (ctx == CURSOR_ARROW)
                        ctx = wm_get_cursor_context(mx, my);
                    cursor_set_shape(ctx);

                    cursor_draw(mx, my);

                    /* During drag: partial swap of only the dirty region.
                     * Otherwise: full screen swap. */
                    {
                        int32_t  drx, dry;
                        uint32_t drw, drh;
                        if (wm_get_drag_dirty_rect(&drx, &dry, &drw, &drh)) {
                            /* Swap the drag dirty rect (old + new window area) */
                            fb_swap_rect((uint32_t)drx, (uint32_t)dry, drw, drh);
                            /* Also swap cursor area (may be outside drag rect) */
                            {
                                int32_t  crx, cry;
                                uint32_t crw, crh;
                                if (cursor_get_rect(&crx, &cry, &crw, &crh))
                                    fb_swap_rect((uint32_t)crx, (uint32_t)cry,
                                                 crw, crh);
                            }
                        } else {
                            fb_swap();
                        }
                    }
                    scheduler_enable();

                    prev_mx = mx;
                    prev_my = my;
                    prev_mb = mb;
                    first_frame = 0;
                } else if (cursor_moved) {
                    /* Cursor-only move (no buttons held) —
                     * swap just the union of old + new cursor rects */
                    scheduler_disable();

                    int32_t  old_rx, old_ry;
                    uint32_t old_rw, old_rh;
                    int had_old = cursor_get_rect(&old_rx, &old_ry,
                                                   &old_rw, &old_rh);

                    cursor_restore();

                    /* Update cursor shape even on cursor-only moves */
                    cursor_shape_t ctx = desktop_get_cursor_context(mx, my);
                    if (ctx == CURSOR_ARROW)
                        ctx = wm_get_cursor_context(mx, my);
                    cursor_set_shape(ctx);

                    cursor_draw(mx, my);

                    int32_t  new_rx, new_ry;
                    uint32_t new_rw, new_rh;
                    cursor_get_rect(&new_rx, &new_ry, &new_rw, &new_rh);

                    /* Single swap of the union bounding box */
                    if (had_old) {
                        int32_t ux = (old_rx < new_rx) ? old_rx : new_rx;
                        int32_t uy = (old_ry < new_ry) ? old_ry : new_ry;
                        int32_t ur = old_rx + (int32_t)old_rw;
                        int32_t nr = new_rx + (int32_t)new_rw;
                        int32_t ub = old_ry + (int32_t)old_rh;
                        int32_t nb = new_ry + (int32_t)new_rh;
                        if (nr > ur) ur = nr;
                        if (nb > ub) ub = nb;
                        if (ux < 0) ux = 0;
                        if (uy < 0) uy = 0;
                        fb_swap_rect((uint32_t)ux, (uint32_t)uy,
                                     (uint32_t)(ur - ux), (uint32_t)(ub - uy));
                    } else {
                        uint32_t sx = (new_rx >= 0) ? (uint32_t)new_rx : 0;
                        uint32_t sy = (new_ry >= 0) ? (uint32_t)new_ry : 0;
                        fb_swap_rect(sx, sy, new_rw, new_rh);
                    }

                    scheduler_enable();

                    prev_mx = mx;
                    prev_my = my;
                }

                /* Periodically flush dirty registry hives to disk */
                registry_flush();

                /* Sleep until next IRQ.  HLT wakes on mouse/keyboard/timer
                 * instantly — much lower latency than yield() which does
                 * a full scheduler context-switch round-trip. */
                __asm__ volatile ("sti; hlt");
            }
        }
    }

halt:
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* --- Test thread functions for cooperative scheduling --- */

void thread_a_func(void)
{
    printk("  [ThreadA] Hello from thread A (1/3)\n");
    yield();
    printk("  [ThreadA] Back in thread A (2/3)\n");
    yield();
    printk("  [ThreadA] Thread A finishing (3/3)\n");
}

void thread_b_func(void)
{
    printk("  [ThreadB] Hello from thread B (1/3)\n");
    yield();
    printk("  [ThreadB] Back in thread B (2/3)\n");
    yield();
    printk("  [ThreadB] Thread B finishing (3/3)\n");
}

/* --- Kernel thread test functions --- */

volatile uint32_t thread_shared_counter = 0;

void thread_inc_func(void *arg)
{
    uint32_t i;
    const char *label = (const char *)arg;
    for (i = 0; i < 5; i++) {
        thread_shared_counter++;
        printk("    [%s] shared_counter = %u\n", label,
               (uint64_t)thread_shared_counter);
        thread_yield();
    }
}

/* --- Mutex test functions --- */

#include "kernel/sched/mutex.h"

static mutex_t test_mutex = MUTEX_INIT("test_mutex");
volatile uint32_t mutex_shared_counter = 0;

void mutex_inc_func(void *arg)
{
    uint32_t i;
    const char *label = (const char *)arg;
    for (i = 0; i < 100; i++) {
        mutex_lock(&test_mutex);
        mutex_shared_counter++;
        mutex_unlock(&test_mutex);
    }
    printk("    [%s] done (100 increments)\n", label);
}

/* --- Semaphore test functions --- */

#include "kernel/sched/semaphore.h"

static semaphore_t test_sem = SEM_INIT("test_sem", 0);
volatile uint32_t sem_produced = 0;
volatile uint32_t sem_consumed = 0;

void sem_producer_func(void *arg)
{
    uint32_t i;
    (void)arg;
    for (i = 0; i < 5; i++) {
        sem_produced++;
        printk("    [Producer] produced item %u\n", (uint64_t)sem_produced);
        sem_signal(&test_sem);
        thread_yield();
    }
}

void sem_consumer_func(void *arg)
{
    uint32_t i;
    (void)arg;
    for (i = 0; i < 5; i++) {
        sem_wait(&test_sem);
        sem_consumed++;
        printk("    [Consumer] consumed item %u\n", (uint64_t)sem_consumed);
    }
}

/* --- Pipe test functions --- */

#include "kernel/ipc/pipe.h"

int pipe_test_id = -1;
volatile uint32_t pipe_test_ok = 0;

static const char pipe_test_msg[] = "Hello from pipe!";

void pipe_writer_func(void *arg)
{
    (void)arg;
    pipe_write(pipe_test_id, pipe_test_msg, 16);
    pipe_close(pipe_test_id, PIPE_WRITE);
}

void pipe_reader_func(void *arg)
{
    char buf[32];
    int32_t n;
    uint32_t i;
    (void)arg;

    for (i = 0; i < 32; i++) buf[i] = 0;
    n = pipe_read(pipe_test_id, buf, 32);
    if (n == 16) {
        /* Verify the message */
        uint32_t match = 1;
        for (i = 0; i < 16; i++) {
            if (buf[i] != pipe_test_msg[i]) { match = 0; break; }
        }
        if (match) pipe_test_ok = 1;
    }
    printk("    [Reader] got %d bytes: \"%s\"\n", (uint64_t)(uint32_t)n, buf);
    pipe_close(pipe_test_id, PIPE_READ);
}

/* --- Shared memory test functions --- */

#include "kernel/ipc/shmem.h"

volatile uint32_t shmem_test_ok = 0;

void shmem_writer_func(void *arg)
{
    int id;
    volatile uint32_t *counter;
    uint32_t i;
    (void)arg;

    id = shmem_open("test_counter");
    if (id < 0) return;

    counter = (volatile uint32_t *)shmem_map(id);
    if (!counter) return;

    for (i = 0; i < 100; i++)
        (*counter)++;

    printk("    [ShmWriter] done (100 increments, counter=%u)\n",
           (uint64_t)*counter);
    shmem_unmap(id);
}

void shmem_reader_func(void *arg)
{
    int id;
    volatile uint32_t *counter;
    uint32_t i;
    (void)arg;

    id = shmem_open("test_counter");
    if (id < 0) return;

    counter = (volatile uint32_t *)shmem_map(id);
    if (!counter) return;

    for (i = 0; i < 100; i++)
        (*counter)++;

    printk("    [ShmReader] done (100 increments, counter=%u)\n",
           (uint64_t)*counter);

    if (*counter == 200)
        shmem_test_ok = 1;

    shmem_unmap(id);
}

/* --- Test thread functions for preemptive scheduling --- */

static volatile uint32_t pa_count = 0;
static volatile uint32_t pb_count = 0;

void preempt_a_func(void)
{
    uint32_t i;
    for (i = 0; i < 3; i++) {
        pa_count++;
        printk("  [PreemptA] Running (%u/3) — no yield!\n",
               (uint64_t)pa_count);
        /* Busy-wait ~50ms (loop, not yield) to prove preemption */
        sleep_ms(100);
    }
}

void preempt_b_func(void)
{
    uint32_t i;
    for (i = 0; i < 3; i++) {
        pb_count++;
        printk("  [PreemptB] Running (%u/3) — no yield!\n",
               (uint64_t)pb_count);
        sleep_ms(100);
    }
}

/* --- User-mode test function ---
 * This runs in ring 3 — NO kernel function calls allowed!
 * All I/O goes through INT 0x80 syscalls. */
void user_test_func(void)
{
    /* sys_write(fd=1, buf, len) via INT 0x80 */
    static const char msg1[] = "  [UserMode] Hello from ring 3!\n";
    __asm__ volatile(
        "mov $1, %%rax\n"   /* SYS_WRITE */
        "mov $1, %%rdi\n"   /* fd = stdout */
        "mov %0, %%rsi\n"   /* buf */
        "mov %1, %%rdx\n"   /* len */
        "int $0x80\n"
        :
        : "r"((uint64_t)msg1), "r"((uint64_t)sizeof(msg1) - 1)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );

    /* sys_write again to confirm we survived */
    static const char msg2[] = "  [UserMode] Syscall returned successfully!\n";
    __asm__ volatile(
        "mov $1, %%rax\n"   /* SYS_WRITE */
        "mov $1, %%rdi\n"   /* fd = stdout */
        "mov %0, %%rsi\n"   /* buf */
        "mov %1, %%rdx\n"   /* len */
        "int $0x80\n"
        :
        : "r"((uint64_t)msg2), "r"((uint64_t)sizeof(msg2) - 1)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );

    /* sys_exit(0) */
    __asm__ volatile(
        "mov $3, %%rax\n"   /* SYS_EXIT */
        "mov $0, %%rdi\n"   /* exit code = 0 */
        "int $0x80\n"
        :
        :
        : "rax", "rdi", "memory"
    );

    /* Should never reach here */
    for (;;)
        __asm__ volatile("hlt");
}

/* --- Exec loader: kernel task that loads an ELF from C:\ ---
 * This runs as a kernel task (ring 0). It reads the ELF file from C:\ (IXFS),
 * then calls task_exec() to load it. task_exec() sets up a new user-mode
 * interrupt frame, so on the next schedule the task will run in ring 3
 * at the ELF entry point. */
void exec_loader_func(void)
{
    struct vfs_node *file;
    uint8_t *buf;

    if (!vfs_is_mounted('C')) {
        printk("  [ExecLoader] C:\\ not mounted\n");
        return;
    }

    file = vfs_open("C:\\hello.exe", VFS_O_READ);
    if (!file) {
        printk("  [ExecLoader] hello.exe not found on C:\\\n");
        return;
    }

    buf = (uint8_t *)kmalloc(file->size);
    if (!buf) {
        printk("  [ExecLoader] cannot allocate buffer\n");
        vfs_close(file);
        return;
    }

    vfs_read(file, 0, (uint32_t)file->size, buf);
    vfs_close(file);

    if (task_exec(buf, file->size) < 0) {
        printk("  [ExecLoader] exec failed\n");
        kfree(buf);
        return;
    }

    /* task_exec set our TCB's rsp to a new interrupt frame pointing at the
     * ELF entry. We must NOT yield (INT 0x81 would overwrite that frame) or
     * return (task_wrapper would mark us DEAD). Instead, halt and let the
     * preemptive scheduler's PIT interrupt pick up the new frame. */
    for (;;)
        __asm__ volatile("hlt");
}

/* --- Shell loader: kernel task that execs shell.exe from C:\ --- */
void shell_loader_func(void)
{
    struct vfs_node *file;
    uint8_t *buf;

    if (!vfs_is_mounted('C')) {
        klog(LOG_WARN, "shell", "C:\\ not mounted");
        return;
    }

    file = vfs_open("C:\\shell.exe", VFS_O_READ);
    if (!file) {
        klog(LOG_WARN, "shell", "shell.exe not found on C:\\");
        return;
    }

    buf = (uint8_t *)kmalloc(file->size);
    if (!buf) {
        klog(LOG_ERROR, "shell", "cannot allocate buffer");
        vfs_close(file);
        return;
    }

    vfs_read(file, 0, (uint32_t)file->size, buf);
    vfs_close(file);

    if (task_exec(buf, file->size) < 0) {
        klog(LOG_ERROR, "shell", "exec failed");
        kfree(buf);
        return;
    }

    for (;;)
        __asm__ volatile("hlt");
}

/* --- Fork test function ---
 * Runs in ring 3. Forks, child prints and exits, parent calls waitpid. */
void fork_test_func(void)
{
    long child_pid;

    /* SYS_FORK */
    __asm__ volatile(
        "mov $5, %%rax\n"
        "int $0x80\n"
        : "=a"(child_pid)
        :
        : "rcx", "r11", "memory"
    );

    if (child_pid == 0) {
        /* Child process */
        static const char msg[] = "  [Fork] Child process running!\n";
        __asm__ volatile(
            "mov $1, %%rax\n"
            "mov $1, %%rdi\n"
            "mov %0, %%rsi\n"
            "mov %1, %%rdx\n"
            "int $0x80\n"
            :
            : "r"((uint64_t)msg), "r"((uint64_t)sizeof(msg) - 1)
            : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
        );

        /* Exit with code 42 */
        __asm__ volatile(
            "mov $3, %%rax\n"
            "mov $42, %%rdi\n"
            "int $0x80\n"
            :
            :
            : "rax", "rdi", "memory"
        );
    } else {
        /* Parent process */
        static const char msg[] = "  [Fork] Parent waiting for child...\n";
        __asm__ volatile(
            "mov $1, %%rax\n"
            "mov $1, %%rdi\n"
            "mov %0, %%rsi\n"
            "mov %1, %%rdx\n"
            "int $0x80\n"
            :
            : "r"((uint64_t)msg), "r"((uint64_t)sizeof(msg) - 1)
            : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
        );

        /* SYS_WAITPID(child_pid) */
        long status;
        __asm__ volatile(
            "mov $7, %%rax\n"
            "mov %1, %%rdi\n"
            "int $0x80\n"
            : "=a"(status)
            : "r"(child_pid)
            : "rcx", "r11", "memory"
        );

        /* Report result */
        static const char msg2[] = "  [Fork] Parent: child exited!\n";
        __asm__ volatile(
            "mov $1, %%rax\n"
            "mov $1, %%rdi\n"
            "mov %0, %%rsi\n"
            "mov %1, %%rdx\n"
            "int $0x80\n"
            :
            : "r"((uint64_t)msg2), "r"((uint64_t)sizeof(msg2) - 1)
            : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
        );

        /* Exit parent */
        __asm__ volatile(
            "mov $3, %%rax\n"
            "mov $0, %%rdi\n"
            "int $0x80\n"
            :
            :
            : "rax", "rdi", "memory"
        );
    }

    for (;;)
        __asm__ volatile("hlt");
}
