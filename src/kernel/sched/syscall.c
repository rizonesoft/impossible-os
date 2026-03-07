/* ============================================================================
 * syscall.c — System call dispatcher and implementations
 *
 * Handles INT 0x80 from user-mode code.
 * Syscall number in RAX, arguments in RDI, RSI, RDX.
 * Return value placed back in frame->rax.
 * ============================================================================ */

#include "kernel/sched/syscall.h"
#include "kernel/idt.h"
#include "kernel/sched/task.h"
#include "kernel/printk.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/serial.h"
#include "kernel/fs/initrd.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/heap.h"
#include "kernel/drivers/pit.h"
#include "kernel/net/net.h"

/* --- Port I/O helper --- */
static inline void outb_sc(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw_sc(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* --- Input helper: try both keyboard and serial --- */
static char input_trygetchar(void)
{
    char c;
    c = keyboard_trygetchar();
    if (c) return c;
    c = serial_trygetchar();
    return c;
}

/* --- Syscall implementations --- */

/* SYS_WRITE: write data to a file descriptor. */
static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len)
{
    uint64_t i;
    const char *str = (const char *)buf;

    if (fd != STDOUT_FD)
        return -1;
    if (!str || len == 0)
        return 0;

    for (i = 0; i < len; i++) {
        if (str[i] == '\0')
            break;
        printk("%c", (int)str[i]);
    }
    return (int64_t)i;
}

/* SYS_READ: blocking read from stdin (keyboard + serial). */
static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len)
{
    uint64_t i;
    char *dst = (char *)buf;
    char c;

    if (fd != STDIN_FD)
        return -1;
    if (!dst || len == 0)
        return 0;

    /* Wait for the first byte (from keyboard OR serial) */
    for (;;) {
        c = input_trygetchar();
        if (c != 0)
            break;
        yield();
    }
    dst[0] = c;

    /* Fill remaining bytes non-blocking */
    for (i = 1; i < len; i++) {
        c = input_trygetchar();
        if (c == 0)
            break;
        dst[i] = c;
    }
    return (int64_t)i;
}

/* SYS_EXIT: terminate the current task. */
static void sys_exit(uint64_t code)
{
    task_exit((int32_t)code);
}

/* SYS_READFILE: read a file from the initrd by name. */
static int64_t sys_readfile(uint64_t name_ptr, uint64_t buf_ptr,
                            uint64_t buf_size)
{
    const char *name = (const char *)name_ptr;
    uint8_t *buf = (uint8_t *)buf_ptr;
    struct vfs_node *root = initrd_get_root();
    struct vfs_node *file;
    uint64_t to_read;

    if (!root || !name || !buf || buf_size == 0)
        return -1;

    file = vfs_finddir(root, name);
    if (!file)
        return -1;

    to_read = file->size < buf_size ? file->size : buf_size;
    vfs_read(file, 0, (uint32_t)to_read, buf);
    return (int64_t)to_read;
}

/* SYS_READDIR: read a directory entry at index from the initrd root. */
static int64_t sys_readdir(uint64_t buf_ptr, uint64_t buf_size,
                           uint64_t index)
{
    char *buf = (char *)buf_ptr;
    struct vfs_node *root = initrd_get_root();
    struct vfs_dirent *entry;
    uint64_t i;

    if (!root || !buf || buf_size == 0)
        return -1;

    entry = vfs_readdir(root, (uint32_t)index);
    if (!entry)
        return -1;

    for (i = 0; i < buf_size - 1 && entry->name[i]; i++)
        buf[i] = entry->name[i];
    buf[i] = '\0';
    return 0;
}

/* Process info structure — must match user/include/syscall.h */
struct proc_info {
    uint32_t pid;
    uint32_t state;
    char     name[32];
};

/* SYS_GETPROCS: fill buffer with process info. Returns count. */
static int64_t sys_getprocs(uint64_t buf_ptr, uint64_t buf_size)
{
    struct proc_info *out = (struct proc_info *)buf_ptr;
    uint32_t count = task_count();
    uint32_t max = (uint32_t)(buf_size / sizeof(struct proc_info));
    uint32_t i, j;

    if (!out || max == 0)
        return (int64_t)count;

    for (i = 0; i < count && i < max; i++) {
        struct task *t = task_get_by_pid(i);
        if (!t) {
            out[i].pid = i;
            out[i].state = 0xFF;
            out[i].name[0] = '\0';
            continue;
        }
        out[i].pid = t->pid;
        out[i].state = t->state;
        if (t->name) {
            for (j = 0; j < 31 && t->name[j]; j++)
                out[i].name[j] = t->name[j];
            out[i].name[j] = '\0';
        } else {
            out[i].name[0] = '\0';
        }
    }
    return (int64_t)count;
}

/* --- INT 0x80 handler --- */

static uint64_t syscall_handler(struct interrupt_frame *frame)
{
    uint64_t syscall_nr = frame->rax;
    uint64_t arg1 = frame->rdi;
    uint64_t arg2 = frame->rsi;
    uint64_t arg3 = frame->rdx;
    int64_t ret = -1;

    switch (syscall_nr) {
    case SYS_WRITE:
        ret = sys_write(arg1, arg2, arg3);
        break;
    case SYS_READ:
        ret = sys_read(arg1, arg2, arg3);
        break;
    case SYS_EXIT:
        sys_exit(arg1);
        break;
    case SYS_YIELD:
        yield();
        ret = 0;
        break;
    case SYS_FORK:
        ret = (int64_t)task_fork(frame);
        break;
    case SYS_EXEC: {
        const char *filename = (const char *)arg1;
        struct vfs_node *root = initrd_get_root();
        struct vfs_node *file;

        if (!root || !filename) { ret = -1; break; }

        file = vfs_finddir(root, filename);
        if (!file) {
            printk("[EXEC] File not found: %s\n", filename);
            ret = -1;
            break;
        }

        {
            uint8_t *buf = (uint8_t *)kmalloc(file->size);
            if (!buf) { ret = -1; break; }
            vfs_read(file, 0, file->size, buf);
            ret = (int64_t)task_exec(buf, file->size);
            if (ret < 0) kfree(buf);
        }
        break;
    }
    case SYS_WAITPID:
        ret = (int64_t)task_waitpid((uint32_t)arg1);
        break;
    case SYS_READFILE:
        ret = sys_readfile(arg1, arg2, arg3);
        break;
    case SYS_READDIR:
        ret = sys_readdir(arg1, arg2, arg3);
        break;
    case SYS_GETPROCS:
        ret = sys_getprocs(arg1, arg2);
        break;
    case SYS_KILL:
        if (arg1 > 0 && arg1 < task_count()) {
            struct task *t = task_get_by_pid((uint32_t)arg1);
            if (t && t->state != TASK_DEAD) {
                t->state = TASK_DEAD;
                t->exit_status = -1;
                printk("[KILL] Task %u killed\n", (uint64_t)arg1);
                ret = 0;
            } else {
                ret = -1;
            }
        } else {
            ret = -1;
        }
        break;
    case SYS_UPTIME:
        ret = (int64_t)uptime();
        break;
    case SYS_REBOOT:
        printk("[REBOOT] System rebooting...\n");
        outb_sc(0x64, 0xFE);
        for (;;) __asm__ volatile("hlt");
        break;
    case SYS_SHUTDOWN:
        printk("[SHUTDOWN] System shutting down...\n");
        outw_sc(0x604, 0x2000);     /* QEMU ACPI power-off */
        outw_sc(0xB004, 0x2000);    /* Bochs power-off */
        for (;;) __asm__ volatile("hlt");
        break;
    case SYS_PING: {
        /* arg1 = IP address (big-endian), arg2 = seq number */
        icmp_send_echo((uint32_t)arg1, 1, (uint16_t)arg2, "ImpOS", 5);
        ret = 0;
        break;
    }
    case SYS_NETINFO: {
        /* arg1 = user buffer, arg2 = buffer size */
        uint8_t *ubuf = (uint8_t *)arg1;
        uint64_t sz = arg2 < sizeof(net_cfg) ? arg2 : sizeof(net_cfg);
        uint64_t k;
        const uint8_t *src = (const uint8_t *)&net_cfg;
        if (ubuf) {
            for (k = 0; k < sz; k++)
                ubuf[k] = src[k];
        }
        ret = 0;
        break;
    }
    default:
        printk("[WARN] Unknown syscall %u from PID %u\n",
               syscall_nr, (uint64_t)task_current()->pid);
        ret = -1;
        break;
    }

    frame->rax = (uint64_t)ret;
    return (uint64_t)frame;
}

void syscall_init(void)
{
    idt_register_handler(0x80, syscall_handler);
    printk("[OK] Syscall handler registered (INT 0x80)\n");
}
