# TODO-06 -- IRQL Model & DPCs

> **Goal:** Implement a Windows-style Interrupt Request Level (IRQL) model and a real Deferred Procedure Call (DPC) subsystem so interrupt handlers can defer non-trivial work safely. DPCs run at `DISPATCH_LEVEL`, enforce preemption constraints, and provide a deterministic bridge between hard-interrupt context and thread context.

> [!IMPORTANT]
> **Current state:** The kernel has `workqueue_enqueue()` with IRQ-safe enqueue semantics, but there is no explicit IRQL contract (`PASSIVE_LEVEL`, `DISPATCH_LEVEL`, device IRQL) and no DPC queue drained at `DISPATCH_LEVEL`. Driver code therefore cannot reason about which APIs are legal in interrupt context, and deferred work policy depends on ad-hoc conventions instead of an enforced kernel model.

## Inputs

- [`include/kernel/sched/workqueue.h`](../../include/kernel/sched/workqueue.h)
- [`src/kernel/sched/workqueue.c`](../../src/kernel/sched/workqueue.c)
- [`include/kernel/idt.h`](../../include/kernel/idt.h)
- [`src/kernel/main/boot_interrupts.c`](../../src/kernel/main/boot_interrupts.c)
- [`src/kernel/drivers/pit.c`](../../src/kernel/drivers/pit.c)
- [`src/kernel/drivers/lapic.c`](../../src/kernel/drivers/lapic.c)
- [`src/kernel/sched/spinlock.c`](../../src/kernel/sched/spinlock.c)
- → XREF: `TODO-01-kernel-init-sequencing.md §3` -- DPC init belongs in Phase 1 (§3) after timer/interrupt controller readiness; `dpc_init()` is gated on `SUBSYS_TIMER`.
- → XREF: `01-boot-platform/TODO-06-interrupt-timer-arch.md §5` -- `irq_request()` dynamic IRQ API must exist before IRQL levels are mapped to IOAPIC vectors; LAPIC timer (§7 of that TODO) must be calibrated before DPC dispatch at `DISPATCH_LEVEL` is wired.
- → XREF: `01-boot-platform/TODO-06-interrupt-timer-arch.md §7` -- LAPIC timer calibration is the prerequisite for the timer/APIC scheduling path for DPC dispatch (§6 of this TODO).
- → XREF: `04-drivers-hardware/INDEX.md` -- ISR drivers (NIC/storage/input) must migrate from ad-hoc workqueue usage to DPC top-half/bottom-half contracts.
- → XREF: `TODO-05-native-api-ssdt.md §8` -- synchronization and wait semantics at `DISPATCH_LEVEL` must align with native API behavior; NtQueueApcThread (SSDT 0x0043) and NtQueueApcThreadEx (SSDT 0x0380) consume §11–§12 APC infrastructure
- → XREF: `TODO-07-time-filetime-management.md §6` -- kernel time service (`KeQuerySystemTime`, `KeQueryTickCount`) provides FILETIME-based due times for KTIMER upgrade in §9
- → XREF: `TODO-10-exception-dispatch-seh.md §5` -- KiUserApcDispatcher ring-3 delivery of user-mode APCs parallels KiUserExceptionDispatcher; §12 sets up the user-mode frame
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §9` -- async I/O completion queues user-mode APC to the issuing thread; depends on §11–§12 APC infrastructure

## Outcome

- `KIRQL` exists as a first-class kernel type with enforced level transitions.
- Per-CPU IRQL state is tracked and queryable via `KeGetCurrentIrql()`.
- Interrupt entry/exit paths set and restore IRQL correctly for ISR execution.
- DPC objects can be initialized, queued from ISR, and drained on each CPU at `DISPATCH_LEVEL`.
- Workqueue remains available for `PASSIVE_LEVEL` work, but no longer serves as a substitute for DPC semantics.
- DPC targeting, importance levels, and `KeFlushQueuedDpcs` give drivers full control over where, when, and how DPCs execute on SMP systems.
- Threaded DPCs run at `PASSIVE_LEVEL` in a dedicated kernel thread, reducing latency for audio/video drivers that cannot tolerate `DISPATCH_LEVEL` preemption delays.
- KTIMER objects carry an optional KDPC; timer expiry auto-queues the DPC -- no manual `KeInsertQueueDpc` in the timer callback.
- `KAPC` objects enable kernel-mode and user-mode asynchronous procedure calls per-thread, providing the foundation for async I/O completion, thread cleanup, and alertable waits.
- `KiDeliverApc` delivers queued APCs at the correct IRQL transition points -- special kernel APCs at `APC_LEVEL`, normal kernel APCs at `PASSIVE_LEVEL`, user APCs on alertable wait completion.
- Illegal operations at elevated IRQL fail fast with diagnostics rather than deadlocking silently.

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On  | Status |
| --- | :---: | -------------------------------------------------- | ----------- | :----: |
| 💎  |   1   | `KIRQL` type, constants, and core contract         | --          |  [x]   |
| 💎  |   2   | Per-CPU IRQL tracking and transition primitives    | §1          |  [x]   |
| 💎  |   3   | Interrupt entry/exit IRQL integration              | §2          |  [x]   |
| 💎  |   4   | DPC object type and per-CPU queue                  | §2          |  [x]   |
| 💎  |   5   | DPC drain loop at `DISPATCH_LEVEL`                 | §3, §4      |  [x]   |
| 💎  |   6   | Timer/APIC scheduling path for DPC dispatch        | §5          |  [x]   |
| 💎  |   7   | DPC targeting, importance, and flush               | §4, §5      |  [x]   |
| 💎  |   8   | Threaded DPCs (`PASSIVE_LEVEL` DPC variant)        | §5          |  [x]   |
| 💎  |   9   | Timer-DPC association                              | §4, §6      |  [ ]   |
| 💎  |  10   | Driver migration and workqueue contract split      | §5          |  [ ]   |
| 💎  |  11   | APC object type and per-thread queues              | §1, §2      |  [ ]   |
| 💎  |  12   | APC delivery mechanism (KiDeliverApc)              | §11         |  [ ]   |
| ⭐  |  13   | IRQL violation traps and structured telemetry      | §2, §3, §5  |  [ ]   |
| ⭐  |  14   | Budgeted DPC/APC fairness and starvation watchdog  | §5, §6, §12 |  [ ]   |

> 💎 = parity -- core IRQL, DPC, and APC behavior expected from Windows NT and mirrored by Linux's hardirq/softirq/signal split.
> ⭐ = exclusive -- Impossible OS adds explicit diagnostics and fairness controls as first-class kernel guarantees.

---

## 1. `KIRQL` Type, Constants, and Core Contract

- [x] Create `include/kernel/sched/irql.h` with `typedef uint8_t KIRQL`.
- [x] Define canonical levels: `PASSIVE_LEVEL = 0`, `APC_LEVEL = 1`, `DISPATCH_LEVEL = 2`, `HIGH_LEVEL = 31`.
- [x] Define device IRQL range constants (`DIRQL_MIN`, `DIRQL_MAX`) and map IRQ vectors to effective device IRQLs.
- [x] Document API legality per level (allocation, blocking waits, scheduler calls, and lock classes).
- [x] Add `KeGetCurrentIrql()`, `KeRaiseIrql(new_irql, old_irql_out)`, and `KeLowerIrql(old_irql)` declarations.
- [x] Commit: `"kernel: sched -- add KIRQL model and IRQL API surface"`

## 2. Per-CPU IRQL Tracking and Transition Primitives

- [x] Add `current_irql` to the per-CPU structure and initialize BSP/AP defaults to `PASSIVE_LEVEL`.
- [x] Implement `KeGetCurrentIrql()` as a per-CPU read with no locking.
- [x] Implement `KeRaiseIrql()` with monotonic raise validation and debug assertions for illegal transitions.
- [x] Implement `KeLowerIrql()` with strict restore checks (`old_irql <= current_irql`) and instrumentation.
- [x] Ensure spinlock paths that currently `cli/sti` are aligned to IRQL semantics (`DISPATCH_LEVEL` or higher where required).
- [x] Commit: `"kernel: sched -- track current IRQL per CPU and enforce transitions"`

## 3. Interrupt Entry/Exit IRQL Integration

- [x] On interrupt/trap entry from thread context, raise to the mapped DIRQL before ISR body execution.
- [x] Preserve prior IRQL in the interrupt frame/context and restore it on exit.
- [x] Ensure nested interrupts preserve highest-active IRQL correctly and unwind in strict LIFO order.
- [x] Keep end-of-interrupt signaling (LAPIC/PIC) ordered correctly relative to IRQL lowering.
- [x] Add debug-only assertions that ISR code paths do not attempt blocking operations at DIRQL.
- [x] Commit: `"kernel: irq -- wire IRQL raises/lowers into interrupt path"`

> **Note:** IRQL tracking in `isr_handler` is software-only -- no LAPIC TPR writes on interrupt entry/exit. The LAPIC hardware already masks lower-priority vectors via the ISR/PPR mechanism during interrupt delivery. Explicit TPR writes are reserved for `KeRaiseIrql`/`KeLowerIrql` when kernel code intentionally changes level. This avoids interference with emulated LAPIC on WHPX/VBox/TCG.

## 4. DPC Object Type and Per-CPU Queue

- [x] `dpc.h` + `dpc.c`: `KDPC` struct with routine, context, args, intrusive queue link, queued flag, cpu_target
- [x] `KeInitializeDpc()`, `KeInsertQueueDpc()` (ISR-safe, zero alloc), `KeRemoveQueueDpc()` -- all implemented
- [x] Per-CPU DPC queues (static `cpu_queues[MAX_CPUS]`) with irqsave spinlocks, FIFO, depth warning at 64
- [x] `dpc_init()` wired into `boot_phase3()` before `task_init()`
- [x] `task_init()` updated to return `boot_result_t` (was `void`)
- [x] Commit: `"kernel: sched -- wire DPC init + timer resolution into boot path"`

**Test checkpoint:** `KeInitializeDpc(&dpc, routine, ctx)` sets all fields. `KeInsertQueueDpc` from `DISPATCH_LEVEL` returns 1 (newly queued). Second `KeInsertQueueDpc` for same DPC returns 0 (no-op). `KeRemoveQueueDpc` returns 1 for queued DPC, 0 for un-queued. `dpc_this_cpu_queue()->depth` increments on insert and decrements on remove. Serial: `"dpc: per-CPU DPC queues initialized"` during Phase 1 boot. If crash, check POST -- 0xD400 = never entered `dpc_init`, 0xD401 = completed successfully.

## 5. DPC Drain Loop at `DISPATCH_LEVEL`

- [x] `KiDispatchDpc()`: raises to DISPATCH_LEVEL via `KeRaiseIrql()`, dequeues DPCs under irqsave spinlock, executes callbacks, restores IRQL
- [x] Bounded: `DPC_BATCH_LIMIT = 32` max DPCs per drain to prevent scheduler starvation
- [x] Diagnostics: `q->executed` counter incremented per DPC; `q->depth` tracked live; logs `"dispatched N DPCs on CPU M"`
- [x] DPC callbacks run at DISPATCH_LEVEL -- no blocking/paging (enforced by IRQL, not runtime check)
- [x] Fast skip: returns immediately if queue head is NULL
- [x] Wire into `KeLowerIrql()` and timer ISR: moved to §6 (Timer/Clock interrupt DPC dispatch) -- auto-drain on IRQL drop
- [x] Commit: `"kernel: sched -- add DPC dispatcher at DISPATCH_LEVEL"`

**Test checkpoint:** After `KeInsertQueueDpc` + manual `KiDispatchDpc()`, DPC callback fires. `KeGetCurrentIrql()` inside callback returns `DISPATCH_LEVEL`. Callback sets a flag; flag is set after `KiDispatchDpc()` returns. Queue depth returns to 0 after drain. Executed count increments by 1. Serial: `"dpc: dispatched N DPCs on CPU M"`.

## 6. Timer/APIC Scheduling Path for DPC Dispatch

- [x] `dpc_drain_current_cpu()` wired into LAPIC timer ISR (`lapic_timer_handler`) after EOI, before schedule
- [x] Also wired into PIT timer ISR (`pit_irq_handler`) for TCG compatibility
- [x] Uses lightweight drain (no IRQL management) -- caller is already in ISR context
- [x] Coalescing: `dpc_drain_current_cpu()` returns immediately if queue head is NULL (fast skip)
- [x] No recursion: DPC callbacks can re-queue but the drain loop is bounded by `DPC_BATCH_LIMIT=32`
- [x] Commit: `"kernel: timer -- schedule and coalesce DPC dispatch"`

> [!WARNING]
> **High-risk section.** This wires `KiDispatchDpc` into the LAPIC timer ISR return path. A bug here causes DPC drain on every timer tick -- if the drain crashes, the system triple-faults on the next tick with no recovery. **Rollback:** If DPC dispatch crashes, comment out the `KiDispatchDpc()` call in the timer ISR and fall back to workqueue-only deferred work. Test timer interrupts still work (scheduler tick, compositor frame) before wiring DPC dispatch.

**Test checkpoint:** After wiring, LAPIC timer fires → DPC drains automatically. Queue a DPC, wait one tick, verify callback executed. Re-queued DPC from inside callback does not deadlock. DPC dispatch on BSP and AP cores (SMP). Serial: timer tick rate unchanged after wiring. If crash, check POST -- 0xD600 = entered but DPC dispatch crashed, 0xD601 = dispatch completed. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

## 7. DPC Targeting, Importance, and Flush

Control which CPU a DPC runs on, how urgently it executes, and provide a synchronization barrier for driver teardown.

- [x] `KeSetTargetProcessorDpc(dpc, cpu_number)` -- sets `cpu_target` field
- [x] `KeSetImportanceDpc(dpc, importance)` -- 4 levels: Low/Medium/MediumHigh/High
- [x] `KeInsertQueueDpc` modified: HighImportance -> head-insert; all others -> tail (FIFO)
- [x] `KeFlushQueuedDpcs()` -- drains local queue directly, spin-waits for other CPUs
- [x] `KDPC_IMPORTANCE` enum added; `KDPC.importance` field (default: MediumImportance)
- [x] `KeSetTargetProcessorDpcEx` (>64 CPU): moved to `16-architecture-ports/TODO-03-smp-scaling-processor-groups.md §3`
- [x] Cross-CPU IPI for MediumHighImportance: moved to `16-architecture-ports/TODO-03-smp-scaling-processor-groups.md §3` (part of DpcEx)
- [x] Commit: `"kernel: sched -- add DPC targeting, importance, and flush"`

**Test checkpoint:** `KeSetTargetProcessorDpc` to CPU 1 + `KeInsertQueueDpc` from CPU 0 → DPC callback fires on CPU 1 (check `smp_this_cpu()` in callback). `HighImportance` DPC runs before `LowImportance` DPC queued earlier. `KeFlushQueuedDpcs` returns only after callback completes. Verify on QEMU WHPX SMP (2+ vCPUs), TCG, bare metal -- IPI delivery for cross-CPU DPC targeting differs across platforms.

## 8. Threaded DPCs (`PASSIVE_LEVEL` DPC Variant)

Threaded DPCs run at `PASSIVE_LEVEL` in a dedicated per-CPU kernel thread, allowing operations forbidden at `DISPATCH_LEVEL` (paging, mutex acquisition). Used by audio/video drivers for latency-sensitive work.

- [x] `KeInitializeThreadedDpc(dpc, routine, context)` -- sets `dpc->threaded = 1`
- [x] `KDPC.threaded` field added; `drain_queue()` moves threaded DPCs to per-CPU threaded list instead of executing inline
- [x] `dpc_thread_fn()` worker thread: drains threaded list at PASSIVE_LEVEL; yields when idle
- [x] `dpc_start_threads()` creates BSP worker thread after scheduler init; wired into `boot_phase3()`
- [x] Per-AP threaded DPC threads: moved to `16-architecture-ports/TODO-03-smp-scaling-processor-groups.md` -- requires cross-CPU task creation
- [x] Registry toggle `ThreadDpcEnable`: N/A -- always enabled; add toggle only if a real use case requires disabling threaded DPCs
- [x] Commit: `"kernel: sched -- add threaded DPC support at PASSIVE_LEVEL"`

> [!TIP]
> Threaded DPCs are Win11's answer to Linux's threaded IRQs (`request_threaded_irq`). Both solve the same problem: long-running interrupt work that needs to sleep or page. Impossible OS supports both models -- threaded DPC for Win32 driver compat, threaded IRQ semantics via workqueue for Linux compat.

**Test checkpoint:** `KeInitializeThreadedDpc` + `KeInsertQueueDpc` → callback fires with `KeGetCurrentIrql() == PASSIVE_LEVEL`. Threaded DPC can acquire a mutex without deadlock. Regular DPC queued during threaded DPC execution preempts it. Serial: `"dpc: threaded DPC thread started on CPU N"` during init.

## 9. Timer-DPC Association

Bridge between kernel timer objects and the DPC subsystem. When a timer fires, its associated DPC is automatically queued -- no manual `KeInsertQueueDpc` in the timer callback.

> [!NOTE]
> Minimal prerequisite -- full KTIMER upgrade deferred until TODO-07 §6 (kernel time service) lands. This section defines a lightweight `kernel_timer_t` struct with a `KDPC *dpc` field, a linked-list timer wheel checked on each LAPIC timer tick, and `KeSetTimerEx`/`KeCancelTimer` API. When TODO-07 §6 provides `KeQuerySystemTime` and FILETIME arithmetic, `kernel_timer_t` is upgraded to the full KTIMER type with FILETIME due times, QPC-based expiry, and timer coalescing.

- [ ] Define `kernel_timer_t` struct: `due_time_ticks` (absolute tick count), `period_ticks` (0 = single-shot), `KDPC *dpc` (optional), `active` flag, linked-list next pointer
- [ ] Add per-CPU timer list; check expired timers in the LAPIC timer ISR after DPC dispatch (§6)
- [ ] Implement `KeSetTimerEx(kernel_timer_t *Timer, uint64_t DueTimeTicks, uint64_t PeriodTicks, KDPC *Dpc)` -- inserts timer into per-CPU list; on expiry, calls `KeInsertQueueDpc(timer->dpc, timer, NULL)` if dpc is non-NULL
- [ ] Implement `KeSetTimer(kernel_timer_t *Timer, uint64_t DueTimeTicks, KDPC *Dpc)` as single-shot convenience wrapper (`PeriodTicks = 0`)
- [ ] Periodic timers re-queue their DPC on each period expiry until cancelled
- [ ] `KeCancelTimer` removes timer from list, prevents further DPC queueing; does NOT dequeue an already-queued DPC -- caller must call `KeFlushQueuedDpcs` if synchronization is needed
- [ ] Commit: `"kernel: timer -- add timer-DPC association for auto-queued deferred work"`

**Test checkpoint:** `KeSetTimerEx` with 50ms period + DPC → DPC fires 3 times in 150ms window (check counter in callback). `KeCancelTimer` stops further DPC queueing. Timer without DPC still fires normally (NULL dpc field). Verify periodic re-queue doesn't leak DPC nodes.

## 10. Driver Migration and Workqueue Contract Split

- [ ] Define policy: ISR top-half does minimal register/ack work, then queues DPC; thread-level heavy work goes to workqueue.
- [ ] Migrate at least one representative IRQ-heavy driver path (RTL8139 RX/TX or AHCI completion) to DPC-first flow.
- [ ] Update `workqueue.h` comments to clarify it is `PASSIVE_LEVEL` deferred thread work, not a DPC replacement.
- [ ] Add helper wrappers for common ISR pattern: `ack -> queue dpc -> return`.
- [ ] Record driver follow-up checklist under `todo/04-drivers-hardware`.
- [ ] Commit: `"drivers: irq -- migrate ISR deferred path to DPC model"`

**Test checkpoint:** Migrated driver ISR path: interrupt fires → DPC queued → callback processes data at `DISPATCH_LEVEL`. Workqueue callback runs at `PASSIVE_LEVEL` for heavy work. `workqueue.h` comments updated. Driver smoke test under continuous interrupt load (network RX flood or disk I/O burst) remains stable. Serial: no DPC starvation warnings after 60s load test.

## 11. APC Object Type and Per-Thread Queues

Asynchronous Procedure Calls (APCs) are the per-thread deferred work mechanism at `APC_LEVEL`. Kernel-mode APCs handle I/O completion, thread cleanup, and inter-thread injection. User-mode APCs deliver asynchronous callbacks to alertable threads. Without APCs, `NtQueueApcThread`, async I/O completion, and alertable waits cannot function.

- [ ] Create `include/kernel/sched/apc.h` and `src/kernel/sched/apc.c`
- [ ] Define `KAPC` structure: `Type`, `Size`, `Thread` (target `KTHREAD`/`task_t`), `ApcListEntry`, `KernelRoutine` (cleanup callback), `RundownRoutine` (thread-exit cleanup), `NormalRoutine` (the actual APC function), `NormalContext`, `SystemArgument1`, `SystemArgument2`, `ApcStateIndex`, `ApcMode` (KernelMode/UserMode), `Inserted` flag
- [ ] Define `KAPC_STATE` structure embedded in each thread (`task_t`): two list heads (`ApcListHead[KernelMode]`, `ApcListHead[UserMode]`), `KernelApcPending` flag, `UserApcPending` flag, `KernelApcInProgress` flag, `Process` pointer
- [ ] Add `KAPC_STATE ApcState` and `KAPC_STATE SavedApcState` fields to `task_t` -- `SavedApcState` used during `KeAttachProcess`/`KeStackAttachProcess` context switches
- [ ] Implement `KeInitializeApc(Apc, Thread, Environment, KernelRoutine, RundownRoutine, NormalRoutine, ApcMode, NormalContext)` -- initializes all KAPC fields; validates parameters
- [ ] Implement `KeInsertQueueApc(Apc, SystemArgument1, SystemArgument2, Increment)` -- inserts APC into target thread's queue; special kernel APCs at head, normal/user APCs at tail; sets pending flags; returns `TRUE` on success, `FALSE` if thread is exiting
- [ ] Implement `KeRemoveQueueApc(Apc)` -- removes a queued APC before delivery; returns `TRUE` if it was queued
- [ ] Add critical/guarded region support: `KeEnterCriticalRegion()` / `KeLeaveCriticalRegion()` -- blocks normal kernel APC delivery; `KeEnterGuardedRegion()` / `KeLeaveGuardedRegion()` -- blocks all kernel APC delivery
- [ ] Commit: `"kernel: sched -- add KAPC object type and per-thread APC queues"`

**Test checkpoint:** `KeInitializeApc` + `KeInsertQueueApc` to current thread succeeds; `KernelApcPending` flag is set. `KeRemoveQueueApc` returns `TRUE` and clears the flag. `KeInsertQueueApc` to exiting thread returns `FALSE`. `KeEnterCriticalRegion` prevents normal kernel APC delivery. Serial: `"apc: initialized per-thread APC queues"` on first thread init.

## 12. APC Delivery Mechanism (KiDeliverApc)

> [!WARNING]
> **High-risk section.** This wires `KiDeliverApc` into the interrupt/exception return path and `KeLowerIrql`. A bug in the delivery engine fires on every interrupt return and every IRQL transition, causing system-wide crashes. **Rollback:** If APC delivery crashes, remove the `KiDeliverApc` call from the interrupt return path and `KeLowerIrql`; APC queues accumulate but the system runs. Re-enable incrementally: first special kernel APCs only, then normal kernel APCs, then user APCs.

The APC delivery engine runs at defined IRQL transition points -- on return from interrupt/exception, when IRQL drops below `APC_LEVEL`, and when a thread completes an alertable wait. This is the bridge between queued APCs and their execution.

- [ ] Implement `KiDeliverApc(KPROCESSOR_MODE PreviousMode, void *ExceptionFrame, void *TrapFrame)`:
  - Raise IRQL to `APC_LEVEL`
  - **Special kernel APCs**: drain all from head of kernel APC list; execute `KernelRoutine` at `APC_LEVEL`; no thread permission needed
  - **Normal kernel APCs**: if thread is not in critical region and `KernelApcInProgress == FALSE`: set `KernelApcInProgress = TRUE`, lower to `PASSIVE_LEVEL`, call `NormalRoutine`, raise back to `APC_LEVEL`, clear `KernelApcInProgress`
  - **User APCs**: if `PreviousMode == UserMode` and thread is alertable and user APC list is non-empty: set up user-mode trap frame to redirect execution to `KiUserApcDispatcher` (→ XREF TODO-10 §5 for user-mode frame setup)
  - Restore original IRQL
- [ ] Wire `KiDeliverApc` into interrupt/exception return path: call when returning to `PASSIVE_LEVEL` or `APC_LEVEL` and `KernelApcPending` or `UserApcPending` is set. Add `POST16(0xDC00)` before first `KiDeliverApc` call in ISR return and `POST16(0xDC01)` after return
- [ ] Wire into `KeWaitForSingleObject` / `KeWaitForMultipleObjects`: when `Alertable == TRUE` and wait completes or is interrupted, deliver user APCs before returning `STATUS_USER_APC`
- [ ] Wire into `KeLowerIrql`: when lowering from >= `APC_LEVEL` to below `APC_LEVEL`, check for pending kernel APCs and deliver
- [ ] Implement `KeTestAlertThread(AlertMode)` -- tests and delivers pending user APCs without entering a wait
- [ ] Thread exit path: call `RundownRoutine` for all remaining queued APCs to prevent resource leaks
- [ ] Commit: `"kernel: sched -- add KiDeliverApc and APC delivery integration"`

> [!NOTE]
> User-mode APC delivery requires `KiUserApcDispatcher` in the user-mode runtime (ntdll equivalent). The kernel sets up a modified trap frame that redirects ring-3 execution to the dispatcher, which calls the APC routine and then calls `NtContinue` to restore the original context. Full user-mode dispatcher implementation is in TODO-10 §5; this section handles the kernel-side frame setup only.

**Test checkpoint:** Queue kernel APC to current thread; on `KeLowerIrql` to `PASSIVE_LEVEL`, APC fires (callback sets flag). Queue special kernel APC during ISR; APC fires on interrupt return. `KeEnterCriticalRegion` suppresses normal kernel APC delivery; `KeLeaveCriticalRegion` triggers deferred delivery. Thread exit calls `RundownRoutine` for un-delivered APCs. Serial: `"apc: delivered N kernel APCs, M user APCs"`. If crash, check POST -- 0xDC00 = entered `KiDeliverApc` in ISR return, 0xDC01 = completed. Verify on QEMU WHPX, TCG, VirtualBox, bare metal -- ISR return path modification is platform-sensitive.

## 13. IRQL Violation Traps and Structured Telemetry

- [ ] Add `IRQL_REQUIRE_AT_MOST(level)` and `IRQL_REQUIRE_AT_LEAST(level)` macros for fast debug enforcement.
- [ ] Log IRQL contract violations with subsystem, CPU, current level, required level, and callsite symbol.
- [ ] Convert silent misuse patterns (blocking wait at `DISPATCH_LEVEL`, `KeLowerIrql` mismatch) into explicit fault paths.
- [ ] Feed counters into existing kernel logging for boot/runtime health checks.
- [ ] Commit: `"kernel: sched -- add IRQL contract diagnostics and telemetry"`

**Test checkpoint:** `IRQL_REQUIRE_AT_MOST(APC_LEVEL)` at `DISPATCH_LEVEL` triggers diagnostic log: `"irql: violation at <callsite> -- required <= APC_LEVEL, current = DISPATCH_LEVEL"`. Blocking wait at `DISPATCH_LEVEL` faults immediately (no deadlock). `KeLowerIrql` mismatch (lowering to wrong level) triggers assertion. Violation counters visible in kernel log.

## 14. Budgeted DPC/APC Fairness and Starvation Watchdog

- [ ] Add per-tick DPC budget (count and/or time) with carry-over to avoid monopolizing CPU time.
- [ ] Add DPC watchdog: single DPC exceeding 100us threshold triggers `DPC_WATCHDOG_VIOLATION` warning (matching Win11 Bug Check 0x133 semantics); cumulative time at `DISPATCH_LEVEL` exceeding period triggers escalation
- [ ] Add watchdog warning when DPC queue depth remains above threshold for N ticks.
- [ ] Add DPC importance-based ordering in the drain loop (§7 importance levels determine execution order).
- [ ] Add APC starvation watchdog: warn when kernel APC queue depth on any thread exceeds threshold (indicates thread stuck in critical region or elevated IRQL too long).
- [ ] Publish tuning constants in one header for platform-specific calibration.
- [ ] Commit: `"kernel: sched -- add DPC/APC budget fairness and watchdog"`

**Test checkpoint:** DPC callback sleeping for 200us triggers `DPC_WATCHDOG_VIOLATION` warning in serial log (threshold 100us). DPC queue depth > 64 for > 5 ticks triggers depth warning. `HighImportance` DPC runs before `LowImportance` during budget-limited drain. APC starvation watchdog fires when kernel APC queue depth > threshold on test thread. Tuning constants in `include/kernel/sched/dpc_config.h`.

---

## OS Comparison

| ⭐ | Feature                        | Win11                          | Linux                          | Impossible OS                 |
|----|--------------------------------|--------------------------------|--------------------------------|-------------------------------|
| 💎 | IRQL / preemption levels       | ✅ KIRQL (PASSIVE→HIGH)        | ✅ preempt/softirq/hardirq     | ✅ §1–§3 done                 |
| 💎 | DPC bottom-half queue          | ✅ KDPC at DISPATCH_LEVEL      | ✅ softirq/tasklet/NAPI        | ⬜ §4–§6                      |
| 💎 | ISR-safe deferred enqueue      | ✅ KeInsertQueueDpc            | ✅ IRQ-safe enqueue            | ⬜ §4                         |
| 💎 | Per-CPU deferred queues        | ✅ Per-CPU DPC state           | ✅ Per-CPU softirq             | ⬜ §4–§6                      |
| 💎 | DPC targeting (CPU affinity)   | ✅ KeSetTargetProcessorDpc     | ✅ Per-CPU workqueues          | ⬜ §7                         |
| 💎 | DPC importance / priority      | ✅ 4 levels (Low→High)         | ⚠️ Priority workqueues         | ⬜ §7                         |
| 💎 | DPC flush barrier              | ✅ KeFlushQueuedDpcs           | ✅ flush_workqueue             | ⬜ §7                         |
| 💎 | Threaded DPCs (PASSIVE)        | ✅ KeInitializeThreadedDpc     | ✅ request_threaded_irq        | ⬜ §8                         |
| 💎 | Timer-DPC auto-queue           | ✅ KeSetTimerEx + KDPC         | ✅ timer_setup + callback      | ⬜ §9                         |
| 💎 | Context legality contract      | ✅ API rules by IRQL           | ✅ might_sleep() + atomic      | ✅ §1 in irql.h               |
| 💎 | Workqueue (thread deferred)    | ✅ Work items at PASSIVE       | ✅ alloc_workqueue             | ⚠️ §10 -- exists, needs split  |
| 💎 | APC objects (KAPC)             | ✅ KeInitialize/InsertApc      | ⚠️ Signals only                | ⬜ §11                        |
| 💎 | APC delivery engine            | ✅ KiDeliverApc at APC_LEVEL   | ⚠️ do_signal on return         | ⬜ §12                        |
| 💎 | Critical/guarded regions       | ✅ KeEnterCriticalRegion       | ⚠️ preempt_disable             | ⬜ §11                        |
| 💎 | Alertable wait + user APC      | ✅ WaitForSingleObjectEx       | ❌ No equivalent               | ⬜ §12                        |
| ⭐ | IRQL violation telemetry       | ⚠️ Checked builds only         | ⚠️ Fragmented debug warnings   | ⬜ §13 -- unified diagnostics  |
| ⭐ | DPC/APC fairness watchdog      | ⚠️ Internal heuristics         | ⚠️ Subsystem-specific          | ⬜ §14 -- explicit policy      |

> **After §1–§10:** Impossible OS reaches full parity on interrupt-level execution, deferred work architecture, and driver ISR→DPC migration.
> **§11–§12** add the APC subsystem -- the per-thread deferred work mechanism required by async I/O completion, `NtQueueApcThread`, alertable waits, and thread cleanup.
> **§13–§14** turn correctness and fairness into explicit kernel contracts instead of hidden implementation behavior.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_irql_dpc()` (→ XREF: `00-infrastructure/TODO-03-kernel-test-framework.md §1`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_irql_dpc.c` with:
  - **IRQL basics (§1–§3):**
    - `KeGetCurrentIrql()` returns `PASSIVE_LEVEL` when called from normal thread context
    - `KeRaiseIrql(DISPATCH_LEVEL, &old)` sets current IRQL to `DISPATCH_LEVEL`; old is `PASSIVE_LEVEL`
    - `KeLowerIrql(PASSIVE_LEVEL)` restores to `PASSIVE_LEVEL` after raise
    - `KeRaiseIrql` to a level below current IRQL triggers a debug assertion (illegal transition)
    - `KIRQL` constants: `PASSIVE_LEVEL == 0`, `APC_LEVEL == 1`, `DISPATCH_LEVEL == 2`, `HIGH_LEVEL == 31`
  - **DPC core (§4–§6):**
    - `KeInitializeDpc` sets routine and context; `KDPC` fields are non-NULL after init
    - `KeInsertQueueDpc` from `DISPATCH_LEVEL` succeeds and enqueues the DPC
    - `KeRemoveQueueDpc` removes a queued DPC before it fires; returns `TRUE`
    - DPC callback executes at `DISPATCH_LEVEL` (callback checks `KeGetCurrentIrql() == DISPATCH_LEVEL`)
    - DPC callback sets a flag; after `KiDispatchDpc()`, the flag is set (proves execution)
    - Duplicate `KeInsertQueueDpc` for same DPC does not double-enqueue (single execution)
    - Per-CPU queue isolation: DPC queued on CPU 0 does not drain on CPU 1 (SMP test)
    - IRQL violation: attempt blocking wait at `DISPATCH_LEVEL` is trapped (does not deadlock)
  - **DPC targeting/importance (§7):**
    - `KeSetTargetProcessorDpc` to CPU 1 + queue from CPU 0 → callback fires on CPU 1
    - `HighImportance` DPC runs before `LowImportance` DPC queued earlier on same CPU
    - `KeFlushQueuedDpcs` blocks until callback completes; returns only after flag is set
  - **Threaded DPCs (§8):**
    - `KeInitializeThreadedDpc` + `KeInsertQueueDpc` → callback fires at `PASSIVE_LEVEL`
    - Threaded DPC callback can acquire a mutex without deadlock
    - Regular DPC queued during threaded DPC execution preempts it
  - **Timer-DPC (§9):**
    - `KeSetTimerEx` with 50ms period + DPC → DPC fires 3 times in ~150ms (counter == 3)
    - `KeCancelTimer` stops further DPC queueing; counter stops incrementing
    - Timer with NULL DPC still fires normally
  - **APC (§11–§12):**
    - `KeInitializeApc` + `KeInsertQueueApc` to current thread → `KernelApcPending` flag set
    - `KeRemoveQueueApc` returns `TRUE`; `KernelApcPending` cleared
    - Kernel APC callback fires on `KeLowerIrql` to `PASSIVE_LEVEL`; callback sets flag
    - `KeEnterCriticalRegion` blocks normal kernel APC delivery; flag NOT set after lower
    - `KeLeaveCriticalRegion` triggers deferred delivery; flag IS set
    - Special kernel APC fires even inside critical region (not blocked)
    - `KeInsertQueueApc` to exiting thread returns `FALSE`
  - **Driver migration (§10):**
    - After ISR→DPC migration, interrupt fires → DPC queued → callback processes data at `DISPATCH_LEVEL` (flag set)
    - Workqueue callback runs at `PASSIVE_LEVEL` after DPC (second flag set)
    - Driver smoke test: 60s continuous interrupt load with no crash or DPC starvation warning
  - **IRQL violation traps (§13):**
    - `IRQL_REQUIRE_AT_MOST(APC_LEVEL)` at `DISPATCH_LEVEL` triggers diagnostic log (grep for `"irql: violation"`)
    - `IRQL_REQUIRE_AT_LEAST(DISPATCH_LEVEL)` at `PASSIVE_LEVEL` triggers diagnostic log
    - Blocking wait at `DISPATCH_LEVEL` faults immediately; does not deadlock
    - `KeLowerIrql` mismatch (lowering to wrong saved level) triggers assertion
    - Violation counter increments per violation; counter > 0 after test
  - **DPC/APC watchdog (§14):**
    - DPC callback sleeping 200us triggers `DPC_WATCHDOG_VIOLATION` warning in serial log
    - DPC queue depth > 64 for > 5 ticks triggers depth warning in serial log
    - `HighImportance` DPC runs before `LowImportance` during budget-limited drain
    - APC starvation: kernel APC queue depth > threshold triggers warning in serial log
- [ ] Register in `test_runner_init()`: `test_register_irql_dpc()`
- [ ] Commit: `"test: add IRQL model, DPC, and APC test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot log prints IRQL subsystem init in Phase 1 after timer/interrupt setup
- [ ] `KeGetCurrentIrql()` reports `PASSIVE_LEVEL` in normal thread context and mapped DIRQL inside ISR
- [ ] Queuing `KDPC` from timer interrupt executes callback at `DISPATCH_LEVEL` and returns to prior IRQL
- [ ] Blocking wait attempt from DPC path is trapped and logged as IRQL violation
- [ ] Workqueue callback still runs in thread context (`PASSIVE_LEVEL`) and may yield safely
- [ ] SMP check: per-CPU DPC queue drains on each active core without cross-core corruption
- [ ] DPC targeted to CPU 1 from CPU 0 fires on CPU 1 (verify `smp_this_cpu()` in callback)
- [ ] `HighImportance` DPC runs before `LowImportance` DPC queued earlier
- [ ] `KeFlushQueuedDpcs` returns only after all queued DPCs complete
- [ ] Threaded DPC fires at `PASSIVE_LEVEL`; can acquire mutex without deadlock
- [ ] `KeSetTimerEx` with DPC → DPC auto-fires on timer expiry without manual enqueue
- [ ] Kernel APC callback fires on `KeLowerIrql` from `APC_LEVEL` to `PASSIVE_LEVEL`
- [ ] `KeEnterCriticalRegion` blocks normal kernel APC; `KeLeaveCriticalRegion` delivers deferred APCs
- [ ] Special kernel APC fires during ISR return path regardless of critical region
- [ ] Thread exit calls `RundownRoutine` for remaining queued APCs
- [ ] Driver smoke test: representative NIC/storage interrupt path uses `ISR → DPC` split and remains stable under load
- [ ] All 4 platforms: QEMU WHPX, QEMU TCG, VirtualBox, bare metal
- [ ] Commit: `"kernel: sched -- IRQL model, DPC, and APC subsystem"`
