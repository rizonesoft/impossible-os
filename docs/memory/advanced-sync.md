<!-- docs: covers=todo/03-memory-concurrency/TODO-08-advanced-sync.md sources=include/kernel/sched/spinlock.h,include/kernel/sched/mutex.h,src/kernel/sched/mutex.c,include/kernel/sched/event.h,src/kernel/sched/event.c,src/kernel/nt/nt_sync.c,src/kernel/test/test_nt_sync.c,src/kernel/test/test_ipc.c,src/kernel/sched/task.c reviewed=2026-09-28 order=8 -->
# Synchronisation Primitives

## What is it?

The locks and wait objects kernel code uses to coordinate threads and CPUs. The baseline set exists: IRQ-safe spinlocks, mutexes with priority inheritance, reader-writer locks, semaphores, condition variables, events, seqlocks, and the NT event, mutant and semaphore objects with `NtWaitForMultipleObjects`. The roadmap's additions (ticket locks, futexes, TLS, once-initialisation, barriers, cancellation, a unified wait vtable and timeouts on every primitive) are not built, and a known SMP defect in the mutex wait queue is still open.

## How does it work?

**Spinlocks.** `spinlock_t` is a single word ([`spinlock.h`](../../include/kernel/sched/spinlock.h)). `spin_lock_irqsave()` disables interrupts and spins; `spin_unlock_irqrestore()` releases with the matching fence. A holder must never sleep, and a spinlock must never be held across `mutex_lock()` or `sem_wait()`.

**Mutexes.** `mutex_lock()` in [`mutex.c`](../../src/kernel/sched/mutex.c) tries a compare-and-swap on the lock word; on failure it adds the thread to a fixed wait list of `MUTEX_MAX_WAITERS` (16), boosts the direct owner's priority (only the direct owner: if that owner is itself blocked on another mutex, the thread it waits for is not boosted, despite a code comment saying otherwise), marks the thread blocked and yields. `mutex_unlock()` restores the owner's base priority and wakes a waiter. The wait list is updated without its own lock, and a seventeenth waiter is not queued at all; both are the subject of the roadmap's backfill section.

**Events and waits.** `event_t` ([`event.h`](../../include/kernel/sched/event.h)) supports manual and auto reset; `event_wait_timeout()` is the only primitive with a timeout today. Semaphores, condition variables, reader-writer locks and seqlocks have their own headers in `include/kernel/sched/`.

**NT objects.** [`nt_sync.c`](../../src/kernel/nt/nt_sync.c) exposes events, mutants and semaphores as Object Manager objects and implements `NtWaitForSingleObject` and `NtWaitForMultipleObjects`. The four keyed-event services are registered to a stub that returns `STATUS_NOT_IMPLEMENTED`.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `spin_lock()`, `spin_lock_irqsave()`, `spin_trylock()`, `spin_is_locked()` | Spinlocks ([`spinlock.h`](../../include/kernel/sched/spinlock.h)) |
| `mutex_init()`, `mutex_lock()`, `mutex_unlock()`, `mutex_trylock()` | Sleeping mutex with priority inheritance ([`mutex.h`](../../include/kernel/sched/mutex.h)) |
| `sem_init()`, `sem_wait()`, `sem_signal()`, `cond_wait()`, `cond_broadcast()` | Semaphores and condition variables |
| `event_wait_timeout()`, `event_try_consume()` | Kernel events ([`event.h`](../../include/kernel/sched/event.h)) |
| `NtCreateEvent`, `NtCreateMutant`, `NtCreateSemaphore`, `NtWaitForMultipleObjects` | NT synchronisation objects ([`nt_sync.c`](../../src/kernel/nt/nt_sync.c)) |

## How do I use it?

Protect short, non-sleeping critical sections with `spin_lock_irqsave()`, and anything that may block with a mutex. Win32 code uses the NT objects through the usual `CreateEvent`, `WaitForMultipleObjects` family. The NT objects are tested in [`test_nt_sync.c`](../../src/kernel/test/test_nt_sync.c), and the kernel mutex and semaphore in [`test_ipc.c`](../../src/kernel/test/test_ipc.c):

```bash
bash scripts/test.sh SUITE=abi
bash scripts/test.sh SUITE=sched
bash scripts/test.sh SUITE=ipc
```

## What is not implemented yet?

- **Ticket locks** for FIFO-fair spinning ([Ticket Locks](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#1-ticket-locks----fifo-fair-spinlocks-opus)).
- **A preemption count API.** The per-CPU field exists; `preempt_disable()` and `preempt_enable()` do not ([Preemption Count](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#2-preemption-count-opus)).
- **Thread-local storage and futexes** ([Thread-Local Storage](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#3-thread-local-storage-tls-opus), [Futexes](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#4-futexes-opus)).
- **Once-initialisation, barriers and cancellation** ([`pthread_once`](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#5-pthread_once--call_once-sonnet), [`pthread_barrier_t`](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#6-pthread_barrier_t----kernel-level-barrier-sonnet), [Thread Cancellation](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#7-thread-cancellation-sonnet)).
- **One wait vtable across every primitive, and timeouts everywhere** ([`WaitForMultipleObjects` (`waitable_t` vtable)](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#8-waitformultipleobjects-waitable_t-vtable-opus), [Unified `_timeout(ms)` API](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#9-unified-_timeoutms-api-sonnet)).
- **Keyed events and thread alerts** ([Sync Syscalls Wired to SSDT](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#10-sync-syscalls-wired-to-ssdt-keyed-events-alerts)).
- **A locked, unbounded mutex wait queue** ([Mutex Wait-Queue SMP-Safety Backfill](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md#11-mutex-wait-queue-smp-safety-backfill)).

## How does it compare with Windows 11 and Linux?

Windows 11 has queued spinlocks, keyed events, `WaitOnAddress`, TLS and `WaitForMultipleObjects` over every dispatcher object; Linux has queued spinlocks, `futex(2)`, TLS, `pthread_barrier_t` and cancellation. Impossible OS has the classic primitive set, priority inheritance and the NT wait objects, but none of the roadmap's rows yet. Its planned additions are one wait interface that every primitive implements and a kernel-level barrier object.

## See also

- [Advanced Synchronisation Primitives roadmap](../../todo/03-memory-concurrency/TODO-08-advanced-sync.md)
- [Object Manager](../kernel/object-manager.md)
- [IRQL, DPCs and APCs](../kernel/irql-dpc.md)
- [Scheduler](scheduler.md)
- [Win32 IPC Extensions](win32-ipc-extensions.md)
