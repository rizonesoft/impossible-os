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

| ⭐  | Order | Deliverable                                        | Depends On  | Status |
| --- | :---: | -------------------------------------------------- | ----------- | :----: |
| 💎  |   1   | `KIRQL` type, constants, and core contract         | --          |  [x]   |
| 💎  |   2   | Per-CPU IRQL tracking and transition primitives    | §1          |  [x]   |
| 💎  |   3   | Interrupt entry/exit IRQL integration              | §2          |  [/]   |
| 💎  |   4   | DPC object type and per-CPU queue                  | §2          |  [x]   |
| 💎  |   5   | DPC drain loop at `DISPATCH_LEVEL`                 | §3, §4      |  [x]   |
| 💎  |   6   | Timer/APIC scheduling path for DPC dispatch        | §5          |  [/]   |
| 💎  |   7   | DPC targeting, importance, and flush               | §4, §5      |  [/]   |
| 💎  |   8   | Threaded DPCs (`PASSIVE_LEVEL` DPC variant)        | §5          |  [x]   |
| 💎  |   9   | Timer-DPC association                              | §4, §6      |  [/]   |
| 💎  |  10   | Driver migration and workqueue contract split      | §5          |  [/]   |
| 💎  |  11   | APC object type and per-thread queues              | §1, §2      |  [/]   |
| 💎  |  12   | APC delivery mechanism (KiDeliverApc)              | §11         |  [ ]   |
| ⭐  |  13   | IRQL violation traps and structured telemetry      | §2, §3, §5  |  [ ]   |
| ⭐  |  14   | Budgeted DPC/APC fairness and starvation watchdog  | §5, §6, §12 |  [ ]   |
| 💎  |  15   | Threaded DPC list synchronization                  | §8          |  [ ]   |
| 💎  |  16   | KeFlushQueuedDpcs threaded DPC completion          | §8, §15     |  [ ]   |
| 💎  |  17   | Per-CPU threaded DPC worker affinity               | §8, §15     |  [ ]   |
| ⭐  |  18   | System worker thread pool (long-period periodic)   | §8          |  [ ]   |

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
> **Deferred:** [H] `irql.h` KeLowerIrql promised DPC drain-on-lower the impl never did; true NT drain-on-lower still unbuilt -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §12 (item: "DPC drain-on-lower" at line 285)
> **Deferred:** [M] `isr_handler` reports the LAPIC timer at DISPATCH and IPIs at HIGH instead of the named CLOCK_LEVEL/IPI_LEVEL -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §3 (item: "Report system vectors at named IRQLs" at line 136)
> **Deferred:** [M] strict-LIFO raise/lower pairing documented but only the monotonic check is enforced -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §13 (item: "per-CPU IRQL transition stack" at line 303)
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
> - Review-pass: the depth-warn `klog` moved AFTER the unlock (was serial I/O under the queue spinlock + cli); `executed++` moved under the lock.
> - Insert retries on the transient `queued_cpu==MAX_CPUS` (set while a remove/drain clears the DPC) so a concurrent re-insert is never dropped, and never double-links the observable case.
> - Caller precondition (NT contract, documented in `dpc.h`): a single KDPC must not be inserted concurrently from more than one CPU; sequential cross-CPU operations are safe.
> - Canonical: `src/kernel/sched/dpc.c`; tests in `src/kernel/test/test_sched.c`.
> **Verified:** 2026-06-26 | commit `7fefe6ab` | 5/5 items | build OK | tests 54 kernel + 16 user PASS
> **Deferred:** [H] threaded-DPC handoff double-owns a re-inserted KDPC (`drain_queue` clears `queued` + drops the lock before the `threaded_head` prepend) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §15 (item: "Threaded-DPC pending state" at line 351)
> **Deferred:** [M] per-CPU DPC queue/lock storage false-shares the ISR-hot insert/drain path -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §7 (item: "Cacheline-align per-CPU DPC storage" at line 207)
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
> **Deferred:** [H] `KeFlushQueuedDpcs` `head==NULL` poll can return while a remote callback is still in-flight (no in-flight tracking; silent 100000-spin timeout) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §7 (item: "`KeFlushQueuedDpcs` completion barrier" at line 221)
> **Deferred:** [H] threaded-DPC `drain_queue` hand-off races the worker on `threaded_head` and re-insert -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §15 (item: "Threaded-DPC pending state" at line 365)
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
- [x] `KeSetImportanceDpc(dpc, importance)` -- 4 levels: Low/Medium/MediumHigh/High
- [x] `KeInsertQueueDpc` modified: HighImportance -> head-insert; all others -> tail (FIFO)
- [x] `KeFlushQueuedDpcs()` -- drains local queue directly, spin-waits for other CPUs
- [x] `KDPC_IMPORTANCE` enum added; `KDPC.importance` field (default: MediumImportance)
- [x] `KeSetTargetProcessorDpcEx` (>64 CPU): moved to `16-architecture-ports/TODO-03-smp-scaling-processor-groups.md §5`
- [x] Cross-CPU IPI for MediumHighImportance: moved to `16-architecture-ports/TODO-03-smp-scaling-processor-groups.md §5` (part of DpcEx)
- [x] Cacheline-align per-CPU DPC storage: `struct dpc_queue` padded + `aligned(64)`, per-CPU spinlock wrapped in 64B `dpc_lock_slot` (`DPC_QLOCK` accessor); two `_Static_assert`s pin both to one cache line. `dpc.c`/`dpc.h`
- [ ] `KeFlushQueuedDpcs` completion barrier (H): `head==NULL` polling returns while a remote callback is still in-flight (`drain_queue` dequeues before running it); track a per-CPU in-flight count + escalate the silent 100000-spin timeout (Codex §5)
- [x] Commit: `"kernel: sched -- add DPC targeting, importance, and flush"`

**Test checkpoint:** `KeSetTargetProcessorDpc` to CPU 1 + `KeInsertQueueDpc` from CPU 0 → DPC callback fires on CPU 1 (check `smp_this_cpu()` in callback). `HighImportance` DPC runs before `LowImportance` DPC queued earlier. `KeFlushQueuedDpcs` returns only after callback completes. Verify on QEMU WHPX SMP (2+ vCPUs), TCG, bare metal -- IPI delivery for cross-CPU DPC targeting differs across platforms.

> **Notes:**
> - DPC targeting/importance/flush: `KeSetTargetProcessorDpc` (cpu_target), `KeSetImportanceDpc` (4 levels), `KeInsertQueueDpc` HighImportance head-insert, `KeFlushQueuedDpcs`, and the `KDPC_IMPORTANCE` enum are implemented in `src/kernel/sched/dpc.c`.
> - Review-pass: the per-CPU DPC queue + lock storage is now cacheline-aligned (`struct dpc_queue` padded/aligned, per-CPU lock in a 64B `dpc_lock_slot`) to avoid ISR-hot-path false sharing; `_Static_assert`s pin the layout.
> - One open item is deferred (see Deferred): the `KeFlushQueuedDpcs` in-flight completion barrier (the cross-CPU "returns only after callback completes" checkpoint clause).
> - Canonical: `src/kernel/sched/dpc.c`.
> **Deferred:** [H] `KeFlushQueuedDpcs` `head==NULL` poll can return while a remote callback is still in-flight (no per-CPU in-flight tracking; silent 100000-spin timeout); the full barrier also needs threaded-DPC completion (§16). §9 ktimer teardown (free-after-cancel) now depends on this barrier -- until it lands, free dynamically-allocated timers only after the DPC is known to have run -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §7 (item: "`KeFlushQueuedDpcs` completion barrier" at line 240)

---

## 8. Threaded DPCs (`PASSIVE_LEVEL` DPC Variant)

Threaded DPCs run at `PASSIVE_LEVEL` in a dedicated per-CPU kernel thread, allowing operations forbidden at `DISPATCH_LEVEL` (paging, mutex acquisition). Used by audio/video drivers for latency-sensitive work.

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
> **Deferred:** [H] `dpc_thread_fn` yield-spins when idle + drains a CPU's threaded list unbounded (self-rearming DPC monopolizes the single worker) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §15 (item: "Worker idle wakeup + drain budget" at line 392)
> **Deferred:** [M] threaded `threaded_head`/`threaded_pending` lost-wakeup (clear-after-drain) + cache-line false sharing -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §15 (item: "Fold pending into the atomic handoff" at line 393)
> **Deferred:** [M] threaded callbacks run on the BSP worker, not the queuing CPU (no per-CPU affinity) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §17 (item: "Option B: Document that threaded DPCs have no CPU affinity guarantee" at line 422)
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
> - Scope: §9 owns the minimal prereq; full KTIMER (FILETIME/QPC/coalescing) -> TODO-08; DPC in-flight completion barrier -> §7 (line 240); ordered O(1) expiry structure -> the §9 perf-scalability item above.
> **Verified:** 2026-06-26 | commit `b026cf74` | 6/7 items | build OK | sched 94 PASS | smoke PASS (2.56s)
> **Accepted:** [H] `KeFlushQueuedDpcs` is not a true in-flight completion barrier, so timer free-after-cancel can race a still-running DPC (reason: infra owned elsewhere) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §7 (item: "`KeFlushQueuedDpcs` completion barrier" at line 240)
> **Deferred:** [H] timer ISR does an O(active-timers) full-list scan per tick under the ktimer lock; needs an ordered expiry structure (timer wheel/min-heap) (reason: empty-list fast path covers the common case; ordered structure is full-KTIMER work) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §9 (item: "Perf-scalability (deferred): replace the O(active-timers) per-tick ISR scan" at line 297)
> **Quality reviewed:** 2026-06-26 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 2H+3L fixed, 1H accepted-XREF, 1H deferred | scope: kernel-code-quality

---

## 10. Driver Migration and Workqueue Contract Split

- [x] Three-tier policy documented in `dpc.h` + `workqueue.h` headers: ISR claims+acks+queues a DPC (`KeRequestDpcFromIsr`); DPC at DISPATCH; blocking work to the PASSIVE workqueue.
- [x] Migrated the RTL8139 error/stats path to DPC-first (`rtl8139.c`: ISR acks REG_ISR + queues `rtl8139_err_dpc`, logging at DISPATCH); RX drain stays inline (full RX DPC-first -> §12, filed in `04-drivers-hardware/TODO-14 §7`).
- [x] `workqueue.h` comment de-conflated: PASSIVE thread tier (blocking work), explicitly NOT a DPC replacement; points the DPC analogue to `dpc.h`.
- [x] `KeRequestDpcFromIsr` static inline in `dpc.h` -- no-log `KeInsertQueueDpcEx` (no serial I/O at DIRQL); device claim/ack stays explicit (no ack-callback macro).
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
- [ ] KAPC cross-thread lifetime hardening (deferred, latent -- no cross-thread APC consumer until §12): thread-generation identity (reject a stale `apc->thread` after slot reuse) + O(1) per-mode tail pointers + a queue-depth cap.
- [ ] Thread-slot lifecycle lock (deferred, pre-existing, latent): `thread_exit`/`join`/`reap`/`kthread_create` must mutually exclude on true SMP (a joiner can free a running thread's stack); BSP-only scheduler unraced today.
- [x] Commit: `"kernel: sched -- add KAPC object type and per-thread APC queues"`

**Test checkpoint:** `KeInitializeApc` + `KeInsertQueueApc` to a thread succeeds; `KernelApcPending` flag is set. `KeRemoveQueueApc` returns `TRUE` and clears the flag. `KeInsertQueueApc` to an exiting (THREAD_DEAD) thread returns `FALSE`. `KeEnterCriticalRegion` -> `KeAreApcsDisabled` reads `TRUE` (not `KeAreAllApcsDisabled`); `KeEnterGuardedRegion` -> `KeAreAllApcsDisabled` reads `TRUE`. Serial: `"apc: initialized per-thread APC queues"` on first thread init.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 4 KAPC tests (17 asserts), 0 failures. init fields, insert/remove + pending flag, insert-to-DEAD rejected, critical/guarded region gating.

> **Notes:**
> - Shipped `sched/apc.{c,h}` (KAPC + KAPC_STATE + KeInitializeApc/KeInsertQueueApc/KeRemoveQueueApc + critical/guarded regions + KeAreApcsDisabled/AllDisabled), mirroring `dpc.c` (intrusive link + per-thread irqsave lock, zero-alloc insert).
> - `struct thread` gains the APC queues + counters; APC state resets under `apc_lock` in create + task-init (NON-insertable until reset); `thread_exit` + reap mark DEAD/FREE under the same lock (no APC onto an exiting/reused thread).
> - Downstream: UNBLOCKS `02-kernel-core/TODO-06 §8` (push-lock) + §9 (guarded-mutex); delivery (KiDeliverApc) is §12. Codex design + adversarial adoptions in commit `STAMPHASH11`.
> - Canonical doc: `include/kernel/sched/apc.h`.
> - Scope: §11 owns the KAPC object + queue ops + region counters; delivery/rundown -> §12; KeStackAttachProcess + cross-thread generation/tail hardening -> the deferred items above.
> **Verified:** 2026-06-26 | commit `STAMPHASH11R` | 9/12 items | build OK | sched 128 PASS | smoke PASS (2.6s)
> **Deferred:** [Critical] thread-slot lifecycle lock -- thread_exit/join/reap/create are not mutually exclusive on a true SMP scheduler (a joiner can free a still-running thread's stack) (reason: pre-existing; BSP-only scheduler unraced today) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §11 (item: "Thread-slot lifecycle lock" at line 363)
> **Deferred:** [H] KAPC cross-thread lifetime hardening -- thread-generation identity + O(1) per-mode tail pointers + depth cap (reason: latent; no cross-thread APC consumer until §12) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §11 (item: "KAPC cross-thread lifetime hardening" at line 362)
> **Quality reviewed:** 2026-06-26 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+2M fixed, 1Crit deferred | scope: kernel-code-quality

---

## 12. APC Delivery Mechanism (KiDeliverApc)

> [!WARNING]
> **High-risk section.** This wires `KiDeliverApc` into the interrupt/exception return path and `KeLowerIrql`. A bug in the delivery engine fires on every interrupt return and every IRQL transition, causing system-wide crashes. **Rollback:** If APC delivery crashes, remove the `KiDeliverApc` call from the interrupt return path and `KeLowerIrql`; APC queues accumulate but the system runs. Re-enable incrementally: first special kernel APCs only, then normal kernel APCs, then user APCs.

The APC delivery engine runs at defined IRQL transition points -- on return from interrupt/exception, when IRQL drops below `APC_LEVEL`, and when a thread completes an alertable wait. This is the bridge between queued APCs and their execution.

- [ ] Implement `KiDeliverApc(KPROCESSOR_MODE PreviousMode, void *ExceptionFrame, void *TrapFrame)`:
  - Raise IRQL to `APC_LEVEL`
  - **Special kernel APCs**: drain all from head of kernel APC list; execute `KernelRoutine` at `APC_LEVEL`; no thread permission needed
  - **Normal kernel APCs**: if thread is not in critical region and `KernelApcInProgress == FALSE`: set `KernelApcInProgress = TRUE`, lower to `PASSIVE_LEVEL`, call `NormalRoutine`, raise back to `APC_LEVEL`, clear `KernelApcInProgress`
  - **User APCs**: if `PreviousMode == UserMode` and thread is alertable and user APC list is non-empty: set up user-mode trap frame to redirect execution to `KiUserApcDispatcher` (→ XREF TODO-23 §5 for user-mode frame setup)
  - **Special user APCs**: if `SpecialUserApcPending` is set, deliver even when the thread is NOT alertable (Win11 NtQueueApcThreadEx special APCs interrupt non-alertable user-mode return); same `KiUserApcDispatcher` frame setup
  - Restore original IRQL
- [ ] Wire `KiDeliverApc` into interrupt/exception return path: call when returning to `PASSIVE_LEVEL` or `APC_LEVEL` and `KernelApcPending` or `UserApcPending` is set. Add `POST16(0xDC00)` before first `KiDeliverApc` call in ISR return and `POST16(0xDC01)` after return
- [ ] Wire into `KeWaitForSingleObject` / `KeWaitForMultipleObjects`: when `Alertable == TRUE` and wait completes or is interrupted, deliver user APCs before returning `STATUS_USER_APC`
- [ ] Wire into `KeLowerIrql`: when lowering from >= `APC_LEVEL` to below `APC_LEVEL`, check for pending kernel APCs and deliver
- [ ] DPC drain-on-lower (NT software-interrupt dispatch): when `KeLowerIrql` crosses from >= `DISPATCH_LEVEL` to below, drain pending DPCs via `KiDispatchDpc` with a reentrancy guard (today drain is §6 timer-ISR only)
- [ ] Implement `KeTestAlertThread(AlertMode)` -- tests and delivers pending user APCs without entering a wait
- [ ] Wire `NtQueueApcThreadEx`/`NtQueueApcThreadEx2`: route the `QUEUE_USER_APC_SPECIAL_USER_APC` flag to set `SpecialUserApcPending` (separate from plain `NtQueueApcThread` which queues a regular alertable user APC)
- [ ] Thread exit path: call `RundownRoutine` for all remaining queued APCs to prevent resource leaks
- [ ] Commit: `"kernel: sched -- add KiDeliverApc and APC delivery integration"`

> [!NOTE]
> User-mode APC delivery requires `KiUserApcDispatcher` in the user-mode runtime (ntdll equivalent). The kernel sets up a modified trap frame that redirects ring-3 execution to the dispatcher, which calls the APC routine and then calls `NtContinue` to restore the original context. Full user-mode dispatcher implementation is in TODO-23 §5; this section handles the kernel-side frame setup only.

**Test checkpoint:** Queue kernel APC to current thread; on `KeLowerIrql` to `PASSIVE_LEVEL`, APC fires (callback sets flag). Queue special kernel APC during ISR; APC fires on interrupt return. `KeEnterCriticalRegion` suppresses normal kernel APC delivery; `KeLeaveCriticalRegion` triggers deferred delivery. A `SpecialUserApcPending` APC fires on user-mode return even when the thread is NOT alertable (regular user APC does not). Thread exit calls `RundownRoutine` for un-delivered APCs. Serial: `"apc: delivered N kernel APCs, M user APCs"`. If crash, check POST -- 0xDC00 = entered `KiDeliverApc` in ISR return, 0xDC01 = completed. Verify on QEMU WHPX, TCG, VirtualBox, bare metal -- ISR return path modification is platform-sensitive.

---

## 13. IRQL Violation Traps and Structured Telemetry

- [ ] Add `IRQL_REQUIRE_AT_MOST(level)` and `IRQL_REQUIRE_AT_LEAST(level)` macros for fast debug enforcement.
- [ ] Log IRQL contract violations with subsystem, CPU, current level, required level, and callsite symbol.
- [ ] Convert silent misuse patterns (blocking wait at `DISPATCH_LEVEL`, `KeLowerIrql` mismatch) into explicit fault paths.
- [ ] Add a per-CPU IRQL transition stack recording `{old_irql, new_irql, callsite}` on raise/entry and LIFO-validating on lower/exit -- catches missing restores, stale saved-IRQL reuse, non-LIFO pairing (Linux lockdep parity)
- [ ] Feed counters into existing kernel logging for boot/runtime health checks.
- [ ] Commit: `"kernel: sched -- add IRQL contract diagnostics and telemetry"`

**Test checkpoint:** `IRQL_REQUIRE_AT_MOST(APC_LEVEL)` at `DISPATCH_LEVEL` triggers diagnostic log: `"irql: violation at <callsite> -- required <= APC_LEVEL, current = DISPATCH_LEVEL"`. Blocking wait at `DISPATCH_LEVEL` faults immediately (no deadlock). `KeLowerIrql` mismatch (lowering to wrong level) triggers assertion. A forced non-LIFO raise/lower sequence is caught by the transition-stack validator (logs the offending callsite). Violation counters visible in kernel log.

---

## 14. Budgeted DPC/APC Fairness and Starvation Watchdog

- [ ] Add per-tick DPC budget (count and/or time) with carry-over to avoid monopolizing CPU time.
- [ ] Add DPC watchdog with both Win11 Bug Check 0x133 sub-cases, escalated independently: param 0x0 = single DPC/ISR over 100us; param 0x1 = cumulative time at `>= DISPATCH_LEVEL` per period over budget (short-DPC floods trip 0x1 alone)
- [ ] Add watchdog warning when DPC queue depth remains above threshold for N ticks.
- [ ] Add DPC importance-based ordering in the drain loop (§7 importance levels determine execution order).
- [ ] Add APC starvation watchdog: warn when kernel APC queue depth on any thread exceeds threshold (indicates thread stuck in critical region or elevated IRQL too long).
- [ ] Publish tuning constants in one header for platform-specific calibration.
- [ ] Commit: `"kernel: sched -- add DPC/APC budget fairness and watchdog"`

**Test checkpoint:** A single DPC callback sleeping 200us triggers the param 0x0 `DPC_WATCHDOG_VIOLATION` (100us threshold). A burst of many sub-100us DPCs whose cumulative `>= DISPATCH_LEVEL` time exceeds the period budget trips the param 0x1 escalation with NO single 0x0 trip (separate regression case). DPC queue depth > 64 for > 5 ticks triggers depth warning. `HighImportance` DPC runs before `LowImportance` during budget-limited drain. APC starvation watchdog fires when kernel APC queue depth > threshold on test thread. Tuning constants in `include/kernel/sched/dpc_config.h`.

---

## 15. Threaded DPC List Synchronization

> [!WARNING]
> **Codex adversarial review finding (high).** `drain_queue()` in ISR context prepends threaded DPCs onto `threaded_head[cpu_id]` while the worker thread concurrently reads and rewrites the same pointer. The list is plain shared state with no lock -- a race can lose queued DPCs or corrupt the list under SMP load.

- [ ] Replace raw `threaded_head[cpu_id]` manipulation with `atomic_exchange`: ISR producer atomically swaps head to NULL, worker drains the snapshot
- [ ] Alternative: protect `threaded_head[]` with per-CPU spinlock (same as `queue_locks[]`)
- [ ] Verify: stress test with concurrent threaded DPC insertions from multiple ISRs on different CPUs
- [ ] Threaded-DPC pending state (H, double-owner): `drain_queue` clears `queued` + drops the lock before prepending to `threaded_head`; a re-insert in that gap double-owns the KDPC. Hold a pending state until the worker completes (Codex §4 re-adv)
- [ ] Worker idle wakeup + drain budget (H): `dpc_thread_fn` yield-spins idle (background CPU burn) and drains a CPU list unbounded (self-rearming DPC monopolizes the worker). Block on a producer-signaled event + per-CPU batch budget (Codex §8)
- [ ] Fold pending into the atomic handoff: clear `threaded_pending` in the same atomic snapshot that empties the list (clear-after-drain races a producer set -> stranded node) + pad `threaded_head`/`threaded_pending` to per-CPU lines (Codex §8)
- [ ] Commit: `"kernel: fix threaded DPC list race -- atomic handoff between ISR and worker"`

**Test checkpoint:** Run with 2+ CPUs, fire threaded DPCs from both LAPIC and PIT ISRs simultaneously. No lost callbacks, no list corruption. Serial log shows all threaded DPC completions.

**Regression risk:** Changing the handoff pattern affects every threaded DPC consumer. Rollback: revert to single-CPU threaded DPC model (BSP-only).

---

## 16. KeFlushQueuedDpcs Threaded DPC Completion

> [!WARNING]
> **Codex adversarial review finding (high).** `KeFlushQueuedDpcs()` only waits for normal DPC queue drain -- it does not wait for `threaded_head[]` or in-flight threaded DPC callbacks. A driver teardown calling `KeFlushQueuedDpcs()` can free state while a threaded DPC is still executing, causing use-after-free.

- [ ] Extend `KeFlushQueuedDpcs()` to also spin-wait on `threaded_pending[cpu_id] == 0` for all CPUs
- [ ] Add an `in_flight_threaded` counter per CPU: incremented before threaded DPC callback, decremented after; flush waits for zero
- [ ] Alternatively: add `KeFlushQueuedDpcsEx(FLUSH_THREADED)` for callers that need threaded DPC quiesce
- [ ] Commit: `"kernel: KeFlushQueuedDpcs waits for threaded DPC completion"`

**Test checkpoint:** Queue a threaded DPC, call `KeFlushQueuedDpcs()` from another thread, verify flush blocks until callback completes. Free the DPC object after flush -- no crash.

---

## 17. Per-CPU Threaded DPC Worker Affinity

> [!NOTE]
> **Codex adversarial review finding (medium).** The current single worker thread drains all CPUs' threaded queues from whichever CPU the scheduler assigns it. Threaded DPCs targeted at a specific CPU may execute on the wrong core, breaking callbacks that rely on `smp_this_cpu()` or per-CPU device state.

- [ ] Option A: Create one worker thread per online CPU with CPU affinity (`task_set_affinity(pid, cpu_mask)`)
- [ ] Option B: Document that threaded DPCs have no CPU affinity guarantee (PASSIVE_LEVEL, any-CPU execution) and forbid per-CPU assumptions in threaded DPC callbacks
- [ ] Option C: Route all threaded DPCs to a global queue (not per-CPU) with a pool of N worker threads
- [ ] Evaluate: Windows NT threaded DPCs run on the target CPU's thread -- Option A is the correct parity choice
- [ ] Prerequisite: `task_set_affinity()` does not exist yet (-> XREF: `TODO-21-process-model-extensions.md §10`)
- [ ] Commit: `"kernel: per-CPU threaded DPC workers with affinity"`

**Test checkpoint:** Queue threaded DPC targeting CPU 1. Verify callback's `smp_this_cpu()->cpu_id == 1`. Verify BSP-targeted threaded DPC runs on CPU 0.

---

## 18. System Worker Thread Pool (Long-Period Periodic Callbacks)

> [!NOTE]
> Threaded DPCs (§8) run at PASSIVE_LEVEL and are designed for sub-millisecond bottom-half work that must not block. They are wrong for the "monitor a slow firmware property every 60 seconds" use-case: a threaded DPC that sleeps holds the worker against other PASSIVE-level deferrals. §18 introduces a **separate** kthread pool dedicated to long-period periodic callbacks (1+ second cadence) that may legitimately call into firmware (UEFI runtime services), the registry, or other slow paths.

The kernel needs a generic "background monitor" primitive: register a callback with a period (in ms), and the system runs it on a dedicated kthread at that cadence. Mirrors NT `IoQueueWorkItem` / system worker threads (`ExpWorkerThread`) and Linux `delayed_work` / `workqueues`. Without it every monitor consumer would have to spawn its own kthread, leading to unbounded thread proliferation and inconsistent shutdown / cancellation semantics.

**Files:** `include/kernel/sched/kworker.h` (new), `src/kernel/sched/kworker.c` (new), `src/kernel/test/test_kworker.c` (new)

- [ ] Define `kworker_callback_t` typedef (`void (*)(void *ctx)`) + `struct kworker_entry` (callback, ctx, period_ms, last_fire_ns, deadline_ns, active flag) in [`include/kernel/sched/kworker.h`](../../include/kernel/sched/kworker.h).
- [ ] Define worker-pool API: `int kworker_register(kworker_callback_t fn, void *ctx, uint32_t period_ms)` returns a token (>= 0) or negative on failure (no slots, NULL fn). `void kworker_unregister(int token)` cancels and waits for any in-flight call to drain.
- [ ] Static slot table sized to `KWORKER_MAX_ENTRIES` (start with 16; grow if real consumer pressure demands). Spinlock-protected for register/unregister; the worker thread reads under the same lock or a snapshot copy.
- [ ] `void kworker_init(void)` (called from boot Phase 3 after scheduler is up): creates ONE kthread (`kthread_create(kworker_main, NULL, KWORKER_STACK_SIZE)`) that loops forever:
  - Snapshot the active entry list under spinlock.
  - For each active entry, if `now_ns >= entry.deadline_ns`, call its callback, set `last_fire_ns = now_ns`, advance `deadline_ns += period_ms * 1_000_000`.
  - Sleep until the next-earliest deadline (`sleep_ms((min_remaining_ns) / 1_000_000)`), capped at `KWORKER_MAX_SLEEP_MS` (~1000ms) so a freshly registered short-period entry is picked up promptly.
- [ ] Cooperative cancellation: `kworker_unregister()` sets `active=0` AND waits on a per-entry condvar / completion that the worker signals after a callback returns. Caller blocks until any in-flight call finishes; subsequent callback fires cannot occur because the slot is marked inactive.
- [ ] Crash safety: a callback that panics must NOT take down the worker thread. Wrap each call in the kernel's SEH equivalent (when TODO-10 SEH lands -> XREF: [`TODO-10 §11`](TODO-10-kernel-security-hardening.md)). Until then, document the contract: "callbacks must not fault; misbehaving consumer = worker thread dies = system silently loses all monitoring." Acceptable as initial state; tighten when SEH ships.
- [ ] **Diagnostics:** klog at register/unregister + on every callback fire under a debug flag (`KWORKER_TRACE`, default off in release). Per-entry `last_fire_ns` accessor for monitoring tools.
- [ ] **Consumer wiring 1 (deferred from TODO-02 §15 / drift detection):** register `uefi_secureboot_revalidate_tick` with `period_ms = 5 * 60 * 1000` (5 min). Detects post-boot firmware drift / NVRAM corruption / physical-attack tampering during sleep. -> XREF: [`01-boot-platform/TODO-02 §15`](../01-boot-platform/TODO-02-uefi-hardening-secureboot.md#15-post-boot-securebootrevalidation) (item: "Wire `uefi_secureboot_revalidate_tick()` into a 5-minute periodic kernel worker"). When this lands, flip the §15 follow-up `[ ]` to `[x]` and remove the local `// TODO consumer-wiring blocked-on-§18` placeholder if any.
- [ ] **Consumer wiring 2 (UEFI variable-store health monitor, gap-audit 2026-05-01 M1, migrated from TODO-02 §14):** the existing `uefi_runtime_populate_vars_registry()` writes once at boot. On firmware with small or leaking variable stores (a Lenovo class of bug), the first user-visible symptom of NVRAM-near-full is silent SetVariable failures on BootNext / dbx / MOK / capsule writes. Implementation:
  - Add `uefi_runtime_refresh_vars_registry()` in [`src/kernel/uefi_runtime.c`](../../src/kernel/uefi_runtime.c) that re-reads `QueryVariableInfo` and rewrites the `HKLM\SYSTEM\SecureBoot\Vars\*` registry keys (same fields the boot-time populator writes today, plus a new `VarsLow` DWORD = 1 when `RemainingSize < MaxStorageSize / 8`).
  - Hook `uefi_runtime_refresh_vars_registry()` into `rt_call_exit()` (or directly into `uefi_set_variable()`) so every `SetVariable` -- successful or failed -- updates the registry mirror with the post-write quota.
  - Add `uefi_vars_health_tick()` that calls refresh + emits `klog(LOG_WARN, "UEFI", "Vars store near-full: remaining=%u of max=%u (12.5%% threshold)", ...)` once on the transition into the low-quota state (sticky-once-warned to avoid spam; re-arm when quota recovers). Register with `kworker_register(uefi_vars_health_tick, NULL, 60 * 1000)` (60-second period per item spec).
  - Test (`src/kernel/test/test_uefi_boot.c`): `test_uefi_vars_health_threshold` synthesizes a low-`RemainingSize` fixture (best-effort -- the firmware mock layer does not yet support QueryVariableInfo override; gate the assertion on `g_boot_info.uki_test_mode` or accept the gap with a Note).
- [ ] **Consumer wiring 3 (S3 resume re-prime; gap-audit 2026-05-01):** when [`TODO-26 §3`](TODO-26-power-management.md#3-s3-suspend-to-ram) (S3 suspend/resume) lands, the resume handler must call BOTH `uefi_secureboot_refresh()` and `uefi_runtime_refresh_vars_registry()` after `pm_notify_resume()` finishes (firmware/registry back to D0) and before user threads unblock. Reciprocal back-references already filed in TODO-26 §3 prose for the SecureBoot drift refresh; add a parallel item there for the vars-registry re-prime in the same commit that migrates this consumer here.
- [ ] Tests in `test_kworker.c`: register a callback with period=10ms, sleep_ms(50), verify it fired ~5 times +/-1 (loose bound to absorb scheduler jitter); register two callbacks with different periods, verify each fires at its own cadence; unregister mid-flight, verify no further fires after unregister returns.
- [ ] Commit: `"kernel/sched: system worker thread pool for long-period periodic callbacks; wire UEFI vars-health + SecureBoot drift consumers"`

**Test checkpoint:** `kworker_register(fn, NULL, 50)` schedules `fn` to fire every 50 ms; `sleep_ms(500)` confirms ~10 fires. `kworker_unregister(token)` blocks until the in-flight call returns and prevents further fires. UEFI `Vars\VarsLow` registry key flips to 1 when a synthetic low-quota fixture is injected; klog WARN line fires once on the transition. SecureBoot drift detection ticks every 5 minutes without measurably impacting CPU (one `uefi_get_variable` per tick is single-digit milliseconds). Test on: QEMU TCG, QEMU WHPX, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched, TEST_CAT_SCHED) | new test_kworker_* suites + extended test_uefi_vars_health_threshold

---

## OS Comparison

| ⭐ | Feature                        | 🪟 Win11                          | 🐧 Linux                          | 🚀 Impossible OS                 |
|----|--------------------------------|--------------------------------|--------------------------------|-------------------------------|
| 💎 | IRQL / preemption levels       | ✅ KIRQL (PASSIVE→HIGH)        | ✅ preempt/softirq/hardirq     | ✅ §1-§3 done                 |
| 💎 | DPC bottom-half queue          | ✅ KDPC at DISPATCH_LEVEL      | ✅ softirq/tasklet/NAPI        | ✅ §4-§6                      |
| 💎 | ISR-safe deferred enqueue      | ✅ KeInsertQueueDpc            | ✅ IRQ-safe enqueue            | ✅ §4                         |
| 💎 | Per-CPU deferred queues        | ✅ Per-CPU DPC state           | ✅ Per-CPU softirq             | ✅ §4-§6                      |
| 💎 | DPC targeting (CPU affinity)   | ✅ KeSetTargetProcessorDpc     | ✅ Per-CPU workqueues          | ⚠️ §7 DPC; §17 threaded      |
| 💎 | DPC importance / priority      | ✅ 4 levels (Low→High)         | ⚠️ Priority workqueues         | ✅ §7                         |
| 💎 | DPC flush barrier              | ✅ KeFlushQueuedDpcs           | ✅ flush_workqueue             | ⚠️ §7 normal; §16 threaded   |
| 💎 | Threaded DPCs (PASSIVE)        | ✅ KeInitializeThreadedDpc     | ✅ request_threaded_irq        | ⚠️ §8; §15-§17 hardening     |
| 💎 | Timer-DPC auto-queue           | ✅ KeSetTimerEx + KDPC         | ✅ timer_setup + callback      | ✅ §9 ktimer (tick-based)     |
| 💎 | Context legality contract      | ✅ API rules by IRQL           | ✅ might_sleep() + atomic      | ✅ §1 in irql.h               |
| 💎 | Workqueue (thread deferred)    | ✅ Work items at PASSIVE       | ✅ alloc_workqueue             | ⚠️ §10 -- exists, needs split  |
| 💎 | APC objects (KAPC)             | ✅ KeInitialize/InsertApc      | ⚠️ Signals only                | ✅ §11 KAPC + per-thread queue |
| 💎 | APC delivery engine            | ✅ KiDeliverApc at APC_LEVEL   | ⚠️ do_signal on return         | ⬜ §12                        |
| 💎 | Critical/guarded regions       | ✅ KeEnterCriticalRegion       | ⚠️ preempt_disable             | ✅ §11 critical + guarded      |
| 💎 | Alertable wait + user APC      | ✅ WaitForSingleObjectEx       | ❌ No equivalent               | ⬜ §12                        |
| 💎 | Special user-mode APCs         | ✅ NtQueueApcThreadEx (RS5+)   | ❌ No equivalent               | ⬜ §12 SpecialUserApc         |
| 💎 | ISR sync object                | ✅ KeSynchronizeExecution      | ✅ spin_lock_irqsave           | ✅ §10 KeSynchronizeExecution |
| ⭐ | IRQL nesting validation (LIFO) | ❌ No runtime check            | ✅ lockdep IRQ-state           | ⬜ §13 transition stack       |
| ⭐ | IRQL violation telemetry       | ⚠️ Checked builds only         | ⚠️ Fragmented debug warnings   | ⬜ §13 -- unified diagnostics  |
| ⭐ | DPC/APC fairness watchdog      | ⚠️ Internal heuristics         | ⚠️ Subsystem-specific          | ⬜ §14 -- explicit policy      |

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

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched, TEST_CAT_SCHED) | suites: test_irql_dpc + test_kworker (registered via test_register_irql_dpc + test_register_kworker)
