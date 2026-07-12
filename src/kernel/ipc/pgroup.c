/* ============================================================================
 * pgroup.c -- POSIX process groups, sessions, and console job control
 *
 * See kernel/ipc/pgroup.h for the ownership boundary. Load-bearing invariants:
 *   - The Ctrl+C fan-out is a NO-OP until a shell establishes a foreground group
 *     (g_console.foreground_pgid != 0), so a bare pgid==0 scan can never terminate
 *     PID 0 or the kernel session.
 *   - setpgid returns EACCES for a child that has already exec'd (the fork/exec
 *     race POSIX defines), with the full ESRCH/EPERM/EINVAL/EACCES mapping.
 *   - A single IRQ-safe (spin_lock_irqsave) job-control lock covers pgid/sid
 *     validate-and-commit AND the console owner fields, so a parent setpgid(child)
 *     cannot race the child's setsid/setpgid into an inconsistent (pgid, sid)
 *     pair; irqsave also keeps IRQs off across the task-publication num_tasks++ so
 *     a timer cannot schedule a half-built fork child. The latency-critical Ctrl+C
 *     ISR reads foreground_pgid LOCK-FREE (atomic) and never takes this lock, so
 *     it never spins behind a process-context scan (a bounded O(TASK_MAX) scan
 *     under the lock is a self-inflicted, non-ISR latency).
 *   - tcsetpgrp enforces the caller-in-controlling-session + non-empty-target-
 *     group-in-session authorization boundary rather than a raw global write.
 * ============================================================================ */

#include "kernel/ipc/pgroup.h"
#include "kernel/ipc/signal.h"          /* signal_send_group, SIGINT */
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/errno.h"
#include "kernel/klog.h"

/* --- Controlling-console job-control object (singleton) ------------------- *
 * One console today. `lock` serializes every pgid/sid mutation and the owner
 * fields, so setpgid/setsid/tcsetpgrp validate-and-commit transactions linearize.
 * `foreground_pgid` is a SINGLE-WORD ATOMIC (0 = no foreground group): the Ctrl+C
 * keyboard ISR reads it LOCK-FREE (an aligned uint32 never tears), so the ISR
 * never takes `lock` and can never spin behind a process-context scan. Writers
 * hold `lock` for validation but commit `foreground_pgid` with an atomic store.
 * pgid 0 is rejected as a foreground group (tcsetpgrp EINVAL), so 0 unambiguously
 * means "unset". */
struct console_jobctl {
    spinlock_t lock;
    uint32_t   controlling_sid;   /* session that owns the console (valid iff has_owner) */
    uint32_t   foreground_pgid;   /* ATOMIC: foreground group, 0 = none (ISR reads lock-free) */
    uint8_t    has_owner;         /* 1 once a session has claimed the console */
};

static struct console_jobctl g_console = {
    .lock            = SPINLOCK_INIT,
    .controlling_sid = 0,
    .foreground_pgid = 0,
    .has_owner       = 0,
};

/* Look up a live (non-dead) task by pid; NULL if absent or dead. */
static struct task *live_task(uint32_t pid)
{
    struct task *t = task_get_by_pid(pid);
    if (!t || t->state == TASK_DEAD)
        return (struct task *)0;
    return t;
}

/* Count live members of a process group (bounded scan of tasks[]). PID 0 (the
 * kernel session) is never a job-control group member. Callers that need a
 * coherent view hold g_console.lock across the scan + their decision. */
static uint32_t pgrp_member_count(uint32_t pgid)
{
    uint32_t n = task_count();
    uint32_t i;
    uint32_t members = 0;
    for (i = 1; i < n; i++) {           /* i=1: PID 0 excluded */
        struct task *t = task_get_by_pid(i);
        if (t && t->state != TASK_DEAD && t->pgid == pgid)
            members++;
    }
    return members;
}

/* A process group EXISTS as long as ANY live task is a member -- it is NOT tied
 * to a leader whose pid==pgid (the leader can exit while members remain). Return
 * 1 and set *out_sid to a live member's session if the group exists, else 0.
 * PID 0 is excluded. Callers hold g_console.lock. */
static int pgrp_session(uint32_t pgid, uint32_t *out_sid)
{
    uint32_t n = task_count();
    uint32_t i;
    for (i = 1; i < n; i++) {           /* i=1: PID 0 excluded */
        struct task *t = task_get_by_pid(i);
        if (t && t->state != TASK_DEAD && t->pgid == pgid) {
            if (out_sid)
                *out_sid = t->sid;
            return 1;
        }
    }
    return 0;
}

/* Is any live task a member of session `sid`? PID 0 (session 0, the kernel
 * session) is excluded -- it is never a reclaimable console owner. Callers hold
 * g_console.lock. */
static int session_alive(uint32_t sid)
{
    uint32_t n = task_count();
    uint32_t i;
    for (i = 1; i < n; i++) {           /* i=1: PID 0 / session 0 excluded */
        struct task *t = task_get_by_pid(i);
        if (t && t->state != TASK_DEAD && t->sid == sid)
            return 1;
    }
    return 0;
}

/* Release the console if its controlling session has no live members left, so a
 * later session can claim it (otherwise a terminated shell bricks job control
 * until reboot). Caller holds g_console.lock. Idempotent. */
static void console_reclaim_if_dead(void)
{
    if (g_console.has_owner && !session_alive(g_console.controlling_sid)) {
        g_console.has_owner = 0;
        /* foreground group belonged to the dead session -- atomic store so the
         * lock-free ISR reader observes the clear. */
        __atomic_store_n(&g_console.foreground_pgid, 0u, __ATOMIC_RELEASE);
    }
}

void pgroup_note_exec(struct task *t)
{
    uint64_t flags;
    /* Set has_execed under the job-control lock so a concurrent pgroup_setpgid
     * (which reads it under the same lock) cannot commit target->pgid AFTER the
     * exec completes: the two transitions are now mutually exclusive, not merely
     * ordered. RELEASE store pairs with pgroup_setpgid's ACQUIRE load. */
    spin_lock_irqsave(&g_console.lock, &flags);
    __atomic_store_n(&t->has_execed, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_console.lock, flags);
}

uint64_t pgroup_jobctl_lock(void)
{
    uint64_t flags;
    spin_lock_irqsave(&g_console.lock, &flags);
    return flags;
}

void pgroup_jobctl_unlock(uint64_t flags)
{
    spin_unlock_irqrestore(&g_console.lock, flags);
}

/* ------------------------------------------------------------------------- */

int pgroup_setpgid_decide(uint32_t caller_sid,
                          uint32_t target_pid, uint32_t target_sid,
                          int target_is_caller, int target_is_child,
                          int target_has_execed,
                          uint32_t new_pgid, int dest_present, uint32_t dest_sid)
{
    /* A negative pgid (arrives as a high-bit-set uint32 from the syscall register)
     * is invalid (POSIX EINVAL). Valid pgids are small positive process ids. */
    if ((int32_t)new_pgid < 0)
        return -EINVAL;
    /* The target must be the caller or one of the caller's children. */
    if (!target_is_caller && !target_is_child)
        return -ESRCH;
    /* A child that has already exec'd cannot be moved (POSIX EACCES). */
    if (!target_is_caller && target_has_execed)
        return -EACCES;
    /* Target must be in the caller's session and not a session leader. */
    if (target_sid != caller_sid)
        return -EPERM;
    if (target_sid == target_pid)               /* target is a session leader */
        return -EPERM;
    /* The destination group must be in the caller's session: either the target
     * makes its own group (new_pgid == target_pid) or an existing same-session
     * group. An unknown destination group is EPERM (no such group in session). */
    if (new_pgid != target_pid) {
        if (!dest_present || dest_sid != caller_sid)
            return -EPERM;
    }
    return 0;
}

int pgroup_setsid_decide(uint32_t caller_pid, uint32_t caller_pgid,
                         int group_has_other_members)
{
    /* A process-group leader cannot create a new session (POSIX EPERM): a leader
     * is exactly a process whose pgid equals its own pid. */
    if (caller_pgid == caller_pid)
        return -EPERM;
    /* Even a non-leader is rejected if a group named caller_pid still has other
     * live members: a new session with pgid=caller_pid would span two sessions. */
    if (group_has_other_members)
        return -EPERM;
    return 0;
}

int pgroup_orphan_link_keeps_alive(uint32_t member_pgid, uint32_t member_sid,
                                   int parent_alive, uint32_t parent_pgid,
                                   uint32_t parent_sid)
{
    /* A live parent in a DIFFERENT group but the SAME session keeps the member's
     * group non-orphaned (a signal could still reach it via that parent). */
    return parent_alive && parent_pgid != member_pgid && parent_sid == member_sid;
}

int pgroup_genconsole_authorized(uint32_t caller_sid, int has_owner,
                                 uint32_t controlling_sid,
                                 int grp_present, uint32_t grp_sid)
{
    /* A console control event may only reach a group in the caller's own
     * controlling console session: the console must be owned, the caller must be
     * in the controlling session, and the target group must exist in that same
     * session. Anything else (no owner, foreign caller, empty/foreign group) is
     * unauthorized -- never signal across the session boundary. */
    if (!has_owner)
        return 0;
    if (caller_sid != controlling_sid)
        return 0;
    if (!grp_present || grp_sid != controlling_sid)
        return 0;
    return 1;
}

int pgroup_setpgid(uint32_t pid, uint32_t pgid)
{
    struct task *caller = task_current();
    uint32_t caller_pid, caller_sid;
    int rc;

    uint64_t flags;
    spin_lock_irqsave(&g_console.lock, &flags);
    /* Sample caller identity INSIDE the lock: a concurrent setsid on another
     * thread of the caller must not leave us validating against a stale sid. */
    caller_pid = caller->pid;
    caller_sid = caller->sid;
    if (pid == 0)
        pid = caller_pid;
    if (pgid == 0)
        pgid = pid;                     /* pgid 0 => create/join the target's own group */
    /* Reject a negative pgid (EINVAL) BEFORE the target lookup, so the live path
     * and pgroup_setpgid_decide agree on precedence -- e.g. setpgid(unknown, -1)
     * is EINVAL on both, not ESRCH here vs EINVAL in the core. */
    if ((int32_t)pgid < 0) {
        rc = -EINVAL;
        goto out;
    }
    {
        struct task *target = live_task(pid);
        if (!target) {
            rc = -ESRCH;                /* target does not exist (cannot decide) */
            goto out;
        }
        {
            int dest_present = 0;
            uint32_t dest_sid = 0;
            /* A group exists as long as any member is alive -- resolve via a live
             * member scan, NOT live_task(pgid) (the leader may have exited). */
            if (pgid != target->pid)
                dest_present = pgrp_session(pgid, &dest_sid);
            /* has_execed: pgroup_note_exec's RELEASE store under this same lock
             * pairs with this ACQUIRE load, making exec vs setpgid exclusive. */
            rc = pgroup_setpgid_decide(
                caller_sid, target->pid, target->sid,
                target->pid == caller_pid,
                target->parent_pid == caller_pid,
                __atomic_load_n(&target->has_execed, __ATOMIC_ACQUIRE),
                pgid, dest_present, dest_sid);
            if (rc == 0)
                target->pgid = pgid;
        }
    }
out:
    spin_unlock_irqrestore(&g_console.lock, flags);
    if (rc == 0)
        klog(LOG_DEBUG, "pgrp", "setpgid: pid %u -> pgid %u",
             (uint64_t)pid, (uint64_t)pgid);
    return rc;
}

int pgroup_setsid(void)
{
    struct task *caller = task_current();
    uint32_t caller_pid;
    int rc;

    uint64_t flags;
    spin_lock_irqsave(&g_console.lock, &flags);
    caller_pid = caller->pid;
    /* Gather the live group-collision fact under the lock, then let the pure core
     * decide (so the tested decider and the live path can never diverge). */
    rc = pgroup_setsid_decide(caller_pid, caller->pgid,
                              pgrp_member_count(caller_pid) > 0);
    if (rc == 0) {
        caller->sid  = caller_pid;
        caller->pgid = caller_pid;
        /* Detach from the controlling console: a new session never inherits the
         * old session's controlling terminal (POSIX). Ownership stays with the
         * old session's other members; the new leader has no controlling tty. */
        rc = (int)caller_pid;
    }
    spin_unlock_irqrestore(&g_console.lock, flags);
    /* Log AFTER releasing the lock: klog takes further locks and polls the UART,
     * which must never happen while holding a leaf spinlock with IRQs disabled.
     * Success returns the new sid (>= 0); failure returns a negative errno. */
    if (rc >= 0)
        klog(LOG_DEBUG, "pgrp", "setsid: pid %u new session/group", (uint64_t)caller_pid);
    return rc;
}

int pgroup_getpgid(uint32_t pid)
{
    struct task *t;
    if (pid == 0)
        return (int)task_current()->pgid;
    t = live_task(pid);
    return t ? (int)t->pgid : -ESRCH;
}

int pgroup_getsid(uint32_t pid)
{
    struct task *t;
    if (pid == 0)
        return (int)task_current()->sid;
    t = live_task(pid);
    return t ? (int)t->sid : -ESRCH;
}

int pgroup_getpgrp(void)
{
    return (int)task_current()->pgid;
}

int pgroup_tcsetpgrp(uint32_t pgid)
{
    struct task *caller = task_current();
    uint32_t caller_sid, grp_sid, effective_sid;
    int rc;

    /* Group 0 is the kernel session and can never be a console foreground group
     * (it would let Ctrl+C reach kernel threads / early boot processes). */
    if (pgid == 0)
        return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_console.lock, &flags);
    console_reclaim_if_dead();          /* a dead controlling session releases the console */
    caller_sid = caller->sid;
    /* The session that will control the console after this call: the existing
     * owner, or (if unowned) the caller's session. The caller must belong to it. */
    if (g_console.has_owner) {
        if (caller_sid != g_console.controlling_sid) {
            rc = -ENOTTY;
            goto out;
        }
        effective_sid = g_console.controlling_sid;
    } else {
        effective_sid = caller_sid;
    }
    /* The target group must have a live member (a group outlives its leader), and
     * that member's session must be the controlling session -- derive it from a
     * live member, never from live_task(pgid) whose leader may be dead. */
    if (!pgrp_session(pgid, &grp_sid)) {
        rc = -EINVAL;                   /* empty / nonexistent group */
        goto out;
    }
    if (grp_sid != effective_sid) {
        rc = -EPERM;                    /* foreign-session group */
        goto out;
    }
    /* All checks passed: commit console ownership (if unowned) AND the foreground
     * group transactionally, so a failed call never leaves the console claimed. */
    if (!g_console.has_owner) {
        g_console.controlling_sid = caller_sid;
        g_console.has_owner       = 1;
    }
    /* Atomic store so the lock-free Ctrl+C ISR observes the new foreground group
     * without taking g_console.lock. */
    __atomic_store_n(&g_console.foreground_pgid, pgid, __ATOMIC_RELEASE);
    rc = 0;
out:
    spin_unlock_irqrestore(&g_console.lock, flags);
    return rc;
}

int pgroup_tcgetpgrp(void)
{
    int rc;
    uint64_t flags;
    spin_lock_irqsave(&g_console.lock, &flags);
    console_reclaim_if_dead();
    {
        uint32_t fg = g_console.foreground_pgid;
        rc = fg ? (int)fg : -ENOTTY;
    }
    spin_unlock_irqrestore(&g_console.lock, flags);
    return rc;
}

int pgroup_is_orphaned(uint32_t pgid)
{
    uint32_t n = task_count();
    uint32_t i;
    /* Orphaned iff no member has a parent that is in a DIFFERENT group but the
     * SAME session. A single non-orphaning parent link disqualifies. */
    for (i = 1; i < n; i++) {
        struct task *t = task_get_by_pid(i);
        struct task *p;
        if (!t || t->state == TASK_DEAD || t->pgid != pgid)
            continue;
        p = live_task(t->parent_pid);
        if (pgroup_orphan_link_keeps_alive(t->pgid, t->sid,
                                           p != (struct task *)0,
                                           p ? p->pgid : 0, p ? p->sid : 0))
            return 0;                   /* has an in-session parent outside the group */
    }
    return 1;
}

int GenerateConsoleCtrlEvent(uint32_t dwCtrlEvent, uint32_t dwProcessGroupId)
{
    struct task *caller = task_current();
    uint32_t pgid, caller_sid, grp_sid = 0;
    int grp_present, authorized;

    if (dwCtrlEvent != CTRL_C_EVENT && dwCtrlEvent != CTRL_BREAK_EVENT)
        return 0;                       /* FALSE: unsupported event */

    /* Gather the console + target-group facts and authorize under the lock; then
     * fan the signal out AFTER releasing it (never klog/signal under the lock). */
    uint64_t flags;
    spin_lock_irqsave(&g_console.lock, &flags);
    console_reclaim_if_dead();
    caller_sid = caller->sid;
    pgid = (dwProcessGroupId == 0) ? caller->pgid : dwProcessGroupId;
    grp_present = (pgid != 0) ? pgrp_session(pgid, &grp_sid) : 0;
    authorized = (pgid != 0) &&
                 pgroup_genconsole_authorized(caller_sid, g_console.has_owner,
                                              g_console.controlling_sid,
                                              grp_present, grp_sid);
    spin_unlock_irqrestore(&g_console.lock, flags);

    if (!authorized)
        return 0;                       /* FALSE: unauthorized / empty / cross-session */
    signal_send_group(pgid, SIGINT);
    return 1;                           /* TRUE */
}

int pgroup_foreground_snapshot(uint32_t *out_pgid)
{
    /* ISR path (Ctrl+C): a single LOCK-FREE atomic load. The ISR does NOT take
     * g_console.lock -- so it can never spin behind a process-context tcsetpgrp/
     * GenerateConsoleCtrlEvent scan -- and does NOT run the O(TASK_MAX) reclaim
     * scan (dead-owner reclamation is the process-context callers' job). A stale
     * foreground group here is harmless: signal_send_group finds no live members
     * of a dead group and no-ops. 0 unambiguously means "no foreground group". */
    uint32_t fg = __atomic_load_n(&g_console.foreground_pgid, __ATOMIC_ACQUIRE);
    if (fg == 0)
        return 0;
    if (out_pgid)
        *out_pgid = fg;
    return 1;
}
