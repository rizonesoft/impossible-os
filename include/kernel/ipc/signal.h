/* ============================================================================
 * signal.h — Kernel signal delivery
 *
 * POSIX-inspired process signals for Impossible OS. Signals can be sent
 * between processes or from the kernel (e.g., Ctrl+C → SIGINT).
 *
 * Each task has a per-signal handler table. Default handlers:
 *   - SIGKILL: always terminates (cannot be caught or ignored)
 *   - SIGTERM: clean termination (default: exit)
 *   - SIGINT:  interrupt (default: exit, Ctrl+C)
 *   - SIGCHLD: child exited (default: ignore)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Signal numbers (POSIX-compatible values) */
#define SIGINT   2    /* interrupt — Ctrl+C */
#define SIGKILL  9    /* kill — cannot be caught */
#define SIGTERM  15   /* terminate — clean exit */
#define SIGCHLD  17   /* child process exited */

/* Maximum signal number */
#define SIG_MAX  32

/* Special handler values */
#define SIG_DFL  ((signal_handler_t)0)   /* default handler */
#define SIG_IGN  ((signal_handler_t)1)   /* ignore signal */

/* Signal handler function type.
 * For kernel-mode signals, this is called directly.
 * The signal number is passed as the argument. */
typedef void (*signal_handler_t)(int sig);

/* Per-task signal state (embedded in struct task) */
struct signal_state {
    signal_handler_t handlers[SIG_MAX]; /* per-signal handlers (SIG_DFL=default) */
    volatile uint32_t pending;          /* bitmask of pending signals */
};

/* --- API --- */

/* Initialize signal state for a new task. */
void signal_init_task(struct signal_state *ss);

/* Send a signal to a task by PID.
 * Returns 0 on success, -1 if invalid PID. */
int signal_send(uint32_t pid, int sig);

/* Register a signal handler for the current task.
 * Returns the previous handler, or SIG_DFL on error. */
signal_handler_t signal_handler(int sig, signal_handler_t handler);

/* Check and dispatch pending signals for the current task.
 * Called from the scheduler or yield path. */
void signal_check(void);

/* Called from keyboard driver when Ctrl+C is pressed.
 * Sends SIGINT to the foreground task. */
void signal_ctrl_c(void);
