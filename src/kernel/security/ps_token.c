/* ============================================================================
 * ps_token.c -- Primary-token references + thread self-impersonation
 *
 * The process-side of SRM token assignment: pin/unpin a task's primary token
 * (Ob-refcounted) and install/clear a thread impersonation token on the CURRENT
 * thread. Tokens are Ob objects, so lifetime is refcount-driven; every pin here
 * is balanced by an unpin and every impersonation swap dereferences the token it
 * displaces. Cross-thread impersonation (NtImpersonateThread) and the effective-
 * token capture consumed by SeAccessCheck are owned by later SRM sections (the
 * per-token lock and the subject-context capture) that this file deliberately
 * does not reach into.
 * ============================================================================ */

#include "kernel/security/token.h"
#include "kernel/sched/task.h"
#include "kernel/ob/ob.h"
#include "kernel/klog.h"

ACCESS_TOKEN *PsReferencePrimaryToken(struct task *task)
{
    ACCESS_TOKEN *tok;

    if (!task || !task->token)
        return (ACCESS_TOKEN *)0;

    tok = (ACCESS_TOKEN *)task->token;
    ObReferenceObject(tok);   /* pin: caller balances with PsDereferencePrimaryToken */
    return tok;
}

void PsDereferencePrimaryToken(ACCESS_TOKEN *token)
{
    if (token)
        ObDereferenceObject(token);
}

void ImpersonateSelf(SECURITY_IMPERSONATION_LEVEL level)
{
    struct task   *cur = task_current();
    struct thread *thr = thread_current();
    ACCESS_TOKEN  *imp, *old;

    if (!cur || !thr || !cur->token)
        return;

    /* Deep-copy the process primary token as an impersonation token. The dup is
     * Ob-allocated with refcount 1, which the thread's impersonation slot owns
     * until it is swapped out or the thread reverts. */
    imp = NtDuplicateToken((ACCESS_TOKEN *)cur->token, 0, 0, TokenImpersonation);
    if (!imp)
        return;
    imp->ImpersonationLevel = level;

    /* Atomic exchange so a future effective-token reader never observes a torn
     * pointer; a prior impersonation token is dereferenced (its owning ref was
     * this slot). */
    old = (ACCESS_TOKEN *)__atomic_exchange_n(&thr->impersonation_token,
                                              imp, __ATOMIC_ACQ_REL);
    if (old)
        PsDereferencePrimaryToken(old);

    klog(LOG_DEBUG, "security", "Thread %u impersonating self at level %u",
         (uint64_t)thr->id, (uint64_t)level);
}

void RevertToSelf(void)
{
    struct thread *thr = thread_current();
    ACCESS_TOKEN  *old;

    if (!thr)
        return;

    old = (ACCESS_TOKEN *)__atomic_exchange_n(&thr->impersonation_token,
                                              (void *)0, __ATOMIC_ACQ_REL);
    if (old)
        PsDereferencePrimaryToken(old);
}
