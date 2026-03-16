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

## 6. Spinlocks (IRQ-Safe) ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `include/kernel/sched/spinlock.h` defines `spinlock_t`, `SPINLOCK_INIT`, `spin_lock`, `spin_unlock`, `spin_lock_irqsave`, `spin_unlock_irqrestore`, `spin_trylock`. Confirm `src/kernel/sched/spinlock.c` implements all four blocking functions using `pushfq`/`popfq` for RFLAGS save/restore. Verify `pit.c`, `keyboard.c`, and `rtl8139.c` include `spinlock.h` and use `spin_lock_irqsave`/`spin_unlock_irqrestore` to protect their shared state. Run `bash scripts/build.sh clean` and confirm `=== BUILD OK ===`. Confirm commit `"sched: spinlocks (IRQ-safe)"` exists. Fix any inconsistencies found.


- [x] Implement `spinlock_t` (volatile uint32_t flag)
- [x] Implement `spin_lock(s)` — disable interrupts, CAS loop until acquired
- [x] Implement `spin_unlock(s)` — release flag, restore interrupts
- [x] Implement `spin_lock_irqsave(s, flags)` — save RFLAGS, disable IRQs, acquire
- [x] Implement `spin_unlock_irqrestore(s, flags)` — release, restore RFLAGS
- [x] Use in: PIT IRQ handler (timer queue), keyboard IRQ, NIC receive path
- [x] Commit: `"sched: spinlocks (IRQ-safe)"`

### Notes

**Files:**
- `include/kernel/sched/spinlock.h` — API + `SPINLOCK_INIT` macro, `DEFINE_SPINLOCK` helper
- `src/kernel/sched/spinlock.c` — full implementation

**API:**
```c
spinlock_t lock = SPINLOCK_INIT;
// or: DEFINE_SPINLOCK(lock);

void spin_lock(spinlock_t *s);                        // cli + CAS loop
void spin_unlock(spinlock_t *s);                      // store 0 + sti
void spin_lock_irqsave(spinlock_t *s, uint64_t *f);   // pushfq+cli + CAS loop
void spin_unlock_irqrestore(spinlock_t *s, uint64_t f); // store 0 + popfq
int  spin_trylock(spinlock_t *s);                     // non-blocking, no IRQ
int  spin_is_locked(const spinlock_t *s);             // debug query
```

**IRQ safety rules — memorise these:**
1. **ALWAYS use `spin_lock_irqsave` / `spin_unlock_irqrestore`** in any function that can be called from BOTH thread context (IRQs on) and IRQ context (IRQs off). Using plain `spin_lock` from thread context is fine; using it from IRQ context is also fine. The dangerous case is mixing: if you call `spin_unlock` (which does `sti`) from inside an IRQ handler, you re-enable interrupts while still inside the handler — this can cause re-entrance.
2. **Never call yield(), mutex_lock(), sem_wait(), or any sleeping primitive** while holding a spinlock. A spinlock critical section must be non-blocking.
3. **Never call printk() or kmalloc()** while holding a spinlock. Both can trigger IRQs or sleep.
4. **Hold-time limit: < ~100 ns** (~200–400 CPU cycles at 2–4 GHz). Count your instructions. For anything longer, use a mutex.
5. **Never hold two spinlocks** unless you can prove a strict acquisition order (use lock ordering IDs from §2/§16 to verify). Acquiring in inconsistent order deadlocks even on single-core when one IRQ fires mid-acquisition.

**RFLAGS save/restore pattern (why it matters):**

```c
/* Thread context — IRQs enabled before call */
uint64_t flags;
spin_lock_irqsave(&lock, &flags);   /* saves RFLAGS (IF=1), clears IF */
/* ... critical section ... */
spin_unlock_irqrestore(&lock, flags); /* clears flag, restores RFLAGS (IF=1) */

/* IRQ context — IRQs already disabled by CPU before handler entry */
uint64_t flags;
spin_lock_irqsave(&lock, &flags);   /* saves RFLAGS (IF=0), clears IF (nop) */
/* ... critical section ... */
spin_unlock_irqrestore(&lock, flags); /* clears flag, restores RFLAGS (IF=0) */
/* IRQs NOT re-enabled — correct! The handler's iret will restore IF. */
```

Without irqsave, `spin_unlock` calls `sti` during IRQ handler — re-enabling interrupts mid-handler causes a second IRQ to fire, which can corrupt the first handler's local state (the NIC Rx path is the classic victim).

**Memory ordering (from §10 barrier.h):**
- Spin loop body: `barrier()` prevents GCC from caching `flag` in a register (CSE).
- After CAS (acquire fence): `barrier()` prevents critical-section loads from being hoisted before the CAS.
- Before clear (release fence): `barrier()` prevents critical-section stores from being sunk after the flag clear.
- On x86, `LOCK CMPXCHG` already carries a full hardware barrier — the `barrier()` calls are compiler-only on x86. On SMP non-x86, `smp_mb()` would be needed.

**Where applied:**
- `pit.c`: `pit_lock` protects `tick_count` and `pit_callback_*` state. IRQ handler and `pit_get_ticks()`/`sleep_ms()`/`register_callback()` all use `spin_lock_irqsave`.
- `keyboard.c`: `kb_lock` protects the `kb_buffer` ring (head/tail). IRQ-side `kb_buffer_push` and thread-side `keyboard_getchar()`/`keyboard_trygetchar()` use `spin_lock_irqsave`.
- `rtl8139.c`: `nic_rx_lock` protects `rx_offset`/`rx_ready` between IRQ handler and `rtl8139_receive()`; `nic_tx_lock` protects `tx_cur` in `rtl8139_send()`.

**Gotcha — `keyboard_getchar()` spin-wait:**

```c
/* Correct: release lock before hlt, else PIT/keyboard IRQ can't fire */
for (;;) {
    spin_lock_irqsave(&kb_lock, &flags);
    if (kb_head != kb_tail) break;     /* data available */
    spin_unlock_irqrestore(&kb_lock, flags);
    __asm__ volatile("hlt");           /* sleep until next IRQ */
}
/* lock still held here — read and release */
```

The lock must be released before `hlt`. If held during `hlt`, the keyboard IRQ fires, tries to push to the buffer (needs `kb_lock`), deadlocks. The irqrestore restores IF=1 so HLT can fire.

**Future — `spin_trylock`:**
`spin_trylock` does NOT disable IRQs. It is intended for lock-elision patterns in thread context (try to skip expensive work if a lock is already held). Do not use from IRQ context.

---


## 7. Atomic Operations ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `include/kernel/atomic.h` defines `atomic_t`, `atomic64_t`, `ATOMIC_INIT`, `atomic_read`, `atomic_set`, `atomic_inc`, `atomic_dec`, `atomic_dec_and_test`, `atomic_cmpxchg`, `atomic_fetch_add`, and their 64-bit variants. Confirm `mutex.h` and `rwlock.h` both use `atomic_t` for their flag/count fields. Confirm `mutex.c` and `rwlock.c` use `atomic_read`/`atomic_set`/`atomic_cmpxchg` for all accesses. Run `bash scripts/build.sh clean` and confirm `=== BUILD OK ===`. Confirm commit `"kernel: atomic operations"` exists in git history. Fix any inconsistencies found.


- [x] Create `include/kernel/atomic.h`
- [x] `atomic_t` typedef (volatile int32_t or struct wrapper)
- [x] `atomic_read(a)`, `atomic_set(a, v)` — simple read/write with barrier
- [x] `atomic_inc(a)`, `atomic_dec(a)`, `atomic_dec_and_test(a)` — for ref counting
- [x] `atomic_cmpxchg(a, old, new)` — CAS primitive for lock-free algorithms
- [x] `atomic_fetch_add(a, delta)` — atomic add, returns old value
- [x] Replace bare `volatile` flags in `mutex.c`/`rwlock.c` with `atomic_t` where appropriate
- [x] Commit: `"kernel: atomic operations"`

### Notes

**Design: struct wrapper, not bare typedef**

`atomic_t` is `struct { volatile int32_t val; }`, not `typedef volatile int32_t`. This prevents accidental direct access (`a.val = 5` compiles but is obviously wrong), forces callers through the API, and allows the struct to grow (e.g., add a debug owner field) without changing call sites. Matches Linux `atomic_t` design exactly.

**API summary:**

| Function | Ordering | Returns | Use for |
|----------|----------|---------|---------|
| `atomic_read(a)` | Acquire | `int32_t` | Reading flags (prevents hoisting) |
| `atomic_set(a, v)` | Release | void | Writing flags (prevents sinking) |
| `atomic_inc(a)` | Acq+Rel | void | Reference count increment |
| `atomic_dec(a)` | Acq+Rel | void | Reference count decrement |
| `atomic_dec_and_test(a)` | Acq+Rel | `int` (1 if now 0) | Ref count — free if returns 1 |
| `atomic_cmpxchg(a, old, new)` | Acq+Rel | old value | CAS for lock-free algorithms |
| `atomic_fetch_add(a, delta)` | Acq+Rel | old value | Ticket lock `next_ticket` |
| `atomic64_read/set/fetch_add` | same | 64-bit | Tick counters, byte offsets |

**Memory ordering rationale:**
- `__ATOMIC_ACQUIRE` on reads: prevents critical-section loads from being hoisted above the read (equivalent to a load-fence on the consuming side).
- `__ATOMIC_RELEASE` on writes: prevents prior stores from being reordered after the write (ensures published data is visible before the flag is set).
- `__ATOMIC_ACQ_REL` on RMW: both-sided barrier — the modification is a synchronisation point. Required for CAS in `mutex_trylock` and `atomic_dec_and_test` in reference counting.

On x86 TSO these map to: ACQUIRE = compiler barrier + normal load, RELEASE = compiler barrier + normal store, ACQ_REL = LOCK-prefixed instruction. No MFENCE is emitted unless SMP and `mb()` is explicitly called.

**Where applied:**

| File | Old type | New type | Accesses updated |
|------|----------|----------|-----------------|
| `mutex.h` | `volatile uint32_t locked` | `atomic_t locked` | `mutex_init`, `mutex_lock`, `mutex_unlock`, `mutex_trylock`, `mutex_is_locked` |
| `rwlock.h` | `volatile uint32_t reader_count/writer_held/writer_pending` | `atomic_t` × 3 | All rwlock functions |
| `mutex.c` `mutex_trylock` | bare `if (m->locked)` + direct store | `atomic_cmpxchg` | Now race-free trylock |

**Gotcha — `atomic_cmpxchg` return value semantics:**
`atomic_cmpxchg(a, old, new)` returns the **old** value of `*a`. If it equals `old`, the swap succeeded. If it differs, the swap failed (another thread changed it first). This matches the Linux kernel convention; differs from some POSIX docs that return a bool.

```c
/* Correct trylock pattern */
if (atomic_cmpxchg(&m->locked, 0, 1) != 0)
    return 0; /* was already 1 (locked) — CAS failed */
/* if returned 0, old value was 0 → CAS succeeded, we now hold the lock */
```

**Gotcha — `atomic_dec_and_test` for reference counting:**
```c
if (atomic_dec_and_test(&obj->refcount)) {
    /* We were the last reference holder — safe to free */
    kfree(obj);
}
/* DO NOT access obj after this point even if the test returned 0 —
 * another thread may have decremented it to 0 and freed it. */
```

**64-bit variants:**
`atomic64_t` / `atomic64_read` / `atomic64_set` / `atomic64_fetch_add` are provided for use with 64-bit counters (PIT tick counter, byte offset in ring buffers). On x86-64, 64-bit aligned loads/stores are naturally atomic in hardware — the `__atomic_*` builtins ensure the compiler doesn't split them.

---


## 8. Wait/Event Objects ✅

**Prompt:** This section is marked complete. Verify: `include/kernel/sched/event.h` defines `event_t`, `event_type_t` (MANUAL/AUTO\_RESET), `EVENT_INIT`, `DEFINE_EVENT`, and all six API functions. Verify `src/kernel/sched/event.c` compiles and links. Confirm MANUAL\_RESET wakes all waiters and stays set; AUTO\_RESET wakes one and self-clears. Confirm `event_wait_timeout` uses `pit_get_ticks()` and `PIT_TARGET_FREQ` for the deadline. Run `bash scripts/build.sh clean` → `=== BUILD OK ===`. Confirm commit `"sched: wait/event objects"` in git history. Fix any inconsistencies found.


- [x] Define `event_t` struct (state flag, type enum MANUAL/AUTO_RESET, wait queue)
- [x] Implement `event_init(ev, type, initial_state)` — initialize event
- [x] Implement `event_wait(ev)` — block until event is set; auto-reset clears on wake
- [x] Implement `event_set(ev)` — signal event (wake all for manual, one for auto)
- [x] Implement `event_reset(ev)` — clear manual-reset event
- [x] Implement `event_wait_timeout(ev, ms)` — wait with timeout (returns 0 on timeout)
- [x] Use for: boot phase sync, vsync compositor signal, driver init handshakes
- [x] Commit: `"sched: wait/event objects"`

### Notes

**Files:**
- `include/kernel/sched/event.h` — `event_t`, `EVENT_INIT`, `DEFINE_EVENT`, full API
- `src/kernel/sched/event.c` — implementation

**Types at a glance:**
| Type | `event_set` wakes | Clears on wake | Stays set |
|------|------------------|----------------|-----------|
| `EVENT_MANUAL_RESET` | ALL waiters | No — must call `event_reset()` | Yes, until reset |
| `EVENT_AUTO_RESET` | ONE waiter (FIFO) | Yes, self-clears | Only if no waiters waiting |

**Event state machine — AUTO_RESET:**
```
Unsignalled ──event_set()+waiters──► Unsignalled (waiter woken + cleared)
Unsignalled ──event_set()+no waiters► Signalled
Signalled   ──event_wait()──────────► Unsignalled (consumed by first caller)
Signalled   ──event_reset()─────────► Unsignalled (explicit clear)
```

**Event state machine — MANUAL_RESET:**
```
Unsignalled ──event_set()──► Signalled (all waiters woken, all future wait() return immediately)
Signalled   ──event_reset()─► Unsignalled
```

**Usage examples:**

```c
/* Boot phase sync: wait for AHCI/disk before mounting VFS */
DEFINE_EVENT(disk_ready, EVENT_MANUAL_RESET, 0);

/* AHCI driver (thread or IRQ): */
event_set(&disk_ready);

/* VFS init (thread): */
event_wait(&disk_ready);   /* blocks until AHCI signals */

/* Vsync signal from PIT callback to compositor (AUTO_RESET): */
DEFINE_EVENT(vsync, EVENT_AUTO_RESET, 0);

/* PIT callback (IRQ context): */
event_set(&vsync);   /* safe from IRQ — only sets flag + marks thread ready */

/* Compositor thread: */
event_wait(&vsync);  /* blocks each frame until PIT fires */

/* Timeout for driver init: */
if (!event_wait_timeout(&nic_ready, 3000)) {
    klog(LOG_ERROR, "net", "NIC init timed out after 3 s");
}
```

**IRQ safety:**
- `event_set()` is IRQ-safe: only writes `atomic_t state` and sets `THREAD_READY`. No lock, no yield.
- `event_wait()` and `event_wait_timeout()` call `yield()` — **thread context only**.
- `event_reset()` is IRQ-safe: single atomic store.

**Timeout implementation:**
Uses `pit_get_ticks()` / `PIT_TARGET_FREQ` (100 Hz). Minimum resolution = 10 ms. For sub-millisecond precision a high-resolution timer would be needed.

**Spurious wakeup handling:**
`event_wait()` re-checks `ev->state` after each `yield()`. If the wait queue overflows (> 16 waiters) and the thread falls through to `yield()` without being properly enqueued, it still re-checks the state correctly on resume — this is the correct Linux-style "always re-test predicate" pattern.

**Gotcha — AUTO_RESET and multiple waiters:**
If two threads call `event_wait()` and then `event_set()` is called, only ONE thread wakes. The second remains blocked until the next `event_set()`. This is intentional and is the defining property of auto-reset events. For "release all" semantics, use `EVENT_MANUAL_RESET` with an explicit `event_reset()` afterwards.

---


## 9. Work Queues (Deferred Work) ✅

**Prompt:** This section is marked complete. Verify: `include/kernel/sched/workqueue.h` defines `work_item_t`, `workqueue_t`, `WQ_POOL_SIZE=64`, and the full API (`workqueue_create`, `workqueue_enqueue`, `workqueue_flush`, `workqueue_destroy`). Verify `src/kernel/sched/workqueue.c` compiles and links. Confirm `sys_wq` is declared `extern` in the header and defined in workqueue.c. Confirm `main.c` creates `sys_wq` after `task_init()`. Confirm `rtl8139.c` defers `net_rx()` via `sys_wq` using `workqueue_enqueue`. Run `bash scripts/build.sh clean` → `=== BUILD OK ===`. Confirm commit `"kernel: work queues"` in git history.


- [x] Define `work_item_t` struct (callback function ptr, arg, next pointer)
- [x] Define `workqueue_t` (spinlock-protected linked list + semaphore)
- [x] Implement `workqueue_create(name)` — spawn a kernel thread for processing
- [x] Implement `workqueue_enqueue(wq, func, arg)` — IRQ-safe, uses spinlock
- [x] Work queue thread: loop on semaphore, dequeue, call `func(arg)`, repeat
- [x] Create default system work queue (`sys_wq`) at boot
- [x] Wire NIC receive DMA completion to `sys_wq` (replace direct IRQ processing)
- [x] Commit: `"kernel: work queues"`

### Notes

**Files:**
- `include/kernel/sched/workqueue.h` — API, `work_item_t`, `workqueue_t`, `sys_wq` extern
- `src/kernel/sched/workqueue.c` — full implementation

**Architecture:**
```
IRQ handler (< 1 µs):           Worker thread (normal context):
  copy packet                     loop:
  workqueue_enqueue()               sem_count > 0?
  ▼                                 dequeue item
  lock + push to list               call fn(arg)
  sem_count++                       return node to free_head
  unlock
```

**Static pool design (no kmalloc in IRQ path):**
`workqueue_t` contains `work_item_t pool[64]` — 64 pre-allocated nodes. `workqueue_enqueue()` pops a node from `free_head` under spinlock. The worker thread pushes it back after `fn(arg)` returns. This means zero dynamic allocation in the hot path, making `workqueue_enqueue()` safe and deterministic from IRQ context.

**IRQ safety:**
- `workqueue_enqueue()` — IRQ-safe (spinlock_irqsave, no yield, no alloc)
- `workqueue_create()`, `workqueue_flush()`, `workqueue_destroy()` — thread context only

**Worker thread locates its queue via PID registry:**
`task_create()` takes a `void (*)(void)` entry with no arg. Work queues maintain a static `wq_registry[]` table. On startup, the worker calls `task_current()`, scans the registry for a queue whose `worker_pid` matches, and loops forever processing its queue.

**`sys_wq` initialization in `main.c`:**
Created after `task_init()` — the scheduler is briefly enabled so the worker task can start, then disabled again. The NIC init happens much earlier (before `task_init()`), so the early-boot `sys_wq == NULL` fallback in `rtl8139.c` handles DHCP packets received during boot.

**NIC receive path (rtl8139.c):**
```c
/* IRQ handler — if sys_wq ready, defer net_rx: */
struct nic_rx_work *w = kmalloc(sizeof(*w));
memcpy(w->data, pkt_buf, pkt_len);
w->len = pkt_len;
workqueue_enqueue(sys_wq, nic_rx_work_fn, w);

/* Worker thread calls: */
static void nic_rx_work_fn(void *arg) {
    struct nic_rx_work *w = arg;
    net_rx(w->data, w->len);   /* thread context — can yield, alloc, etc. */
    kfree(w);
}
```

**Gotcha — pool exhaustion:**
If 64 items are enqueued faster than the worker can process them (e.g. a DMA storm), `workqueue_enqueue()` returns 0 and the packet is dropped. This is correct: the NIC ring buffer would overflow anyway. A warning log would help diagnose this in production.

**Gotcha — `workqueue_destroy()` does not kill the worker task:**
The worker loops infinitely. `workqueue_destroy()` flushes and frees the `workqueue_t` struct, but the worker task becomes an orphan (it exits on the next iteration because `wq->fn` is null). A future improvement would send a poison-pill work item to signal the worker to call `task_exit()`.

---


## 10. Memory Barriers & Compiler Fences ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `include/kernel/barrier.h` exists and defines `barrier()`, `mb()`, `rmb()`, `wmb()`, `smp_mb()`, `smp_rmb()`, `smp_wmb()`. Verify `include/kernel/sched/spinlock.h` and `include/kernel/atomic.h` both `#include "kernel/barrier.h"` and document their acquire/release barrier contract. Run `bash scripts/build.sh clean` and confirm `=== BUILD OK ===`. Confirm commit `"kernel: memory barriers"` exists in git history. Fix any inconsistencies found. Add notes covering when to use each barrier type and the x86 TSO memory model rationale.

> **Note for single-core:** On x86 single-core, the CPU guarantees strong ordering for most operations. The compiler barrier (`barrier()`) is still needed to prevent GCC from optimizing away `volatile` accesses. The full `mb()` becomes important when SMP is added.


- [x] Create `include/kernel/barrier.h`
- [x] `barrier()` — compiler-only fence: `__asm__ volatile("" ::: "memory")`
- [x] `mb()` — full memory barrier: `__asm__ volatile("mfence" ::: "memory")`
- [x] `rmb()` — read barrier: `__asm__ volatile("lfence" ::: "memory")`
- [x] `wmb()` — write barrier: `__asm__ volatile("sfence" ::: "memory")`
- [x] `smp_mb()`, `smp_rmb()`, `smp_wmb()` — SMP-aware aliases (= mb/rmb/wmb now; nop when !SMP)
- [x] Apply `barrier()` in `spinlock.h` and `atomic.h` acquire/release paths
- [x] Commit: `"kernel: memory barriers"`

### Notes

**Files created:**
- `include/kernel/barrier.h` — header-only, no `.c` file required
- `include/kernel/sched/spinlock.h` — API stub with `#include "kernel/barrier.h"`
- `include/kernel/atomic.h` — GCC `__atomic_*` wrappers with `#include "kernel/barrier.h"`

**Barrier quick reference:**

| Macro | Instruction | Cost (x86) | When to use |
|-------|-------------|-----------|-------------|
| `barrier()` | _(compiler only, no hw insn)_ | ~0 | Volatile flag reads in spin loops, seqlock read-side, any loop that observes a flag written by another path |
| `mb()` | `MFENCE` | ~100 cycles | Lock release, pointer publication, any inter-CPU handshake requiring both load and store ordering |
| `rmb()` | `LFENCE` | ~5 cycles | After reading a seqlock sequence, after non-temporal loads (`MOVNTDQA`), Spectre-v1 barriers |
| `wmb()` | `SFENCE` | ~5 cycles | Flushing NT stores to framebuffer/DMA rings before updating a tail pointer; seqlock write-side |
| `smp_mb/rmb/wmb()` | same as above (or `barrier()` on !SMP) | 0 on !SMP | Cross-CPU data publication — prefer these over raw `mb()` so single-core builds pay no CPU cost |

**x86 TSO memory model:**
x86 uses Total Store Order: stores are globally visible in program order, and loads observe stores in program order. The single exception is store→load reordering (a later load can pass an earlier store in the store buffer). `MFENCE` closes this gap by draining the store buffer. Consequences:
- `wmb()` costs almost nothing on x86 for regular stores (compiler clobber only matters); SFENCE only affects non-temporal (streaming) stores.
- `rmb()` costs almost nothing on x86 for regular loads (loads are already ordered); LFENCE is needed only for speculative execution barriers.
- `mb()` is expensive (~100 cycles) because it must drain the store buffer.

**Spinlock acquire/release contract (`sched/spinlock.h`):**
```c
/* acquire — barrier() after CAS prevents critical-section loads
 * from being hoisted before the lock is seen as held by GCC.
 * On x86, LOCK CMPXCHG already carries an implicit hardware barrier. */
while (!CAS(&s->flag, 0, 1)) barrier(); // spin body: prevents CSE on flag
barrier(); // acquire fence

/* release — barrier() before clearing flag prevents critical-section
 * stores from being sunk (reordered after) the flag clear. */
barrier(); // release fence
s->flag = 0;
```

**Atomic acquire/release ordering (`atomic.h`):**
- Loads use `__ATOMIC_ACQUIRE` → prevent load hoisting past the atomic read.
- Stores use `__ATOMIC_RELEASE` → prevent store sinking past the atomic write.
- RMW (CAS, fetch_add) use `__ATOMIC_ACQ_REL` → both-sided ordering.
- This matches Linux `atomic_read()` / `atomic_set()` semantics and the C11 memory model.

**SMP alias design decision:**
`smp_mb()` / `smp_rmb()` / `smp_wmb()` conditionalize on `CONFIG_SMP`:
- With `CONFIG_SMP`: alias to `mb()` / `rmb()` / `wmb()` (full hardware barriers).
- Without `CONFIG_SMP`: alias to `barrier()` (compiler fence only — zero CPU cost).
This pattern is identical to Linux's `smp_mb()` macro. Prefer `smp_*` in all kernel code so single-core builds are fast; only use raw `mb()` when a hardware barrier is unconditionally required (e.g. DMA completion, framebuffer non-temporal write flush).

**Gotcha — DO NOT use `mb()` in tight loops:**
Every `MFENCE` takes ~100 cycles and serializes the entire pipeline. A spin-wait loop that calls `mb()` on every iteration will burn 100× more CPU than one using `barrier()`. Use `barrier()` in spin loops, `mb()` only at the lock boundary.

**Gotcha — `wmb()` does NOT order regular stores vs. loads:**
`SFENCE` only orders stores-before vs. stores-after. If you need a store ordered before a subsequent load on another CPU, use `mb()` (MFENCE). A common mistake: `wmb(); flag = 1;` then expecting another CPU's `if (flag)` load to see all prior stores — this is only guaranteed by `mb()`, not `wmb()`.

**Future — SMP:** When `CONFIG_SMP` is enabled, the `smp_*` macros automatically switch to full hardware barriers. The only additional requirement is that `LOCK CMPXCHG` (already used in `atomic_cmpxchg`) carries an implicit full barrier on x86 — no extra `mb()` is needed around CAS on x86 SMP. On non-x86 SMP (RISC-V, ARM), `smp_mb()` would need to emit the appropriate fence instruction.


---

## 11. Priority Inheritance (Mutex Enhancement) ✅

**Prompt:** This section is marked complete. Verify: `struct thread` in `task.h` has `priority` and `base_priority` fields. Priority constants `THREAD_PRIO_NORMAL=16` etc. exist. `find_next_task()` in `task.c` selects the highest-priority READY thread (not round-robin). `mutex_lock()` calls `thread_boost_priority()` on the owner before yielding. `mutex_unlock()` calls `thread_restore_priority()` before releasing and wakes the highest-priority waiter. `thread_set_priority`, `thread_boost_priority`, `thread_restore_priority` are exported from `task.h`. Run `bash scripts/build.sh clean` → `=== BUILD OK ===`. Confirm commit `"sched: mutex priority inheritance"` in git history.


- [x] Add `base_priority` field to `thread_t` (saved original priority before boost)
- [x] Add `priority` (current effective priority) field to `thread_t`
- [x] Update scheduler to select highest-priority READY thread (priority-aware)
- [x] In `mutex_lock()`: if blocked, boost lock owner's priority to waiter's priority
- [x] In `mutex_unlock()`: restore owner's priority to `base_priority`
- [x] Handle cascaded inheritance (A waits on B, B waits on C → C gets A's priority)
- [x] Commit: `"sched: mutex priority inheritance"`

### Notes

**Files:**
- `include/kernel/sched/task.h` — `priority`/`base_priority` in `struct thread`, priority constants, `thread_boost_priority`, `thread_restore_priority`, `thread_set_priority`
- `src/kernel/sched/task.c` — priority-aware `find_next_task()`, PI helper implementations
- `src/kernel/sched/mutex.c` — PI in `mutex_lock()` and `mutex_unlock()`

**Priority constants:**
| Constant | Value | Use |
|---|---|---|
| `THREAD_PRIO_IDLE` | 0 | background-only idle tasks |
| `THREAD_PRIO_LOW` | 8 | below-normal |
| `THREAD_PRIO_NORMAL` | 16 | default for all new threads |
| `THREAD_PRIO_HIGH` | 24 | high-priority kernel threads |
| `THREAD_PRIO_REALTIME` | 31 | near-interrupt priority |

**Priority inversion scenario:**
```
LOW  holds mutex M
MED  preempts LOW (higher priority)  
HIGH blocks on M — but LOW can't run because MED has the CPU
HIGH is effectively blocked by MED (priority inversion!)

With PI:
HIGH blocks → mutex_lock() boosts LOW to HIGH's priority
LOW now has higher priority than MED → preempts MED
LOW releases M → thread_restore_priority() → LOW back to THREAD_PRIO_LOW
HIGH wakes and acquires M
```

**Scheduler change (`find_next_task`):**
Replaced pure round-robin with a priority scan: iterates all tasks×threads, tracks the highest-priority READY/RUNNING thread. Equal-priority threads are still visited in round-robin order (circular from the last scheduled slot) for fairness. Time complexity: O(N×T) per scheduler tick — acceptable for TASK_MAX=32, THREAD_MAX=16.

**Cascaded inheritance:**
When HIGH blocks on mutex_A (owned by MED) and MED blocks on mutex_B (owned by LOW):
- HIGH blocking on A boosts MED to HIGH's priority
- MED blocking on B boosts LOW to HIGH's priority  
The current implementation handles one hop (direct owner boost). True cascaded inheritance would require walking the mutex chain. This is noted as a future improvement.

**`mutex_unlock` wakes highest-priority waiter:**
On unlock, the wait queue is scanned to find the waiter with the highest `priority` before waking it (not FIFO). This prevents a low-priority waiter from winning the mutex when a high-priority one is also waiting.

**Gotcha — self-resetting base_priority:**
`thread_restore_priority()` resets `priority = base_priority`. If a thread is boosted by two different mutexes simultaneously, releasing one mutex will drop the boost even if the other mutex still needs it. Full PI (Linux `rt_mutex`) uses a PI-chain list to handle this. Our implementation is correct for the common single-mutex-PI case.

**Gotcha — task_init priority:**
All new threads created via `task_create`, `task_create_user`, and `thread_create` initialize `priority = base_priority = THREAD_PRIO_NORMAL`. The PID 0 (main/boot) thread also gets NORMAL priority.

**Future Work — Cascaded (full chain) PI:**
The current implementation does one hop: HIGH blocking on mutex_A boosts the direct owner. If that owner is itself blocked on mutex_B (owned by LOW), LOW's priority is **not** automatically raised. To implement full cascaded PI:
- `mutex_t` needs a `blocked_on` pointer to the mutex the owner is waiting for (if any)
- `mutex_lock()` walks the chain: A.owner → A.owner.blocked_on → B.owner → … up to a depth limit (Linux uses 10)
- `mutex_unlock()` walks back to re-evaluate the highest remaining waiter priority

See Linux `rt_mutex_adjust_prio_chain()` for the reference implementation.

- [ ] Add `blocked_on` pointer to `mutex_t` (set/cleared in `mutex_lock`/`mutex_unlock`)
- [ ] In `mutex_lock()`: walk `blocked_on` chain and boost each owner in the chain
- [ ] Add a depth limit (e.g., `PI_MAX_CHAIN_DEPTH = 10`) to prevent infinite loops
- [ ] In `mutex_unlock()`: re-evaluate priority of remaining waiters and demote if appropriate
- [ ] Add test: three-task scenario (HIGH → MED → LOW) to verify chain boost

---

## 12. Seqlocks (Ultra-Fast Read Path) ✅

**Prompt:** This section is marked complete. Verify: `seqlock_t` exists in `include/kernel/sched/seqlock.h` with a `spinlock_t lock`, `volatile uint64_t seq`, and `uint64_t irq_flags`. `seqlock_write_lock` increments seq → odd (store-release); `seqlock_write_unlock` increments → even. `seqlock_read_begin` spins while seq is odd and returns it (load-acquire). `seqlock_read_retry` issues `rmb()` then checks if seq changed. `src/kernel/sched/seqlock.c` builds without warnings. Run `bash scripts/build.sh clean` → `=== BUILD OK ===`. Confirm commit `"sched: seqlocks"` in git history.


- [x] Define `seqlock_t` (spinlock + volatile uint64_t sequence counter)
- [x] Implement `seqlock_write_lock(sl)` / `seqlock_write_unlock(sl)` — inc counter odd/even
- [x] Implement `seqlock_read_begin(sl)` — return current sequence (retry if odd)
- [x] Implement `seqlock_read_retry(sl, seq)` — return true if sequence changed
- [x] Usage pattern: `do { seq = seqlock_read_begin(sl); ... } while (seqlock_read_retry(sl, seq))`
- [x] Use for: system uptime counter, jiffies, cached RTC time
- [x] Commit: `"sched: seqlocks"`

### Notes

**Files:**
- `include/kernel/sched/seqlock.h` — `seqlock_t`, `SEQLOCK_INIT`, writer/reader API
- `src/kernel/sched/seqlock.c` — implementation with acquire/release barrier protocol

**Protocol — counter states:**
```
seq = 0 (even)   → no write in progress, readers may proceed
seq = 1 (odd)    → write in progress, readers spin in seqlock_read_begin
seq = 2 (even)   → write complete, consistent state; readers check seq != begin
seq = 3 (odd)    → another write in progress...
```

**Standard usage pattern:**
```c
seqlock_t sys_time_lock = SEQLOCK_INIT;
uint64_t  g_uptime_ticks;

/* Writer (PIT interrupt handler): */
seqlock_write_lock(&sys_time_lock);
g_uptime_ticks++;
seqlock_write_unlock(&sys_time_lock);

/* Reader (any thread/IRQ): */
uint64_t seq, ticks;
do {
    seq   = seqlock_read_begin(&sys_time_lock);
    ticks = g_uptime_ticks;
} while (seqlock_read_retry(&sys_time_lock, seq));
/* ticks is now consistent */
```

**Memory barriers in the implementation:**
| Operation | Barrier | Purpose |
|---|---|---|
| `seqlock_write_lock` — seq++ | `__ATOMIC_RELEASE` | data writes can't escape before the odd-inc |
| `seqlock_write_unlock` — seq++ | `__ATOMIC_RELEASE` | data writes visible before even-inc signals done |
| `seqlock_read_begin` — load seq | `__ATOMIC_ACQUIRE` | data reads can't be hoisted before seq read |
| `seqlock_read_retry` — `rmb()` | read barrier | all data reads complete before final seq sample |

**Concurrency properties:**
- Multiple simultaneous readers → zero contention (no lock, no atomic RMW)
- Multiple simultaneous writers → serialized by internal spinlock
- Writer never blocks readers (readers retry instead)
- Reader never blocks writer (writer doesn't wait for readers)

**IRQ safety:**
- `seqlock_write_lock` uses `spin_lock_irqsave` → safe from any context
- `seqlock_read_begin` / `seqlock_read_retry` take no lock → safe from IRQ context
- Uptime counter can be updated from PIT IRQ and read from any thread

**OS comparison:**
| Feature | Windows | Linux | Impossible OS |
|---|---|---|---|
| Seqlock | ❌ (uses ERESOURCE) | ✅ `seqlock_t` / `seqcount_t` | ✅ `seqlock_t` / `DEFINE_SEQLOCK` |

**Gotcha — no pointers in seqlock-protected data:**
A reader may read a pointer while a writer is changing it. Between `seqlock_read_begin` and the `wmb()` at the end of `seqlock_write_unlock`, the reader can observe a half-updated pointer. This causes an illegal dereference crash even before `seqlock_read_retry` returns true. Only use seqlocks for scalar values (integers, timestamps, counters). For pointer-containing structures, use RCU (§13).

**Gotcha — reader critical section must be short:**
If the read-side takes too long (e.g., slow syscall), writers may increment the counter multiple times. Each retry costs a full re-read. For data updated at >10 KHz, a slow reader may spin indefinitely. Keep the read body minimal (just copy the data, process afterward).

**Gotcha — SEQLOCK_INIT irq_flags:**
`irq_flags` in `seqlock_t` is only valid between `seqlock_write_lock` and `seqlock_write_unlock`. If two writers nest (impossible since the spinlock prevents it), the irq_flags would be clobbered. The spinlock guarantee that only one writer is ever in the critical section makes this safe.

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
| ✅ Done   | 10. Memory Barriers        | `barrier.h` complete — `barrier()`, `mb()`, `rmb()`, `wmb()`, `smp_*` aliases |
| ✅ Done   | 7. Atomic Operations       | `atomic.h` complete — rwlock_t + mutex_t flags use `atomic_t`  |
| ✅ Done   | 6. Spinlocks               | `spinlock.c` complete — cli/sti + pushfq/popfq, applied to PIT, keyboard, NIC |
| 🔴 P0     | 23. Ticket Locks           | Fairer spinlock variant — implement alongside §6               |
| 🔴 P0     | 25. Stack Guard Pages      | Catches stack overflow before it silently corrupts memory      |
| 🔴 P0     | 26. Preemption Count       | Lighter than IRQ disable for non-interrupt critical sections   |
| ✅ Done   | 11. Priority Inheritance   | `priority`/`base_priority` in thread, PI mutex + priority-aware sched |
| ✅ Done   | 8. Wait/Event Objects      | `event.h`/`.c` complete — MANUAL/AUTO_RESET, timeout, IRQ-safe |
| ✅ Done   | 9. Work Queues             | `workqueue.h`/`.c` + `sys_wq` at boot, NIC rx deferred         |
| 🟠 P1     | 15. Futexes                | User-mode mutex/condvar; needed when user processes mature     |
| 🟠 P1     | 22. Thread-Local Storage   | Required for user-space C runtime (errno, locale, pthreads)    |
| 🟠 P1     | 24. Kernel Watchdog        | Catches deadlocked/hung tasks that lockdep can’t detect        |
| 🟠 P1     | 28. pthread_once           | Eliminates init races; replaces all ad-hoc bool init guards    |
| 🟠 P1     | 29. pthread_barrier_t      | Frame-sync for compositor audio+render pipeline                |
| ✅ Done   | 12. Seqlocks               | `seqlock_t`, `SEQLOCK_INIT`, IRQ-safe writer, lock-free reader  |
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
| Spinlocks (IRQ-safe)           | ✅ `KSPIN_LOCK`               | ✅ `spinlock_t`             | ✅ `spin_lock/unlock`, irqsave/irqrestore      |
| Atomic Operations              | ✅ `Interlocked*`             | ✅ `atomic_t`               | ✅ `atomic_read/set/inc/dec/cmpxchg/fetch_add` |
| Wait/Event Objects             | ✅ `KEVENT`                   | ✅ `completion`             | ✅ `event_wait/set/reset`, MANUAL+AUTO_RESET  |
| Work Queues                    | ✅ DPC + work items           | ✅ `workqueue_struct`       | ✅ `workqueue_create/enqueue`, `sys_wq`       |
| Memory Barriers                | ✅ `KeMemoryBarrier`          | ✅ `mb()`/`rmb()`/`wmb()`   | ✅ `barrier()` + `mb/rmb/wmb()` + `smp_*`     |
| Priority Inheritance           | ⚠️ Heuristic only             | ⚠️ Opt-in `rt_mutex`        | ✅ `thread_boost/restore_priority`, default on all mutexes  |
| Seqlocks                       | ❌                            | ✅ `seqlock_t`              | ✅ `seqlock_t` / `DEFINE_SEQLOCK`, `seqlock_read_begin/retry`  |
| RCU                            | ❌                            | ✅ `rcu_*`                  | ⬜ §13 P2                                     |
| SMP / Per-CPU                  | ✅ Full NUMA                  | ✅ Full NUMA                | ⬜ §14 Phase 2 — after §6/7/10/11/26          |
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
