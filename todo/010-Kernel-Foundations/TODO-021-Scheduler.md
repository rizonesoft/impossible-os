# P0103 — Kernel Thread Scheduler

> **Goal:** Evolve the current simple round-robin dispatcher into a fair,
> priority-aware, real-time-capable scheduler that prevents CPU-hungry threads
> from starving interactive ones — even on a single core.
>
> [!NOTE]
> The current scheduler (§1 of TODO-020) is a basic round-robin with preemption
> at 100 Hz (PIT). All sections here build on top of that foundation.
>
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc`
> is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB
> heap silently. See `rules.md` Known Gotchas.

> [!IMPORTANT]
> → XREF: `TODO-020-Threading-Synchronization.md §14` — The current scheduler tick
> source is the LAPIC timer with hardcoded ICR=10,000,000. On Hyper-V Gen 2, the
> actual bus frequency may differ, making schedule quanta unpredictable (too fast
> or too slow). When the Hyper-V synthetic timer enhancement (§14 Phase 2 in
> TODO-020) is implemented, the scheduler should prefer `hv_timer_read_ns()` for
> accurate `vruntime` accounting and `target_latency` enforcement.

---

## 1. Priority-Based Scheduling

**Prompt:** The current round-robin scheduler has no notion of priority — every
thread gets an equal time slice. Add 40 static priority levels (0 = highest,
39 = lowest) modelled after Linux `SCHED_OTHER` nice values mapped to 0–39.
The scheduler always picks the highest-priority READY thread. Implement as a
fixed-size array of 40 run queues (one per priority level), so `O(1)` dequeue
at the current highest non-empty level. After completing all items, mark every
item as `[x]`, update this prompt to a verification prompt, run
`bash scripts/build.sh clean`, and commit as
`"sched: priority-based O(1) scheduler"`. Add notes covering run-queue layout
and priority assignment policy.

> **Prerequisite:** §11 Priority Inheritance (TODO-020) must be complete —
> the priority boost mechanism writes directly to the scheduler's effective
> priority field read here.

- [ ] Add `priority` (0–39) and `base_priority` fields to `thread_t`
- [ ] Replace single run-queue with `run_queue_t queues[40]` array
- [ ] Scheduler pick: scan from 0 upward, dequeue first READY thread found — `O(1)` via bitmask
- [ ] Add `thread_set_priority(tid, prio)` syscall — validated range 0–39
- [ ] Default priority: 20 (middle) for user threads, 5 for kernel service threads
- [ ] Idle thread at priority 39 — runs only when all others are blocked
- [ ] Boot log: `[OK] sched: priority scheduler active (40 levels)`
- [ ] Commit: `"sched: priority-based O(1) scheduler"`

---

## 2. Dynamic Priority Aging (Starvation Prevention)

**Prompt:** A low-priority thread can starve forever if high-priority threads
keep running. Aging fixes this: every N scheduler ticks that a READY thread
has not run, its effective priority is bumped by 1 toward 0 (higher). When it
finally runs, its priority resets to its base. This guarantees every thread
eventually runs. Equivalent to Linux's `SCHED_OTHER` dynamic priority decay.
After completing all items, mark every item as `[x]`, update this prompt to a
verification prompt, run `bash scripts/build.sh clean`, and commit as
`"sched: dynamic priority aging"`. Add notes on aging interval tuning.

- [ ] Add `ticks_waiting` counter to `thread_t` (incremented each PIT tick while READY but not running)
- [ ] In scheduler tick ISR: scan READY threads, increment `ticks_waiting`
- [ ] Aging threshold: if `ticks_waiting >= AGING_THRESHOLD` (default: 200 ticks = 2s), boost effective priority by 1
- [ ] Cap boost: effective priority never goes below 0
- [ ] On thread dispatch: reset `ticks_waiting = 0`, restore `effective_priority = base_priority`
- [ ] Configurable via Registry `SYSTEM\Scheduler\AgingThresholdTicks`
- [ ] Test: low-priority thread starved by high-priority spinner — verify it eventually runs
- [ ] Commit: `"sched: dynamic priority aging"`

---

## 3. CFS-Style Fair Scheduling (vruntime)

**Prompt:** The Completely Fair Scheduler (CFS) from Linux is the gold standard
for single-core interactive fairness. Each thread has a `vruntime` counter
(virtual runtime in nanoseconds, normalized by priority weight). The scheduler
always runs the thread with the lowest `vruntime` — ensuring every thread gets
a proportional share of CPU time. On a single core, CFS eliminates jitter
between threads of the same priority and gives high-priority threads a larger
CPU share without completely starving low-priority ones.

The simplified single-core implementation: maintain a sorted structure
(min-heap or red-black tree) of READY threads keyed by `vruntime`.
On preemption, update the running thread's `vruntime +=
elapsed_ns / priority_weight`. Next thread = min-vruntime READY thread.

After completing all items, mark every item as `[x]`, update this prompt to a
verification prompt, run `bash scripts/build.sh clean`, and commit as
`"sched: CFS-style vruntime fair scheduler"`. Add notes covering vruntime
math, weight table, and the latency vs throughput tradeoff.

> [!NOTE]
> CFS and §1 priority queues are **complementary**, not competing. Use CFS
> within a priority level (threads of equal priority share fairly). §1 priority
> still determines which band runs first.

```c
/* Priority weight table — higher priority → larger weight → smaller vruntime increment */
static const uint32_t prio_to_weight[40] = {
    /* prio 0 */  88761, /* prio 1 */ 71755, /* ... */  /* prio 20 */ 1024, /* prio 39 */ 15
};
/* On each tick: running_thread->vruntime += (tick_ns * 1024) / prio_to_weight[prio]; */
```

- [ ] Add `vruntime` (`uint64_t`, nanoseconds) field to `thread_t`
- [ ] Add `priority_weight` (from table above) field to `thread_t`
- [ ] Replace per-priority round-robin list with a min-heap keyed on `vruntime`
- [ ] On context switch out: `vruntime += elapsed_ns * 1024 / weight`
- [ ] Scheduler pick: dequeue minimum-vruntime READY thread (O(log n) heap)
- [ ] New thread starts with `vruntime = min(all_ready_vruntime)` (no catch-up penalty)
- [ ] Timer slice: variable — thread runs until `vruntime` exceeds `min_vruntime + target_latency/n_threads`
- [ ] `target_latency` configurable via Registry `SYSTEM\Scheduler\TargetLatencyUs` (default: 6000 µs)
- [ ] Boot log: `[OK] sched: CFS vruntime scheduler active`
- [ ] Test: 3 threads (prio 10, 20, 30) — verify CPU share ratio is approximately 4:2:1
- [ ] Commit: `"sched: CFS-style vruntime fair scheduler"`

---

## 4. Real-Time Scheduling Classes

**Prompt:** Audio, video decode, and input handling need guaranteed latency —
they must preempt any normal thread the moment they become READY. Implement
two real-time scheduling classes matching POSIX: `SCHED_FIFO` (run until
blocks or yields, no time slice) and `SCHED_RR` (real-time round-robin with
a fixed 1ms slice). RT threads always preempt non-RT threads. Within RT,
priority 0–99 (99 = highest). Equivalent to Linux `SCHED_FIFO` / `SCHED_RR`
and Windows `REALTIME_PRIORITY_CLASS`. After completing all items, mark every
item as `[x]`, update this prompt to a verification prompt, run
`bash scripts/build.sh clean`, and commit as `"sched: real-time scheduling
classes"`. Add notes on RT priority ceiling and starvation risk.

> [!CAUTION]
> A `SCHED_FIFO` thread at priority 99 that never yields will starve ALL other
> threads including the kernel watchdog (§24 of TODO-020). Use RT scheduling
> only for threads with provably bounded run times. The watchdog must be
> `SCHED_FIFO` at a priority higher than any user RT thread.

- [ ] Add `sched_class` enum to `thread_t`: `SCHED_NORMAL`, `SCHED_FIFO`, `SCHED_RR`
- [ ] Add `rt_priority` (0–99) field — only meaningful for RT threads
- [ ] Scheduler: RT threads form a separate run-queue checked before normal queues
- [ ] `SCHED_FIFO`: no time-slice expiry — only preempted by higher-priority RT thread
- [ ] `SCHED_RR`: 1ms hard time slice, then round-robins within same RT priority
- [ ] `thread_set_sched(tid, class, rt_prio)` — requires `CAP_SCHED_RT` capability
- [ ] Kernel watchdog thread: `SCHED_FIFO` priority 99 (highest possible)
- [ ] Test: `SCHED_FIFO` thread preempts `SCHED_NORMAL` thread mid-slice
- [ ] Commit: `"sched: real-time scheduling classes"`

---

## 5. Scheduler Statistics & `/sys/sched`

**Prompt:** Expose per-thread scheduler statistics via the `/sys/sched` VFS
virtual file so the Task Manager and shell can monitor CPU usage, context
switch rates, and scheduling class without a dedicated syscall. Each thread
entry shows: TID, name, scheduling class, priority, vruntime, CPU time used,
voluntary and involuntary context switch counts, and time slice overruns.
Linux exposes `/proc/<pid>/sched`; Windows uses ETW. Impossible OS does it
better: a single `/sys/sched` snapshot readable by any process, no tracing
infrastructure needed. After completing all items, mark every item as `[x]`,
update this prompt to a verification prompt, run `bash scripts/build.sh clean`,
and commit as `"sched: /sys/sched statistics"`. Add notes on the output format.

- [ ] Add per-thread counters to `thread_t`: `cpu_time_ns`, `ctx_switches_voluntary`, `ctx_switches_involuntary`, `slice_overruns`
- [ ] Increment counters at every context switch (distinguish voluntary `yield()` vs preemption)
- [ ] Implement `/sys/sched` VFS virtual file — `read()` returns formatted table
- [ ] Format (one row per thread):
  ```
  TID  NAME           CLASS    PRI  VRUNTIME     CPU_MS  VSWTCH  ISWTCH
  0    main           NORMAL   20   0            1234    42      5
  1    watchdog       FIFO     99   0            2       0       0
  ```
- [ ] Add `sched` shell command — reads and pretty-prints `/sys/sched`
- [ ] Wire CPU usage column to Task Manager thread view
- [ ] Commit: `"sched: /sys/sched statistics"`

---

## 6. CPU Frequency Scaling Hook

**Prompt:** On real hardware, the CPU runs at different clock speeds depending
on load (DVFS — Dynamic Voltage and Frequency Scaling). The scheduler knows
actual CPU load (idle thread fraction) and should hint the ACPI power manager
when to scale frequency up or down. This is the scheduler's side of the CPU
frequency scaling feature from TODO-100 Power Management. After completing all
items, mark every item as `[x]`, update this prompt to a verification prompt,
run `bash scripts/build.sh clean`, and commit as
`"sched: CPU frequency scaling hook"`. Add notes on the load measurement
window and hysteresis.

> **Cross-reference:** Low-level ACPI `_PSS` (Performance Supported States)
> implementation is in `TODO-080-Drivers.md §9`. This section covers the
> scheduler-side load measurement and the upscale/downscale decision.

- [ ] Track idle fraction: `idle_ticks / total_ticks` over a 100ms sliding window
- [ ] Expose `sched_cpu_load_pct()` — returns 0–100 integer load percentage
- [ ] On load > 80% for 200ms: call `cpufreq_request_scale_up()`
- [ ] On load < 20% for 500ms: call `cpufreq_request_scale_down()`
- [ ] Hysteresis: don't oscillate — require 200ms stable before changing direction
- [ ] Configurable thresholds via Registry `SYSTEM\Scheduler\ScaleUpPct` / `ScaleDownPct`
- [ ] Commit: `"sched: CPU frequency scaling hook"`

---

## Priority Order

| Priority | Section                    | Reason                                                               |
|----------|----------------------------|----------------------------------------------------------------------|
| 🔴 P0     | 1. Priority Scheduling     | Foundation — enables §2, §3, §4; fixes UI lag from background work  |
| 🔴 P0     | 2. Priority Aging          | Prevents starvation — required alongside §1                         |
| 🟠 P1     | 3. CFS vruntime            | Fair CPU sharing for interactive responsiveness                     |
| 🟠 P1     | 4. Real-Time Classes       | Required for audio, video, input at guaranteed latency              |
| 🟡 P2     | 5. Scheduler Statistics    | Observability — Task Manager integration                            |
| 🔵 Future | 6. CPU Frequency Scaling   | Depends on ACPI DVFS (TODO-080 §9) and TODO-100 Power Management    |

---

## OS Comparison

| Feature                        | Windows 11 Kernel           | Linux Kernel                | Impossible OS                         |
|--------------------------------|-----------------------------|-----------------------------|---------------------------------------|
| Priority scheduling            | ✅ 32 levels                | ✅ 40 nice levels           | ⬜ §1 P0 — 40 levels, O(1) bitmask    |
| Starvation prevention          | ✅ Priority boost heuristic | ✅ Priority aging           | ⬜ §2 P0 — configurable aging ticks   |
| Fair CPU sharing (CFS)         | ❌ Priority only            | ✅ CFS vruntime             | ⬜ §3 P1 — vruntime + weight table    |
| Real-time scheduling           | ✅ `REALTIME_PRIORITY_CLASS`| ✅ `SCHED_FIFO`/`SCHED_RR`  | ⬜ §4 P1 — `SCHED_FIFO`/`SCHED_RR`    |
| Scheduler statistics           | ✅ ETW + Task Manager       | ✅ `/proc/<pid>/sched`      | ⬜ §5 P2 — `/sys/sched` unified view  |
| CPU frequency scaling          | ✅ Windows power plans      | ✅ `cpufreq` governors      | ⬜ §6 Future                          |
| **Unified /sys/sched view**    | ❌ ETW only, not readable   | ❌ Per-process `/proc` only | ⬜ **§5 — single file, all threads**  |

> **After §1–4:** Impossible OS matches Windows 11 and Linux in scheduler
> sophistication for a single-core system.
> **After §5:** Exceeds both in scheduler observability — one file, all threads,
> no tracing infrastructure needed.
