---
schema_version: 1
id: smp-phase2
domain: 03-memory-concurrency
status: active
title: "TODO-07 -- SMP Phase 2"
---

# TODO-07 -- SMP Phase 2

> **Goal:** Complete the remaining work to make the scheduler and memory system truly SMP-correct on top of the existing Phase 1 AP bringup (LAPIC/IOAPIC, per-CPU data structures). Covers SMP-safe atomics, TLB shootdown IPIs, per-CPU run queues, work-stealing load balancing, adaptive mutex spinning, per-CPU RCU, CPU hotplug, `READ_ONCE`/`WRITE_ONCE` discipline, and CPU feature intersection across APs.

> [!IMPORTANT]
> **SMP Phase 1 prerequisite:** AP bringup, LAPIC initialisation, and per-CPU data (`this_cpu()`) must be complete before any section here is implemented. This TODO assumes `smp.c` already brings up APs and parks them in a spin loop. Do not start §3 (per-CPU run queues) before §1 (atomics audit) and §8 (`READ_ONCE`/`WRITE_ONCE`) are done -- racy per-CPU access without these primitives causes non-deterministic corruption.

## Inputs

- [`src/kernel/smp/smp.c`](../../src/kernel/smp/smp.c)
- [`include/kernel/sched/spinlock.h`](../../include/kernel/sched/spinlock.h)
- [`include/kernel/sched/mutex.h`](../../include/kernel/sched/mutex.h)
- [`include/kernel/sched/rwlock.h`](../../include/kernel/sched/rwlock.h)
- [`src/kernel/sched/sched.c`](../../src/kernel/sched/sched.c)
- [`include/kernel/sched/task.h`](../../include/kernel/sched/task.h)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c)
- → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §1` -- per-priority run queue structure being adapted here to per-CPU run queues in §3
- → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §6` -- CPU affinity mask stored in `task_t`; per-CPU run queues in §3 must honour it
- → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §5` -- per-CPU data pointer (`gs` base, `swapgs`) and AP spin-loop address; §4 and §5 read `this_cpu()` from the structure set up here
- → XREF: `02-kernel-core/TODO-07-irql-model-dpcs.md` -- IPI delivery at `DISPATCH_LEVEL`; TLB shootdown in §2 and RCU IPIs in §7 must not lower IRQL during handler
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §1` -- `vmm_unmap` and `mprotect` call `tlb_shootdown()` from §2 after every PTE change on a shared address space
- → XREF: `02-kernel-core/TODO-11-peb-teb-user-abi.md §15` -- per-thread TEB unmap on `thread_join()` is consumer of §7; until `tlb_shootdown()` lands, §15 defers TEB reclamation to `task_cleanup()` and accepts a bounded leak (16 * 4 KiB per multithreaded process). Implementing §7 here unblocks §15's runtime reclamation path.
- → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md` §10 -- `irq_send_ipi(cpu, vector)` HAL wrapper owned by §2 (item at the TLB-shootdown section); Phase 1 uses `lapic_send_ipi()` until it lands

## Outcome

- Every lock, atomic operation, and memory barrier in `spinlock.c`, `mutex.c`, `rwlock.c`, and all drivers carries a `LOCK`-prefixed instruction or an explicit `smp_mb()` hardware barrier; no silent promotions assumed from the compiler.
- `vmm_unmap()` and `mprotect()` issue a TLB shootdown IPI to all CPUs that have the address space mapped, with an atomic ack counter ensuring completion before returning.
- The scheduler uses one `struct rq` per logical CPU; `schedule()` dequeues only from the local run queue; new tasks land on the least-loaded CPU.
- An idle CPU steals half the ready threads from the busiest run queue, respecting per-thread `affinity_mask`, balancing every 4 ms.
- Mutexes spin briefly on the owner CPU before blocking, eliminating syscall overhead for short critical sections on hot lock paths.
- `synchronize_rcu()` uses per-CPU preempt counters and quiescent-state IPIs instead of disabling the scheduler, allowing RCU readers on all CPUs simultaneously.
- `cpu_up(id)` and `cpu_down(id)` provide a hotplug stub; task migration and AP parking are implemented even if the ACPI hotplug trigger is a future item.
- `READ_ONCE`/`WRITE_ONCE` macros are defined and annotated on all intentionally racy per-CPU accesses, eliminating undefined-behaviour from unguarded shared reads.
- AP CPUID feature sets are intersected against the BSP at bringup; context-switch state save falls back to FXSAVE if any AP lacks AVX, ensuring no AP executes an unsupported `XSAVE` path.

## Implementation Order

| ⭐   | Order | Deliverable                                           | Depends On             | Status |
| --- | :---: | ----------------------------------------------------- | ---------------------- | :----: |
| 💎   |   1   | §8 `READ_ONCE`/`WRITE_ONCE` macros                    | --                     |  [ ]   |
| 💎   |   2   | §1 SMP-safe atomics audit + `smp_mb/rmb/wmb` barriers | §8                     |  [ ]   |
| 💎   |   3   | §9 CPU feature intersection at AP bringup             | SMP Phase 1            |  [ ]   |
| 💎   |   4   | §2 TLB shootdown IPI (`tlb_shootdown`)                | §1, §8                 |  [ ]   |
| 💎   |   5   | §3 Per-CPU run queues                                 | §1, §8, §9, TODO-06 §1 |  [ ]   |
| 💎   |   6   | §5 Adaptive mutex spin-on-owner                       | §1, §3                 |  [ ]   |
| ⭐   |   7   | §6 Per-CPU RCU upgrade                                | §1, §3                 |  [ ]   |
| 💎   |   8   | §4 Work-stealing load balancer                        | §3, §6                 |  [ ]   |
| 💎   |   9   | §7 CPU hotplug stub                                   | §3, §6, §4             |  [ ]   |

> 💎 = parity -- Windows NT and Linux both implement per-CPU run queues, TLB shootdowns, adaptive spinning, RCU, and hotplug; Impossible OS must match.
> ⭐ = exclusive -- the per-CPU RCU design (quiescent-state IPI with `call_rcu` deferred callbacks) goes further than Impossible OS's current scheduler-disable approach, matching Linux's `Tree RCU` sophistication.

---

## 1. SMP-Safe Atomics Audit + `smp_mb` Barriers `[Opus]`

Audit every lock, atomic, and shared-variable access in the codebase for missing `LOCK` prefixes and implicit ordering assumptions that hold only on uniprocessor. Enable `smp_mb()`, `smp_rmb()`, and `smp_wmb()` as hardware `MFENCE`/`LFENCE`/`SFENCE` under `CONFIG_SMP` and as compiler barriers only on UP.

**Files:** `include/kernel/sched/spinlock.h`, `include/kernel/sched/mutex.h`, `include/kernel/sched/rwlock.h`, `src/kernel/smp/smp.c`, all driver files

> [!CAUTION]
> LOCK-prefixed instructions are expensive on real hardware (full bus lock on older microarchitectures). Audit first, add barriers with targeted justification, not blanket insertion. Every `smp_mb()` call must carry a comment explaining which producer–consumer pair it orders.

- [ ] Define `smp_mb()` as `__asm__ volatile("mfence" ::: "memory")` under `CONFIG_SMP`; `__asm__ volatile("" ::: "memory")` on UP
- [ ] Define `smp_rmb()` as `__asm__ volatile("lfence" ::: "memory")` / UP compiler barrier; `smp_wmb()` as `__asm__ volatile("sfence" ::: "memory")` / UP compiler barrier
- [ ] Audit `spinlock_acquire()` / `spinlock_release()` -- verify `LOCK XCHG` on acquire, `smp_mb()` or `MOV+SFENCE` on release
- [ ] Audit `mutex_lock()` / `mutex_unlock()` -- ensure store of `owner = NULL` is not hoisted before critical-section stores
- [ ] Audit `rwlock` -- reader fast-path must use `LOCK XADD`; writer must use `LOCK CMPXCHG` loop
- [ ] Audit all drivers that touch MMIO-mapped shared flags -- add `smp_mb()` where a DMA completion bit is polled
- [ ] Audit `signal_send` `t->state` wake (`src/kernel/ipc/signal.c`): plain BLOCKED/WAITING->READY RMW can resurrect a DEAD task (UAF); CAS-from-BLOCKED/WAITING or task-state lock. Amplified by TODO-21 §17 group fan-out.
- [ ] Shrink IRQ-off time in `src/kernel/ipc/pgroup.c` job-control paths: scan tasks[] lock-free, hold `g_console.lock` only for the O(1) validate+commit (no bounded scan under irqsave). Consumed by TODO-21 §17.
- [ ] Boot log (SMP only): `[SMP] atomics audit: LOCK-prefix verified, smp_mb/rmb/wmb active`
- [ ] Commit: `"smp: SMP-safe atomics audit -- LOCK-prefix, smp_mb/rmb/wmb barriers"`

## 2. TLB Shootdown IPI `[Opus]`

After `vmm_unmap()` or `mprotect()` modifies a PTE in a shared address space, all CPUs that have the address space active must flush the affected TLB entries. Send an `IPI_TLB_FLUSH` to the CPU mask, have each receiver run `invlpg` for the range, then acknowledge via an atomic counter; the sender spins until the counter reaches zero.

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`, `src/kernel/smp/smp.c`

> [!IMPORTANT]
> The atomic ack counter must be stack-allocated or a per-CPU slot -- never a global. A global `tlb_ack_counter` races when two CPUs initiate shootdowns simultaneously. Use a `tlb_shootdown_work_t` struct passed to the IPI handler via a per-CPU pending pointer.

- [ ] Define `tlb_shootdown_work_t { uintptr_t vaddr; size_t len; atomic_int pending; }`
- [ ] `tlb_shootdown(cpu_mask, vaddr, len)` -- populate work struct, `atomic_store(&pending, popcount(cpu_mask))`, send `IPI_TLB_FLUSH` to each CPU in mask, spin on `atomic_load(&pending) > 0`
- [ ] IPI handler `ipi_tlb_flush_handler()`: read pending work ptr, loop `invlpg(vaddr)` for range, `atomic_fetch_sub(&pending, 1)`, return
- [ ] Hook `tlb_shootdown()` into `vmm_unmap()`, `vmm_mprotect()`, and the RO path `vmm_set_ro()`/`vmm_protect_range()` (W^X + SSDT write-protect TODO-12 §27) after the PTE store, so RO transitions are coherent across all CPUs, not BSP-local.
- [ ] Also hook `tlb_shootdown()` into `vmm_unmap_user_page()` so live per-process shrink/free is cross-CPU coherent (consumer: `02-kernel-core/TODO-21 §3` brk/sbrk shrink defers here)
- [ ] Also hook `tlb_shootdown()` into `vmm_install_guard_page()`/`vmm_uninstall_guard_page()` -- guards install at RUNTIME, so a stale remote translation can overrun a guard silently (consumer: `02-kernel-core/TODO-21 §19`)
- [ ] Single-CPU fast path: if `cpu_mask` has only the local CPU bit set, just `invlpg(vaddr)` without IPI overhead
- [ ] Boot log: `[SMP] TLB shootdown IPI registered (vector 0xE0)`
- [ ] `smp_call_function(cpu, fn, arg)` cross-CPU synchronous call (same IPI+ack pattern): first consumer is BSP delegation of timer-resolution transitions (`timer_set_tick_hz` refuses AP callers today; `KeSetTimerResolution` rolls back AP-side requests -- `src/kernel/time/timer_resolution.c`, filed from `01-boot-platform/TODO-11` §6 review)
- [ ] `irq_send_ipi(cpu, vector)` arch-neutral wrapper over `lapic_send_ipi()` in `src/kernel/irq.c`; retrofit direct callers so neutral code drops `kernel/drivers/lapic.h` (filed from `01-boot-platform/TODO-11` §10)
- [ ] Commit: `"smp: TLB shootdown IPI -- invlpg range, atomic ack counter, vmm_unmap/mprotect hook"`

## 3. Per-CPU Run Queues `[Opus]`

Replace the single global run queue with one `struct rq` per logical CPU. `schedule()` dequeues only from the local CPU's run queue. New tasks are assigned to the CPU with the fewest ready threads at creation time. All existing priority-queue and CFS structure from `TODO-05` is preserved; the change is in run-queue ownership, not queue internals.

**Files:** `src/kernel/sched/sched.c`, `include/kernel/sched/sched.h`, `include/kernel/sched/task.h`

> [!IMPORTANT]
> → XREF: `TODO-06-scheduler-enhancement.md §1` -- the `run_queue_t` struct (40-level bitmap dequeue) is the per-CPU run queue here. Do not redesign the queue internals; wrap it in `struct rq { run_queue_t q; spinlock_t lock; uint32_t nr_running; uint64_t load_weight; }`.
> Each `struct rq` is protected by its own spinlock, not a global scheduler lock. The idle thread is per-CPU -- each CPU has its own idle task at priority 39.

- [ ] Define `struct rq { run_queue_t q; spinlock_t lock; uint32_t nr_running; uint64_t load_weight; task_t *idle; }` in `sched.h`
- [ ] Allocate `g_rq[MAX_CPUS]`; initialise each during `sched_init_cpu(cpu_id)` called by each AP
- [ ] `schedule()`: acquires `g_rq[this_cpu()].lock`, dequeues from local `rq.q`, releases lock, performs context switch
- [ ] `task_create()`: pick `cpu = argmin(g_rq[cpu].nr_running)` across online CPUs; enqueue into `g_rq[cpu].q`; respect `affinity_mask`
- [ ] Per-CPU idle task: created during `sched_init_cpu(cpu_id)` at priority 39, `SCHED_NORMAL`
- [ ] Per-CPU current-thread cursor: `thread_current()` from `g_rq[this_cpu()]`, not global `current_task`/`current_thread`; closes probe-gating for TODO-12 §12 (`ssdt_previous_mode`), TODO-23 §4 (`in_system_service`) + §16 telemetry attribution
- [ ] Once the cursor above is per-CPU, move the ORDINAL fault-injection arm records (`kmalloc`/`pmm_alloc`/`vmm_map`/`copy_user`
      4-field groups in `per_cpu_data`, `include/kernel/smp.h`) into task-owned storage so an arm survives migration, and make claim/decrement/reload ONE synchronized transaction -- the four allocator gates do plain read-modify-write today, which a sibling thread can interleave under preemption. Blocked until then because a task-owned record reached through a global cursor is not migration-safe. -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §21 (item: "Make the ORDINAL selectors migration-safe")
- [ ] Boot log per AP: `[SCHED] CPU%u: run queue initialised`
- [ ] Commit: `"sched: per-CPU run queues -- struct rq[MAX_CPUS], local dequeue, task placement"`
- [ ] task_cleanup reap barrier: a TASK_DEAD task must be off-CPU on ALL CPUs before its lock-free frees (pml4, per-thread/TEB frames, handle table, unveil, `env_free` -> T22 §1); local-CR3 guard covers only the reaper.
- [ ] thread_join / thread_free_stacks off-CPU barrier: prove a joined thread is off-CPU on ALL CPUs before its kernel stack is freed -- a KI_TRY victim can still run on it after THREAD_DEAD publish -> XREF: 02-kernel-core/TODO-23 §14
- [ ] `thread_reap_kernel_slot` publishes `THREAD_FREE` before `apc_rundown_thread` + field cleanup finish; a lockless `kthread_create` scan can claim the slot mid-reap and corrupt it. Make FREE the final store after teardown. (TODO-12 §12)
- [ ] Tasks-publication lock: serialize `num_tasks++`/slot-publish vs scheduler enumeration + `job_kill_all_members`; reconcile an AP-refused timer-resolution release (-> XREF `02-kernel-core/TODO-21-process-model-extensions.md §14`)

## 4. Work-Stealing Load Balancer `[Opus]`

An idle CPU checks all other run queues and steals roughly half of the ready threads from the busiest one. Stealing respects `affinity_mask` (a thread pinned to CPU 0 is never stolen by CPU 1). The balance loop runs every `SCHED_BALANCE_INTERVAL_MS` milliseconds (default 4 ms) from the idle thread.

**Files:** `src/kernel/sched/sched.c`, `include/kernel/sched/sched.h`

> [!IMPORTANT]
> Work-stealing requires acquiring two run-queue locks simultaneously. Always take the lock with the lower CPU index first to prevent deadlock (`lock_two_rqs(a, b)` wrapper that enforces ordering).

- [ ] `find_busiest_rq()` -- scan `g_rq[cpu].nr_running` for the CPU with the highest load that is not the local CPU; return pointer or `NULL`
- [ ] `steal_work(dst_rq, src_rq)` -- take `lock_two_rqs(dst, src)`, move `src_rq->nr_running / 2` READY tasks respecting `affinity_mask`, update `nr_running` on both sides, release both locks
- [ ] Idle thread body: spin on `need_resched`; every `SCHED_BALANCE_INTERVAL_MS` call `steal_work(local_rq, find_busiest_rq())`
- [ ] `SCHED_BALANCE_INTERVAL_MS` default 4; configurable via Registry `HKLM\SYSTEM\Scheduler\BalanceIntervalMs`
- [ ] Load metric `load_weight` = sum of `priority_weight` of all READY tasks in the queue (not just count)
- [ ] Commit: `"sched: work-stealing load balancer -- lock ordering, affinity-aware steal, 4 ms interval"`

## 5. Adaptive Mutex Spin-on-Owner `[Opus]`

Before a contended mutex blocks the calling thread, spin for up to `MUTEX_SPIN_CYCLES` (default 200) checking whether the lock owner is currently running on another CPU. If the owner is preempted, stop spinning and sleep immediately -- there is no point waiting for a thread that is not making progress.

**Files:** `src/kernel/sched/mutex.c`, `include/kernel/sched/mutex.h`

> [!IMPORTANT]
> `owner_on_cpu` check reads `task_t::current_cpu` and `g_rq[cpu].current_task` without holding the run-queue lock -- this is an intentionally racy observation. Annotate both reads with `READ_ONCE()` (§8) to avoid undefined behaviour from compiler hoisting.

- [ ] Add `task_t *owner` field to `mutex_t` -- set to `current_task` on acquire, `NULL` on release
- [ ] `mutex_spin_on_owner(m)`: loop up to `MUTEX_SPIN_CYCLES`; each iteration checks `READ_ONCE(m->owner)->current_cpu` against online CPU bitmap; if owner is not running (`current_task != owner` on its CPU), break and fall through to blocking path
- [ ] Gate on `CONFIG_SMP`: on UP, skip spinning entirely (no other CPU can make progress)
- [ ] `MUTEX_SPIN_CYCLES` default 200; configurable via Registry `HKLM\SYSTEM\Scheduler\MutexSpinCycles`
- [ ] Boot log (SMP only): `[SCHED] adaptive mutex spin-on-owner active (%u cycles)`
- [ ] Commit: `"sched: adaptive mutex spin-on-owner -- 200-cycle spin, owner-on-cpu check, CONFIG_SMP"`

## 6. Per-CPU RCU Upgrade `[Opus]`

Replace the current `scheduler_disable()` RCU read-side primitive with a per-CPU preempt counter. `synchronize_rcu()` sends a quiescent-state IPI to each online CPU and waits for acknowledgement via a per-CPU flag. `call_rcu(cb)` defers callbacks to a per-CPU list drained after each quiescent state.

**Files:** `src/kernel/sched/rcu.c` (new), `include/kernel/sched/rcu.h` (new), `src/kernel/smp/smp.c`

> [!IMPORTANT]
> The quiescent-state definition: a CPU has passed a quiescent state when its per-CPU preempt counter was 0 at any point since the grace period began. An IPI is used only as a nudge to force a context switch on CPUs that are stuck in a long RCU read-side section; the acknowledgement is set by the next `rcu_read_unlock()` on that CPU.

- [ ] Per-CPU data: `uint32_t rcu_preempt_cnt` (0 = quiescent), `bool rcu_qs_acked`
- [ ] `rcu_read_lock()` / `rcu_read_unlock()`: `rcu_preempt_cnt++` / `rcu_preempt_cnt--`; on decrement to 0, set `rcu_qs_acked = true`
- [ ] `synchronize_rcu()`: increment global `rcu_grace_period`; for each online CPU with `rcu_preempt_cnt > 0`, send `IPI_RCU_QS`; spin until all CPUs have set `rcu_qs_acked`; reset flags
- [ ] `call_rcu(callback, data)`: enqueue `{ callback, data }` on per-CPU `rcu_callbacks` list; drained in `rcu_drain_callbacks()` called from `schedule()` when quiescent
- [ ] `IPI_RCU_QS` handler: if current thread is not in an RCU read-side section (`rcu_preempt_cnt == 0`), set `rcu_qs_acked = true` immediately; otherwise set a `rcu_qs_pending` flag that `rcu_read_unlock()` checks
- [ ] Boot log: `[RCU] per-CPU Tree RCU active; grace-period IPI registered`
- [ ] Commit: `"rcu: per-CPU RCU -- preempt counter, quiescent-state IPI, call_rcu deferred callbacks"`

## 7. CPU Hotplug Stub `[Sonnet]`

Provide `cpu_up(id)` and `cpu_down(id)` kernel functions that manage the online CPU set. `cpu_down` migrates all tasks off the target CPU's run queue and sends an INIT IPI to park the AP in its spin loop. `cpu_up` re-activates a parked AP and re-initialises its run queue.

**Files:** `src/kernel/smp/smp.c`, `include/kernel/smp/smp.h`, `src/kernel/sched/sched.c`

- [ ] Add `cpu_state_t` enum per CPU: `CPU_OFFLINE`, `CPU_ONLINE`, `CPU_GOING_OFFLINE`; stored in `g_cpu_state[MAX_CPUS]`
- [ ] `cpu_down(id)`: set state to `CPU_GOING_OFFLINE`; wait for target CPU to reach scheduler idle; drain its run queue -- `steal_work(local_rq, &g_rq[id])` moving all threads; send INIT IPI to park AP; set state `CPU_OFFLINE`
- [ ] `cpu_up(id)`: re-send SIPI to parked AP; wait for AP to signal `CPU_ONLINE` via atomic store; reinitialise `g_rq[id]`
- [ ] Registry: `HKLM\HARDWARE\CPU\<N>\Enabled` (`REG_DWORD 0` = offline on next boot); hotplug stub reads at runtime but does not require reboot
- [ ] Expose `sys_cpu_hotplug(id, action)` syscall -- requires `CAP_SYS_ADMIN`
- [ ] Boot log: `[SMP] hotplug: cpu_up/cpu_down registered for %u CPUs`
- [ ] Commit: `"smp: CPU hotplug stub -- cpu_up/cpu_down, task migration, INIT IPI park"`

## 8. `READ_ONCE` / `WRITE_ONCE` Macros `[Sonnet]`

Define `READ_ONCE(x)` and `WRITE_ONCE(x, val)` as volatile-cast + compiler barrier macros. Annotate all intentionally racy per-CPU accesses (owner field in §5, `nr_running` sampling in §4, `rcu_preempt_cnt` in §6) to eliminate compiler-induced load/store elimination and reordering.

**Files:** `include/kernel/smp/barriers.h` (new or extend `include/kernel/sched/spinlock.h`)

- [ ] `#define READ_ONCE(x)    (*(const volatile typeof(x) *)&(x))`
- [ ] `#define WRITE_ONCE(x, v) (*(volatile typeof(x) *)&(x) = (v))`
- [ ] Add `barrier()` as `__asm__ volatile("" ::: "memory")` (compiler-only; not a hardware fence)
- [ ] Annotate `mutex_t::owner` read in `mutex_spin_on_owner()` with `READ_ONCE()`
- [ ] Annotate `rq::nr_running` sampling in `find_busiest_rq()` with `READ_ONCE()`
- [ ] Annotate `rcu_preempt_cnt` read in `synchronize_rcu()` polling with `READ_ONCE()`
- [ ] Commit: `"smp: READ_ONCE/WRITE_ONCE macros -- volatile-cast, barrier(), racy-access annotations"`

## 9. CPU Feature Intersection at AP Bringup `[Opus]`

During SMP bringup each AP executes CPUID and reports its feature flags to the BSP. The BSP computes the intersection; the result becomes the system-wide `cpu_features` mask used for context-switch state save selection (`XSAVE` vs `FXSAVE`). Any AP that fails the mandatory feature check (SSE2, NX) is left offline.

**Files:** `src/kernel/smp/smp.c`, `include/kernel/smp/smp.h`, `src/kernel/sched/sched.c`

> [!IMPORTANT]
> This must run during AP bringup (before the AP enters the scheduler for the first time) so that the feature mask is stable before the first `XSAVE`-capable thread is scheduled. Do not query features lazily per context switch.

- [ ] Per-CPU TSS + guarded IST stacks with `ltr` on every AP (today only BSP `gdt_init()` loads TR; an AP NMI/#DF/MCE on the IST gates triple-faults); consumer `01-boot-platform/TODO-11` §4 IDT IST assignments
- [ ] Each AP: execute CPUID leaf 1 (ECX/EDX) and leaf 7 (EBX/ECX) during bringup trampoline; store in `per_cpu_cpuid[AP_id]`
- [ ] BSP: after all APs check in, compute `g_cpu_feature_intersection = AND(per_cpu_cpuid[0..n])`; publish to `cpu_features` global
- [ ] If any AP lacks SSE2 or NX: `klog(LOG_WARN, "SMP", "CPU%u missing mandatory feature SSE2/NX -- left offline")`; set `g_cpu_state[id] = CPU_OFFLINE`
- [ ] `sched_context_save_mode()`: if `cpu_features & CPU_AVX` → `XSAVE`; else → `FXSAVE`; set once at boot, constant thereafter
- [ ] `cpu_features_report()`: log intersection flags at boot: `[SMP] feature intersection: SSE2 NX AVX2 (mask=0x...)`
- [ ] Commit: `"smp: CPU feature intersection -- CPUID AND across APs, XSAVE/FXSAVE context switch selection"`

---

## OS Comparison


| ⭐   | Feature                                           | 🪟 Win11                                               | 🐧 Linux                                                | 🚀 Impossible OS                                             |
| --- | ------------------------------------------------- | ----------------------------------------------------- | ------------------------------------------------------ | ----------------------------------------------------------- |
| 💎   | LOCK-prefix atomics + hardware memory barriers    | ✅ `InterlockedXxx`; `KeMemoryBarrier()`               | ✅ `atomic_t`; `smp_mb/rmb/wmb`; `LOCK` prefix enforced | ⬜ §1 -- full audit, `smp_mb/rmb/wmb`, CONFIG_SMP gate       |
| 💎   | TLB shootdown IPI                                 | ✅ `KeFlushEntireTb` / per-processor IPI               | ✅ `flush_tlb_mm_range()`; IPI + `invlpg`               | ⬜ §2 -- `tlb_shootdown(mask, vaddr, len)`, atomic ack       |
| 💎   | Per-CPU run queues                                | ✅ Per-processor `KPRCB` dispatch queues               | ✅ Per-CPU `struct rq`; `schedule()` dequeues           | ⬜ §3 -- `struct rq[MAX_CPUS]`, local dequeue, load-weighted |
| 💎   | Work-stealing load balancer                       | ✅ `KiBalanceSetManager` thread; idle processor steals | ✅ `load_balance()`; NEWIDLE trigger; CFS load          | ⬜ §4 -- idle-steal, `load_weight`, affinity-aware, 4 ms     |
| 💎   | Adaptive mutex spin-on-owner                      | ✅ `ExAcquireFastMutex` adaptive spin                  | ✅ `mutex_optimistic_spin()`; owner-on-cpu check        | ⬜ §5 -- 200-cycle spin, `READ_ONCE(owner->current_cpu)`     |
| ⭐   | Per-CPU RCU with `call_rcu` deferred callbacks    | ❌ No public RCU; kernel uses                          | ✅ Tree RCU; `synchronize_rcu()`; `call_rcu()`          | ⬜ §6 -- preempt counter + QS IPI                            |
| 💎   | CPU hotplug                                       | ✅ Logical processor online/offline via ACPI           | ✅ `cpu_up()` / `cpu_down()`; task migration;           | ⬜ §7 -- task migration, INIT IPI park,                      |
| 💎   | `READ_ONCE` / `WRITE_ONCE` racy-access discipline | ✅ `ReadNoFence` / `WriteNoFence` WDK macros           | ✅ `READ_ONCE()` / `WRITE_ONCE()` (volatile +           | ⬜ §8 -- volatile-cast macros, annotated on all              |
| 💎   | CPU feature intersection across APs               | ✅ HAL validates AP CPUID at                           | ✅ `cpu_data[]` per-AP; `cpu_has()` uses intersection   | ⬜ §9 -- CPUID AND across APs, `XSAVE`/`FXSAVE`              |

> **After §1–§9:** Impossible OS matches Windows NT and Linux in SMP correctness for all common scheduler, memory, and locking subsystems. The per-CPU RCU implementation (`call_rcu` with deferred callbacks and quiescent-state IPI) is a step beyond a basic `synchronize_rcu()` and places Impossible OS at the level of Linux's Tree RCU -- a read-side primitive Windows lacks an equivalent public API for.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU `-smp 2`: serial log shows `[SMP] feature intersection: ...` and two `[SCHED] CPU%u: run queue initialised` lines
- [ ] TLB shootdown: map a page, `mprotect` it read-only on CPU 0; CPU 1 attempting write triggers `#PF` -- confirms shootdown
- [ ] Per-CPU run queues: create 8 threads; `sched` shell command shows at most 4 per CPU (even distribution)
- [ ] Work-stealing: starve CPU 0 with 8 threads, leave CPU 1 idle; after 4 ms `sched` shows balanced `nr_running`
- [ ] Adaptive spin: contended mutex with short critical section; `sched` shows near-zero involuntary context switches for that thread pair
- [ ] `call_rcu`: deferred callback fires after all readers exit; verified via serial log `[RCU] callback drained`
- [ ] Commit: `"smp: SMP Phase 2 -- atomics, TLB shootdown, per-CPU rq, stealing, RCU, hotplug"`
