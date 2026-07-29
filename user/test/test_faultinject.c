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

/* Sentinels for the fork+exec site probes below. Distinct from a normal
 * exit status so a child that dies some other way cannot be mistaken for a
 * clean refusal. */
#define FI_SENTINEL_SITE_REFUSED      71  /* exec refused, image intact  */
#define FI_SENTINEL_EXEC_UNEXPECTED_OK 72 /* exec succeeded and returned */
#define FI_SENTINEL_ARM_FAILED        73  /* injection unavailable       */
#define FI_SENTINEL_SITE_NOT_CONSUMED 74  /* exec refused, but not here  */
#define FI_SENTINEL_FORK_UNEXPECTED_OK 75 /* fork did not fail closed    */
#define FI_SENTINEL_FORK_CONTROL_OK   76  /* disarmed control fork ran   */

/* The kmalloc-backed exec sites, both reached by an exec carrying argv from
 * a forked child. FAULT_SITE_PEB_FRAMES is deliberately absent: it is a
 * pmm site that fails AFTER the exec commit point, so its refusal
 * terminates the child instead of returning, and asserting on it needs the
 * generated exec-destroyed exit status that section 23 is filed to add. */
static const unsigned int fi_site_probes[] = {
    FAULT_SITE_EXEC_ARGV_TABLE,
    FAULT_SITE_EXEC_PRIVATE_FRAMES,
};
#define FI_SITE_PROBE_COUNT \
    (sizeof(fi_site_probes) / sizeof(fi_site_probes[0]))

int main(void)
{
    HANDLE h_before, h_after, h_clear, h_unrelated, h_final;
    unsigned int probe;

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

    /* ---- Site-targeted + PMM-countdown selectors --------------------
     *
     * An ordinal countdown can only say "fail the N-th allocation", and N
     * drifts whenever the path gains or loses an allocation. These arm a
     * NAMED site instead. The assertions below deliberately include the
     * negative cases: an arm that names a site this path never reaches
     * must NOT fire, otherwise a mis-targeted test would quietly pass on
     * an unrelated allocation -- which is the failure mode the selector
     * exists to remove. */

    UTEST_ASSERT(utest_fault_inject(FAULT_PMM_COUNTDOWN, 1) == 0,
                 "FAULT_PMM_COUNTDOWN arms with N=1");
    UTEST_ASSERT(utest_fault_inject(FAULT_PMM_COUNTDOWN, 0) != 0,
                 "FAULT_PMM_COUNTDOWN rejects N=0");
    UTEST_ASSERT(utest_fault_inject(FAULT_CLEAR_ALL, 0) == 0,
                 "CLEAR_ALL disarms the PMM countdown");

    /* Every assigned site id must arm. The kernel and this binary agree on
     * the ids through the generated ABI contract (reached via the
     * abi_numbers.h facade), so a kernel-side
     * renumber breaks `make check-abi` rather than silently retargeting
     * these arms at the wrong allocation. */
    UTEST_ASSERT(utest_fault_inject(FAULT_KMALLOC_SITE,
                                    FAULT_SITE_EXEC_ARGV_TABLE) == 0,
                 "FAULT_KMALLOC_SITE arms the argv-table site");
    UTEST_ASSERT(utest_fault_inject(FAULT_KMALLOC_SITE,
                                    FAULT_SITE_EXEC_PRIVATE_FRAMES) == 0,
                 "FAULT_KMALLOC_SITE arms the private-frame-table site");
    UTEST_ASSERT(utest_fault_inject(FAULT_PMM_SITE,
                                    FAULT_SITE_PEB_FRAMES) == 0,
                 "FAULT_PMM_SITE arms the PEB-frame site");

    /* Negative case 1: every arm shape that could never FIRE must be
     * refused at arm time. This matters more than it looks: the consumed
     * check below reads a disarmed slot as "it fired", so an arm that
     * reports success while installing nothing claimable would let a
     * mistyped test certify a branch that never executed. */
    UTEST_ASSERT(utest_fault_inject(FAULT_KMALLOC_SITE,
                                    FAULT_SITE_MAX + 1) != 0,
                 "an out-of-range site id is refused at arm time");
    UTEST_ASSERT(utest_fault_inject(FAULT_KMALLOC_SITE,
                                    FAULT_SITE_NONE) != 0,
                 "arming FAULT_SITE_NONE is refused, not a silent disarm");
    UTEST_ASSERT(utest_fault_inject(FAULT_KMALLOC_SITE,
                                    FAULT_SITE_PEB_FRAMES) != 0,
                 "a kmalloc arm on the pmm-owned PEB site is refused");
    UTEST_ASSERT(utest_fault_inject(FAULT_PMM_SITE,
                                    FAULT_SITE_EXEC_ARGV_TABLE) != 0,
                 "a pmm arm on a kmalloc-owned exec site is refused");
    UTEST_ASSERT(utest_fault_inject(FAULT_SITE_QUERY, 0) != 0,
                 "FAULT_SITE_QUERY with tag 0 returns -1, not consumed");

    /* Negative case 2: with an exec-path site armed, an allocation on a
     * path that never enters that site must still succeed. sys_openfile
     * allocates a handle via kmalloc, but nowhere near task_exec's argv
     * table, so the armed injection must not touch it. */
    UTEST_ASSERT(utest_fault_inject(FAULT_KMALLOC_SITE,
                                    FAULT_SITE_EXEC_ARGV_TABLE) == 0,
                 "re-arm argv-table site for the negative probe");
    h_unrelated = sys_openfile("C:\\hello.txt", 0);
    UTEST_ASSERT(h_unrelated != INVALID_HANDLE_VALUE,
                 "site-armed injection does NOT fire on an unrelated path");
    if (h_unrelated != INVALID_HANDLE_VALUE)
        sys_closehandle(h_unrelated);

    /* Replacement semantics: arming an ordinal countdown after a site arm
     * must leave exactly one program live, so a fired site can never fall
     * through to a stale countdown and fail an unrelated branch. */
    UTEST_ASSERT(utest_fault_inject(FAULT_KMALLOC_COUNTDOWN, 1) == 0,
                 "ordinal arm replaces the live site arm");
    UTEST_ASSERT(utest_fault_inject(FAULT_CLEAR_ALL, 0) == 0,
                 "CLEAR_ALL disarms every selector");
    h_final = sys_openfile("C:\\hello.txt", 0);
    UTEST_ASSERT(h_final != INVALID_HANDLE_VALUE,
                 "baseline restored after all site/ordinal arming");
    if (h_final != INVALID_HANDLE_VALUE)
        sys_closehandle(h_final);

    /* ---- Positive end-to-end: a named site actually fires -----------
     *
     * Everything above proves the selectors ARM. That is not the same as
     * proving they FIRE at the branch they name: a mis-annotated site, or
     * an allocator gate that never consults the arm, would pass every
     * assertion above. These forks drive a real `task_exec` and require
     * the exec to be REFUSED with the child's own image still intact --
     * which can only happen if the annotation really does wrap the
     * allocation the site id names.
     *
     * Pre-commit refusals return to the caller, so the child survives to
     * report a sentinel; that survival is itself part of the assertion. */

    for (probe = 0; probe < FI_SITE_PROBE_COUNT; probe++) {
        long pid_probe = sys_fork();

        if (pid_probe == 0) {
            char *const probe_argv[] = { (char *)"hello.exe", (char *)0 };

            if (utest_fault_inject(FAULT_KMALLOC_SITE,
                                   fi_site_probes[probe]) != 0) {
                sys_exit(FI_SENTINEL_ARM_FAILED);
                for (;;) sys_yield();
            }
            /* The arm must be live before we drive the path, or the
             * refusal below would prove nothing. */
            if (utest_fault_inject(FAULT_SITE_QUERY,
                                   FAULT_ALLOC_KMALLOC) == 0) {
                sys_exit(FI_SENTINEL_ARM_FAILED);
                for (;;) sys_yield();
            }
            if (sys_exec("hello.exe", probe_argv, (char *const *)0) >= 0) {
                /* exec reported success and RETURNED -- either it did not
                 * fail where the site said it would, or it failed after
                 * the commit point and wrongly came back. */
                utest_fault_inject(FAULT_CLEAR_ALL, 0);
                sys_exit(FI_SENTINEL_EXEC_UNEXPECTED_OK);
                for (;;) sys_yield();
            }
            /* Refused -- but a refusal alone is NOT the assertion. A
             * missing image or an unrelated earlier OOM also refuses,
             * with the arm still sitting there unconsumed. Requiring the
             * arm to now read 0 is what pins the failure to the site we
             * named: only fault_site_claim() clears it. */
            if (utest_fault_inject(FAULT_SITE_QUERY,
                                   FAULT_ALLOC_KMALLOC) != 0) {
                utest_fault_inject(FAULT_CLEAR_ALL, 0);
                sys_exit(FI_SENTINEL_SITE_NOT_CONSUMED);
                for (;;) sys_yield();
            }
            utest_fault_inject(FAULT_CLEAR_ALL, 0);
            sys_exit(FI_SENTINEL_SITE_REFUSED);
            for (;;) sys_yield();
        }

        if (pid_probe > 0) {
            long st = sys_waitpid((int)pid_probe);
            UTEST_ASSERT(st == FI_SENTINEL_SITE_REFUSED,
                         "named exec site fires: arm consumed AT that site");
        } else {
            UTEST_ASSERT(pid_probe > 0, "fork for the site probe succeeded");
        }
    }

    UTEST_ASSERT(utest_fault_inject(FAULT_CLEAR_ALL, 0) == 0,
                 "CLEAR_ALL after the site probes");

    /* ---- fork's own PML4 allocation: named, and fork fails CLOSED ----
     *
     * This is the one pre-commit refusal a ring-3 test could not previously
     * drive. The allocation lives inside task_fork, so an ordinal countdown
     * cannot address it without landing on whichever of fork's allocations
     * happens to be N-th; naming it is what makes the probe deterministic.
     * The arming task is the PARENT -- task_fork runs entirely on the
     * parent's thread, so the self-PID scoping every selector already
     * applies reaches this allocation without any descendant-inheritance
     * machinery. */
    {
        long pid_fork;
        HANDLE h_between;

        UTEST_ASSERT(utest_fault_inject(FAULT_PMM_SITE,
                                        FAULT_SITE_FORK_CHILD_PML4) == 0,
                     "FAULT_PMM_SITE arms the fork-child PML4 site");

        /* NEGATIVE: the parent keeps allocating between the arm and the
         * fork, and none of that traffic may consume the arm. Without this
         * the probe would prove only that SOMETHING failed during fork --
         * which an ordinal countdown would also produce. */
        h_between = sys_openfile("C:\\hello.txt", 0);
        UTEST_ASSERT(h_between != INVALID_HANDLE_VALUE,
                     "an armed fork site does not break unrelated allocations");
        if (h_between != INVALID_HANDLE_VALUE)
            sys_closehandle(h_between);
        UTEST_ASSERT(utest_fault_inject(FAULT_SITE_QUERY,
                                        FAULT_ALLOC_PMM) != 0,
                     "a kmalloc-backed syscall leaves the pmm fork arm live");

        pid_fork = sys_fork();
        if (pid_fork == 0) {
            /* Reached only if fork did NOT fail closed. Exit at once: a
             * surviving child would re-run the remainder of this binary and
             * emit a second [UTEST-END] into the launcher's stream. */
            sys_exit(FI_SENTINEL_FORK_UNEXPECTED_OK);
            for (;;) sys_yield();
        }
        UTEST_ASSERT(pid_fork < 0,
                     "fork fails closed when the child PML4 cannot allocate");
        if (pid_fork > 0)
            sys_waitpid((int)pid_fork);

        /* The refusal alone still proves nothing -- a task-table-full fork
         * refuses identically with the arm untouched. Requiring the arm to
         * now read CONSUMED pins the failure to the named allocation. */
        UTEST_ASSERT(utest_fault_inject(FAULT_SITE_QUERY,
                                        FAULT_ALLOC_PMM) == 0,
                     "the fork-site arm was CONSUMED at the named allocation");

        /* CLEAR_ALL must disarm this site too -- and proving that requires a
         * LIVE arm to clear. Asserting it against the arm consumed above
         * would be vacuous: that query is already required to read 0, so the
         * assertion would hold even if CLEAR_ALL stopped clearing pmm sites
         * entirely. Re-arm, confirm live, clear, then confirm gone. */
        UTEST_ASSERT(utest_fault_inject(FAULT_PMM_SITE,
                                        FAULT_SITE_FORK_CHILD_PML4) == 0,
                     "fork site re-arms after being consumed");
        UTEST_ASSERT(utest_fault_inject(FAULT_SITE_QUERY,
                                        FAULT_ALLOC_PMM) != 0,
                     "the re-armed fork site is live before CLEAR_ALL");
        UTEST_ASSERT(utest_fault_inject(FAULT_CLEAR_ALL, 0) == 0,
                     "CLEAR_ALL succeeds with a live fork-site arm");
        UTEST_ASSERT(utest_fault_inject(FAULT_SITE_QUERY,
                                        FAULT_ALLOC_PMM) == 0,
                     "CLEAR_ALL disarmed the live fork-site arm");
        pid_fork = sys_fork();
        if (pid_fork == 0) {
            sys_exit(FI_SENTINEL_FORK_CONTROL_OK);
            for (;;) sys_yield();
        }
        UTEST_ASSERT(pid_fork > 0,
                     "fork succeeds again once the fork site is disarmed");
        if (pid_fork > 0)
            UTEST_ASSERT(sys_waitpid((int)pid_fork)
                             == FI_SENTINEL_FORK_CONTROL_OK,
                         "the control child ran and exited normally");
    }

    UTEST_ASSERT(utest_fault_inject(FAULT_CLEAR_ALL, 0) == 0,
                 "CLEAR_ALL after the fork-site probe");

    UTEST_END();
    return g_fail;
}
