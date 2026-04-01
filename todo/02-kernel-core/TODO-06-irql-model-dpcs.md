# TODO-06 — IRQL Model & DPCs

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
- → XREF: `TODO-01-kernel-init-sequencing.md §3` — DPC init belongs in Phase 1 (§3) after timer/interrupt controller readiness; `dpc_init()` is gated on `SUBSYS_TIMER`.
- → XREF: `01-boot-platform/TODO-03-interrupt-timer-arch.md §5` — `irq_request()` dynamic IRQ API must exist before IRQL levels are mapped to IOAPIC vectors; LAPIC timer (§7 of that TODO) must be calibrated before DPC dispatch at `DISPATCH_LEVEL` is wired.
- → XREF: `01-boot-platform/TODO-03-interrupt-timer-arch.md §7` — LAPIC timer calibration is the prerequisite for the timer/APIC scheduling path for DPC dispatch (§6 of this TODO).
- → XREF: `04-drivers-hardware/INDEX.md` — ISR drivers (NIC/storage/input) must migrate from ad-hoc workqueue usage to DPC top-half/bottom-half contracts.
- → XREF: `TODO-05-native-api-layer.md` — synchronization and wait semantics at `DISPATCH_LEVEL` must align with native API behavior.

## Outcome

- `KIRQL` exists as a first-class kernel type with enforced level transitions.
- Per-CPU IRQL state is tracked and queryable via `KeGetCurrentIrql()`.
- Interrupt entry/exit paths set and restore IRQL correctly for ISR execution.
- DPC objects can be initialized, queued from ISR, and drained on each CPU at `DISPATCH_LEVEL`.
- Workqueue remains available for `PASSIVE_LEVEL` work, but no longer serves as a substitute for DPC semantics.
- Illegal operations at elevated IRQL fail fast with diagnostics rather than deadlocking silently.

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On | Status |
| --- | :---: | -------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | `KIRQL` type, constants, and core contract         | —          |  [x]   |
| 💎  |   2   | Per-CPU IRQL tracking and transition primitives    | §1         |  [x]   |
| 💎  |   3   | Interrupt entry/exit IRQL integration              | §2         |  [x]   |
| 💎  |   4   | DPC object type and per-CPU queue                  | §2         |  [ ]   |
| 💎  |   5   | DPC drain loop at `DISPATCH_LEVEL`                 | §3, §4     |  [ ]   |
| 💎  |   6   | Timer/APIC scheduling path for DPC dispatch        | §5         |  [ ]   |
| 💎  |   7   | Driver migration and workqueue contract split      | §5         |  [ ]   |
| ⭐  |   8   | IRQL violation traps and structured telemetry      | §2, §3, §5 |  [ ]   |
| ⭐  |   9   | Budgeted DPC fairness and starvation watchdog      | §5, §6     |  [ ]   |

> 💎 = parity — core IRQL and DPC behavior expected from Windows NT and mirrored by Linux's hardirq/softirq split.
> ⭐ = exclusive — Impossible OS adds explicit diagnostics and fairness controls as first-class kernel guarantees.

---

## 1. `KIRQL` Type, Constants, and Core Contract

- [x] Create `include/kernel/sched/irql.h` with `typedef uint8_t KIRQL`.
- [x] Define canonical levels: `PASSIVE_LEVEL = 0`, `APC_LEVEL = 1`, `DISPATCH_LEVEL = 2`, `HIGH_LEVEL = 31`.
- [x] Define device IRQL range constants (`DIRQL_MIN`, `DIRQL_MAX`) and map IRQ vectors to effective device IRQLs.
- [x] Document API legality per level (allocation, blocking waits, scheduler calls, and lock classes).
- [x] Add `KeGetCurrentIrql()`, `KeRaiseIrql(new_irql, old_irql_out)`, and `KeLowerIrql(old_irql)` declarations.
- [x] Commit: `"kernel: sched — add KIRQL model and IRQL API surface"`

## 2. Per-CPU IRQL Tracking and Transition Primitives

- [x] Add `current_irql` to the per-CPU structure and initialize BSP/AP defaults to `PASSIVE_LEVEL`.
- [x] Implement `KeGetCurrentIrql()` as a per-CPU read with no locking.
- [x] Implement `KeRaiseIrql()` with monotonic raise validation and debug assertions for illegal transitions.
- [x] Implement `KeLowerIrql()` with strict restore checks (`old_irql <= current_irql`) and instrumentation.
- [x] Ensure spinlock paths that currently `cli/sti` are aligned to IRQL semantics (`DISPATCH_LEVEL` or higher where required).
- [x] Commit: `"kernel: sched — track current IRQL per CPU and enforce transitions"`

## 3. Interrupt Entry/Exit IRQL Integration

- [x] On interrupt/trap entry from thread context, raise to the mapped DIRQL before ISR body execution.
- [x] Preserve prior IRQL in the interrupt frame/context and restore it on exit.
- [x] Ensure nested interrupts preserve highest-active IRQL correctly and unwind in strict LIFO order.
- [x] Keep end-of-interrupt signaling (LAPIC/PIC) ordered correctly relative to IRQL lowering.
- [x] Add debug-only assertions that ISR code paths do not attempt blocking operations at DIRQL.
- [x] Commit: `"kernel: irq — wire IRQL raises/lowers into interrupt path"`

> **Note:** IRQL tracking in `isr_handler` is software-only — no LAPIC TPR writes on interrupt entry/exit. The LAPIC hardware already masks lower-priority vectors via the ISR/PPR mechanism during interrupt delivery. Explicit TPR writes are reserved for `KeRaiseIrql`/`KeLowerIrql` when kernel code intentionally changes level. This avoids interference with emulated LAPIC on WHPX/VBox/TCG.

## 4. DPC Object Type and Per-CPU Queue

- [ ] Create `include/kernel/sched/dpc.h` and `src/kernel/sched/dpc.c`.
- [ ] Define `KDPC` with routine pointer, deferred context, optional argument pair, and queue link.
- [ ] Implement `KeInitializeDpc()`, `KeInsertQueueDpc()`, and `KeRemoveQueueDpc()`.
- [ ] Build a lock-protected per-CPU DPC queue with bounded memory strategy (pre-allocated nodes or static pool fallback).
- [ ] Enforce that `KeInsertQueueDpc()` is callable at ISR IRQL and does not block or allocate unbounded memory.
- [ ] Wire `dpc_init()` into boot path (Phase 1, after timer): per-CPU DPC queue + drain loop; `BOOT_REQUIRE(SUBSYS_TIMER)` — moved from TODO-01 §3
- [ ] Update `task_init()` to return `boot_result_t` instead of `void` — moved from TODO-01 §8
- [ ] Commit: `"kernel: sched — add KDPC type and per-CPU DPC queue"`

## 5. DPC Drain Loop at `DISPATCH_LEVEL`

- [ ] Implement `KiDispatchDpc()` that raises to `DISPATCH_LEVEL`, drains queued DPCs, and restores prior IRQL.
- [ ] Guarantee DPC routines run with interrupts in the correct state for `DISPATCH_LEVEL` semantics.
- [ ] Support bounded batch draining so long DPC bursts do not starve normal scheduling.
- [ ] Track queue depth, executed count, and overrun counters per CPU for diagnostics.
- [ ] Ensure DPC callbacks are forbidden from blocking waits or pageable operations.
- [ ] Commit: `"kernel: sched — add DPC dispatcher at DISPATCH_LEVEL"`

## 6. Timer/APIC Scheduling Path for DPC Dispatch

- [ ] Trigger `KiDispatchDpc()` from the periodic timer/APIC path after ISR critical work and before returning to normal thread execution.
- [ ] Add a pending flag so repeated queue inserts coalesce wakeups and avoid redundant dispatch entry.
- [ ] Validate DPC dispatch on BSP and AP cores in SMP mode.
- [ ] Ensure no recursion/deadlock if a DPC re-queues work for the same CPU.
- [ ] Commit: `"kernel: timer — schedule and coalesce DPC dispatch"`

## 7. Driver Migration and Workqueue Contract Split

- [ ] Define policy: ISR top-half does minimal register/ack work, then queues DPC; thread-level heavy work goes to workqueue.
- [ ] Migrate at least one representative IRQ-heavy driver path (RTL8139 RX/TX or AHCI completion) to DPC-first flow.
- [ ] Update `workqueue.h` comments to clarify it is `PASSIVE_LEVEL` deferred thread work, not a DPC replacement.
- [ ] Add helper wrappers for common ISR pattern: `ack -> queue dpc -> return`.
- [ ] Record driver follow-up checklist under `todo/04-drivers-hardware`.
- [ ] Commit: `"drivers: irq — migrate ISR deferred path to DPC model"`

## 8. IRQL Violation Traps and Structured Telemetry

- [ ] Add `IRQL_REQUIRE_AT_MOST(level)` and `IRQL_REQUIRE_AT_LEAST(level)` macros for fast debug enforcement.
- [ ] Log IRQL contract violations with subsystem, CPU, current level, required level, and callsite symbol.
- [ ] Convert silent misuse patterns (blocking wait at `DISPATCH_LEVEL`, `KeLowerIrql` mismatch) into explicit fault paths.
- [ ] Feed counters into existing kernel logging for boot/runtime health checks.
- [ ] Commit: `"kernel: sched — add IRQL contract diagnostics and telemetry"`

## 9. Budgeted DPC Fairness and Starvation Watchdog

- [ ] Add per-tick DPC budget (count and/or time) with carry-over to avoid monopolizing CPU time.
- [ ] Add watchdog warning when DPC queue depth remains above threshold for N ticks.
- [ ] Add optional priority classes for DPC categories (timer/network/storage) with deterministic ordering.
- [ ] Publish tuning constants in one header for platform-specific calibration.
- [ ] Commit: `"kernel: sched — add DPC budget fairness and watchdog"`

---

## OS Comparison


| ⭐ | Feature                                    | Win11                                       | Linux                                                   | Impossible OS                                          |
|----|--------------------------------------------|---------------------------------------------|---------------------------------------------------------|--------------------------------------------------------|
| 💎 | First-class IRQL/preemption levels         | ✅ `KIRQL` (`PASSIVE`/`DISPATCH`/DIRQL/...) | ✅ preempt/irq contexts (`process`/`softirq`/`hardirq`) | ✅ §1-§3 — Per-CPU IRQL tracked, spinlocks IRQL-aware, |
| 💎 | Deferred interrupt bottom half             | ✅ DPC queue at `DISPATCH_LEVEL`            | ✅ softirq/tasklet/NAPI bottom-half model               | ⬜ §4–§6                                               |
| 💎 | ISR-safe deferred queue API                | ✅ `KeInsertQueueDpc`                       | ✅ IRQ-safe enqueue primitives in net/block             | ⬜ §4                                                  |
| 💎 | Per-CPU deferred work queues               | ✅ Per-CPU DPC state                        | ✅ Per-CPU softirq and work processing                  | ⬜ §4–§6                                               |
| 💎 | Context legality contract                  | ✅ API rules by IRQL                        | ✅ `might_sleep()` and atomic-context rules             | 🔄 §1 — Legality table documented in `irql.h`          |
| 💎 | Workqueue for thread-context deferred work | ✅ Work items at passive level              | ✅ kernel workqueues at process context                 | ⚠️ §7 — Partial — exists; needs explicit               |
| ⭐ | Built-in IRQL violation telemetry          | ⚠️ Mostly internal/checked builds           | ⚠️ Debug warnings exist but fragmented                  | ⬜ §8 — unified contract diagnostics                   |
| ⭐ | DPC fairness budget with watchdog policy   | ⚠️ Internal heuristics                      | ⚠️ Subsystem-specific tuning                            | ⬜ §9 — explicit and configurable policy               |

> **After §1–§7:** Impossible OS reaches parity on interrupt-level execution guarantees and deferred work architecture required for production drivers.
> **§8–§9** turn correctness and fairness into explicit kernel contracts instead of hidden implementation behavior.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_irql_dpc()` (XREF: `docs/infrastructure/kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_irql_dpc.c` with:
  - `KeGetCurrentIrql()` returns `PASSIVE_LEVEL` when called from normal thread context
  - `KeRaiseIrql(DISPATCH_LEVEL, &old)` sets current IRQL to `DISPATCH_LEVEL`; old is `PASSIVE_LEVEL`
  - `KeLowerIrql(PASSIVE_LEVEL)` restores to `PASSIVE_LEVEL` after raise
  - `KeRaiseIrql` to a level below current IRQL triggers a debug assertion (illegal transition)
  - `KIRQL` constants: `PASSIVE_LEVEL == 0`, `APC_LEVEL == 1`, `DISPATCH_LEVEL == 2`, `HIGH_LEVEL == 31`
  - `KeInitializeDpc` sets routine and context; `KDPC` fields are non-NULL after init
  - `KeInsertQueueDpc` from `DISPATCH_LEVEL` succeeds and enqueues the DPC
  - `KeRemoveQueueDpc` removes a queued DPC before it fires; returns TRUE
  - DPC callback executes at `DISPATCH_LEVEL` (callback checks `KeGetCurrentIrql() == DISPATCH_LEVEL`)
  - DPC callback sets a flag; after `KiDispatchDpc()`, the flag is set (proves execution)
  - Duplicate `KeInsertQueueDpc` for same DPC does not double-enqueue (single execution)
  - Per-CPU queue isolation: DPC queued on CPU 0 does not drain on CPU 1 (SMP test)
  - IRQL violation: attempt blocking wait at `DISPATCH_LEVEL` is trapped (does not deadlock)
- [ ] Register in `test_runner_init()`: `test_register_irql_dpc()`
- [ ] Commit: `"test: add IRQL model and DPC test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot log prints IRQL subsystem init in Phase 1 after timer/interrupt setup.
- [ ] `KeGetCurrentIrql()` reports `PASSIVE_LEVEL` in normal thread context and mapped DIRQL inside ISR.
- [ ] Queuing `KDPC` from timer interrupt executes callback at `DISPATCH_LEVEL` and returns to prior IRQL.
- [ ] Blocking wait attempt from DPC path is trapped and logged as IRQL violation.
- [ ] Workqueue callback still runs in thread context (`PASSIVE_LEVEL`) and may yield safely.
- [ ] SMP check: per-CPU DPC queue drains on each active core without cross-core corruption.
- [ ] Driver smoke test: representative NIC/storage interrupt path uses `ISR -> DPC` split and remains stable under load.
- [ ] Commit: `"kernel: sched — IRQL model and DPC subsystem"`
