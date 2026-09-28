<!-- docs: covers=todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md sources=include/kernel/sched/task.h,src/kernel/sched/task.c,src/kernel/drivers/lapic.c,src/kernel/nt/nt_process.c,include/kernel/nt/service_numbers.h,src/kernel/test/test_sched.c,src/kernel/sched/mutex.c reviewed=2026-09-28 order=6 -->
# Scheduler

## What is it?

The scheduler decides which thread runs next. Today only the boot CPU dispatches threads (the other CPUs boot, then idle and answer interrupts), and the policy is a flat round-robin over every runnable thread with 32 fixed priority levels, per-thread kernel stacks and priority inheritance for mutexes. The roadmap's larger scheduler (O(1) priority queues, fair-share `vruntime`, real-time and deadline classes, affinity, statistics and CPU bandwidth control) is not built; only the per-thread kernel stack section is complete.

## How does it work?

`find_next_task()` in [`task.c`](../../src/kernel/sched/task.c) walks every `(task, thread)` slot in one global cyclic order, starting one slot after the current thread, and picks the first runnable thread at the highest priority it finds. Same-priority threads in different processes and in the same process get equal turns, so a `while (!cond) yield()` loop cannot starve a sibling thread. `schedule()` runs this on the timer tick after each thread's quantum, `SCHED_QUANTUM` (5 ticks), and `schedule_now()` runs it immediately when a thread blocks or yields.

Priorities run from `THREAD_PRIO_IDLE` (0) to `THREAD_PRIO_REALTIME` (31), higher meaning more important, with new threads at `THREAD_PRIO_NORMAL` (16) ([`task.h`](../../include/kernel/sched/task.h)). When a thread blocks on a mutex held by a lower-priority thread, `thread_boost_priority()` raises the direct owner (not a chain of owners, so nested inversion is still possible), and `thread_restore_priority()` drops it back to its base priority when it unlocks.

Each thread has its own kernel stack (`kernel_rsp`, `kernel_stack_base`, `kernel_stack_pages` in `struct thread`), switched into `TSS.rsp0` on every context switch. The table sizes are fixed: `TASK_MAX` (32) processes and `THREAD_MAX` (16) threads per process. FPU and XSAVE state is still saved per process in `struct task`, not per thread.

The tick is driven by the local APIC timer, which [`lapic.c`](../../src/kernel/drivers/lapic.c) calibrates at boot with `lapic_timer_calibrate()`: it reads the frequency from MSR or CPUID where available and otherwise measures against the HPET, the ACPI PM timer or the PIT.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `schedule()`, `schedule_now()`, `yield()` | Scheduling entry points ([`task.h`](../../include/kernel/sched/task.h)) |
| `thread_set_priority()`, `thread_boost_priority()`, `thread_restore_priority()` | Priority changes and mutex priority inheritance |
| `NtSetInformationThread(ThreadPriority)` | Win32 priority change, range-checked against `THREAD_PRIO_REALTIME` ([`nt_process.c`](../../src/kernel/nt/nt_process.c)) |
| `NtSetInformationThread(ThreadAffinityMask, ThreadIdealProcessor)` | Accepted and ignored |
| `THREAD_PRIO_*`, `SCHED_QUANTUM`, `TASK_MAX`, `THREAD_MAX` | Scheduling constants |

## How do I use it?

Kernel code sets a thread's priority with `thread_set_priority()` and may rely on the flat round-robin for fairness. The scheduler suite in [`test_sched.c`](../../src/kernel/test/test_sched.c) covers the per-thread kernel stack mirror, cooperative-yield fairness and thread slot reuse:

```bash
bash scripts/test.sh SUITE=sched
```

## What is not implemented yet?

- **O(1) priority queues and a 40-level range** ([40-Level Priority Queues](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#1-40-level-priority-queues--o1-bitmap-dequeue)).
- **Priority aging, `vruntime` fair share, `SCHED_FIFO`/`SCHED_RR` and `SCHED_DEADLINE`** ([Dynamic Priority Aging](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#2-dynamic-priority-aging), [CFS vruntime](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#3-cfs-vruntime--prio_to_weight-table), [Real-Time Classes](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#4-sched_fifo--sched_rr-real-time-classes), [EDF Scheduling](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#5-sched_deadline-edf-scheduling)).
- **CPU affinity.** The Win32 call is accepted but stores nothing ([CPU Affinity](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#6-cpu-affinity)).
- **Statistics and `/sys/sched`** ([Scheduler Stats](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#7-scheduler-stats--syssched-vfs-file)).
- **Tick calibration as specified.** The APIC timer is calibrated; the roadmap's nanosecond tick and hypervisor timer path are open ([Scheduler Tick Calibration](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#8-scheduler-tick-calibration)).
- **CPU frequency scaling and Worker Factory syscalls** ([CPU Frequency Scaling Hook](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#9-cpu-frequency-scaling-hook), [Worker Factory Syscalls](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#10-worker-factory-syscalls-wired-to-ssdt)).
- **Dynamic task and thread tables** in place of the fixed 32 and 16 ([Dynamic Thread Table](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#12-dynamic-thread-table--resource-driven-thread-limits), [Dynamic Task Table](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#13-dynamic-task-table--reusable-pid-slot-allocation)).
- **CPU bandwidth control, lost-wakeup-safe wait and wake, and per-thread FPU state** ([Process/Job CPU Bandwidth Control](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#14-processjob-cpu-bandwidth-control-opus), [Wait/Wake Transaction Locking](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#15-waitwake-transaction-locking-lost-wakeup-class-opus), [XSAVE / FPU State Ownership](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md#16-move-xsave--fpu-state-ownership-from-struct-task-to-struct-thread)).

## How does it compare with Windows 11 and Linux?

Windows 11 schedules 32 priority levels with dynamic boosts, affinity and ideal processors; Linux has CFS (now EEVDF), `SCHED_FIFO`, `SCHED_RR`, `SCHED_DEADLINE`, `sched_setaffinity` and cpufreq. Impossible OS matches Windows' 32-level fixed range and has priority inheritance, but has no dynamic boosts, fair-share accounting, real-time classes or affinity; only the per-thread kernel stack row of the roadmap's table is done. Deadline scheduling with bandwidth reclaim is planned parity with Linux (`SCHED_DEADLINE` with `SCHED_FLAG_RECLAIM`) and an addition beyond Windows; the planned addition beyond both is a single `/sys/sched` snapshot of every thread.

## See also

- [Scheduler Enhancement roadmap](../../todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md)
- [SMP Phase 2](smp-phase2.md)
- [Synchronisation Primitives](advanced-sync.md)
- [IRQL, DPCs and APCs](../kernel/irql-dpc.md)
- [Interrupt and Timer Architecture](../boot/interrupt-timer-architecture.md)
