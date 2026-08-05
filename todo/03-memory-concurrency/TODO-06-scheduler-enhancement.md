---
schema_version: 1
id: scheduler-enhancement
domain: 03-memory-concurrency
status: active
title: "TODO-06 -- Scheduler Enhancement"
---

# TODO-06 -- Scheduler Enhancement

> **Goal:** Evolve the current round-robin dispatcher into a production-quality scheduler: O(1) priority queues, starvation-proof dynamic aging, CFS vruntime fair sharing, `SCHED_FIFO`/`SCHED_RR`/`SCHED_DEADLINE` real-time classes, CPU affinity, accurate tick calibration via RDTSC+HPET, a unified `/sys/sched` stats view, CPU frequency scaling hooks for ACPI P-states, and dynamic resource-driven thread limits instead of a tiny fixed per-process slot ceiling.

> [!IMPORTANT]
> The current scheduler tick source is the LAPIC timer with a hardcoded ICR. On Hyper-V Gen 2, the actual bus frequency differs, making quanta unpredictable. §8 tick calibration fixes this and is a hard prerequisite for accurate CFS vruntime accounting in §3. Do not implement §3 before §8 is done.

## Inputs

- `src/kernel/sched/sched.c` (planned -- created by §1)
- `include/kernel/sched/sched.h` (planned -- created by §1)
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c)
- [`include/kernel/sched/task.h`](../../include/kernel/sched/task.h)
- [`src/kernel/drivers/lapic.c`](../../src/kernel/drivers/lapic.c)
- → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §5` -- HPET and LAPIC calibration APIs consumed by §4
- → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §4` -- hypervisor detection before timer selection; §9 Hyper-V synthetic timer fallback depends on it
- → XREF: `02-kernel-core/TODO-07-irql-model-dpcs.md` -- scheduler tick ISR runs at `DISPATCH_LEVEL`; RT scheduling interacts with DPC queuing and IRQL transitions
- → XREF: `02-kernel-core/TODO-08-time-filetime-management.md §3` -- `uptime_ns()` used for CFS vruntime accounting in §1 and for EDF deadline tracking in §3
- → XREF: `04-drivers-hardware` domain -- ACPI `_PSS` P-state table needed for §9 CPU frequency scaling governors
- → XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §5` -- SSDT indices 0x0180--0x0186 reserved for Worker Factory (kernel thread pool) syscalls; §10 wires them
- → XREF: `02-kernel-core/TODO-21-process-model-extensions.md §5,§10` -- scope overlap: TODO-21 §5 defines the process-level `sched_policy` field and `NtSetInformationProcess(ProcessSchedulingPolicy)` API; TODO-21 §10 defines `SetProcessAffinityMask`. Scheduler loop changes for RT classes (§4) and thread-level affinity (§6) are authoritative HERE.
- → XREF: `02-kernel-core/TODO-11-peb-teb-user-abi.md §15` -- current user-thread TEB placement is `TEB_USER_BASE - tid * 0x1000`; §12 here must decouple thread identity from storage slot and replace tid-derived VA placement with an allocator-backed scheme
- → XREF: `02-kernel-core/TODO-21-process-model-extensions.md §9` -- process resource limits own quota fields and API surface; §12 here consumes those limits during thread admission and replaces fixed `THREAD_MAX` exhaustion with resource-driven failures

## Outcome

- The scheduler always picks the highest-priority ready thread in O(1) via a 64-bit run-queue bitmap and `bsf`; no O(N) scan.
- Low-priority threads can never starve -- `sleep_avg`-based aging promotes waiting threads toward priority 0 at a configurable rate.
- Threads at the same priority share CPU proportionally via CFS vruntime; a high-priority thread gets a larger weight share but cannot completely starve equal-class peers.
- `SCHED_FIFO` and `SCHED_RR` real-time threads always preempt any `SCHED_NORMAL` thread; `SCHED_DEADLINE` threads are admitted only if utilization stays below 90%.
- Per-thread `affinity_mask` pins threads to specific CPUs; the scheduler respects it when distributing work across run queues.
- The scheduler tick is calibrated via RDTSC + HPET measurement to produce accurate nanosecond quanta; Hyper-V synthetic timer is used as a fallback.
- `/sys/sched` VFS file and `sched` shell command expose per-thread class, priority, vruntime, CPU time, and context-switch counts in a single snapshot.
- The scheduler exposes a `cpufreq_governor_t` vtable; `performance` and `powersave` governors track idle fraction and request P-state changes via ACPI.
- Thread creation is limited by memory, quotas, and user VA space rather than a small compile-time `THREAD_MAX` array ceiling; stable TIDs survive slot reuse and table growth.

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On                               | Status |
| --- | :---: | ---------------------------------------- | ---------------------------------------- | :----: |
| 💎   |   1   | §1 40-level priority queues + O(1) bitmap dequeue | --                                       |  [ ]   |
| 💎   |   2   | §2 Dynamic priority aging (starvation prevention) | §1                                       |  [ ]   |
| 💎   |   3   | §8 Scheduler tick calibration (RDTSC+HPET) | D01 T11 §3, 01-boot-platform/TODO-09-cpu-boot-sequencing.md §4 |  [ ]   |
| 💎   |   4   | §3 CFS vruntime + `prio_to_weight` table | §1, §3 (tick calibration)                |  [ ]   |
| 💎   |   5   | §4 `SCHED_FIFO` / `SCHED_RR` real-time classes | §1, §4 (CFS baseline)                    |  [ ]   |
| ⭐   |   6   | §5 `SCHED_DEADLINE` EDF scheduling       | §5 (RT infra), §4 (CFS)                  |  [ ]   |
| 💎   |   7   | §6 CPU affinity (`ThreadAffinityMask`)   | §1 (per-CPU run queues)                  |  [ ]   |
| ⭐   |   8   | §7 Scheduler stats + `/sys/sched` VFS file | §1–§5 (meaningful data)                  |  [ ]   |
| 💎   |   9   | §9 CPU frequency scaling hook + P-state governors | §7 (load measurement), ACPI              |  [ ]   |
| 💎   |  10   | Worker Factory syscalls wired to SSDT    | §1, D02 T12 §4                           |  [ ]   |
| 💎   |  11   | Per-thread kernel stack + TSS.rsp0 switching | --                                       |  [x]   |
| 💎   |  12   | Dynamic thread table + resource-driven thread limits | §11, D02 T11 §15, D02 T21 §9             |  [ ]   |
| 💎   |  13   | Dynamic task table + reusable PID slot allocation | §12                                      |  [ ]   |
| ⭐   |  14   | §14 Process/job CPU bandwidth control (cap/reserve) | D02 T25 §7 (rate record), §8 (tick calibration) |  [ ]   |
| 💎   |  15   | §15 Wait/wake transaction locking (lost-wakeup fix) | §1 (run-queue lock granularity)          |  [ ]   |

> 💎 = parity -- Windows and Linux both implement priority queues, aging, CFS-equivalent, RT classes, affinity, tick calibration, and cpufreq; Impossible OS must match.
> ⭐ = exclusive -- `SCHED_DEADLINE` with GRUB bandwidth reclaim and the unified `/sys/sched` all-threads snapshot are differentiators over the base Windows NT scheduler.

---

## 1. 40-Level Priority Queues + O(1) Bitmap Dequeue

Replace the single round-robin run-queue with 40 per-priority FIFO rings. A 64-bit bitmap marks which levels are non-empty; `bsf` (bit-scan forward) finds the highest-priority non-empty level in one instruction -- strictly O(1) regardless of thread count.

**Files:** `include/kernel/sched/sched.h`, `src/kernel/sched/sched.c`

> [!IMPORTANT]
> Per-CPU run queues must be protected by a per-CPU spinlock, not a global lock. A global run-queue lock becomes the primary scalability bottleneck on SMP. Design the lock granularity now even if SMP is not yet active -- retrofitting it later is more disruptive than getting it right here.

- [ ] Define `run_queue_t`: 40-entry array of FIFO ring-buffers, `uint64_t bitmap` (bit N = priority N non-empty)
- [ ] `rq_enqueue(rq, thread)` -- push to per-priority ring, set bitmap bit N with `bitmap |= (1ULL << prio)`
- [ ] `rq_dequeue(rq)` -- `prio = bsf(bitmap)`, pop from `rings[prio]`, clear bit if ring is now empty
- [ ] Add `priority` (0–39, 0 = highest) and `base_priority` fields to `task_t`
- [ ] Default priorities: user threads = 20, kernel service threads = 5, idle thread = 39
- [ ] `thread_set_priority(tid, prio)` syscall; validated range 0–39
- [ ] Boot log: `[SCHED] priority queues active: 40 levels, O(1) bitmap dispatch`
- [ ] Commit: `"sched: 40-level priority queues with O(1) bitmap dequeue"`

## 2. Dynamic Priority Aging

Guarantee every thread eventually runs by boosting the effective priority of READY threads that have been waiting too long -- an exponential moving average (`sleep_avg`) raises the effective priority toward 0 while the thread starves, and resets when it runs.

**Files:** `src/kernel/sched/sched.c`, `include/kernel/sched/sched.h`

- [ ] Add to `task_t`: `ticks_waiting` (increments each scheduler tick while READY but not dispatched), `effective_priority` (starts at `base_priority`, boosted by aging)
- [ ] In the scheduler tick ISR: for each READY-but-not-running thread, `ticks_waiting++`; when `ticks_waiting >= AGING_THRESHOLD` (default 200 ticks ≈ 2 s), decrement `effective_priority` by 1 toward 0, reset `ticks_waiting`
- [ ] `effective_priority` is clamped at 0 (never exceeds highest level)
- [ ] On thread dispatch: `effective_priority = base_priority`, `ticks_waiting = 0`
- [ ] Configurable via Registry `HKLM\SYSTEM\Scheduler\AgingThresholdTicks`
- [ ] Commit: `"sched: dynamic priority aging -- starvation prevention via ticks_waiting boost"`

## 3. CFS vruntime + `prio_to_weight` Table

Completely Fair Scheduler: within each priority level threads share CPU in proportion to their weight. Each thread tracks `vruntime` (nanoseconds of CPU time, normalised by weight); the scheduler always dispatches the thread with the lowest `vruntime`. High-weight (high-priority) threads accumulate `vruntime` more slowly and therefore get more CPU.

**Files:** `src/kernel/sched/sched.c`, `include/kernel/sched/sched.h`

> [!IMPORTANT]
> Accurate nanosecond tick measurement (§8) must be complete before this section. Hardcoded ICR tick durations produce wrong `vruntime` increments and make the weight table meaningless.
> CFS does **not** replace the 40-level priority structure from §1 -- it operates **within** each priority level. Threads at priority 10 share fairly among themselves; priority 10 as a group still preempts priority 20 entirely.

- [ ] Add to `task_t`: `vruntime` (`uint64_t`, nanoseconds), `priority_weight` (from table below)
- [ ] Define `prio_to_weight[40]` -- Linux-compatible weight table: `prio 0 = 88761`, `prio 20 = 1024`, `prio 39 = 15`
- [ ] On context switch out: `thread->vruntime += (elapsed_ns * 1024) / thread->priority_weight`
- [ ] Per-priority run queue sorted by `vruntime` (min-heap or sorted list); `rq_dequeue` returns minimum
- [ ] New threads start at `vruntime = min_vruntime` of their priority level (no catch-up debt)
- [ ] Variable time slice: run until `vruntime > min_vruntime + target_latency / n_ready_threads`; `target_latency` default 6000 µs, configurable via `HKLM\SYSTEM\Scheduler\TargetLatencyUs`
- [ ] Track `min_vruntime` per priority level; advance only forward (never decrease)
- [ ] Boot log: `[SCHED] CFS vruntime active; target_latency=%u µs`
- [ ] Commit: `"sched: CFS vruntime fair scheduling -- prio_to_weight table, min_vruntime"`

## 4. `SCHED_FIFO` / `SCHED_RR` Real-Time Classes

Two POSIX real-time scheduling classes that always preempt any `SCHED_NORMAL` thread: `SCHED_FIFO` runs until it blocks or yields (no time slice); `SCHED_RR` round-robins with a 10 ms hard slice within the same RT priority level.

**Files:** `src/kernel/sched/sched.c`, `include/kernel/sched/sched.h`

> [!CAUTION]
> A `SCHED_FIFO` thread at RT priority 99 that never yields will starve all other threads including the kernel watchdog. The watchdog must be `SCHED_FIFO` at priority 99, ensuring it cannot itself be starved. Only `CAP_SCHED_RT` threads may enter RT classes.

- [ ] Add `sched_class` enum to `task_t`: `SCHED_NORMAL`, `SCHED_FIFO`, `SCHED_RR`, `SCHED_DEADLINE`
- [ ] Add `rt_priority` (0–99) to `task_t`; ignored for `SCHED_NORMAL`
- [ ] RT run queue: a separate fixed-priority queue checked before all `SCHED_NORMAL` queues; `SCHED_FIFO` threads at the same RT priority round-robin only on yield/block
- [ ] `SCHED_FIFO`: no slice expiry; preempted only by a higher-RT-priority thread becoming READY
- [ ] `SCHED_RR`: 10 ms hard slice (`SCHED_RR_TIMESLICE_NS = 10_000_000`); on expiry, re-enqueue at tail of same RT-priority level
- [ ] `thread_set_sched(tid, class, rt_prio)` -- requires `CAP_SCHED_RT`; returns `STATUS_PRIVILEGE_NOT_HELD` otherwise
- [ ] Kernel watchdog thread: spawn at `SCHED_FIFO`, `rt_priority = 99` before any user thread
- [ ] Commit: `"sched: SCHED_FIFO / SCHED_RR real-time classes, CAP_SCHED_RT enforcement"`

## 5. `SCHED_DEADLINE` EDF Scheduling

Earliest-Deadline-First scheduling for hard real-time tasks: each thread declares a `(runtime, deadline, period)` triple; the scheduler always dispatches the thread whose deadline is nearest. Admission control at 90% utilisation prevents over-subscription; GRUB bandwidth reclaim returns unused runtime to other deadline threads.

**Files:** `src/kernel/sched/sched.c`, `include/kernel/sched/sched.h`

> [!IMPORTANT]
> EDF admission control: the sum of `runtime/period` over all admitted `SCHED_DEADLINE` threads must not exceed 0.9 (90% CPU utilisation). Reject `thread_set_sched(SCHED_DEADLINE)` with `STATUS_INSUFFICIENT_RESOURCES` if admission would exceed this bound.

- [ ] Add to `task_t` (for `SCHED_DEADLINE` only): `dl_runtime_ns`, `dl_deadline_ns`, `dl_period_ns`, `dl_remaining_ns`, `dl_absolute_deadline`
- [ ] Implement `deadline_admissibility_check()` -- sum `runtime/period` over all admitted threads; reject if sum + new entry > 0.9
- [ ] EDF run queue: ordered by `dl_absolute_deadline`; `rq_dequeue_edf()` returns the thread with the earliest deadline
- [ ] On each scheduler tick: decrement `dl_remaining_ns` by tick duration; if `dl_remaining_ns == 0`, suspend thread until next period
- [ ] On period renewal: `dl_absolute_deadline += dl_period_ns`, `dl_remaining_ns = dl_runtime_ns`, re-enqueue
- [ ] GRUB bandwidth reclaim: if the current deadline thread has unused runtime when a lower-priority deadline thread has an earlier absolute deadline, donate remaining bandwidth
- [ ] `thread_set_sched(tid, SCHED_DEADLINE, &sched_attr)` -- `sched_attr` contains runtime/deadline/period; admission check runs before activation
- [ ] Commit: `"sched: SCHED_DEADLINE EDF scheduling -- admission control, GRUB bandwidth reclaim"`

## 6. CPU Affinity

Pin threads to specific CPUs by setting an `affinity_mask` bitmask on the task; the scheduler only enqueues the thread on a run queue whose CPU bit is set in the mask.

**Files:** `include/kernel/sched/task.h`, `src/kernel/sched/sched.c`, `src/kernel/sched/syscall.c`

- [ ] Add `affinity_mask` (`uint64_t`, bit N = CPU N allowed) to `task_t`; default `0xFFFFFFFFFFFFFFFF` (all CPUs)
- [ ] In `rq_enqueue()`: if `affinity_mask` has exactly one CPU bit set, force enqueue to that CPU's run queue; otherwise balance normally
- [ ] `NtSetInformationThread(handle, ThreadAffinityMask, &mask, sizeof(mask))` -- store mask, re-queue thread if needed (→ XREF `02-kernel-core/TODO-12-native-api-ssdt.md`)
- [ ] `NtQueryInformationThread(handle, ThreadAffinityMask, &mask, ...)` -- return current mask
- [ ] Win32 `SetThreadAffinityMask(thread, mask)` → `NtSetInformationThread(ThreadAffinityMask)`
- [ ] Commit: `"sched: CPU affinity -- affinity_mask in task_t, NtSetInformationThread wiring"`

## 7. Scheduler Stats + `/sys/sched` VFS File

Expose per-thread scheduler metrics as a readable VFS file -- class, priority, vruntime, CPU time, voluntary and involuntary context-switch counts -- queryable by the `sched` shell command and Task Manager without a dedicated syscall.

> [!NOTE]
> → XREF: `02-kernel-core/TODO-21-process-model-extensions.md §8` already added per-PROCESS `vol_ctxsw`/`invol_ctxsw` and `user_time_ns`+`kernel_time_ns` to `struct task` (statistical tick accounting, incremented in `schedule()`/`schedule_now()`). This section's per-THREAD counters belong on `struct thread`, and the process-level row should AGGREGATE the §8 task totals -- do NOT re-instrument the switch paths or add parallel process-level counters.

**Files:** `src/kernel/fs/sysfs_sched.c` (new), `src/shell/cmd_sched.c` (new)

- [ ] Add to `task_t`: `cpu_time_ns`, `ctx_switches_vol` (yield/block), `ctx_switches_invol` (preemption), `slice_overruns`, `last_cpu`
- [ ] Increment counters at every context switch; distinguish voluntary (thread called `yield()`/`sleep()`) from involuntary (preempted by higher priority or tick expiry)
- [ ] Implement `/sys/sched` VFS read callback: one row per thread -- `TID  NAME  CLASS  PRI  VRUNTIME  CPU_MS  VSWTCH  ISWTCH  CPU`
- [ ] `sched` shell command: reads `/sys/sched`, prints formatted table with totals
- [ ] Wire CPU-time column to Task Manager thread view
- [ ] Commit: `"sched: /sys/sched VFS file + sched shell command, per-thread stats"`

## 8. Scheduler Tick Calibration

Replace the hardcoded LAPIC ICR with a measured RDTSC + HPET calibration that produces accurate nanosecond tick intervals on any hardware. Fall back to the Hyper-V synthetic timer on Gen 2 VMs where HPET is absent.

**Files:** `src/kernel/drivers/lapic.c`, `include/kernel/drivers/lapic.h`, `src/kernel/sched/sched.c`

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §5` -- unified timer HAL (`uptime_ns()` and HPET calibration APIs) must be ready before this section; do not duplicate the HPET read path.
> → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §4` -- hypervisor detection must be complete so the Hyper-V synthetic timer fallback can be safely branched on.

- [ ] At boot, measure LAPIC timer frequency: set ICR to a known value, read RDTSC before and after `N` PIT/HPET ticks, compute `lapic_hz = (N * hpet_period_ns) / rdtsc_delta`
- [ ] Derive `tick_icr` such that the LAPIC fires every `SCHED_TICK_NS` (default 1 ms = 1 000 000 ns)
- [ ] Store `g_tick_ns` (actual calibrated tick duration in nanoseconds); CFS uses this for `vruntime` accounting
- [ ] On hypervisor detection (`hypervisor_detect()` → Hyper-V): use `MSR_HV_STIMER0_CONFIG` synthetic timer instead of LAPIC ICR; programmatically set next trigger
- [ ] Boot log: `[SCHED] tick calibrated: ICR=%u, tick=%u ns` or `[SCHED] tick: Hyper-V synthetic timer`
- [ ] Commit: `"sched: tick calibration -- RDTSC+HPET measurement, Hyper-V synthetic timer fallback"`

## 9. CPU Frequency Scaling Hook

Define a `cpufreq_governor_t` vtable and wire two built-in governors -- `performance` (always max P-state) and `powersave` (scale down on idle) -- to the scheduler's per-CPU load measurement. The ACPI P-state transition is deferred to the drivers domain; this section owns the scheduler-side load tracking and governor policy.

**Files:** `include/kernel/sched/cpufreq.h` (new), `src/kernel/sched/cpufreq.c` (new), `src/kernel/sched/sched.c`

> [!IMPORTANT]
> → XREF: `04-drivers-hardware` domain -- `cpufreq_set_pstate(cpu, pstate)` low-level implementation (ACPI `_PSS` MSR write) lives in the drivers domain. This section defines the vtable and policy; the driver registers the hardware callback.

- [ ] Define `cpufreq_governor_t`: vtable with `governor_tick(cpu, load_pct)` and `governor_init(cpu)` callbacks
- [ ] Track per-CPU idle fraction: `idle_ticks / total_ticks` over a 100 ms sliding window; expose as `sched_cpu_load_pct(cpu)` (0–100)
- [ ] `performance` governor: always calls `cpufreq_set_pstate(cpu, 0)` (highest P-state); no load check
- [ ] `powersave` governor: on `load_pct > 80` for 200 ms → `cpufreq_set_pstate(cpu, 0)`; on `load_pct < 20` for 500 ms → `cpufreq_set_pstate(cpu, max_pstate)`; hysteresis prevents oscillation
- [ ] `cpufreq_register_driver(cpu, &driver)` -- called by the ACPI cpufreq driver to install `cpufreq_set_pstate` callback
- [ ] Active governor selectable via Registry `HKLM\SYSTEM\Scheduler\CpufreqGovernor` (`"performance"` / `"powersave"`)
- [ ] Boot log: `[CPUFREQ] governor: %s; %u P-states available` (or `no cpufreq driver registered` if ACPI not ready)
- [ ] Commit: `"sched: cpufreq governor vtable -- performance and powersave, load tracking"`

## 10. Worker Factory Syscalls Wired to SSDT
The Worker Factory is the kernel-side thread pool (backs `TpAllocPool` / `CreateThreadpoolWork`). The scheduler owns thread creation/reaping logic. (→ XREF: TODO-12-native-api-ssdt.md §5)

- [ ] `NtCreateWorkerFactory(FactoryHandle, DesiredAccess, ObjectAttributes, CompletionPortHandle, WorkerProcessHandle, StartRoutine, StartParameter, MaxThreadCount, StackReserve, StackCommit)` → SSDT 0x0180
- [ ] `NtWorkerFactoryWorkerReady(WorkerFactoryHandle)` → SSDT 0x0181: signal that worker thread is idle and ready
- [ ] `NtReleaseWorkerFactoryWorker(WorkerFactoryHandle)` → SSDT 0x0182: release a worker back to pool
- [ ] `NtShutdownWorkerFactory(WorkerFactoryHandle, PendingWorkerCount)` → SSDT 0x0183
- [ ] `NtQueryInformationWorkerFactory(FactoryHandle, InfoClass, Buffer, Length, RetLen)` → SSDT 0x0184
- [ ] `NtSetInformationWorkerFactory(FactoryHandle, InfoClass, Buffer, Length)` → SSDT 0x0185
- [ ] `NtWaitForWorkViaWorkerFactory(FactoryHandle, MiniPacket, ...)` → SSDT 0x0186: block until work item available
- [ ] Commit: `"sched: wire Worker Factory syscalls to SSDT (0x0180--0x0186)"`

**Test checkpoint:** `NtCreateWorkerFactory` tied to I/O completion port creates pool. `NtWaitForWorkViaWorkerFactory` blocks; posting to IOCP wakes a worker. `NtShutdownWorkerFactory` drains all threads.

---

## 11. Per-Thread Kernel Stack + TSS.rsp0 Switching

The scheduler currently stores `kernel_rsp` per-task (in `struct task`), not per-thread. `tss_set_kernel_stack()` and `smp_this_cpu()->syscall_rsp0` are updated only on task switches. When multiple ring-3 threads share a task, they all enter ring 0 on the same kernel stack -- a second interrupt or syscall while the first thread's kernel frame is live would corrupt it.

> [!IMPORTANT]
> **Prerequisite for D02 T11 §14 (`uthread_create`).** Discovered during Codex design review 2026-04-10: `uthread_create()` allocates a per-thread kernel stack, but the scheduler never tells the TSS about it. Intra-task thread switches leave `rsp0` pointing at the previous thread's (or main thread's) kernel stack, so ring-3 → ring-0 transitions land on the wrong stack.

→ XREF: [`02-kernel-core/TODO-11-peb-teb-user-abi.md §14`](../02-kernel-core/TODO-11-peb-teb-user-abi.md) -- consumer (uthread_create per-thread kernel stack)

- [x] Add `uint64_t kernel_rsp` to `struct thread` in `include/kernel/sched/task.h` -- top of this thread's kernel stack (for TSS rsp0). Initialize to 0 for kernel threads. Also added `kernel_stack_base`, `kernel_stack_pages`, `_kstack_pad`.
- [x] Add `uint8_t *kernel_stack_base` and `uint32_t kernel_stack_pages` to `struct thread` -- ownership metadata for PMM-allocated kernel stacks. Enables correct deallocation at `thread_join()` via `thread_free_stacks()` helper.
- [x] In `schedule()` and `schedule_now()`: changed from `tasks[next_task].kernel_rsp` to `tasks[next_task].threads[next_thread].kernel_rsp`. Reads per-thread, not per-task. Skip if 0 (kernel thread).
- [x] ALL 4 task-creation paths mirror `kernel_rsp` into `threads[0]`: `task_create()`, `task_create_user()`, `task_fork()`, `task_exec()`. Codex design review caught `task_create_user` and `task_fork` which were missing from the original checklist.
- [x] `task_cleanup()` walks secondary threads (tid 1+) and calls `thread_free_stacks()` for each. Thread 0 uses the task-level `kfree` path (unchanged).
- [x] `thread_join()` refactored to use `thread_free_stacks()` helper. Handles both PMM-allocated (`kernel_stack_pages > 0`) and kmalloc'd (`stack_base != NULL`) stacks correctly.
- [x] Unit test in `test_sched.c`: `test_per_thread_kernel_rsp_mirror` -- verifies PID 0 and PID 1 `threads[0].kernel_rsp == tasks[pid].kernel_rsp` mirror invariant. Pure read-only oracle check.
- [x] Commit: `"sched: per-thread kernel_rsp + TSS.rsp0 switching on intra-task thread switch"` (3bf99ed0)

**Test checkpoint:** Boot completes normally (single-threaded tasks unchanged). DPC worker, work queue, cmd.exe all still function. The scheduler log shows `[sched] rsp0 updated` on task switches. No kernel stack corruption on interrupt entry. Test on: QEMU WHPX (2 CPUs), TCG, VirtualBox, bare metal.

---

## 12. Dynamic Thread Table + Resource-Driven Thread Limits

The current thread model uses `struct task { struct thread threads[THREAD_MAX]; uint32_t num_threads; }`, where TID is implicitly the array slot and the user-mode TEB/stack layout derives virtual addresses from that bounded TID. Slot reuse fixes leaks, but it does not remove the architectural ceiling: thread creation still bottoms out at a small compile-time constant and the user-mode ABI still assumes tightly bounded TIDs. This section replaces the fixed-slot design with a full resource-driven implementation.

> [!IMPORTANT]
> This is a full feature, not a temporary migration note. The target state is that thread admission fails only for real reasons: memory exhaustion, process quota, or user VA allocator exhaustion. A tiny fixed `THREAD_MAX` is an implementation artifact to remove, not a product requirement to preserve.

> [!NOTE]
> → XREF: [`02-kernel-core/TODO-11-peb-teb-user-abi.md §15`](../02-kernel-core/TODO-11-peb-teb-user-abi.md) -- per-thread TEB allocation currently uses `TEB_USER_BASE - tid * 0x1000`; this section must replace tid-derived user VA placement with allocator-backed TEB and user-stack reservations.
> → XREF: [`02-kernel-core/TODO-21-process-model-extensions.md §9`](../02-kernel-core/TODO-21-process-model-extensions.md) -- process resource limits own the quota surface; this section consumes those limits when admitting new threads.

- [ ] Replace fixed inline `threads[THREAD_MAX]` storage with growable per-task thread storage (`thread_capacity`, reusable free-slot tracking, explicit live-thread count). `num_threads` must stop meaning both "highest used slot" and "number of live threads".
- [ ] Separate stable thread identity from storage slot. Add monotonic per-task TID allocation (or equivalent stable ID source) so TID survives slot reuse and table compaction. All scheduler, handle-table, and `NtQueryInformationThread` paths must stop assuming `tid == slot_index`.
- [ ] Add reusable slot/free-list logic for dead joined threads. Slot reuse must be race-safe and must not block forever on double-join or stale TIDs.
- [ ] Replace `TEB_USER_BASE - tid * 0x1000` and `USER_THREAD_STACK_BASE - tid * stride` placement with allocator-backed user VA reservations for TEB pages and user-thread stacks. Distinct live threads must never alias VAs even when storage slots are reused.
- [ ] Update `uthread_create`, `NtCreateThread_handler`, and thread teardown paths to consume allocator-backed TEB and user-stack reservations, and to release them on thread death when the required unmap/shootdown infrastructure exists.
- [ ] Make thread admission resource-driven: fail with `STATUS_INSUFFICIENT_RESOURCES` or quota-specific errors when stack pages, TEB pages, process thread quota, or user VA space cannot be reserved. Remove fixed-constant exhaustion as the primary failure mode.
- [ ] Define inheritance and accounting rules for new threads: process affinity mask, scheduling policy/class, capability/mitigation state, and any future per-process thread quota must copy or intersect deterministically at creation time.
- [ ] Add regression tests: create/join cycles beyond the old `THREAD_MAX`, sparse TID reuse, repeated kernel-thread slot reuse, user-thread TEB/stack uniqueness after slot reuse, quota exhaustion, and cleanup correctness on process exit.
- [ ] Commit: `"sched: dynamic thread table and resource-driven thread limits"`

**Test checkpoint:** Creating and joining more than 16 kernel threads in the same process no longer fails on a fixed slot ceiling. Multiple live user threads have unique TEB and stack VAs even after earlier threads exit and slots are reused. Thread creation fails only for real resource reasons (quota, VA exhaustion, page allocation failure), and the returned status identifies the real limit. `NtQueryInformationThread` and Ob thread lookup continue to return stable thread identities after table growth and slot reuse. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

---

## 13. Dynamic Task Table + Reusable PID Slot Allocation

Mirror of §12 applied to tasks (processes): the current `tasks[TASK_MAX]` storage with `task_create()` using `pid = num_tasks++` monotonically means the global task table is **append-only**, not reusable. `task_cleanup()` frees handles + stacks + TEBs but never restores the slot -- so after `TASK_MAX` process creations (32 today), every subsequent `task_create()` returns -1 even though dead slots are sitting there fully torn down.

> [!IMPORTANT]
> Not triggered today (the user-mode test launcher ships only 2 binaries in the manifest), but becomes a hard blocker the moment the planned §9-§15 test suite lands (7-8 sequential task_create calls on top of boot-side cmd.exe/shell tasks; and the manifest advertises a 128-entry `UTEST_MANIFEST_MAX` ceiling). Caught by Codex adversarial re-review of 00-infrastructure/TODO-04 §4 (2026-04-20, commit `11e816fb`). Section §4 of the user-mode test framework already uses the consumer API and therefore validates the fix.

> [!NOTE]
> → XREF: [`00-infrastructure/TODO-04-usermode-test-framework.md §4`](../00-infrastructure/TODO-04-usermode-test-framework.md) -- user-mode test launcher is the first consumer to hit the ceiling once §9-§15 binaries ship.
> → XREF: [`00-infrastructure/TODO-04-usermode-test-framework.md §8`](../00-infrastructure/TODO-04-usermode-test-framework.md) -- stress binaries currently loop internally because a launcher-side loop of N spawns would exhaust TASK_MAX; once this section lands, §8 can promote `stress_iters` from "reserved" to a runtime-tunable launcher policy.
> → XREF: `§12` -- thread-table dynamic growth, same design shape applied to threads; this section consumes the same allocator-backed VA pattern for task-level TEB/kernel-stack placement.

- [ ] Replace `static struct task tasks[TASK_MAX]; static uint32_t num_tasks` in [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) with growable task storage (`task_capacity`, explicit `live_task_count`, reusable PID free-list). `num_tasks` must stop meaning both "highest used slot" and "number of live tasks".
- [ ] Move `static uint32_t current_task` (today a single global in [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c)) into per-CPU state (`smp_this_cpu()->current_task_pid` or a pointer field). Every consumer of `task_current()` / `current_task` (scheduler, syscall dispatchers, fault-injection bridge, signal delivery, task_cleanup owner-check) must read from the current CPU's per-CPU record so that concurrent schedule() calls on other CPUs cannot stomp the identity of a task currently executing on THIS CPU. Caught by Codex adversarial review of the user-mode fault-injection bridge (2026-04-20) -- `sys_fault_inject_dispatch` reads `task_current()->pid` to scope the per-task filter, and a concurrent AP schedule() write to the global can hand it the wrong PID. Until this lands, SYS_FAULT_INJECT stamps a "SMP task-filter race bounded by the global current_task" caveat via an Accepted XREF; consumers that need stronger guarantees must synchronize externally.
- [ ] Separate stable PID from storage slot. Add monotonic PID allocation with reuse-after-drain (or equivalent stable ID source) so PID survives slot reuse and table compaction. All `task_get_by_pid`, `signal_send`, `task_waitpid`, and `NtQueryInformationProcess` paths must stop assuming `pid == slot_index`.
- [ ] Add reusable slot/free-list logic for dead tasks: `task_cleanup()` returns the slot to a free-list; `task_create()` pops from the free-list before appending/growing. Slot reuse must be race-safe (spinlock or RCU-style list) and must not resurrect a stale pointer held by the launcher or signal layer.
- [ ] Atomic task-slot CLAIM: two creators (a tick preempts ring-0 mid-`task_create`) must never select the same `num_tasks` slot -- reserve/publish atomically. -> XREF: `02-kernel-core/TODO-21-process-model-extensions.md §9`
- [ ] Replace the `num_tasks < TASK_MAX` creation gate with resource-driven admission: fail with `STATUS_INSUFFICIENT_RESOURCES` or quota-specific errors when kernel stack pages, PEB/TEB pages, or process quota cannot be reserved. Remove fixed-constant exhaustion as the primary failure mode.
- [ ] Update `u_run_one()` in [`src/kernel/test/test_usermode.c`](../../src/kernel/test/test_usermode.c) path (the user-mode launcher) and other sequential-create consumers (cmd.exe launch in [`boot_desktop.c`](../../src/kernel/main/boot_desktop.c)) to tolerate sparse PIDs -- today they store the returned pid but never compare against a prior pid, so this should be a no-op; confirm with a code-truth pass.
- [ ] Add regression tests under TEST_CAT_SCHED: create + cleanup cycles beyond the old `TASK_MAX`, sparse PID reuse, repeated task-slot reuse, quota exhaustion, and cleanup correctness. Prior dead slots must be reclaimed; the `N+1`-th task creation on a TASK_MAX-sized table must succeed after N tasks exited.
- [ ] Commit: `"sched: dynamic task table and reusable PID slot allocation"`

> **Blocks:** TODO-21 §15/§18 (POSIX reaping / wait4 / ZOMBIE lifecycle). Reaping cannot return a `tasks[]` slot, and NT process-handle identity cannot stay stable across reuse, until stable-PID + slot-reuse (items above) land -> XREF: `02-kernel-core/TODO-21-process-model-extensions.md §15` (Deferred design spec). The `PROCESS_OBJECT` terminal-state ownership choice that pairs with reuse is operator-reserved -> `todo/answers.md` Q2.

**Test checkpoint:** Running the user-mode test launcher (TODO-04 §4) with a 64-entry manifest (synthetic test binaries generated for this regression) succeeds end-to-end; every binary spawns, runs, exits, and its slot is reused by a later binary with no `task_create failed` log. The existing TASK_MAX ceiling no longer gates boot-time test runs. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 14. Process/Job CPU Bandwidth Control `[Opus]`

Enforce the CPU rate-limit records published by the quota subsystem: a per-period runtime budget for a process or job, refilled each period, with reservation admission and throttle/unthrottle. This is the CONSUMER of the policy record; §3 owns proportional weights within a priority level and does NOT own caps, periods, or reservations.

**Files:** `src/kernel/sched/sched.c`, `src/kernel/sched/task.c`, `include/kernel/sched/sched.h`

> [!IMPORTANT]
> §3 (CFS weights) and this section are different mechanisms: a weight decides SHARE when threads compete, a bandwidth cap decides whether a runnable thread may run AT ALL this period. A weight table alone cannot cap an otherwise-idle system.

- [ ] Consume `quota_rate_limit_get()` for `QUOTA_RATE_CLASS_CPU` per process/job; honor NS budgets over `period_ns`. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §7` (item: "Define a rate-limit record")
- [ ] Per-period refill: charge consumed runtime against the budget each tick; refill at period rollover without drift accumulation.
- [ ] Throttle when `hard_cap` is exhausted (dequeue until refill) and unthrottle at rollover; a throttled thread must not spin the run queue.
- [ ] Hierarchical accounting: a member's runtime consumes both its own and its job's budget; the tighter of the two throttles.
- [ ] Reservation admission: refuse a `reservation` the remaining CPU capacity cannot honor, rather than overcommitting silently.
- [ ] Re-read policy on `generation` change only, so the hot path does not copy the record every tick.
- [ ] Commit: `"sched: process/job CPU bandwidth control -- period refill, hard cap throttle, reservation admission"`

**Test checkpoint:** a process with a `hard_cap` of 20% of `period_ns` consumes no more than that over 10 periods on an otherwise idle CPU; a job cap throttles its members collectively even when each member is individually under its own cap; a reservation exceeding remaining capacity is refused at admission; a throttled thread is off the run queue and resumes at refill.

---

## 15. Wait/Wake Transaction Locking (Lost-Wakeup Class) `[Opus]`

Every wait primitive publishes its waiter into the queue BEFORE setting `THREAD_BLOCKED`, and checks its predicate outside any lock. A waker running in that window finds the thread not yet blocked, dequeues it, and the wake is LOST: the waiter then stores `THREAD_BLOCKED` and sleeps forever. Confirmed in `enqueue_and_block` (`src/kernel/sched/event.c`) and present in the semaphore, mutex, condvar, and rwlock wait paths plus the `thread_join`/`thread_exit` handshake.

**Files:** `src/kernel/sched/event.c`, `semaphore.c`, `mutex.c`, `condvar.c`, `rwlock.c`, `task.c`, `src/kernel/ob/ob_mutex.c`

> [!IMPORTANT]
> This is a LATENT pre-existing race, not a regression: before the accounting seam landed, the waker's unconditional `state = THREAD_READY` was overwritten by the waiter's own `THREAD_BLOCKED` store, losing the wake identically. Swapping two stores is NOT sufficient; the predicate, the state transition, and the queue mutation must be one transaction.

- [ ] Add a per-primitive IRQ-safe wait lock covering predicate check, `THREAD_BLOCKED` transition, and queue publication as one critical section.
- [ ] Wakers take the same lock across dequeue + `task_wake_thread()`, so a waiter is never discoverable before it is blocked.
- [ ] Give each wait a generation token the wake must match: today the claim tests only `THREAD_BLOCKED`, so a delayed second waker can release a LATER, unrelated wait by the same thread (wake-epoch ABA).
- [ ] Make `thread->state` uniformly atomic: `task_wake_thread()` CASes it while ~16 other writers use plain stores, so the CAS orders nothing against them. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §7`
- [ ] Resolve the interrupted task through PER-CPU scheduler state: the tick charges `tasks[current_task]`, one global cursor, so once APs schedule a tick can bill the wrong process. -> XREF: `02-kernel-core/TODO-25 §7`
- [ ] Derive the tick quantum from the ACTUAL elapsed monotonic delta, not the nominal rate: a one-shot/tickless arm charges a full nominal quantum for an arbitrary interval. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §7`
- [ ] `thread_join`/`thread_exit` handshake: publish join metadata and blocked state as one transaction, re-check target death before sleeping. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §7`
- [ ] Deterministic regression test injecting a wake between queue publication and yield; the waiter must not sleep forever.
- [ ] Commit: `"sched: wait/wake transaction locking -- close the lost-wakeup window in all wait primitives"`

**Test checkpoint:** a test that publishes a waiter and forces a wake before the yield observes the thread runnable rather than permanently blocked; `thread_join` racing `thread_exit` never leaves the joiner asleep; all existing sched/ipc suites stay green.

---

## OS Comparison


| ⭐   | Feature                      | 🪟 Win11              | 🐧 Linux                   | 🚀 Impossible OS          |
| --- | ---------------------------- | -------------------- | ------------------------- | ------------------------ |
| 💎   | O(1) priority queues         | ✅ 32 levels          | ✅ 40 nice levels          | ⬜ §1 40-level bsf        |
| 💎   | Starvation prevention        | ✅ priority boost     | ✅ priority decay          | ⬜ §2 ticks_waiting aging |
| 💎   | CFS fair CPU share           | ❌ fixed-priority     | ✅ CFS vruntime            | ⬜ §3 CFS per-level       |
| 💎   | RT FIFO/RR classes           | ✅ REALTIME_CLASS     | ✅ SCHED_FIFO/RR           | ⬜ §4 FIFO+RR+CAP         |
| ⭐   | EDF deadline sched           | ❌ none               | ✅ SCHED_DEADLINE          | ⬜ §5 EDF+GRUB            |
| 💎   | CPU affinity                 | ✅ SetAffinityMask    | ✅ sched_setaffinity       | ⬜ §6 affinity_mask       |
| ⭐   | All-thread sched view        | ❌ ETW only           | ❌ per-process /proc       | ⬜ §7 /sys/sched          |
| 💎   | Tick calibration             | ✅ HAL HPET/TSC       | ✅ calibrate_delay         | ⬜ §8 RDTSC+HPET          |
| 💎   | CPU freq scaling             | ✅ power plans        | ✅ cpufreq governors       | ⬜ §9 governor vtable     |
| 💎   | Per-thread kernel stack      | ✅ per-KTHREAD        | ✅ per-task_struct         | ✅ §11 per-thread rsp0    |
| 💎   | Resource-driven thread limit | ✅ memory/quota bound | ✅ pid/task + memory bound | ⬜ §12 dynamic table      |

> Parity: matches Win11+Linux on priority queues, aging, RT classes, affinity, tick calibration, cpufreq, per-thread kernel stacks. EDF deadline scheduling and unified /sys/sched thread snapshot are exclusive.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_sched()` (see `src/kernel/test/test_runner.c`).
> Tests run with `debug=1` or `test=1` in boot.conf under `TEST_CAT_SCHED`.

- [ ] §1: `rq_enqueue` + `rq_dequeue` returns highest-priority thread
- [ ] §1: bitmap `bsf` selects correct level after enqueue/dequeue
- [ ] §2: `effective_priority` decrements after `AGING_THRESHOLD` ticks
- [ ] §3: `vruntime` increments proportional to inverse weight
- [ ] §4: RT thread preempts SCHED_NORMAL at any priority
- [ ] §5: admission rejects when utilization > 90%
- [ ] §11: `threads[0].kernel_rsp != 0` after `task_init()`
- [ ] §12: repeated `kthread_create` / `thread_join` cycles succeed well past the old `THREAD_MAX`
- [ ] §12: sparse TID reuse does not alias TEB or user-stack VAs across live threads

> **Note:** Most scheduler tests require multi-thread runtime behavior (preemption, timing, context switch) which cannot be validated in WSL. Test checkpoints in each section define serial-log criteria for QEMU/bare metal.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Priority: 3 threads at priority 5, 20, 39 -- verify priority 5 runs first, priority 39 only runs when others block
- [ ] Aging: CPU-spinner at priority 5 + sleeper at priority 39 -- after ~2 s serial log shows `[SCHED] aging boost` for the sleeper
- [ ] CFS: 3 threads at priority 10, 20, 30 -- CPU share ratio ≈ 4:2:1 after 5 s (`sched` command shows vruntime spread)
- [ ] `SCHED_FIFO`: RT-priority 50 thread preempts running priority-20 thread mid-slice; confirmed by context-switch counter
- [ ] `SCHED_DEADLINE`: admit two threads at 40% + 40% = 80% utilisation -- accepted; add third at 20% -- rejected with `STATUS_INSUFFICIENT_RESOURCES`
- [ ] CPU affinity: pin thread to CPU 0; `sched` command shows `last_cpu=0` across 100+ switches
- [ ] `sched` shell command prints table with all boot threads; CPU-time increments with each call
- [ ] Tick calibration: serial log shows `[SCHED] tick calibrated: ICR=N, tick=1000000 ns` (≈1 ms)
- [ ] Dynamic thread table: create/join more than 16 kernel threads in one process; no `max threads reached` until a real quota, memory, or VA limit is hit
- [ ] Dynamic user threads: multiple live threads receive distinct TEB and user-stack VAs even after slot reuse; no `tid * stride` aliasing
- [ ] Commit: `"sched: enhanced scheduler -- priority, aging, CFS, RT, EDF, affinity, tick cal, cpufreq"`

**Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched)

## History

| Date | Action | Summary |
|------|--------|---------|
| 2026-04-10 | validate | 11 sections checked; stripped 9 model tags, fixed 2 broken input anchors (planned files), fixed en-dash, compacted OS Comparison table (added §10/§11 rows), added Unit Tests skeleton, added §11 row to OS Comparison |

---
