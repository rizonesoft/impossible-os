/* ============================================================================
 * nt_audit.c -- Syscall audit and tracing hook (SSDT 0x0150-0x0152)
 *
 * Fixed hook pool + one spinlock for registration. The hot path reads a
 * relaxed atomic count and bails at zero cost when no hook is registered.
 *
 * Invocation safety (the hard part on this kernel's uniprocessor primitives):
 *   - Hooks are snapshotted under s_audit_lock and invoked OUTSIDE it, so a
 *     hook may safely yield/block/take locks.
 *   - Each slot carries an in_flight atomic count; a dispatch increments it
 *     under the lock and decrements after invoking. nt_audit_unregister has
 *     non-blocking try-semantics: under the lock, in_flight==0 proves no
 *     dispatch is mid-invoke on the slot (the increment is under the same
 *     lock), so it clears the slot and the caller may free the context; if a
 *     dispatch is in flight it returns STATUS_UNSUCCESSFUL and the caller
 *     retries (there is no wait/wake primitive to block on). register reuses a
 *     slot only when it is free AND has no in-flight dispatch.
 *   - Recursion is broken by a PER-THREAD thread->in_audit flag (migration-safe):
 *     a hook's own syscall is not re-audited.
 * Registration is a kernel-internal C API only (a raw function-pointer syscall
 * is unsafe until previous-mode tracking is SMP-closed); only the read-only
 * state query is SSDT-exposed.
 * ============================================================================ */

#include "kernel/nt/nt_audit.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/zw.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/task.h"
#include "kernel/cpu_security.h"
#include "kernel/klog.h"

/* ---- Hook table ---------------------------------------------------------- */

typedef struct audit_hook {
    int32_t               handle;       /* 0 = free; else a live registration id */
    uint32_t              flags;        /* AUDIT_PRE_CALL / POST_CALL / BOTH */
    SYSCALL_AUDIT_ROUTINE routine;
    void                 *context;
    uint64_t              invocations;
    uint32_t              in_flight;    /* dispatches currently invoking this slot (atomic) */
} audit_hook_t;

static audit_hook_t   g_hooks[NT_AUDIT_MAX_HOOKS];
static DEFINE_SPINLOCK(s_audit_lock);
static uint32_t       g_audit_count;          /* live hook count (atomic fast-path read) */
static int32_t        g_next_id = 1;          /* monotonic; 0 is never a live id */
static uint64_t       g_denied_count;         /* syscalls blocked by a pre-hook (atomic) */

#define AUDIT_ID_MAX  0x7FFFFFFF

static inline int in_audit_thread(void)
{
    struct thread *t = thread_current();
    return (t && t->in_audit);
}

uint32_t nt_audit_hook_count(void)
{
    return __atomic_load_n(&g_audit_count, __ATOMIC_RELAXED);
}

/* ---- Registration -------------------------------------------------------- */

NTSTATUS nt_audit_register(SYSCALL_AUDIT_ROUTINE routine, void *context,
                           uint32_t flags, int32_t *out_handle)
{
    if (!routine || !out_handle)
        return STATUS_INVALID_PARAMETER;
    if ((flags & AUDIT_FLAG_MASK) == 0 || (flags & ~AUDIT_FLAG_MASK) != 0)
        return STATUS_INVALID_PARAMETER;
    /* A hook must not mutate the table from inside its own callback: unregister
     * would self-drain (deadlock) and register recursion is disallowed. */
    if (in_audit_thread())
        return STATUS_UNSUCCESSFUL;

    uint64_t irq;
    spin_lock_irqsave(&s_audit_lock, &irq);

    if (g_next_id >= AUDIT_ID_MAX) {
        spin_unlock_irqrestore(&s_audit_lock, irq);
        return STATUS_INSUFFICIENT_RESOURCES;   /* id space exhausted */
    }
    for (uint32_t i = 0; i < NT_AUDIT_MAX_HOOKS; i++) {
        /* Reuse a slot only once it is both free AND fully drained. */
        if (g_hooks[i].handle == 0 &&
            __atomic_load_n(&g_hooks[i].in_flight, __ATOMIC_ACQUIRE) == 0) {
            g_hooks[i].flags       = flags & AUDIT_FLAG_MASK;
            g_hooks[i].routine     = routine;
            g_hooks[i].context     = context;
            g_hooks[i].invocations = 0;
            g_hooks[i].handle      = g_next_id++;
            *out_handle = g_hooks[i].handle;
            __atomic_store_n(&g_audit_count, g_audit_count + 1, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&s_audit_lock, irq);
            return STATUS_SUCCESS;
        }
    }

    spin_unlock_irqrestore(&s_audit_lock, irq);
    return STATUS_INSUFFICIENT_RESOURCES;   /* pool full */
}

NTSTATUS nt_audit_unregister(int32_t handle)
{
    if (handle <= 0)
        return STATUS_INVALID_HANDLE;
    if (in_audit_thread())
        return STATUS_UNSUCCESSFUL;   /* would self-drain */

    uint64_t irq;
    spin_lock_irqsave(&s_audit_lock, &irq);
    for (uint32_t i = 0; i < NT_AUDIT_MAX_HOOKS; i++) {
        if (g_hooks[i].handle == handle) {
            /* A dispatch increments in_flight UNDER this lock before releasing
             * it to invoke, so in_flight==0 here proves no dispatch is mid-invoke
             * on this slot: once cleared, the caller may safely free the context.
             * If a dispatch is in flight, refuse (busy) rather than block -- the
             * tree has no wait/wake primitive and a yield-drain can livelock
             * under priority inversion. The caller retries at a quiescent point. */
            if (__atomic_load_n(&g_hooks[i].in_flight, __ATOMIC_ACQUIRE) != 0) {
                spin_unlock_irqrestore(&s_audit_lock, irq);
                return STATUS_UNSUCCESSFUL;
            }
            g_hooks[i].handle  = 0;
            g_hooks[i].routine = (SYSCALL_AUDIT_ROUTINE)0;
            g_hooks[i].context = (void *)0;
            g_hooks[i].flags   = 0;
            __atomic_store_n(&g_audit_count, g_audit_count - 1, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&s_audit_lock, irq);
            return STATUS_SUCCESS;
        }
    }
    spin_unlock_irqrestore(&s_audit_lock, irq);
    return STATUS_INVALID_HANDLE;
}

void nt_audit_reset_for_test(void)
{
    uint64_t irq;
    spin_lock_irqsave(&s_audit_lock, &irq);
    for (uint32_t i = 0; i < NT_AUDIT_MAX_HOOKS; i++) {
        g_hooks[i].handle  = 0;
        g_hooks[i].routine = (SYSCALL_AUDIT_ROUTINE)0;
        g_hooks[i].context = (void *)0;
        g_hooks[i].flags   = 0;
        g_hooks[i].invocations = 0;
        __atomic_store_n(&g_hooks[i].in_flight, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_audit_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_denied_count, 0, __ATOMIC_RELEASE);
    /* g_next_id kept monotonic so a stale handle never re-matches. */
    spin_unlock_irqrestore(&s_audit_lock, irq);
}

/* ---- Hot-path dispatch (session-scoped) ---------------------------------- */

NTSTATUS nt_audit_begin(uint32_t service_number, const uint64_t *args,
                        nt_audit_session_t *sess)
{
    sess->n = 0;
    struct thread *t = thread_current();
    if (t && t->in_audit)
        return STATUS_SUCCESS;   /* recursion: a hook's own syscall is not audited */

    /* One snapshot of ALL matching hooks (pre and/or post); hold an in_flight
     * ref on each for the whole session so unregister cannot remove a hook
     * between its PRE and POST. */
    uint32_t n = 0;
    uint64_t irq;
    spin_lock_irqsave(&s_audit_lock, &irq);
    for (uint32_t i = 0; i < NT_AUDIT_MAX_HOOKS; i++) {
        if (g_hooks[i].handle != 0 && (g_hooks[i].flags & AUDIT_FLAG_MASK)) {
            sess->hooks[n].routine = g_hooks[i].routine;
            sess->hooks[n].context = g_hooks[i].context;
            sess->hooks[n].idx     = i;
            sess->hooks[n].flags   = g_hooks[i].flags;
            g_hooks[i].invocations++;
            __atomic_add_fetch(&g_hooks[i].in_flight, 1, __ATOMIC_ACQUIRE);
            n++;
        }
    }
    spin_unlock_irqrestore(&s_audit_lock, irq);
    sess->n = n;
    if (n == 0)
        return STATUS_SUCCESS;

    /* Invoke PRE hooks with the recursion guard set (cleared before returning
     * so the handler runs un-guarded and its own nested syscalls are audited). */
    NTSTATUS block = STATUS_SUCCESS;
    if (t)
        t->in_audit = 1;
    for (uint32_t i = 0; i < n; i++) {
        if ((sess->hooks[i].flags & AUDIT_PRE_CALL) && block == STATUS_SUCCESS) {
            NTSTATUS r = sess->hooks[i].routine(service_number, args,
                                                sess->hooks[i].context,
                                                AUDIT_PHASE_PRE, STATUS_SUCCESS);
            if (r != STATUS_SUCCESS)
                block = r;   /* first denier wins */
        }
    }
    if (t)
        t->in_audit = 0;

    if (block != STATUS_SUCCESS) {
        /* Denied: release the refs now; there is no POST phase. */
        for (uint32_t i = 0; i < n; i++)
            __atomic_sub_fetch(&g_hooks[sess->hooks[i].idx].in_flight, 1, __ATOMIC_RELEASE);
        sess->n = 0;
        __atomic_add_fetch(&g_denied_count, 1, __ATOMIC_RELAXED);
        return block;
    }
    return STATUS_SUCCESS;   /* refs stay held for nt_audit_end */
}

void nt_audit_end(nt_audit_session_t *sess, uint32_t service_number,
                  const uint64_t *args, NTSTATUS status)
{
    uint32_t n = sess->n;
    if (n == 0)
        return;   /* no session (recursion, no hooks, or denied) */

    struct thread *t = thread_current();
    if (t)
        t->in_audit = 1;
    for (uint32_t i = 0; i < n; i++) {
        if (sess->hooks[i].flags & AUDIT_POST_CALL)
            (void)sess->hooks[i].routine(service_number, args,
                                         sess->hooks[i].context,
                                         AUDIT_PHASE_POST, status);
    }
    if (t)
        t->in_audit = 0;
    for (uint32_t i = 0; i < n; i++)
        __atomic_sub_fetch(&g_hooks[sess->hooks[i].idx].in_flight, 1, __ATOMIC_RELEASE);
    sess->n = 0;
}

/* ---- Syscall handlers ---------------------------------------------------- *
 * Registration is NOT exposed through the SSDT (a raw function-pointer syscall
 * is unsafe until previous-mode tracking is SMP-closed); nt_audit_register /
 * nt_audit_unregister above are the kernel C API. Only the read-only state
 * query is a syscall. */

/* NtQuerySyscallAuditState(PVOID Buffer, ULONG Length, PULONG ReturnLength).
 * Returns hook metadata (handle/flags/invocation count) -- no function
 * pointers are exposed, so this is safe to read from user mode. */
static NTSTATUS NtQuerySyscallAuditState_handler(uint64_t buffer, uint64_t buf_len_raw,
                                                 uint64_t retlen_out, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    uint32_t length = (uint32_t)buf_len_raw;   /* ULONG ABI; narrow raw arg */

    /* Snapshot the live hooks under the lock (brief; hooks are not invoked
     * here, so a query issued from within a hook does not self-deadlock). */
    SYSCALL_AUDIT_HOOK_INFO info[NT_AUDIT_MAX_HOOKS];
    uint32_t active = 0;
    uint64_t irq;
    spin_lock_irqsave(&s_audit_lock, &irq);
    for (uint32_t i = 0; i < NT_AUDIT_MAX_HOOKS; i++) {
        if (g_hooks[i].handle != 0) {
            info[active].Handle          = g_hooks[i].handle;
            info[active].Flags           = g_hooks[i].flags;
            info[active].InvocationCount = g_hooks[i].invocations;
            active++;
        }
    }
    spin_unlock_irqrestore(&s_audit_lock, irq);
    uint64_t denied = __atomic_load_n(&g_denied_count, __ATOMIC_RELAXED);

    uint32_t required = (uint32_t)__builtin_offsetof(SYSCALL_AUDIT_STATE, Hooks)
                        + active * (uint32_t)sizeof(SYSCALL_AUDIT_HOOK_INFO);
    if (retlen_out) {
        if (ProbeForWriteIfUser((void *)retlen_out, sizeof(uint32_t), sizeof(uint32_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
        if (copy_to_user((void *)retlen_out, &required, sizeof(uint32_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
    }
    if (!buffer || length < required)
        return STATUS_BUFFER_TOO_SMALL;

    /* Build the header + entries in a bounded kernel buffer, then copy out. */
    uint8_t staging[__builtin_offsetof(SYSCALL_AUDIT_STATE, Hooks)
                    + NT_AUDIT_MAX_HOOKS * sizeof(SYSCALL_AUDIT_HOOK_INFO)];
    SYSCALL_AUDIT_STATE *st = (SYSCALL_AUDIT_STATE *)staging;
    st->ActiveCount = active;
    st->MaxHooks    = NT_AUDIT_MAX_HOOKS;
    st->DeniedCount = denied;
    for (uint32_t i = 0; i < active; i++)
        st->Hooks[i] = info[i];

    if (ProbeForWriteIfUser((void *)buffer, required, sizeof(uint64_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (copy_to_user((void *)buffer, staging, required) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* ---- SSDT registration --------------------------------------------------- */

int nt_audit_register_ssdt(void)
{
    /* Only the read-only state query is exposed through the SSDT. Hook
     * registration takes a raw kernel function pointer, which is unsafe to
     * accept from a ring-3-reachable syscall while previous-mode tracking is
     * not SMP-closed; nt_audit_register/unregister remain kernel-internal C
     * APIs. Ring-3-safe registration (a validated hook descriptor gated by
     * SeAuditPrivilege) is owned by the token/SRM access-check work. SSDT
     * 0x0150/0x0151 stay reserved stubs. */
    return ssdt_register(SSDT_NtQuerySyscallAuditState,
                         (SSDT_HANDLER)NtQuerySyscallAuditState_handler);
}
