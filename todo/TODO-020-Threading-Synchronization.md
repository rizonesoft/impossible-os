# P0102 — Threading & Synchronization

> **Goal:** Kernel thread infrastructure, mutual exclusion, counting semaphores,
> read-write locks, and condition variables — the foundation for concurrent kernel
> tasks, IPC, and GUI app isolation.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Kernel Threads ✅

**Prompt:** This section is marked complete. Verify the implementation is correct and consistent: review `src/kernel/sched.c` and `include/task.h` to confirm `thread_t` struct has id, stack_ptr, stack_base, stack_size, state, parent_task fields, that `thread_create`, `thread_exit`, `thread_join`, `thread_yield` all exist and work, and that the scheduler iterates threads. Run `bash scripts/build.sh clean` and test two threads sharing a global variable. Fix any inconsistencies in the TODO items below. Confirm the commit `"sched: kernel threads"` exists in git history. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to threading. Create or update documentation in `docs/` covering the threading model, thread API, and scheduler architecture.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: review that `mutex_t` struct, `mutex_lock`, `mutex_unlock`, `mutex_trylock` exist and function correctly. Confirm the lock uses compare-and-swap with a wait queue (not busy spinning). Run `bash scripts/build.sh clean` and test mutexes protecting shared state between threads. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to synchronization primitives. Create or update documentation in `docs/` covering the mutex API and implementation details.


- [x] Define `mutex_t` struct (locked flag, owner thread, wait queue)
- [x] Implement `mutex_lock(m)` — block if already locked (CAS → sleep, no busy-spin)
- [x] Implement `mutex_unlock(m)` — release, wake one blocked thread
- [x] Implement `mutex_trylock(m)` — non-blocking attempt, returns 0 on success
- [x] Add deadlock detection (lock ordering check)
- [x] Commit: `"sched: mutex synchronization"`

---

## 3. Semaphores ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: review that `semaphore_t` struct with count and wait queue exists, that `sem_wait`, `sem_signal`, `sem_init` all function correctly. Run `bash scripts/build.sh clean`. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to semaphores. Create or update documentation in `docs/` covering the semaphore API and usage patterns.


- [x] Define `semaphore_t` struct (count, wait queue)
- [x] Implement `sem_wait(s)` — decrement, block if count < 0
- [x] Implement `sem_signal(s)` — increment, wake one waiter
- [x] Implement `sem_init(s, initial_count)`
- [x] Commit: `"sched: semaphore synchronization"`

---

## 4. Read-Write Locks ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: review `include/kernel/sched/rwlock.h` to confirm `rwlock_t` struct has `reader_count`, `writer_held`, `writer_pending`, and separate reader/writer wait queues. Confirm `rwlock_read_lock`, `rwlock_read_unlock`, `rwlock_write_lock`, `rwlock_write_unlock`, `rwlock_try_read`, `rwlock_try_write` all exist in `src/kernel/sched/rwlock.c`. Verify that `writer_pending` prevents new reader acquisitions while a writer is waiting (starvation guard). Run `bash scripts/build.sh clean` and confirm `build/kernel/sched/rwlock.o` appears in the linker output. Confirm the commit `"sched: read-write locks"` exists in git history. After verifying, mark all items as `[x]` and update this prompt for future correctness checks. Create or update documentation in `docs/` covering the rwlock API, starvation prevention design, and usage examples.


- [x] Define `rwlock_t` struct (reader count, writer flag, writer_pending, separate read/write wait queues)
- [x] Implement `rwlock_read_lock(rw)` / `rwlock_read_unlock(rw)`
- [x] Implement `rwlock_write_lock(rw)` / `rwlock_write_unlock(rw)`
- [x] Implement `rwlock_try_read(rw)` / `rwlock_try_write(rw)` — non-blocking
- [x] Writer starvation guard: `writer_pending` blocks new readers while a writer waits
- [x] Use for: VFS mount table, loaded module list, process table
- [x] Commit: `"sched: read-write locks"`

---

## 5. Condition Variables ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: review `include/kernel/sched/condvar.h` to confirm `condvar_t` struct has a waiter queue matching the mutex/semaphore pattern. Confirm `cond_wait`, `cond_signal`, `cond_broadcast` all exist in `src/kernel/sched/condvar.c`. Verify that `cond_wait` calls `mutex_unlock` before `yield()` and `mutex_lock` after waking. Verify `cond_broadcast` wakes all waiters and clears the queue. Run `bash scripts/build.sh clean` and confirm `build/kernel/sched/condvar.o` appears in the linker output. Confirm the commit `"sched: condition variables"` exists in git history. Update `README.md` if it contains stale or incorrect references to synchronization. Check `docs/kernel/condvar.md` exists and covers the API, atomic mutex-release semantics, and producer-consumer usage.


- [x] Define `condvar_t` struct (waiter\_tasks, waiter\_threads, num\_waiters, name)
- [x] Implement `cond_wait(cond, mutex)` — enqueue → block → unlock mutex → yield → re-lock mutex
- [x] Implement `cond_signal(cond)` — wake one waiter (FIFO), remove from queue
- [x] Implement `cond_broadcast(cond)` — wake all waiters, clear queue
- [x] Graceful degradation when wait queue is full (spin-yield fallback with warning)
- [x] Commit: `"sched: condition variables"`

---

## 6. Spinlocks (IRQ-Safe)

**Prompt:** Spinlocks are the only safe synchronization primitive inside interrupt handlers — they busy-wait using atomic CAS without calling `yield()` or the scheduler. `spin_lock(s)` disables interrupts on the current CPU and spins until the lock is acquired. `spin_unlock(s)` releases and restores interrupts. `spin_lock_irqsave(s, flags)` / `spin_unlock_irqrestore(s, flags)` save/restore the interrupt flag for nested usage. This is equivalent to Windows `KSPIN_LOCK` and Linux `spinlock_t`. Spinlocks must only be held for very short durations (< ~100 ns) — they are for protecting small critical sections inside IRQ handlers. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: spinlocks (IRQ-safe)"`. Create or update documentation in `docs/` covering spinlock usage and IRQ safety rules.


- [ ] Implement `spinlock_t` (volatile uint32_t flag)
- [ ] Implement `spin_lock(s)` — disable interrupts, CAS loop until acquired
- [ ] Implement `spin_unlock(s)` — release flag, restore interrupts
- [ ] Implement `spin_lock_irqsave(s, flags)` — save RFLAGS, disable IRQs, acquire
- [ ] Implement `spin_unlock_irqrestore(s, flags)` — release, restore RFLAGS
- [ ] Use in: PIT IRQ handler (timer queue), keyboard IRQ, NIC receive path
- [ ] Commit: `"sched: spinlocks (IRQ-safe)"`

---

## 7. Atomic Operations

**Prompt:** Atomic operations are fundamental to lock-free data structures and the implementation of higher-level primitives (spinlocks, reference counting). Wrap GCC's `__sync_*` / `__atomic_*` builtins in a thin `kernel/atomic.h` header for portability and clarity. Key operations: `atomic_read`, `atomic_set`, `atomic_inc`, `atomic_dec`, `atomic_dec_and_test` (reference counting), `atomic_cmpxchg` (CAS), `atomic_fetch_add`. These are used by: `rwlock_t` reader count, kernel object reference counts, lock-free ring buffers, and the slab allocator. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: atomic operations"`. Create or update documentation in `docs/` covering the atomic API and memory ordering guarantees.


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

**Prompt:** Event objects are one-shot or auto-reset synchronization handles — equivalent to Windows `KEVENT` (manual-reset and auto-reset) and Linux `completion`. A manual-reset event: `event_set` wakes all waiters; remains set until `event_reset` clears it. An auto-reset event: `event_set` wakes exactly one waiter and immediately clears itself. These are ideal for: boot synchronization (wait for disk init to complete), vsync signal from PIT to compositor, driver initialization handshakes. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: wait/event objects"`. Create or update documentation in `docs/` covering event types, wait semantics, and usage examples.


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

**Prompt:** IRQ handlers must be short — they run with interrupts disabled and cannot sleep, yield, or call VFS. Work queues solve this by deferring slow work to a kernel thread that runs in normal context. An IRQ handler calls `workqueue_enqueue(wq, func, arg)` to schedule a callback — the work queue kernel thread picks it up and calls `func(arg)` in a yieldable context. This is equivalent to Linux `workqueue_struct` / `schedule_work()` and Windows DPC + work items. Used by: NIC receive path (packet processing after DMA), disk IRQ (completing async I/O), USB events. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: work queues"`. Create or update documentation in `docs/` covering the workqueue API, IRQ-safety rules, and work item lifecycle.


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

**Prompt:** Memory barriers prevent the CPU and compiler from reordering memory accesses across synchronization boundaries. Without them, lock-free code and even spinlock implementations can silently mis-order on modern out-of-order CPUs. Implement as lightweight macros using GCC's built-ins and x86 `MFENCE`/`LFENCE`/`SFENCE` instructions. Add `barrier()` (compiler-only fence) and `mb()`/`rmb()`/`wmb()` (full/read/write memory barriers). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: memory barriers"`. Create or update documentation in `docs/` covering when to use each barrier type.

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

## Priority Order

| Priority | Section                    | Reason                                                        |
|----------|----------------------------|---------------------------------------------------------------|
| ✅ Done   | 1. Kernel Threads          | Verified complete                                             |
| ✅ Done   | 2. Mutexes                 | Verified complete                                             |
| ✅ Done   | 3. Semaphores              | Verified complete                                             |
| ✅ Done   | 4. Read-Write Locks        | Implemented — rwlock.o linked, BUILD OK                       |
| ✅ Done   | 5. Condition Variables     | Implemented — condvar.o linked, BUILD OK                      |
| 🔴 P0     | 10. Memory Barriers        | Prerequisite for correct spinlocks and atomics                |
| 🔴 P0     | 7. Atomic Operations       | Prerequisite for spinlocks and reference counting             |
| 🔴 P0     | 6. Spinlocks               | Needed for IRQ-safe locking in PIT, keyboard, NIC handlers    |
| 🟠 P1     | 8. Wait/Event Objects      | Boot sync, vsync, driver handshakes — cleaner than semaphores |
| 🟠 P1     | 9. Work Queues             | Required for proper IRQ bottom-half processing                |
