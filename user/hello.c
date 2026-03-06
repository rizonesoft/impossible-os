/* ============================================================================
 * hello.c — Minimal user-mode ELF test program
 *
 * This is compiled as a freestanding ELF binary, loaded at 0x400000.
 * All I/O goes through INT 0x80 syscalls.
 * ============================================================================ */

/* Syscall numbers (must match kernel's syscall.h) */
#define SYS_WRITE   1
#define SYS_EXIT    3

typedef unsigned long uint64_t;

static inline long syscall2(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline void syscall1(long nr, long a1)
{
    __asm__ volatile(
        "int $0x80"
        :
        : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory"
    );
}

static uint64_t strlen(const char *s)
{
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

void _start(void)
{
    const char msg[] = "  [ELF] Hello from user-mode ELF binary!\n";
    syscall2(SYS_WRITE, 1, (long)msg, (long)strlen(msg));

    const char msg2[] = "  [ELF] About to call sys_exit(42)\n";
    syscall2(SYS_WRITE, 1, (long)msg2, (long)strlen(msg2));

    syscall1(SYS_EXIT, 42);

    /* Should never reach here */
    for (;;) ;
}
