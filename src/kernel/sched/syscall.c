/* ============================================================================
 * syscall.c — System call dispatcher and implementations
 *
 * Handles INT 0x80 from user-mode code.
 * Syscall number in RAX, arguments in RDI, RSI, RDX.
 * Return value placed back in frame->rax.
 * ============================================================================ */

#include "syscall.h"
#include "idt.h"
#include "task.h"
#include "printk.h"
#include "keyboard.h"

/* --- Syscall implementations --- */

/* SYS_WRITE: write data to a file descriptor.
 * Currently only fd=1 (stdout) is supported → serial/framebuffer. */
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

/* SYS_READ: read data from a file descriptor.
 * Currently only fd=0 (stdin) is supported → keyboard buffer. */
static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len)
{
    uint64_t i;
    char *dst = (char *)buf;

    if (fd != STDIN_FD)
        return -1;
    if (!dst || len == 0)
        return 0;

    /* Non-blocking: read whatever is available */
    for (i = 0; i < len; i++) {
        char c = keyboard_trygetchar();
        if (c == 0)
            break;
        dst[i] = c;
    }
    return (int64_t)i;
}

/* SYS_EXIT: terminate the current task. */
static void sys_exit(uint64_t code)
{
    struct task *cur = task_current();
    printk("[SCHED] Task %u (\"%s\") called sys_exit(%u)\n",
           (uint64_t)cur->pid, cur->name ? cur->name : "?", code);
    cur->state = TASK_DEAD;

    /* Yield away — we're dead */
    for (;;)
        yield();
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
        /* never returns */
        break;
    case SYS_YIELD:
        yield();
        ret = 0;
        break;
    default:
        printk("[WARN] Unknown syscall %u from PID %u\n",
               syscall_nr, (uint64_t)task_current()->pid);
        ret = -1;
        break;
    }

    /* Place return value in RAX of the caller's frame */
    frame->rax = (uint64_t)ret;
    return (uint64_t)frame;
}

void syscall_init(void)
{
    idt_register_handler(0x80, syscall_handler);
    printk("[OK] Syscall handler registered (INT 0x80)\n");
}
