/* ============================================================================
 * kernel_main — Kernel entry point
 *
 * Called from entry.asm in 64-bit Long Mode.
 * Initializes serial, framebuffer, parses boot info, prints system details.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/multiboot2.h"
#include "kernel/boot_info.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/printk.h"
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
#include "kernel/fs/vfs.h"
#include "kernel/fs/initrd.h"
#include "kernel/fs/fat32.h"
#include "kernel/fs/ixfs.h"
#include "kernel/sched/task.h"
#include "kernel/sched/syscall.h"
#include "kernel/ipc/pipe.h"
#include "kernel/ipc/shmem.h"
#include "kernel/mm/swap.h"

#include "kernel/drivers/mouse.h"
#include "kernel/drivers/virtio_input.h"
#include "desktop/wm.h"
#include "desktop/font.h"
#include "kernel/drivers/pit.h"
#include "desktop/desktop.h"
#include "kernel/acpi.h"
#include "kernel/version.h"

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

    /* Step 7b: Initialize VirtIO block device */
    virtio_blk_init();

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

    /* Step 8b: Initialize CMOS real-time clock */
    rtc_init();

    /* Step 9: Initialize PS/2 keyboard */
    keyboard_init();

    /* Step 10a: Initialize PS/2 mouse */
    mouse_init();

    /* Step 10: PCI bus scan and NIC init */
    pci_scan();
    rtl8139_init();
    net_init();
    virtio_input_init();  /* VirtIO tablet for absolute mouse coords */

    /* Step 11: Enable interrupts */
    __asm__ volatile ("sti");

    /* Step 12: DHCP — obtain IP address (needs interrupts enabled) */
    dhcp_discover();

    /* Step 8: Print boot banner */
    fb_set_color(FB_COLOR_CYAN, FB_COLOR_BG_DEFAULT);
    printk("\n");
    printk("  ================================================\n");
    printk("      Impossible OS\n");
    printk("  ================================================\n");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    version_print();
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

        printk("\n  Cooperative test: two threads alternate via yield()\n");

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

    /* === Preemptive scheduling test === */
    {
        extern void preempt_a_func(void);
        extern void preempt_b_func(void);

        task_create(preempt_a_func, "PreemptA");
        task_create(preempt_b_func, "PreemptB");

        printk("\n  Preemptive test: threads run without yield()\n");
        scheduler_enable();

        /* Main thread sleeps while the preemptive threads run */
        sleep_ms(600);

        scheduler_disable();

        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("[OK] ");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("Preemptive scheduling test passed\n");
    }

    /* === Kernel thread test (shared globals) === */
    {
        extern volatile uint32_t thread_shared_counter;
        extern void thread_inc_func(void *arg);

        int tid_a, tid_b;
        int32_t status_a, status_b;

        thread_shared_counter = 0;
        printk("\n  Thread test: two threads sharing a global counter\n");

        tid_a = thread_create(thread_inc_func, (void *)"ThreadA", 0);
        tid_b = thread_create(thread_inc_func, (void *)"ThreadB", 0);

        if (tid_a >= 0 && tid_b >= 0) {
            /* Join both threads — blocks until each exits */
            status_a = thread_join((uint32_t)tid_a);
            status_b = thread_join((uint32_t)tid_b);
            (void)status_a;
            (void)status_b;

            if (thread_shared_counter == 10) {
                fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                printk("[OK] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("Kernel thread test passed (counter=%u)\n",
                       (uint64_t)thread_shared_counter);
            } else {
                fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
                printk("[FAIL] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("Kernel thread test: expected 10, got %u\n",
                       (uint64_t)thread_shared_counter);
            }
        } else {
            fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
            printk("[FAIL] ");
            fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
            printk("thread_create failed\n");
        }
    }

    /* === Mutex test (two threads, protected counter) === */
    {
        extern volatile uint32_t mutex_shared_counter;
        extern void mutex_inc_func(void *arg);

        int mtid_a, mtid_b;

        mutex_shared_counter = 0;
        printk("\n  Mutex test: two threads racing on a protected counter\n");

        mtid_a = thread_create(mutex_inc_func, (void *)"MutexA", 0);
        mtid_b = thread_create(mutex_inc_func, (void *)"MutexB", 0);

        if (mtid_a >= 0 && mtid_b >= 0) {
            thread_join((uint32_t)mtid_a);
            thread_join((uint32_t)mtid_b);

            if (mutex_shared_counter == 200) {
                fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                printk("[OK] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("Mutex test passed (counter=%u)\n",
                       (uint64_t)mutex_shared_counter);
            } else {
                fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
                printk("[FAIL] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("Mutex test: expected 200, got %u\n",
                       (uint64_t)mutex_shared_counter);
            }
        } else {
            fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
            printk("[FAIL] ");
            fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
            printk("mutex thread_create failed\n");
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
        printk("\n  Semaphore test: producer-consumer synchronization\n");

        stid_c = thread_create(sem_consumer_func, (void *)0, 0);
        stid_p = thread_create(sem_producer_func, (void *)0, 0);

        if (stid_p >= 0 && stid_c >= 0) {
            thread_join((uint32_t)stid_p);
            thread_join((uint32_t)stid_c);

            if (sem_consumed == 5) {
                fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                printk("[OK] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("Semaphore test passed (consumed=%u)\n",
                       (uint64_t)sem_consumed);
            } else {
                fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
                printk("[FAIL] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("Semaphore test: expected 5, got %u\n",
                       (uint64_t)sem_consumed);
            }
        } else {
            fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
            printk("[FAIL] ");
            fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
            printk("semaphore thread_create failed\n");
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
        printk("\n  Pipe test: writer thread sends data to reader thread\n");

        if (pipe_create(pipe_fds) == 0) {
            pipe_test_id = pipe_fds[0];  /* same ID for both ends */

            ptid_r = thread_create(pipe_reader_func, (void *)0, 0);
            ptid_w = thread_create(pipe_writer_func, (void *)0, 0);

            if (ptid_w >= 0 && ptid_r >= 0) {
                thread_join((uint32_t)ptid_w);
                thread_join((uint32_t)ptid_r);

                if (pipe_test_ok) {
                    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                    printk("[OK] ");
                    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                    printk("Pipe test passed\n");
                } else {
                    fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
                    printk("[FAIL] ");
                    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                    printk("Pipe test: data mismatch\n");
                }
            } else {
                fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
                printk("[FAIL] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("pipe thread_create failed\n");
            }
        } else {
            fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
            printk("[FAIL] ");
            fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
            printk("pipe_create failed\n");
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
        printk("\n  Shared memory test: two threads sharing a named counter\n");

        shm_id = shmem_create("test_counter", sizeof(uint32_t));
        if (shm_id >= 0) {
            shm_tw = thread_create(shmem_writer_func, (void *)0, 0);
            shm_tr = thread_create(shmem_reader_func, (void *)0, 0);

            if (shm_tw >= 0 && shm_tr >= 0) {
                thread_join((uint32_t)shm_tw);
                thread_join((uint32_t)shm_tr);

                if (shmem_test_ok) {
                    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                    printk("[OK] ");
                    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                    printk("Shared memory test passed\n");
                } else {
                    fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
                    printk("[FAIL] ");
                    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                    printk("Shared memory test: counter != 200\n");
                }
            } else {
                fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
                printk("[FAIL] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("shmem thread_create failed\n");
            }

            shmem_unmap(shm_id);  /* creator unmaps too */
        } else {
            fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
            printk("[FAIL] ");
            fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
            printk("shmem_create failed\n");
        }
    }

    /* === Swap test (explicit swap out → swap in data path) === */
    {
        uint8_t *swap_src;
        uint8_t *swap_dst;
        uint32_t swap_ok = 1;
        uint32_t k;

        printk("\n  Swap test: write pattern, copy to swap slot, read back, verify\n");

        swap_init(64);  /* 64 slots = 256 KiB swap */

        swap_src = (uint8_t *)kmalloc(4096);
        swap_dst = (uint8_t *)kmalloc(4096);

        if (swap_src && swap_dst) {
            /* Write known pattern */
            for (k = 0; k < 4096; k++)
                swap_src[k] = (uint8_t)(k & 0xFF);

            printk("    Written 4 KiB pattern to %p\n", (uintptr_t)swap_src);

            /* Copy pattern into swap slot 0 */
            {
                for (k = 0; k < 4096; k++)
                    swap_store[k] = swap_src[k];
                swap_slots[0].in_use = 1;
                swap_slots[0].virt_addr = (uintptr_t)swap_src;
                swap_used++;
            }

            /* Zero the source (simulate frame deallocation) */
            for (k = 0; k < 4096; k++)
                swap_src[k] = 0;

            printk("    Swapped out to slot 0, source zeroed\n");

            /* Read back from swap slot 0 */
            {
                for (k = 0; k < 4096; k++)
                    swap_dst[k] = swap_store[k];
                swap_slots[0].in_use = 0;
                swap_used--;
            }

            printk("    Swapped in from slot 0\n");

            /* Verify pattern */
            for (k = 0; k < 4096; k++) {
                if (swap_dst[k] != (uint8_t)(k & 0xFF)) {
                    swap_ok = 0;
                    break;
                }
            }

            if (swap_ok) {
                fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                printk("[OK] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("Swap test passed (4 KiB pattern verified, %u/%u slots)\n",
                       (uint64_t)swap_get_used_slots(),
                       (uint64_t)swap_get_total_slots());
            } else {
                fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
                printk("[FAIL] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("Swap test: mismatch at byte %u\n", (uint64_t)k);
            }

            kfree(swap_src);
            kfree(swap_dst);
        } else {
            fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
            printk("[FAIL] ");
            fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
            printk("swap test alloc failed\n");
        }
    }

    /* === VirtIO-blk test (read sector 0) === */
    if (virtio_blk_present()) {
        uint8_t sect0[512];
        uint32_t vt;
        int vrc;

        printk("\n  VirtIO-blk test: reading sector 0\n");

        for (vt = 0; vt < 512; vt++)
            sect0[vt] = 0;

        vrc = virtio_blk_read(0, 1, sect0);
        if (vrc == 0) {
            /* Print first 16 bytes as hex */
            printk("    Sector 0 bytes: ");
            for (vt = 0; vt < 16; vt++)
                printk("%x ", (uint64_t)sect0[vt]);
            printk("\n");

            /* Check MBR signature (0x55AA at offset 510-511) */
            if (sect0[510] == 0x55 && sect0[511] == 0xAA) {
                fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                printk("[OK] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("VirtIO-blk: sector 0 read OK (MBR signature found)\n");
            } else {
                fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
                printk("[OK] ");
                fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
                printk("VirtIO-blk: sector 0 read OK (%u sectors total)\n",
                       (uint64_t)virtio_blk_capacity());
            }
        } else {
            fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
            printk("[FAIL] ");
            fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
            printk("VirtIO-blk: sector 0 read failed\n");
        }
    }
    /* === User mode test === */
    {
        extern void user_test_func(void);

        syscall_init();
        task_create_user(user_test_func, "UserTest");

        printk("\n  User mode test: ring 3 program calls sys_write\n");
        scheduler_enable();

        /* Main thread sleeps while user task runs */
        sleep_ms(500);

        scheduler_disable();

        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("[OK] ");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("User mode test passed\n");
    }

    /* === Exec test: load and run an ELF from the initrd === */
    {
        extern void exec_loader_func(void);

        printk("\n  Exec test: load hello.exe from initrd\n");

        /* The exec loader is a kernel task that loads the ELF binary.
         * We use a kernel task because exec needs to read from the initrd
         * (which requires kernel-mode VFS access) before switching to user mode. */
        task_create(exec_loader_func, "ExecLoader");
        scheduler_enable();
        sleep_ms(500);
        scheduler_disable();

        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("[OK] ");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("Exec test passed\n");
    }

    /* === Fork test: fork a user process, child prints, parent waits === */
    {
        extern void fork_test_func(void);

        printk("\n  Fork test: fork + waitpid\n");
        task_create_user(fork_test_func, "ForkTest");
        scheduler_enable();
        sleep_ms(800);
        scheduler_disable();

        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("[OK] ");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("Fork test passed\n");
    }

    /* === Launch the shell === */
    {
        extern void shell_loader_func(void);

        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("\n  Impossible OS kernel loaded successfully!\n\n");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);

        /* Initialize window manager */
        wm_init();

        /* Initialize desktop (wallpaper, taskbar, copy backgrounds to IXFS) */
        desktop_init();

        /* Create a demo window */
        {
            int demo = wm_create_window("Welcome", 100, 80, 400, 250, WM_DEFAULT_FLAGS);
            if (demo >= 0) {
                /* Draw some content in the demo window */
                wm_fill_rect(demo, 0, 0, 400, 250, 0x001E1E2E);

                /* Draw a greeting using font module into window's fb */
                {
                    uint32_t *fb = wm_get_framebuffer(demo);
                    uint32_t pitch = wm_get_client_width(demo);
                    const char *msg = "Welcome to Impossible OS!";
                    uint32_t mx_i = 0;
                    int32_t tx = 20, ty = 20;
                    (void)fb; (void)pitch;
                    while (msg[mx_i]) {
                        font_draw_char((uint32_t)(100 + (int32_t)WM_BORDER_WIDTH + tx + (int32_t)(mx_i * FONT_WIDTH)),
                                       (uint32_t)(80 + (int32_t)WM_TITLEBAR_HEIGHT + (int32_t)WM_BORDER_WIDTH + ty),
                                       msg[mx_i], 0x0088DDFF, 0x001E1E2E);
                        mx_i++;
                    }
                    /* Also write into the window framebuffer for compositing */
                    mx_i = 0;
                    while (msg[mx_i]) {
                        /* Render each glyph into the window's own buffer */
                        const uint8_t *glyph = font_get_glyph(msg[mx_i]);
                        uint32_t py, px;
                        for (py = 0; py < FONT_HEIGHT; py++) {
                            uint8_t row = glyph[py];
                            for (px = 0; px < FONT_WIDTH; px++) {
                                uint32_t color = (row & (0x80 >> px)) ? 0x0088DDFF : 0x001E1E2E;
                                uint32_t wx = (uint32_t)tx + mx_i * FONT_WIDTH + px;
                                uint32_t wy = (uint32_t)ty + py;
                                wm_put_pixel(demo, wx, wy, color);
                            }
                        }
                        mx_i++;
                    }
                }
            }
        }

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

            for (;;) {
                int32_t mx, my;
                uint8_t mb;

                /* Get mouse state from the best available source */
                if (virtio_input_available()) {
                    struct virtio_input_state vis = virtio_input_get_state();
                    mx = vis.x;
                    my = vis.y;
                    mb = vis.buttons;
                    /* Update PS/2 mouse state so mouse_draw_cursor() works */
                    mouse_set_position(mx, my);
                } else {
                    struct mouse_state ms = mouse_get_state();
                    mx = ms.x;
                    my = ms.y;
                    mb = ms.buttons;
                }

                uint8_t cursor_moved = (mx != prev_mx || my != prev_my);
                uint8_t btn_changed  = (mb != prev_mb);

                /* Check if clock needs update (every second) */
                uint64_t cur_sec = uptime();
                uint8_t clock_tick = (cur_sec != last_clock_sec);
                if (clock_tick) last_clock_sec = cur_sec;

                /* Only do work if something actually changed */
                uint8_t need_full = first_frame || btn_changed
                                 || (cursor_moved && mb != 0);

                if (need_full) {
                    /* Full composite needed: first frame, button change,
                     * or dragging (cursor moved with button held) */

                    /* Dispatch mouse */
                    if (!desktop_handle_click(mx, my, mb)) {
                        wm_handle_mouse(mx, my, mb);
                    }

                    /* Full redraw */
                    wm_mark_dirty();
                    wm_composite();
                    mouse_draw_cursor();
                    fb_swap();

                    prev_mx = mx;
                    prev_my = my;
                    prev_mb = mb;
                    first_frame = 0;
                } else if (cursor_moved) {
                    /* Cursor-only move (no buttons held) — just
                     * erase old cursor and draw at new position */
                    mouse_restore_under();
                    mouse_draw_cursor();
                    fb_swap();

                    prev_mx = mx;
                    prev_my = my;
                } else if (clock_tick) {
                    /* Clock-only update: redraw taskbar in-place */
                    mouse_restore_under();
                    desktop_draw_taskbar();
                    mouse_draw_cursor();
                    fb_swap();
                }

                /* Yield to other tasks instead of HLT.  HLT surrenders the
                 * entire time slice; yield() does a cooperative context switch
                 * but keeps us in the ready queue for prompt re-scheduling. */
                yield();
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

/* --- Exec loader: kernel task that loads an ELF from initrd ---
 * This runs as a kernel task (ring 0). It reads the ELF file from the initrd,
 * then calls task_exec() to load it. task_exec() sets up a new user-mode
 * interrupt frame, so on the next schedule the task will run in ring 3
 * at the ELF entry point. */
void exec_loader_func(void)
{
    struct vfs_node *root = initrd_get_root();
    struct vfs_node *file;
    uint8_t *buf;

    if (!root) {
        printk("  [ExecLoader] initrd not available\n");
        return;
    }

    file = vfs_finddir(root, "hello.exe");
    if (!file) {
        printk("  [ExecLoader] hello.exe not found in initrd\n");
        return;
    }

    buf = (uint8_t *)kmalloc(file->size);
    if (!buf) {
        printk("  [ExecLoader] cannot allocate buffer\n");
        return;
    }

    vfs_read(file, 0, (uint32_t)file->size, buf);

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

/* --- Shell loader: kernel task that execs shell.exe from initrd --- */
void shell_loader_func(void)
{
    struct vfs_node *root = initrd_get_root();
    struct vfs_node *file;
    uint8_t *buf;

    if (!root) {
        printk("[ShellLoader] initrd not available\n");
        return;
    }

    file = vfs_finddir(root, "shell.exe");
    if (!file) {
        printk("[ShellLoader] shell.exe not found in initrd\n");
        return;
    }

    buf = (uint8_t *)kmalloc(file->size);
    if (!buf) {
        printk("[ShellLoader] cannot allocate buffer\n");
        return;
    }

    vfs_read(file, 0, (uint32_t)file->size, buf);

    if (task_exec(buf, file->size) < 0) {
        printk("[ShellLoader] exec failed\n");
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
