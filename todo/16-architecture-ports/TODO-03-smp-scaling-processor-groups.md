---
schema_version: 1
id: smp-scaling-processor-groups
domain: 16-architecture-ports
status: active
title: "TODO-03 -- SMP Scaling & Processor Groups"
---

# TODO-03 -- SMP Scaling & Processor Groups

> **Goal:** Scale the kernel beyond 16 CPUs to support up to 256+ cores via Windows-style processor groups. Each group holds up to 64 logical processors; APIs that take a single CPU number get group-aware variants. This unblocks server-class hardware and future many-core ARM platforms.

> [!IMPORTANT]
> **Current state:** `MAX_CPUS=16` in `acpi.h`. All per-CPU arrays are statically sized. `KeSetTargetProcessorDpc` takes a bare `uint32_t` CPU number. No processor group concept exists. This is sufficient for desktop/laptop hardware but blocks server deployments.

## Inputs

- [`include/kernel/acpi.h`](../../include/kernel/acpi.h) -- `MAX_CPUS` constant
- [`include/kernel/smp.h`](../../include/kernel/smp.h) -- `per_cpu_data` struct, static arrays
- [`include/kernel/sched/dpc.h`](../../include/kernel/sched/dpc.h) -- `KeSetTargetProcessorDpc`, `DPC_TARGET_CURRENT`
- -> XREF: `02-kernel-core/TODO-07-irql-model-dpcs.md §9` -- `KeSetTargetProcessorDpcEx` deferred here

## Outcome

- `MAX_CPUS` raised to 256 (or dynamic allocation for per-CPU data).
- `PROCESSOR_NUMBER` type: `{ uint16_t Group; uint8_t Number; }` -- identifies a CPU within a group.
- Group-aware DPC API: `KeSetTargetProcessorDpcEx(KDPC *, PROCESSOR_NUMBER *)`.
- Group-aware affinity: `GROUP_AFFINITY` type for thread/interrupt affinity.
- ACPI MADT parsing supports >16 LAPIC entries.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| 💎  |   1   | Raise MAX_CPUS and dynamic per-CPU allocation | --         |  [ ]   |
| 💎  |   2   | PROCESSOR_NUMBER type and group definitions | §1         |  [ ]   |
| 💎  |   3   | KeSetTargetProcessorDpcEx -- group-aware DPC targeting | §2         |  [ ]   |
| 💎  |   4   | GROUP_AFFINITY for thread and interrupt affinity | §2         |  [ ]   |
| 💎  |   5   | MADT parsing for >64 LAPIC entries       | §1         |  [ ]   |

---

## 1. Raise MAX_CPUS and Dynamic Per-CPU Allocation

- [ ] Increase `MAX_CPUS` from 16 to 256 in `acpi.h`
- [ ] Convert static per-CPU arrays to dynamic allocation where needed (DPC queues, spinlock arrays)
- [ ] Verify boot still works with 1, 2, and 16 CPUs after the increase
- [ ] Commit: `"kernel: smp -- raise MAX_CPUS to 256"`

## 2. PROCESSOR_NUMBER Type

- [ ] Define `PROCESSOR_NUMBER { uint16_t Group; uint8_t Number; uint8_t Reserved; }` in a new header
- [ ] Define `GROUP_AFFINITY { uint64_t Mask; uint16_t Group; uint16_t Reserved[3]; }`
- [ ] Commit: `"kernel: nt -- PROCESSOR_NUMBER and GROUP_AFFINITY types"`

## 3. KeSetTargetProcessorDpcEx

- [ ] Implement `KeSetTargetProcessorDpcEx(KDPC *dpc, PROCESSOR_NUMBER *proc)` -- converts group+number to flat CPU index, calls `KeSetTargetProcessorDpc`
- [ ] Cross-CPU IPI for immediate dispatch on MediumHigh/HighImportance DPCs targeting a remote CPU
- [ ] Commit: `"kernel: sched -- KeSetTargetProcessorDpcEx with cross-CPU IPI"`

## 4. GROUP_AFFINITY for Affinity APIs

- [ ] Wire into `NtSetInformationThread(ThreadGroupInformation)` and `NtSetInformationProcess(ProcessDefaultCpuSetsInformation)`
- [ ] Commit: `"kernel: sched -- GROUP_AFFINITY thread/process affinity"`

## 5. MADT Parsing for >64 LAPICs

- [ ] Extend MADT parser to handle more than 64 LAPIC entries
- [ ] Commit: `"kernel: acpi -- MADT support for >64 logical processors"`

---

## OS Comparison

| ⭐  | Feature         | 🪟 Win11            | 🐧 Linux                 | 🚀 Impossible OS |
| --- | --------------- | ------------------- | ------------------------ | ---------------- |
| 💎  | >64 CPU support | ✅ Processor groups | ✅ cpumask_t (8192 CPUs) | ⬜ §1-§5         |
| 💎  | Group-aware DPC | ✅ DpcEx APIs       | ❌ N/A (no DPC model)    | ⬜ §3            |
| 💎  | Group affinity  | ✅ GROUP_AFFINITY   | ✅ cpu_set_t             | ⬜ §4            |

## Verification

- [ ] Boot with 1, 2, 16 CPUs -- no regressions
- [ ] `KeSetTargetProcessorDpcEx` targets correct CPU in group
- [ ] MADT with >16 entries parsed correctly
