/* ============================================================================
 * task.h -- Kernel task/process and thread management
 *
 * Provides kernel and user-mode threads with cooperative and preemptive
 * scheduling. Supports fork, exec, waitpid, process cleanup, and
 * per-task kernel threads.
 *
 * Design:
 *   - Round-robin scheduler, cooperative + preemptive
 *   - Per-task kernel stack (8 KiB) + optional user stack (16 KiB)
 *   - Per-task thread list (threads share PID/address space)
 *   - Context switch via interrupt frame swapping
 *   - PID 0 = idle/main thread (uses the boot stack)
 *   - Tasks that return from their entry function are marked DEAD
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"
#include "kernel/atomic.h"     /* atomic_t utest_capture_seq */
#include "kernel/sched/apc.h"     /* KAPC_STATE per-thread APC queues */
#include "kernel/sched/spinlock.h"
#include "kernel/sched/mutex.h"   /* mutex_t environ_lock (env is thread-context only) */
#include "kernel/ipc/signal.h"
#include "kernel/ob/handle_table.h"
#include "kernel/task_limits.h"   /* rlimit_t, RLIM_NLIMITS, RLIMIT_* */
#include "kernel/quota/quota.h"   /* quota_block_t, quota_absorb_record_t */
#include "kernel/quota/quota_policy.h" /* quota_policy_t (ProcessQuotaLimits) */
#include "kernel/sched/syscall.h" /* FAULT_SITE_* ids for the site-targeted
                                   * fault-injection helpers below. syscall.h
                                   * pulls only kernel/types.h, so this adds
                                   * no include cycle. */

/* ---- Site-targeted fault-injection allocator tags ----
 * Declared above struct task because the arm array below is sized by
 * FI_ALLOC_COUNT. Each allocator that participates in site targeting owns
 * its own arm slot, mirroring the per-allocator ordinal countdowns, so
 * arming one allocator's program never disturbs another's. */
#define FI_ALLOC_KMALLOC  ((uint32_t)FAULT_ALLOC_KMALLOC)
#define FI_ALLOC_PMM      ((uint32_t)FAULT_ALLOC_PMM)
#define FI_ALLOC_COUNT    2u   /* number of participating allocators */
#define FI_ALLOC_SHIFT    24u  /* site ids occupy the low 24 bits   */

/* Tags are 1-based so 0 stays "no allocator"; the array is 0-based. */
#define FI_ARM_SLOT(alloc)       ((uint32_t)(alloc) - 1u)
#define FI_ARM_PACK(alloc, site) \
    ((((uint32_t)(alloc)) << FI_ALLOC_SHIFT) | ((uint32_t)(site)))

/* Forward declaration: kernel-mode SEH (KI_TRY/KI_EXCEPT) registration chain
 * head lives in struct thread. Full type in kernel/except.h; forward-declared
 * here so task.h stays free of the exception-ABI include. */
struct ki_exception_registration;

/* Task states */
#define TASK_RUNNING    0   /* currently on the CPU */
#define TASK_READY      1   /* runnable, waiting for scheduler */
#define TASK_DEAD       2   /* finished, awaiting cleanup */
#define TASK_BLOCKED    3   /* waiting on I/O or event */
#define TASK_WAITING    4   /* blocked on waitpid */

/* Thread states */
#define THREAD_RUNNING  0
#define THREAD_READY    1
#define THREAD_DEAD     2
#define THREAD_BLOCKED  3   /* blocked on join */
#define THREAD_FREE     4   /* slot reaped by thread_join, safe to reuse */

/* Limits */
#define TASK_MAX         32          /* max concurrent tasks */
#define THREAD_MAX       16          /* max threads per task */
/* Per-process current-directory cap. Pinned equal to VFS_MAX_PATH by a
 * _Static_assert in task.c so cwd strings never truncate at the VFS boundary. */
#define TASK_CWD_MAX     512
#define THREAD_STACK_SIZE 8192       /* 8 KiB per thread stack */
#define TASK_STACK_SIZE  8192        /* 8 KiB per kernel task stack */
#define USER_STACK_SIZE  16384       /* 16 KiB per user task stack */
#define SCHED_QUANTUM    5           /* ticks per time slice (50ms at 100Hz) */

/* ---- Ring-3 test harness self-report (SYS_TEST_REPORT, syscall 48) ----
 *
 * A test binary's exit code carries exactly two outcomes (0 = no failures,
 * 77 = the whole binary was skippable), so a binary that skipped ONE of its
 * sub-tests and passed the rest had nowhere to say so: the launcher classed
 * it PASS with zero skips in TAP / JUnit XML / JSON while the serial log
 * showed the [SKIP] line. This record is the third outcome's channel.
 *
 * SKIP BLOCKS, NOT SUB-TESTS: one UTEST_SKIP guards a block that may contain
 * several assertions, so `skip_blocks` counts skip SITES taken, and is never
 * comparable to the assertion counts beside it. The two are separate
 * dimensions and the artifacts report them as such.
 *
 * FAIL-CLOSED: every field is ring-3 supplied. `state` is the kernel's own
 * verdict on the submission and is the only field a consumer may trust
 * before checking; INVALID means the binary contradicted itself and the
 * launcher escalates it to FAIL rather than dropping the counts silently.
 * NONE (the zero value, so a fresh or recycled slot degrades correctly) is
 * the legacy exit-code-only path every non-reporting binary stays on. */
#define TASK_UTEST_REPORT_NONE    0u  /* never submitted -- legacy behaviour  */
#define TASK_UTEST_REPORT_VALID   1u  /* submitted once, within bounds        */
#define TASK_UTEST_REPORT_INVALID 2u  /* contradicted itself -- escalate FAIL */
/* Transient: one submitter has CLAIMED the single-submission slot with an
 * atomic compare-exchange and is filling the counts. Any second submitter
 * loses that exchange and invalidates the record, so "exactly once" holds
 * even with several user threads in the same process -- a check-then-set
 * would let two threads both observe NONE and let the last writer win,
 * quietly replacing a skip-bearing report with a zero-skip one. A record
 * still reading CLAIMED at reap means the submitter died mid-write; the
 * launcher treats that as INVALID, fail-closed. */
#define TASK_UTEST_REPORT_CLAIMED 3u
/* Per-binary ceiling on any single reported count. A real binary reports
 * tens to low thousands of assertions; this bound exists so a malicious or
 * corrupt count cannot wrap the launcher's 32-bit aggregates. Three counts
 * per binary, TASK_MAX binaries, all at the cap still sum well inside
 * uint32_t (3 * 32 * 1e6 < 2^32), which is what makes the aggregation safe
 * without per-add overflow checks. */
#define TASK_UTEST_REPORT_MAX     1000000u
/* Separate, far smaller ceiling on the SKIP-BLOCK count, because that one
 * is not merely an aggregate -- the launcher emits a TAP point, a JUnit
 * <testcase> and a JSON record per skip block, so this count governs
 * ARTIFACT FAN-OUT, not just arithmetic. At the assertion ceiling above a
 * single binary could demand three million log records, exhaust the run's
 * wall clock and push every later binary's result off the end of the log:
 * a denial of service dressed up as honest reporting. Real binaries take a
 * handful of skip sites; 256 is already generous. Over the bound the
 * report is REFUSED, never clamped -- a clamped count is a wrong number
 * that still looks like a measurement. */
#define TASK_UTEST_REPORT_SKIP_MAX 256u

/* KERNEL_TESTS-only, and the guard is load-bearing rather than tidy: the
 * only consumer is the user-mode test launcher, which is itself
 * test-flavor. Left unguarded, the record added 16 bytes to every one of
 * the TASK_MAX task slots in a PRODUCTION kernel and shifted the slot
 * stride modulo the cache line, so adjacent slots stopped sharing line
 * phase -- a real layout cost for a facility a release build can never
 * use, and a direct contradiction of this section's own claim that the
 * release flavor pays nothing. */
#ifdef KERNEL_TESTS
struct task_utest_report {
    uint32_t asserts_passed;  /* UTEST_ASSERT calls that held    */
    uint32_t asserts_failed;  /* UTEST_ASSERT calls that did not */
    uint32_t skip_blocks;     /* UTEST_SKIP sites taken          */
    uint32_t state;           /* TASK_UTEST_REPORT_*             */
};
#endif /* KERNEL_TESTS */

/* Load-bearing coupling, not a restatement of the value: a fresh or recycled
 * task slot is zero-filled, and that zero MUST read as "never reported" so an
 * uninitialized TCB degrades to the legacy path instead of presenting a
 * zero-count VALID report the launcher would believe. */
_Static_assert(TASK_UTEST_REPORT_NONE == 0,
               "a zeroed TCB must read as never-reported");
/* Mutual distinctness is load-bearing, not cosmetic: the two
 * compare-exchanges in the dispatcher and the CLAIMED-to-INVALID mapping
 * in the launcher all key off these four values differing. Pinned at
 * COMPILE time rather than by a runtime suite -- a test comparing
 * constants to each other cannot fail unless someone edits both halves,
 * so it verifies nothing while inflating the assertion count. */
_Static_assert(TASK_UTEST_REPORT_VALID != TASK_UTEST_REPORT_NONE &&
               TASK_UTEST_REPORT_INVALID != TASK_UTEST_REPORT_NONE &&
               TASK_UTEST_REPORT_VALID != TASK_UTEST_REPORT_INVALID &&
               TASK_UTEST_REPORT_CLAIMED != TASK_UTEST_REPORT_NONE &&
               TASK_UTEST_REPORT_CLAIMED != TASK_UTEST_REPORT_VALID &&
               TASK_UTEST_REPORT_CLAIMED != TASK_UTEST_REPORT_INVALID,
               "the four report states must be mutually distinguishable");
_Static_assert(TASK_UTEST_REPORT_SKIP_MAX < TASK_UTEST_REPORT_MAX,
               "the artifact-fan-out bound must be tighter than the wrap bound");

/* Reset to the legacy (never-reported) state. Called by every task
 * constructor: task slots are monotonic today, but each constructor already
 * scrubs the fields a recycled slot could otherwise inherit, and a report is
 * per-process -- a fork must NOT inherit the parent's submission, or the
 * child's own UTEST_END would read as the forbidden second call. */
#ifdef KERNEL_TESTS
static inline void task_utest_report_reset(struct task_utest_report *r)
{
    r->asserts_passed = 0;
    r->asserts_failed = 0;
    r->skip_blocks    = 0;
    r->state          = TASK_UTEST_REPORT_NONE;
}
/* Constructors call THIS, never the helper directly: in the release
 * flavor the member does not exist, so `&t->utest_report` would not even
 * compile. The macro keeps every constructor reading identically in both
 * flavors while the release build resolves it to nothing. */
#define TASK_UTEST_REPORT_RESET(tp) task_utest_report_reset(&(tp)->utest_report)
#else
#define TASK_UTEST_REPORT_RESET(tp) ((void)0)
#endif /* KERNEL_TESTS */

/* Exec argument (argv/envp) ingestion caps for the exec argument-handoff
 * feature. These are EARLY sanity bounds; the BINDING limit is that the exact
 * serialized argv frame must fit USER_STACK_SIZE (enforced by argv_frame_bytes()
 * in the SYS_EXEC path) -- a single ARG_STRING_MAX string cannot actually fit a
 * 16 KiB stack and is rejected there. ARG_ARGC_MAX is bounded so the kernel-side
 * temporary argv-address array (kmalloc'd, argc*8 bytes) stays <= 4 KiB. */
#define ARG_ARGC_MAX     511u        /* max argv entries: (n+1)*sizeof(char*) stays
                                      * <= 4 KiB so the pointer array + the SYS_EXEC
                                      * snapshot array are single kmalloc slots (same
                                      * bound + rationale as ENV_MAX_ENTRIES). */
#define ARG_STRING_MAX   4096u       /* max single argv/envp string bytes incl NUL (one
                                      * kmalloc snapshot slot; also caps a runaway scan
                                      * for a missing NUL. A single exec arg this large is
                                      * already beyond any realistic use, and the exact
                                      * argv frame must fit the 16 KiB user stack anyway). */
#define ARG_MAX          262144u     /* max aggregate argv+envp bytes (Linux-parity ceiling;
                                      * the binding argv limit is the 16 KiB frame check). */
/* Fixed user-stack overhead the argv frame needs beyond argv_frame_bytes() (which
 * already accounts for the string-area + argc parity pads). This is EXACTLY the
 * builder's fixed block: AT_RANDOM (16 B) + auxv (16 pairs * 16 B = 256 B) + the
 * envp NULL terminator (8 B) = 280 B. It must equal that exactly: a larger value
 * wrongly rejects a maximal argv frame that fits USER_STACK_SIZE; a smaller one
 * risks a stack overwrite. Keep in lockstep with the auxv pair count in the
 * task_exec frame builder. SYS_EXEC and the builder both add it to argv_frame_bytes. */
#define ARGV_FRAME_RESERVE 280u

/* Per-thread user stack layout for secondary threads (created by uthread_create).
 * The MAIN thread's user stack is at USER_ELF_END - USER_STACK_SIZE (0x8FC000).
 * Secondary thread stacks are placed below the TEB region, striding downward
 * by USER_STACK_SIZE per tid:
 *   0x7FFCB000 = lowest TEB (tid 15)
 *   0x7FFCA000 = USER_THREAD_STACK_BASE (just below TEB region)
 *   tid 1: [0x7FFC6000 .. 0x7FFCA000)  (16 KiB)
 *   tid 2: [0x7FFC2000 .. 0x7FFC6000)
 *   ...
 *   tid 15: [0x7FF8E000 .. 0x7FF92000) */
#define USER_THREAD_STACK_BASE  0x7FFCA000ULL

/* Lowest user thread stack VA: USER_THREAD_STACK_BASE - THREAD_MAX * USER_STACK_SIZE.
 * Must not overlap with: USER_ELF range (0x800000-0x900000), main user stack
 * (0x8FC000-0x900000), or TEB region (0x7FFCB000-0x7FFDB000). */
#define USER_THREAD_STACK_LOWEST \
    (USER_THREAD_STACK_BASE - (uint64_t)THREAD_MAX * USER_STACK_SIZE)

_Static_assert(USER_THREAD_STACK_BASE < 0x7FFCB000ULL,
    "USER_THREAD_STACK_BASE must be below TEB region (0x7FFCB000)");
_Static_assert(USER_THREAD_STACK_LOWEST < USER_THREAD_STACK_BASE,
    "USER_THREAD_STACK_LOWEST must be below BASE (not wrapped)");
_Static_assert(USER_THREAD_STACK_LOWEST > 0x1000000ULL,
    "USER_THREAD_STACK_LOWEST must be well above kernel region");

/* Thread priority range.
 * Higher value = higher priority.  Default = THREAD_PRIO_NORMAL.
 * The scheduler always picks the highest-priority READY thread.
 * Priority inheritance temporarily boosts a lock owner to match its
 * highest-priority waiter; the original is stored in base_priority. */
#define THREAD_PRIO_IDLE     0   /* idle / background tasks only */
#define THREAD_PRIO_LOW      8   /* below-normal */
#define THREAD_PRIO_NORMAL  16   /* default for all new threads */
#define THREAD_PRIO_HIGH    24   /* above-normal */
#define THREAD_PRIO_REALTIME 31  /* top -- interrupt-like priority */

/* Saved CPU context (callee-saved registers only for cooperative switch) */
struct task_context {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;       /* return address (where to resume) */
};

/* Thread entry function type */
typedef void (*thread_entry_t)(void *arg);

/* Thread Control Block -- schedulable unit within a task */
struct thread {
    uint32_t    id;             /* thread ID (unique within parent task) */
    uint32_t    state;          /* THREAD_RUNNING, THREAD_READY, etc. */
    uint64_t    rsp;            /* saved stack pointer (interrupt frame) */
    uint8_t    *stack_base;     /* base of thread stack (for kfree) */
    uint32_t    stack_size;     /* allocated stack size in bytes */
    uint32_t    parent_task;    /* index into tasks[] (owning task) */
    int32_t     exit_status;    /* exit status (set on THREAD_DEAD) */
    int32_t     join_tid;       /* thread we're waiting on (-1 = none) */
    uint8_t     pledge_pending; /* a fine handler-side pledge check on THIS thread
                                 * flagged a violation; the dispatch tail terminates
                                 * on this (not the NTSTATUS value, which an audit
                                 * hook may also return). Per-thread so a sibling's
                                 * tail cannot consume this thread's provenance. */
    /* --- Suspend / resume --- */
    uint32_t    suspend_count;  /* >0 = suspended (NtSuspendThread/NtResumeThread) */
    /* --- Priority (for priority-aware scheduler and PI) --- */
    uint32_t    priority;       /* current effective priority (may be boosted) */
    uint32_t    base_priority;  /* original priority before any boost */
    /* --- Per-thread kernel stack (for TSS.rsp0 on ring-3 threads) --- */
    uint64_t    kernel_rsp;     /* top of this thread's kernel stack; 0 = kernel thread (no rsp0 switch) */
    uint8_t    *kernel_stack_base;  /* PMM-allocated base (for pmm_free_frame at join); NULL for kmalloc'd stacks */
    uint32_t    kernel_stack_pages; /* number of PMM pages; 0 = stack is kmalloc'd (use kfree on stack_base) */
    /* --- Per-thread user stack (for ring-3 threads created by uthread_create) --- */
    uintptr_t   user_stack_va;  /* user-space VA of this thread's stack; 0 = kernel thread */
    uint32_t    user_stack_pages; /* number of PMM pages mapped for user stack; 0 = none */
    uint32_t    _ustack_pad;    /* alignment padding */
    /* --- Per-thread TEB (for ring-3 threads; Win32 requires one TEB per thread) --- */
    void       *teb;            /* TEB * in user address space; NULL for kernel threads */
    uint64_t    kernel_gs_base; /* MSR 0xC0000102 value for this thread's TEB; 0 for kernel threads */
    /* --- APC state (kernel/sched/apc.h) --- */
    KAPC_STATE  apc_state;          /* per-thread APC queues + pending flags */
    KAPC_STATE  saved_apc_state;    /* swap target for a future KeStackAttachProcess (field only;
                                     * the attach API itself is deferred -- needs CR3 switching) */
    int32_t     kernel_apc_disable; /* critical-region nesting: blocks NORMAL kernel APCs */
    int32_t     special_apc_disable;/* guarded-region nesting: blocks ALL kernel APCs */
    spinlock_t  apc_lock;           /* guards apc_state queues + the DEAD/FREE insert race */
    /* --- NT previous mode (SSDT probe gating) --- */
    uint32_t    previous_mode;      /* 0 = KernelMode, 1 = UserMode; set at syscall entry.
                                     * Per-thread so a Zw kernel call on one CPU cannot clear
                                     * another CPU's user-syscall probe flag. Zero-init (a fresh
                                     * kernel thread defaults to KernelMode). */
    uint32_t    in_system_service;  /* 1 while a user-originated system service runs on this
                                     * thread: set at the true ring-3 -> ring-0 syscall entry
                                     * (SYSCALL fast path + INT 0x2E), cleared at exit. A plain
                                     * store, not a ++/-- RMW -- a thread runs one syscall at a
                                     * time (kernel code never issues SYSCALL/INT 0x2E), so the
                                     * flag never nests, and the store matches previous_mode's SMP
                                     * profile exactly (same global-cursor caveat, no lost-update
                                     * risk). Distinct from previous_mode, which zw_dispatch forces
                                     * to KernelMode for nested Zw calls -- this flag is NOT touched
                                     * by Zw, so a fault inside a nested Zw within a user syscall
                                     * still reads 1. Consumed by ki_kernel_bugcheck_code() to pick
                                     * 0x3B SYSTEM_SERVICE_EXCEPTION vs 0x1E. Reset to 0 at every
                                     * slot reuse (with previous_mode). Zero-init. */
    uint8_t     in_audit;           /* 1 while this thread is running a syscall-audit hook;
                                     * per-thread (migration-safe) recursion guard so a hook's
                                     * own syscall is not itself re-audited. Zero-init. */
#ifdef KERNEL_TESTS
    uint32_t    fault_site_current; /* FAULT_SITE_* this thread is executing right now;
                                     * FAULT_SITE_NONE when outside every annotated site.
                                     * PER-THREAD, not per-task: a task owns many
                                     * independently scheduled threads, so a task-wide
                                     * marker would let a sibling's allocation consume the
                                     * single-shot injection armed for this thread's site
                                     * (same hazard pledge_pending above is per-thread for).
                                     * Follows the thread across CPU migration by
                                     * construction. Zero-init. KERNEL_TESTS-only. */
#endif
    uint8_t     in_knf_trace;       /* 1 while this thread is running the KNF publish
                                     * observability bridge; recursion guard so a klog/ETW
                                     * path that re-publishes a KNF state cannot recurse.
                                     * Acquired/released via atomic exchange in knf.c: the
                                     * global scheduler cursor can alias one thread across
                                     * CPUs, so a plain set/clear could tear. Zero-init. */
    /* --- Kernel-mode SEH (KI_TRY/KI_EXCEPT) --- */
    struct ki_exception_registration *kernel_exception_list; /* newest-first chain of active
                                     * KI_TRY registrations on THIS thread's kernel stack; NULL =
                                     * no guard active. Follows the thread across CPU migration.
                                     * ki_raise_kernel_exception walks it in fault context after
                                     * snapshotting thread_current() once and validating the trap
                                     * RSP against this thread's stack bounds. Zero-init; reset to
                                     * NULL on thread-slot (re)creation (a leaked node would point
                                     * at freed stack). Mutated only by the owning thread. */
    /* --- Impersonation (SRM token assignment) --- */
    void       *impersonation_token; /* ACCESS_TOKEN *; thread-level override, NULL = use the
                                      * owning task's primary token. Swapped only by the current
                                      * thread (ImpersonateSelf/RevertToSelf) via __atomic exchange
                                      * so a future effective-token reader never sees a torn ptr.
                                      * MUST be reset to NULL on thread-slot (re)creation -- a stale
                                      * pointer from a prior tenant would silently impersonate. */
};

/* --- Process accounting: job baseline and coherent sampling ---------------
 * Both structs carry the SAME cumulative metrics in the SAME order, because a
 * baseline is nothing but a sample taken at join time and every consumer
 * subtracts one from the other. Keeping them as two named types (rather than
 * one aliased everywhere) is what keeps `job_acct_base` from being mistaken for
 * a live reading -- the subtraction is the whole point of the field. */
struct task_acct_base {
    uint64_t user_time_ns;
    uint64_t kernel_time_ns;
    uint64_t io_read_count;
    uint64_t io_read_bytes;
    uint64_t io_write_count;
    uint64_t io_write_bytes;
    uint64_t io_other_count;
    uint64_t io_other_bytes;
    /* Carried here too, so a membership-interval delta covers EVERY metric a
     * direct sample reports. Omitting them made the two views disagree: a job
     * could report its members' CPU and I/O while silently losing their wakeup
     * and timer activity, which is exactly the battery/health signal the
     * section exists to feed. */
    uint64_t wakeup_count;
    uint64_t timer_create_count;
};

/* One process's cumulative metrics read under a SINGLE timestamp, so a policy
 * consumer can divide two samples into a rate itself. The kernel stores no
 * rate and no window: see the metric block in struct task for why a shared
 * window record would let two consumers corrupt each other's baseline.
 *
 * NOT a coherent cross-counter snapshot: the counters are independent RELAXED
 * atomics, so a sample taken while the task runs can straddle an update. That
 * is the same contract the counters themselves carry, and it is sufficient for
 * rate derivation, where a one-event skew is absorbed by the next sample. */
typedef struct task_acct_sample {
    uint64_t timestamp_ns;          /* uptime_ns() at the moment of the read */
    uint64_t user_time_ns;
    uint64_t kernel_time_ns;
    uint64_t io_read_count;
    uint64_t io_read_bytes;
    uint64_t io_write_count;
    uint64_t io_write_bytes;
    uint64_t io_other_count;
    uint64_t io_other_bytes;
    uint64_t wakeup_count;
    uint64_t timer_create_count;
} task_acct_sample_t;

/* Task Control Block */
struct task {
    uint32_t    pid;            /* process ID */
    uint32_t    state;          /* TASK_RUNNING, TASK_READY, etc. */
    uint64_t    rsp;            /* saved stack pointer (interrupt frame) */
    uint8_t    *stack_base;     /* base of kernel stack (for kfree) */
    /* Superseded exec kernel stack awaiting reclamation.
     *
     * task_exec() cannot free the stack it replaces: its own caller
     * (exec_loader_func / the syscall path) is still executing on that stack
     * and keeps using it until the task iretqs to ring 3. The old base is
     * parked here instead and drained at the two points where the task is
     * provably no longer on it -- the START of the task's NEXT task_exec (it
     * had to re-enter the kernel on the NEW stack to get there) and
     * task_cleanup (the task is TASK_DEAD and off-CPU). Without this, every
     * re-exec leaked TASK_STACK_SIZE + one guard page permanently. At most one
     * stale stack per task is outstanding, and reap returns it. */
    uint8_t    *stack_pending_free;
    /* 1 = this task was produced by task_fork and therefore SHARES its
     * parent's image frames until an exec isolates it.
     *
     * task_exec decides whether to run the private-frame isolation remap by
     * looking at cr3, treating cr3 == 0 as "launcher-spawned, no parent to
     * isolate from". That inference is wrong for one case: task_fork leaves a
     * child's cr3 at 0 when vmm_create_user_pml4 runs out of memory. Such a
     * child would skip isolation entirely and let exec_load write the new
     * binary straight through the identity mappings it still shares with its
     * parent -- parent-image corruption, reachable by exhausting memory.
     * Recording the origin explicitly removes the inference: a fork child
     * without an isolated CR3 is refused an exec before anything mutates. */
    uint8_t     forked_shares_parent_image;
    uint64_t    kernel_rsp;     /* top of kernel stack (for TSS rsp0) */
    uint8_t    *user_stack_base;/* base of user stack (NULL for kernel tasks) */
    const char *name;           /* human-readable name */
    uint32_t    parent_pid;     /* PID of parent (0 for init) */
    /* --- POSIX process group + session (job control) ---
     * pgid/sid are the process-group and session IDs. PID 0 is its own
     * session+group leader (pgid=sid=0); every other task inherits its creator's
     * pgid+sid at create/fork (a fresh new process joins the parent group until
     * it setpgid/setsid). Mutated only via pgroup_setpgid/pgroup_setsid under the
     * single job-control lock (kernel/ipc/pgroup.h); a lone getter read is a
     * plain aligned load (no tearing). has_execed is a monotonic flag set once
     * after a successful task_exec (atomic RELEASE); pgroup_setpgid ACQUIRE-loads
     * it and returns EACCES for an already-exec'd child, per POSIX. */
    uint32_t    pgid;           /* process-group ID */
    uint32_t    sid;            /* session ID */
    uint8_t     has_execed;     /* 1 after first successful exec (setpgid -> EACCES) */
    int32_t     exit_status;    /* exit code (set on TASK_DEAD) */
    int32_t     wait_pid;       /* PID we're waiting on (-1 = none) */
    /* --- Current working directory (process-wide, shared by all threads) ---
     * Canonical absolute path "X:\\...". Read/written only via task_get_cwd /
     * task_set_cwd, which snapshot/commit under cwd_lock so a concurrent
     * SetCurrentDirectory on one thread can never be observed half-written by
     * another (the whole struct is shared across threads[]). */
    char        cwd[TASK_CWD_MAX];
    spinlock_t  cwd_lock;
    /* Serializes a SetCurrentDirectory COMMIT (the hidden "=X:" drive-cwd env
     * update + the task cwd write) as one transaction so two threads changing to
     * the same drive concurrently cannot commit different winners to cwd vs "=X:"
     * (TODO-22 s12 adversarial finding). A SLEEPING mutex, not a spinlock: the env
     * update allocates. Lock order: chdir_lock (outer) -> environ_lock (inside
     * env_set) -> cwd_lock (inside task_set_cwd); the two inner locks are never
     * held simultaneously. Taken ONLY on the chdir path. */
    mutex_t     chdir_lock;
    /* --- Per-process environment + arguments (env.h API) ---
     * environ: NULL-terminated "KEY=VALUE" UTF-8 array; argv: NULL-terminated
     * argument array. Both are process-wide (shared by all threads[]) and are
     * mutated/read ONLY via the env.h API (env_get_copy/env_set/env_unset/
     * env_copy/env_free) or task_set_argv, which serialize on environ_lock.
     * environ_lock is a MUTEX, not a spinlock: env mutation allocates/frees
     * heap+PMM (forbidden under a spinlock) and env is thread-context-only
     * (never touched from an ISR). Freed at the task_cleanup reap barrier. */
    char      **environ;        /* "KEY=VALUE" strings, or NULL */
    uint32_t    environ_count;  /* live entries (excludes NULL terminator) */
    uint32_t    environ_bytes;  /* cached SUM(strlen(entry)+1) under environ_lock; O(1) block cap */
    char      **argv;           /* argument strings, or NULL until argv setup */
    int         argc;
    mutex_t     environ_lock;
    /* exec_pending state machine (bulletproofing):
     *   task_exec():  exec_pending = 1, exec_pending_tick = uptime()
     *   schedule():   exec_pending = 0 on switch-in (frame consumed)
     *   Stuck:        if exec_pending && (uptime() - exec_pending_tick) > 10 -> force-clear + WARN
     * A stuck exec_pending means the task was never scheduled in -- frame is lost. */
    uint32_t    exec_pending;       /* 1 = exec'd frame pending, skip save on switch-out */
    uint64_t    exec_pending_tick;  /* tick when exec_pending was set (0 = not pending) */
    /* --- Process accounting: times, I/O, context switches ---
     * Statistical tick accounting: the timer-tick ISR charges one tick quantum
     * to user_time_ns or kernel_time_ns by the interrupted ring (CS & 3). I/O
     * counters are bumped in the handle-based read/write path; ctxsw counters
     * in the two scheduler switch paths. All plain uint64_t written with
     * __atomic RELAXED -- independent monotonic counters, single-writer on the
     * single-CPU BSP today, RELAXED-in-spirit for the future SMP scheduler (no
     * multi-field snapshot coherence is required, so no acquire/release).
     * create_time_filetime is an absolute FILETIME captured ONCE at creation
     * (stable across a later KeSetSystemTime/NTP step); create_time_ns is the
     * monotonic creation stamp. Per-THREAD ctxsw/CPU-time + the /sys/sched view
     * are owned by the per-thread scheduler-stats surface (which aggregates, not
     * duplicates, these per-process totals); VM/fault counters are owned by the
     * VMM per-process memory-counter surface. */
    uint64_t    create_time_ns;      /* uptime_ns() at task_create() */
    uint64_t    create_time_filetime;/* absolute FILETIME at creation, or 0 if unsourced */
    uint64_t    user_time_ns;        /* ring-3 ticks charged (statistical) */
    uint64_t    kernel_time_ns;      /* ring-0 ticks charged (statistical) */
    uint64_t    io_read_count;       /* handle/legacy file read operations */
    uint64_t    io_read_bytes;       /* bytes read via handle/legacy file path */
    uint64_t    io_write_count;      /* handle file write operations */
    uint64_t    io_write_bytes;      /* bytes written via handle file path */
    uint64_t    vol_ctxsw;           /* voluntary switches (yield/block) */
    uint64_t    invol_ctxsw;         /* involuntary switches (preempt) */
    /* --- Control I/O, wakeup, and timer-creation metrics ---
     * Same RELAXED monotonic discipline as the counters above. Each metric has
     * exactly ONE canonical event at ONE instrumentation layer, because the
     * same logical action is reachable from several layers and counting it at
     * more than one inflates every rate derived from it:
     *   io_other_*         a COMPLETED device-control request, counted once at
     *                      completion with the bytes it actually transferred.
     *                      A failed or zero-byte control still counts as an OP
     *                      (it occupied the device) and contributes no bytes.
     *   wakeup_count       a thread of this task went BLOCKED -> READY because
     *                      a synchronization primitive released it. Waking an
     *                      already-runnable thread, a preemption requeue, and
     *                      thread creation are NOT wakeups: they reach READY
     *                      without a wait ever having been satisfied. Of the
     *                      17 sites assigning THREAD_READY only the 10 wait
     *                      grants go through task_wake_thread() (event x2,
     *                      semaphore, mutex, condvar x2, rwlock x2, ob_mutex,
     *                      and thread_exit's joiner release).
     *   timer_create_count a timer OBJECT was created. Arming and rearming an
     *                      existing timer are not creations.
     * No RATE is stored here. A sampler reads these through task_acct_sample()
     * under one coherent timestamp and each policy consumer keeps its own prior
     * sample; one shared window record in the kernel would let two consumers
     * reset each other's baseline and read mismatched samples. */
    uint64_t    io_other_count;      /* completed device-control operations */
    uint64_t    io_other_bytes;      /* bytes transferred by control operations */
    uint64_t    wakeup_count;        /* BLOCKED -> READY wait grants received */
    uint64_t    timer_create_count;  /* timer objects created by this process */
    /* Exception-dispatch telemetry rate gate (TODO-23 s16): packed
     * {window_ms:44, count:20}, updated by a lock-free CAS in
     * except_telem_rate_gate(). Caps per-process dispatch-telemetry events so a
     * process spraying intentional exceptions cannot flood the structured log.
     * Attribution is best-effort until per-CPU current-task lands (TODO-07). */
    uint64_t    except_telem_rate;
    uintptr_t   cr3;            /* per-process PML4 phys addr (0 = kernel PML4) */
    /* --- Per-task thread list --- */
    struct thread threads[THREAD_MAX];   /* thread pool for this task */
    uint32_t     num_threads;            /* number of threads (>= 1, thread 0 = main) */
    /* --- FPU/SIMD state (lazy XSAVE) --- */
    void       *xsave_area;     /* 64-byte-aligned XSAVE buffer; NULL = not yet allocated */
    uint8_t     fpu_used;       /* 1 = this thread has touched FP/SIMD registers */
    uint8_t     _fpu_pad[7];
    /* --- Signal state --- */
    struct signal_state signals;         /* per-task signal handlers + pending mask */
    /* --- Object Manager handle table --- */
    HANDLE_TABLE handle_table; /* per-process handle table */
    uint64_t total_handles_created; /* cumulative handle allocs, diagnostics (64-bit: no wrap) */
    /* --- Security token --- */
    void *token;                         /* ACCESS_TOKEN * (NULL until SRM assigns one) */
    /* --- Per-process syscall filter (seccomp / SystemCallDisablePolicy) ---
     * NULL = no filter (all syscalls allowed). Published/read via __atomic
     * acquire/release; superseded snapshots are chained on retired_prev and
     * the whole chain is freed at task_cleanup (the reap barrier), NOT at the
     * TASK_DEAD transition -- consistent with how the kernel stacks/CR3 are
     * reclaimed. MUST be explicitly reset to NULL on task-slot (re)creation --
     * a stale pointer from a prior tenant would be a use-after-free AND a
     * sandbox escape. syscall_filter_counted tracks whether this task still
     * contributes to g_syscall_filter_count, so the count can drop at
     * TASK_DEAD (stops taxing the global fast path) while the memory free
     * waits for the reap barrier -- and neither step double-counts. */
    struct syscall_filter *syscall_filter;
    uint8_t syscall_filter_counted;
    /* --- Per-process mitigation policy (SetProcessMitigationPolicy /
     * prctl-style hardening) --- monotonic bitmask: bits are only ever set,
     * never cleared, for a process's lifetime. Set via __atomic_fetch_or
     * (ACQ_REL) and read via __atomic_load_n (ACQUIRE) through the
     * task_mitigation_* accessors; a plain |= would drop a concurrent set on
     * SMP. Zeroed on slot (re)creation like syscall_filter; inherited from the
     * parent at task_fork (a spawn via NtCreateProcess starts fresh). See
     * include/kernel/nt/mitigation_policy.h for the MIT_* bit definitions. */
    uint64_t mitigation_flags;
    /* --- SearchPathW current-directory ordering policy (SetSearchPathMode) ---
     * Holds the applied BASE_SEARCH_PATH_* bits that decide whether SearchPathW
     * consults the current directory before or after the system directories, and
     * whether the choice is permanent (locked). 0 = unset -> default SAFE ordering
     * (CWD searched after PATH). Updated ONLY via SetSearchPathMode's __atomic CAS
     * loop (a plain store would drop a concurrent set and could downgrade a
     * PERMANENT enable on SMP); read via an ACQUIRE load. Zeroed on slot
     * (re)creation and reset to the safe default at fork (an unsafe CWD-first
     * ordering must never be inherited). See include/kernel/env_searchpath.h. */
    uint32_t search_path_mode;
    /* --- OpenBSD-style process restriction (pledge / unveil) ---
     * pledge_mask holds ALLOWED syscall-category bits with a sentinel (bit 63)
     * marking "has pledged"; 0 = never pledged (all allowed). Tighten-only via a
     * CAS intersection (see pledge_apply) -- a plain |= would EXPAND privilege.
     * unveil_list is a leaf-lock-guarded, append-only allowlist of folded path
     * prefixes; NULL = full filesystem visible. Both are zeroed on slot
     * (re)creation and inherited fail-closed at fork BEFORE the child is
     * published. Freed at the reap barrier (pledge_unveil_teardown). See
     * include/kernel/nt/pledge.h. */
    uint64_t pledge_mask;
    struct unveil_entry *unveil_list;
    uint8_t  unveil_locked;
    uint8_t  unveil_active;      /* set on the first unveil_add OR unveil_lock: the
                                  * task is now filesystem-restricted, so an ACTIVE
                                  * but EMPTY list (lock-before-add) denies all --
                                  * enforcement gates on THIS, not unveil_list != NULL */
    uint32_t unveil_gen;         /* bumped under unveil_lock on every add/replace;
                                  * lets a lock-free clone detect concurrent mutation */
    spinlock_t unveil_lock;
    /* --- Job Object membership (Win32 Job Objects) ---
     * NULL = not in a job. When set, this task holds ONE Ob reference on the
     * JOB_OBJECT body (so the job outlives the member); the job's member_pids
     * array holds this task's pid. Set only under the job's own spinlock by
     * ob_job assign / fork-inherit; cleared (and the Ob ref dropped) by
     * ob_job_detach_task from EVERY process-death path. Zeroed on slot
     * (re)creation like the pledge fields. Typed struct pointer (not void*) so
     * the compiler enforces the lifetime contract. See kernel/ob/ob_job.h.
     *
     * job_lock serializes ALL reads/writes of `job` (assign, detach, fork
     * inherit) so the raw pointer is never freed out from under a concurrent
     * user. Lock order is job_lock -> the JOB_OBJECT's own spinlock (never the
     * reverse). A task holding its membership reference keeps the job alive
     * until its own detach drops it under job_lock, so no assignment can free
     * the job while a detacher is mid-flight. Zero-init (SPINLOCK_INIT) is an
     * unlocked lock. */
    struct job_object *job;
    spinlock_t job_lock;
    /* What joining `job` folded from this task's usage into the job's quota
     * block (kernel/quota/quota.h). Published with the membership and consumed
     * by the detach, both under job_lock, so exactly one detacher withdraws it.
     *
     * It must be RECORDED rather than recomputed: only this amount is owned by
     * the membership. Post-join charges reached the job through chain receipts
     * and are returned by those receipts, so withdrawing the task's current
     * usage at detach would return them twice and corrupt other members'
     * accounting. Zeroed on slot (re)creation like the job pointer. */
    struct quota_absorb_record job_absorb;
    /* Cumulative accounting values captured when this task JOINED `job`, so the
     * job's CPU/I/O aggregate covers what its members did WHILE ASSOCIATED.
     *
     * Without a baseline the aggregate is the sum of member LIFETIME totals, so
     * a process that burned an hour of CPU and was then assigned to a job would
     * hand that hour to the job retroactively -- and a job used to measure a
     * workload would report time nobody spent in it. Subtracting the baseline
     * makes both halves of the lifetime consistent: a live member contributes
     * (current - base) at query time and a departing one folds exactly the same
     * delta into the persistent accumulators.
     *
     * Published under job->lock in the SAME critical section that appends the
     * pid to member_pids[], so a collector walking that array under the same
     * lock can never observe a member whose baseline is not yet set. Consumed
     * by the detach that claims the membership.
     *
     * Deliberately NOT reset on slot (re)creation, unlike `job` and
     * `job_absorb`: it is meaningless without a membership and is overwritten
     * by every path that creates one (ob_job_assign, and fork inheritance,
     * which routes through it). A reused slot's stale value is therefore never
     * read -- and zeroing it would suggest the field means something when no
     * membership exists, which is exactly what it must not be read as. */
    struct task_acct_base job_acct_base;
    /* --- Resource quota accounting (kernel/quota/quota.h) ---
     * `quota` is this process's own accounting block; `quota_user` is the
     * canonical block shared by every process running as the same owner SID,
     * so a per-user budget cannot be multiplied by opening more processes.
     * The task holds ONE reference on each.
     *
     * quota_lock guards BOTH pointers, and guarding them is not a formality: a
     * charger must load a pointer AND acquire its reference inside this lock,
     * because teardown clears and dereferences under the same lock. A bare
     * release-store would order the publication but would not stop a reader
     * that already loaded the pointer from referencing a block the final
     * dereference has since freed. The pair is written and cleared together,
     * so both are read in one critical section. Zero-init (SPINLOCK_INIT) is
     * an unlocked lock; NULL blocks are normal before quota_task_init and
     * after quota_task_teardown. */
    struct quota_block *quota;
    struct quota_block *quota_user;
    spinlock_t quota_lock;
    /* Charge gate: {state, in-flight charger count} packed in one word, read and
     * written ONLY through the quota_gate_* helpers in kernel/quota/quota_ledger.h
     * (see the state machine documented there). Every chain charge enters it, so
     * a job-membership transition can drain chargers and publish membership
     * atomically with respect to charging. Zero-init decodes as OPEN with nothing
     * in flight, which is exactly what a fresh slot must mean; task death
     * publishes SEALED and a reused slot is re-zeroed like every other field.
     *
     * It is deliberately NOT inside the ledger below: a gate reachable only
     * through a ledger would be bypassed by every charger that keeps its receipt
     * in its own structure, and it would force the charge path to allocate. */
    atomic64_t quota_gate;
    /* Outstanding-obligation ledger, or NULL until something charges through it.
     * Guarded by quota_lock like the block pair above, with ONE deliberate
     * difference: it is cleared at REAP (quota_ledger_task_release), not at death,
     * because obligations for resources that outlive the task must stay
     * returnable after quota_task_teardown has released the blocks. The ledger is
     * refcounted, so an obligation outliving even the reap keeps its own storage
     * alive. */
    struct quota_ledger *quota_ledger;
    /* --- User-mode ABI --- */
    uint64_t kernel_gs_base;             /* MSR 0xC0000102 value; 0 for kernel tasks */
    void *peb;                           /* PEB * in user address space (NULL for kernel tasks) */
    void *teb;                           /* TEB * in user address space (NULL for kernel tasks) */
    uint64_t tls_bitmap;                 /* per-process TLS bitmap: bit N = slot N allocated */
    uint64_t tls_expansion_bitmap[16];   /* 1024 expansion slots (indices 64-1087) */
    uint8_t  tls_expansion_allocated;    /* 1 if expansion array has been demand-allocated */
    uintptr_t tls_expansion_phys;        /* physical base of expansion pages (for free) */
    uintptr_t tls_expansion_virt;        /* virtual base of expansion pages (for unmap) */
    /* --- ELF auxv --- */
    void    *user_auxv;                  /* user-space VA of first AT_TYPE qword (0 if none) */
    uint32_t user_auxv_pairs;            /* number of (type, value) pairs including AT_NULL */
    uint32_t _auxv_pad;
    /* --- Loaded binary format name (set by task_exec) ---
     * Points at a static string from the exec format registry (e.g.
     * "ELF", "PE32+", "EIF"). Read by the user-mode launcher
     * (test_usermode.c) to surface which loader picked the binary
     * in the `UTEST: <name>: format=<fmt>` per-binary log line,
     * so a regression that silently routes PE binaries through the
     * ELF loader surfaces even when the binary itself exits 0.
     * NULL until the first successful task_exec on this task. */
    const char *loaded_format;
    /* --- Ring-3 test harness self-report (SYS_TEST_REPORT) ---
     * What the binary says it did: assertions passed/failed and how many
     * skip BLOCKS it took (one UTEST_SKIP guards a block that may contain
     * several assertions, so this is never an assertion count). Claimed
     * with an atomic compare-exchange and published with a second one, so
     * exactly one submission per image wins even with several user
     * threads; read by the user-mode launcher (test_usermode.c) with a
     * paired acquire, after the task is dead and before task_cleanup.
     *
     * Every value is ring-3 supplied and therefore untrusted: `state`
     * carries the fail-closed verdict (see TASK_UTEST_REPORT_*) and the
     * launcher escalates INVALID to a FAIL rather than trusting counts.
     *
     * KERNEL_TESTS-only, member AND type: a release kernel has no
     * launcher to read it, and carrying it there cost 16 bytes on every
     * one of the TASK_MAX slots plus a shift in slot-to-cache-line phase. */
#ifdef KERNEL_TESTS
    struct task_utest_report utest_report;
    /* Source-level per-binary stdout capture ownership. Set once at
     * spawn (task_create_captured), inherited unchanged across fork() --
     * the OPPOSITE of utest_report above, which is per-process and reset
     * on every constructor. utest_capture_seq is meaningful ONLY in the
     * OWNER's own slot (tasks[utest_capture_owner_pid]): every task in a
     * fork tree sharing one owner draws from THAT slot's counter rather
     * than its own, giving an O(1) unique sequence number across the
     * whole tree with no parentage walk.
     *
     * The draw is made under s_capture_budget_lock (test_usermode.c), not
     * by a bare atomic_fetch_add as it was before the producer emission
     * budget shipped: the sequence number, the budget verdict and the
     * run-wide charge have to be ONE decision, or a descendant can draw a
     * number after another CPU has already published the stream's
     * terminator. It stays an atomic_t because readers outside that lock
     * (tests, diagnostics) still load it without taking it.
     *
     * IT IS NOT A PHYSICAL-ORDER GUARANTEE. The values are unique and
     * monotonically ASSIGNED, but two tasks sharing one owner (a fork
     * descendant, or a second thread) can reach this counter
     * concurrently, and klog releases its ring lock before the serial
     * write, so the order records land on the wire can differ from
     * their seq order. Every consumer MUST reassemble by sorting on
     * seq, never by position in the stream; the host reconciler
     * (scripts/utest-capture.py) does exactly that. An earlier version
     * of this comment claimed emission order was preserved as well,
     * which would invite a future consumer to undo that rule.
     *
     * The per-write escape/chunk staging buffer deliberately does NOT
     * live here: it is a local (stack) variable inside the syscall's own
     * write loop (struct utest_capture_ctx, test_usermode.h), scoped to
     * ONE call on ONE thread's own stack. Chunking was only ever meant
     * to span one write() call (each call gets its own final=1
     * terminator, never coalesced with a later call), so nothing needs
     * to persist here between calls -- and a stack-local buffer needs no
     * lock at all, closing both the intra-task multi-writer buffer race
     * and the cross-thread chunk-ordering race an earlier per-task
     * buffer design had.
     *
     * Reading task_current() to decide "is THIS task captured" carries a
     * pre-existing, project-wide limitation this feature does not close:
     * task_current()/thread_current() resolve through a GLOBAL scheduler
     * cursor, not per-CPU state (documented at ssdt.c:36-41 for the
     * identical previous_mode lookup). On real multi-CPU AP scheduling,
     * CPU A's task_current() call could race CPU B's context switch and
     * return CPU B's struct task *, misattributing bytes to the wrong
     * owner. Every other task_current()-dependent kernel behavior has
     * this same exposure; closing it needs the per-CPU current-task
     * cursor work tracked in the SMP Phase 2 per-CPU run queues TODO
     * (03-memory-concurrency/TODO-07) -- out of scope here for the same
     * reason ssdt.c accepted it rather than fixing it inline. */
    uint8_t  utest_capture_active;
    uint32_t utest_capture_owner_pid;
    atomic_t utest_capture_seq;
    /* Producer-side emission-budget latch. Like utest_capture_seq it is
     * meaningful ONLY in the OWNER's slot, and like it, a fork descendant
     * charges the OWNER's latch rather than its own. Once set, this owner
     * has emitted its one [UTEST-CAPTURE-OVER] marker and every later
     * chunk is dropped, so an abusive binary's wire cost stops growing.
     *
     * The COMPOUND decision is what s_capture_budget_lock (test_usermode.c)
     * protects: latch, sequence draw and run-wide charge are linearized
     * together, so the overflow marker is always the owner's highest
     * emitted sequence number. Making the field atomic alone would leave
     * each access individually safe and the DECISION racy -- which is the
     * failure the lock exists to close, and why the lock is not optional.
     *
     * EVERY access is nonetheless atomic, because the lock does not
     * synchronize the one reader that runs outside it: the emitter's fast
     * path loads this in test_usermode_capture_start to enter discard mode
     * without paying an acquisition on every well-behaved write. That read
     * is deliberately lock-free -- the value is monotonic within a run, so
     * a stale 0 only costs one more trip through the locked claim -- but
     * monotonicity is an argument about VALUES and cannot license a data
     * race, so the load pairs with a release store rather than reading a
     * plain byte another CPU may be writing. The remaining access,
     * task_utest_capture_reset, runs before the slot is published. */
    uint8_t  utest_capture_stopped;
#endif
    /* --- User-mode section-view VA bump allocator ---
     * Per-task bump pointer for MapViewOfSection / sys_shmem_map. The
     * task's private user address space has a dedicated range starting
     * at SECTION_VIEW_BASE (0x10000000) that is NOT in the USER_ELF
     * region -- mapping a shmem frame there installs User-bit PTEs
     * independent of the image range. Each view bumps the pointer by
     * its page-rounded size; no reclaim on unmap (simple, no
     * fragmentation, bounded by SECTION_VIEW_LIMIT). Resets on
     * task_exec (fresh process image starts fresh). */
    uintptr_t next_section_view_va;      /* 0 = uninitialized, >0 = next free VA */
    /* --- Per-process resource limits (POSIX rlimit model) ---
     * Process-wide, shared by every thread in threads[]. Read/written ONLY via
     * task_rlimit_get / task_rlimit_set, which snapshot/commit under rlimit_lock
     * (irqsave, because a future RLIMIT_CPU enforcer runs in the timer-tick ISR
     * that already touches this struct). rlimit_lock is a leaf lock: never held
     * across another lock or any blocking op. Indexed by the RLIMIT_* ABI values
     * in kernel/task_limits.h; PID 0 gets defaults, every other task inherits its
     * creator's array at create/fork and preserves it across exec. */
    rlimit_t    rlimits[RLIM_NLIMITS];
    spinlock_t  rlimit_lock;
    /* --- Windows quota policy (kernel/quota/quota_policy.h) ---
     * The ProcessQuotaLimits fields that have no other home: working-set
     * bounds, pagefile limit, flags, CPU rate word. Pool limits live on the
     * quota block and TimeLimit lives in rlimits[RLIMIT_CPU]; this record is
     * only the remainder, so no limit is stored in two places.
     *
     * quota_policy_lock is a SPINLOCK: it serializes the whole
     * ProcessQuotaLimits query/commit transaction, and nothing under it can
     * block -- rlimit_lock and the quota block's lock are themselves irqsave
     * spinlocks over a bounded number of atomic stores. A mutex would have
     * been the natural fit for a "long" transaction, but mutex_unlock clears
     * `locked` before the owner fields (src/kernel/sched/mutex.c), so a
     * contended SMP handoff can strand the new owner -- a security-relevant
     * transaction must not depend on that. LOCK ORDER: quota_policy_lock ->
     * {quota_lock, rlimit_lock, quota block lock}; never the reverse.
     * Initialized once per slot; the VALUES are reset by quota_policy_reset()
     * when a slot is reused, so a recycled PID never inherits the dead
     * process's limits. */
    quota_policy_t quota_policy;
    spinlock_t     quota_policy_lock;
#ifdef KERNEL_TESTS
    /* Site-targeted fault-injection arm (FAULT_KMALLOC_SITE / FAULT_PMM_SITE).
     *
     * Packed as FI_ARM_PACK(allocator_tag, FAULT_SITE_*); 0 = disarmed. The
     * COMPLETE arm program lives in this one word on the TASK, never split
     * with per_cpu_data: the ordinal countdowns are per-CPU and therefore
     * carry the documented "a migrating task does not trigger" caveat
     * (include/kernel/smp.h), which a named-site selector must not inherit
     * -- a site test that silently stops firing after a migration would
     * certify a branch it never exercised.
     *
     * Single shot: the allocator gate consumes it with a compare-exchange,
     * so exactly one allocation can fire even when sibling threads on other
     * CPUs reach the same site simultaneously. Reset on slot reuse.
     *
     * INDEXED BY ALLOCATOR (FI_ARM_SLOT) because the ordinal countdowns it
     * sits beside are themselves per-allocator: one shared word would make
     * arming a kmalloc program silently delete a live pmm one. */
    uint32_t    fault_site_arm[FI_ALLOC_COUNT];
#endif
};

/* Task entry function type */
typedef void (*task_entry_t)(void);

#ifdef KERNEL_TESTS
/* Reset the capture fields to "not captured" -- called by EVERY task
 * constructor exactly like TASK_UTEST_REPORT_RESET, so a fresh or recycled
 * slot never inherits a prior tenant's owner pid or a stale
 * partially-filled staging buffer. task_create_captured() (task.c) is the
 * only caller that arms capture afterward, and it does so BEFORE the new
 * task is published (num_tasks++), so a task cannot be selected by the
 * scheduler on another CPU and run with capture_active still unset. */
static inline void task_utest_capture_reset(struct task *t)
{
    t->utest_capture_active = 0;
    t->utest_capture_owner_pid = 0;
    atomic_set(&t->utest_capture_seq, 0);
    /* Cleared for the same reason the sequence counter is: a recycled slot
     * whose prior tenant had exhausted its budget would otherwise start
     * already latched and emit nothing at all, which reaches the host as a
     * binary that never wrote -- a silent false green, not a bounded stop.
     *
     * Atomic like every other access to this field. This one runs before
     * the slot is published, so nothing can observe it concurrently today;
     * it is written this way because slot reuse is planned, and 1 -> 0 is
     * the ONE transition that could let the lock-free fast path observe a
     * stale latch and silently discard a new tenant's output. */
    __atomic_store_n(&t->utest_capture_stopped, 0, __ATOMIC_RELEASE);
}
#define TASK_UTEST_CAPTURE_RESET(tp) task_utest_capture_reset(tp)

/* Inherit capture ownership from parent to child unchanged -- the OPPOSITE
 * of task_utest_capture_reset above (which the child's slot already got
 * from its own constructor): a fork()'d descendant's writes belong to the
 * same test binary as its parent, not to a new owner of its own.
 * utest_capture_seq is deliberately left alone (task_utest_capture_reset
 * already zeroed it): only the OWNER's own slot is ever read, so a
 * descendant's own copy is never consulted. Extracted as its own function
 * (rather than left inline in task_fork) so it is unit-testable on two
 * plain struct task fixtures without a live scheduler. */
static inline void task_utest_capture_inherit(struct task *child,
                                              const struct task *parent)
{
    child->utest_capture_active = parent->utest_capture_active;
    child->utest_capture_owner_pid = parent->utest_capture_owner_pid;
}
#define TASK_UTEST_CAPTURE_INHERIT(childp, parentp) \
    task_utest_capture_inherit(childp, parentp)

/* KERNEL_TESTS-only spawn entry point: identical to task_create() except
 * the new task's capture fields are armed (active=1, owner=own pid) BEFORE
 * the task is published, closing the publish-before-arm race a plain
 * task_create() plus a post-hoc field-set would leave open. The only
 * caller is the user-mode test launcher (test_usermode.c u_spawn_one()). */
int task_create_captured(task_entry_t entry, const char *name);
#else
#define TASK_UTEST_CAPTURE_RESET(tp) ((void)0)
#define TASK_UTEST_CAPTURE_INHERIT(childp, parentp) ((void)0)
#endif /* KERNEL_TESTS */

/* --- API --- */

/* Initialize the scheduler (makes the current execution context PID 0).
 * Returns BOOT_OK on success. */
boot_result_t task_init(void);

/* Assign the SYSTEM primary token to PID 0 (the initial system process).
 * Called from boot_phase3 immediately after task_init(), once ObpTokenType is
 * registered and before any later kernel task is created -- descendants inherit
 * their parent's token at fork/create time, so PID 0 must own one first. */
void task_assign_initial_token(void);

/* Create a new kernel thread. Returns PID or -1 on failure. */
int task_create(task_entry_t entry, const char *name);

/* Create a new user-mode thread. Returns PID or -1 on failure.
 * The entry function runs in ring 3 with its own user stack. */
int task_create_user(task_entry_t entry, const char *name);

/* Allocate the XSAVE area for a task (lazy -- called on first FPU use). */
void task_alloc_xsave(struct task *t);

/* Voluntarily yield the CPU to the next ready task. */
void yield(void);

/* Get the currently running task. */
struct task *task_current(void);

/* Snapshot the task's current working directory into `out` (NUL-terminated,
 * bounded by out_size) under cwd_lock. `t` NULL or out_size 0 yields "". */
void task_get_cwd(struct task *t, char *out, uint32_t out_size);

/* Commit `abs` (a canonical absolute path) as the task's cwd under cwd_lock.
 * Returns 0 on success, -1 if `abs` does not fit TASK_CWD_MAX. */
int task_set_cwd(struct task *t, const char *abs);

/* Snapshot resource limit `resource` (a RLIMIT_* index) into *out under
 * rlimit_lock. Returns RLIMIT_OK, or RLIMIT_ERR_INVAL for a NULL arg or a
 * resource index outside [0, RLIM_NLIMITS) (in which case *out is zeroed). */
int task_rlimit_get(struct task *t, int resource, rlimit_t *out);

/* Validate and commit a new limit for `resource` under rlimit_lock. Policy:
 *   - nl->rlim_cur must be <= nl->rlim_max              -> RLIMIT_ERR_INVAL
 *   - RAISING the hard limit (rlim_max) above its current value requires
 *     caller_privileged != 0                            -> RLIMIT_ERR_PERM
 *   - lowering either limit is always permitted (an unprivileged lower of the
 *     hard limit is irreversible, matching getrlimit(2))
 * caller_privileged is precomputed at the syscall/NT boundary via
 * SeSinglePrivilegeCheck(&SeIncreaseQuotaPrivilege, previous_mode); passing it in
 * keeps this helper free of any security-header dependency and directly testable.
 * Returns RLIMIT_OK on commit, or a RLIMIT_ERR_* code (nothing is written on error). */
int task_rlimit_set(struct task *t, int resource, const rlimit_t *nl,
                    int caller_privileged);

/* Copy `parent`'s entire rlimit array into a fresh (unpublished) `child` slot,
 * snapshotting the parent under its rlimit_lock. Called on every process-creation
 * path so a child inherits (never resets) its creator's limits; copying all
 * RLIM_NLIMITS entries also guarantees a reused slot carries no prior tenant's
 * limits. `child` must not be schedulable yet (no lock is taken on it). */
void task_rlimit_inherit(struct task *child, struct task *parent);

/* Per-process mitigation policy (mitigation_flags). apply OR-sets bits
 * atomically (monotonic -- bits are never cleared); get is an acquire load.
 * Both are SMP-safe: a plain |= would drop a concurrent set. */
void     task_mitigation_apply(struct task *t, uint64_t add_mask);
uint64_t task_mitigation_get(struct task *t);
/* Apply a ProcessChildProcessPolicy request to t. Monotonic: a
 * NoChildProcessCreation request OR-sets MIT_NO_CHILD_PROCESS; a request that
 * omits the bit while it is already set is a clear attempt and is refused.
 * Returns 0 on success (set, or benign leave-unset), -1 if the request would
 * clear an already-set MIT_NO_CHILD_PROCESS. Consumed by unit tests + the
 * future ring-3 setter (currently deferred); no live ring-3 caller today. */
int      task_mitigation_child_set(struct task *t, uint32_t child_flags);

/* Resolve `in` (relative or absolute) against `t`'s cwd into a canonical
 * absolute path `out`. Handles drive-relative "X:tail" / bare "X:" by consulting
 * the current drive's remembered directory: `t`'s cwd when its drive matches,
 * else the hidden "=X:" env variable, else the "X:\" root (TODO-22 s12). Returns
 * 0 on success, -1 on invalid input / overflow. Explicit-task form so the
 * resolver is unit-testable against a fixture task without task_current(). */
int task_resolve_path_for(struct task *t, const char *in, char *out,
                          uint32_t out_size);

/* Resolve `in` against the CURRENT task's cwd (task_resolve_path_for over
 * task_current()). The single entry point every NT pathname syscall uses so
 * relative paths resolve consistently. Returns 0 on success, -1 on failure. */
int task_resolve_path(const char *in, char *out, uint32_t out_size);

/* Get total number of tasks (including dead ones). */
uint32_t task_count(void);

/* Get a task by PID. Returns NULL if invalid. */
struct task *task_get_by_pid(uint32_t pid);

/* --- Process accounting metrics (CPU / I/O / wakeup / timer) -------------
 * The instrumentation seams for the metrics documented on struct task. Each is
 * the SINGLE canonical counting point for its event; adding a second caller at
 * another layer double-counts and silently inflates every derived rate.
 *
 * All three are lock-free RELAXED atomic increments and take no allocation, no
 * lock, and no log, so they are safe in interrupt context and beneath a caller's
 * spinlock -- which they must be, since the wake grant is counted while the
 * waking primitive still holds its own lock. A NULL task is a no-op, so a
 * counting site never has to pre-validate a lookup that may have raced a death.
 */

/* Count one COMPLETED read / write of `bytes` bytes. Call at EVERY successful
 * completion regardless of what backs the handle -- file, pipe, or device --
 * because these counters describe the PROCESS's I/O, not one subsystem's.
 * Missing a backing kind does not merely lose those events: it makes the whole
 * per-process and per-job breakdown wrong for any workload that uses it (a
 * pipe-heavy process reporting zero I/O). A zero or negative transfer is not a
 * completion and must not be passed here. */
void task_acct_note_read_io(struct task *t, uint64_t bytes);
void task_acct_note_write_io(struct task *t, uint64_t bytes);

/* Count one COMPLETED device-control operation and the bytes it moved. Call
 * once at completion, never at submission: a control that fails after issue
 * still occupied the device (so it counts as an op) but moved nothing. */
void task_acct_note_control_io(struct task *t, uint64_t bytes);

/* Count one wait grant: `prev_state` is the thread's state BEFORE it was set to
 * THREAD_READY, and only THREAD_BLOCKED counts. Taking the previous state as an
 * argument (rather than reading it here) keeps the check on the caller's side of
 * its own lock, where the transition is atomic with respect to other wakers. */
void task_acct_note_wakeup(struct task *t, uint32_t prev_state);

/* Count one timer OBJECT creation. Not called on arm or rearm. */
void task_acct_note_timer_create(struct task *t);

/* THE wait-grant seam: make `thread_idx` of `t` runnable and count the wakeup
 * iff THIS caller is the one that actually took it out of BLOCKED. Every
 * synchronization primitive that releases a waiter (event, semaphore, mutex,
 * condvar, rwlock, object-manager mutant) goes through here, so the transition
 * and its accounting cannot drift apart -- an open-coded `state = THREAD_READY`
 * at a wait-grant site is a counting bug.
 *
 * Sites that reach READY WITHOUT satisfying a wait -- thread creation and the
 * scheduler's preemption requeue -- deliberately do NOT call this: they assign
 * the state directly, because no waiter was granted anything.
 *
 * THE TRANSITION IS CLAIMED WITH A CAS, AND A LOST CLAIM CHANGES NOTHING. None
 * of these primitives serializes its wake path under a spinlock (event,
 * semaphore, mutex, condvar, and rwlock have no lock at all), so two CPUs
 * waking the same waiter would both observe THREAD_BLOCKED under a plain
 * read-then-write. The compare-exchange makes exactly one of them the winner.
 *
 * A FAILED CAS MUST NOT STORE. The open-coded assignments this replaced set
 * THREAD_READY unconditionally, which is only safe while nothing else can move
 * the thread concurrently: once a winner exists, the loser's store would race
 * the scheduler and could push a thread that has already advanced to
 * THREAD_RUNNING back to THREAD_READY -- scheduling it twice -- or resurrect
 * one that reached THREAD_DEAD/THREAD_FREE. Only a BLOCKED thread is waiting
 * for a grant, so a non-BLOCKED target means there is nothing to grant.
 *
 * KNOWN PRE-EXISTING RACE, NOT introduced or worsened here: the wait paths
 * publish a waiter into the primitive's queue BEFORE setting THREAD_BLOCKED, so
 * a waker running in that window finds the thread not yet blocked. This seam
 * does nothing (the wake is lost); the unconditional store it replaced also
 * lost it, because the waiter's own THREAD_BLOCKED store immediately overwrote
 * the READY the waker had just written. Closing it needs a per-primitive wait
 * lock covering predicate, state, and queue as one transaction -- owned by the
 * scheduler's wait/wake transaction-locking work, not by accounting
 * instrumentation. Do not read the CAS as proof the window is closed. */
static inline void task_wake_thread(struct task *t, uint32_t thread_idx)
{
    uint32_t expected = THREAD_BLOCKED;

    if (!t || thread_idx >= t->num_threads)
        return;

    if (__atomic_compare_exchange_n(&t->threads[thread_idx].state, &expected,
                                    (uint32_t)THREAD_READY, 0 /* strong */,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        task_acct_note_wakeup(t, THREAD_BLOCKED);   /* sole winner counts */
}

/* Read every cumulative metric under one timestamp. `out` is fully written (or
 * fully zeroed when `t` is NULL) so a caller never derives a rate from a
 * partially filled sample. */
void task_acct_sample(const struct task *t, task_acct_sample_t *out);

/* Capture the join-time baseline used for job membership-interval accounting.
 * The caller must hold the lock under which the membership is published, so the
 * baseline is visible to any collector that can see the membership. */
void task_acct_capture_base(const struct task *t, struct task_acct_base *out);

/* Cumulative metrics minus a baseline: what the task accumulated since the
 * baseline was captured. Saturates at zero rather than wrapping, so a counter
 * that was reset underneath a live baseline reports no contribution instead of
 * an astronomically large one. */
void task_acct_delta_since(const struct task *t, const struct task_acct_base *base,
                           struct task_acct_base *out);

/* The pure arithmetic behind task_acct_delta_since, split out so the
 * field-by-field subtraction can be exercised against synthetic values without
 * a live task. Eight independent saturating subtractions is exactly the shape
 * where a copy/paste field swap hides, and a test driving a real task can only
 * move the fields it can provoke. `base` may be NULL (the whole of `now`). */
void task_acct_delta_fields(const struct task_acct_base *now,
                            const struct task_acct_base *base,
                            struct task_acct_base *out);

/* Enable/disable preemptive scheduling. */
void scheduler_enable(void);
void scheduler_disable(void);

/* Called from PIT IRQ handler -- preemptive round-robin.
 * Returns the (possibly new) interrupt frame pointer to restore. */
struct interrupt_frame;
uint64_t schedule(struct interrupt_frame *frame);

/* --- Process lifecycle --- */

/* Fork the current task. Returns child PID to parent, 0 to child, -1 on error.
 * 'frame' is the parent's interrupt frame (from the syscall). */
int task_fork(struct interrupt_frame *frame);

/* Caller-owned staging-buffer ownership token for task_exec().
 *
 * task_exec's caller reads the binary into a staging buffer and owns it. On a
 * SUCCESSFUL exec the caller never runs again: publication makes the new ring-3
 * frame scheduler-visible and re-enables interrupts, so a tick can switch the
 * task into the new image before task_exec even returns, and the caller's free
 * is simply never reached. SYS_EXEC escapes that only by accident of running
 * with IF=0 (INT 0x80 is an interrupt gate); every kernel-launcher caller runs
 * in ordinary thread context with interrupts enabled and does race it.
 *
 * The token closes that by handing task_exec an allocator-specific release it
 * performs itself, after its last read of 'data' and BEFORE publication -- the
 * one point where the buffer is provably dead and the caller is provably still
 * running. 'release' is invoked at most once and the token clears itself first,
 * so task_exec's release and the caller's own task_exec_staging_release() after
 * the call cannot double-free: exactly one of them does the work on every path.
 *
 * The two shapes in tree: kmalloc (ptr) and pmm_alloc_contiguous (phys+pages).
 * A caller with nothing to release passes NULL. */
struct task_exec_staging;
typedef void (*task_exec_release_fn)(struct task_exec_staging *st);

struct task_exec_staging {
    task_exec_release_fn release;  /* cleared once consumed; NULL = nothing owed */
    void      *ptr;                /* kmalloc shape: buffer to kfree */
    uintptr_t  phys;               /* pmm shape: base physical address */
    uint32_t   pages;              /* pmm shape: frame count */
};

/* Release the staging buffer if it has not been released yet. Idempotent by
 * construction: the release hook is cleared BEFORE it is invoked, so a second
 * call (or a caller that always calls it after task_exec, which is the intended
 * shape) is a no-op. NULL token is a no-op. */
void task_exec_staging_release(struct task_exec_staging *st);

/* Release hook for the kmalloc shape -- kfree(st->ptr). Shared by the three
 * kmalloc-based callers so the shape exists once, not three times. */
void task_exec_staging_kfree(struct task_exec_staging *st);

/* Exec: load an ELF binary and replace the current task's code.
 * 'data' is the raw ELF file, 'size' is its length.
 * 'staging' is the caller's ownership token for 'data' (see above); it may be
 * NULL when the caller owns nothing. On SUCCESS task_exec releases it just
 * before publication; on every failure return it is left untouched and the
 * caller still owns the buffer (its image is intact, or destroyed with the
 * caller running on its own kernel stack -- either way it can still free).
 * Callers must call task_exec_staging_release() after task_exec regardless of
 * the result; it is a no-op when task_exec already released.
 *
 * THREE outcomes, and a caller that only tests for -1 is wrong:
 *   0                          success. The task is published; on the syscall
 *                              path route the return through
 *                              task_exec_take_pending_frame().
 *   -1                         pre-commit refusal. The image is INTACT and the
 *                              caller may report the error normally.
 *   TASK_EXEC_IMAGE_DESTROYED  the image is GONE (past the commit point). The
 *                              caller MUST release its staging and then
 *                              task_exit(TASK_EXIT_EXEC_IMAGE_DESTROYED); it
 *                              must never return toward ring 3. */
int task_exec(const uint8_t *data, uint64_t size,
              struct task_exec_staging *staging);

/* Adopt a pending exec frame on the syscall-return path.
 *
 * task_exec() publishes the new ring-3 frame and sets exec_pending, expecting
 * the next context switch to adopt it. A synchronous SYS_EXEC has no such
 * switch guaranteed, so its handler MUST route its return through this helper:
 * otherwise, when no tick lands in the window, the stub iretqs back to the
 * pre-exec RIP over the freshly loaded image and the task executes garbage.
 *
 * Returns the frame pointer the ISR stub should iretq from -- the exec frame
 * when one is pending, otherwise 'frame' unchanged. Safe (and a no-op) to call
 * from any syscall-return path; only the exec'ing task can see its own flag. */
uint64_t task_exec_take_pending_frame(struct interrupt_frame *frame);

/* Exit the current task with a status code.
 * Wakes any parent waiting via waitpid. */
/* task_exec() return value meaning "the image is GONE".
 *
 * Past task_exec's commit point the calling task's image has been replaced
 * with zeroed private frames, so there is nothing to resume: the task MUST NOT
 * reach ring 3 again. task_exec cannot simply call task_exit() itself, because
 * every caller still owns the staging buffer it read the binary into and frees
 * it only after task_exec returns -- terminating inside would strand that
 * buffer on every failed exec, which a loop of failing fork+execs turns into
 * an unbounded kernel-memory leak.
 *
 * So the contract is explicit instead: on this value the KERNEL caller (which
 * is running on its own kernel stack, not ring 3 -- returning to it is safe)
 * must release its buffer and then call task_exit(). Returning to ring 3 with
 * this value is a bug. A plain -1 keeps its old meaning: the exec failed
 * before the commit point and the task's image is intact, so the caller may
 * report the error normally. */
#define TASK_EXEC_IMAGE_DESTROYED (-2)

/* ---- Reserved exit statuses for KERNEL-ORIGINATED terminations -----------
 *
 * A parent reading waitpid gets a raw int32, so every producer of an exit
 * status shares one number space. Two regions are already spoken for: signal
 * deaths use -(signum) (`signal.c`, so -1 through -64), and ordinary
 * applications return small non-negative codes. A kernel-originated
 * termination that wants to NAME its cause must therefore sit outside both, or
 * it is not a cause at all -- the first attempt at this used -2, which is
 * exactly SIGINT.
 *
 * Reserve a block well below the signal range. Add new reasons here, never as
 * a bare literal at the call site. */
#define TASK_EXIT_REASON_BASE           (-1000)

/* Layer 1: the reserved block must stay clear of the signal range, which is
 * what -(signum) consumes. This invariant has already failed once unprotected
 * (the first attempt used -2, i.e. SIGINT), so it is pinned rather than
 * described. SIG_MAX is the largest signal number signal.c can negate. */
_Static_assert(TASK_EXIT_REASON_BASE < -(int)SIG_MAX,
               "kernel exit-reason block overlaps the -(signum) range");

/* The task was terminated because a post-commit exec destroyed its image: past
 * task_exec's commit point there is nothing to return to, so the task is exited
 * rather than handed back a failure. Lets a parent distinguish "the exec commit
 * point killed my child" from a kill, a signal, or the child's own exit -- the
 * distinction the post-commit lifecycle test depends on for an honest oracle. */
#define TASK_EXIT_EXEC_IMAGE_DESTROYED  (TASK_EXIT_REASON_BASE - 1)

/* The usermode test launcher's loader refused to execute a planned binary
 * because the bytes it read no longer matched the content identity the
 * enumeration plan froze for that entry. Named here rather than in the test
 * header for the reason this block exists: the first version used -7, which
 * is exactly -(SIGBUS-range signum), so a replaced binary and a signal death
 * were indistinguishable to any consumer classifying by status value. */
#define TASK_EXIT_UTEST_IDENTITY        (TASK_EXIT_REASON_BASE - 2)

#ifdef KERNEL_TESTS
/* Test seam over the internal kernel-stack free helper, so the reclamation
 * test exercises the production allocator discrimination and the
 * uninstall-guard-before-PMM-free ordering rather than a copy of them.
 * Takes the STACK base (guard page sits at base - 4096), matching what
 * task.stack_base / task.stack_pending_free hold. */
void task_test_free_kernel_stack(uint8_t *stack_base);
#endif

void task_exit(int32_t status);

/* Centralized remote-death transition for killing ANOTHER task (never the
 * caller). Idempotent: a no-op if the target is already TASK_DEAD. Performs the
 * full teardown that a remote kill must do without running the target's
 * task_exit -- mark TASK_DEAD + exit status, drop the syscall-filter count,
 * mark the OB process object dead, and detach any Job Object membership. Used
 * by NtTerminateProcess and NtTerminateJobObject; the target notices the state
 * at its next kernel entry (coordinated cross-CPU teardown is tracked
 * separately). Safe to call at elevated IRQL (takes only irqsave locks). */
void task_terminate_remote(struct task *t, int32_t exit_code);

/* Release every resource reclaimable at a process's DEAD transition, shared by
 * ALL death paths (explicit exit, normal entry return, remote kill, fatal
 * signal): drop the syscall-filter count, mark the OB process object dead,
 * detach any Job Object membership, and reap any leaked timer-resolution
 * request. Idempotent per sub-call. The caller owns the TASK_DEAD state store;
 * this only frees resources that do NOT need the off-CPU reap barrier (stacks,
 * CR3, PEB/TEB are freed later in task_cleanup). Log-free by contract -- callers
 * may run at raised IRQL where klog could block on a disk flush. */
void task_death_teardown(struct task *t);

/* Wait for a child task to exit. Returns exit status, or -1 on error. */
int32_t task_waitpid(uint32_t child_pid);

/* Clean up a dead task's resources (free stacks). */
void task_cleanup(uint32_t pid);

/* --- Thread API --- */

/* Create a new kernel-mode thread within the current task.
 * Builds a ring-0 interrupt frame (CS=GDT_KERNEL_CODE, SS=GDT_KERNEL_DATA).
 * Returns thread ID (>= 1) or -1 on failure.
 * Thread shares PID and address space with parent task. */
int kthread_create(thread_entry_t entry, void *arg, uint32_t stack_size);

/* Create a new user-mode thread within the current task.
 * Builds a ring-3 interrupt frame (CS=GDT_USER_CODE|3, SS=GDT_USER_DATA|3).
 * Allocates per-thread kernel stack (PMM, 8 KiB + guard) and user stack
 * (PMM + vmm_map_user_page, USER_STACK_SIZE) in the parent's per-process PML4.
 * Requires the parent task to have a PEB (user task); rejects kernel tasks.
 * Returns thread ID (>= 1) or -1 on failure. */
int uthread_create(thread_entry_t entry, void *arg, uint32_t user_stack_size);

/* Legacy wrapper -- dispatches to uthread_create if task has PEB, else kthread_create.
 * Kept for back-compat with existing test code. */
int thread_create(thread_entry_t entry, void *arg, uint32_t stack_size);

/* Exit the current thread with a status code.
 * Wakes any thread blocked in thread_join(). */
void thread_exit(int32_t status);

/* Block until the specified thread exits. Returns exit status, or -1 on error. */
int32_t thread_join(uint32_t thread_id);

/* Voluntarily yield the CPU to the next ready thread.
 * (This is the same as yield() -- threads and tasks share the scheduler.) */
void thread_yield(void);

#ifdef KERNEL_TESTS
/* ---- Site-targeted fault injection (test flavor only) ----
 *
 * An ordinal countdown can only say "fail the N-th allocation", and N drifts
 * whenever any code on the path gains or loses an allocation. These helpers
 * let a test name the allocation instead. Allocation sites opt in by
 * bracketing the call with fault_site_enter()/fault_site_restore(); the
 * allocator gates ask fault_site_claim() whether THIS allocation is the
 * armed one.
 *
 * The arm word is task-owned and the current-site marker is thread-owned, so
 * the whole program survives CPU migration and no sibling thread can consume
 * another thread's injection. */
/* Mark the current thread as executing `site`; returns the previous site so
 * a nested annotation can restore it. No-op (returns FAULT_SITE_NONE) when
 * there is no current thread. */
uint32_t fault_site_enter(uint32_t site);

/* Restore the current thread's site marker to `saved`. */
void fault_site_restore(uint32_t saved);

/* Returns 1 exactly once if the calling thread's current site matches the
 * task's armed site for `alloc_tag`, atomically consuming the single shot.
 * Returns 0 otherwise -- including when no site is armed, which is what lets
 * the ordinal countdown path run unchanged. */
int fault_site_claim(uint32_t alloc_tag);

/* The allocator that OWNS a site (every site is reached through exactly
 * one), or 0 for FAULT_SITE_NONE and any out-of-range id. */
uint32_t fault_site_owner(uint32_t site);

/* Install a site arm for ONE allocator, replacing only that allocator's
 * previous site arm. Fails CLOSED: returns -1 for an invalid tag, for
 * FAULT_SITE_NONE, and for a site owned by the OTHER allocator -- all of
 * which would otherwise report success while installing nothing that could
 * ever be claimed. Use fault_site_arm_clear() to disarm. */
int fault_site_arm_set(uint32_t alloc_tag, uint32_t site);

/* Clear the calling task's site arm for ONE allocator. */
void fault_site_arm_clear(uint32_t alloc_tag);

/* Read one allocator's arm word: the packed FI_ARM_PACK value while armed,
 * 0 once an allocation has consumed it. This is the CONSUMED RECEIPT a
 * ring-3 regression needs: without it a test can only observe that some
 * operation failed, not that it failed at the site the test named -- an
 * unrelated failure would otherwise look identical to a hit. Returns 0 for
 * an invalid tag, which a caller distinguishes by having armed first. */
uint32_t fault_site_arm_peek(uint32_t alloc_tag);

/* Slot-reuse resets. Task slots and thread slots are recycled and this file
 * clears stale per-slot state field by field (there is no blanket memset),
 * so an arm left behind by a dead task would otherwise fire inside whatever
 * process next inherits the slot. Called from every create/fork path. */
static inline void fault_site_reset_task(struct task *t)
{
    uint32_t i;

    if (!t)
        return;
    for (i = 0; i < FI_ALLOC_COUNT; i++)
        __atomic_store_n(&t->fault_site_arm[i], 0u, __ATOMIC_RELEASE);
}
static inline void fault_site_reset_thread(struct thread *th)
{
    if (th)
        th->fault_site_current = FAULT_SITE_NONE;
}
#else /* !KERNEL_TESTS */
/* Release flavor: the site annotations compile to nothing, so an annotated
 * allocation site costs exactly zero instructions in a shipping kernel and
 * the call sites need no #ifdef of their own. */
static inline uint32_t fault_site_enter(uint32_t site)
{
    (void)site;
    return 0u;
}
static inline void fault_site_restore(uint32_t saved) { (void)saved; }
static inline void fault_site_reset_task(struct task *t) { (void)t; }
static inline void fault_site_reset_thread(struct thread *th) { (void)th; }
#endif /* KERNEL_TESTS */

/* Get the current thread within the current task.
 * Returns NULL if current task has no thread tracking (PID 0 boot). */
struct thread *thread_current(void);

/* Set the effective priority of a thread (0 = lowest, 31 = highest).
 * Also updates base_priority to the same value (permanent change).
 * For temporary boosts, write thread->priority directly. */
void thread_set_priority(uint32_t task_pid, uint32_t thread_id, uint32_t prio);

/* Boost a thread's effective priority if new_prio > current priority.
 * Used by mutex PI: does NOT change base_priority.
 * Returns 1 if the boost was applied, 0 if not needed. */
int thread_boost_priority(uint32_t task_pid, uint32_t thread_id, uint32_t new_prio);

/* Restore a thread's effective priority to its base_priority.
 * Called from mutex_unlock() to undo PI boosts. */
void thread_restore_priority(uint32_t task_pid, uint32_t thread_id);

/* --- TLS slot allocation (64 static + 1024 expansion) --- */

#define TLS_MINIMUM_AVAILABLE   64      /* static slots in TEB.TlsSlots[] */
#define TLS_EXPANSION_SLOTS     1024    /* expansion slots via TEB.TlsExpansionSlots */
#define TLS_MAXIMUM_AVAILABLE   1088    /* TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS */
#define TLS_EXPANSION_BITMAP_WORDS 16   /* 1024 / 64 = 16 uint64_t words */

/* Allocate a TLS slot. Returns slot index (0-1087) or -1 if all full.
 * Indices 0-63: static (TEB.TlsSlots). 64-1087: expansion (TEB.TlsExpansionSlots).
 * Expansion array demand-allocated on first index >= 64. */
int tls_alloc(uint32_t pid);

/* Free a TLS slot. Zeroes the slot in the thread's TEB. Returns 0 or -1. */
int tls_free(uint32_t pid, uint32_t index);

/* Get/Set TLS value for the current thread (kernel-side helper).
 * User-mode code reads/writes gs:[0x1480 + index*8] directly for 0-63,
 * or via TEB.TlsExpansionSlots[index - 64] for 64-1087. */
uint64_t tls_get_value(uint32_t pid, uint32_t index);
void tls_set_value(uint32_t pid, uint32_t index, uint64_t value);

/* --- Assembly (switch_context.asm) --- */

/* Switch from old_rsp to new_rsp. Saves callee-saved regs on old stack,
 * restores them from new stack, then returns into the new task. */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
