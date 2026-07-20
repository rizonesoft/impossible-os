/* quota_policy.h -- canonical per-process quota policy (ProcessQuotaLimits)
 *
 * ONE place that owns a process's Windows-shaped quota limits, so a
 * ProcessQuotaLimits query and set cannot see or leave a half-updated mixture
 * of the three stores the limits actually live in:
 *
 *   PagedPoolLimit / NonPagedPoolLimit -> the task's PROCESS quota block
 *                                         (quota_set_limit / quota_limit)
 *   TimeLimit                          -> RLIMIT_CPU (task_rlimit_get/_set)
 *   Min/MaximumWorkingSetSize,
 *   WorkingSetLimit, PagefileLimit,
 *   Flags, CpuRateLimit               -> the quota_policy_t record below
 *
 * Each store has its own lock and its own failure mode, so a naive setter can
 * publish field 3 and then fail field 4. Every mutation therefore goes through
 * quota_policy_set(), which serializes on the task's policy mutex, PRE-
 * VALIDATES the whole request (representation, ordering, flags, privilege)
 * before touching anything, and restores the pre-image if a commit step still
 * fails. A caller never observes a partial transaction.
 *
 * TOKEN-LOCAL OVERLAY: the pool limits are written to the task's own PROCESS
 * block, never to the shared USER block behind it. A restricted token can
 * therefore be capped tighter than the user block it shares without lowering
 * the unrestricted parent's budget -- and because the charge path already
 * walks process -> user -> job, the tighter of the two is what actually binds.
 *
 * IRQL: PASSIVE_LEVEL only. The transaction takes a mutex (it must be held
 * across other locks) and is reachable only from the syscall path.
 *
 * Owner: TODO-25-kernel-resource-accounting-quotas.md section 8.
 */
#ifndef _KERNEL_QUOTA_QUOTA_POLICY_H
#define _KERNEL_QUOTA_QUOTA_POLICY_H

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/quota_syscall_info.h"

struct task;

/* 100-ns ticks per second: the LARGE_INTEGER TimeLimit unit. */
#define QUOTA_TIME_100NS_PER_SEC    10000000ULL

/* Largest whole second representable in a signed 100-ns LARGE_INTEGER. A
 * request past this saturates rather than wrapping into a negative (which
 * would read back as an invalid limit). */
#define QUOTA_TIME_LIMIT_MAX_SEC    (0x7FFFFFFFFFFFFFFFLL / \
                                     (int64_t)QUOTA_TIME_100NS_PER_SEC)

/* The fields with no other home. Guarded by the task's policy mutex; zero
 * throughout means "no policy set", which reads back as unlimited. */
typedef struct quota_policy {
    uint64_t min_working_set;    /* bytes; 0 = unlimited */
    uint64_t max_working_set;    /* bytes; 0 = unlimited */
    uint64_t working_set_limit;  /* bytes; 0 = unlimited (EX only) */
    uint64_t pagefile_limit;     /* bytes; 0 = unlimited */
    uint32_t flags;              /* QUOTA_LIMITS_* subset, validated on set */
    uint32_t cpu_rate_limit;     /* RATE_QUOTA_LIMIT word (EX only) */
    uint64_t generation;         /* bumped on every committed set */
} quota_policy_t;

/* Reset a task's policy record to "nothing set". Called when a task slot is
 * (re)used, so a recycled slot never presents the dead process's limits.
 * Lock-free by contract: the slot is not published yet. */
void quota_policy_reset(struct task *t);

/* Project the task's live limits into `out`. `out` is always fully written
 * (zeroed first), so a short-circuit return never leaks stack. Returns
 * STATUS_INVALID_PARAMETER for a NULL argument.
 *
 * The projection reads real state only: pool limits come from the process
 * block, TimeLimit from RLIMIT_CPU, the rest from the policy record. Nothing
 * is fabricated -- a limit that is genuinely unset reads back as 0 (unlimited)
 * rather than as a plausible-looking default. */
NTSTATUS quota_policy_query(struct task *t, QUOTA_LIMITS_EX *out);

/* Validate and commit a complete limit set. `in` must already be a
 * kernel-owned copy (the syscall layer bounce-copies it); `is_ex` selects
 * whether the EX-only suffix (WorkingSetLimit, Reserved2..4, Flags,
 * CpuRateLimit) participates. `caller_privileged` is the result of
 * SeSinglePrivilegeCheck(&SeIncreaseQuotaPrivilege, ...) -- computed by the
 * caller, which is the only layer that knows the request's previous mode.
 *
 * Returns:
 *   STATUS_SUCCESS              committed in full
 *   STATUS_INVALID_PARAMETER    NULL arg, min > max working set, negative or
 *                               unrepresentable TimeLimit, unknown/contra-
 *                               dictory flag bits, non-zero Reserved fields,
 *                               CPU rate percent above 100
 *   STATUS_PRIVILEGE_NOT_HELD   the request RAISES a limit (including removing
 *                               a cap by writing 0) without the privilege
 *   STATUS_INSUFFICIENT_RESOURCES  the task has no quota block to write to
 * On any non-success status NOTHING has changed. */
NTSTATUS quota_policy_set(struct task *t, const QUOTA_LIMITS_EX *in,
                          int is_ex, int caller_privileged);

/* The handle count this process may actually reach, reconciling RLIMIT_NOFILE
 * with the object manager's own handle_table.handle_limit.
 *
 * The two encode "no limit" INVERSELY -- handle_limit 0 disables enforcement,
 * while RLIMIT_NOFILE 0 means deny every open -- so a naive min() would read a
 * deny-all rlimit as unlimited. This resolves the inversion explicitly and is
 * READ-ONLY: it never writes handle_limit back. Pushing the rlimit INTO the
 * handle table needs the table's reservation/commit lock, which does not exist
 * yet (TODO-05-object-manager.md section 3), so a write here could admit a limit for an entry a
 * concurrent insert overwrites.
 *
 * Returns the effective cap in handles, or QUOTA_HANDLE_LIMIT_UNLIMITED. */
uint64_t quota_policy_effective_handle_limit(struct task *t);

#define QUOTA_HANDLE_LIMIT_UNLIMITED  0xFFFFFFFFFFFFFFFFULL

/* Layer 2 of the 5-layer ABI defense: re-check at runtime what the header's
 * _Static_asserts pin at compile time, so a mismatched prebuilt object cannot
 * reach a boot that only the header was rebuilt for. Returns 1 when the ABI is
 * intact, 0 on any size/offset mismatch. */
int quota_policy_abi_verify(void);

/* Convert between RLIMIT_CPU seconds and the LARGE_INTEGER TimeLimit.
 * Exposed for the unit tests, which must pin the saturation and infinity
 * behaviour independently of the syscall path. */
int64_t  quota_policy_seconds_to_time_limit(uint64_t seconds);
uint64_t quota_policy_time_limit_to_seconds(int64_t time_limit);

#endif /* _KERNEL_QUOTA_QUOTA_POLICY_H */
