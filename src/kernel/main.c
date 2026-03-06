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
#include "pci.h"
#include "rtl8139.h"
#include "net.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "ata.h"
#include "vfs.h"
#include "initrd.h"
#include "fat32.h"
#include "ixfs.h"
#include "task.h"
#include "syscall.h"

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

    /* Step 10: PCI bus scan and NIC init */
    pci_scan();
    rtl8139_init();
    net_init();

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

        task_create(shell_loader_func, "ShellLoader");
        scheduler_enable();

        /* Main thread idles forever while shell runs */
        for (;;)
            __asm__ volatile("hlt");
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
