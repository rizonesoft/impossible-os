#ifndef KERNEL_TASK_LIMITS_H
#define KERNEL_TASK_LIMITS_H

/* ============================================================================
 * task_limits.h -- per-process resource limits (POSIX rlimit model)
 *
 * Owns the STORAGE and ABI numbering for per-process resource limits. The array
 * on `struct task` is the single source of truth; the locked accessors
 * (task_rlimit_get / task_rlimit_set, declared in sched/task.h) are the only
 * sanctioned way to read or mutate it.
 *
 * ABI numbering is the Linux UAPI resource order (asm-generic/resource.h) on
 * purpose: when the deferred Linux getrlimit/setrlimit/prlimit syscalls land,
 * `rlimits[resource]` indexes correctly with the userspace RLIMIT_* value and no
 * translation table is needed. RLIM_NLIMITS therefore matches Linux (16), not the
 * count of limits actively populated today.
 *
 * SCOPE: this header ships rlimit STORAGE + accessors only; the consumers below
 * are owned by other subsystems (precise cross-references live in the process-
 * model-extensions roadmap):
 *   - Enforce RLIMIT_AS       : the per-process VM / page-counter path
 *   - Enforce RLIMIT_CPU      : the scheduler timer-tick charger
 *   - Enforce RLIMIT_MEMLOCK  : the VirtualLock / VMM memory-pin path
 *   - Enforce RLIMIT_CORE     : the crash-dump writer (size gate; 0 suppresses)
 *   - RLIMIT_NOFILE reconcile : the existing handle_table.handle_limit
 *   - Windows ProcessQuotaLimits : the unified kernel quota authority
 *   - Linux get/set/prlimit   : pending a linux_syscall_table registration point
 * ========================================================================== */

#include "kernel/types.h"

/* One per-process resource limit. Invariant maintained by task_rlimit_set():
 * rlim_cur <= rlim_max (soft never exceeds hard). RLIM_INFINITY means "no
 * limit" for both fields. */
typedef struct rlimit {
    uint64_t rlim_cur;   /* soft limit -- the enforced ceiling */
    uint64_t rlim_max;   /* hard limit -- rlim_cur may never exceed this */
} rlimit_t;

#define RLIM_INFINITY  ((uint64_t)0xFFFFFFFFFFFFFFFFULL)

/* Linux UAPI resource indices (asm-generic/resource.h). Load-bearing: these are
 * the array subscripts AND the ABI values the deferred Linux syscalls will pass. */
#define RLIMIT_CPU        0   /* CPU time, seconds */
#define RLIMIT_FSIZE      1   /* maximum file size, bytes */
#define RLIMIT_DATA       2   /* data segment (brk/sbrk), bytes */
#define RLIMIT_STACK      3   /* stack size, bytes */
#define RLIMIT_CORE       4   /* core-dump size, bytes (0 = suppress) */
#define RLIMIT_RSS        5   /* resident set size, bytes */
#define RLIMIT_NPROC      6   /* number of processes for this user */
#define RLIMIT_NOFILE     7   /* open file descriptors/handles */
#define RLIMIT_MEMLOCK    8   /* locked/pinned memory, bytes */
#define RLIMIT_AS         9   /* address-space (virtual memory), bytes */
#define RLIMIT_LOCKS      10  /* file locks */
#define RLIMIT_SIGPENDING 11  /* pending signals */
#define RLIMIT_MSGQUEUE   12  /* POSIX message queue bytes */
#define RLIMIT_NICE       13  /* nice ceiling */
#define RLIMIT_RTPRIO     14  /* realtime priority ceiling */
#define RLIMIT_RTTIME     15  /* realtime CPU time, microseconds */
#define RLIM_NLIMITS      16  /* array size -- matches Linux RLIM_NLIMITS */

/* Default soft limits stamped on PID 0 only; every other task INHERITS its
 * creator's limits, so a process that lowered a hard limit cannot escape it by
 * spawning a child. Advisory today: enforcement is owned elsewhere (see the
 * SCOPE block above). Unlisted resources default to RLIM_INFINITY. */
#define RLIMIT_DEFAULT_STACK_CUR    ((uint64_t)8 * 1024 * 1024)  /* 8 MiB soft stack */
#define RLIMIT_DEFAULT_NOFILE_CUR   ((uint64_t)256)              /* soft handle cap */
#define RLIMIT_DEFAULT_NOFILE_MAX   ((uint64_t)4096)             /* hard handle cap */
#define RLIMIT_DEFAULT_MEMLOCK_CUR  ((uint64_t)8 * 1024 * 1024)  /* 8 MiB pinnable (soft) */
#define RLIMIT_DEFAULT_MEMLOCK_MAX  ((uint64_t)8 * 1024 * 1024)  /* 8 MiB finite hard ceiling */

/* task_rlimit_set() status codes (kernel-internal; NT/Linux callers translate). */
#define RLIMIT_OK          0
#define RLIMIT_ERR_INVAL  (-1)   /* bad resource index or rlim_cur > rlim_max */
#define RLIMIT_ERR_PERM   (-2)   /* unprivileged attempt to RAISE the hard limit */

#endif /* KERNEL_TASK_LIMITS_H */
