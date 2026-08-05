---
schema_version: 1
id: irql-model-dpcs
domain: 02-kernel-core
status: active
title: "TODO-07 -- IRQL Model & DPCs"
---

# TODO-07 -- IRQL Model & DPCs

> **Validated:** 2026-06-26 | validate-todo-file clean (structure / IO table / XREF / test wiring; integrity gate 8/8)
> **Gap-audited:** 2026-06-26 | parity-research-analyst (Win11 24H2 + Linux 6.12) + codex-gap-audit red-team | added: §11 special-user-APC state + KeAreApcsDisabled, §12 special-user-APC delivery/NtQueueApcThreadEx, §10 KINTERRUPT/KeSynchronizeExecution sync object, §13 IRQL LIFO transition-stack validator, §14 0x133 param-0x0/0x1 split + test | fixed stale T12 APC owner XREF (TODO-17 -> T07) | covered: KINTERRUPT routing + IRQ affinity owned by T11 §5; PREEMPT_RT future (domain-16)

> **Goal:** Implement a Windows-style Interrupt Request Level (IRQL) model and a real Deferred Procedure Call (DPC) subsystem so interrupt handlers can defer non-trivial work safely. DPCs run at `DISPATCH_LEVEL`, enforce preemption constraints, and provide a deterministic bridge between hard-interrupt context and thread context.

> [!IMPORTANT]
> **Current state:** Core IRQL and DPC infrastructure (§1-§8) is implemented and working: `KIRQL` exists, per-CPU IRQL tracking is enforced, interrupt entry/exit raises and lowers IRQL correctly, DPCs queue and drain at `DISPATCH_LEVEL`, CPU targeting and importance are implemented, and threaded DPC baseline support exists. §1 and §2 are verified+stamped (§2 added the redundant-TPR skip); §3 (system-vector CLOCK/IPI levels) carries one open review refinement and is `[/]`. Remaining: §9 timer-DPC association, §10 driver migration/workqueue contract split, §11-§14 APC delivery and diagnostics/fairness, and §15-§17 threaded-DPC correctness hardening from the Codex review findings.

## Inputs

- [`include/kernel/sched/workqueue.h`](../../include/kernel/sched/workqueue.h)
- [`src/kernel/sched/workqueue.c`](../../src/kernel/sched/workqueue.c)
- [`include/kernel/idt.h`](../../include/kernel/idt.h)
- [`src/kernel/main/boot_interrupts.c`](../../src/kernel/main/boot_interrupts.c)
- [`src/kernel/drivers/pit.c`](../../src/kernel/drivers/pit.c)
- [`src/kernel/drivers/lapic.c`](../../src/kernel/drivers/lapic.c)
- [`src/kernel/sched/spinlock.c`](../../src/kernel/sched/spinlock.c)
- → XREF: `TODO-01-kernel-init-sequencing.md §3` -- DPC queue init belongs in Phase 1 (§3) after timer/interrupt controller readiness; `dpc_init_queues()` is gated on `SUBSYS_TIMER`, while `dpc_init()` remains the Phase 3 full-ready hook once the scheduler exists.
- → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §9` -- `irq_request()` dynamic IRQ API must exist before IRQL levels are mapped to IOAPIC vectors; LAPIC timer (§2 of that TODO) must be calibrated before DPC dispatch at `DISPATCH_LEVEL` is wired.
- → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §2` -- LAPIC timer calibration is the prerequisite for the timer/APIC scheduling path for DPC dispatch (§1 of this TODO).
- → XREF: `04-drivers-hardware/INDEX.md` -- ISR drivers (NIC/storage/input) must migrate from ad-hoc workqueue usage to DPC top-half/bottom-half contracts.
- → XREF: `TODO-12-native-api-ssdt.md §7` -- synchronization and wait semantics at `DISPATCH_LEVEL` must align with native API behavior; NtQueueApcThread (SSDT 0x0043) and NtQueueApcThreadEx (SSDT 0x0380) consume §11-§12 APC infrastructure
- → XREF: `TODO-08-time-filetime-management.md §6` -- kernel time service (`KeQuerySystemTime`, `KeQueryTickCount`) provides FILETIME-based due times for KTIMER upgrade in §9
- → XREF: `TODO-23-exception-dispatch-seh.md §5` -- KiUserApcDispatcher ring-3 delivery of user-mode APCs parallels KiUserExceptionDispatcher; §12 sets up the user-mode frame
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §9` -- async I/O completion queues user-mode APC to the issuing thread; depends on §11-§12 APC infrastructure

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

| ⭐  | Order | Deliverable                                            | Depends On  | Status |
| --- | :---: | ------------------------------------------------------ | ----------- | :----: |
| 💎  |   1   | `KIRQL` type, constants, and core contract             | --          |  [x]   |
| 💎  |   2   | Per-CPU IRQL tracking and transition primitives        | §1          |  [x]   |
| 💎  |   3   | Interrupt entry/exit IRQL integration                  | §2          |  [/]   |
| 💎  |   4   | DPC object type and per-CPU queue                      | §2          |  [x]   |
| 💎  |   5   | DPC drain loop at `DISPATCH_LEVEL`                     | §3, §4      |  [x]   |
| 💎  |   6   | Timer/APIC scheduling path for DPC dispatch            | §5          |  [/]   |
| 💎  |   7   | DPC targeting, importance, and flush                   | §4, §5      |  [x]   |
| 💎  |   8   | Threaded DPCs (`PASSIVE_LEVEL` DPC variant)            | §5          |  [x]   |
| 💎  |   9   | Timer-DPC association                                  | §4, §6      |  [/]   |
| 💎  |  10   | Driver migration and workqueue contract split          | §5          |  [/]   |
| 💎  |  11   | APC object type and per-thread queues                  | §1, §2      |  [/]   |
| 💎  |  12   | APC delivery mechanism (KiDeliverApc)                  | §11         |  [/]   |
| ⭐  |  13   | IRQL violation traps and structured telemetry          | §2, §3, §5  |  [/]   |
| ⭐  |  14   | Budgeted DPC/APC fairness and starvation watchdog      | §5, §6, §12 |  [/]   |
| 💎  |  15   | Threaded DPC list synchronization                      | §8          |  [/]   |
| 💎  |  16   | KeFlushQueuedDpcs completion barrier (normal+threaded) | §7, §8, §15 |  [x]   |
| 💎  |  17   | Per-CPU threaded DPC worker affinity                   | §8, §15     |  [/]   |
| ⭐  |  18   | System worker thread pool (long-period periodic)       | §8          |  [/]   |

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

**Test checkpoint:** Build passes with `include/kernel/sched/irql.h` included by the scheduler, spinlock, and interrupt code. `PASSIVE_LEVEL == 0`, `APC_LEVEL == 1`, `DISPATCH_LEVEL == 2`, and `HIGH_LEVEL == 31` in compile-time assertions or unit tests. Serial boot reaches Phase 1 with no IRQL header regressions. Verify on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 19 kernel + 16 user-mode PASS (TCG)
> **Notes:**
> - KIRQL contract header is sound (typedef uint8_t KIRQL; PASSIVE/APC/DISPATCH/DIRQL/CLOCK/IPI/POWER/HIGH levels; `vector_to_irql`; `KeGetCurrentIrql`/`KeRaiseIrql`/`KeLowerIrql`).
> - Review added a `_Static_assert` block pinning the level values + strict-ascending order at compile time (closes the contract's compile-time-assertion requirement).
> - Review fixed header-vs-impl contract drift: `irql.h` + `dpc.h` no longer promise DPC-drain-on-lower or strict-LIFO enforcement the impl does not provide; comments point to the real owners.
> - Canonical contract: `include/kernel/sched/irql.h`.
> **Verified:** 2026-06-26 | commit `e9318e14` | 5/5 items | build OK | tests 19 kernel + 16 user PASS
> **Deferred:** [H] `KeRaiseIrql`/`KeLowerIrql` write the LAPIC TPR even when the byte is unchanged (PASSIVE<->APC both 0x00), taxing the spinlock hot path (RESOLVED 2026-06-26 by §2) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §2 (item: "Skip redundant LAPIC TPR MMIO writes" at line 112)
> **Deferred:** [M] `isr_handler` reports the LAPIC timer at DISPATCH and IPIs at HIGH instead of the named CLOCK_LEVEL/IPI_LEVEL -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §3 (item: "Report system vectors at named IRQLs" at line 136)
> **Deferred:** [M] strict-LIFO raise/lower pairing documented but only the monotonic check is enforced (now counted + strict-trappable in §13; LIFO validator still deferred) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §13 (item: "Per-CPU IRQL transition stack" at line 424)
> **Quality reviewed:** 2026-06-26 | Codex 3x (adversarial, consistency, perf) | 0 fixed in-scope, 2H+2M deferred | scope: kernel-code-quality (re-adversarial skipped: header doc + compile-assert only, no functional change)

---

## 2. Per-CPU IRQL Tracking and Transition Primitives

- [x] Add `current_irql` to the per-CPU structure and initialize BSP/AP defaults to `PASSIVE_LEVEL`.
- [x] Implement `KeGetCurrentIrql()` as a per-CPU read with no locking.
- [x] Implement `KeRaiseIrql()` with monotonic raise validation and debug assertions for illegal transitions.
- [x] Implement `KeLowerIrql()` with strict restore checks (`old_irql <= current_irql`) and instrumentation.
- [x] Ensure spinlock paths that currently `cli/sti` are aligned to IRQL semantics (`DISPATCH_LEVEL` or higher where required).
- [x] Skip redundant LAPIC TPR MMIO writes: `KeRaiseIrql`/`KeLowerIrql` gate `irql_set_tpr` on `irql_to_tpr(new) != irql_to_tpr(prev)` (compute-based, no per-CPU cache; PASSIVE<->APC both 0x00 skip). HIGH_LEVEL cli/sti stays independent. `irql.c`
- [x] Commit: `"kernel: sched -- track current IRQL per CPU and enforce transitions"`

**Test checkpoint:** `KeGetCurrentIrql()` returns `PASSIVE_LEVEL` in normal thread context. `KeRaiseIrql(DISPATCH_LEVEL, &old)` reports `old == PASSIVE_LEVEL`; `KeLowerIrql(old)` restores `PASSIVE_LEVEL`. Illegal lower-on-raise and invalid restore-on-lower trigger diagnostics or assertions instead of silently proceeding. Verify on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 37 kernel + 16 user-mode PASS (TCG)
> **Notes:**
> - Per-CPU IRQL tracking (`current_irql`), `KeGetCurrentIrql`/`KeRaiseIrql`/`KeLowerIrql` with monotonic raise + symmetric-lower validation, and LAPIC TPR programming live in `src/kernel/sched/irql.c`.
> - Review-pass perf fix: `KeRaiseIrql`/`KeLowerIrql` skip the UC LAPIC TPR store when `irql_to_tpr` is unchanged across the transition (PASSIVE<->APC; CLOCK/IPI/POWER/HIGH share 0xFF); `HIGH_LEVEL` cli/sti stays independent.
> - Skip is compute-based (compares mapped TPR values), relying on the invariant that the only TPR writers are this path plus two `lapic.c` init writes (both TPR=0=PASSIVE) -- no per-CPU cache or desync risk.
> - Tests: `test_sched.c` pins `irql_to_tpr` band mapping, the skip precondition (PASSIVE==APC, CLOCK==IPI==HIGH), and `vector_to_irql` boundaries.
> - Canonical: `src/kernel/sched/irql.c`; contract header `include/kernel/sched/irql.h` (§1).
> **Verified:** 2026-06-26 | commit `3c3c4902` | 6/6 items | build OK | tests 37 kernel + 16 user PASS
> **Quality reviewed:** 2026-06-26 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2M+1L fixed | scope: kernel-code-quality

---

## 3. Interrupt Entry/Exit IRQL Integration

- [x] On interrupt/trap entry from thread context, raise to the mapped DIRQL before ISR body execution.
- [x] Preserve prior IRQL in the interrupt frame/context and restore it on exit.
- [x] Ensure nested interrupts preserve highest-active IRQL correctly and unwind in strict LIFO order.
- [x] Keep end-of-interrupt signaling (LAPIC/PIC) ordered correctly relative to IRQL lowering.
- [x] Add debug-only assertions that ISR code paths do not attempt blocking operations at DIRQL.
- [ ] Report system vectors at named IRQLs: LAPIC timer at `CLOCK_LEVEL`, IPI at `IPI_LEVEL` (today `vector_to_irql` reads timer DISPATCH, IPI HIGH). NT model: enter at CLOCK then lower to DISPATCH before §6 DPC drain (Codex §1 M)
- [ ] Expose the current thread's active ring-3 `interrupt_frame` (user RSP/RBP/RIP) from kernel mode so `RtlWalkFrameChain(flags&1)` can walk the user stack -> XREF: `TODO-23 §7` (item: "`RtlWalkFrameChain` `flags & 1` user walk returns 0")
- [x] Commit: `"kernel: irq -- wire IRQL raises/lowers into interrupt path"`

> **Note:** IRQL tracking in `isr_handler` is software-only -- no LAPIC TPR writes on interrupt entry/exit. The LAPIC hardware already masks lower-priority vectors via the ISR/PPR mechanism during interrupt delivery. Explicit TPR writes are reserved for `KeRaiseIrql`/`KeLowerIrql` when kernel code intentionally changes level. This avoids interference with emulated LAPIC on WHPX/VBox/TCG.

**Test checkpoint:** Trigger a timer interrupt and confirm ISR entry raises to the mapped DIRQL while interrupt exit restores the prior thread IRQL. Nested interrupts preserve highest-active IRQL and unwind cleanly with no stuck elevated level after return. Serial boot and timer tick remain stable after the ISR-path change. Verify on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Notes:**
> - Interrupt entry/exit IRQL integration ships in `src/kernel/idt.c` `isr_handler`: hardware vectors (>= 32) raise software `current_irql` on entry and restore the saved prior level on exit; nested interrupts keep the highest-active level.
> - Software-only IRQL tracking in the ISR path (no LAPIC TPR write, per the §3 Note); the LAPIC ISR/PPR masks lower-priority vectors during delivery.
> - One open refinement is deferred (see Deferred): reporting the LAPIC timer at `CLOCK_LEVEL` and IPIs at `IPI_LEVEL` instead of the `vector_to_irql`-derived DISPATCH/HIGH.
> - Canonical: `src/kernel/idt.c` (`isr_handler` IRQL entry/exit).
> **Deferred:** [M] `isr_handler` reports the LAPIC timer at DISPATCH and IPIs at HIGH, not the named `CLOCK_LEVEL`/`IPI_LEVEL` (reason: needs the timer handler to enter CLOCK then lower to DISPATCH before the §6 DPC drain to keep DPCs at DISPATCH; cross-file timer-path change needing bare-metal validation; non-functional-today) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §3 (item: "Report system vectors at named IRQLs" at line 136)

---

## 4. DPC Object Type and Per-CPU Queue

- [x] `dpc.h` + `dpc.c`: `KDPC` struct with routine, context, args, intrusive queue link, queued flag, cpu_target, queued_cpu (the CPU a queued DPC lives on, for cross-CPU remove/re-insert)
- [x] `KeInitializeDpc()`, `KeInsertQueueDpc()` (ISR-safe, zero alloc), `KeRemoveQueueDpc()` -- all implemented
- [x] Per-CPU DPC queues (static `cpu_queues[MAX_CPUS]`) with irqsave spinlocks, FIFO, depth warning at 64
- [x] `dpc_init_queues()` wired into Phase 1 before `sti`; `dpc_init()` remains the Phase 3 full-ready hook before `task_init()`
- [x] `task_init()` updated to return `boot_result_t` (was `void`)
- [x] Commit: `"kernel: sched -- wire DPC init + timer resolution into boot path"`

**Test checkpoint:** `KeInitializeDpc(&dpc, routine, ctx)` sets all fields. `KeInsertQueueDpc` from `DISPATCH_LEVEL` returns 1 (newly queued). Second `KeInsertQueueDpc` for same DPC returns 0 (no-op). `KeRemoveQueueDpc` returns 1 for queued DPC, 0 for un-queued. `dpc_this_cpu_queue()->depth` increments on insert and decrements on remove. Serial: `"dpc: per-CPU DPC queues initialized"` during Phase 1 boot. If crash, check POST -- 0xD400 = never entered `dpc_init`, 0xD401 = completed successfully.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 54 kernel + 16 user-mode PASS (TCG)
> **Notes:**
> - DPC object + per-CPU queue: `KDPC` struct, `KeInitializeDpc`/`KeInsertQueueDpc`/`KeRemoveQueueDpc`, per-CPU `cpu_queues[MAX_CPUS]` FIFO with irqsave locks + depth-64 warn, `dpc_init_queues` Phase-1 -- in `src/kernel/sched/dpc.c`.
> - Review-pass SMP fix: added `queued_cpu` so cross-CPU `KeRemoveQueueDpc` and re-insert operate on the queue the DPC actually lives on; the old code re-resolved `DPC_TARGET_CURRENT` to the caller's CPU and scanned/locked the wrong queue.
> - Review-pass: the depth-warn no longer klogs on the insert path at all (§7 review) -- it arms a per-CPU `warn_pending` flag emitted once per tick by `dpc_watchdog_tick`, since `KeInsertQueueDpc` is callable up to DIRQL; `executed++` moved under the lock.
> - Insert retries on the transient `queued_cpu==MAX_CPUS` (set while a remove/drain clears the DPC) so a concurrent re-insert is never dropped, and never double-links the observable case.
> - Caller precondition (NT contract, documented in `dpc.h`): a single KDPC must not be inserted concurrently from more than one CPU; sequential cross-CPU operations are safe.
> - Canonical: `src/kernel/sched/dpc.c`; tests in `src/kernel/test/test_sched.c`.
> **Verified:** 2026-06-26 | commit `7fefe6ab` | 5/5 items | build OK | tests 54 kernel + 16 user PASS
> **Deferred:** [H] threaded-DPC handoff double-owns a re-inserted KDPC (`drain_queue` clears `queued` + drops the lock before the `threaded_head` prepend) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §15 (item: "Threaded-DPC pending state" at line 351) (RESOLVED 2026-06-26 by §15 commit `23d6b38a`: `drain_queue` hands the DPC to the threaded list under the SAME `DPC_QLOCK` keeping `queued=1`, so the KDPC is never un-owned.)
> **Deferred:** [M] per-CPU DPC queue/lock storage false-shares the ISR-hot insert/drain path -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §7 (item: "Cacheline-align per-CPU DPC storage" at line 207) (RESOLVED 2026-06-26 by §7 commit `57da90ed`: `struct dpc_queue` padded + `aligned(64)`, per-CPU lock in a 64B `dpc_lock_slot`; `_Static_assert`s pin both to one cache line.)
> **Quality reviewed:** 2026-06-26 | Codex 7x (adversarial, consistency, perf, re-adversarial x4) | 4H+1L fixed, 1H+1M deferred | scope: kernel-code-quality

---

## 5. DPC Drain Loop at `DISPATCH_LEVEL`

- [x] `KiDispatchDpc()`: raises to DISPATCH_LEVEL via `KeRaiseIrql()`, dequeues DPCs under irqsave spinlock, executes callbacks, restores IRQL
- [x] Bounded: `DPC_BATCH_LIMIT = 32` max DPCs per drain to prevent scheduler starvation
- [x] Diagnostics: `q->executed` counter incremented per DPC; `q->depth` tracked live; logs `"dispatched N DPCs on CPU M"`
- [x] DPC callbacks run at DISPATCH_LEVEL -- no blocking/paging (enforced by IRQL, not runtime check)
- [x] Fast skip: returns immediately if queue head is NULL
- [x] Wire into `KeLowerIrql()` and timer ISR: moved to §6 (Timer/Clock interrupt DPC dispatch) -- auto-drain on IRQL drop
- [x] Commit: `"kernel: sched -- add DPC dispatcher at DISPATCH_LEVEL"`

**Test checkpoint:** After `KeInsertQueueDpc` + manual `KiDispatchDpc()`, DPC callback fires. `KeGetCurrentIrql()` inside callback returns `DISPATCH_LEVEL`. Callback sets a flag; flag is set after `KiDispatchDpc()` returns. Queue depth returns to 0 after drain. Executed count increments by 1. Serial: `"dpc: dispatched N DPCs on CPU M"`.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 58 kernel + 16 user-mode PASS (TCG)
> **Notes:**
> - DPC drain: `KiDispatchDpc` raises to DISPATCH_LEVEL, `drain_queue` dequeues one batch (`DPC_BATCH_LIMIT`=32) under the per-CPU lock and runs non-threaded callbacks at DISPATCH after unlock; `dpc_drain_current_cpu` is the timer-ISR drain.
> - Review-pass fix: `KeFlushQueuedDpcs` local drain now routes through `KiDispatchDpc` so callbacks run at DISPATCH_LEVEL (was the flush caller's PASSIVE); a bounded single batch avoids spinning forever on a DPC that re-arms itself during the flush.
> - Diagnostics: `q->executed`/`depth` updated under the lock; `KiDispatchDpc` logs "dispatched N DPCs".
> - Tests: `test_sched.c` asserts both the `KiDispatchDpc` drain and the `KeFlushQueuedDpcs`-from-PASSIVE callbacks run at DISPATCH_LEVEL.
> - Canonical: `src/kernel/sched/dpc.c`.
> **Verified:** 2026-06-26 | commit `38b5b9f6` | 6/6 items | build OK | tests 58 kernel + 16 user PASS
> **Deferred:** [H] threaded-DPC `drain_queue` hand-off races the worker on `threaded_head` and re-insert -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §15 (item: "Threaded-DPC pending state" at line 365) (RESOLVED 2026-06-26 by §15 commit `23d6b38a`: the hand-off + worker pop are serialized under `DPC_QLOCK` with `queued=1` held across the two lists.)
> **Quality reviewed:** 2026-06-26 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 1H fixed, 2H deferred | scope: kernel-code-quality

---

## 6. Timer/APIC Scheduling Path for DPC Dispatch

- [x] `dpc_drain_current_cpu()` wired into LAPIC timer ISR (`lapic_timer_handler`) after EOI, before schedule
- [x] Also wired into PIT timer ISR (`pit_irq_handler`) for TCG compatibility
- [x] Uses lightweight drain (no IRQL management) -- caller is already in ISR context
- [x] Coalescing: `dpc_drain_current_cpu()` returns immediately if queue head is NULL (fast skip)
- [x] No recursion: DPC callbacks can re-queue but the drain loop is bounded by `DPC_BATCH_LIMIT=32`
- [x] Commit: `"kernel: timer -- schedule and coalesce DPC dispatch"`
- [ ] DPC runtime budget in the timer ISR drain: `DPC_BATCH_LIMIT=32` caps the COUNT but not per-callback runtime; add a time budget (mono_ns cap per drain) or move arbitrary-callback execution out of hard IRQ context to a DISPATCH-level dispatch point -- filed from `01-boot-platform/TODO-11` §7 perf review (`dpc.c` drain_queue, `lapic.c` lapic_timer_handler)

> [!WARNING]
> **High-risk section.** This wires `KiDispatchDpc` into the LAPIC timer ISR return path. A bug here causes DPC drain on every timer tick -- if the drain crashes, the system triple-faults on the next tick with no recovery. **Rollback:** If DPC dispatch crashes, comment out the `KiDispatchDpc()` call in the timer ISR and fall back to workqueue-only deferred work. Test timer interrupts still work (scheduler tick, compositor frame) before wiring DPC dispatch.

**Test checkpoint:** After wiring, LAPIC timer fires → DPC drains automatically. Queue a DPC, wait one tick, verify callback executed. Re-queued DPC from inside callback does not deadlock. DPC dispatch on BSP and AP cores (SMP). Serial: timer tick rate unchanged after wiring. If crash, check POST -- 0xD600 = entered but DPC dispatch crashed, 0xD601 = dispatch completed. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Notes:**
> - Timer/APIC DPC dispatch: both `lapic_timer_handler` and `pit_irq_handler` call `dpc_drain_current_cpu()` after EOI and before `schedule()`, in lockstep; the lightweight ISR-context drain is bounded by `DPC_BATCH_LIMIT`=32.
> - The drain machinery (`drain_queue`, fast-skip-on-empty, per-CPU locking) was verified in the §5 review; this section is the wiring into the timer ISR return path.
> - One open item is deferred (see Deferred): a per-callback runtime budget for the timer-ISR drain (count is capped, runtime is not).
> - Canonical: `src/kernel/drivers/lapic.c` (`lapic_timer_handler`), `src/kernel/drivers/pit.c` (`pit_irq_handler`).
> **Deferred:** [M] timer-ISR DPC drain caps the COUNT (`DPC_BATCH_LIMIT`=32) but not per-callback runtime -- a single long DPC stalls the tick; needs a mono_ns time budget per drain or moving arbitrary-callback execution out of hard-IRQ (detection relates to the §14 watchdog) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §6 (item: "DPC runtime budget in the timer ISR drain" at line 212)

---

## 7. DPC Targeting, Importance, and Flush

Control which CPU a DPC runs on, how urgently it executes, and provide a synchronization barrier for driver teardown.

- [x] `KeSetTargetProcessorDpc(dpc, cpu_number)` -- sets `cpu_target` field
- [x] `KeSetImportanceDpc(dpc, importance)` -- 4 levels in WDK `wdm.h` numeric order (Low=0, Medium=1, High=2, MediumHigh=3); `_Static_assert` + test pin the ABI; only `HighImportance` head-inserts
- [x] `KeInsertQueueDpc` modified: HighImportance -> head-insert; all others -> tail (FIFO)
- [x] `KeFlushQueuedDpcs()` -- drains local queue directly, spin-waits for other CPUs
- [x] `KDPC_IMPORTANCE` enum added; `KDPC.importance` field (default: MediumImportance)
- [x] `KeSetTargetProcessorDpcEx` (>64 CPU): moved to `16-architecture-ports/TODO-03-smp-scaling-processor-groups.md §5`
- [x] Cross-CPU IPI for MediumHighImportance: moved to `16-architecture-ports/TODO-03-smp-scaling-processor-groups.md §5` (part of DpcEx)
- [x] Cacheline-align per-CPU DPC storage: `struct dpc_queue` padded + `aligned(64)`, per-CPU spinlock wrapped in 64B `dpc_lock_slot` (`DPC_QLOCK` accessor); two `_Static_assert`s pin both to one cache line. `dpc.c`/`dpc.h`
- [x] `KeFlushQueuedDpcs` completion barrier -- shipped in §16: per-CPU `s_wd[cpu].in_flight` (normal) + global `s_in_flight_threaded` (threaded) sampled with the queue heads under `DPC_QLOCK`; silent timeout now fail-closes via bugcheck.
- [x] Commit: `"kernel: sched -- add DPC targeting, importance, and flush"`

**Test checkpoint:** `KeSetTargetProcessorDpc` to CPU 1 + `KeInsertQueueDpc` from CPU 0 → the DPC links onto CPU 1's queue (`queued_cpu == 1`); actual callback execution on an idle AP awaits the deferred DPC IPI / remote drain trigger (today AP queues drain only when that AP lowers IRQL, so a non-BSP target can strand -- see the Remote-target DPC IPI item). `HighImportance` DPC runs before `LowImportance` DPC queued earlier. `KeFlushQueuedDpcs` returns only after callback completes. Verify on QEMU WHPX SMP (2+ vCPUs), TCG, bare metal.

> **Notes:**
> - DPC targeting/importance/flush: `KeSetTargetProcessorDpc` (cpu_target), `KeSetImportanceDpc` (4 levels), `KeInsertQueueDpc` HighImportance head-insert, `KeFlushQueuedDpcs`, and the `KDPC_IMPORTANCE` enum are implemented in `src/kernel/sched/dpc.c`.
> - Review-pass: the per-CPU DPC queue + lock storage is now cacheline-aligned (`struct dpc_queue` padded/aligned, per-CPU lock in a 64B `dpc_lock_slot`) to avoid ISR-hot-path false sharing; `_Static_assert`s pin the layout.
> - The `KeFlushQueuedDpcs` in-flight completion barrier (the cross-CPU "returns only after callback completes" clause) shipped in §16 -- per-CPU `s_wd[].in_flight` (normal) + global `s_in_flight_threaded` (threaded), sampled under `DPC_QLOCK`.
> - Review fixes: importance enum reordered to the WDK ABI; the queue-depth warning moved off the DIRQL insert path to a per-CPU lock-serialized `warn_pending` flag emitted once per tick by `dpc_watchdog_tick`; the insert retry loop bounded (`DPC_INSERT_MAX_SPINS` -> bugcheck, never drops).
> - Canonical: `src/kernel/sched/dpc.c`.
> **Verified:** 2026-06-26 | commit `1deba872` | 9/9 items | build OK | tests 201 kernel + 16 user PASS | lint 0 err
> **Accepted:** [H] remote-AP-targeted normal DPC can strand -- no DPC IPI / drain trigger for an otherwise-idle AP (ktimer pins its DPCs to the BSP to avoid this; doc contract tightened in `dpc.h`) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §14 (item: "Remote-target DPC IPI" at line 452)
> **Quality reviewed:** 2026-06-26 | Codex 7x (adversarial, consistency, perf, re-adversarial x4) | 2H+5M fixed, 1H accepted | scope: kernel-code-quality

---

## 8. Threaded DPCs (`PASSIVE_LEVEL` DPC Variant)

Threaded DPCs run at `PASSIVE_LEVEL` in a kernel worker thread, allowing operations forbidden at `DISPATCH_LEVEL` (paging, mutex acquisition). Used by audio/video drivers for latency-sensitive work. Today a single all-CPU worker runs every threaded DPC on its own scheduled CPU (NO per-CPU affinity -- see the `KDEFERRED_ROUTINE` AFFINITY contract in `dpc.h`); per-CPU affinity workers (NT parity) are deferred to §17 Option A.

- [x] `KeInitializeThreadedDpc(dpc, routine, context)` -- sets `dpc->threaded = 1`
- [x] `KDPC.threaded` field added; `drain_queue()` moves threaded DPCs to per-CPU threaded list instead of executing inline
- [x] `dpc_thread_fn()` worker thread: drains threaded list at PASSIVE_LEVEL; yields when idle
- [x] `dpc_start_threads()` creates BSP worker thread after scheduler init; wired into `boot_phase3()`
- [x] Per-AP threaded DPC threads: moved to `16-architecture-ports/TODO-03-smp-scaling-processor-groups.md` -- requires cross-CPU task creation
- [x] Commit: `"kernel: sched -- add threaded DPC support at PASSIVE_LEVEL"`

> [!TIP]
> Threaded DPCs are Win11's answer to Linux's threaded IRQs (`request_threaded_irq`). Both solve the same problem: long-running interrupt work that needs to sleep or page. Impossible OS supports both models -- threaded DPC for Win32 driver compat, threaded IRQ semantics via workqueue for Linux compat.

**Test checkpoint:** `KeInitializeThreadedDpc` + `KeInsertQueueDpc` → callback fires with `KeGetCurrentIrql() == PASSIVE_LEVEL`. Threaded DPC can acquire a mutex without deadlock. Regular DPC queued during threaded DPC execution preempts it. Serial: `"dpc: threaded DPC thread started on CPU N"` during init.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 62 kernel + 16 user-mode PASS (TCG)
> **Notes:**
> - Threaded DPC baseline: `KeInitializeThreadedDpc` (`threaded=1`), `drain_queue` moves threaded DPCs to `threaded_head[]`, the single all-CPU `dpc_thread_fn` worker drains them at PASSIVE, `dpc_start_threads` wires it into Phase 3.
> - Review-pass: `dpc_start_threads` now checks `task_create`'s return + logs an error on failure (was a silent sink); the `KDEFERRED_ROUTINE` doc now states threaded DPCs run at PASSIVE.
> - Tests: `test_dpc_init_threaded` asserts `KeInitializeThreadedDpc` sets `threaded=1`; the behavioral PASSIVE-execution + handoff stress test is deferred with the §15 sync redesign.
> - Canonical: `src/kernel/sched/dpc.c`.
> **Verified:** 2026-06-26 | commit `bd8f42c2` | 5/5 items | build OK | tests 62 kernel + 16 user PASS
> **Deferred:** [H] `dpc_thread_fn` yield-spins when idle + drains a CPU's threaded list unbounded (self-rearming DPC monopolizes the single worker) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §15 (item: "Worker idle wakeup + drain budget" at line 279)
> **Deferred:** [M] threaded `threaded_head`/`threaded_pending` lost-wakeup (clear-after-drain) + cache-line false sharing -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §15 (item: "Fold pending into the atomic handoff" at line 280)
> **Deferred:** [M] threaded callbacks run on the BSP worker, not the queuing CPU (no per-CPU affinity) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §17 (item: "Option B: Document that threaded DPCs have no CPU affinity guarantee" at line 281)
> **Quality reviewed:** 2026-06-26 | Codex 3x (adversarial, consistency, perf) | 1H+1M fixed, 1H+2M deferred | scope: kernel-code-quality (re-adversarial skipped: task_create return-check + doc + test, no locking/lifecycle change)

---

## 9. Timer-DPC Association

Bridge between kernel timer objects and the DPC subsystem. When a timer fires, its associated DPC is automatically queued -- no manual `KeInsertQueueDpc` in the timer callback.

> [!NOTE]
> Minimal prerequisite -- full KTIMER upgrade deferred until TODO-08 §6 (kernel time service) lands. This section defines a lightweight `kernel_timer_t` struct with a `KDPC *dpc` field, a linked-list timer wheel checked on each LAPIC timer tick, and `KeSetTimerEx`/`KeCancelTimer` API. When TODO-08 §6 provides `KeQuerySystemTime` and FILETIME arithmetic, `kernel_timer_t` is upgraded to the full KTIMER type with FILETIME due times, QPC-based expiry, and timer coalescing.

- [x] `kernel_timer_t` in `include/kernel/sched/ktimer.h`: `due_time_ticks`, `period_ticks` (0=single-shot), `KDPC *dpc`, `cpu` owner, `volatile active`, `next`; `KeInitializeTimer` zeros it.
- [x] Per-CPU timer lists + cacheline-padded irqsave lock slots (mirror `dpc.c`); `ktimer_expire_current_cpu()` runs in the LAPIC + PIT ISR before `dpc_drain_current_cpu` (after `nt_timer_tick`) for same-tick DPC dispatch.
- [x] `KeSetTimerEx(timer, DueTicks, PeriodTicks, Dpc)` in `ktimer.c` -- arms on the BSP service CPU (AP timers masked); on expiry pins the DPC to the service CPU via `KeInsertQueueDpcOnCpu` (no `cpu_target` mutation) so it lands on a draining queue.
- [x] `KeSetTimer(timer, DueTicks, Dpc)` single-shot wrapper (`PeriodTicks = 0`).
- [x] Periodic timers re-arm in-list to the next boundary past `now` in O(1) (overflow-guarded, fires once per pass) and re-queue their DPC each period until cancelled.
- [x] `KeCancelTimer` -- fully locked on the service list, unlinks if armed, returns prior armed state; does NOT await an already-queued DPC (expiry queues the DPC + clears `active` atomically under the lock).
- [ ] Perf-scalability (deferred): replace the O(active-timers) per-tick ISR scan with an ordered expiry structure (timer wheel/min-heap); the empty-list lock-free fast path already covers the no-timer case. Part of the full KTIMER upgrade.
- [x] Commit: `"kernel: timer -- add timer-DPC association for auto-queued deferred work"`

**Test checkpoint:** `KeSetTimerEx` with 50ms period + DPC → DPC fires 3 times in 150ms window (check counter in callback). `KeCancelTimer` stops further DPC queueing. Timer without DPC still fires normally (NULL dpc field). Verify periodic re-queue doesn't leak DPC nodes.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 9 ktimer tests (29 asserts), 0 failures. Unit tests drive expiry deterministically (arm `due <= system_get_ticks()`, call `ktimer_expire_current_cpu()`, HIGH_LEVEL-protected); the 3-fires-in-150ms wall-clock + AP-armed SMP paths validate on WHPX/bare-metal serial.

> **Notes:**
> - Shipped `ktimer.{c,h}` (tick-based `kernel_timer_t` + `KeInitializeTimer`/`KeSetTimerEx`/`KeSetTimer`/`KeCancelTimer` + ISR-side `ktimer_expire_current_cpu`) and no-log `KeInsertQueueDpcEx` + service-CPU-pinning `KeInsertQueueDpcOnCpu` in `dpc.c`.
> - Wired into `lapic_timer_handler` + `pit_irq_handler` after `nt_timer_tick`, before the DPC drain (same-tick dispatch); `ktimer_init_lists()` runs pre-`sti` in `boot_interrupts.c`.
> - SMP/lifetime: timers AND their DPCs pinned to the BSP service CPU (AP timers/queues do not drain); `KeCancelTimer` fully locked; DPC handoff atomic under the ktimer lock (one-way ktimer->dpc); periodic re-arm O(1); empty-list lock-free fast path.
> - Reviewed: Codex design+adversarial+consistency+perf + 5 re-adversarial rounds fixed AP-target lost-wakeup, unbounded catch-up, cancel-vs-handoff UAF, klog-under-lock, dropped-warn; evidence in commit `2cf5af54` + review commit.
> - Canonical doc: `include/kernel/sched/ktimer.h` (ownership + lifetime + service-CPU contract).
> - Scope: §9 owns the minimal prereq; full KTIMER (FILETIME/QPC/coalescing) -> TODO-08; the DPC in-flight completion barrier shipped in §16 (timer free-after-cancel is now safe after `KeFlushQueuedDpcs`); ordered O(1) expiry structure -> the §9 perf-scalability item above.
> **Verified:** 2026-06-26 | commit `b026cf74` | 6/7 items | build OK | sched 94 PASS | smoke PASS (2.56s)
> **Deferred:** [H] timer ISR does an O(active-timers) full-list scan per tick under the ktimer lock; needs an ordered expiry structure (timer wheel/min-heap) (reason: empty-list fast path covers the common case; ordered structure is full-KTIMER work) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §9 (item: "Perf-scalability (deferred): replace the O(active-timers) per-tick ISR scan" at line 297)
> **Quality reviewed:** 2026-06-26 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 2H+3L fixed, 1H accepted-XREF, 1H deferred | scope: kernel-code-quality

---

## 10. Driver Migration and Workqueue Contract Split

- [x] Three-tier policy documented in `dpc.h` + `workqueue.h` headers: ISR claims+acks+queues a DPC (`KeRequestDpcFromIsr`); DPC at DISPATCH; blocking work to the PASSIVE workqueue.
- [x] Migrated the RTL8139 error/stats path to DPC-first (`rtl8139.c`: ISR acks REG_ISR + queues `rtl8139_err_dpc`, logging at DISPATCH); RX drain stays inline (full RX DPC-first -> §12, filed in `04-drivers-hardware/TODO-14 §7`).
- [x] `workqueue.h` comment de-conflated: PASSIVE thread tier (blocking work), explicitly NOT a DPC replacement; points the DPC analogue to `dpc.h`.
- [x] `KeRequestDpcFromIsr` static inline in `dpc.h` -- calls `KeInsertQueueDpc` (no inline klog; depth crossing arms `warn_pending`, so no serial I/O at DIRQL); device claim/ack stays explicit.
- [x] `KINTERRUPT` + `KeSynchronizeExecution` in `sched/kinterrupt.{c,h}`, irq.c-integrated: the dispatcher runs the bound ISR under `ki->lock`+`active_cpu`; KeSync raises to SynchronizeIrql + takes the lock; self-ISR rejected (lock-free counter).
  - This item owns the sync-object + SynchronizeIrql contract; GSI/vector routing + affinity already shipped in `01-boot-platform/TODO-11 §5`. → XREF: 01-boot-platform/TODO-11 §5 (irq_request_gsi)
- [x] Recorded the RTL8139 RX DPC-first follow-up in `04-drivers-hardware/TODO-14 §7` (after §12 drain-on-lower).
- [ ] KINTERRUPT lifetime hardening (deferred, latent): full mask+drain teardown barrier for live-interrupt hot-unplug + unified `irq_chain_lock` serialization of exclusive register/unregister vs bind. No driver binds a KINTERRUPT yet.
- [x] Commit: `"drivers: irq -- migrate ISR deferred path to DPC model"`

**Test checkpoint:** Migrated driver ISR path: interrupt fires → DPC queued → callback processes data at `DISPATCH_LEVEL`. Workqueue callback runs at `PASSIVE_LEVEL` for heavy work. `workqueue.h` comments updated. Driver smoke test under continuous interrupt load (network RX flood or disk I/O burst) remains stable. Serial: no DPC starvation warnings after 60s load test.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 4 KINTERRUPT/ISR-DPC tests (13 asserts), 0 failures. KeSynchronizeExecution + self-ISR-reject + KeRequestDpcFromIsr are unit-tested; the RTL8139 error-DPC + the RX-flood "stable under load" checkpoint validate on WHPX/bare-metal serial.

> **Notes:**
> - Shipped `sched/kinterrupt.{c,h}` (KINTERRUPT + KeInitializeInterrupt/KeConnectInterrupt/KeDisconnectInterrupt/KeSynchronizeExecution + self-ISR-reject counter) and `KeRequestDpcFromIsr` in `dpc.h`; the RTL8139 error path is now DPC-first.
> - KINTERRUPT is irq.c-integrated: both dispatchers run a bound vector's ISR under `ki->lock` + set `active_cpu`; `irq_bind/unbind_kinterrupt` back KeConnect/KeDisconnect. Opt-in -- unbound vectors take no extra lock.
> - Design + adversarial + re-adversarial adoptions (RX drain stays inline, KINTERRUPT dispatcher-bound, lock coexists with `nic_rx_lock`, inline DPC helper not ack-macro, no serial-I/O under the interrupt lock) in commit `cbbc0f62`.
> - Canonical doc: `include/kernel/sched/kinterrupt.h` (sync contract + lifetime/quiesce precondition).
> - Scope: §10 owns the per-interrupt SynchronizeIrql + lock contract; GSI/vector routing -> `01-boot-platform/TODO-11 §5`; full RX DPC-first -> `04-drivers-hardware/TODO-14 §7`; KINTERRUPT full teardown barrier -> the deferred item above.
> **Verified:** 2026-06-26 | commit `4a7bbd72` | 7/8 items | build OK | sched 110 PASS | smoke PASS (2.8s)
> **Accepted:** [M] RTL8139 RX still kmallocs a work item per packet in the ISR (full preallocated-ring RX DPC-first) (reason: scope -- RX migration deferred) -> XREF: 04-drivers-hardware/TODO-14-network-drivers.md §7 (item: "RTL8139 RX DPC-first" at line 170)
> **Deferred:** [H] KINTERRUPT lifetime hardening -- full teardown barrier for live-interrupt hot-unplug + unified register/unregister/bind serialization (reason: infra; latent, no driver binds) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §10 (item: "KINTERRUPT lifetime hardening" at line 327)
> **Quality reviewed:** 2026-06-26 | Codex 6x (adversarial, consistency, perf, re-adversarial x3) | 3H+2M+2L fixed, 1M accepted-XREF, 1H deferred | scope: kernel-code-quality

---

## 11. APC Object Type and Per-Thread Queues

Asynchronous Procedure Calls (APCs) are the per-thread deferred work mechanism at `APC_LEVEL`. Kernel-mode APCs handle I/O completion, thread cleanup, and inter-thread injection. User-mode APCs deliver asynchronous callbacks to alertable threads. Without APCs, `NtQueueApcThread`, async I/O completion, and alertable waits cannot function.

- [x] Created `include/kernel/sched/apc.h` + `src/kernel/sched/apc.c`.
- [x] `KAPC` struct (apc.h): type/size/thread/next (ApcListEntry)/kernel_routine/rundown_routine/normal_routine/normal_context/system_arg1/2/apc_state_index/apc_mode/inserted. Mirrors KDPC (intrusive link + Inserted flag).
- [x] `KAPC_STATE` (apc.h): `apc_list_head[Kernel/User]`, `kernel/user/special_user_apc_pending`, `kernel_apc_in_progress`, `process`. Embedded in `struct thread` (the schedulable unit is `struct thread`, not `struct task`).
- [x] `struct thread` gains `apc_state` + `saved_apc_state` (KAPC_STATE) + separate `kernel_apc_disable`/`special_apc_disable` counters (OUTSIDE the swappable KAPC_STATE) + a per-thread `apc_lock`. `saved_apc_state` is field-only (attach deferred).
- [x] `KeInitializeApc(...)` in `apc.c` -- inits all KAPC fields; a special kernel APC (NULL NormalRoutine) is forced KernelMode.
- [x] `KeInsertQueueApc(...)` -- under the target's `apc_lock`: special-kernel->head, normal/user->tail, sets the pending flag; returns FALSE if the target is THREAD_DEAD/FREE or the APC is already inserted. ISR-safe, zero-alloc.
- [x] `KeRemoveQueueApc(...)` -- under the lock; returns 1 if it was queued; clears the pending flag when its queue empties.
- [x] Critical/guarded regions: `KeEnterCriticalRegion`/`Leave` (++/--`kernel_apc_disable`), `KeEnterGuardedRegion`/`Leave` (++/--`special_apc_disable`), on `thread_current()`, underflow-guarded.
  - → XREF consumers: `02-kernel-core/TODO-06 §8` (EX_PUSH_LOCK acquire contract) + §9 (guarded mutex) -- now UNBLOCKED.
- [x] APC-disabled query: `KeAreApcsDisabled()` (critical OR guarded) + `KeAreAllApcsDisabled()` (guarded only).
- [ ] KeStackAttachProcess/KeUnstackDetachProcess + nested SavedApcState round-trip test (deferred): needs process CR3-attach infra; §11 ships the `saved_apc_state` field only.
- [ ] KAPC cross-thread lifetime hardening (deferred, latent): thread-generation identity, O(1) per-mode tail pointers, queue-depth cap, cross-thread leave-time delivery resignal (unlocked `kernel_apc_pending` leave-gate can miss a racing insert).
- [ ] Thread-slot lifecycle lock (deferred, pre-existing, latent): `thread_exit`/`join`/`reap`/`kthread_create` must mutually exclude on true SMP (a joiner can free a running thread's stack); BSP-only scheduler unraced today.
- [x] Commit: `"kernel: sched -- add KAPC object type and per-thread APC queues"`

**Test checkpoint:** `KeInitializeApc` + `KeInsertQueueApc` to a thread succeeds; `KernelApcPending` flag is set. `KeRemoveQueueApc` returns `TRUE` and clears the flag. `KeInsertQueueApc` to an exiting (THREAD_DEAD) thread returns `FALSE`. `KeEnterCriticalRegion` -> `KeAreApcsDisabled` reads `TRUE` (not `KeAreAllApcsDisabled`); `KeEnterGuardedRegion` -> `KeAreAllApcsDisabled` reads `TRUE`. Serial: `"apc: initialized per-thread APC queues"` on first thread init.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 4 KAPC tests (17 asserts), 0 failures. init fields, insert/remove + pending flag, insert-to-DEAD rejected, critical/guarded region gating.

> **Notes:**
> - Shipped `sched/apc.{c,h}` (KAPC + KAPC_STATE + KeInitializeApc/KeInsertQueueApc/KeRemoveQueueApc + critical/guarded regions + KeAreApcsDisabled/AllDisabled), mirroring `dpc.c` (intrusive link + per-thread irqsave lock, zero-alloc insert).
> - `struct thread` gains the APC queues + counters; APC state resets under `apc_lock` in create + task-init (NON-insertable until reset); `thread_exit` + reap mark DEAD/FREE under the same lock (no APC onto an exiting/reused thread).
> - Downstream: UNBLOCKS `02-kernel-core/TODO-06 §8` (push-lock) + §9 (guarded-mutex); delivery (KiDeliverApc) is §12. Codex design + adversarial adoptions in commit `6c4317cb`.
> - Canonical doc: `include/kernel/sched/apc.h`.
> - Scope: §11 owns the KAPC object + queue ops + region counters; delivery/rundown -> §12; KeStackAttachProcess + cross-thread generation/tail hardening -> the deferred items above.
> **Verified:** 2026-06-26 | commit `830ebf88` | 9/12 items | build OK | sched 128 PASS | smoke PASS (2.6s)
> **Deferred:** [Critical] thread-slot lifecycle lock -- thread_exit/join/reap/create are not mutually exclusive on a true SMP scheduler (a joiner can free a still-running thread's stack) (reason: pre-existing; BSP-only scheduler unraced today) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §11 (item: "Thread-slot lifecycle lock" at line 363)
> **Deferred:** [H] KAPC cross-thread lifetime hardening -- thread-generation identity + O(1) per-mode tail pointers + depth cap (reason: latent; no cross-thread APC consumer until §12) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §11 (item: "KAPC cross-thread lifetime hardening" at line 362)
> **Quality reviewed:** 2026-06-26 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+2M fixed, 1Crit deferred | scope: kernel-code-quality

---

## 12. APC Delivery Mechanism (KiDeliverApc)

> [!NOTE]
> **Shipped: kernel-mode delivery via `KeLowerIrql` only.** The original plan also wired `KiDeliverApc` into the C interrupt-return path; the Codex design review rejected running PASSIVE-level APC work at the C `isr_handler` restore site (before `iretq` / interrupt-state restore), so that path is deferred to a future audited return-trampoline. Kernel APCs deliver when `KeLowerIrql` crosses below `APC_LEVEL` (and DPCs when crossing below `DISPATCH_LEVEL`). **Rollback:** remove the `KiDeliverApc` / `dpc_drain_current_cpu` calls from `irql_lower_deliver` / `KeLowerIrql`; queues accumulate but the system runs.

The APC delivery engine runs at the `KeLowerIrql` transition point -- when IRQL drops below `APC_LEVEL`, queued kernel APCs are delivered. User-mode delivery (alertable-wait + trap-frame redirect) is deferred (blocked on the user dispatcher + a kernel wait primitive).

- [x] `KiDeliverApc` KERNEL-mode engine (`apc.c`): special APCs run `KernelRoutine` at `APC_LEVEL` (guarded-blocked); normal APCs also run `NormalRoutine` at `PASSIVE_LEVEL` (critical/in-progress blocked); dequeue-under-lock, run-after-unlock.
- [x] `KeLowerIrql` delivers kernel APCs crossing below `APC_LEVEL`, gated only by a guarded region (special APCs deliver inside a critical region); no per-CPU guard across the yieldable NormalRoutine.
- [x] DPC drain-on-lower: `KeLowerIrql` crossing below `DISPATCH_LEVEL` drains via `dpc_drain_current_cpu()` bracketed at DISPATCH under a tight `dpc_draining` guard; `KiDispatchDpc` restores via `irql_lower_deliver`. Unblocks §6 + §10.
- [x] Thread-exit / reap `RundownRoutine` via `apc_rundown_thread()` at all four death sites (`task_exit`, `task_wrapper`, `thread_exit`, `thread_reap_kernel_slot`), after DEAD/FREE under `apc_lock`.
- [ ] **User-mode APC delivery** (deferred, blocked): user trap-frame redirect to `KiUserApcDispatcher` for alertable + special-user APCs -> XREF: `TODO-23-exception-dispatch-seh.md §5` + TODO-10 trap-frame edit.
- [ ] **Alertable-wait integration** (deferred, blocked): `KiDeliverApc` into `KeWaitForSingleObject`/`Multiple` + `STATUS_USER_APC` + `KeTestAlertThread`. Blocked: no kernel wait primitive / no `alertable` thread state.
- [ ] **ISR-return delivery** (deferred, design-rejected): kernel-APC delivery from the C `isr_handler` restore site -- rejected (PASSIVE work before `iretq`); needs an audited return-trampoline. `KeLowerIrql` covers kernel APCs.
- [ ] **`NtQueueApcThreadEx`/`Ex2` special-user routing** (deferred, blocked): set `SpecialUserApcPending` + SSDT 0x0380/0x0381; `NtQueueApcThread` stays `STATUS_NOT_IMPLEMENTED` -> XREF: `TODO-12-native-api-ssdt.md §7`.
- [ ] **DPC software-interrupt-on-enqueue** (deferred): request a DISPATCH software interrupt at `KeInsertQueueDpc` time (NT HVL model) to close the `KeLowerIrql` drain-on-lower tail race; today drains on the next lower or timer tick.
- [x] Commit: `"kernel: sched -- add KiDeliverApc and APC delivery integration"`

> [!NOTE]
> User-mode APC delivery requires `KiUserApcDispatcher` in the user-mode runtime (ntdll equivalent): the kernel sets up a modified trap frame that redirects ring-3 to the dispatcher, which calls the APC routine then `NtContinue`. That dispatcher + trap-frame edit (TODO-23 §5 + TODO-10) do not exist yet, so user-mode delivery is deferred; this section ships the kernel-mode engine only.

**Test checkpoint:** Queue a normal kernel APC to the current thread and call `KiDeliverApc` -- `KernelRoutine` runs at `APC_LEVEL` then `NormalRoutine` at `PASSIVE_LEVEL`, pending clears, the delivery counter advances. A special kernel APC (NormalRoutine NULL) runs only its `KernelRoutine`. A critical region suppresses normal kernel APC delivery but NOT special; a guarded region suppresses both. A special kernel APC IS delivered through the real `KeLowerIrql` path (raise to `APC_LEVEL`, enter critical region, queue, lower to PASSIVE) despite the critical region. `apc_rundown_thread()` runs `RundownRoutine` for a still-queued APC on an exiting thread and empties the queue. All exercised by `TEST_CAT_SCHED`.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 6 §12 suites, 0 failures
> **Notes:**
> - Shipped: `KiDeliverApc` engine + `apc_rundown_thread()` in `apc.c`; `irql_lower_deliver()` + `KeLowerIrql` DPC-drain-on-lower + kernel-APC-delivery in `irql.c`; per-CPU `dpc_draining` guard in `smp.h`; rundown at 4 death/reap sites.
> - How it runs: kernel APCs deliver when `KeLowerIrql` crosses below `APC_LEVEL`; DPCs drain (bracketed at DISPATCH) when crossing below `DISPATCH_LEVEL`; `KiDispatchDpc` restores via `irql_lower_deliver` to keep its bounded single-batch contract.
> - Downstream: DPC drain-on-lower unblocks §6 timer-DPC latency + §10 RX-DPC-first; resolves §1's deferred "drain-on-lower unbuilt". Codex design + adversarial + 4 re-adversarial rounds; adoption in the commit.
> - Canonical doc: this section + `include/kernel/sched/apc.h` / `irql.h` headers.
> - Scope boundary: §12 owns kernel-mode delivery + DPC drain-on-lower + rundown. User-mode delivery, alertable waits, `KeTestAlertThread`, `Ex`/`Ex2`, ISR-return delivery deferred (TODO-23 §5 + TODO-10 + a wait primitive).
> **Verified:** 2026-06-26 | commit `e19187b6` | 4/9 items | build OK | sched 153 PASS | smoke PASS
> **Accepted:** [M] cross-thread `KeInsertQueueApc` racing a target's `KeLeaveCriticalRegion` can miss the unlocked `kernel_apc_pending` leave-gate (reason: no cross-thread kernel-APC producer yet; same-thread unaffected) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §11 (item: "KAPC cross-thread lifetime hardening" at line 361)
> **Deferred:** [M] `KeLowerIrql` drain-on-lower is best-effort -- a DPC queued in the probe->lower window survives the crossing (reason: needs a DPC software-interrupt-on-enqueue; pre-existing, race-neutral vs the unconditional bracket) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §12 (item: "DPC software-interrupt-on-enqueue" at line 397)
> **Quality reviewed:** 2026-06-26 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+1M+1L fixed, 1M accepted, 1M deferred, 1L open | scope: kernel-code-quality

---

## 13. IRQL Violation Traps and Structured Telemetry

- [x] `IRQL_REQUIRE_AT_MOST(level)` / `IRQL_REQUIRE_AT_LEAST(level)` macros in `irql.h` routing to `_irql_check_max` / new `_irql_check_min` (log the `__func__` callsite + count the violation).
- [x] Log IRQL contract violations (callsite, CPU, current + required level) + per-CPU `irql_violations` counter in `per_cpu_data`; monotonic `KeRaiseIrql`/`KeLowerIrql` mismatches also counted.
- [x] Explicit fault path: `irql_set_strict()` strict mode bugchecks before any klog (default off = telemetry); `KeLowerIrqlForced(level,reason)` classifies + counts forced lowers (`task_exit`/`task_wrapper` routed).
- [ ] **Per-CPU IRQL transition stack** (deferred, blocked): `{old,new,callsite}` LIFO validator -- blocked because spinlock `irqsave` + `irql_lower_deliver` + forced lowers write `current_irql` outside `KeRaise`/`KeLower`, so the stack diverges.
- [ ] **Centralize the IRQL write surface** (prerequisite for the validator): route every `current_irql` writer (spinlock irqsave/restore, `irql_lower_deliver`, forced lowers) through stack-aware primitives + a lint forbidding raw writes.
- [x] Feed counters into kernel logging: `irql_telemetry_dump()` sums per-CPU `irql_violations` + `irql_forced_lowers` to klog for boot/runtime health checks.
- [x] Commit: `"kernel: sched -- add IRQL contract diagnostics and telemetry"`

**Test checkpoint:** `IRQL_REQUIRE_AT_MOST(APC_LEVEL)` at `DISPATCH_LEVEL` logs a diagnostic + bumps the per-CPU `irql_violations` counter; `IRQL_REQUIRE_AT_LEAST(DISPATCH_LEVEL)` at `PASSIVE_LEVEL` does the same. A `KeRaiseIrql`/`KeLowerIrql` monotonic mismatch (lower-as-raise / raise-as-lower) is counted + clamped. `KeLowerIrqlForced(PASSIVE,reason)` from `DISPATCH` lowers + bumps `irql_forced_lowers`; a forced "lower" to a HIGHER level is rejected (counted as a violation, not performed). In strict mode (`irql_set_strict(1)`, default OFF) any violation escalates to `KeBugCheckEx` -- so a blocking wait at `DISPATCH_LEVEL` traps immediately instead of deadlocking. All exercised by `TEST_CAT_SCHED` (strict-mode bugcheck not unit-tested -- it halts; telemetry-mode counting is).

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 5 §13 suites, 0 failures
> **Notes:**
> - Shipped: `IRQL_REQUIRE_*` macros + `_irql_check_min` + `KeLowerIrqlForced` + strict mode + `irql_telemetry_dump` in `irql.{c,h}`; per-CPU `irql_violations`/`irql_forced_lowers` in `smp.h`; `task_exit`/`task_wrapper` routed.
> - How it runs: every violation site (REQUIRE macros, monotonic mismatch, forced-lower reject) calls `irql_record_violation` (atomic counter, then `KeBugCheckEx` in strict mode before klog); default telemetry mode logs + counts + continues.
> - Downstream: `irql_telemetry_dump` feeds boot/runtime health. Codex design + adversarial + re-adversarial adoptions in the commit.
> - Canonical doc: this section + `include/kernel/sched/irql.h`.
> - Scope boundary: §13 owns the macros + counters + strict trap + forced-lower classification. The LIFO transition-stack validator + the IRQL-write-surface centralization it needs are deferred here; §14 owns DPC/APC budget + watchdog.
> **Verified:** 2026-06-26 | commit `281e8db8` | 4/6 items | build OK | sched 166 PASS | smoke PASS
> **Deferred:** [M] per-CPU LIFO IRQL transition-stack validator unbuilt (reason: needs a centralized IRQL write surface -- spinlock `irqsave` / `irql_lower_deliver` / forced lowers all bypass `KeRaise`/`KeLower`) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §13 (item: "Per-CPU IRQL transition stack" at line 424)
> **Quality reviewed:** 2026-06-26 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 3M+1L fixed | scope: kernel-code-quality

---

## 14. Budgeted DPC/APC Fairness and Starvation Watchdog

- [x] Per-tick DPC count budget + carry-over: per-CPU token bucket in `dpc.c` (refill `DPC_BUDGET_PER_TICK`/tick, cap `DPC_BUDGET_CARRYOVER_MAX`); `drain_queue` spends a token/DPC, warns once/tick on exhaustion (hard bound stays `DPC_BATCH_LIMIT`).
- [x] DPC watchdog 0x133 **param 0x0** (single DPC > 100us): `drain_queue` times each DPC via `rdtscp_read` vs a precomputed threshold (armed only when all online CPUs have RDTSCP); WARN default, `dpc_watchdog_set_strict()` -> `KeBugCheckEx`.
- [ ] DPC watchdog 0x133 **param 0x1** (cumulative >= `DISPATCH_LEVEL` time/period) -- deferred, blocked: needs per-CPU time-at-DISPATCH accounting at every `current_irql` write site (the §13 IRQL-write-surface centralization, line 425).
- [x] Sustained queue-depth warning: `dpc_watchdog_tick()` warns when depth > `DPC_QUEUE_WARN_DEPTH` for `DPC_DEPTH_WARN_TICKS` consecutive ticks.
- [x] DPC importance-based drain ordering: provided by §7 head-insert (`HighImportance` -> head; `drain_queue` dequeues head-first), so high runs first. No drain-time reorder.
- [x] APC starvation watchdog: `kernel_apc_depth` in `KAPC_STATE` maintained under `apc_lock`; `KeInsertQueueApc` warns once on the kernel-APC crossing of `APC_STARVATION_WARN_DEPTH`.
- [x] Tuning constants in new `include/kernel/sched/dpc_config.h`.
- [ ] AP DPC watchdog coverage (deferred, NOT blocked): `dpc_watchdog_tick` is BSP-only, so an AP's `s_wd[ap]` budget never refills and its `warn_pending` crossing never emits. Fix: BSP cross-CPU sweep of budget + `warn_pending`, atomic budget.
- [ ] Remote-target DPC IPI (deferred, NOT blocked): a `KeSetTargetProcessorDpc`-to-idle-AP DPC can strand (no DPC IPI yet); add a DPC IPI vector draining the AP on remote insert. -> XREF: 16-architecture-ports/TODO-03-smp-scaling-processor-groups.md
- [x] Commit: `"kernel: sched -- add DPC/APC budget fairness and watchdog"`

**Test checkpoint:** Unit (`TEST_CAT_SCHED`): `kernel_apc_depth` tracks insert/remove/rundown (2 -> 1 -> 0); `dpc_watchdog_set_strict()` toggles. Runtime (WHPX / bare metal via serial -- not unit-testable without a live timer/TSC): a single DPC running > 100us logs `dpc: watchdog: DPC ... ran N us` and, in strict mode, bugchecks `0x133` param 0x0; queue depth > `DPC_QUEUE_WARN_DEPTH` for `DPC_DEPTH_WARN_TICKS` ticks logs the sustained-depth warning; per-tick budget exhaustion logs once; `HighImportance` DPC runs before `LowImportance` (§7 head-insert); APC starvation logs on the `APC_STARVATION_WARN_DEPTH` crossing. Tuning constants in `include/kernel/sched/dpc_config.h`. (param 0x1 cumulative-DISPATCH-time is deferred.)

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 2 §14 suites, 0 failures
> **Notes:**
> - Shipped: per-CPU DPC token-budget + single-DPC TSC watchdog + sustained-depth warn (`dpc.c`); `KAPC_STATE.kernel_apc_depth` + APC starvation warn (`apc.c`); `dpc_config.h`; `BUGCHECK_DPC_WATCHDOG_VIOLATION` 0x133.
> - How it runs: `dpc_watchdog_tick()` (timer ISR, pre-drain) refills budget + checks depth; `drain_queue` times each DPC (RDTSCP, armed only if all online CPUs have it) + spends a token; WARN default, `dpc_watchdog_set_strict(1)` bugchecks overruns.
> - Downstream: importance ordering reuses §7 head-insert. Codex design + adversarial + 3 re-adversarial adoptions in the commit.
> - Canonical doc: this section + `include/kernel/sched/dpc_config.h`.
> - Scope boundary: §14 owns the budget + watchdog + APC starvation. Deferred: 0x133 param 0x1 (needs the §13 IRQL-write-surface centralization) + AP-CPU watchdog coverage (owner §17). Threaded-DPC fairness -> §15-§17.
> **Verified:** 2026-06-26 | commit `ee2f09ab` | 6/8 items | build OK | sched 171 PASS | smoke PASS
> **Deferred:** [H] DPC watchdog 0x133 param 0x1 (cumulative >= `DISPATCH_LEVEL` time/period) unbuilt -- needs per-CPU time-at-DISPATCH accounting (reason: the §13 IRQL-write-surface centralization) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §14 (item: "DPC watchdog 0x133 **param 0x1**" at line 448)
> **Deferred:** [M] AP-CPU DPC watchdog coverage -- `dpc_watchdog_tick` services only the ticking BSP (reason: mirrors the existing BSP-only AP-drain limit) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §17 (item: "AP DPC watchdog coverage" at line 511)
> **Quality reviewed:** 2026-06-26 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 4M+1L fixed | scope: kernel-code-quality

---

## 15. Threaded DPC List Synchronization

> [!NOTE]
> **Resolved.** The original race (`drain_queue` ISR producer vs the worker thread both touching `threaded_head[cpu_id]` lock-free) is fixed: the threaded list is now a per-CPU FIFO whose head/tail/pending are mutated only under the per-CPU `DPC_QLOCK` (producer hand-off, worker pop, and `KeRemoveQueueDpc` all hold it), and the KDPC stays `queued`-owned across the cpu_queues->threaded hand-off so no concurrent re-insert can double-own it.

- [x] `threaded_head[]`/`threaded_pending[]` -> a cache-line-aligned per-CPU `struct dpc_threaded_slot threaded_q[]`; ALL mutation under the per-CPU `DPC_QLOCK` (producer/worker/`KeRemoveQueueDpc`) -- chose the spinlock over `atomic_exchange`.
- [x] Double-owner closed: `drain_queue` keeps `dpc->queued=1` across the cpu_queues->threaded hand-off (under `DPC_QLOCK`); the worker clears `queued` after popping; `KeRemoveQueueDpc` also unlinks from `threaded_q`.
- [x] FIFO order preserved: `threaded_q` appends to the tail (not head-prepend), so threaded DPCs run in insertion order.
- [x] Worker idle + budget: `dpc_thread_fn` idles on `event_wait_timeout` (yield-poll, SMP-safe); `dpc_watchdog_tick` re-signals while pending; <= `DPC_THREADED_BATCH_LIMIT`/CPU/pass.
- [x] Worker started at boot (CAS-idempotent `dpc_start_threads`) so a threaded DPC queued from ANY IRQL always has a worker -- a lazy first-PASSIVE-init start would strand one initialized above PASSIVE then queued.
- [x] `pending` folded into the locked hand-off (set/cleared under `DPC_QLOCK`); slot cache-line padded (`_Static_assert sizeof==64`).
- [ ] Runtime stress (deferred, runtime-only): fire threaded DPCs from multiple ISRs on 2+ CPUs concurrently -- validate on WHPX / bare metal (no SMP-ISR concurrency in the unit harness).
- [ ] SMP-safe blocking worker (deferred): worker yield-polls via `event_wait_timeout` (READY when idle, CPU cost) -- `event_t`'s waiter queue is lock-free, unsafe vs multi-CPU-ISR `event_set`. Make `event_t` SMP-safe, then switch to `event_wait`.
- [x] Commit: `"kernel: fix threaded DPC list race -- atomic handoff between ISR and worker"`

**Test checkpoint:** Unit (`TEST_CAT_SCHED`): a threaded DPC handed off by `dpc_drain_current_cpu` stays `queued=1` (owned) and `KeRemoveQueueDpc` cancels it off the threaded list (`queued`->0). Runtime (WHPX / bare metal, not unit-testable -- needs concurrent SMP ISRs): fire threaded DPCs from both LAPIC and PIT ISRs on 2+ CPUs simultaneously; no lost callbacks, no list corruption, FIFO order preserved; the worker idles on a bounded yield-poll and runs on hand-off / within one tick.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 2 §15 suites, 0 failures
> **Notes:**
> - Shipped (`dpc.c`): per-CPU FIFO `struct dpc_threaded_slot threaded_q[]` (cache-line padded); all mutation under `DPC_QLOCK`; `KeRemoveQueueDpc` unlinks from it; worker idle = `event_wait_timeout` (yield-poll, SMP-safe; true-block deferred).
> - How it runs: `drain_queue` (ISR) appends + `event_set`s under the lock keeping `queued=1`; the worker pops one node/iteration under the lock, runs at PASSIVE within a batch budget; `dpc_watchdog_tick` re-signals while pending (self-heal).
> - Worker lifecycle: `dpc_start_threads` is started at boot (`boot_desktop.c`) and is CAS-idempotent; boot-time (not lazy) so a threaded DPC queued from any IRQL always has a worker. The idle yield-poll CPU cost is deferred to the SMP-safe-`event_t` item.
> - Downstream: closes the §8 threaded-DPC race + double-owner; §16 owns the `KeFlushQueuedDpcs` threaded-completion barrier. Codex design + adversarial + perf + re-adversarial adoptions in the commit.
> - Canonical doc: this section + `include/kernel/sched/dpc.h` / `dpc_config.h`.
> - Scope boundary: §15 owns the threaded-list sync + worker idle/budget. `KeFlushQueuedDpcs` threaded wait -> §16; per-CPU threaded-worker affinity -> §17.

**Regression risk:** Changing the handoff pattern affects every threaded DPC consumer. Rollback: revert to single-CPU threaded DPC model (BSP-only).

> **Verified:** 2026-06-26 | commit `23d6b38a` | 6/8 items | build OK | 175 sched + 16 user tests, smoke PASSED
> **Deferred:** [HIGH] idle threaded-DPC worker yield-polls (`event_wait_timeout` keeps it READY when idle) -- `event_t`'s waiter queue is lock-free, unsafe vs multi-CPU-ISR `event_set` -> XREF: 02-kernel-core/TODO-07 §15 (item: "SMP-safe blocking worker (deferred)" -- make `event_t` SMP-safe then switch to `event_wait`)
> **Deferred:** [INFO] concurrent SMP-ISR threaded-DPC stress is runtime-only (no SMP-ISR concurrency in the unit harness) -> XREF: 02-kernel-core/TODO-07 §15 (item: "Runtime stress (deferred, runtime-only)")
> **Quality reviewed:** 2026-06-26 | Codex 14x (adversarial x4, consistency x4, perf x4, re-adversarial x2) | 4H+1M+3L fixed, 2H+1info open | scope: kernel-code-quality

---

## 16. KeFlushQueuedDpcs Threaded DPC Completion

> [!NOTE]
> **Resolved.** `KeFlushQueuedDpcs()` is now a true completion barrier (normal + threaded). It waits until every CPU's normal queue and threaded list are empty, no normal callback is mid-execution (`s_wd[cpu].in_flight`), and no threaded callback is in flight (`s_in_flight_threaded`). A teardown caller that has stopped its producers (NT contract) can then free the KDPC/context with no use-after-free. Bounded; fail-closed (bugcheck) on a stuck or self-rearming DPC.

- [x] Unified bounded wait loop -- per CPU samples normal head + threaded head + `s_wd[cpu].in_flight` together under `DPC_QLOCK` (closes the lock-free sampling race), then global `s_in_flight_threaded`; `yield()`s between rounds.
- [x] Threaded in-flight: global atomic `s_in_flight_threaded` ++ under `DPC_QLOCK` at the worker pop, -- after `routine()`; `dpc_in_flight_threaded()` accessor.
- [x] Normal in-flight: per-CPU `s_wd[cpu].in_flight` set under `DPC_QLOCK` at dequeue, cleared after `routine()` -- closes the §7-deferred normal-DPC in-flight gap (flush no longer returns while a non-threaded callback runs).
- [x] Fail-closed on cap exhaustion (self-rearming/wedged DPC): bugchecks `DPC_WATCHDOG_VIOLATION` sub-case 0x2 -- the void NT ABI cannot signal a soft failure. (Rejected a separate `KeFlushQueuedDpcsEx`.)
- [x] Re-entrancy guard (a DPC callback calling flush self-deadlocks on its own in-flight count): entry bugchecks if IRQL != PASSIVE (normal DPC, sub-case 0x4) or caller is the threaded worker task (threaded DPC, 0x3). NT no-flush-from-DPC.
- [x] Commit: `"kernel: KeFlushQueuedDpcs completion barrier -- normal + threaded in-flight"`

**Test checkpoint:** Unit (`TEST_CAT_SCHED`): hand a threaded DPC to the worker (`dpc_drain_current_cpu` at DISPATCH), then `KeFlushQueuedDpcs()` at PASSIVE -- after it returns the callback flag is set and `dpc_in_flight_threaded()==0`, so the KDPC could be freed with no UAF. The normal-DPC in-flight wait runs synchronously inside `drain_queue`/`KiDispatchDpc`, so its cross-CPU "blocks until complete" proof (and the fail-closed bugcheck path) is runtime-only -- WHPX / bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 1 §16 suite, 0 failures
> **Notes:**
> - Shipped (`dpc.c`): `KeFlushQueuedDpcs` rebuilt as a unified bounded retry loop over per-CPU normal+threaded heads + `s_wd[cpu].in_flight` + global `s_in_flight_threaded`; `drain_queue`/`dpc_thread_fn` bracket callbacks.
> - How it runs: caller at PASSIVE; `yield()`s between rounds so the scheduled-thread worker runs (single-CPU safe). Bounded by `DPC_FLUSH_THREADED_YIELD_CAP`; cap hit bugchecks (fail-closed) rather than returning as if quiesced.
> - Contract: PASSIVE-only, MUST NOT be called from a DPC routine (a threaded callback self-deadlocks -> bugcheck 0x3); flushes work outstanding AT CALL TIME, does NOT block new inserts -- caller stops producers first (NT semantics).
> - Downstream: closes the §15-deferred threaded-flush barrier AND the §7-deferred normal-DPC in-flight barrier; unblocks §9 timer free-after-cancel. Codex design + adversarial (multi-round) + consistency + perf adoptions in the commit.
> - Canonical doc: this section + `src/kernel/sched/dpc.c` `KeFlushQueuedDpcs` + `include/kernel/sched/dpc_config.h`.
> - Scope boundary: §16 owns the full DPC completion barrier. Per-CPU threaded-worker affinity -> §17; idle-poll worker cost + SMP-safe `event_t` -> §15 deferred item.
> **Verified:** 2026-06-26 | commit `ad5f1ef9` | 6/6 items | build OK | 177 sched + 16 user tests, smoke PASSED
> **Accepted:** [M] a normal DPC that ILLEGALLY lowers IRQL to PASSIVE before flushing bypasses the 0x4 entry guard, but still fail-closes via the 0x2 cap bugcheck (no UAF) (reason: a precise in_flight entry check false-positives a legit caller racing a timer-ISR-nested drain; root cause is the unbuilt IRQL-lower LIFO validator) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §13 (item: "Per-CPU IRQL transition stack" at line 421)
> **Quality reviewed:** 2026-06-26 | Codex 13x (design + adversarial-impl + re-adversarial + adversarial + consistency + perf) | 6H fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 17. Per-CPU Threaded DPC Worker Affinity

> [!NOTE]
> **Codex adversarial review finding (medium).** The current single worker thread drains all CPUs' threaded queues from whichever CPU the scheduler assigns it. Threaded DPCs targeted at a specific CPU may execute on the wrong core, breaking callbacks that rely on `smp_this_cpu()` or per-CPU device state.

- [x] Option B (shipped): documented the threaded-DPC no-CPU-affinity contract in `dpc.h` (`KDEFERRED_ROUTINE` + the 3 threaded entry points) -- target selects the threaded list only; the callback runs on the worker's CPU, not the target.
- [ ] Option A (deferred, blocked): one worker per online CPU with affinity (NT parity) -- needs `task_set_affinity()` (does not exist) -> XREF: 02-kernel-core/TODO-21-process-model-extensions.md §10 (item: "CPU affinity per process" at line 60).
- [x] Option C rejected: a global N-worker pool gives no per-CPU affinity, and IPI-dispatch is unsafe (a PASSIVE threaded callback may block/page). Real affinity needs Option A.
- [x] Evaluate: NT runs threaded DPCs on the target CPU's thread -- Option A is the parity choice (deferred above).
- [ ] AP DPC watchdog coverage (deferred, NOT blocked): AP DPCs drain via `KeLowerIrql` but the masked AP timer never refills `s_wd[ap]` budget; needs a BSP cross-CPU sweep + atomic budget -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §14.
- [x] Commit: `"kernel: document threaded DPC no-affinity contract (Option B); per-CPU affinity deferred"`

**Test checkpoint:** No kernel test surface for Option B (documentation contract; the shipped change is `dpc.h` header comments only). The `smp_this_cpu()->cpu_id == target` affinity test belongs to the deferred Option A and is not runnable until per-CPU affinity workers exist. Contract check: grep `dpc.h` for the AFFINITY note on the four threaded-DPC entry points.

> **Test runner:** N/A (Option B is a `dpc.h` documentation contract; the real affinity test belongs to deferred Option A) | validation: header-comment review + build OK
> **Notes:**
> - Shipped (`dpc.h`): the threaded-DPC no-CPU-affinity contract on `KDEFERRED_ROUTINE` + the 3 threaded entry points -- a threaded DPC's target selects only the threaded list; the all-CPU worker runs the callback on its own CPU, not the target.
> - Why Option B now: real per-CPU affinity (Option A, NT parity) needs `task_set_affinity()` which does not exist; IPI-dispatch is unsafe for a PASSIVE callback. The honest interim documents the contract so drivers do not assume per-CPU state.
> - Downstream: closes the threaded-DPC affinity footgun at the API surface. Codex design review (2 mediums) adoptions in the commit.
> - Canonical doc: `include/kernel/sched/dpc.h` (`KDEFERRED_ROUTINE` AFFINITY note).
> - Scope boundary: §17 owns only the documented contract; Option A (per-CPU affinity workers) is deferred (blocked on `task_set_affinity`); the AP-watchdog gap is owned by §14.
> **Verified:** 2026-06-26 | commit `103403de` | 3/5 items | build OK | docs-only (dpc.h header comments)
> **Deferred:** [M] threaded DPCs have no CPU affinity -- per-CPU affinity workers (NT parity) not built -> XREF: 02-kernel-core/TODO-21-process-model-extensions.md §10 (item: "CPU affinity per process" at line 65)
> **Deferred:** [M] AP DPC watchdog coverage -- `dpc_watchdog_tick` services only the BSP while AP-targeted DPCs drain via `KeLowerIrql` (stale `s_wd[ap]` budget) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §14 (item: "AP DPC watchdog coverage" at line 450)
> **Quality reviewed:** 2026-06-26 | Codex 2x (design, adversarial) | 3M fixed (contract scope, AP-watchdog rationale, §8 stale text), 2M deferred | scope: kernel-code-quality (docs-only)

---

## 18. System Worker Thread Pool (Long-Period Periodic Callbacks)

> [!NOTE]
> Threaded DPCs (§8) run at PASSIVE_LEVEL and are designed for sub-millisecond bottom-half work that must not block. They are wrong for the "monitor a slow firmware property every 60 seconds" use-case: a threaded DPC that sleeps holds the worker against other PASSIVE-level deferrals. §18 introduces a **separate** kthread pool dedicated to long-period periodic callbacks (1+ second cadence) that may legitimately call into firmware (UEFI runtime services), the registry, or other slow paths.

The kernel needs a generic "background monitor" primitive: register a callback with a period (in ms), and the system runs it on a dedicated kthread at that cadence. Mirrors NT `IoQueueWorkItem` / system worker threads (`ExpWorkerThread`) and Linux `delayed_work` / `workqueues`. Without it every monitor consumer would have to spawn its own kthread, leading to unbounded thread proliferation and inconsistent shutdown / cancellation semantics.

**Files:** `include/kernel/sched/kworker.h` (new), `src/kernel/sched/kworker.c` (new), `src/kernel/test/test_kworker.c` (new)

> [!NOTE]
> **Design (Codex design review adopted -- resolve before coding):**
> - **Rundown/cancellation (H):** token = slot index + generation. The worker, UNDER the slot spinlock and in the SAME critical section that checks `active` + `deadline_ns`, sets `running=1`, snapshots `callback`+`ctx`, releases the lock, calls the callback, re-locks, sets `running=0`, advances `deadline_ns`. `kworker_unregister` under the lock sets `active=0` + bumps the slot generation, then waits until `running==0`. This guarantees: no fire after unregister returns, no stale-snapshot call, no use of a freed `ctx`.
> - **Single worker is a documented limit (H):** ONE kthread runs all entries serially, so a slow/hung callback delays every other entry past its deadline. Callbacks MUST be bounded and non-hanging; a per-callback duration watchdog klog-warns past `KWORKER_CALLBACK_WARN_MS`. This is a single-worker background-monitor primitive, NOT a full N-thread workqueue -- do not claim full NT/Linux workqueue parity.
> - **Self-unregister guard (H):** record the kworker thread; `kworker_unregister` called FROM the worker thread (a callback unregistering its own/any token) sets `active=0` and SKIPS the drain-wait (else it deadlocks waiting for itself). `kworker_unregister` returns `int` status (so self-unregister can be signalled).
> - **Consumer signatures (M):** the callback type is `void (*)(void *ctx)`; an `int`/no-arg consumer like `uefi_secureboot_revalidate_tick` needs a `void(void*)` wrapper (e.g. `secureboot_revalidate_worker`).

- [x] `kworker_callback_t` (`void(*)(void*)`) + `struct kworker_entry` (callback, ctx, period_ms, last_fire_ns, deadline_ns, generation, active, running) in `kworker.h`; `KWORKER_MAX_ENTRIES=16`.
- [x] API (`kworker.c`): `int kworker_register(fn, ctx, period_ms)` -> token>=0 / neg (NULL fn, period 0, full table); `int kworker_unregister(int token)` cancels + drains in-flight; `int kworker_init(void)`; `uint64_t kworker_last_fire_ns(token)`.
- [x] Token = `(generation<<8)|slot` over a static `s_entries[16]` under `s_kworker_lock`; the 23-bit-masked generation keeps the token non-negative and pins the slot so a stale token cannot cancel a reused slot.
- [x] `kworker_init` creates ONE `task_create` worker (Phase 3, CAS-idempotent) that loops: fire due entries, coalesce `deadline=now+period`, yield-poll to the next deadline (cap `KWORKER_MAX_SLEEP_MS`) -- yield not `sleep_ms` (HAL busy-HLTs).
- [x] Rundown: worker sets `running=1` UNDER the lock it checks active+deadline; `unregister` clears active under lock then yield-drains `running==0`. Self-unregister (caller==worker thread via `thread_current()`) skips the drain.
- [x] Crash-safety contract: a faulting callback kills the worker (no SEH yet, documented) -> XREF: 02-kernel-core/TODO-10-kernel-security-hardening.md §11. Per-callback duration watchdog warns past `KWORKER_CALLBACK_WARN_MS`.
- [x] Diagnostics: klog at worker start / register-fail / over-duration; `kworker_last_fire_ns()` accessor.
- [x] Consumer wiring 1 (shipped): `uefi_secureboot_register_monitor()` registers a wrapper around `uefi_secureboot_revalidate_tick` at 5 min; `boot_desktop.c` calls it after `kworker_init()` success. -> XREF: `01-boot-platform/TODO-02 §15`.
- [ ] **Consumer wiring 2 (UEFI variable-store health monitor, gap-audit 2026-05-01 M1, migrated from TODO-02 §14):** the existing `uefi_runtime_populate_vars_registry()` writes once at boot. On firmware with small or leaking variable stores (a Lenovo class of bug), the first user-visible symptom of NVRAM-near-full is silent SetVariable failures on BootNext / dbx / MOK / capsule writes. Implementation:
  - Add `uefi_runtime_refresh_vars_registry()` in [`src/kernel/uefi_runtime.c`](../../src/kernel/uefi_runtime.c) that re-reads `QueryVariableInfo` and rewrites the `HKLM\SYSTEM\SecureBoot\Vars\*` registry keys (same fields the boot-time populator writes today, plus a new `VarsLow` DWORD = 1 when `RemainingSize < MaxStorageSize / 8`).
  - Hook `uefi_runtime_refresh_vars_registry()` into `rt_call_exit()` (or directly into `uefi_set_variable()`) so every `SetVariable` -- successful or failed -- updates the registry mirror with the post-write quota.
  - Add `uefi_vars_health_tick()` that calls refresh + emits `klog(LOG_WARN, "UEFI", "Vars store near-full: remaining=%u of max=%u (12.5%% threshold)", ...)` once on the transition into the low-quota state (sticky-once-warned to avoid spam; re-arm when quota recovers). Register with `kworker_register(uefi_vars_health_tick, NULL, 60 * 1000)` (60-second period per item spec).
  - Test (`src/kernel/test/test_uefi_boot.c`): `test_uefi_vars_health_threshold` synthesizes a low-`RemainingSize` fixture (best-effort -- the firmware mock layer does not yet support QueryVariableInfo override; gate the assertion on `g_boot_info.uki_test_mode` or accept the gap with a Note).
- [ ] **Consumer wiring 3 (S3 resume re-prime; gap-audit 2026-05-01):** when [`TODO-26 §3`](TODO-26-power-management.md#3-s3-suspend-to-ram) (S3 suspend/resume) lands, the resume handler must call BOTH `uefi_secureboot_refresh()` and `uefi_runtime_refresh_vars_registry()` after `pm_notify_resume()` finishes (firmware/registry back to D0) and before user threads unblock. Reciprocal back-references already filed in TODO-26 §3 prose for the SecureBoot drift refresh; add a parallel item there for the vars-registry re-prime in the same commit that migrates this consumer here.
- [x] Tests (`test_kworker.c`, `TEST_CAT_SCHED`): deterministic register-validation, stale-token, generation rundown, slot exhaustion, invalid-token (huge period so the worker never fires mid-test). Timing fire-count is runtime-only.
- [x] Commit: `"kernel/sched: system worker thread pool (kworker) + SecureBoot drift consumer; vars-health + S3 deferred"`

**Test checkpoint:** `kworker_register(fn, NULL, 50)` schedules `fn` to fire every 50 ms; `sleep_ms(500)` confirms ~10 fires. `kworker_unregister(token)` blocks until the in-flight call returns and prevents further fires. UEFI `Vars\VarsLow` registry key flips to 1 when a synthetic low-quota fixture is injected; klog WARN line fires once on the transition. SecureBoot drift detection ticks every 5 minutes without measurably impacting CPU (one `uefi_get_variable` per tick is single-digit milliseconds). Test on: QEMU TCG, QEMU WHPX, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched, TEST_CAT_SCHED) | 5 `test_kworker_*` suites, 0 failures (vars-health test ships with deferred Consumer 2)
> **Notes:**
> - Shipped: `kworker.h`/`kworker.c` -- a single-worker background-monitor pool (register/unregister/init + `kworker_last_fire_ns`), `KWORKER_MAX_ENTRIES=16`, generation-rundown cancellation, duration watchdog; `test_kworker.c` (5 deterministic suites).
> - How it runs: one `task_create` worker (own task) at boot Phase 3 fires due callbacks at PASSIVE via a cooperative yield-poll (not `sleep_ms`, which busy-HLTs single-CPU); Consumer 1 (SecureBoot, 5 min) wired after a confirmed start.
> - Concurrency: rundown via `running`-under-lock + thread-identity self-unregister guard; three-state `kworker_init` so a caller reports success only when the worker is running. Codex design + adversarial + 4 re-adversarial rounds in the commit.
> - Downstream: distinct from threaded DPCs (§8, sub-ms); the long-period monitor primitive NT IoQueueWorkItem / Linux delayed_work fill.
> - Canonical doc: `include/kernel/sched/kworker.h`.
> - Scope boundary: §18 owns the primitive + Consumer 1. Consumer 2 (UEFI vars-health) and Consumer 3 (S3 resume re-prime) are deferred (cross-TODO / blocked).
> **Verified:** 2026-06-26 | commit `b93b6a77` | 9/11 items | build OK | 197 sched + 16 user tests, smoke PASSED
> **Deferred:** [M] Consumer 2 -- UEFI variable-store health monitor (`uefi_runtime_refresh_vars_registry` + `uefi_vars_health_tick`, new) -> XREF: 01-boot-platform/TODO-02-uefi-hardening-secureboot.md §14 (UEFI vars registry)
> **Deferred:** [M] Consumer 3 -- S3 resume re-prime (SecureBoot + vars registry after `pm_notify_resume`) -> XREF: 02-kernel-core/TODO-26-power-management.md §3 (S3 suspend/resume -- not yet created)
> **Quality reviewed:** 2026-06-26 | Codex 8x (design, adversarial, re-adversarial x4, consistency, perf) | 7H+4M fixed, 1M accepted (gen-wrap), 2M deferred | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                        | 🪟 Win11                     | 🐧 Linux                      | 🚀 Impossible OS               |
| --- | ------------------------------ | ---------------------------- | ----------------------------- | ------------------------------ |
| 💎  | IRQL / preemption levels       | ✅ KIRQL (PASSIVE→HIGH)      | ✅ preempt/softirq/hardirq    | ✅ §1-§3 done                  |
| 💎  | DPC bottom-half queue          | ✅ KDPC at DISPATCH_LEVEL    | ✅ softirq/tasklet/NAPI       | ✅ §4-§6                       |
| 💎  | DPC drain on IRQL lower        | ✅ at DISPATCH→below         | ✅ softirq on local_bh_enable | ✅ §12 KeLowerIrql drain       |
| 💎  | ISR-safe deferred enqueue      | ✅ KeInsertQueueDpc          | ✅ IRQ-safe enqueue           | ✅ §4                          |
| 💎  | Per-CPU deferred queues        | ✅ Per-CPU DPC state         | ✅ Per-CPU softirq            | ✅ §4-§6                       |
| 💎  | DPC targeting (CPU affinity)   | ✅ KeSetTargetProcessorDpc   | ✅ Per-CPU workqueues         | ⚠️ §7 DPC; §17 threaded        |
| 💎  | DPC importance / priority      | ✅ 4 levels (Low→High)       | ⚠️ Priority workqueues        | ✅ §7                          |
| 💎  | DPC flush barrier              | ✅ KeFlushQueuedDpcs         | ✅ flush_workqueue            | ⚠️ §7 normal; §16 threaded     |
| 💎  | Threaded DPCs (PASSIVE)        | ✅ KeInitializeThreadedDpc   | ✅ request_threaded_irq       | ⚠️ §8 + §15 sync; §16-§17      |
| 💎  | Timer-DPC auto-queue           | ✅ KeSetTimerEx + KDPC       | ✅ timer_setup + callback     | ✅ §9 ktimer (tick-based)      |
| 💎  | Context legality contract      | ✅ API rules by IRQL         | ✅ might_sleep() + atomic     | ✅ §1 in irql.h                |
| 💎  | Workqueue (thread deferred)    | ✅ Work items at PASSIVE     | ✅ alloc_workqueue            | ⚠️ §10 -- exists, needs split  |
| 💎  | APC objects (KAPC)             | ✅ KeInitialize/InsertApc    | ⚠️ Signals only               | ✅ §11 KAPC + per-thread queue |
| 💎  | APC delivery engine            | ✅ KiDeliverApc at APC_LEVEL | ⚠️ do_signal on return        | ✅ §12 kernel-mode             |
| 💎  | Critical/guarded regions       | ✅ KeEnterCriticalRegion     | ⚠️ preempt_disable            | ✅ §11 critical + guarded      |
| 💎  | Alertable wait + user APC      | ✅ WaitForSingleObjectEx     | ❌ No equivalent              | ⬜ §12                         |
| 💎  | Special user-mode APCs         | ✅ NtQueueApcThreadEx (RS5+) | ❌ No equivalent              | ⬜ §12 SpecialUserApc          |
| 💎  | ISR sync object                | ✅ KeSynchronizeExecution    | ✅ spin_lock_irqsave          | ✅ §10 KeSynchronizeExecution  |
| ⭐  | IRQL nesting validation (LIFO) | ❌ No runtime check          | ✅ lockdep IRQ-state          | ⬜ §13 transition stack        |
| ⭐  | IRQL violation telemetry       | ⚠️ Checked builds only       | ⚠️ Fragmented debug warnings  | ✅ §13 counters + strict trap  |
| ⭐  | DPC/APC fairness watchdog      | ⚠️ Internal heuristics       | ⚠️ Subsystem-specific         | ✅ §14 budget + 0x133 + APC    |

> **After §1-§8:** Impossible OS reaches parity on the core IRQL contract and DPC architecture: IRQL transitions, per-CPU DPC queues, auto-drain, targeting, importance, and baseline threaded DPC support all exist.
> **§9-§10** close the remaining timer-DPC and driver-migration gaps so drivers stop treating workqueue as a DPC substitute.
> **§11-§12** add the APC subsystem -- the per-thread deferred work mechanism required by async I/O completion, `NtQueueApcThread`, alertable waits, and thread cleanup.
> **§13-§14** turn correctness and fairness into explicit kernel contracts instead of hidden implementation behavior.
> **§15-§17** fix the threaded-DPC race, flush, and affinity gaps surfaced by the Codex adversarial review.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_irql_dpc()` in [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_irql_dpc.c` with:
  - **IRQL basics (§1-§3):**
    - `KeGetCurrentIrql()` returns `PASSIVE_LEVEL` when called from normal thread context
    - `KeRaiseIrql(DISPATCH_LEVEL, &old)` sets current IRQL to `DISPATCH_LEVEL`; old is `PASSIVE_LEVEL`
    - `KeLowerIrql(PASSIVE_LEVEL)` restores to `PASSIVE_LEVEL` after raise
    - `KeRaiseIrql` to a level below current IRQL triggers a debug assertion (illegal transition)
    - `KIRQL` constants: `PASSIVE_LEVEL == 0`, `APC_LEVEL == 1`, `DISPATCH_LEVEL == 2`, `HIGH_LEVEL == 31`
  - **DPC core (§4-§6):**
    - `KeInitializeDpc` sets routine and context; `KDPC` fields are non-NULL after init
    - `KeInsertQueueDpc` from `DISPATCH_LEVEL` succeeds and enqueues the DPC
    - `KeRemoveQueueDpc` removes a queued DPC before it fires; returns `TRUE`
    - DPC callback executes at `DISPATCH_LEVEL` (callback checks `KeGetCurrentIrql() == DISPATCH_LEVEL`)
    - DPC callback sets a flag; after `KiDispatchDpc()`, the flag is set (proves execution)
    - Duplicate `KeInsertQueueDpc` for same DPC does not double-enqueue (single execution)
    - Per-CPU queue isolation: DPC queued on CPU 0 does not drain on CPU 1 (SMP test)
    - IRQL violation: attempt blocking wait at `DISPATCH_LEVEL` is trapped (does not deadlock)
  - **DPC targeting/importance (§7):**
    - `KeSetTargetProcessorDpc` to CPU 1 + queue from CPU 0 → DPC links onto CPU 1's queue (`queued_cpu == 1`); CPU-1 callback execution awaits the deferred Remote-target DPC IPI item (idle AP has no drain trigger)
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
  - **APC (§11-§12):**
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

---

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot log prints IRQL subsystem init in Phase 1 after timer/interrupt setup
- [ ] `KeGetCurrentIrql()` reports `PASSIVE_LEVEL` in normal thread context and mapped DIRQL inside ISR
- [ ] Queuing `KDPC` from timer interrupt executes callback at `DISPATCH_LEVEL` and returns to prior IRQL
- [ ] Blocking wait attempt from DPC path is trapped and logged as IRQL violation
- [ ] Workqueue callback still runs in thread context (`PASSIVE_LEVEL`) and may yield safely
- [ ] SMP check: per-CPU DPC queue drains on each active core without cross-core corruption
- [ ] DPC targeted to CPU 1 from CPU 0 links onto CPU 1's queue (`queued_cpu == 1`); CPU-1 execution awaits the deferred Remote-target DPC IPI item
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

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched, TEST_CAT_SCHED) | suites: test_irql_dpc + test_kworker (registered via test_register_irql_dpc + test_register_kworker)
