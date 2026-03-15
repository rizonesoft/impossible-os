# P0102 — Threading & Synchronization

> **Goal:** Kernel thread infrastructure, mutual exclusion, counting semaphores,
> read-write locks, and condition variables — the foundation for concurrent kernel
> tasks, IPC, and GUI app isolation.
>
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Kernel Threads ✅

**Prompt:** This section is marked complete. Verify the implementation is correct and consistent: review `src/kernel/sched.c` and `include/task.h` to confirm `thread_t` struct has id, stack_ptr, stack_base, stack_size, state, parent_task fields, that `thread_create`, `thread_exit`, `thread_join`, `thread_yield` all exist and work, and that the scheduler iterates threads. Run `bash scripts/build.sh clean` and test two threads sharing a global variable. Fix any inconsistencies in the TODO items below. Confirm the commit `"sched: kernel threads"` exists in git history. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to threading. Add notes, gotchas, and design decisions directly in this TODO section covering the threading model, thread API, and scheduler architecture.


- [x] Define `thread_t` struct (id, stack_ptr, stack_base, stack_size, state, parent_task)
- [x] Implement `thread_create(entry, arg, stack_size)` — allocate stack, init context
- [x] Implement `thread_exit(status)` — clean up, notify joiners
- [x] Implement `thread_join(thread)` — block until target thread exits
- [x] Implement `thread_yield()` — voluntary context switch to next thread
- [x] Add per-task thread list (linked list of `thread_t` within `struct task`)
- [x] Update scheduler to schedule threads (not just tasks)
- [x] Test: two threads in one process sharing globals
- [x] Commit: `"sched: kernel threads"`

---

## 2. Mutexes ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: review that `mutex_t` struct, `mutex_lock`, `mutex_unlock`, `mutex_trylock` exist and function correctly. Confirm the lock uses compare-and-swap with a wait queue (not busy spinning). Run `bash scripts/build.sh clean` and test mutexes protecting shared state between threads. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to synchronization primitives. Add notes, gotchas, and design decisions directly in this TODO section covering the mutex API and implementation details.


- [x] Define `mutex_t` struct (locked flag, owner thread, wait queue)
- [x] Implement `mutex_lock(m)` — block if already locked (CAS → sleep, no busy-spin)
- [x] Implement `mutex_unlock(m)` — release, wake one blocked thread
- [x] Implement `mutex_trylock(m)` — non-blocking attempt, returns 0 on success
- [x] Add deadlock detection (lock ordering check)
- [x] Commit: `"sched: mutex synchronization"`

> **Implementation note — Adaptive Mutexes:** Once §6 Spinlocks and §10 Memory
> Barriers are complete, consider upgrading `mutex_lock()` to spin briefly
> (e.g., 200 cycles) before sleeping if the owner thread is currently running.
> This avoids a context switch for short critical sections held by a thread
> actively executing on the CPU. Linux calls this `MUTEX_SPIN_ON_OWNER`;
> Windows uses a similar heuristic in `KMUTEX`. Implement as a loop of
> `PAUSE` + owner-state check before calling `schedule()` — no separate
> section needed, just an extension of `mutex_lock()`.

---

## 3. Semaphores ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: review that `semaphore_t` struct with count and wait queue exists, that `sem_wait`, `sem_signal`, `sem_init` all function correctly. Run `bash scripts/build.sh clean`. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to semaphores. Add notes, gotchas, and design decisions directly in this TODO section covering the semaphore API and usage patterns.


- [x] Define `semaphore_t` struct (count, wait queue)
- [x] Implement `sem_wait(s)` — decrement, block if count < 0
- [x] Implement `sem_signal(s)` — increment, wake one waiter
- [x] Implement `sem_init(s, initial_count)`
- [x] Commit: `"sched: semaphore synchronization"`

---

## 4. Read-Write Locks ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: review `include/kernel/sched/rwlock.h` to confirm `rwlock_t` struct has `reader_count`, `writer_held`, `writer_pending`, and separate reader/writer wait queues. Confirm `rwlock_read_lock`, `rwlock_read_unlock`, `rwlock_write_lock`, `rwlock_write_unlock`, `rwlock_try_read`, `rwlock_try_write` all exist in `src/kernel/sched/rwlock.c`. Verify that `writer_pending` prevents new reader acquisitions while a writer is waiting (starvation guard). Run `bash scripts/build.sh clean` and confirm `build/kernel/sched/rwlock.o` appears in the linker output. Confirm the commit `"sched: read-write locks"` exists in git history. After verifying, mark all items as `[x]` and update this prompt for future correctness checks. Add notes, gotchas, and design decisions directly in this TODO section covering the rwlock API, starvation prevention design, and usage examples.


- [x] Define `rwlock_t` struct (reader count, writer flag, writer_pending, separate read/write wait queues)
- [x] Implement `rwlock_read_lock(rw)` / `rwlock_read_unlock(rw)`
- [x] Implement `rwlock_write_lock(rw)` / `rwlock_write_unlock(rw)`
- [x] Implement `rwlock_try_read(rw)` / `rwlock_try_write(rw)` — non-blocking
- [x] Writer starvation guard: `writer_pending` blocks new readers while a writer waits
- [x] Use for: VFS mount table, loaded module list, process table
- [x] Commit: `"sched: read-write locks"`

---

## 5. Condition Variables ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: review `include/kernel/sched/condvar.h` to confirm `condvar_t` struct has a waiter queue matching the mutex/semaphore pattern. Confirm `cond_wait`, `cond_signal`, `cond_broadcast` all exist in `src/kernel/sched/condvar.c`. Verify that `cond_wait` calls `mutex_unlock` before `yield()` and `mutex_lock` after waking. Verify `cond_broadcast` wakes all waiters and clears the queue. Run `bash scripts/build.sh clean` and confirm `build/kernel/sched/condvar.o` appears in the linker output. Confirm the commit `"sched: condition variables"` exists in git history. Update `README.md` if it contains stale or incorrect references to synchronization. Add notes directly in this TODO section if inconsistencies or gaps are found.


- [x] Define `condvar_t` struct (waiter\_tasks, waiter\_threads, num\_waiters, name)
- [x] Implement `cond_wait(cond, mutex)` — enqueue → block → unlock mutex → yield → re-lock mutex
- [x] Implement `cond_signal(cond)` — wake one waiter (FIFO), remove from queue
- [x] Implement `cond_broadcast(cond)` — wake all waiters, clear queue
- [x] Graceful degradation when wait queue is full (spin-yield fallback with warning)
- [x] Commit: `"sched: condition variables"`

### Notes

**API** (`include/kernel/sched/condvar.h`):
```c
condvar_t cond = CONDVAR_INIT;
void cond_init(condvar_t *cond, const char *name);
void cond_wait(condvar_t *cond, mutex_t *mutex);   // atomic release + sleep + re-lock
void cond_signal(condvar_t *cond);                 // wake one waiter (FIFO)
void cond_broadcast(condvar_t *cond);              // wake all waiters
```

**cond_wait atomic sequence:**
1. Enqueue calling thread in condvar's wait queue
2. Set thread state to `THREAD_BLOCKED`
3. `mutex_unlock(mutex)` — release before sleeping
4. `yield()` — scheduler skips thread until woken
5. `mutex_lock(mutex)` — re-acquire before returning to caller

**Caller must hold the mutex** before calling `cond_wait`.

**Always use a `while` loop, not `if`** (spurious wakeup guard + race with other consumers):
```c
while (queue_empty(q))
    cond_wait(&q->nonempty, &q->lock);
```

**Producer-consumer pattern:**
```c
/* Producer */
mutex_lock(&q->lock);
enqueue(q, item);
cond_signal(&q->nonempty);
mutex_unlock(&q->lock);

/* Consumer */
mutex_lock(&q->lock);
while (queue_empty(q))
    cond_wait(&q->nonempty, &q->lock);
item = dequeue(q);
mutex_unlock(&q->lock);
```

**Real usage:** compositor vsync wait (`vsync_cond`), shell input wait (`input_cond`).


---

## 6. Spinlocks (IRQ-Safe)

**Prompt:** Spinlocks are the only safe synchronization primitive inside interrupt handlers — they busy-wait using atomic CAS without calling `yield()` or the scheduler. `spin_lock(s)` disables interrupts on the current CPU and spins until the lock is acquired. `spin_unlock(s)` releases and restores interrupts. `spin_lock_irqsave(s, flags)` / `spin_unlock_irqrestore(s, flags)` save/restore the interrupt flag for nested usage. This is equivalent to Windows `KSPIN_LOCK` and Linux `spinlock_t`. Spinlocks must only be held for very short durations (< ~100 ns) — they are for protecting small critical sections inside IRQ handlers. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: spinlocks (IRQ-safe)"`. Add notes, gotchas, and design decisions directly in this TODO section covering spinlock usage and IRQ safety rules.


- [ ] Implement `spinlock_t` (volatile uint32_t flag)
- [ ] Implement `spin_lock(s)` — disable interrupts, CAS loop until acquired
- [ ] Implement `spin_unlock(s)` — release flag, restore interrupts
- [ ] Implement `spin_lock_irqsave(s, flags)` — save RFLAGS, disable IRQs, acquire
- [ ] Implement `spin_unlock_irqrestore(s, flags)` — release, restore RFLAGS
- [ ] Use in: PIT IRQ handler (timer queue), keyboard IRQ, NIC receive path
- [ ] Commit: `"sched: spinlocks (IRQ-safe)"`

---

## 7. Atomic Operations

**Prompt:** Atomic operations are fundamental to lock-free data structures and the implementation of higher-level primitives (spinlocks, reference counting). Wrap GCC's `__sync_*` / `__atomic_*` builtins in a thin `kernel/atomic.h` header for portability and clarity. Key operations: `atomic_read`, `atomic_set`, `atomic_inc`, `atomic_dec`, `atomic_dec_and_test` (reference counting), `atomic_cmpxchg` (CAS), `atomic_fetch_add`. These are used by: `rwlock_t` reader count, kernel object reference counts, lock-free ring buffers, and the slab allocator. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: atomic operations"`. Add notes, gotchas, and design decisions directly in this TODO section covering the atomic API and memory ordering guarantees.


- [ ] Create `include/kernel/atomic.h`
- [ ] `atomic_t` typedef (volatile int32_t or struct wrapper)
- [ ] `atomic_read(a)`, `atomic_set(a, v)` — simple read/write with barrier
- [ ] `atomic_inc(a)`, `atomic_dec(a)`, `atomic_dec_and_test(a)` — for ref counting
- [ ] `atomic_cmpxchg(a, old, new)` — CAS primitive for lock-free algorithms
- [ ] `atomic_fetch_add(a, delta)` — atomic add, returns old value
- [ ] Replace bare `volatile` flags in `mutex.c`/`rwlock.c` with `atomic_t` where appropriate
- [ ] Commit: `"kernel: atomic operations"`

---

## 8. Wait/Event Objects

**Prompt:** Event objects are one-shot or auto-reset synchronization handles — equivalent to Windows `KEVENT` (manual-reset and auto-reset) and Linux `completion`. A manual-reset event: `event_set` wakes all waiters; remains set until `event_reset` clears it. An auto-reset event: `event_set` wakes exactly one waiter and immediately clears itself. These are ideal for: boot synchronization (wait for disk init to complete), vsync signal from PIT to compositor, driver initialization handshakes. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: wait/event objects"`. Add notes, gotchas, and design decisions directly in this TODO section covering event types, wait semantics, and usage examples.


- [ ] Define `event_t` struct (state flag, type enum MANUAL/AUTO_RESET, wait queue)
- [ ] Implement `event_init(ev, type, initial_state)` — initialize event
- [ ] Implement `event_wait(ev)` — block until event is set; auto-reset clears on wake
- [ ] Implement `event_set(ev)` — signal event (wake all for manual, one for auto)
- [ ] Implement `event_reset(ev)` — clear manual-reset event
- [ ] Implement `event_wait_timeout(ev, ms)` — wait with timeout (returns 0 on timeout)
- [ ] Use for: boot phase sync, vsync compositor signal, driver init handshakes
- [ ] Commit: `"sched: wait/event objects"`

---

## 9. Work Queues (Deferred Work)

**Prompt:** IRQ handlers must be short — they run with interrupts disabled and cannot sleep, yield, or call VFS. Work queues solve this by deferring slow work to a kernel thread that runs in normal context. An IRQ handler calls `workqueue_enqueue(wq, func, arg)` to schedule a callback — the work queue kernel thread picks it up and calls `func(arg)` in a yieldable context. This is equivalent to Linux `workqueue_struct` / `schedule_work()` and Windows DPC + work items. Used by: NIC receive path (packet processing after DMA), disk IRQ (completing async I/O), USB events. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: work queues"`. Add notes, gotchas, and design decisions directly in this TODO section covering the workqueue API, IRQ-safety rules, and work item lifecycle.


- [ ] Define `work_item_t` struct (callback function ptr, arg, next pointer)
- [ ] Define `workqueue_t` (spinlock-protected linked list + semaphore)
- [ ] Implement `workqueue_create(name)` — spawn a kernel thread for processing
- [ ] Implement `workqueue_enqueue(wq, func, arg)` — IRQ-safe, uses spinlock
- [ ] Work queue thread: loop on semaphore, dequeue, call `func(arg)`, repeat
- [ ] Create default system work queue (`sys_wq`) at boot
- [ ] Wire NIC receive DMA completion to `sys_wq` (replace direct IRQ processing)
- [ ] Commit: `"kernel: work queues"`

---

## 10. Memory Barriers & Compiler Fences

**Prompt:** Memory barriers prevent the CPU and compiler from reordering memory accesses across synchronization boundaries. Without them, lock-free code and even spinlock implementations can silently mis-order on modern out-of-order CPUs. Implement as lightweight macros using GCC's built-ins and x86 `MFENCE`/`LFENCE`/`SFENCE` instructions. Add `barrier()` (compiler-only fence) and `mb()`/`rmb()`/`wmb()` (full/read/write memory barriers). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: memory barriers"`. Add notes, gotchas, and design decisions directly in this TODO section covering when to use each barrier type.

> **Note for single-core:** On x86 single-core, the CPU guarantees strong ordering for most operations. The compiler barrier (`barrier()`) is still needed to prevent GCC from optimizing away `volatile` accesses. The full `mb()` becomes important when SMP is added.


- [ ] Create `include/kernel/barrier.h`
- [ ] `barrier()` — compiler-only fence: `__asm__ volatile("" ::: "memory")`
- [ ] `mb()` — full memory barrier: `__asm__ volatile("mfence" ::: "memory")`
- [ ] `rmb()` — read barrier: `__asm__ volatile("lfence" ::: "memory")`
- [ ] `wmb()` — write barrier: `__asm__ volatile("sfence" ::: "memory")`
- [ ] `smp_mb()`, `smp_rmb()`, `smp_wmb()` — SMP-aware aliases (= mb/rmb/wmb now; nop when !SMP)
- [ ] Apply `barrier()` in `spinlock.h` and `atomic.h` acquire/release paths
- [ ] Commit: `"kernel: memory barriers"`


---

## 11. Priority Inheritance (Mutex Enhancement)

**Prompt:** Priority inversion happens when a low-priority thread holds a mutex that a high-priority thread needs, while a medium-priority thread preempts it — the high-priority thread is effectively blocked by the medium one. Priority inheritance fixes this: when a high-priority thread blocks on a mutex, the lock owner's priority is temporarily boosted to match. Implement as an extension to `mutex_t` — add a `priority_ceiling` field; on block, boost the owner's thread priority to `max(owner_prio, waiter_prio)`; on unlock, restore the original priority. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: mutex priority inheritance"`. Add notes, gotchas, and design decisions directly in this TODO section covering the priority inversion problem and the inheritance solution.


- [ ] Add `base_priority` field to `thread_t` (saved original priority before boost)
- [ ] Add `priority` (current effective priority) field to `thread_t`
- [ ] Update scheduler to select highest-priority READY thread (priority-aware)
- [ ] In `mutex_lock()`: if blocked, boost lock owner's priority to waiter's priority
- [ ] In `mutex_unlock()`: restore owner's priority to `base_priority`
- [ ] Handle cascaded inheritance (A waits on B, B waits on C → C gets A's priority)
- [ ] Commit: `"sched: mutex priority inheritance"`

---

## 12. Seqlocks (Ultra-Fast Read Path)

**Prompt:** Seqlocks allow readers to proceed without taking any lock at all — they read a monotonically-incrementing sequence counter before and after the read; if the counter changed (writer was active), they retry. Writers increment the counter before and after modifying data (odd = write in progress). This gives O(1) reads with zero locking overhead for read-mostly data that changes rarely (e.g., system clock, jiffies, uptime counter). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: seqlocks"`. Add notes, gotchas, and design decisions directly in this TODO section covering seqlock semantics and use cases.


- [ ] Define `seqlock_t` (spinlock + volatile uint64_t sequence counter)
- [ ] Implement `seqlock_write_lock(sl)` / `seqlock_write_unlock(sl)` — inc counter odd/even
- [ ] Implement `seqlock_read_begin(sl)` — return current sequence (retry if odd)
- [ ] Implement `seqlock_read_retry(sl, seq)` — return true if sequence changed
- [ ] Usage pattern: `do { seq = seqlock_read_begin(sl); ... } while (seqlock_read_retry(sl, seq))`
- [ ] Use for: system uptime counter, jiffies, cached RTC time
- [ ] Commit: `"sched: seqlocks"`

---

## 13. RCU — Read-Copy-Update

**Prompt:** RCU is Linux's most powerful scalability primitive — readers hold no lock at all (zero overhead), writers atomically publish a new version of a data structure by updating a pointer, then wait for all current readers to finish before freeing the old version. Implement a simplified single-core RCU: `rcu_read_lock()` / `rcu_read_unlock()` disable preemption (on single-core this is sufficient); `synchronize_rcu()` blocks until all RCU read-side critical sections complete; `rcu_assign_pointer(ptr, new)` / `rcu_dereference(ptr)` handle pointer publishing with barriers. Use for: VFS dentry cache, network route table, loaded module list. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: RCU (single-core)"`. Add notes, gotchas, and design decisions directly in this TODO section covering RCU concepts, the grace period, and safe usage patterns.

> **Scope:** Single-core simplified RCU only. Full SMP RCU (quiescent-state tracking per CPU) is deferred to the SMP phase.


- [ ] Implement `rcu_read_lock()` — disable preemption (single-core: disable scheduler)
- [ ] Implement `rcu_read_unlock()` — re-enable preemption
- [ ] Implement `synchronize_rcu()` — wait for all in-progress RCU read sections to exit
- [ ] Implement `rcu_assign_pointer(ptr, new)` — write barrier + pointer store
- [ ] Implement `rcu_dereference(ptr)` — read barrier + pointer load
- [ ] Use for: VFS path cache, route table, module list (replace rwlock where read-dominant)
- [ ] Commit: `"sched: RCU (single-core)"`

---

## 14. SMP Support (Future)

**Prompt:** SMP (symmetric multi-processing) allows multiple CPU cores to run kernel threads simultaneously. This requires: per-CPU data structures (no false sharing), IPI (inter-processor interrupt) for TLB shootdown and cross-CPU wakeups, CPU-aware lock primitives (spinlock must use `LOCK` prefix on x86 SMP), NUMA-aware memory allocation, and a load-balancing scheduler. This is a large future milestone — do not start until the single-core kernel is stable and the full feature set is implemented.

> **Prerequisite:** §7 Atomic Operations and §10 Memory Barriers must be complete before any SMP work begins.


- [ ] Parse ACPI MADT to discover all CPUs and APICs
- [ ] Initialize secondary CPUs (AP startup via SIPI IPI)
- [ ] Implement per-CPU data (`per_cpu(var, cpu)` macro using GS segment)
- [ ] Add `LOCK` prefix to atomic ops and spinlocks for SMP correctness
- [ ] Implement IPI: `send_ipi_single(cpu)`, `send_ipi_all_but_self()`
- [ ] TLB shootdown IPI: flush remote CPU page tables on `munmap`/`mprotect`
- [ ] SMP-aware scheduler: run queue per CPU, load balancing via work stealing
- [ ] Commit series: `"smp: AP bringup"`, `"smp: per-CPU data"`, `"smp: scheduler"`

---

## 15. Futexes (User-Space Fast Mutex)

**Prompt:** Futexes (fast userspace mutexes) allow user-mode mutex/condvar implementations to avoid syscalls in the uncontended case. The kernel only gets involved when a thread actually needs to sleep or wake. `SYS_FUTEX_WAIT(addr, expected)` — if `*addr == expected`, sleep on that address. `SYS_FUTEX_WAKE(addr, n)` — wake up to N threads sleeping on that address. User-mode pthreads, C++ `std::mutex`, and Go's runtime all use futexes under the hood. Prerequisite: §1 File Descriptors (for per-process futex table) and §3 User-Mode Heap. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: futex syscall"`. Add notes, gotchas, and design decisions directly in this TODO section covering futex semantics and a user-mode mutex example.


- [ ] Define per-process futex wait table (hash map of address → wait queue)
- [ ] Implement `SYS_FUTEX_WAIT(uaddr, expected)` — atomic check-and-sleep
- [ ] Implement `SYS_FUTEX_WAKE(uaddr, n)` — wake up to N waiters
- [ ] Implement `SYS_FUTEX_WAKE_OP` — combined wake + atomic operation (for condvar)
- [ ] Port or write a user-mode mutex using futexes (for `user/lib/`)
- [ ] Test: two user threads contend on a futex mutex, verify no deadlock
- [ ] Commit: `"kernel: futex syscall"`

---

## 16. Lock Dependency Validator (Debug Build)

**Prompt:** A lock dependency validator (like Linux `lockdep` or Windows Driver Verifier) detects potential deadlocks at runtime by building a directed graph of lock acquisition order. Every time a lock is acquired, the validator records which locks the current thread already holds. If the new acquisition would create a cycle in the dependency graph (A→B and B→A acquired in different threads), it fires a warning immediately — before the actual deadlock occurs. Enable with `-DLOCKDEP` at build time. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: lock dependency validator"`. Add notes, gotchas, and design decisions directly in this TODO section covering lockdep usage, interpreting warnings, and adding annotations.

> **Debug only:** Zero overhead in release builds (`#ifdef LOCKDEP`). Only active in debug/testing builds.


- [ ] Define lock class: unique ID per lock type (not per instance), registered at first lock
- [ ] Per-thread held-locks stack: record each acquired lock's class ID
- [ ] On `mutex_lock()` / `rwlock_write_lock()`: check if acquiring class creates a cycle with held stack
- [ ] Dependency graph: directed edges from held-class → acquiring-class; detect cycles (DFS)
- [ ] On cycle detected: `printk("[LOCKDEP] Potential deadlock: ...")` with full held-locks stack
- [ ] Apply to: `mutex_t`, `rwlock_t`, `spinlock_t` (condvar and semaphore excluded — different semantics)
- [ ] All state inside `#ifdef LOCKDEP` guards — zero overhead in release
- [ ] Commit: `"debug: lock dependency validator"`

---

## Priority Order

| Priority  | Section                    | Reason                                                         |
| --------- | -------------------------- | -------------------------------------------------------------- |
| ✅ Done   | 1. Kernel Threads          | Verified complete                                              |
| ✅ Done   | 2. Mutexes                 | Verified complete                                              |
| ✅ Done   | 3. Semaphores              | Verified complete                                              |
| ✅ Done   | 4. Read-Write Locks        | Implemented — rwlock.o linked, BUILD OK                        |
| ✅ Done   | 5. Condition Variables     | Implemented — condvar.o linked, BUILD OK                       |
| 🔴 P0     | 10. Memory Barriers        | Prerequisite for correct spinlocks and atomics                 |
| 🔴 P0     | 7. Atomic Operations       | Prerequisite for spinlocks and reference counting              |
| 🔴 P0     | 6. Spinlocks               | Needed for IRQ-safe locking in PIT, keyboard, NIC handlers     |
| 🔴 P0     | 23. Ticket Locks           | Fairer spinlock variant — implement alongside §6               |
| 🔴 P0     | 25. Stack Guard Pages      | Catches stack overflow before it silently corrupts memory      |
| 🔴 P0     | 26. Preemption Count       | Lighter than IRQ disable for non-interrupt critical sections   |
| 🔴 P0     | 11. Priority Inheritance   | Prevents priority inversion — required for real-time tasks     |
| 🟠 P1     | 8. Wait/Event Objects      | Boot sync, vsync, driver handshakes — cleaner than semaphores  |
| 🟠 P1     | 9. Work Queues             | Required for proper IRQ bottom-half processing                 |
| 🟠 P1     | 15. Futexes                | User-mode mutex/condvar; needed when user processes mature     |
| 🟠 P1     | 22. Thread-Local Storage   | Required for user-space C runtime (errno, locale, pthreads)    |
| 🟠 P1     | 24. Kernel Watchdog        | Catches deadlocked/hung tasks that lockdep can’t detect        |
| 🟠 P1     | 28. pthread_once           | Eliminates init races; replaces all ad-hoc bool init guards    |
| 🟠 P1     | 29. pthread_barrier_t      | Frame-sync for compositor audio+render pipeline                |
| 🟡 P2     | 12. Seqlocks               | Ultra-fast clock/uptime reads; no blocking needed              |
| 🟡 P2     | 13. RCU                    | Lock-free reads for VFS, routing table, module list            |
| 🟡 P2     | 16. Lock Validator (debug) | Catches deadlocks before they happen; debug builds only        |
| 🟡 P2     | 30. KCSAN                  | Runtime data-race detector — debug build; beats Windows        |
| 🔵 P3     | 31. TSX/HTM Lock Elision   | Hardware perf opt — real Intel hardware only, TAA-gated        |
| 🟡 P2     | 27. Thread Cancellation    | POSIX pthread_cancel; needed for clean user-space threading    |
| 🔵 P2     | 14. SMP Support            | Critical for production HW — after §6/7/10/11/26 complete      |

---

## Impossible OS Differentiators

The following sections (§17–24) are features that go beyond what either Windows 11 or Linux offer — unique to Impossible OS.

## 17. Priority Inheritance ON by Default

**Prompt:** Linux's `rt_mutex` requires explicit opt-in — the standard `mutex_t` has no inheritance and is susceptible to priority inversion. Windows applies heuristic priority boosts (not true inheritance) and only for specific scenarios. Impossible OS makes priority inheritance the **default** on every `mutex_t` — no opt-in, no separate type, no special annotation needed. Developers never have to think about priority inversion. Extend the existing §11 implementation so that inheritance is always active. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: priority inheritance on by default"`. Create or add notes directly in this TODO section explaining the default-on design decision and how it differs from Linux/Windows.


- [ ] Ensure §11 priority inheritance is implemented in `mutex_lock()` with no flags needed
- [ ] Remove any `PI_MUTEX` opt-in flag — plain `mutex_t` always inherits
- [ ] Document: all `mutex_t` locks have priority inheritance — no exceptions
- [ ] Add a build-time `static_assert` that confirms PI cannot be disabled
- [ ] Commit: `"sched: priority inheritance on by default"`

---

## 18. Wait-on-Multiple Primitives

**Prompt:** Windows has `WaitForMultipleObjects` — wait for any or all of N kernel handles simultaneously. Linux has no equivalent for kernel sync objects (only `poll`/`epoll` for file descriptors). Impossible OS adds a unified `waitable_t` interface — a common vtable pointer embedded in `mutex_t`, `semaphore_t`, `event_t`, and `condvar_t`. `wait_any(handles[], count, timeout_ms)` blocks until at least one is signalled; `wait_all(handles[], count, timeout_ms)` blocks until all are signalled. Returns the index of the signalled handle (or `WAIT_TIMEOUT`). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: wait-on-multiple primitives"`. Add notes, gotchas, and design decisions directly in this TODO section covering the waitable interface and usage examples.


- [ ] Define `waitable_t` interface: `{ int (*is_ready)(void *); void (*add_waiter)(void *, waiter_t *); }`
- [ ] Embed `waitable_t` vtable pointer in `mutex_t`, `semaphore_t`, `event_t`, `condvar_t`
- [ ] Implement `wait_any(waitable_t *handles[], count, timeout_ms)` — wake on first ready
- [ ] Implement `wait_all(waitable_t *handles[], count, timeout_ms)` — wake when all ready
- [ ] Return value: index of signalled handle, or `WAIT_TIMEOUT (-1)` on timeout
- [ ] Test: wait_any on [mutex, event] — verify correct index returned
- [ ] Commit: `"sched: wait-on-multiple primitives"`

---

## 19. Unified `_timeout(ms)` API

**Prompt:** Linux timeout semantics are inconsistent — some primitives take jiffies, some `timespec`, some relative time, some absolute. Windows is similarly fragmented. Impossible OS standardises: **every** blocking primitive has a `_timeout(ms)` variant with identical semantics — pass milliseconds, get back `WAIT_SIGNALLED` or `WAIT_TIMEOUT`. Implement by extending §8 `event_wait_timeout`, and adding `mutex_lock_timeout`, `sem_wait_timeout`, `rwlock_read_lock_timeout`, `rwlock_write_lock_timeout`. The timer uses the PIT tick counter for ms-accurate timeouts. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: unified timeout API"`. Add notes, gotchas, and design decisions directly in this TODO section covering timeout semantics and the return value convention.

```c
#define WAIT_SIGNALLED   0
#define WAIT_TIMEOUT    -1
```

- [ ] Add `mutex_lock_timeout(m, ms)` → `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] Add `sem_wait_timeout(s, ms)` → `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] Add `rwlock_read_lock_timeout(rw, ms)` → `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] Add `rwlock_write_lock_timeout(rw, ms)` → `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] Add `cond_wait_timeout(cond, mutex, ms)` → `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] All timeout variants use PIT tick counter for ms-accurate measurement
- [ ] Commit: `"sched: unified timeout API"`

---

## 20. Graphical Deadlock Visualization

**Prompt:** When the §16 lock dependency validator detects a deadlock cycle, instead of a plain text kernel log dump, Impossible OS draws the dependency graph directly to the framebuffer using the existing GFX subsystem — thread boxes connected by lock-dependency arrows, the cycle edges highlighted in red, with the blocking lock name and owner thread labelled. This is unprecedented — Linux prints a wall of text to dmesg; Windows shows a blue screen with a stop code. Impossible OS shows a clear, readable diagram. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: graphical deadlock visualization"`. Add notes, gotchas, and design decisions directly in this TODO section covering the visualization format and how to interpret it.

> **Prerequisite:** §16 Lock Dependency Validator must be complete first.


- [ ] On deadlock detection: collect the cycle (thread IDs, lock names, edges)
- [ ] Call `gfx_deadlock_draw(cycle_nodes[], edges[], count)` — new GFX function
- [ ] Draw thread boxes (`gfx_fill_rect` + thread name label)
- [ ] Draw lock-dependency arrows between boxes (`gfx_draw_line`)
- [ ] Highlight cycle edges in red; non-cycle edges in grey
- [ ] Label each edge with the lock name (`gfx_draw_text`)
- [ ] Flush to framebuffer: `fb_swap()` — visible even without a shell
- [ ] Commit: `"debug: graphical deadlock visualization"`

---

## 21. Named Lock Browser (`/sys/locks` + `locks` shell command)

**Prompt:** Every Impossible OS synchronization primitive already has a `name` field. Expose all currently-held locks via a VFS virtual file `/sys/locks` — each entry shows: lock name, type (mutex/rwlock/spinlock/semaphore), owner thread, waiter count, and time held in milliseconds. A `locks` shell command reads and formats this output. Linux `/proc/locks` only shows file locks. Windows has no equivalent for kernel sync objects. This provides real-time lock observability — useful for debugging deadlocks and performance bottlenecks. The Task Manager (Phase 05) can also read this interface. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: /sys/locks named lock browser"`. Add notes, gotchas, and design decisions directly in this TODO section covering the /sys/locks format and shell usage.


- [ ] Add global lock registry: linked list of all initialized primitives (registered in `mutex_init`, `rwlock_init`, etc.)
- [ ] Track per-lock: owner thread id, waiter count, acquire timestamp (PIT ticks)
- [ ] Implement `/sys/locks` VFS virtual file — `read()` returns formatted text entries
- [ ] Format: `TYPE  NAME              OWNER   WAITERS  HELD_MS\n mutex heap_lock         task:3  2        14\n`
- [ ] Add `locks` shell command — reads and pretty-prints `/sys/locks`
- [ ] Wire lock registry into Task Manager (Phase 05) lock viewer panel
- [ ] Commit: `"kernel: /sys/locks named lock browser"`

---

## 22. Thread-Local Storage (TLS)

**Prompt:** Every C runtime library (musl, glibc) and POSIX `pthreads` relies on per-thread storage for `errno`, locale data, stack canaries, and `pthread_getspecific()` values. Without TLS, user-space multithreaded programs silently corrupt each other's error state. Implement TLS as a per-thread data block allocated at `thread_create()` time and pointed to by the `FS` segment register. The ELF TLS model (Initial Exec) stores compile-time `__thread` variables at a negative offset from `FS:0`. For the kernel side, expose `tls_get(thread, key)` / `tls_set(thread, key, value)` for arbitrary per-thread data (equivalent to `pthread_key_*`). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: thread-local storage (TLS)"`. Add notes, gotchas, and design decisions directly in this TODO section.

> **Cross-reference:** §027 Process Model — the per-process file descriptor table and CWD also use per-thread storage for user-space visibility.

> [!CAUTION]
> TLS block must be allocated from `pmm_alloc_contiguous()` if > 4 KB. Default user-space TLS is typically 512 B–4 KB. Use `pmm_alloc_contiguous()` if the user requests a large TLS block via `pthread_key_create`.

- [ ] Define `tls_block_t` per-thread struct: pointer array for dynamic keys + static `__thread` data area
- [ ] Allocate TLS block in `thread_create()` (initial size: 4 KB, from `kmalloc`)
- [ ] Load `FS` base register to point at TLS block (`WRMSR IA32_FS_BASE`)
- [ ] On context switch: save/restore `FS` base per thread (add to `thread_t`)
- [ ] Implement `tls_key_create(destructor)` → unique key (like `pthread_key_create`)
- [ ] Implement `tls_set(key, value)` / `tls_get(key)` — O(1) slot lookup
- [ ] Run TLS destructors on `thread_exit()` (call registered destructor for each non-NULL key)
- [ ] ELF TLS: support `PT_TLS` program header at `exec()` time (copy TLS template per thread)
- [ ] Place kernel `errno` per-thread: `#define errno (*tls_errno())` in `<errno.h>`
- [ ] Test: two threads each set errno to different values, verify no interference
- [ ] Commit: `"kernel: thread-local storage (TLS)"`

---

## 23. Ticket Locks — Fairer Spinlocks

**Prompt:** Standard CAS-based spinlocks (`spin_lock` in §6) have a starvation problem under high contention: whichever thread wins the atomic CAS instruction gets the lock, regardless of how long it has been waiting. Ticket locks solve this with FIFO ordering: an `atomic_fetch_add` on `next_ticket` gives the caller their ticket number; they spin waiting for `now_serving` to reach their ticket; on `spin_unlock`, `now_serving` is incremented. This guarantees strict FIFO ordering with zero extra overhead in the uncontended case. Implement as a drop-in companion to `spinlock_t` — same IRQ-save/restore semantics, same usage — just fair. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: ticket locks (fair spinlocks)"`. Add notes directly comparing ticket locks to CAS spinlocks and documenting when to prefer each.

```c
/* Example: fair spinlock with FIFO ordering */
typedef struct { uint32_t next_ticket; uint32_t now_serving; } ticket_lock_t;

void ticket_lock(ticket_lock_t *tl) {
    uint32_t my_ticket = atomic_fetch_add(&tl->next_ticket, 1);
    while (atomic_read(&tl->now_serving) != my_ticket) barrier();
}
void ticket_unlock(ticket_lock_t *tl) { atomic_inc(&tl->now_serving); }
```

> [!NOTE]
> Prefer ticket locks for any shared data structure accessed by > 2 concurrent threads (e.g., run queue, NIC Tx ring). Use plain `spinlock_t` only for ultra-short single-owner sections where fairness is irrelevant.

- [ ] Define `ticket_lock_t` (`next_ticket` + `now_serving` — two adjacent `uint32_t`)
- [ ] Implement `ticket_lock(tl)` — `atomic_fetch_add` + spin loop with `barrier()`
- [ ] Implement `ticket_unlock(tl)` — `atomic_inc(&now_serving)`
- [ ] Implement `ticket_lock_irqsave(tl, flags)` / `ticket_unlock_irqrestore(tl, flags)`
- [ ] Implement `ticket_trylock(tl)` — CAS on both words atomically (only if uncontended)
- [ ] Replace scheduler run-queue lock with `ticket_lock_t` (most contended lock in the kernel)
- [ ] Commit: `"sched: ticket locks (fair spinlocks)"`

---

## 24. Kernel Watchdog Thread

**Prompt:** When a task spins in an infinite loop or holds a spinlock for too long, the kernel silently hangs — `lockdep` (§16) cannot catch it because no lock ordering is violated, and the PIT timer never fires (spinlocks disable interrupts). A kernel watchdog thread runs at the highest priority and monitors a per-CPU heartbeat counter. Every PIT tick, each CPU core increments its heartbeat. The watchdog checks this counter every N seconds; if a core's heartbeat has not advanced, it fires: logs to serial, draws a warning banner on the framebuffer, and optionally panics. Equivalent to Linux `CONFIG_LOCKUP_DETECTOR` (soft lockup / hard lockup detector). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: kernel watchdog thread"`. Add notes directly in this TODO section covering the heartbeat mechanism, threshold tuning, and how it complements `lockdep`.

> **Complements §16 lockdep:** lockdep catches potential deadlocks before they happen (dependency graph). The watchdog catches actual hangs that are already in progress — infinite loops, held spinlocks, runaway interrupt storms.

- [ ] Add `cpu_heartbeat` counter (atomic `uint64_t`, incremented every PIT tick in the ISR)
- [ ] Create `watchdog_thread` kernel thread (highest priority, runs every 5 seconds)
- [ ] In `watchdog_thread`: compare `cpu_heartbeat` now vs. 5 seconds ago
- [ ] If heartbeat has not advanced by at least 1 tick: fire watchdog
- [ ] Watchdog action: log `[WATCHDOG] Soft lockup detected — CPU stuck for Xs` to serial
- [ ] Draw warning banner on framebuffer: red bar at top with thread name + uptime
- [ ] Configurable threshold via Registry `SYSTEM\Watchdog\TimeoutSeconds` (default: 10)
- [ ] Configurable action via Registry `SYSTEM\Watchdog\Action` (`log` / `panic` / `reboot`)
- [ ] Test: create a task that spins forever — verify watchdog fires within timeout
- [ ] Commit: `"debug: kernel watchdog thread"`

---

## 25. Thread Stack Guard Pages

**Prompt:** Each `thread_create()` allocates a stack from `kmalloc` or PMM but
places no protection at the bottom. A thread that overflows its stack silently
corrupts whatever is adjacent in memory — on a freestanding kernel this can be
another thread's stack, heap metadata, or PMM bookkeeping data, with no error
signal whatsoever. Fix: immediately after allocating the stack, mark the bottom
page as `PROT_NONE` via `vmm_map(stack_base, PAGE_SIZE, VMM_PROT_NONE)`. The
first write past the stack bottom triggers a page fault — the fault handler
checks if the faulting address is a known guard page and panics cleanly with
`"Stack overflow in thread '%s' (tid=%d)"` instead of silent corruption.
Equivalent to Windows stack guard pages and Linux `SIGSEGV` on `MAP_STACK`.
After completing all items, mark every item as `[x]`, update this prompt to a
verification prompt, run `bash scripts/build.sh clean`, and commit as
`"sched: thread stack guard pages"`. Add notes covering the guard page
location, size, and the page fault handler check.

> [!CAUTION]
> The stack itself must use `pmm_alloc_contiguous()` if > 4 KB (which is always
> — default stack is 64 KB). The guard page has no physical backing frame; it is
> a VMM-only mapping with `PROT_NONE`. Only the `vmm_map_guard()` call itself is
> cheap — the cost is already paid when allocating the stack.

- [ ] After stack alloc in `thread_create()`: `vmm_map_guard(stack_base, PAGE_SIZE)`
- [ ] `vmm_map_guard()`: maps 1 page at `stack_base` with `PROT_NONE` (no read, write, exec)
- [ ] In page fault handler: check if faulting address is in any thread's guard page range
- [ ] On guard page fault: log `[PANIC] Stack overflow: thread '%s' (tid=%d) at 0x%016llx`
- [ ] Include current stack pointer and thread name in the panic message
- [ ] On `thread_exit()` / `thread_destroy()`: unmap guard page before freeing stack
- [ ] Test: create a thread that recurses infinitely — verify clean stack overflow panic
- [ ] Commit: `"sched: thread stack guard pages"`

---

## 26. Preemption Count (`preempt_disable` / `preempt_enable`)

**Prompt:** Kernel code that must not be preempted but doesn't need full
interrupt safety currently has only one option: `spin_lock_irqsave()` which
disables all interrupts on the CPU. This is too heavy when the only requirement
is "don't context-switch away from me" — for example, RCU read-side critical
sections, `kmap()` for atomic page mapping, and `pagefault_disable()` all just
need to inhibit the scheduler, not IRQs. A preemption count solves this: add a
`preempt_count` field to `thread_t`; `preempt_disable()` increments it;
`preempt_enable()` decrements it and reschedules if `preempt_count == 0` and
a reschedule was requested. The scheduler checks `if (current->preempt_count >
0) return` at preemption points. IRQs remain fully enabled — timers and
network interrupts still fire. Equivalent to Linux `preempt_disable()` /
`preempt_enable()`. After completing all items, mark every item as `[x]`,
update this prompt to a verification prompt, run `bash scripts/build.sh clean`,
and commit as `"sched: preemption count"`. Add notes on nesting rules
and the reschedule-pending flag.

> **Pairs with §13 RCU:** `rcu_read_lock()` / `rcu_read_unlock()` are
> implemented as `preempt_disable()` / `preempt_enable()` on single-core.
> This makes §26 a prerequisite for a clean §13 implementation.

```c
/* Usage: protect a critical section without disabling IRQs */
preempt_disable();
void *ptr = some_global_ptr;   /* safe — won't be freed mid-read */
do_something(ptr);
preempt_enable();              /* reschedules if needed */
```

- [ ] Add `preempt_count` (`uint32_t`) field to `thread_t`, initialized to 0
- [ ] Add `need_resched` (`bool`) flag to `thread_t` — set by scheduler tick if preemption was deferred
- [ ] Implement `preempt_disable()` — `current->preempt_count++`
- [ ] Implement `preempt_enable()` — `if (--current->preempt_count == 0 && need_resched) schedule()`
- [ ] In PIT tick ISR: if `current->preempt_count > 0`, set `need_resched = true` and return
- [ ] In `schedule()`: assert `preempt_count == 0` on debug builds (detect illegal scheduling)
- [ ] Implement `preempt_count()` macro — returns current thread's count (useful for assertions)
- [ ] Update §13 RCU: replace `preempt_disable_irq()` calls with `preempt_disable()`
- [ ] Test: `preempt_disable()` in a loop — verify PIT tick fires but context switch is deferred
- [ ] Commit: `"sched: preemption count"`

---

## 27. Thread Cancellation (`pthread_cancel`)

**Prompt:** POSIX thread cancellation allows one thread to request that another
thread terminate cleanly. `pthread_cancel(tid)` marks the target thread for
cancellation. The target thread exits at the next **cancellation point** — a
blocking call such as `sem_wait()`, `cond_wait()`, `pipe_read()`, or
`sleep()`. Two cancellation types exist: `PTHREAD_CANCEL_DEFERRED` (only exits
at cancellation points — safe) and `PTHREAD_CANCEL_ASYNCHRONOUS` (exits
immediately — dangerous, rarely used). Cleanup handlers registered with
`pthread_cleanup_push()` are invoked in LIFO order on cancellation so that
locks and resources are released correctly.

Implement by adding a `cancel_requested` flag and `cancel_state` enum to
`thread_t`. Cancellation points check `cancel_requested` and call
`thread_exit(PTHREAD_CANCELED)` if set. This is the mechanism that allows
`pthread_join()` to detect that a thread was cancelled rather than returning
normally.

> **Related to §15 Futexes:** `SYS_FUTEX_WAIT` is a POSIX cancellation point —
> when a futex wait is cancelled, the futex wait table entry must be cleaned up
> atomically to avoid dangling waiter records.

> [!NOTE]
> `PTHREAD_CANCEL_ASYNCHRONOUS` is intentionally **not implemented**. It is
> impossible to implement safely in most kernel contexts — Linux documents it
> as unsafe and recommends deferred cancellation for all practical uses.

- [ ] Add `cancel_requested` (`bool`) field to `thread_t`
- [ ] Add `cancel_state` enum: `CANCEL_ENABLED` (default) / `CANCEL_DISABLED`
- [ ] Implement `thread_cancel(tid)` — set `cancel_requested = true` on target thread
- [ ] Add `SYS_PTHREAD_CANCEL` syscall
- [ ] Add `pthread_setcancelstate(state, &oldstate)` — enable/disable cancellability
- [ ] Mark as cancellation points: `sem_wait`, `cond_wait`, `pipe_read`, `mq_recv`, `sleep`, `futex_wait`
- [ ] At each cancellation point: `if (cancel_requested && cancel_state == CANCEL_ENABLED) thread_exit(PTHREAD_CANCELED)`
- [ ] Implement cleanup handler stack: `pthread_cleanup_push(fn, arg)` / `pthread_cleanup_pop(execute)`
- [ ] On cancellation exit: invoke cleanup handlers LIFO before `thread_exit()`
- [ ] `pthread_join()` distinguishes `PTHREAD_CANCELED` from normal exit code
- [ ] Test: cancel a thread blocked in `cond_wait()` — verify cleanup handler runs
- [ ] Commit: `"sched: thread cancellation (pthread_cancel)"`

---

## 28. `pthread_once` / `once_flag` — Single-Initialization Primitive

**Prompt:** Many subsystems need to run initialization code exactly once, no
matter how many threads call the init function simultaneously. Without a
`once_flag`, the naive approach uses a global bool that creates a race:
two threads both read `initialized == false`, both call init, and one
corrupts the other’s result. `pthread_once` solves this atomically: a
`once_flag` wraps a bool + mutex; the first thread to call
`pthread_once(&flag, init_fn)` runs `init_fn`, all others block until it
completes, then return immediately. Subsequent calls are a single atomic
read — essentially free once initialized. This is the mechanism behind
C++ `std::call_once` and every lazy singleton in the kernel (e.g.,
font cache init, registry root init, NTP state init). After completing
all items, mark every item as `[x]`, update this prompt to a verification
prompt, run `bash scripts/build.sh clean`, and commit as
`"sched: pthread_once / once_flag"`. Add notes covering why the naive bool
approach races and how once_flag avoids it.

```c
/* ~15 lines total, built on §2 mutex */
typedef struct { mutex_t mu; bool done; } once_flag;
#define ONCE_FLAG_INIT { MUTEX_INIT, false }

void pthread_once(once_flag *f, void (*fn)(void)) {
    if (atomic_read(&f->done)) return;  /* fast path: already done */
    mutex_lock(&f->mu);
    if (!f->done) { fn(); atomic_write(&f->done, true); }
    mutex_unlock(&f->mu);
}
```

- [ ] Define `once_flag_t` struct: `{ mutex_t mu; bool done; }` with `ONCE_FLAG_INIT` macro
- [ ] Implement `pthread_once(flag, fn)` with double-checked locking (atomic read on fast path)
- [ ] Expose `call_once(flag, fn)` as a C11-compatible alias
- [ ] Register `once_flag_t` with lockdep (§16) to catch improper nesting
- [ ] Replace any existing ad-hoc `static bool initialized` guards in kernel subsystems with `once_flag_t`
- [ ] Test: 8 threads simultaneously call `pthread_once` — verify `fn` runs exactly once
- [ ] Commit: `"sched: pthread_once / once_flag"`

---

## 29. `pthread_barrier_t` — Barrier Synchronization

**Prompt:** A barrier blocks a fixed number of threads until all of them have
arrived, then releases all of them simultaneously. This is essential for
stage-synchronized pipelines: the compositor can wait for both the render
thread and audio thread to finish their frame work before flipping the
framebuffer. Without a barrier, each thread needs ad-hoc semaphore
choreography that is error-prone and hard to read.

Implementation: `barrier_t` holds a count, a target, a mutex, and a
condition variable. Each thread calls `barrier_wait(&b)` — it increments
count under the mutex. If `count < target`, it waits on the condvar. The
last thread to arrive (`count == target`) resets `count = 0` and broadcasts.
Windows has no direct `pthread_barrier_t` equivalent (developers use manual
counters + events). Linux has it in pthreads. Impossible OS adds it to the
kernel so compositor frame sync needs zero user-space coordination.

After completing all items, mark every item as `[x]`, update this prompt
to a verification prompt, run `bash scripts/build.sh clean`, and commit as
`"sched: pthread_barrier_t"`. Add notes on the phase-reset trick and
the `PTHREAD_BARRIER_SERIAL_THREAD` return value.

> **Impossible OS advantage:** Windows has no `pthread_barrier_t` —
> developers must emulate it with `WaitForMultipleObjects` on N events
> (fragile, O(N) handles). Linux has it only in user-space pthreads, not as
> a kernel primitive. Impossible OS exposes it as a first-class kernel sync
> object accessible from both kernel threads and user processes.

```c
typedef struct {
    uint32_t  target;   /* total threads that must arrive   */
    uint32_t  count;    /* threads that have arrived so far */
    uint32_t  phase;    /* toggles 0/1 to prevent spurious release */
    mutex_t   mu;
    condvar_t cv;
} barrier_t;

void barrier_wait(barrier_t *b) {
    mutex_lock(&b->mu);
    uint32_t my_phase = b->phase;
    if (++b->count == b->target) {
        b->count = 0;
        b->phase ^= 1;           /* next generation */
        condvar_broadcast(&b->cv);
    } else {
        while (b->phase == my_phase) condvar_wait(&b->cv, &b->mu);
    }
    mutex_unlock(&b->mu);
}
```

- [ ] Define `barrier_t` struct: `target`, `count`, `phase`, `mutex_t`, `condvar_t`
- [ ] Implement `barrier_init(&b, count)` — set target, zero count, init mu + cv
- [ ] Implement `barrier_wait(&b)` — phase-based (handles spurious wakeups, reusable)
- [ ] Return `BARRIER_SERIAL_THREAD` (non-zero) to exactly one thread per generation
- [ ] Implement `barrier_destroy(&b)` — asserts no threads are currently waiting
- [ ] Add `barrier_wait_timeout(&b, timeout_ms)` — using §19 unified timeout API
- [ ] Wire compositor frame sync to use `barrier_wait` instead of ad-hoc semaphores
- [ ] Test: N=4 threads, verify none proceed until all 4 have called `barrier_wait`
- [ ] Test: reuse same barrier across 3 generations without reinit
- [ ] Commit: `"sched: pthread_barrier_t"`

---

## 30. KCSAN — Kernel Concurrency Sanitizer

**Prompt:** KCSAN is a dynamic data-race detector for the kernel. It instruments
every memory read and write (via compiler instrumentation, `-fsanitize=thread`
adapted for the kernel) and reports when two threads access the same memory
location concurrently without holding a common lock, and at least one of the
accesses is a write. This catches the class of bugs that §16 `lockdep` cannot:
unprotected shared variables that are accessed without any lock at all.

Linux added KCSAN in v5.8 (2020). Windows has no equivalent in-kernel
data-race detector. For a production OS, shipping without a data-race
detector means shipping with unknown races in the codebase. KCSAN runs in
debug builds only (`#ifdef KCSAN`) with zero overhead in release.

After completing all items, mark every item as `[x]`, update this prompt
to a verification prompt, run `bash scripts/build.sh clean`, and commit as
`"debug: KCSAN kernel concurrency sanitizer"`. Add notes on the
instrumentation mechanism and how to suppress known-benign races
(`data_race()` annotation).

> **Complements §16 lockdep:** lockdep detects lock ordering violations.
> KCSAN detects unprotected shared memory access. Together they form a
> complete concurrency correctness suite.

> [!NOTE]
> KCSAN requires compiler support. Verify that `x86_64-elf-gcc` supports
> `-fsanitize=thread` or adapt to use `__sanitizer_*` callbacks manually.

- [ ] Implement `__tsan_read*` / `__tsan_write*` callback stubs in `src/kernel/debug/kcsan.c`
- [ ] Add per-access shadow cell: `{thread_id, pc, is_write}` stored in a parallel shadow map
- [ ] On every instrumented access: compare current thread with shadow cell thread
- [ ] If mismatch and at least one is a write and no common lock held: report data race
- [ ] Race report: log `[KCSAN] data race: %s (tid=%d, pc=%p) vs %s (tid=%d, pc=%p)` to serial
- [ ] Annotate known-benign racy accesses with `READ_ONCE()` / `WRITE_ONCE()` / `data_race()`
- [ ] Compile with `KCSAN=1 make` flag — zero overhead in normal builds
- [ ] Test: create two threads with an unsynchronized counter, verify KCSAN fires
- [ ] Test: annotate with `data_race()`, verify KCSAN is suppressed
- [ ] Commit: `"debug: KCSAN kernel concurrency sanitizer"`

---

## 31. TSX / HTM Lock Elision (Hardware Transactional Memory)

**Prompt:** Intel TSX (Transactional Synchronization Extensions) allows the
CPU to execute a lock’s critical section speculatively without acquiring the
lock at all — if no concurrent access occurs, the transaction commits with
zero synchronization overhead. If a conflict is detected, the CPU aborts and
falls back to the normal lock path. For high-contention spinlocks and mutexes
on real hardware, lock elision can double throughput.

Linux added HTM lock elision for `pthread_mutex` on power and x86, then
disabled it on x86 due to TAA (TSX Asynchronous Abort, CVE-2019-11135).
Intel has since disabled TSX by microcode on most affected CPUs. AMD has
a similar feature (AMD TSXE). For a production OS targeting real hardware,
this decision must be made explicitly: probe CPUID for TSX support, check
for the TAA microcode patch, and enable only on safe configurations.

After completing all items, mark every item as `[x]`, update this prompt
to a verification prompt, run `bash scripts/build.sh clean`, and commit as
`"perf: TSX/HTM lock elision"`. Add notes on the TAA vulnerability,
CPUID probing, and the fallback path.

> [!CAUTION]
> **TSX Security:** TAA (CVE-2019-11135) allows a malicious process to leak
> kernel memory via TSX aborts on affected Intel CPUs. NEVER enable TSX lock
> elision without first checking `CPUID[EAX=7].EBX[bit 11] == 1` (RTM) **AND**
> confirming the TAA microcode mitigation is applied
> (`MSR_IA32_TSX_CTRL` bit available). If in doubt, leave disabled.

> [!NOTE]
> QEMU does not support TSX (`CPUID[eax=07h].EBX.hle` warning in boot log).
> This section is Future/P3 — implement only when targeting real Intel hardware.

- [ ] Probe CPUID for RTM (TSX-NI) support: `EAX=7, EBX bit 11`
- [ ] Check `MSR_IA32_TSX_CTRL` for TAA mitigation (abort if not patched)
- [ ] Wrap `mutex_lock()` with `XBEGIN` / `XEND` / `XABORT` RTM instructions
- [ ] On `XABORT` or conflict: fall back to normal `mutex_lock()` (existing path)
- [ ] Track elision success rate per mutex: if < 50% succeed, disable elision for that lock
- [ ] Configurable via Registry `SYSTEM\Perf\TSXEnabled` (default: auto-detect)
- [ ] Test: high-contention mutex benchmark — measure throughput with/without TSX
- [ ] Commit: `"perf: TSX/HTM lock elision"`

---

## OS Comparison

| Feature                        | 🪟 Windows 11 Kernel          | 🐧 Linux Kernel             | 🚀 Impossible OS                              |
| ------------------------------ | ----------------------------- | ---------------------------- | --------------------------------------------- |
| Kernel Threads                 | ✅ `KTHREAD`                  | ✅ `task_struct`            | ✅ §1 Done                                    |
| Mutexes                        | ✅ `KMUTEX`                   | ✅ `mutex_t`                | ✅ §2 Done                                    |
| Semaphores                     | ✅ `KSEMAPHORE`               | ✅ `semaphore`              | ✅ §3 Done                                    |
| Read-Write Locks               | ✅ `ERESOURCE`                | ✅ `rwlock_t`               | ✅ §4 Done                                    |
| Condition Variables            | ✅ (user-mode)                | ✅ `wait_queue`             | ✅ §5 Done                                    |
| Spinlocks (IRQ-safe)           | ✅ `KSPIN_LOCK`               | ✅ `spinlock_t`             | ⬜ §6 P0                                      |
| Atomic Operations              | ✅ `Interlocked*`             | ✅ `atomic_t`               | ⬜ §7 P0                                      |
| Wait/Event Objects             | ✅ `KEVENT`                   | ✅ `completion`             | ⬜ §8 P1                                      |
| Work Queues                    | ✅ DPC + work items           | ✅ `workqueue_struct`       | ⬜ §9 P1                                      |
| Memory Barriers                | ✅ `KeMemoryBarrier`          | ✅ `mb()`/`rmb()`/`wmb()`   | ⬜ §10 P0                                     |
| Priority Inheritance           | ⚠️ Heuristic only             | ⚠️ Opt-in `rt_mutex`        | ⬜ §11+17 **Default on all mutexes**          |
| Seqlocks                       | ❌                            | ✅ `seqlock_t`              | ⬜ §12 P2                                     |
| RCU                            | ❌                            | ✅ `rcu_*`                  | ⬜ §13 P2                                     |
| SMP / Per-CPU                  | ✅ Full NUMA                  | ✅ Full NUMA                | ⬜ §14 Phase 2 — after §6/7/10/11/26                                 |
| Futexes                        | ✅ (user-mode)                | ✅ `futex()`                | ⬜ §15 P1                                     |
| Lock Validator                 | ✅ Driver Verifier            | ✅ `lockdep`                | ⬜ §16 P2                                     |
| **PI on by default**           | ❌ Heuristic                  | ❌ Opt-in only              | ⬜ **§17 — Impossible OS only**               |
| **Wait-on-multiple**           | ✅ `WaitForMultiple`          | ❌ FDs only                 | ⬜ **§18 — beats Linux**                      |
| **Unified `_timeout(ms)` API** | ❌ Inconsistent               | ❌ Inconsistent             | ⬜ **§19 — Impossible OS only**               |
| **Graphical deadlock diagram** | ❌ BSOD only                  | ❌ Text dmesg only          | ⬜ **§20 — Impossible OS only**               |
| **Named lock browser**         | ❌                            | ❌ File locks only          | ⬜ **§21 — Impossible OS only**               |
| **Thread-Local Storage (TLS)** | ✅ Full TEB                   | ✅ `pthread_key_*`          | ⬜ §22 P1 — `FS`-base, ELF `PT_TLS`           |
| **Ticket locks (fair)**        | ❌ CAS spinlocks only         | ⚠️ Queued spinlocks (SMP)   | ⬜ **§23 P0 — FIFO ordering**                 |
| **Kernel watchdog**            | ✅ KeBugCheck timeout         | ✅ `CONFIG_LOCKUP_DETECTOR` | ⬜ §24 P1 — configurable via Registry         |
| **Stack guard pages**          | ✅ Automatic (Win32 stack)    | ✅ `MAP_STACK` + `SIGSEGV`  | ⬜ §25 P0 — `vmm_map_guard` per thread        |
| **Preemption count**           | ✅ `KeEnterCriticalRegion`    | ✅ `preempt_disable/enable` | ⬜ §26 P0 — prerequisite for RCU              |
| **Thread cancellation**        | ✅ `TerminateThread` (unsafe) | ✅ `pthread_cancel`         | ⬜ §27 P2 — deferred only, safe               |
| **pthread_once / once_flag**   | ✅ `InitOnceExecuteOnce`      | ✅ `pthread_once`           | ⬜ §28 P1 — ~15 lines, zero overhead          |
| **pthread_barrier_t**          | ❌ No equivalent              | ✅ `pthread_barrier_t`      | ⬜ **§29 P1 — kernel-native, beats Windows**  |
| **KCSAN data-race detector**   | ❌ No equivalent              | ✅ `CONFIG_KCSAN` (v5.8+)   | ⬜ **§30 P2 — beats Windows, matches Linux**  |
| **TSX/HTM lock elision**       | ❌ No equivalent              | ⚠️ Disabled (TAA CVE-2019)  | ⬜ §31 Future/P3 — safe-only, CPUID gated     |

> **After §17–29:** Impossible OS exceeds BOTH Windows 11 and Linux in lock safety, ergonomics, and observability.
> **After §30:** Impossible OS matches Linux KCSAN and exceeds Windows (no equivalent) in race detection.
> **After §31:** Impossible OS gains a hardware performance optimization that Linux disabled for safety — but with explicit TAA gating.
