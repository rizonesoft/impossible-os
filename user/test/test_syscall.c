/* ============================================================================
 * test_syscall.c -- §2 stub binary for user/include/syscall.h parity
 *
 * Smoke-checks that user/include/syscall.h still links cleanly after the
 * §2 INT 0x80 ABI sync. This stub deliberately calls ONLY SYS_WRITE so
 * the §2 PASS criterion ("clang user-mode build of test_syscall.c
 * succeeds with no undefined sys_* reference") can be observed without
 * pulling in unwired syscalls (SYS_MMAP/SYS_MUNMAP).
 *
 * The full per-syscall coverage (every sys_* wrapper exercised + return
 * value asserted) is §9's job (`Syscall test binary`).
 *
 * Linked against the same crt0 + libc as hello.exe / cmd.exe /
 * test_harness_smoke.exe.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

int main(void)
{
    UTEST_BEGIN("test_syscall_stub");

    /* sys_write returns the byte count on success. The constant 7
     * matches the literal "[STUB]\n" payload length. */
    UTEST_ASSERT(sys_write(STDOUT_FD, "[STUB]\n", 7) == 7,
                 "sys_write(STDOUT_FD, ..., 7) returns 7");

    /* SECURITY REGRESSION: sys_log(LOG_FATAL, ...) must return -1.
     * The kernel SYS_LOG handler rejects level >= LOG_FATAL because
     * klog(LOG_FATAL, ...) enters an infinite hlt loop, which any
     * unprivileged user binary could otherwise weaponize as a
     * one-line OS halt. If this assertion FAILs, the test binary
     * is still alive only by luck (the hlt loop never executed),
     * meaning the kernel rejection accidentally regressed -- which
     * IS the bug we want surfaced. Caught by Codex adversarial
     * review of the user-mode test framework syscall.h sync work;
     * fix at src/kernel/sched/syscall.c
     * SYS_LOG case (`lvl >= LOG_FATAL`). */
    UTEST_ASSERT(sys_log(LOG_FATAL, "this must not halt", 18) == -1,
                 "sys_log(LOG_FATAL) rejected (would halt OS otherwise)");

    /* Sanity: a normal LOG_INFO still works. */
    UTEST_ASSERT(sys_log(LOG_INFO, "test_syscall: hello", 19) == 0,
                 "sys_log(LOG_INFO) returns 0");

    UTEST_END();
    return g_fail;
}
