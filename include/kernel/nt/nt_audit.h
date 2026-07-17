/* ============================================================================
 * nt_audit.h -- Syscall audit and tracing hook (SSDT 0x0150-0x0152)
 *
 * A first-class kernel API for syscall-level auditing with near-zero overhead
 * when no hook is registered:
 *   - NtRegisterSyscallAuditHook   (0x0150) -- kernel-mode only
 *   - NtUnregisterSyscallAuditHook (0x0151) -- kernel-mode only
 *   - NtQuerySyscallAuditState     (0x0152) -- reports active hooks + coverage
 *
 * The audit routine is a raw kernel function pointer, so registration is
 * gated ASSERT_KERNEL_CALLER (a ring-3 caller supplying a function pointer
 * would be arbitrary kernel-code execution). A pre-call hook returning
 * STATUS_ACCESS_DENIED blocks the syscall. Hooks are invoked from
 * ssdt_dispatch() around the handler call, covering the SYSCALL/SYSRET and
 * INT 0x2E surfaces (the legacy INT 0x80 path does not route through the SSDT
 * and is out of scope). A per-CPU in-audit guard prevents a hook that itself
 * issues a syscall from re-entering the audit path.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

/* ---- Registration flags (NtRegisterSyscallAuditHook Flags) ---------------- */

#define AUDIT_PRE_CALL     0x1u   /* invoke before the handler */
#define AUDIT_POST_CALL    0x2u   /* invoke after the handler */
#define AUDIT_BOTH         (AUDIT_PRE_CALL | AUDIT_POST_CALL)
#define AUDIT_FLAG_MASK    AUDIT_BOTH

/* ---- Audit phase (passed to the routine) ---------------------------------- */

#define AUDIT_PHASE_PRE    0u
#define AUDIT_PHASE_POST   1u

/* Maximum simultaneously-registered hooks (fixed pool; keeps the hot-path
 * snapshot on-stack and bounded). */
#define NT_AUDIT_MAX_HOOKS 8u

/* Audit routine. Called in kernel context on the syscalling thread.
 *   service_number : full SSDT service number as dispatched
 *   args           : the 6 syscall args (read-only snapshot)
 *   context        : opaque value supplied at registration
 *   phase          : AUDIT_PHASE_PRE or AUDIT_PHASE_POST
 *   status         : handler NTSTATUS (meaningful only for POST; 0 for PRE)
 * A PRE routine returning anything other than STATUS_SUCCESS blocks the
 * syscall, which returns that status to the caller. POST return is ignored. */
typedef NTSTATUS (*SYSCALL_AUDIT_ROUTINE)(uint32_t service_number,
                                          const uint64_t *args,
                                          void *context,
                                          uint32_t phase,
                                          NTSTATUS status);

/* ---- NtQuerySyscallAuditState output -------------------------------------- *
 * A header followed by ActiveCount SYSCALL_AUDIT_HOOK_INFO entries. */

typedef struct _SYSCALL_AUDIT_HOOK_INFO {
    int32_t  Handle;        /* registration id */
    uint32_t Flags;         /* AUDIT_PRE_CALL / POST_CALL / BOTH */
    uint64_t InvocationCount;   /* times this hook fired */
} SYSCALL_AUDIT_HOOK_INFO;
_Static_assert(sizeof(SYSCALL_AUDIT_HOOK_INFO) == 16,
               "SYSCALL_AUDIT_HOOK_INFO is 16 bytes");

typedef struct _SYSCALL_AUDIT_STATE {
    uint32_t                ActiveCount;    /* number of live hooks */
    uint32_t                MaxHooks;       /* NT_AUDIT_MAX_HOOKS */
    uint64_t                DeniedCount;    /* syscalls blocked by a pre-hook */
    SYSCALL_AUDIT_HOOK_INFO Hooks[1];       /* ActiveCount entries follow */
} SYSCALL_AUDIT_STATE;
_Static_assert(__builtin_offsetof(SYSCALL_AUDIT_STATE, Hooks) == 16,
               "SYSCALL_AUDIT_STATE.Hooks at offset 16");

/* ---- SSDT registration ---------------------------------------------------- */

int nt_audit_register_ssdt(void);   /* returns 0 on success, negative on failure */

/* ---- Hot-path dispatch (called from ssdt_dispatch) ------------------------ *
 * Fast path is a single relaxed atomic load (nt_audit_hook_count()); the
 * caller skips the whole session when it is zero.
 *
 * One audit SESSION spans the syscall so an AUDIT_BOTH hook's PRE and POST are
 * paired: nt_audit_begin snapshots the matching hooks once (holding a per-hook
 * in_flight ref) and invokes PRE; the refs stay held across the handler; then
 * nt_audit_end invokes POST from the SAME snapshot and releases the refs.
 * While the refs are held, nt_audit_unregister returns STATUS_UNSUCCESSFUL, so
 * a hook cannot be removed between its PRE and POST. */

/* Per-syscall snapshot; opaque to callers, lives on the ssdt_dispatch stack. */
typedef struct nt_audit_snap {
    SYSCALL_AUDIT_ROUTINE routine;
    void                 *context;
    uint32_t              idx;
    uint32_t              flags;
} nt_audit_snap_t;

typedef struct nt_audit_session {
    nt_audit_snap_t hooks[NT_AUDIT_MAX_HOOKS];
    uint32_t        n;
} nt_audit_session_t;

/* Begin a session: invoke PRE hooks. Returns STATUS_SUCCESS to proceed (call
 * the handler then nt_audit_end), or a blocking status from a PRE hook to deny
 * the syscall (do NOT call nt_audit_end -- begin already released its refs). */
NTSTATUS nt_audit_begin(uint32_t service_number, const uint64_t *args,
                        nt_audit_session_t *sess);
/* End a session (only after begin returned STATUS_SUCCESS): invoke POST hooks
 * with the handler result and release the session's refs. */
void nt_audit_end(nt_audit_session_t *sess, uint32_t service_number,
                  const uint64_t *args, NTSTATUS status);

/* Number of live hooks (relaxed). Inlined so the ssdt.c per-syscall fast path
 * is a single atomic load + branch with no frame or out-of-line call when idle. */
extern volatile uint32_t g_nt_audit_hook_count;
static inline uint32_t nt_audit_hook_count(void)
{
    return __atomic_load_n(&g_nt_audit_hook_count, __ATOMIC_RELAXED);
}

/* ---- Pure helpers (kernel API + unit tests) ------------------------------- */

/* Register a hook. Returns STATUS_SUCCESS + *out_handle (>0) or an error
 * (STATUS_INSUFFICIENT_RESOURCES when the pool is full). */
NTSTATUS nt_audit_register(SYSCALL_AUDIT_ROUTINE routine, void *context,
                           uint32_t flags, int32_t *out_handle);
/* Unregister by handle (non-blocking try-semantics). Returns STATUS_SUCCESS
 * once the hook is cleared (its context is then safe to free), STATUS_UNSUCCESSFUL
 * if a dispatch is currently invoking the hook (caller must NOT free the context;
 * retry at a quiescent point), or STATUS_INVALID_HANDLE if not live. Must not be
 * called from within an audit routine. */
NTSTATUS nt_audit_unregister(int32_t handle);
/* Reset the hook table -- test fixture teardown. */
#ifdef KERNEL_TESTS
void nt_audit_reset_for_test(void);
#endif

/* ---- transition_ring_record runtime gate ---------------------------------- *
 * Default off ("cheap"); the syscall entry asm skips transition_ring_record
 * when this byte is 0. Toggled by nt_audit when tracing is enabled. */
extern volatile uint8_t g_transition_ring_active;
void transition_ring_set_enabled(int on);
