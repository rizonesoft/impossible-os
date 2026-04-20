/* ============================================================================
 * test_faultinject.c -- user-mode fault-injection smoke test
 *   (user-mode fault-injection bridge section of the user-mode test
 *    framework TODO)
 *
 * Proves SYS_FAULT_INJECT round-trips end-to-end under boot.conf test=1
 * AND that the armed trap actually fires in the kernel allocator:
 *
 *   1. utest_fault_inject(FAULT_CLEAR_ALL, 0) disarms baseline state.
 *      On test=0 this returns -1; we then SKIP (exit 77) so the same
 *      binary can live in the manifest on either boot policy.
 *   2. sys_openfile("C:\\hello.txt") without any trap armed must
 *      succeed -- this is the "before" baseline.
 *   3. utest_fault_inject(FAULT_KMALLOC_NEXT, 0) arms the next kmalloc
 *      for THIS PID only (kernel-side task_current()->pid filter).
 *   4. sys_openfile on the SAME hello.txt path MUST now return -1
 *      because the handle-allocation kmalloc inside the kernel
 *      ob_create_file_handle path hits the forced NULL.
 *   5. Step 2 "before" passed + step 4 "after" returned -1 proves the
 *      trap fired in kmalloc (Codex L1, 2026-04-20: the earlier
 *      version used a nonexistent file and could not distinguish
 *      "kmalloc failed" from "file not found").
 *   6. FAULT_CLEAR_ALL restores baseline + a final sys_openfile
 *      succeeds again to prove the trap was actually disarmed.
 *
 * SKIP-on-test=0: step 1's CLEAR_ALL returns -1 under test=0 (the gate
 * blocks it). We exit with kselftest UTEST_EXIT_SKIP (77) in that
 * case; g_fail stays 0. Launcher reports SKIP, not FAIL.
 *
 * Linked against the same crt0 + libc as hello.exe / cmd.exe /
 * test_harness_smoke.exe.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

int main(void)
{
    HANDLE h_before, h_after, h_clear;

    UTEST_BEGIN("test_faultinject");

    /* SKIP probe: if test=0 the kernel gate returns -1 on any FAULT_*. */
    if (utest_fault_inject(FAULT_CLEAR_ALL, 0) != 0) {
        sys_write(STDOUT_FD,
                  "[UTEST-END] SKIP (test=0, SYS_FAULT_INJECT denied)\n", 51);
        return 77;  /* UTEST_EXIT_SKIP */
    }

    UTEST_ASSERT(utest_fault_inject(FAULT_CLEAR_ALL, 0) == 0,
                 "FAULT_CLEAR_ALL succeeds under test=1");

    /* Baseline: C:\hello.txt deploys with every sysroot build, so
     * sys_openfile without any trap armed MUST succeed. If this fails
     * the rest of the test is meaningless -- abort early. */
    h_before = sys_openfile("C:\\hello.txt", 0);
    UTEST_ASSERT(h_before != INVALID_HANDLE_VALUE,
                 "baseline: sys_openfile(C:\\hello.txt) without trap succeeds");
    if (h_before != INVALID_HANDLE_VALUE)
        sys_closehandle(h_before);

    /* Arm the NEXT kmalloc to fail (for THIS PID only). The task
     * filter is set kernel-side from task_current()->pid so a
     * concurrent kthread calling kmalloc on the same CPU cannot
     * steal the pending injection. */
    UTEST_ASSERT(utest_fault_inject(FAULT_KMALLOC_NEXT, 0) == 0,
                 "FAULT_KMALLOC_NEXT arms the countdown");

    /* The assertion that proves the bridge works: sys_openfile on the
     * SAME existing file must now return INVALID_HANDLE_VALUE because
     * the handle-allocation kmalloc inside the kernel open path hits
     * the forced NULL. Contrast with the `h_before` baseline above --
     * any other return value means the trap either did not arm (gate
     * broken) or did not fire on the expected allocation site. */
    h_after = sys_openfile("C:\\hello.txt", 0);
    UTEST_ASSERT(h_after == INVALID_HANDLE_VALUE,
                 "armed kmalloc trap: sys_openfile returns INVALID_HANDLE_VALUE");
    if (h_after != INVALID_HANDLE_VALUE)
        sys_closehandle(h_after);

    /* Disarm and prove the disarm worked -- same open must succeed. */
    UTEST_ASSERT(utest_fault_inject(FAULT_CLEAR_ALL, 0) == 0,
                 "FAULT_CLEAR_ALL disarms the trap");
    h_clear = sys_openfile("C:\\hello.txt", 0);
    UTEST_ASSERT(h_clear != INVALID_HANDLE_VALUE,
                 "after CLEAR_ALL: sys_openfile succeeds again");
    if (h_clear != INVALID_HANDLE_VALUE)
        sys_closehandle(h_clear);

    UTEST_END();
    return g_fail;
}
