/* ============================================================================
 * signal.c -- Kernel signal delivery
 *
 * Signal dispatch:
 *   1. signal_send() sets a bit in the target task's pending mask
 *   2. signal_check() is called from scheduler/yield, dispatches pending signals
 *   3. SIGKILL always terminates, ignoring any handler
 *   4. SIGCHLD default is to ignore
 *   5. All others: run handler if set, otherwise default action (terminate)
 * ============================================================================ */

#include "kernel/ipc/signal.h"
#include "kernel/ipc/pgroup.h"          /* pgroup_foreground_snapshot (Ctrl+C fan-out) */
#include "kernel/sched/task.h"
#include "kernel/nt/syscall_filter.h"   /* syscall_filter_task_dead on signal kill */
#include "kernel/ob/ob_process.h"       /* ob_process_mark_dead on signal kill */
#include "kernel/ob/ob_job.h"           /* ob_job_detach_task on signal kill */
#include "kernel/klog.h"
#include "kernel/printk.h"

void signal_init_task(struct signal_state *ss)
{
    uint32_t i;
    for (i = 0; i < SIG_MAX; i++)
        ss->handlers[i] = SIG_DFL;
    ss->pending = 0;
}

int signal_send(uint32_t pid, int sig)
{
    struct task *t;

    if (sig < 1 || sig >= (int)SIG_MAX)
        return -1;

    t = task_get_by_pid(pid);
    if (!t || t->state == TASK_DEAD)
        return -1;

    /* Set the pending bit with an atomic OR: a plain |= would lose a concurrent
     * signal_send on another CPU (read-modify-write torn against signal_check's
     * drain). ACQ_REL so the wake below is ordered after the bit is visible. */
    __atomic_fetch_or(&t->signals.pending, (1U << (uint32_t)sig), __ATOMIC_ACQ_REL);

    /* If the task is blocked, wake it so it can process the signal. Ordered after
     * the pending publish so a target that observes TASK_READY also observes the
     * bit (no wake-without-signal window on SMP). */
    if (t->state == TASK_BLOCKED || t->state == TASK_WAITING) {
        t->state = TASK_READY;
    }

    /* No logging here: signal_send is ISR-reachable (Ctrl+C -> signal_send_group)
     * and must stay a pure, nonblocking primitive -- klog can flush the disk log,
     * which must never run in interrupt context. Delivery is still observable via
     * signal_check's default-action logs (e.g. "PID N interrupted (SIGINT)"). */
    return 0;
}

signal_handler_t signal_handler(int sig, signal_handler_t handler)
{
    struct task *t = task_current();
    signal_handler_t old;

    if (sig < 1 || sig >= (int)SIG_MAX)
        return SIG_DFL;

    /* SIGKILL cannot be caught or ignored */
    if (sig == SIGKILL)
        return SIG_DFL;

    old = t->signals.handlers[sig];
    t->signals.handlers[sig] = handler;
    return old;
}

/* Default signal actions */
static void signal_default_action(struct task *t, int sig)
{
#ifdef KERNEL_TESTS
    /* Every fatal action below publishes TASK_DEAD directly instead of
     * routing through task_terminate_remote, so this is their shared death
     * transition and it owes the capture snapshot. It matters more here than
     * anywhere else: the launcher's descendant reap kills cooperatively with
     * SIGKILL, so this is the ordinary way a captured task dies. SIGCHLD is
     * the one default that is not fatal and must not take the snapshot. */
    if (sig != SIGCHLD)
        task_utest_cap_note_task_death(t);
#endif
    switch (sig) {
    case SIGKILL:
        printk("[SIG] PID %u killed (SIGKILL)\n", (uint64_t)t->pid);
        t->state = TASK_DEAD;
        t->exit_status = -9;
        break;

    case SIGTERM:
        printk("[SIG] PID %u terminated (SIGTERM)\n", (uint64_t)t->pid);
        t->state = TASK_DEAD;
        t->exit_status = -15;
        break;

    case SIGINT:
        printk("[SIG] PID %u interrupted (SIGINT)\n", (uint64_t)t->pid);
        t->state = TASK_DEAD;
        t->exit_status = -2;
        break;

    case SIGCHLD:
        /* Default: ignore */
        break;

    default:
        /* Unknown signal -- terminate */
        printk("[SIG] PID %u: unhandled signal %d\n",
               (uint64_t)t->pid, (uint64_t)(uint32_t)sig);
        t->state = TASK_DEAD;
        t->exit_status = -(int32_t)sig;
        break;
    }

    /* If this signal killed the task, run the shared DEAD-transition teardown:
     * syscall-filter count (memory frees at the reap barrier), OB process object,
     * Job Object membership, and any leaked timer-resolution request. Guarded on
     * the DEAD transition so the non-fatal branches (SIGCHLD) are unaffected;
     * each sub-call is idempotent. */
    if (t->state == TASK_DEAD)
        task_death_teardown(t);
}

void signal_check(void)
{
    struct task *t = task_current();
    uint32_t pending;
    int sig;

    /* Atomically drain the whole pending set to zero and process the snapshot. A
     * plain read + per-bit clear can erase a signal posted concurrently on
     * another CPU (the clear's read-modify-write races signal_send's OR); the
     * exchange takes an all-or-nothing snapshot. A signal arriving AFTER the
     * exchange stays in the field and is handled on the next signal_check. */
    pending = __atomic_exchange_n(&t->signals.pending, 0u, __ATOMIC_ACQ_REL);
    if (pending == 0)
        return;

    for (sig = 1; sig < (int)SIG_MAX; sig++) {
        if (!(pending & (1U << (uint32_t)sig)))
            continue;

        /* SIGKILL is always forced -- no handler */
        if (sig == SIGKILL) {
            signal_default_action(t, sig);
            continue;
        }

        /* Check for user handler */
        {
            signal_handler_t h = t->signals.handlers[sig];

            if (h == SIG_IGN) {
                /* Ignored */
                continue;
            }

            if (h == SIG_DFL) {
                signal_default_action(t, sig);
                continue;
            }

            /* Call custom handler */
            h(sig);
        }
    }
}

void signal_send_group(uint32_t pgid, int sig)
{
    uint32_t num = task_count();
    uint32_t i;

    /* i starts at 1: PID 0 (the kernel session) is never a job-control target,
     * so a "group 0" fan-out can never terminate the system task. Bounded scan;
     * pgid is read racily against a concurrent setpgid by design (same as the
     * single-signal path). */
    for (i = 1; i < num; i++) {
        struct task *t = task_get_by_pid(i);
        if (t && t->state != TASK_DEAD && t->pgid == pgid)
            signal_send(t->pid, sig);
    }
}

void signal_ctrl_c(void)
{
    uint32_t fg_pgid;

    /* Fan SIGINT out to the console's foreground process group. NO-OP until a
     * shell claims the console (pgroup_foreground_snapshot returns 0): with no
     * foreground group, there is deliberately nothing to interrupt -- delivering
     * to "group 0" would otherwise reach PID 0 / the kernel session. Runs in the
     * keyboard ISR; signal_send_group is ISR-safe (atomic pending bit only). */
    if (!pgroup_foreground_snapshot(&fg_pgid))
        return;
    signal_send_group(fg_pgid, SIGINT);
}
