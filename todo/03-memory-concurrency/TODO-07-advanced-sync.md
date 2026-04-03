# TODO-07 -- Advanced Synchronisation Primitives

> **Goal:** Complete the remaining unimplemented synchronisation and threading primitives from the old `TODO-020.01-Synchronization.md`: FIFO ticket locks, preemption count, thread-local storage via `FS` base MSR, user-space futexes, `pthread_once`/`call_once`, `pthread_barrier_t`, thread cancellation, `WaitForMultipleObjects`-style multi-wait, and a unified `_timeout(ms)` API across all blocking primitives.

> [!IMPORTANT]
> **Already complete (do not re-implement):** §1–14 of the old sync file are done: kernel threads, mutexes, semaphores, rwlocks, condvars, spinlocks, atomics, wait/event objects, work queues, memory barriers, priority inheritance (§11), seqlocks (§12), RCU (§13), SMP Phase 1 (§14). This TODO builds exclusively on top of those foundations.
> **Goes to TODO-09:** §16 lockdep, §20–21 deadlock visualization + named lock browser, §24 kernel watchdog, §25 stack guard pages, §30 KCSAN.

## Inputs

- [`include/kernel/sched/spinlock.h`](../../include/kernel/sched/spinlock.h)
- [`include/kernel/sched/mutex.h`](../../include/kernel/sched/mutex.h)
- [`include/kernel/sched/rwlock.h`](../../include/kernel/sched/rwlock.h)
- [`include/kernel/sched/semaphore.h`](../../include/kernel/sched/semaphore.h)
- [`include/kernel/sched/condvar.h`](../../include/kernel/sched/condvar.h)
- [`include/kernel/sched/task.h`](../../include/kernel/sched/task.h)
- [`src/kernel/sched/sched.c`](../../src/kernel/sched/sched.c)
- → XREF: `03-memory-concurrency/TODO-05-scheduler-enhancement.md §1` -- per-CPU run-queue lock is the primary candidate for §1 ticket lock replacement
- → XREF: `03-memory-concurrency/TODO-06-smp-phase2.md §6` -- per-CPU RCU upgrade consumes `preempt_disable()`/`preempt_enable()` from §2 as its read-side primitive
- → XREF: `03-memory-concurrency/TODO-06-smp-phase2.md §8` -- `READ_ONCE`/`WRITE_ONCE` macros required for ticket lock spin loop and mutex owner polling in §1 and §2
- → XREF: `02-kernel-core/TODO-06-irql-model-dpcs.md` -- preemption count in §2 lives below IRQL `DISPATCH_LEVEL`; `preempt_disable` must not lower IRQL, only inhibit the scheduler
- → XREF: `02-kernel-core/TODO-05-native-api-ssdt.md` -- `NtWaitForKeyedEvent` / `NtReleaseKeyedEvent` (§4 futexes) and `NtWaitForMultipleObjects` (§8) are Native API entries
- → XREF: `02-kernel-core/TODO-05-native-api-ssdt.md §4` -- SSDT indices 0x0084–0x0087 reserved for keyed events, 0x030B–0x030C and 0x0384–0x0385 for alert-by-thread-id, 0x038B for NtOpenKeyedEvent2

## Outcome

- The run-queue lock and any other highly contended spinlock use `ticket_lock_t` for strict FIFO ordering; no thread starves under contention.
- Kernel code that needs to stay on-CPU without disabling IRQs calls `preempt_disable()` / `preempt_enable()` instead of `spin_lock_irqsave()`.
- Every user-mode thread has a per-thread TLS block accessible via the `FS` segment; context switch saves/restores the `IA32_FS_BASE` MSR; per-thread `errno` works without global state.
- Futex `WAIT`/`WAKE`/`WAKE_OP` syscalls exist; user-mode mutexes and `std::mutex` implement the fast (uncontended, no syscall) path via them.
- `pthread_once` / `call_once` guarantee exactly one initialisation run across any number of concurrent callers with a single atomic read on the fast path.
- `pthread_barrier_t` is a kernel-level primitive with phase-toggle reuse and a `barrier_wait_timeout(ms)` variant wired to the compositor frame-sync pipeline.
- Thread cancellation is deferred only; cancellation points (`sem_wait`, `cond_wait`, `futex_wait`, `pipe_read`, `sleep`) check `cancel_requested` and invoke cleanup handlers in LIFO order.
- `wait_any` / `wait_all` operate on a `waitable_t` vtable embedded in all sync objects; the calling thread blocks until one or all handles are signalled, with a millisecond timeout.
- Every blocking primitive (`mutex_lock`, `sem_wait`, `rwlock_*_lock`, `cond_wait`) has a `_timeout(ms)` variant returning `WAIT_SIGNALLED(0)` or `WAIT_TIMEOUT(-1)`.

## Implementation Order

| ⭐  | Order | Deliverable                                               | Depends On                           | Status |
| --- | :---: | --------------------------------------------------------- | ------------------------------------ | :----: |
| 💎  |   1   | §1 Ticket locks (`ticket_lock_t`, run-queue replacement)  | TODO-06 §8 (`READ_ONCE`)             |  [ ]   |
| 💎  |   2   | §2 Preemption count (`preempt_disable`/`preempt_enable`)  | §1, TODO-06 §1 (atomics)             |  [ ]   |
| 💎  |   3   | §3 TLS -- `FS` base MSR, context save, `tls_key_*`         | --                                    |  [ ]   |
| 💎  |   4   | §4 Futexes -- `FUTEX_WAIT`/`WAKE`/`WAKE_OP`               | §2, §3                               |  [ ]   |
| 💎  |   5   | §9 Unified `_timeout(ms)` API                             | §1, §2 (primitives stable)           |  [ ]   |
| 💎  |   6   | §5 `pthread_once` / `call_once`                           | §1                                   |  [ ]   |
| 💎  |   7   | §7 Thread cancellation (`pthread_cancel`)                 | §4, §3                               |  [ ]   |
| ⭐  |   8   | §8 `WaitForMultipleObjects` (`waitable_t` + `wait_any`)   | §4, §9                               |  [ ]   |
| ⭐  |   9   | §6 `pthread_barrier_t` -- kernel-level, reusable           | §8 or §9 (timeout variant)           |  [ ]   |
| 💎  |  10   | Sync syscalls wired to SSDT (keyed events, alerts)        | §4, §8, TODO-05 §4                   |  [ ]   |

> 💎 = parity -- ticket locks, preemption count, TLS, futexes, timeout API, pthread_once, and thread cancellation all have direct Linux or Windows NT equivalents.
> ⭐ = exclusive -- `WaitForMultipleObjects` with a first-class `waitable_t` vtable across all sync types is superior to Linux's fd-only `epoll`; `pthread_barrier_t` as a kernel primitive (not just user-space pthreads) is a Windows gap.

---

## 1. Ticket Locks -- FIFO Fair Spinlocks `[Opus]`

Replace the CAS-spinlock with a ticket lock for high-contention sites. `atomic_fetch_add` on `next_ticket` gives each caller their position; they spin on `now_serving == my_ticket`. On release, `now_serving` is incremented, handing the lock to the next waiter in arrival order.

**Files:** `include/kernel/sched/spinlock.h`, `src/kernel/sched/spinlock.c`

> [!IMPORTANT]
> The spin loop must use `READ_ONCE(tl->now_serving)` (→ XREF: `TODO-06 §8`) to prevent the compiler from hoisting the load out of the loop. Add a `PAUSE` instruction in the spin body on x86 to yield the execution port to the hardware thread waiting on the store.

```c
typedef struct { uint32_t next_ticket; uint32_t now_serving; } ticket_lock_t;
```

- [ ] Define `ticket_lock_t` with `next_ticket` and `now_serving` as adjacent `uint32_t`
- [ ] `ticket_lock(tl)` -- `my_ticket = atomic_fetch_add(&tl->next_ticket, 1)`; spin `while (READ_ONCE(tl->now_serving) != my_ticket) { __asm__("pause"); }`
- [ ] `ticket_unlock(tl)` -- `atomic_inc(&tl->now_serving)` (store-release semantics)
- [ ] `ticket_lock_irqsave(tl, flags)` / `ticket_unlock_irqrestore(tl, flags)` -- identical IRQ-save semantics to `spinlock_irqsave`
- [ ] `ticket_trylock(tl)` -- CAS `{ next_ticket, now_serving }` atomically; succeed only if uncontended
- [ ] Replace the scheduler run-queue lock with `ticket_lock_t` (most contended lock in the kernel)
- [ ] Boot log: `[SCHED] run-queue lock: ticket_lock_t (FIFO ordering)`
- [ ] Commit: `"sched: ticket locks -- FIFO spinlock, run-queue lock replacement"`

## 2. Preemption Count `[Opus]`

Add a `preempt_count` field to `task_t`. `preempt_disable()` increments it; `preempt_enable()` decrements and reschedules if `preempt_count == 0` and `need_resched` is set. The scheduler tick ISR sets `need_resched` instead of switching when the count is non-zero. IRQs remain fully enabled -- only the voluntary scheduler switch is inhibited.

**Files:** `include/kernel/sched/task.h`, `src/kernel/sched/sched.c`

> [!IMPORTANT]
> `preempt_disable()` / `preempt_enable()` inhibit the **scheduler**, not interrupts. Timer ISRs, NIC ISRs, and IPIs still fire. Do not confuse with `spin_lock_irqsave()`. The preemption count is a per-thread field read with `READ_ONCE()` in the scheduler tick ISR.

- [ ] Add `preempt_count` (`uint32_t`) and `need_resched` (`bool`) fields to `task_t`, both initialised to 0
- [ ] `preempt_disable()` -- `current->preempt_count++`; memory barrier
- [ ] `preempt_enable()` -- `if (--current->preempt_count == 0 && READ_ONCE(current->need_resched)) schedule()`
- [ ] In scheduler tick ISR: if `current->preempt_count > 0`, set `WRITE_ONCE(current->need_resched, true)` and return without switching
- [ ] `schedule()` debug assertion: `assert(current->preempt_count == 0)` in debug builds
- [ ] Update RCU `rcu_read_lock()` / `rcu_read_unlock()` to use `preempt_disable()` / `preempt_enable()` instead of IRQ disable (→ XREF: `TODO-06 §6`)
- [ ] Boot log: `[SCHED] preemption count active`
- [ ] Commit: `"sched: preemption count -- preempt_disable/enable, need_resched, RCU read-side"`

## 3. Thread-Local Storage (TLS) `[Opus]`

Every thread gets a `tls_block_t` pointed to by `FS:0`. On context switch the kernel saves and restores `IA32_FS_BASE` via `WRMSR`/`RDMSR`. Dynamic keys map to per-thread pointer slots; destructors run on `thread_exit()`. ELF `PT_TLS` template is copied per-thread at `exec()`. Per-thread `errno` uses `FS`-relative access.

**Files:** `include/kernel/sched/task.h`, `src/kernel/sched/sched.c`, `include/kernel/sched/tls.h` (new), `src/kernel/sched/tls.c` (new)

> [!CAUTION]
> TLS block must be allocated from `pmm_alloc_contiguous()` if the user requests > 4 KB (e.g., a large `PT_TLS` segment). Default TLS is 512 B–4 KB -- `kmalloc` is fine. `FS` base must be restored *before* the thread executes its first user-mode instruction after a context switch -- the `WRMSR IA32_FS_BASE` goes in the context-switch exit path, not entry.

- [ ] Define `tls_block_t`: static `__thread` data area at negative offset from `FS:0`, pointer array for dynamic keys at positive offset
- [ ] Allocate `tls_block_t` in `thread_create()`; load `IA32_FS_BASE` to its address via `WRMSR(0xC0000100, tls_base)`
- [ ] Add `fs_base` (`uint64_t`) to `task_t`; save on context switch out (`RDMSR 0xC0000100`), restore on context switch in (`WRMSR`)
- [ ] `tls_key_create(destructor)` → returns unique slot index; `tls_set(key, value)` / `tls_get(key)` -- O(1) slot array lookup
- [ ] On `thread_exit()`: for each non-NULL TLS slot, call its registered destructor
- [ ] ELF `PT_TLS` support at `exec()`: copy TLS template data into new thread's `tls_block_t`
- [ ] Per-thread `errno`: `#define errno (*tls_errno())` where `tls_errno()` returns `(int *)(fs_base + ERRNO_OFFSET)`
- [ ] Test: two threads set `errno` to different values concurrently -- verify no interference
- [ ] Commit: `"kernel: thread-local storage -- FS base MSR, tls_key_create/set/get, per-thread errno"`

## 4. Futexes `[Opus]`

Fast userspace mutexes: `FUTEX_WAIT(uaddr, expected)` atomically checks that `*uaddr == expected` and sleeps; `FUTEX_WAKE(uaddr, n)` wakes up to N waiters sleeping on `uaddr`. The per-process futex hash table maps user-space virtual addresses to kernel wait queues. User-mode `pthread_mutex_lock` avoids syscalls on the uncontended path entirely.

**Files:** `src/kernel/sched/futex.c` (new), `include/kernel/sched/futex.h` (new), `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> The `FUTEX_WAIT` check-and-sleep must be atomic with respect to `FUTEX_WAKE`. If the check (`*uaddr == expected`) passes but the thread sleeps after a `FUTEX_WAKE` was already sent, it sleeps forever. Use a mutex protecting both the comparison and the enqueue into the wait queue; only then release the mutex and call `schedule()`.

- [ ] Per-process futex table: `futex_table_t` -- hash map of `(uaddr % FUTEX_HASH_SIZE)` → `futex_wait_queue_t`
- [ ] `FUTEX_WAIT(uaddr, expected, timeout_ms)` -- validate `uaddr` in user address space; lock bucket; if `*uaddr != expected` return `EAGAIN`; enqueue current thread; unlock bucket; `schedule()`; on wake, return 0
- [ ] `FUTEX_WAKE(uaddr, n)` -- lock bucket; wake up to `n` waiters, remove from queue; unlock; return count woken
- [ ] `FUTEX_WAKE_OP(uaddr, uaddr2, n, op, val)` -- atomically `op(uaddr2, val)`, then wake up to `n` on `uaddr` and 1 on `uaddr2` (for condvar broadcast)
- [ ] `SYS_FUTEX` syscall dispatch: routes to above three operations based on `op` argument
- [ ] `NtWaitForKeyedEvent` / `NtReleaseKeyedEvent` -- Native API wrapper over futex `WAIT`/`WAKE` (→ XREF `TODO-05-native-api-ssdt.md`)
- [ ] On `thread_exit()`: drain any remaining futex wait-table entries to avoid dangling waiter records
- [ ] Commit: `"kernel: futex syscall -- WAIT/WAKE/WAKE_OP, per-process hash table, NtWaitForKeyedEvent"`

## 5. `pthread_once` / `call_once` `[Sonnet]`

Guarantee a one-time initialisation function runs exactly once across any number of concurrent callers. The fast path is a single atomic read -- free once initialised. All concurrent callers block on the mutex while the first caller runs the init function.

**Files:** `include/kernel/sched/once.h` (new), `src/kernel/sched/once.c` (new)

```c
typedef struct { mutex_t mu; bool done; } once_flag_t;
#define ONCE_FLAG_INIT { MUTEX_INIT, false }
```

- [ ] Define `once_flag_t`: `{ mutex_t mu; atomic_bool done; }` with `ONCE_FLAG_INIT` macro
- [ ] `pthread_once(flag, fn)`: fast path `if (atomic_load(&flag->done)) return`; slow path: `mutex_lock`; double-check `if (!flag->done) { fn(); atomic_store(&flag->done, true); }`; `mutex_unlock`
- [ ] Alias `call_once(flag, fn)` as a C11-compatible macro wrapping `pthread_once`
- [ ] Replace any existing `static bool initialized` guards in kernel subsystems (font cache, registry root, NTP state) with `once_flag_t`
- [ ] Test: 8 concurrent threads calling `pthread_once` -- verify `fn` runs exactly once (serial log shows one `"init called"`)
- [ ] Commit: `"sched: pthread_once / call_once -- double-checked locking, atomic fast path"`

## 6. `pthread_barrier_t` -- Kernel-Level Barrier `[Sonnet]`

A reusable phase barrier that releases all threads simultaneously after the last one arrives. Phase-toggle prevents spurious early release on reuse. First-class kernel primitive -- available to kernel threads and user processes alike without user-space pthread scaffolding.

**Files:** `include/kernel/sched/barrier.h` (new), `src/kernel/sched/barrier.c` (new)

> [!NOTE]
> Windows has no `pthread_barrier_t` -- developers emulate it with N events + `WaitForMultipleObjects` (O(N) handles, fragile). Linux has it only in user-space pthreads, not as a kernel primitive. Impossible OS exposes this from kernel threads.

```c
typedef struct {
    uint32_t  target;   /* threads that must arrive       */
    uint32_t  count;    /* threads arrived so far         */
    uint32_t  phase;    /* toggles 0/1 to prevent spurious release */
    mutex_t   mu;
    condvar_t cv;
} barrier_t;
```

- [ ] Define `barrier_t` struct as above; `barrier_init(&b, n)` -- zero count, set target, init mu + cv
- [ ] `barrier_wait(&b)`: increment `count` under lock; if `count == target` → reset count, toggle `phase`, `condvar_broadcast`; else `condvar_wait` until `phase` changes
- [ ] Return `BARRIER_SERIAL_THREAD` (non-zero, e.g. 1) to exactly one thread per generation (the last to arrive)
- [ ] `barrier_wait_timeout(&b, ms)` -- uses §9 unified `_timeout` API; returns `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] `barrier_destroy(&b)` -- asserts no threads currently waiting; destroys mu + cv
- [ ] Wire compositor frame sync to `barrier_wait` instead of ad-hoc semaphores
- [ ] Test: N=4 threads -- none proceed until all 4 call `barrier_wait`; reuse across 3 generations without reinit
- [ ] Commit: `"sched: pthread_barrier_t -- phase-toggle, kernel-level, compositor frame sync"`

## 7. Thread Cancellation `[Sonnet]`

Deferred POSIX cancellation: `thread_cancel(tid)` marks the target thread; it exits cleanly at the next cancellation point. `pthread_cleanup_push/pop` registers LIFO cleanup handlers for lock release and resource cleanup on cancellation.

**Files:** `include/kernel/sched/task.h`, `src/kernel/sched/sched.c`, `src/kernel/sched/cancel.c` (new)

> [!NOTE]
> `PTHREAD_CANCEL_ASYNCHRONOUS` is intentionally **not implemented** -- it is impossible to implement safely in most kernel contexts and Linux documents it as unsafe. Deferred cancellation only.

- [ ] Add to `task_t`: `cancel_requested` (`bool`), `cancel_state` enum (`CANCEL_ENABLED` / `CANCEL_DISABLED`)
- [ ] `thread_cancel(tid)` -- set `cancel_requested = true` on target; `SYS_PTHREAD_CANCEL` syscall entry
- [ ] `pthread_setcancelstate(state, &oldstate)` -- enable/disable cancellability on the calling thread
- [ ] Cancellation check macro `CANCELLATION_POINT()`: `if (cancel_requested && cancel_state == CANCEL_ENABLED) thread_exit(PTHREAD_CANCELED)`
- [ ] Insert `CANCELLATION_POINT()` at: `sem_wait`, `cond_wait`, `pipe_read`, `sleep`, `futex_wait` (→ XREF §4)
- [ ] Cleanup handler stack in `task_t`: `pthread_cleanup_push(fn, arg)` pushes onto a per-thread linked list; `pthread_cleanup_pop(execute)` pops and optionally runs
- [ ] On cancellation path in `thread_exit()`: invoke cleanup handlers LIFO before teardown
- [ ] `pthread_join()` distinguishes `PTHREAD_CANCELED` return from normal exit code
- [ ] Test: cancel a thread blocked in `cond_wait()` -- verify cleanup handler runs; verify `pthread_join` returns `PTHREAD_CANCELED`
- [ ] Commit: `"sched: thread cancellation -- deferred pthread_cancel, cleanup handler stack, cancellation points"`

## 8. `WaitForMultipleObjects` (`waitable_t` vtable) `[Opus]`

A `waitable_t` vtable pointer is embedded in every sync object (`mutex_t`, `semaphore_t`, `event_t`, `condvar_t`). `wait_any(handles[], n, ms)` sleeps until any one handle is signalled; `wait_all(handles[], n, ms)` sleeps until all are signalled. Returns the index of the first-signalled handle or `WAIT_TIMEOUT`.

**Files:** `include/kernel/sched/waitable.h` (new), `src/kernel/sched/wait_multi.c` (new)

> [!IMPORTANT]
> To avoid TOCTOU in `wait_any`, the registration of the thread as a waiter on all N objects must be atomic with respect to any of them becoming signalled. Lock all N waitable objects in a deterministic order (index order) before registering; release all after the thread is safely parked.

- [ ] Define `waitable_t`: `{ int (*is_ready)(void *self); void (*add_waiter)(void *self, waiter_t *); void (*remove_waiter)(void *self, waiter_t *); }` vtable
- [ ] Embed `waitable_t *waitable` pointer in `mutex_t`, `semaphore_t`, `event_t`, `condvar_t`; implement vtable for each type
- [ ] `wait_any(waitable_t *handles[], count, timeout_ms)` -- iterate in index order; register as waiter on all handles; `schedule()`; on wake, identify which handle fired, `remove_waiter` from others; return index
- [ ] `wait_all(waitable_t *handles[], count, timeout_ms)` -- block until all handles report `is_ready`; return 0 or `WAIT_TIMEOUT`
- [ ] `NtWaitForMultipleObjects(count, handles[], wait_all, timeout)` -- Native API entry over `wait_any`/`wait_all` (→ XREF `TODO-05-native-api-ssdt.md`)
- [ ] Win32 `WaitForMultipleObjects(count, handles[], bWaitAll, ms)` → `NtWaitForMultipleObjects`
- [ ] Test: `wait_any` on `[mutex, event]` -- verify correct index returned when event fires first
- [ ] Commit: `"sched: WaitForMultipleObjects -- waitable_t vtable, wait_any/wait_all, NtWaitForMultipleObjects"`

## 9. Unified `_timeout(ms)` API `[Sonnet]`

Every blocking primitive gets a `_timeout(ms)` variant with identical semantics: pass milliseconds relative to now, receive `WAIT_SIGNALLED (0)` or `WAIT_TIMEOUT (-1)`. All variants use the calibrated tick counter from `TODO-05 §8` for ms-accurate measurement.

**Files:** `src/kernel/sched/mutex.c`, `src/kernel/sched/semaphore.c`, `src/kernel/sched/rwlock.c`, `src/kernel/sched/condvar.c`

```c
#define WAIT_SIGNALLED    0
#define WAIT_TIMEOUT     -1
```

- [ ] `mutex_lock_timeout(m, ms)` → `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] `sem_wait_timeout(s, ms)` → `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] `rwlock_read_lock_timeout(rw, ms)` → `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] `rwlock_write_lock_timeout(rw, ms)` → `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] `cond_wait_timeout(cond, mutex, ms)` → `WAIT_SIGNALLED` or `WAIT_TIMEOUT`
- [ ] All variants read the monotonic tick counter (`uptime_ns()`) for deadline computation; use `WAIT_INFINITE (-1)` to match existing non-timeout behaviour
- [ ] Commit: `"sched: unified _timeout(ms) API -- mutex/sem/rwlock/cond variants, WAIT_SIGNALLED/WAIT_TIMEOUT"`

---

## 10. Sync Syscalls Wired to SSDT (Keyed Events, Alerts)

Register keyed event and alert-by-thread-id syscalls in the SSDT for user-mode synchronisation primitives. (→ XREF: 02-kernel-core/TODO-05-native-api-ssdt.md §4)

- [ ] `NtCreateKeyedEvent(KeyedEventHandle, DesiredAccess, ObjectAttributes, Flags)` → SSDT 0x0084
- [ ] `NtOpenKeyedEvent(KeyedEventHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0085
- [ ] `NtWaitForKeyedEvent(KeyedEventHandle, KeyValue, Alertable, Timeout)` → SSDT 0x0086
- [ ] `NtReleaseKeyedEvent(KeyedEventHandle, KeyValue, Alertable, Timeout)` → SSDT 0x0087
- [ ] `NtWaitForAlertByThreadId(Address, Timeout)` → SSDT 0x030B
- [ ] `NtAlertThreadByThreadId(ThreadId)` → SSDT 0x030C
- [ ] `NtAlertThreadByThreadIdEx(...)` → SSDT 0x0384
- [ ] `NtWaitForAlertByThreadIdEx(...)` → SSDT 0x0385
- [ ] `NtOpenKeyedEvent2(...)` → SSDT 0x038B
- [ ] All functions return `NTSTATUS`
- [ ] Commit: `"sync: wire keyed event and alert-by-thread-id syscalls to SSDT"`

**Test checkpoint:** `NtCreateKeyedEvent` + `NtWaitForKeyedEvent` blocks; `NtReleaseKeyedEvent` from another thread wakes it. `NtAlertThreadByThreadId` + `NtWaitForAlertByThreadId` round-trip completes.

---

## OS Comparison


| ⭐ | Feature                                          | 🪟 Win11                                                              | 🐧 Linux                                                               | 🚀 Impossible OS                                                          |
|----|--------------------------------------------------|--------------------------------------------------------------------|---------------------------------------------------------------------|------------------------------------------------------------------------|
| 💎 | FIFO ticket locks                                | ✅ `KSPIN_LOCK` queued spinlocks (FIFO via                         | ✅ `arch/x86/include/asm/spinlock.h` -- ticket locks (pre-qspinlock) | ⬜ §1 -- `ticket_lock_t`, run-queue lock replacement                    |
| 💎 | Preemption count                                 | ✅ `KiAcquireApcLock` / preemption depth in                        | ✅ `preempt_disable()` / `preempt_enable()` per-thread counter      | ⬜ §2 -- `preempt_count` + `need_resched` in `task_t`                   |
| 💎 | Thread-local storage via FS base MSR             | ✅ TEB at `FS:0` (32-bit) /                                        | ✅ `ARCH_SET_FS` via `arch_prctl`; `IA32_FS_BASE` WRMSR             | ⬜ §3 -- `IA32_FS_BASE` WRMSR, context-switch save/restore, PT_TLS      |
| 💎 | Futex `WAIT`/`WAKE` user-space fast mutex        | ✅ `NtWaitForKeyedEvent` / `NtReleaseKeyedEvent` (similar concept) | ✅ `futex(2)` -- `FUTEX_WAIT`/`WAKE`/`WAKE_OP`                       | ⬜ §4 -- per-process hash table, `SYS_FUTEX`, `NtWaitForKeyedEvent`     |
| 💎 | `pthread_once` / C11 `call_once`                 | ✅ `InitOnceExecuteOnce` Win32 / `INIT_ONCE` kernel                | ✅ `pthread_once(3)` / `DEFINE_STATIC_SRCU`                         | ⬜ §5 -- `once_flag_t`, double-checked lock, atomic fast                |
| ⭐ | `pthread_barrier_t` as kernel primitive          | ❌ No native barrier; developers use                               | ⬜ User-space pthreads only; no kernel                              | ⬜ §6 -- phase-toggle, kernel threads + user                            |
| 💎 | Deferred thread cancellation + cleanup handlers  | ✅ `TerminateThread` (unsafe); no POSIX deferred                   | ✅ `pthread_cancel(3)` deferred; `pthread_cleanup_push/pop`         | ⬜ §7 -- `cancel_requested`, cancellation points, LIFO cleanup          |
| ⭐ | `WaitForMultipleObjects` across all sync types   | ✅ `WaitForMultipleObjects` -- handle-based, up to                  | ❌ `epoll`/`select` for FDs only; no                                | ⬜ §8 -- `waitable_t` vtable in mutex/sem/event/condvar, `wait_any/all` |
| 💎 | Unified `_timeout(ms)` across all blocking prims | ✅ All `Wait*` take `DWORD dwMilliseconds`                         | ⬜ Inconsistent (`timespec`, jiffies, relative/absolute per         | ⬜ §9 -- `mutex/sem/rwlock/cond _timeout(ms)`, `WAIT_SIGNALLED/-1`      |

> **After §1–9:** Impossible OS achieves full parity with Windows NT and Linux on synchronisation fundamentals. Two exclusive differentiators stand out: `WaitForMultipleObjects` with a `waitable_t` vtable covers arbitrary kernel sync objects (Linux's `epoll` is FD-only); and `pthread_barrier_t` as a kernel-level reusable primitive fills a gap Windows never addressed natively. The unified `_timeout(ms)` API is a direct improvement on Linux's fragmented timeout conventions.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Ticket lock: 4 threads contending on same `ticket_lock_t` -- serial log confirms FIFO acquisition order (ticket numbers in sequence)
- [ ] Preemption count: `preempt_disable()` in tight loop -- PIT tick fires (serial shows ISR running) but context switch deferred until `preempt_enable()`
- [ ] TLS: two threads set `errno = 42` and `errno = 99` concurrently -- each reads back its own value; no cross-contamination
- [ ] Futex: user-mode mutex built on `FUTEX_WAIT`/`WAKE` -- verify contended lock does not busy-spin; `strace`-equivalent shows syscall only on contention
- [ ] `pthread_once`: 8 threads -- serial log shows `"init called"` exactly once
- [ ] Barrier: 4 threads call `barrier_wait`; none print "arrived" until all 4 have entered; works across 3 generations
- [ ] Cancellation: thread blocked in `cond_wait` is cancelled -- cleanup handler runs (serial log), `pthread_join` returns `PTHREAD_CANCELED`
- [ ] `wait_any`: `wait_any([mutex, event], 2, 1000)` -- signal event, verify returns index 1
- [ ] Timeout API: `mutex_lock_timeout(m, 100)` on a held mutex -- returns `WAIT_TIMEOUT` after ~100 ms
- [ ] Commit: `"sched: advanced sync primitives -- ticket locks, preempt, TLS, futex, once, barrier, cancel, wait_any, timeouts"`
