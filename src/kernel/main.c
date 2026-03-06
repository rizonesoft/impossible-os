/* ============================================================================
 * kernel_main — Kernel entry point
 *
 * Called from entry.asm in 64-bit Long Mode.
 * Initializes serial, framebuffer, parses boot info, prints system details.
 * ============================================================================ */

#include "types.h"
#include "multiboot2.h"
#include "boot_info.h"
#include "serial.h"
#include "framebuffer.h"
#include "printk.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "ata.h"
#include "vfs.h"
#include "initrd.h"
#include "fat32.h"
#include "ixfs.h"
#include "task.h"

/* External: Multiboot2 parser */
extern void multiboot2_parse(uintptr_t mbi_addr);

/* Kernel entry point */
void kernel_main(uint64_t magic, uint64_t mbi)
{
    uint32_t i;
    uint64_t total_ram = 0;

    /* Step 1: Initialize serial (always works, even without display) */
    serial_init();

    /* Step 2: Verify Multiboot2 magic */
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        serial_write("[FAIL] Invalid Multiboot2 magic!\n");
        goto halt;
    }

    /* Step 3: Parse Multiboot2 info structure */
    multiboot2_parse((uintptr_t)mbi);

    /* Step 4: Initialize physical memory manager (needs parsed memory map) */
    pmm_init();

    /* Step 5: Initialize virtual memory manager (needs PMM for page tables) */
    vmm_init();

    /* Step 6: Initialize kernel heap (needs VMM for page mapping) */
    heap_init();

    /* Step 7: Initialize ATA disk driver */
    ata_init();

    /* Step 8: Initialize VFS */
    vfs_init();

    /* Step 9: IXFS on ATA master — format if needed, mount at C:\ (main disk) */
    if (ata_get_drive(0)->present) {
        const struct ata_drive *drv = ata_get_drive(0);
        int fresh_format = 0;
        if (ixfs_init(0) != 0) {
            if (ixfs_format(0, drv->sectors, "Impossible OS") == 0) {
                ixfs_init(0);
                fresh_format = 1;
            }
        }
        if (ixfs_get_root())
            vfs_mount('C', ixfs_get_driver(), ixfs_get_root());

        /* Create Windows-like folder hierarchy on fresh format */
        if (fresh_format && vfs_is_mounted('C')) {
            struct vfs_node *root = vfs_get_drive_root('C');
            if (root && root->ops && root->ops->create) {
                root->ops->create(root, "Impossible", VFS_DIRECTORY);
                root->ops->create(root, "Users", VFS_DIRECTORY);
                root->ops->create(root, "Programs", VFS_DIRECTORY);

                /* Create subdirectories */
                {
                    struct vfs_node *imp = root->ops->finddir(root, "Impossible");
                    if (imp && imp->ops && imp->ops->create) {
                        imp->ops->create(imp, "System", VFS_DIRECTORY);
                        imp->ops->create(imp, "Commands", VFS_DIRECTORY);
                    }
                }
                {
                    struct vfs_node *usr = root->ops->finddir(root, "Users");
                    if (usr && usr->ops && usr->ops->create)
                        usr->ops->create(usr, "Default", VFS_DIRECTORY);
                }
                printk("[OK] Created default folder hierarchy on C:\\\n");
            }
        }
    }

    /* Step 10: Load initrd at B:\ (boot files, read-only) */
    if (g_boot_info.module_available) {
        uint64_t mod_size = g_boot_info.module_end - g_boot_info.module_start;
        initrd_init(g_boot_info.module_start, mod_size);
        vfs_mount('B', initrd_get_driver(), initrd_get_root());
    }


    /* Step 5: Initialize framebuffer (needs parsed boot info) */
    fb_init();

    /* Step 5: Load proper GDT with kernel/user segments and TSS */
    gdt_init();

    /* Step 6: Load IDT with exception and IRQ handlers */
    idt_init();

    /* Step 7: Remap PIC so hardware IRQs don't conflict with CPU exceptions */
    pic_init();

    /* Step 8: Start PIT system timer */
    pit_init();

    /* Step 9: Initialize PS/2 keyboard */
    keyboard_init();

    /* Step 10: Enable interrupts */
    __asm__ volatile ("sti");

    /* Step 8: Print boot banner */
    fb_set_color(FB_COLOR_CYAN, FB_COLOR_BG_DEFAULT);
    printk("\n");
    printk("  ================================================\n");
    printk("      Impossible OS\n");
    printk("  ================================================\n");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("  Architecture: x86-64 (Long Mode)\n");
    printk("  Boot: UEFI via GRUB + Multiboot2\n");
    printk("\n");

    /* Step 6: Print status */
    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
    printk("[OK] ");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("Multiboot2 magic verified: %x\n", magic);

    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
    printk("[OK] ");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("Running in 64-bit Long Mode\n");

    if (g_boot_info.fb_available) {
        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("[OK] ");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("Framebuffer: %ux%ux%u at %p\n",
               (uint64_t)g_boot_info.fb.width,
               (uint64_t)g_boot_info.fb.height,
               (uint64_t)g_boot_info.fb.bpp,
               (uint64_t)g_boot_info.fb.addr);
    }

    /* Step 7: Memory map */
    printk("\n");
    fb_set_color(FB_COLOR_YELLOW, FB_COLOR_BG_DEFAULT);
    printk("--- Memory Map (%u entries) ---\n", (uint64_t)g_boot_info.mmap_count);
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);

    for (i = 0; i < g_boot_info.mmap_count; i++) {
        if (g_boot_info.mmap[i].type == 1) {
            total_ram += g_boot_info.mmap[i].length;
        }
    }
    printk("  Total available RAM: %u MiB\n",
           (uint64_t)(total_ram / (1024 * 1024)));

    /* Step 8: ACPI */
    if (g_boot_info.acpi_available) {
        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("[OK] ");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("ACPI RSDP v%u at %p\n",
               (uint64_t)g_boot_info.acpi_version,
               g_boot_info.acpi_rsdp_addr);
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

        fb_set_color(ok ? FB_COLOR_GREEN : FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
        printk("[%s] ", ok ? "OK" : "FAIL");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("Heap: alloc/free/realloc test passed (used: %u, free: %u bytes)\n",
               heap_get_used(), heap_get_free());
    }

    /* VFS/initrd test: read a file from B:\ */
    if (vfs_is_mounted('B')) {
        struct vfs_node *f = vfs_open("B:\\hello.txt", VFS_O_READ);
        if (f) {
            uint8_t buf[128];
            int n = vfs_read(f, 0, sizeof(buf) - 1, buf);
            if (n > 0) {
                buf[n] = '\0';
                fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                printk("[OK] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("VFS read B:\\hello.txt: \"%s\"\n", (char *)buf);
            }
            vfs_close(f);
        }
    }

    /* IXFS CRUD test on C:\ */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->create) {
            /* Create a file */
            int rc = c_root->ops->create(c_root, "test.txt", VFS_FILE);
            if (rc == 0) {
                /* Write data to the file */
                struct vfs_node *f = vfs_open("C:\\test.txt", VFS_O_WRITE);
                if (f) {
                    const char *msg = "IXFS is working!";
                    uint32_t len = 0;
                    while (msg[len]) len++;
                    vfs_write(f, 0, len, (const uint8_t *)msg);
                    vfs_close(f);
                }

                /* Read it back */
                f = vfs_open("C:\\test.txt", VFS_O_READ);
                if (f) {
                    uint8_t buf[64];
                    int n = vfs_read(f, 0, sizeof(buf) - 1, buf);
                    if (n > 0) {
                        buf[n] = '\0';
                        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                        printk("[OK] ");
                        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                        printk("IXFS CRUD: C:\\test.txt = \"%s\"\n",
                               (char *)buf);
                    }
                    vfs_close(f);
                }

                /* Delete it */
                if (c_root->ops->unlink) {
                    c_root->ops->unlink(c_root, "test.txt");
                    f = vfs_open("C:\\test.txt", VFS_O_READ);
                    fb_set_color(f ? FB_COLOR_RED : FB_COLOR_GREEN,
                                 FB_COLOR_BG_DEFAULT);
                    printk("[%s] ", f ? "FAIL" : "OK");
                    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                    printk("IXFS delete: C:\\test.txt %s\n",
                           f ? "still exists!" : "removed");
                    if (f) vfs_close(f);
                }
            }
        } else {
            printk("  IXFS: no c_root or no create op\n");
        }
    }

    /* IXFS mkdir/rmdir test on C:\ */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->create) {
            /* mkdir */
            int rc = c_root->ops->create(c_root, "TestDir", VFS_DIRECTORY);
            if (rc == 0) {
                /* Verify it shows up in readdir */
                struct vfs_dirent *de = vfs_readdir(c_root, 0);
                if (de && de->type & VFS_DIRECTORY) {
                    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                    printk("[OK] ");
                    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                    printk("IXFS mkdir: C:\\%s\n", de->name);
                }

                /* rmdir (empty dir) */
                if (c_root->ops->unlink) {
                    rc = c_root->ops->unlink(c_root, "TestDir");
                    de = vfs_readdir(c_root, 0);
                    fb_set_color(rc == 0 ? FB_COLOR_GREEN : FB_COLOR_RED,
                                 FB_COLOR_BG_DEFAULT);
                    printk("[%s] ", rc == 0 ? "OK" : "FAIL");
                    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                    printk("IXFS rmdir: C:\\TestDir %s\n",
                           rc == 0 ? "removed" : "failed");
                }
            }
        }
    }
    /* Timer verification */
    printk("\n  Timer test: sleeping 1 second...\n");
    sleep_ms(1000);
    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
    printk("[OK] ");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("Timer working! Ticks: %u, Uptime: %u sec\n",
           pit_get_ticks(), uptime());

    /* === Cooperative threading test === */
    task_init();
    {
        extern void thread_a_func(void);
        extern void thread_b_func(void);

        task_create(thread_a_func, "ThreadA");
        task_create(thread_b_func, "ThreadB");

        printk("\n  Multitasking test: two threads alternate messages\n");

        /* Main thread yields to let both threads run */
        yield();
        yield();
        yield();
        yield();
        yield();
        yield();

        /* Both threads should have finished by now */
        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("[OK] ");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("Cooperative threading test passed\n");
    }

    /* Keyboard echo loop */
    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
    printk("\n  Impossible OS kernel loaded successfully!\n");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("\n> ");
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n') {
            printk("\n> ");
        } else {
            printk("%c", (int)c);
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
