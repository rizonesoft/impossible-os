/* ============================================================================
 * pgroup.h -- POSIX process groups, sessions, and console job control
 *
 * Owns the per-process pgid/sid mutation policy (the fields themselves live in
 * struct task) plus a single controlling-console job-control object: which
 * session controls the one console today, and which process group is in the
 * foreground (receives Ctrl+C). One IRQ-safe job-control lock serializes every
 * mutation here -- pgid/sid validate-and-commit and the console foreground/owner
 * fields -- because the Ctrl+C fan-out reads the foreground group from the
 * keyboard ISR. Getters are lock-free single-field reads (aligned, no tearing).
 *
 * Scope: this module owns the kernel primitives + the foreground-group API. The
 * Linux setpgid/setsid syscall-number adapters are owned by the kernel-security
 * TODO; a terminal/shell consuming NtSetForegroundProcessGroup (tcsetpgrp) to
 * claim Ctrl+C is owned by the terminal TODO. Full orphaned-process-group signal
 * delivery (SIGHUP + SIGCONT to a newly-orphaned stopped group) is deferred with
 * the job-control stop/continue signals it needs; only the orphan DETECTOR ships
 * here.
 * ============================================================================ */

#ifndef KERNEL_IPC_PGROUP_H
#define KERNEL_IPC_PGROUP_H

#include "kernel/types.h"

struct task;

/* Win32 console control events (GenerateConsoleCtrlEvent dwCtrlEvent). Both map
 * to SIGINT on the target group in this kernel's signal model. */
#define CTRL_C_EVENT      0u
#define CTRL_BREAK_EVENT  1u

/* --- Create/exec-time hooks (called from the task lifecycle) --------------- */

/* Mark a task as having exec'd, under the job-control lock so the has_execed
 * transition is mutually exclusive with a concurrent pgroup_setpgid validation
 * (an acquire load alone orders but does not EXCLUDE). Called from task_exec. */
void pgroup_note_exec(struct task *t);

/* Acquire/release the job-control lock so the task lifecycle can commit a child's
 * (pgid, sid) membership together with num_tasks++ in ONE critical section -- the
 * same lock setsid's group-reuse scan holds. This linearizes the child's
 * GROUP-MEMBERSHIP publication against setsid, so a group cannot span two
 * sessions on SMP. It does NOT serialize task-SLOT reservation (pid = num_tasks
 * and most TCB init stay lock-free per the pre-existing task-table convention);
 * it guards exactly the (pgid, sid, num_tasks) triple a job-control scan reads.
 * IRQ-safe (spin_lock_irqsave): the publication must keep the calling CPU's IRQs
 * off across num_tasks++ so a timer cannot schedule the half-built child before
 * fork finishes (INT 0x80 runs with IF=0; irqrestore preserves that). Returns
 * saved IRQ flags for the matching unlock. A strict leaf (takes nothing else);
 * task creation never runs while a job-control op holds it, so no nesting. The
 * Ctrl+C ISR reads the foreground group lock-free and never takes this lock. */
uint64_t pgroup_jobctl_lock(void);
void     pgroup_jobctl_unlock(uint64_t flags);

/* --- Process group / session mutation (0 on success, negative errno on error).
 * Errno values are the kernel/errno.h magnitudes returned NEGATED (e.g. -EPERM),
 * matching the sys_* return convention. --- */

/* setpgid(pid, pgid): pid==0 => caller; pgid==0 => use the target pid (create a
 * group). Enforces POSIX: target is the caller or one of its children in the
 * caller's session, target is not a session leader, target has not already
 * exec'd (EACCES), and the destination group is in the caller's session. */
int pgroup_setpgid(uint32_t pid, uint32_t pgid);

/* setsid(): the caller becomes leader of a brand-new session and process group
 * (sid = pgid = caller pid) and detaches from the controlling console. Fails
 * with -EPERM if the caller is already a process-group leader. Returns the new
 * session id on success. */
int pgroup_setsid(void);

/* Queries: return the id, or negative errno (-ESRCH for an unknown/dead pid). */
int pgroup_getpgid(uint32_t pid);   /* pid==0 => caller */
int pgroup_getsid(uint32_t pid);    /* pid==0 => caller */
int pgroup_getpgrp(void);           /* caller's pgid */

/* --- Controlling-console foreground group (tcsetpgrp / tcgetpgrp) --- */

/* tcsetpgrp(pgid): make pgid the console's foreground group. The caller must be
 * in the console's controlling session and pgid must be a non-empty group in
 * that same session. If no session yet controls the console, the caller's
 * session claims it. Returns 0 or negative errno (-ENOTTY / -EPERM / -EINVAL). */
int pgroup_tcsetpgrp(uint32_t pgid);

/* tcgetpgrp(): the console's foreground pgid, or negative errno when unset
 * (-ENOTTY: no foreground group has been established). */
int pgroup_tcgetpgrp(void);

/* is_orphaned_pgrp(pgid): 1 if the group is orphaned (no member has a parent in
 * a different group within the same session), else 0. DETECTOR ONLY -- the POSIX
 * SIGHUP/SIGCONT delivery to a newly-orphaned stopped group is deferred until
 * job-control stop/continue signals exist. */
int pgroup_is_orphaned(uint32_t pgid);

/* GenerateConsoleCtrlEvent(dwCtrlEvent, dwProcessGroupId): Win32 console control.
 * dwCtrlEvent must be CTRL_C_EVENT or CTRL_BREAK_EVENT (both -> SIGINT).
 * dwProcessGroupId==0 targets the caller's own process group. Returns 1 (TRUE)
 * on success, 0 (FALSE) on a bad event / empty target group. */
int GenerateConsoleCtrlEvent(uint32_t dwCtrlEvent, uint32_t dwProcessGroupId);

/* --- Pure POSIX decision cores (no task lookups; unit-tested directly) ------
 * The syscall wrappers gather live task state, then call these to decide. Ids
 * are pre-normalized (pid!=0, pgid!=0). Return 0 on allow, negative errno on
 * deny. Exposed so the full (allow/EACCES/EPERM/ESRCH/EINVAL) matrix is testable
 * without mutating the live task table. --- */

/* setpgid policy given the target's live facts. target_is_child is 1 when the
 * target is one of the caller's children (0 when it IS the caller). dest_present
 * is 1 when new_pgid names an existing live group; dest_sid is that group's
 * session (ignored when new_pgid == target_pid, i.e. the target makes its own
 * group). */
int pgroup_setpgid_decide(uint32_t caller_sid,
                          uint32_t target_pid, uint32_t target_sid,
                          int target_is_caller, int target_is_child,
                          int target_has_execed,
                          uint32_t new_pgid, int dest_present, uint32_t dest_sid);

/* setsid policy: -EPERM if the caller is already a process-group leader
 * (caller_pgid == caller_pid) OR a process group named caller_pid still has other
 * live members (the caller led it earlier and left) -- either would make the new
 * session's pgid=caller_pid collide. `group_has_other_members` is that live fact.
 * Else 0. */
int pgroup_setsid_decide(uint32_t caller_pid, uint32_t caller_pgid,
                         int group_has_other_members);

/* is_orphaned per-link predicate: 1 if a member's parent link DISQUALIFIES the
 * group from being orphaned (the parent is live, in a different group, same
 * session). A group is orphaned iff no member has such a link. Pure + testable. */
int pgroup_orphan_link_keeps_alive(uint32_t member_pgid, uint32_t member_sid,
                                   int parent_alive, uint32_t parent_pgid,
                                   uint32_t parent_sid);

/* GenerateConsoleCtrlEvent authorization: 1 (allow) iff the console has an owner,
 * the caller is in the controlling session, and the target group is present in
 * that same session -- so a control event can never cross the console-session
 * boundary. grp_present/grp_sid come from a live-member scan. */
int pgroup_genconsole_authorized(uint32_t caller_sid, int has_owner,
                                 uint32_t controlling_sid,
                                 int grp_present, uint32_t grp_sid);

/* Snapshot the current foreground pgid for the Ctrl+C ISR path. Returns 1 and
 * writes *out_pgid when a foreground group is set; returns 0 (no delivery) when
 * unset -- the ISR must be a no-op in that state so Ctrl+C never fans a signal
 * into PID 0 / the kernel session before a shell claims the console. */
int pgroup_foreground_snapshot(uint32_t *out_pgid);

#endif /* KERNEL_IPC_PGROUP_H */
