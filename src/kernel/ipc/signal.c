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

    /* Set the pending bit */
    t->signals.pending |= (1U << (uint32_t)sig);

    /* If the task is blocked, wake it so it can process the signal */
    if (t->state == TASK_BLOCKED || t->state == TASK_WAITING) {
        t->state = TASK_READY;
    }

    klog(LOG_DEBUG, "sig", "Signal %d sent to PID %u",
         (uint64_t)(uint32_t)sig, (uint64_t)pid);

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

    /* If this signal killed the task, run the remote-death teardown: drop its
     * syscall-filter count (memory frees at the reap barrier), mark the OB
     * process object dead (this path previously skipped that), and detach any
     * Job Object membership. Guarded on the DEAD transition so the non-fatal
     * branches (SIGCHLD) are unaffected; each call is idempotent. */
    if (t->state == TASK_DEAD) {
        syscall_filter_task_dead(t);
        ob_process_mark_dead(t->pid);
        ob_job_detach_task(t);
    }
}

void signal_check(void)
{
    struct task *t = task_current();
    uint32_t pending;
    int sig;

    pending = t->signals.pending;
    if (pending == 0)
        return;

    for (sig = 1; sig < (int)SIG_MAX; sig++) {
        if (!(pending & (1U << (uint32_t)sig)))
            continue;

        /* Clear the pending bit */
        t->signals.pending &= ~(1U << (uint32_t)sig);

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

void signal_ctrl_c(void)
{
    uint32_t num = task_count();
    uint32_t i;

    /* Send SIGINT to the last non-dead, non-idle task (the "foreground" task).
     * In a real OS this would be the foreground process group. */
    for (i = num; i > 0; i--) {
        struct task *t = task_get_by_pid(i - 1);
        if (t && t->state != TASK_DEAD && t->pid != 0) {
            signal_send(t->pid, SIGINT);
            return;
        }
    }
}
