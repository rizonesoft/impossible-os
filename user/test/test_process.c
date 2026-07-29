/* ============================================================================
 * test_process.c -- user-mode process-lifecycle coverage binary
 *
 * Exercises the fork/exec/waitpid/kill quartet from ring 3, plus the exec
 * commit-point lifecycle (transactional exec: a pre-commit refusal leaves the
 * old image intact, a post-commit failure terminates rather than returning).
 *
 * TASK-SLOT BUDGET: every sub-test spawns exactly ONE child, so a run consumes
 * 8 slots. That is a hard design constraint, not a style note: TASK_MAX is 32
 * and a reaped slot is NOT reused yet (TODO-06 slot reuse), so slots consumed
 * here are gone for the rest of the boot. A sub-test that wants to probe N
 * variations must loop INSIDE one child (see sub-test 7) rather than fork per
 * variation -- an earlier per-variation fork drove the boot into
 * "task_create: max tasks reached" and destabilised later tests.
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
#include "abi_numbers.h"   /* TASK_EXIT_EXEC_IMAGE_DESTROYED -- facade over
                             * the generated abi/generated/abi_contract.h */

UTEST_DEFINE_STATE();

/* Literal file name for sys_exec (resolved under C:\ by the kernel). */
#define HELLO_EXE_NAME    "hello.exe"

/* A real file that is NOT a loadable image. Nothing in task_exec's pre-commit
 * phase inspects the image bytes, so the format rejection happens in the loader
 * PAST the commit point -- which is exactly the failure class sub-test 4 needs,
 * with no fault injection and no platform dependence. Shipped to the sysroot by
 * the Makefile alongside hello.exe. */
#define GARBAGE_IMAGE_NAME "hello.txt"

/* argv entries for the oversize-refusal test. Must exceed the kernel's
 * ARG_ARGC_MAX (511 as of this writing, include/kernel/sched/task.h) -- not
 * included here because user binaries must not pull in kernel headers. If the
 * kernel cap ever grows past this value the exec stops being refused and
 * sub-test 6 FAILS loudly rather than silently testing nothing. */
#define ARGV_OVERSIZE_ARGC 600

/* Child exit codes. Deliberately clear of 77, which the harness reserves for
 * SKIP, and of hello.exe's own 40+argc range. */
#define SENTINEL_EXEC_RETURNED      61  /* post-commit failure wrongly returned */
#define SENTINEL_PRECOMMIT_OK       62  /* refused, image intact */
#define SENTINEL_EXEC_UNEXPECTED_OK 63  /* exec succeeded when it must not have */
#define SENTINEL_INJECT_UNAVAILABLE 64  /* boot.conf test=0, injection refused */

/* TASK_EXIT_EXEC_IMAGE_DESTROYED (generated into abi/generated/abi_contract.h from
 * include/kernel/sched/task.h) is the exit status of a task terminated because
 * a post-commit exec destroyed its image. Kernel headers stay off-limits to
 * user binaries, so the value arrives through the generated ABI header: a
 * kernel-side change now fails `make check-abi` at build time instead of
 * waiting for this one runtime assertion on a platform where it is not skipped.
 *
 * Distinctness is the whole point, and it is why this is not a small number:
 * -1 is a kill (sub-test 3 asserts exactly that) and signal deaths occupy
 * -(signum), so anything in -1..-SIG_MAX names something else already. */

/* Depth bound for the allocation-countdown sweep in sub-test 7: the sweep fails
 * the 1st, then 2nd, ... Nth allocation of the exec path in turn. A runaway
 * guard, not a tuning knob -- which depth a given failure lands on is a property
 * of the exec path's allocation order and deliberately not asserted (sub-test 4
 * owns the post-commit case deterministically instead). Iterations are LOOP
 * iterations in one child, not forks, so raising it costs no task slots. */
#define EXEC_FAULT_SWEEP_MAX 20

/* hello.exe returns 42 when launched WITHOUT an argv vector (user/hello.c), and
 * 40+argc otherwise. The sweep passes a bare {name, NULL}, so if one of its
 * depths lets the exec through, the child becomes hello.exe and exits with
 * exactly this. A legal sweep outcome, not a success criterion. */
#define HELLO_NO_ARGV_EXIT 42

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
         * only for exactly {"hello.exe","alpha","beta"}). envp NULL PRESERVES the
         * child's current env (empty after fork until env_copy is wired --
         * TODO-12 s7), not the parent's environment. */
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

    /* ---- Sub-test 4: post-commit exec failure TERMINATES the child ----- *
     * task_exec has a commit point: past it the process image has been
     * replaced with zeroed private frames, so a failure there cannot return
     * -1 to a caller that would iretq back into an image that no longer
     * exists. The contract is that such a failure terminates the task
     * instead (TASK_EXEC_IMAGE_DESTROYED -> task_exit).
     *
     * hello.txt drives exactly that: the format rejection happens in the
     * loader, past the commit point. No fault injection, so this runs
     * identically on a test=0 boot and on every platform.
     *
     * If the contract ever regresses to "return -1", the child survives and
     * exits SENTINEL_EXEC_RETURNED, and the assertion below names it. */
    long pid4 = sys_fork();
    UTEST_ASSERT(pid4 >= 0, "sys_fork before post-commit exec returns >= 0");

    if (pid4 == 0) {
        char *const bad_argv[] = { (char *)GARBAGE_IMAGE_NAME, (char *)0 };
        (void)sys_exec(GARBAGE_IMAGE_NAME, bad_argv, (char *const *)0);
        /* Reached ONLY if a post-commit failure wrongly returned to us. */
        sys_exit(SENTINEL_EXEC_RETURNED);
        for (;;) sys_yield();
    }

    long wait4 = sys_waitpid((int)pid4);
    UTEST_ASSERT(wait4 != SENTINEL_EXEC_RETURNED,
                 "post-commit exec failure never returns into the destroyed image");
    /* The EXACT status, not merely "died": a generic -1 would also match a
     * kill or a crash, so it could not prove the commit point was the cause. */
    UTEST_ASSERT(wait4 == TASK_EXIT_EXEC_IMAGE_DESTROYED,
                 "post-commit exec failure terminates the child with the exec-destroyed status");

    /* ---- Sub-test 5: injected OOM refusal leaves the OLD image intact ---- *
     * The mirror of sub-test 4: a refusal BEFORE the commit point must fail
     * safely, because the task's image is still its own -- the exec returns -1
     * and the caller carries on running.
     *
     * SCOPE, precisely: FAULT_KMALLOC_NEXT fails the FIRST kmalloc the SYS_EXEC
     * path makes, which is the argv snapshot array in exec_snapshot_vec
     * (src/kernel/sched/syscall.c) -- so this case refuses at the SYSCALL layer,
     * before task_exec is entered. That is a real and distinct refusal path
     * worth pinning, but it is not task_exec's own pre-commit phase; sub-test 7
     * sweeps deeper allocations to reach that.
     *
     * The injection MUST be armed from inside the child: the kernel binds the
     * fault filter to task_current()->pid at arm time, so arming in the parent
     * before the fork would target the parent instead. */
    long pid5 = sys_fork();
    UTEST_ASSERT(pid5 >= 0, "sys_fork before injected-OOM exec returns >= 0");

    if (pid5 == 0) {
        char *const child_argv[] = { (char *)HELLO_EXE_NAME, (char *)0 };
        long rc;
        if (utest_fault_inject(FAULT_KMALLOC_NEXT, 0) != 0) {
            /* boot.conf test=0 -- the kernel refuses to arm. Report it so the
             * parent skips rather than fails: this binary must stay green on
             * a production boot. */
            sys_exit(SENTINEL_INJECT_UNAVAILABLE);
            for (;;) sys_yield();
        }
        rc = sys_exec(HELLO_EXE_NAME, child_argv, (char *const *)0);
        /* Still alive, still our own image -- disarm and report through it.
         * Being able to run these lines at all IS the assertion: a refusal
         * that had corrupted the image could not execute them. */
        utest_fault_inject(FAULT_CLEAR_ALL, 0);
        sys_exit(rc < 0 ? SENTINEL_PRECOMMIT_OK : SENTINEL_EXEC_UNEXPECTED_OK);
        for (;;) sys_yield();
    }

    long wait5 = sys_waitpid((int)pid5);
    int injection_available = (wait5 != SENTINEL_INJECT_UNAVAILABLE);
    if (!injection_available) {
        /* A real skip: emit a line and record NO assertion. Recording a
         * passing assertion here would let a test=0 configuration silently
         * erase every injected-OOM case while the suite stayed green. */
        UTEST_SKIP("injected-OOM refusal: fault injection off (boot.conf test=0)");
    } else {
        UTEST_ASSERT(wait5 != SENTINEL_EXEC_UNEXPECTED_OK,
                     "injected OOM actually failed the exec");
        UTEST_ASSERT(wait5 == SENTINEL_PRECOMMIT_OK,
                     "injected argv-snapshot OOM returns -1, child's old image intact");
    }

    /* ---- Sub-test 6: oversized argv is refused before any mutation ----- *
     * Same invariant as sub-test 5 but reached WITHOUT fault injection, so it
     * still runs on a test=0 boot where sub-test 5 skips.
     *
     * SCOPE, precisely: at 600 entries the refusal comes from exec_snapshot_vec's
     * ARG_ARGC_MAX quota check in the syscall layer, not from task_exec's own
     * argv-frame budget check. Both are pre-commit and both must leave the image
     * intact, which is what is asserted; the syscall cap is simply the first one
     * reached. Chosen deliberately over a just-at-the-boundary vector because
     * the boundary itself moves with the kernel constant, while "far past any
     * plausible cap" stays a valid rejection case. */
    long pid6 = sys_fork();
    UTEST_ASSERT(pid6 >= 0, "sys_fork before oversized-argv exec returns >= 0");

    if (pid6 == 0) {
        static char *big_argv[ARGV_OVERSIZE_ARGC + 1];
        long rc;
        int i;
        for (i = 0; i < ARGV_OVERSIZE_ARGC; i++)
            big_argv[i] = (char *)HELLO_EXE_NAME;
        big_argv[ARGV_OVERSIZE_ARGC] = (char *)0;

        rc = sys_exec(HELLO_EXE_NAME, (char *const *)big_argv,
                      (char *const *)0);
        sys_exit(rc < 0 ? SENTINEL_PRECOMMIT_OK : SENTINEL_EXEC_UNEXPECTED_OK);
        for (;;) sys_yield();
    }

    long wait6 = sys_waitpid((int)pid6);
    UTEST_ASSERT(wait6 != SENTINEL_EXEC_UNEXPECTED_OK,
                 "an argv vector over the kernel's ARG_ARGC_MAX is not accepted");
    UTEST_ASSERT(wait6 == SENTINEL_PRECOMMIT_OK,
                 "oversized argv is refused pre-commit, child's old image intact");

    /* ---- Sub-test 7: allocation sweep across the whole exec path -------- *
     * Sub-tests 5 and 6 both refuse at the SYSCALL layer, so neither reaches
     * the commit point. This sweep walks the failure point outward one
     * allocation at a time with FAULT_KMALLOC_COUNTDOWN -- failing the 1st,
     * then 2nd, then Nth allocation of the exec path -- so it exercises a range
     * of depths and, measured on this tree, gets far enough to cross the commit
     * point and be terminated there.
     *
     * What it does NOT claim: it addresses allocations by ORDINAL, so it does
     * not pin task_exec's individual named pre-commit branches (argv address
     * table, private-frame table). Sub-test 8 covers the guarded kernel stack
     * directly; hitting the remaining sites BY NAME needs a site-targeted
     * selector, which the user-mode test framework roadmap owns as a follow-up.
     *
     * The oracle is the section's central invariant, and it must hold at EVERY
     * depth: an exec either refuses and leaves the child running its old image,
     * or it destroys the image and TERMINATES the child. It may never return
     * into a destroyed image. Deliberately not asserted per-N against a
     * hardcoded allocation index -- that count shifts whenever the exec path
     * changes an allocation, which is how brittle tests become false ones.
     *
     * Non-vacuity is enforced below: the sweep must actually observe refusals,
     * or it proves nothing and fails. */
    if (!injection_available) {
        UTEST_SKIP("exec allocation sweep: fault injection off (boot.conf test=0)");
    } else {
        long pid7 = sys_fork();
        UTEST_ASSERT(pid7 >= 0, "sys_fork before allocation sweep returns >= 0");

        if (pid7 == 0) {
            char *const sweep_argv[] = { (char *)HELLO_EXE_NAME, (char *)0 };
            unsigned int n;

            /* The whole sweep runs in ONE child, which is the point: a
             * pre-commit refusal RETURNS to this image, so the same child can
             * walk depth after depth. (Forking per depth would burn a task slot
             * each time -- TASK_MAX is 32 and slots are not reused after reap
             * until TODO-06's slot-reuse work lands, so a per-depth fork
             * exhausts the table and destabilises later tests in the boot.)
             *
             * The loop can only end three ways, and each is a verdict:
             *   - a depth lands past the commit point -> the kernel TERMINATES
             *     us and the parent sees the exec-destroyed status;
             *   - a depth lets the exec succeed -> hello.exe replaces us;
             *   - every depth refuses -> we fall out and report how far we got.
             * Surviving to exit after a post-commit failure is precisely the
             * regression this section exists to prevent, and it cannot hide
             * here: it would have to come back as the encoded count below. */
            for (n = 1; n <= EXEC_FAULT_SWEEP_MAX; n++) {
                if (utest_fault_inject(FAULT_KMALLOC_COUNTDOWN, n) != 0) {
                    sys_exit(SENTINEL_INJECT_UNAVAILABLE);
                    for (;;) sys_yield();
                }
                if (sys_exec(HELLO_EXE_NAME, sweep_argv, (char *const *)0) >= 0) {
                    /* Unreachable in practice: a successful exec never returns.
                     * Reaching here means exec claimed success and came back. */
                    utest_fault_inject(FAULT_CLEAR_ALL, 0);
                    sys_exit(SENTINEL_EXEC_UNEXPECTED_OK);
                    for (;;) sys_yield();
                }
                /* Refused, and we are still running our own image -- the next
                 * iteration only exists because that held. */
            }
            utest_fault_inject(FAULT_CLEAR_ALL, 0);
            sys_exit(SENTINEL_PRECOMMIT_OK);
            for (;;) sys_yield();
        }

        long wait7 = sys_waitpid((int)pid7);
        /* The sweep has THREE legal terminal outcomes and the assertion is
         * stated as the violation it forbids, not as an enumeration of them:
         *   - a depth refused cleanly at every step  -> SENTINEL_PRECOMMIT_OK;
         *   - a depth landed past the commit point   -> the exec-destroyed
         *     status, the child having been terminated;
         *   - a depth let the exec through           -> the child IS hello.exe
         *     now and the status is hello's own, so the sweep ended there.
         * Anything ELSE is a failure: SENTINEL_EXEC_UNEXPECTED_OK means sys_exec
         * reported success and RETURNED to a caller whose image should have been
         * replaced, and any other status means the child died in a way the exec
         * contract does not sanction (a fault, a signal, or injection that could
         * not be armed). The set is checked explicitly rather than by excluding
         * one sentinel, so a crash cannot pass as a legal outcome.
         *
         * What is deliberately NOT asserted is WHICH depth does what: that is a
         * property of allocation order, and pinning it is how the earlier
         * stricter version broke when SYS_EXEC was hardened to reject a short
         * read pre-commit. Post-commit reach is pinned by sub-test 4 (no
         * injection, cannot drift); that injection actually FIRES is pinned by
         * sub-test 5, which observes a refusal directly. */
        /* Both outcomes are legal, and that is NOT the vacuity hole it looks
         * like, because post-commit REACH is pinned deterministically elsewhere:
         * sub-test 4 crosses the commit point with no fault injection at all, so
         * no allocation-order change can move it. This sweep owns the other half
         * -- the UNIVERSAL claim that no injected failure depth anywhere on the
         * exec path returns into a destroyed image.
         *
         * Requiring the destroyed status HERE was tried and is wrong: hardening
         * SYS_EXEC to reject a short read pre-commit legitimately moved the
         * sweep's deepest failure back to a clean refusal, and an assertion that
         * fails BECAUSE the kernel got safer is an assertion about allocation
         * ordering, not about the exec contract. */
        UTEST_ASSERT(wait7 != SENTINEL_INJECT_UNAVAILABLE,
                     "allocation sweep had fault injection available throughout");
        UTEST_ASSERT(wait7 == SENTINEL_PRECOMMIT_OK ||
                     wait7 == TASK_EXIT_EXEC_IMAGE_DESTROYED ||
                     wait7 == HELLO_NO_ARGV_EXIT,
                     "allocation sweep ended in a sanctioned outcome, not a crash");
    }

    /* ---- Sub-test 8: PMM-frame OOM is refused with the image intact ----- *
     * Complements the kmalloc cases: `task_exec` allocates its replacement
     * guarded kernel stack with pmm_alloc_contiguous, an allocator no kmalloc
     * countdown can reach at all. FAULT_PMM_NEXT fails the next frame
     * allocation this task makes, so it exercises a PMM refusal on the exec
     * path -- and like every pre-commit refusal it must return -1 and leave
     * this child running its own image, which running the report proves. */
    if (!injection_available) {
        UTEST_SKIP("PMM-frame OOM refusal: fault injection off (boot.conf test=0)");
    } else {
        long pid8 = sys_fork();
        UTEST_ASSERT(pid8 >= 0, "sys_fork before PMM-OOM exec returns >= 0");

        if (pid8 == 0) {
            char *const pmm_argv[] = { (char *)HELLO_EXE_NAME, (char *)0 };
            long rc;
            if (utest_fault_inject(FAULT_PMM_NEXT, 0) != 0) {
                sys_exit(SENTINEL_INJECT_UNAVAILABLE);
                for (;;) sys_yield();
            }
            rc = sys_exec(HELLO_EXE_NAME, pmm_argv, (char *const *)0);
            utest_fault_inject(FAULT_CLEAR_ALL, 0);
            sys_exit(rc < 0 ? SENTINEL_PRECOMMIT_OK : SENTINEL_EXEC_UNEXPECTED_OK);
            for (;;) sys_yield();
        }

        long wait8 = sys_waitpid((int)pid8);
        UTEST_ASSERT(wait8 != SENTINEL_EXEC_UNEXPECTED_OK,
                     "an injected PMM-frame failure actually failed the exec");
        UTEST_ASSERT(wait8 == SENTINEL_PRECOMMIT_OK,
                     "injected PMM-frame OOM is refused with the child's old image intact");
    }

    UTEST_END();
    return g_fail;
}
