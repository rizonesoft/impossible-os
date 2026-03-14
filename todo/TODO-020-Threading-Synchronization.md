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

## 5. Condition Variables

**Prompt:** Condition variables pair with mutexes to allow a thread to atomically release a mutex and sleep until a condition is signalled. `cond_wait(cond, mutex)` releases the mutex, adds the thread to the wait queue, and sleeps; on wake it re-acquires the mutex. `cond_signal` wakes one waiter; `cond_broadcast` wakes all. These are essential for producer-consumer patterns (e.g., the compositor's vsync wait, the shell's input wait). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"sched: condition variables"`.


- [ ] Define `condvar_t` struct (wait queue)
- [ ] Implement `cond_wait(cond, mutex)` — atomically release mutex and sleep
- [ ] Implement `cond_signal(cond)` — wake one waiter
- [ ] Implement `cond_broadcast(cond)` — wake all waiters
- [ ] Test: producer-consumer queue using condvar + mutex
- [ ] Commit: `"sched: condition variables"`

---

## Priority Order

| Priority | Section                  | Reason                                         |
|----------|--------------------------|------------------------------------------------|
| ✅ Done   | 1. Kernel Threads        | Verified complete                              |
| ✅ Done   | 2. Mutexes               | Verified complete                              |
| ✅ Done   | 3. Semaphores            | Verified complete                              |
| 🟠 P1     | 4. Read-Write Locks      | Needed for VFS/module table scalability        |
| 🟠 P1     | 5. Condition Variables   | Needed for compositor vsync, shell input wait  |
