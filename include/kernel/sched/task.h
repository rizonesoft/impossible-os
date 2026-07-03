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
#include "kernel/sched/apc.h"     /* KAPC_STATE per-thread APC queues */
#include "kernel/sched/spinlock.h"
#include "kernel/ipc/signal.h"
#include "kernel/ob/handle_table.h"

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
#define THREAD_STACK_SIZE 8192       /* 8 KiB per thread stack */
#define TASK_STACK_SIZE  8192        /* 8 KiB per kernel task stack */
#define USER_STACK_SIZE  16384       /* 16 KiB per user task stack */
#define SCHED_QUANTUM    5           /* ticks per time slice (50ms at 100Hz) */

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
    uint8_t     in_audit;           /* 1 while this thread is running a syscall-audit hook;
                                     * per-thread (migration-safe) recursion guard so a hook's
                                     * own syscall is not itself re-audited. Zero-init. */
};

/* Task Control Block */
struct task {
    uint32_t    pid;            /* process ID */
    uint32_t    state;          /* TASK_RUNNING, TASK_READY, etc. */
    uint64_t    rsp;            /* saved stack pointer (interrupt frame) */
    uint8_t    *stack_base;     /* base of kernel stack (for kfree) */
    uint64_t    kernel_rsp;     /* top of kernel stack (for TSS rsp0) */
    uint8_t    *user_stack_base;/* base of user stack (NULL for kernel tasks) */
    const char *name;           /* human-readable name */
    uint32_t    parent_pid;     /* PID of parent (0 for init) */
    int32_t     exit_status;    /* exit code (set on TASK_DEAD) */
    int32_t     wait_pid;       /* PID we're waiting on (-1 = none) */
    /* exec_pending state machine (bulletproofing):
     *   task_exec():  exec_pending = 1, exec_pending_tick = uptime()
     *   schedule():   exec_pending = 0 on switch-in (frame consumed)
     *   Stuck:        if exec_pending && (uptime() - exec_pending_tick) > 10 -> force-clear + WARN
     * A stuck exec_pending means the task was never scheduled in -- frame is lost. */
    uint32_t    exec_pending;       /* 1 = exec'd frame pending, skip save on switch-out */
    uint64_t    exec_pending_tick;  /* tick when exec_pending was set (0 = not pending) */
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
};

/* Task entry function type */
typedef void (*task_entry_t)(void);

/* --- API --- */

/* Initialize the scheduler (makes the current execution context PID 0).
 * Returns BOOT_OK on success. */
boot_result_t task_init(void);

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

/* Get total number of tasks (including dead ones). */
uint32_t task_count(void);

/* Get a task by PID. Returns NULL if invalid. */
struct task *task_get_by_pid(uint32_t pid);

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

/* Exec: load an ELF binary and replace the current task's code.
 * 'data' is the raw ELF file, 'size' is its length.
 * Returns 0 on success, -1 on failure. */
int task_exec(const uint8_t *data, uint64_t size);

/* Exit the current task with a status code.
 * Wakes any parent waiting via waitpid. */
void task_exit(int32_t status);

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
