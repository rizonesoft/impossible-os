#ifndef KERNEL_NT_SYSCALL_FILTER_H
#define KERNEL_NT_SYSCALL_FILTER_H

/* ============================================================================
 * syscall_filter.h -- Per-process SSDT syscall filtering (seccomp /
 * PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY parity)
 *
 * A process can lock down which SSDT services it (or its children) may invoke
 * via a per-index allow bitmap. NULL filter = all syscalls allowed (the common
 * case, gated system-wide by g_syscall_filter_count so the dispatch fast path
 * pays a single relaxed-atomic load + branch when no filter exists anywhere).
 *
 * Lifetime model (SMP-safe without RCU): a filter snapshot is IMMUTABLE once
 * published. Installing or tightening allocates a fresh snapshot, copies the
 * old one, applies the change, and publishes it with a release store; the
 * superseded snapshot is chained on retired_prev and freed ONLY at task
 * teardown. A concurrent dispatch reader on another CPU that loaded the old
 * pointer therefore never dereferences freed memory (no reliable single-CPU
 * synchronize_rcu is available to bound reader lifetime here). A per-task
 * generation cap bounds the retire chain so a tightening loop cannot exhaust
 * memory before teardown.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/nt/ssdt.h"       /* SSDT_MAIN_MAX, SSDT_SHADOW_MAX, table ids */
#include "kernel/nt/ntstatus.h"

struct task;

/* One bit per SSDT index; bitmap sized on the full table CAPACITY (not the
 * live registered count) so a later-registered service is covered. */
#define SYSCALL_FILTER_MAIN_WORDS    (SSDT_MAIN_MAX / 64)     /* 16 */
#define SYSCALL_FILTER_SHADOW_WORDS  (SSDT_SHADOW_MAX / 64)   /* 16 */

_Static_assert(SSDT_MAIN_MAX % 64 == 0,   "main table size must be a multiple of 64");
_Static_assert(SSDT_SHADOW_MAX % 64 == 0, "shadow table size must be a multiple of 64");

/* Filter flags. */
#define SYSCALL_FILTER_INHERIT      0x1u   /* child processes inherit a copy */
#define SYSCALL_FILTER_LOCKED       0x2u   /* tighten-only: cannot be relaxed */
#define SYSCALL_FILTER_AUDIT        0x4u   /* log blocked syscalls, do not deny */
#define SYSCALL_FILTER_FLAGS_MASK   0x7u

/* Retired snapshots freed at teardown; cap the chain so a self-tightening
 * loop cannot OOM a live task (each snapshot is ~264 bytes). */
#define SYSCALL_FILTER_MAX_GENERATIONS  64

/* Immutable once published (bitmaps + flags are never mutated in place). */
typedef struct syscall_filter {
    uint64_t allow_main[SYSCALL_FILTER_MAIN_WORDS];     /* 1 = allowed, 0 = blocked */
    uint64_t allow_shadow[SYSCALL_FILTER_SHADOW_WORDS];
    uint32_t flags;
    struct syscall_filter *retired_prev;   /* older snapshot; freed at task teardown */
} SYSCALL_FILTER;

/* ProcessSystemCallFilterPolicy input buffer read by NtSetInformationProcess. */
#define SYSCALL_FILTER_OP_DISALLOW_WIN32K   1u   /* clear all shadow bits */
#define SYSCALL_FILTER_OP_DISALLOW_FSCTL    2u   /* clear NtFsControlFile bit */
#define SYSCALL_FILTER_OP_CUSTOM_BITMAP     3u   /* install caller bitmaps */

typedef struct process_syscall_filter_policy {
    uint32_t operation;    /* SYSCALL_FILTER_OP_* */
    uint32_t flags;        /* SYSCALL_FILTER_INHERIT|LOCKED|AUDIT to apply */
    uint64_t allow_main[SYSCALL_FILTER_MAIN_WORDS];    /* used by CUSTOM_BITMAP */
    uint64_t allow_shadow[SYSCALL_FILTER_SHADOW_WORDS];
} PROCESS_SYSCALL_FILTER_POLICY;

/* System-wide active-filter count. When zero, no process is filtered and the
 * dispatch fast path skips the check entirely (single atomic load). Accessed
 * only through __atomic_* ops (no `volatile`: the atomics already force the
 * codegen). ACQUIRE on the gate load pairs with the RELEASE increments so a
 * just-installed sole filter is not missed on weakly-ordered targets (ARM64).
 * On x86 TSO this pairing is free. */
extern uint32_t g_syscall_filter_count;

static inline uint32_t syscall_filter_active(void)
{
    return __atomic_load_n(&g_syscall_filter_count, __ATOMIC_ACQUIRE);
}

/* Dispatch-time check for the CURRENT task. table_id/index are the decoded
 * SSDT coordinates; service_number is for audit logging only. Returns
 * STATUS_SUCCESS if allowed (or audit-logged-and-allowed), STATUS_ACCESS_DENIED
 * if blocked. KernelMode callers (internal Zw calls) are never filtered. */
NTSTATUS syscall_filter_check(uint32_t table_id, uint32_t index,
                              uint32_t service_number);

/* Pure allow/block decision for a specific filter snapshot (no task/mode
 * resolution). Returns 1 if allowed, 0 if blocked. A NULL filter allows all.
 * The live dispatch path is syscall_filter_check; this is the testable core. */
int syscall_filter_index_allowed(const SYSCALL_FILTER *f, uint32_t table_id,
                                 uint32_t index);

/* NtSetInformationProcess(ProcessSystemCallFilterPolicy) backend. Builds a new
 * immutable snapshot from pol and publishes it on target. Enforces tighten-only
 * + frozen flags when the current filter is LOCKED. */
NTSTATUS syscall_filter_set_policy(struct task *target,
                                   const PROCESS_SYSCALL_FILTER_POLICY *pol);

/* Deep-copy a parent snapshot for an inheriting child (fresh, single-generation
 * snapshot; retired_prev = NULL). Returns NULL on allocation failure. */
SYSCALL_FILTER *syscall_filter_clone(const SYSCALL_FILTER *src);

/* Attach an inherited clone to a not-yet-runnable child and bump the count. */
void syscall_filter_attach(struct task *child, SYSCALL_FILTER *clone);

/* Drop the task's contribution to g_syscall_filter_count at the TASK_DEAD
 * transition WITHOUT freeing the snapshot memory. A dead task never dispatches
 * again, so removing it from the count stops it taxing the global fast path;
 * the memory free is deferred to the reap barrier (syscall_filter_task_teardown
 * from task_cleanup), consistent with stack/CR3 reclaim. Idempotent. */
void syscall_filter_task_dead(struct task *t);

/* Free a task's live filter and its entire retire chain; called from
 * task_cleanup (the reap barrier, after every thread is off-CPU). Also drops
 * the active-count contribution if TASK_DEAD did not already. Idempotent. */
void syscall_filter_task_teardown(struct task *t);

#endif /* KERNEL_NT_SYSCALL_FILTER_H */
