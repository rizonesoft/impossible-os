# TODO-05 — Scheduler Enhancement

> **Goal:** Evolve the current round-robin dispatcher into a production-quality scheduler: O(1) priority queues, starvation-proof dynamic aging, CFS vruntime fair sharing, `SCHED_FIFO`/`SCHED_RR`/`SCHED_DEADLINE` real-time classes, CPU affinity, accurate tick calibration via RDTSC+HPET, a unified `/sys/sched` stats view, and a CPU frequency scaling hook for ACPI P-states.

> [!IMPORTANT]
> The current scheduler tick source is the LAPIC timer with a hardcoded ICR. On Hyper-V Gen 2, the actual bus frequency differs, making quanta unpredictable. §8 tick calibration fixes this and is a hard prerequisite for accurate CFS vruntime accounting in §3. Do not implement §3 before §8 is done.

## Inputs

- [`src/kernel/sched/sched.c`](../../src/kernel/sched/sched.c)
- [`include/kernel/sched/sched.h`](../../include/kernel/sched/sched.h)
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c)
- [`include/kernel/sched/task.h`](../../include/kernel/sched/task.h)
- [`src/kernel/drivers/lapic.c`](../../src/kernel/drivers/lapic.c)
- → XREF: `01-boot-platform/TODO-06-interrupt-timer-arch.md §3` — HPET and LAPIC calibration APIs consumed by §8
- → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §3` — hypervisor detection before timer selection; §8 Hyper-V synthetic timer fallback depends on it
- → XREF: `02-kernel-core/TODO-06-irql-model-dpcs.md` — scheduler tick ISR runs at `DISPATCH_LEVEL`; RT scheduling interacts with DPC queuing and IRQL transitions
- → XREF: `02-kernel-core/TODO-07-time-filetime-management.md §5` — `uptime_ns()` used for CFS vruntime accounting in §3 and for EDF deadline tracking in §5
- → XREF: `04-drivers-hardware` domain — ACPI `_PSS` P-state table needed for §9 CPU frequency scaling governors
- → XREF: `02-kernel-core/TODO-05-native-api-ssdt.md §4` — SSDT indices 0x0180–0x0186 reserved for Worker Factory (kernel thread pool) syscalls; §10 wires them
- → XREF: `02-kernel-core/TODO-09-process-model-extensions.md §5,§10` — scope overlap: TODO-09 §5 defines the process-level `sched_policy` field and `NtSetInformationProcess(ProcessSchedulingPolicy)` API; TODO-09 §10 defines `SetProcessAffinityMask`. Scheduler loop changes for RT classes (§4) and thread-level affinity (§6) are authoritative HERE.

## Outcome

- The scheduler always picks the highest-priority ready thread in O(1) via a 64-bit run-queue bitmap and `bsf`; no O(N) scan.
- Low-priority threads can never starve — `sleep_avg`-based aging promotes waiting threads toward priority 0 at a configurable rate.
- Threads at the same priority share CPU proportionally via CFS vruntime; a high-priority thread gets a larger weight share but cannot completely starve equal-class peers.
- `SCHED_FIFO` and `SCHED_RR` real-time threads always preempt any `SCHED_NORMAL` thread; `SCHED_DEADLINE` threads are admitted only if utilization stays below 90%.
- Per-thread `affinity_mask` pins threads to specific CPUs; the scheduler respects it when distributing work across run queues.
- The scheduler tick is calibrated via RDTSC + HPET measurement to produce accurate nanosecond quanta; Hyper-V synthetic timer is used as a fallback.
- `/sys/sched` VFS file and `sched` shell command expose per-thread class, priority, vruntime, CPU time, and context-switch counts in a single snapshot.
- The scheduler exposes a `cpufreq_governor_t` vtable; `performance` and `powersave` governors track idle fraction and request P-state changes via ACPI.

## Implementation Order

| ⭐  | Order | Deliverable                                          | Depends On                          | Status |
| --- | :---: | ---------------------------------------------------- | ----------------------------------- | :----: |
| 💎  |   1   | §1 40-level priority queues + O(1) bitmap dequeue    | —                                   |  [ ]   |
| 💎  |   2   | §2 Dynamic priority aging (starvation prevention)    | §1                                  |  [ ]   |
| 💎  |   3   | §8 Scheduler tick calibration (RDTSC+HPET)           | TODO-06 §3, TODO-04-cpu §3          |  [ ]   |
| 💎  |   4   | §3 CFS vruntime + `prio_to_weight` table             | §1, §3 (tick calibration)           |  [ ]   |
| 💎  |   5   | §4 `SCHED_FIFO` / `SCHED_RR` real-time classes       | §1, §4 (CFS baseline)               |  [ ]   |
| ⭐  |   6   | §5 `SCHED_DEADLINE` EDF scheduling                   | §5 (RT infra), §4 (CFS)             |  [ ]   |
| 💎  |   7   | §6 CPU affinity (`ThreadAffinityMask`)               | §1 (per-CPU run queues)             |  [ ]   |
| ⭐  |   8   | §7 Scheduler stats + `/sys/sched` VFS file           | §1–§5 (meaningful data)             |  [ ]   |
| 💎  |   9   | §9 CPU frequency scaling hook + P-state governors    | §7 (load measurement), ACPI         |  [ ]   |
| 💎  |  10   | Worker Factory syscalls wired to SSDT                | §1, TODO-05 §4                      |  [ ]   |

> 💎 = parity — Windows and Linux both implement priority queues, aging, CFS-equivalent, RT classes, affinity, tick calibration, and cpufreq; Impossible OS must match.
> ⭐ = exclusive — `SCHED_DEADLINE` with GRUB bandwidth reclaim and the unified `/sys/sched` all-threads snapshot are differentiators over the base Windows NT scheduler.

---

## 1. 40-Level Priority Queues + O(1) Bitmap Dequeue `[Opus]`

Replace the single round-robin run-queue with 40 per-priority FIFO rings. A 64-bit bitmap marks which levels are non-empty; `bsf` (bit-scan forward) finds the highest-priority non-empty level in one instruction — strictly O(1) regardless of thread count.

**Files:** `include/kernel/sched/sched.h`, `src/kernel/sched/sched.c`

> [!IMPORTANT]
> Per-CPU run queues must be protected by a per-CPU spinlock, not a global lock. A global run-queue lock becomes the primary scalability bottleneck on SMP. Design the lock granularity now even if SMP is not yet active — retrofitting it later is more disruptive than getting it right here.

- [ ] Define `run_queue_t`: 40-entry array of FIFO ring-buffers, `uint64_t bitmap` (bit N = priority N non-empty)
- [ ] `rq_enqueue(rq, thread)` — push to per-priority ring, set bitmap bit N with `bitmap |= (1ULL << prio)`
- [ ] `rq_dequeue(rq)` — `prio = bsf(bitmap)`, pop from `rings[prio]`, clear bit if ring is now empty
- [ ] Add `priority` (0–39, 0 = highest) and `base_priority` fields to `task_t`
- [ ] Default priorities: user threads = 20, kernel service threads = 5, idle thread = 39
- [ ] `thread_set_priority(tid, prio)` syscall; validated range 0–39
- [ ] Boot log: `[SCHED] priority queues active: 40 levels, O(1) bitmap dispatch`
- [ ] Commit: `"sched: 40-level priority queues with O(1) bitmap dequeue"`

## 2. Dynamic Priority Aging `[Sonnet]`

Guarantee every thread eventually runs by boosting the effective priority of READY threads that have been waiting too long — an exponential moving average (`sleep_avg`) raises the effective priority toward 0 while the thread starves, and resets when it runs.

**Files:** `src/kernel/sched/sched.c`, `include/kernel/sched/sched.h`

- [ ] Add to `task_t`: `ticks_waiting` (increments each scheduler tick while READY but not dispatched), `effective_priority` (starts at `base_priority`, boosted by aging)
- [ ] In the scheduler tick ISR: for each READY-but-not-running thread, `ticks_waiting++`; when `ticks_waiting >= AGING_THRESHOLD` (default 200 ticks ≈ 2 s), decrement `effective_priority` by 1 toward 0, reset `ticks_waiting`
- [ ] `effective_priority` is clamped at 0 (never exceeds highest level)
- [ ] On thread dispatch: `effective_priority = base_priority`, `ticks_waiting = 0`
- [ ] Configurable via Registry `HKLM\SYSTEM\Scheduler\AgingThresholdTicks`
- [ ] Commit: `"sched: dynamic priority aging — starvation prevention via ticks_waiting boost"`

## 3. CFS vruntime + `prio_to_weight` Table `[Opus]`

Completely Fair Scheduler: within each priority level threads share CPU in proportion to their weight. Each thread tracks `vruntime` (nanoseconds of CPU time, normalised by weight); the scheduler always dispatches the thread with the lowest `vruntime`. High-weight (high-priority) threads accumulate `vruntime` more slowly and therefore get more CPU.

**Files:** `src/kernel/sched/sched.c`, `include/kernel/sched/sched.h`

> [!IMPORTANT]
> Accurate nanosecond tick measurement (§8) must be complete before this section. Hardcoded ICR tick durations produce wrong `vruntime` increments and make the weight table meaningless.
> CFS does **not** replace the 40-level priority structure from §1 — it operates **within** each priority level. Threads at priority 10 share fairly among themselves; priority 10 as a group still preempts priority 20 entirely.

- [ ] Add to `task_t`: `vruntime` (`uint64_t`, nanoseconds), `priority_weight` (from table below)
- [ ] Define `prio_to_weight[40]` — Linux-compatible weight table: `prio 0 = 88761`, `prio 20 = 1024`, `prio 39 = 15`
- [ ] On context switch out: `thread->vruntime += (elapsed_ns * 1024) / thread->priority_weight`
- [ ] Per-priority run queue sorted by `vruntime` (min-heap or sorted list); `rq_dequeue` returns minimum
- [ ] New threads start at `vruntime = min_vruntime` of their priority level (no catch-up debt)
- [ ] Variable time slice: run until `vruntime > min_vruntime + target_latency / n_ready_threads`; `target_latency` default 6000 µs, configurable via `HKLM\SYSTEM\Scheduler\TargetLatencyUs`
- [ ] Track `min_vruntime` per priority level; advance only forward (never decrease)
- [ ] Boot log: `[SCHED] CFS vruntime active; target_latency=%u µs`
- [ ] Commit: `"sched: CFS vruntime fair scheduling — prio_to_weight table, min_vruntime"`

## 4. `SCHED_FIFO` / `SCHED_RR` Real-Time Classes `[Opus]`

Two POSIX real-time scheduling classes that always preempt any `SCHED_NORMAL` thread: `SCHED_FIFO` runs until it blocks or yields (no time slice); `SCHED_RR` round-robins with a 10 ms hard slice within the same RT priority level.

**Files:** `src/kernel/sched/sched.c`, `include/kernel/sched/sched.h`

> [!CAUTION]
> A `SCHED_FIFO` thread at RT priority 99 that never yields will starve all other threads including the kernel watchdog. The watchdog must be `SCHED_FIFO` at priority 99, ensuring it cannot itself be starved. Only `CAP_SCHED_RT` threads may enter RT classes.

- [ ] Add `sched_class` enum to `task_t`: `SCHED_NORMAL`, `SCHED_FIFO`, `SCHED_RR`, `SCHED_DEADLINE`
- [ ] Add `rt_priority` (0–99) to `task_t`; ignored for `SCHED_NORMAL`
- [ ] RT run queue: a separate fixed-priority queue checked before all `SCHED_NORMAL` queues; `SCHED_FIFO` threads at the same RT priority round-robin only on yield/block
- [ ] `SCHED_FIFO`: no slice expiry; preempted only by a higher-RT-priority thread becoming READY
- [ ] `SCHED_RR`: 10 ms hard slice (`SCHED_RR_TIMESLICE_NS = 10_000_000`); on expiry, re-enqueue at tail of same RT-priority level
- [ ] `thread_set_sched(tid, class, rt_prio)` — requires `CAP_SCHED_RT`; returns `STATUS_PRIVILEGE_NOT_HELD` otherwise
- [ ] Kernel watchdog thread: spawn at `SCHED_FIFO`, `rt_priority = 99` before any user thread
- [ ] Commit: `"sched: SCHED_FIFO / SCHED_RR real-time classes, CAP_SCHED_RT enforcement"`

## 5. `SCHED_DEADLINE` EDF Scheduling `[Opus]`

Earliest-Deadline-First scheduling for hard real-time tasks: each thread declares a `(runtime, deadline, period)` triple; the scheduler always dispatches the thread whose deadline is nearest. Admission control at 90% utilisation prevents over-subscription; GRUB bandwidth reclaim returns unused runtime to other deadline threads.

**Files:** `src/kernel/sched/sched.c`, `include/kernel/sched/sched.h`

> [!IMPORTANT]
> EDF admission control: the sum of `runtime/period` over all admitted `SCHED_DEADLINE` threads must not exceed 0.9 (90% CPU utilisation). Reject `thread_set_sched(SCHED_DEADLINE)` with `STATUS_INSUFFICIENT_RESOURCES` if admission would exceed this bound.

- [ ] Add to `task_t` (for `SCHED_DEADLINE` only): `dl_runtime_ns`, `dl_deadline_ns`, `dl_period_ns`, `dl_remaining_ns`, `dl_absolute_deadline`
- [ ] Implement `deadline_admissibility_check()` — sum `runtime/period` over all admitted threads; reject if sum + new entry > 0.9
- [ ] EDF run queue: ordered by `dl_absolute_deadline`; `rq_dequeue_edf()` returns the thread with the earliest deadline
- [ ] On each scheduler tick: decrement `dl_remaining_ns` by tick duration; if `dl_remaining_ns == 0`, suspend thread until next period
- [ ] On period renewal: `dl_absolute_deadline += dl_period_ns`, `dl_remaining_ns = dl_runtime_ns`, re-enqueue
- [ ] GRUB bandwidth reclaim: if the current deadline thread has unused runtime when a lower-priority deadline thread has an earlier absolute deadline, donate remaining bandwidth
- [ ] `thread_set_sched(tid, SCHED_DEADLINE, &sched_attr)` — `sched_attr` contains runtime/deadline/period; admission check runs before activation
- [ ] Commit: `"sched: SCHED_DEADLINE EDF scheduling — admission control, GRUB bandwidth reclaim"`

## 6. CPU Affinity `[Sonnet]`

Pin threads to specific CPUs by setting an `affinity_mask` bitmask on the task; the scheduler only enqueues the thread on a run queue whose CPU bit is set in the mask.

**Files:** `include/kernel/sched/task.h`, `src/kernel/sched/sched.c`, `src/kernel/sched/syscall.c`

- [ ] Add `affinity_mask` (`uint64_t`, bit N = CPU N allowed) to `task_t`; default `0xFFFFFFFFFFFFFFFF` (all CPUs)
- [ ] In `rq_enqueue()`: if `affinity_mask` has exactly one CPU bit set, force enqueue to that CPU's run queue; otherwise balance normally
- [ ] `NtSetInformationThread(handle, ThreadAffinityMask, &mask, sizeof(mask))` — store mask, re-queue thread if needed (→ XREF `02-kernel-core/TODO-05-native-api-ssdt.md`)
- [ ] `NtQueryInformationThread(handle, ThreadAffinityMask, &mask, ...)` — return current mask
- [ ] Win32 `SetThreadAffinityMask(thread, mask)` → `NtSetInformationThread(ThreadAffinityMask)`
- [ ] Commit: `"sched: CPU affinity — affinity_mask in task_t, NtSetInformationThread wiring"`

## 7. Scheduler Stats + `/sys/sched` VFS File `[Sonnet]`

Expose per-thread scheduler metrics as a readable VFS file — class, priority, vruntime, CPU time, voluntary and involuntary context-switch counts — queryable by the `sched` shell command and Task Manager without a dedicated syscall.

**Files:** `src/kernel/fs/sysfs_sched.c` (new), `src/shell/cmd_sched.c` (new)

- [ ] Add to `task_t`: `cpu_time_ns`, `ctx_switches_vol` (yield/block), `ctx_switches_invol` (preemption), `slice_overruns`, `last_cpu`
- [ ] Increment counters at every context switch; distinguish voluntary (thread called `yield()`/`sleep()`) from involuntary (preempted by higher priority or tick expiry)
- [ ] Implement `/sys/sched` VFS read callback: one row per thread — `TID  NAME  CLASS  PRI  VRUNTIME  CPU_MS  VSWTCH  ISWTCH  CPU`
- [ ] `sched` shell command: reads `/sys/sched`, prints formatted table with totals
- [ ] Wire CPU-time column to Task Manager thread view
- [ ] Commit: `"sched: /sys/sched VFS file + sched shell command, per-thread stats"`

## 8. Scheduler Tick Calibration `[Opus]`

Replace the hardcoded LAPIC ICR with a measured RDTSC + HPET calibration that produces accurate nanosecond tick intervals on any hardware. Fall back to the Hyper-V synthetic timer on Gen 2 VMs where HPET is absent.

**Files:** `src/kernel/drivers/lapic.c`, `include/kernel/drivers/lapic.h`, `src/kernel/sched/sched.c`

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-06-interrupt-timer-arch.md §3` — unified timer HAL (`uptime_ns()` and HPET calibration APIs) must be ready before this section; do not duplicate the HPET read path.
> → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §3` — hypervisor detection must be complete so the Hyper-V synthetic timer fallback can be safely branched on.

- [ ] At boot, measure LAPIC timer frequency: set ICR to a known value, read RDTSC before and after `N` PIT/HPET ticks, compute `lapic_hz = (N * hpet_period_ns) / rdtsc_delta`
- [ ] Derive `tick_icr` such that the LAPIC fires every `SCHED_TICK_NS` (default 1 ms = 1 000 000 ns)
- [ ] Store `g_tick_ns` (actual calibrated tick duration in nanoseconds); CFS uses this for `vruntime` accounting
- [ ] On hypervisor detection (`hypervisor_detect()` → Hyper-V): use `MSR_HV_STIMER0_CONFIG` synthetic timer instead of LAPIC ICR; programmatically set next trigger
- [ ] Boot log: `[SCHED] tick calibrated: ICR=%u, tick=%u ns` or `[SCHED] tick: Hyper-V synthetic timer`
- [ ] Commit: `"sched: tick calibration — RDTSC+HPET measurement, Hyper-V synthetic timer fallback"`

## 9. CPU Frequency Scaling Hook `[Opus]`

Define a `cpufreq_governor_t` vtable and wire two built-in governors — `performance` (always max P-state) and `powersave` (scale down on idle) — to the scheduler's per-CPU load measurement. The ACPI P-state transition is deferred to the drivers domain; this section owns the scheduler-side load tracking and governor policy.

**Files:** `include/kernel/sched/cpufreq.h` (new), `src/kernel/sched/cpufreq.c` (new), `src/kernel/sched/sched.c`

> [!IMPORTANT]
> → XREF: `04-drivers-hardware` domain — `cpufreq_set_pstate(cpu, pstate)` low-level implementation (ACPI `_PSS` MSR write) lives in the drivers domain. This section defines the vtable and policy; the driver registers the hardware callback.

- [ ] Define `cpufreq_governor_t`: vtable with `governor_tick(cpu, load_pct)` and `governor_init(cpu)` callbacks
- [ ] Track per-CPU idle fraction: `idle_ticks / total_ticks` over a 100 ms sliding window; expose as `sched_cpu_load_pct(cpu)` (0–100)
- [ ] `performance` governor: always calls `cpufreq_set_pstate(cpu, 0)` (highest P-state); no load check
- [ ] `powersave` governor: on `load_pct > 80` for 200 ms → `cpufreq_set_pstate(cpu, 0)`; on `load_pct < 20` for 500 ms → `cpufreq_set_pstate(cpu, max_pstate)`; hysteresis prevents oscillation
- [ ] `cpufreq_register_driver(cpu, &driver)` — called by the ACPI cpufreq driver to install `cpufreq_set_pstate` callback
- [ ] Active governor selectable via Registry `HKLM\SYSTEM\Scheduler\CpufreqGovernor` (`"performance"` / `"powersave"`)
- [ ] Boot log: `[CPUFREQ] governor: %s; %u P-states available` (or `no cpufreq driver registered` if ACPI not ready)
- [ ] Commit: `"sched: cpufreq governor vtable — performance and powersave, load tracking"`

## 10. Worker Factory Syscalls Wired to SSDT
The Worker Factory is the kernel-side thread pool (backs `TpAllocPool` / `CreateThreadpoolWork`). The scheduler owns thread creation/reaping logic. (→ XREF: TODO-05-native-api-ssdt.md §4)

- [ ] `NtCreateWorkerFactory(FactoryHandle, DesiredAccess, ObjectAttributes, CompletionPortHandle, WorkerProcessHandle, StartRoutine, StartParameter, MaxThreadCount, StackReserve, StackCommit)` → SSDT 0x0180
- [ ] `NtWorkerFactoryWorkerReady(WorkerFactoryHandle)` → SSDT 0x0181: signal that worker thread is idle and ready
- [ ] `NtReleaseWorkerFactoryWorker(WorkerFactoryHandle)` → SSDT 0x0182: release a worker back to pool
- [ ] `NtShutdownWorkerFactory(WorkerFactoryHandle, PendingWorkerCount)` → SSDT 0x0183
- [ ] `NtQueryInformationWorkerFactory(FactoryHandle, InfoClass, Buffer, Length, RetLen)` → SSDT 0x0184
- [ ] `NtSetInformationWorkerFactory(FactoryHandle, InfoClass, Buffer, Length)` → SSDT 0x0185
- [ ] `NtWaitForWorkViaWorkerFactory(FactoryHandle, MiniPacket, ...)` → SSDT 0x0186: block until work item available
- [ ] Commit: `"sched: wire Worker Factory syscalls to SSDT (0x0180–0x0186)"`

**Test checkpoint:** `NtCreateWorkerFactory` tied to I/O completion port creates pool. `NtWaitForWorkViaWorkerFactory` blocks; posting to IOCP wakes a worker. `NtShutdownWorkerFactory` drains all threads.

---

## OS Comparison


| ⭐ | Feature                                       | 🪟 Win11                                                 | 🐧 Linux                                                    | 🚀 Impossible OS                                              |
|----|-----------------------------------------------|-------------------------------------------------------|----------------------------------------------------------|------------------------------------------------------------|
| 💎 | Priority queues — O(1) dequeue                | ✅ 32 priority levels; O(1) per-priority              | ✅ 40 nice levels; O(1) via                              | ⬜ §1 — 40 levels, 64-bit `bsf` bitmap                     |
| 💎 | Starvation prevention / priority aging        | ✅ Priority boost heuristic (UI threads)              | ✅ Dynamic priority decay (`sched_prio_to_weight`)       | ⬜ §2 — `ticks_waiting` EMA aging, Registry-tunable        |
| 💎 | CFS vruntime fair CPU sharing                 | ❌ Fixed-priority only; no vruntime concept           | ✅ CFS — `prio_to_weight`, `min_vruntime`, red-black     | ⬜ §3 — CFS within priority levels +                       |
| 💎 | `SCHED_FIFO` / `SCHED_RR` real-time classes   | ✅ `REALTIME_PRIORITY_CLASS`; no explicit FIFO/RR API | ✅ `SCHED_FIFO` / `SCHED_RR` (POSIX)                     | ⬜ §4 — `SCHED_FIFO` / `SCHED_RR` with `CAP_SCHED_RT`      |
| ⭐ | `SCHED_DEADLINE` EDF + GRUB bandwidth reclaim | ❌ No EDF or deadline scheduling                      | ✅ `SCHED_DEADLINE` (3.14+); CBS + GRUB                  | ⬜ §5 — admission control at 90%, GRUB                     |
| 💎 | CPU affinity                                  | ✅ `SetThreadAffinityMask` Win32 API                  | ✅ `sched_setaffinity(2)` / `pthread_setaffinity_np`     | ⬜ §6 — `affinity_mask` in task, `NtSetInformationThread`  |
| ⭐ | Unified `/sys/sched` all-threads snapshot     | ❌ ETW only; not human-readable without               | ❌ Per-process `/proc/<pid>/sched`; no single-file view  | ⬜ §7 — one file, all threads, `sched`                     |
| 💎 | Scheduler tick calibration                    | ✅ HPET/TSC calibration in HAL at                     | ✅ `calibrate_delay()` + TSC-deadline LAPIC mode         | ⬜ §8 — RDTSC+HPET measurement, Hyper-V synthetic fallback |
| 💎 | CPU frequency scaling governor                | ✅ Windows power plans; `PPM` in                      | ✅ `cpufreq` governors (`ondemand`, `performance`, etc.) | ⬜ §9 — `cpufreq_governor_t` vtable, ACPI P-state hook     |

> **After parity items:** Impossible OS matches Windows and Linux on priority queues, aging, RT classes, affinity, tick calibration, and cpufreq. `SCHED_DEADLINE` adds a hard real-time scheduling class Windows lacks entirely. The unified `/sys/sched` snapshot gives operator-visible scheduler state without ETW tracing infrastructure or per-process `/proc` walking.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Priority: 3 threads at priority 5, 20, 39 — verify priority 5 runs first, priority 39 only runs when others block
- [ ] Aging: CPU-spinner at priority 5 + sleeper at priority 39 — after ~2 s serial log shows `[SCHED] aging boost` for the sleeper
- [ ] CFS: 3 threads at priority 10, 20, 30 — CPU share ratio ≈ 4:2:1 after 5 s (`sched` command shows vruntime spread)
- [ ] `SCHED_FIFO`: RT-priority 50 thread preempts running priority-20 thread mid-slice; confirmed by context-switch counter
- [ ] `SCHED_DEADLINE`: admit two threads at 40% + 40% = 80% utilisation — accepted; add third at 20% — rejected with `STATUS_INSUFFICIENT_RESOURCES`
- [ ] CPU affinity: pin thread to CPU 0; `sched` command shows `last_cpu=0` across 100+ switches
- [ ] `sched` shell command prints table with all boot threads; CPU-time increments with each call
- [ ] Tick calibration: serial log shows `[SCHED] tick calibrated: ICR=N, tick=1000000 ns` (≈1 ms)
- [ ] Commit: `"sched: enhanced scheduler — priority, aging, CFS, RT, EDF, affinity, tick cal, cpufreq"`
