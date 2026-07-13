/* ============================================================================
 * test_process.c -- user-mode process-lifecycle coverage binary
 *
 * Exercises the fork/exec/waitpid/kill quartet from ring 3. Each sub-test
 * spawns exactly one child so the total task slots consumed per run is
 * 3 -- well within TASK_MAX headroom given the isolation task pool.
 *
 * Design:
 *   1. fork + exit(42)  -- child diverges via sys_exit(42); parent
 *                          waitpid returns 42. Proves fork's dual-return
 *                          (child=0, parent=child_pid) and the happy
 *                          waitpid path.
 *   2. fork + exec()    -- child forks, exec's hello.exe; hello's main
 *                          returns 42, crt0 sys_exit(42). Parent
 *                          waitpid returns 42. Proves exec actually
 *                          replaces the process image (would be the
 *                          test_process PID returning 0/g_fail if
 *                          exec silently no-op'd).
 *   3. fork + kill      -- child spins `while(1) sys_yield()`; parent
 *                          calls sys_kill(child_pid). Kernel SYS_KILL
 *                          sets the child's exit_status to -1; parent
 * waitpid returns -1. Safety net: the
 *                          launcher's 10s per-binary timeout kills the
 *                          whole test binary if kill is broken and the
 *                          child spins forever.
 *
 * Non-SMP assumption: the kernel scheduler today is single-CPU
 * (current_task is a global, task_current() reads it lock-free). Any
 * future SMP work in TODO-06 will need its own test once per-CPU
 * current_task lands.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

/* Literal file name for sys_exec (resolved under C:\ by the kernel). */
#define HELLO_EXE_NAME    "hello.exe"

int main(void)
{
    UTEST_BEGIN("test_process");

    /* ---- Sub-test 1: fork + exit-with-status ---------------------- *
     * Child exits with 42 via sys_exit (the wrapper loops forever
     * after the syscall so it never falls off the end of this
     * function -- the child's stack frame for main() is not
     * revisited). Parent waitpid drains the child and recovers its
     * exit status. */
    long pid1 = sys_fork();
    UTEST_ASSERT(pid1 >= 0,
                 "sys_fork returns >= 0 (either parent's child PID or child's 0)");

    if (pid1 == 0) {
        /* Child path -- exit immediately with a recognisable code. */
        sys_exit(42);
        /* sys_exit never returns; for paranoia, spin if it ever did. */
        for (;;) sys_yield();
    }

    UTEST_ASSERT(pid1 > 0,
                 "parent's sys_fork return is a positive child PID");
    long wait1 = sys_waitpid((int)pid1);
    UTEST_ASSERT(wait1 == 42,
                 "sys_waitpid recovers child's sys_exit(42) status");

    /* ---- Sub-test 2: fork + exec(hello.exe) ----------------------- *
     * hello.exe's main returns 42 (see user/hello.c); the ELF crt0
     * passes main's return to sys_exit(), so parent waitpid should
     * also see 42. A silent no-op exec (task_exec returns -1 and
     * control returns to the child) would instead have the child
     * fall through to sys_exit(99) below, yielding a distinct 99
     * that makes the failure mode obvious. */
    long pid2 = sys_fork();
    UTEST_ASSERT(pid2 >= 0,
                 "sys_fork before exec returns >= 0");

    if (pid2 == 0) {
        /* Child path -- replace our image with hello.exe, passing a known argv
         * so the child can prove the vector reached main() (hello returns 42
         * only for exactly {"hello.exe","alpha","beta"}). envp NULL inherits
         * our environment. */
        char *const child_argv[] = {
            (char *)HELLO_EXE_NAME, (char *)"alpha", (char *)"beta", (char *)0
        };
        long exec_rc = sys_exec(HELLO_EXE_NAME, child_argv, (char *const *)0);
        /* Only reached when exec FAILS (returns -1) -- successful
         * exec replaces the whole task image and never returns. */
        (void)exec_rc;
        sys_exit(99);
        for (;;) sys_yield();
    }

    UTEST_ASSERT(pid2 > 0,
                 "parent's fork-before-exec returns positive PID");
    long wait2 = sys_waitpid((int)pid2);
    /* hello returns 40+argc; with 3 args it returns 43 -- proves argc AND the
     * argv[] vector (walked via strlen) reached main() end-to-end. */
    UTEST_ASSERT(wait2 == 43,
                 "exec(hello.exe, argv) delivers argc/argv to main (returns 43)");

    /* ---- Sub-test 3: fork + kill --------------------------------- *
     * Child spins forever (bounded by the launcher's 10s watchdog as
     * a fail-safe). Parent kills the child; the kernel SYS_KILL sets
     * exit_status = -1. waitpid then returns -1.
     *
     * The child's spin does sys_yield so the scheduler visits the
     * parent between iterations. A raw `for(;;);` loop would starve
     * a single-CPU scheduler and the kill IPI never reaches the
     * child -- the watchdog would then fire and FAIL the whole
     * binary. Always yield in user-mode spin loops. */
    long pid3 = sys_fork();
    UTEST_ASSERT(pid3 >= 0,
                 "sys_fork before kill returns >= 0");

    if (pid3 == 0) {
        /* Child path -- cooperative spin. */
        for (;;) sys_yield();
    }

    UTEST_ASSERT(pid3 > 0,
                 "parent's fork-before-kill returns positive PID");
    /* Let the child reach its spin state before we kill it -- a
     * yield or two ensures the child actually started running and
     * is in its spin loop rather than still in fork's return path.
     * Not strictly required (kill works on TASK_READY too) but
     * makes the interleaving observable in serial. */
    sys_yield();
    sys_yield();

    long kill_rc = sys_kill((int)pid3);
    UTEST_ASSERT(kill_rc == 0,
                 "sys_kill(child) returns 0");

    long wait3 = sys_waitpid((int)pid3);
    UTEST_ASSERT(wait3 == -1,
                 "sys_waitpid(killed child) returns -1 (kill-forced exit_status)");

    UTEST_END();
    return g_fail;
}
