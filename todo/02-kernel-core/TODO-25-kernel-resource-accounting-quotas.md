---
schema_version: 1
id: kernel-resource-accounting-quotas
domain: 02-kernel-core
status: active
title: "TODO-25 -- Kernel Resource Accounting & Quotas"
---

# TODO-25 -- Kernel Resource Accounting & Quotas

> **Goal:** Create one kernel authority for resource accounting and quota enforcement across objects, handles, processes, jobs, pools, registry, ALPC, notifications, crash buffers, and system-global limits. Today each subsystem plans local counters. That is not enough for a production OS: quota failures must be consistent, diagnosable, inherited, security-checked, and queryable.

> [!IMPORTANT]
> **Current state:** Process accounting and rlimits are partially planned in TODO-21; handle quota is planned in TODO-05; registry quota is planned in TODO-14; memory allocators and scheduler statistics live in `03-memory-concurrency`. There is no central `quota_charge` / `quota_return` API, no per-token quota block, no Job Object limit integration, and no single system information class for resource pressure.

## Inputs

- [`include/kernel/sched/task.h`](../../include/kernel/sched/task.h)
- [`src/kernel/ob`](../../src/kernel/ob/)
- [`src/kernel/registry.c`](../../src/kernel/registry.c)
- → XREF: [`TODO-05-object-manager.md`](./TODO-05-object-manager.md) -- handle/object counters
- → XREF: [`TODO-21-process-model-extensions.md`](./TODO-21-process-model-extensions.md) -- rlimits and Job Objects
- → XREF: [`TODO-15-security-reference-monitor.md`](./TODO-15-security-reference-monitor.md) -- token quota blocks and privileges
- → XREF: [`03-memory-concurrency/TODO-03-advanced-allocator.md`](../03-memory-concurrency/TODO-03-advanced-allocator.md) -- allocator statistics provider

## Outcome

- Every chargeable kernel resource has a type, owner, current usage, peak usage, hard limit, and failure status.
- Processes inherit quota blocks from tokens/jobs and can be constrained independently.
- Job Objects enforce aggregate process, memory, handle, CPU, and notification limits.
- Quota failures return NTSTATUS-compatible codes and produce audit/diagnostic events.
- System resource pressure can be queried from user mode and used by health/recovery policy.

## Implementation Order

| ⭐   | Order | Deliverable                                 | Depends On    | Status |
| --- | :---: | ------------------------------------------- | ------------- | :----: |
| 💎   |   1   | Resource type registry                      | --            |  [ ]   |
| 💎   |   2   | Quota block and charge API                  | §1            |  [ ]   |
| 💎   |   3   | Process/token/job ownership model           | T21, T15      |  [ ]   |
| 💎   |   4   | Object and handle quota integration         | T05           |  [ ]   |
| 💎   |   5   | Pool and allocation quota integration       | D03           |  [ ]   |
| 💎   |   6   | Registry, ALPC, notification quotas         | T24, T14, T16 |  [ ]   |
| ⭐   |   7   | CPU, I/O, and wakeup accounting             | T08, T21      |  [ ]   |
| 💎   |   8   | Native query/set quota syscalls             | T12           |  [ ]   |
| ⭐   |   9   | Resource pressure events and recovery hooks | T16, T30      |  [ ]   |
| 💎   |  10   | Tests, leak sweeps, and dashboards          | §1..§9        |  [ ]   |

## 1. Resource Type Registry

- [ ] Define resource types: handles, object bodies, namespace entries, paged pool, nonpaged pool, registry bytes, ALPC messages, notification states, timers, threads, processes, sections, mapped views, crash buffers.
- [ ] Each type includes accounting unit, default limit, privilege override, and human-readable name.
- [ ] Register types during Phase 2 before subsystem creation.

## 2. Quota Block and Charge API

- [ ] Define `quota_block_t`: refcount, owner SID, limits[], usage[], peaks[], failures[].
- [ ] Add `quota_charge(block, type, amount)`, `quota_return(block, type, amount)`, `quota_try_transfer`.
- [ ] Charges are atomic and rollback-safe.
- [ ] All failed charges return `STATUS_QUOTA_EXCEEDED` or a more specific NTSTATUS.

## 3. Process/Token/Job Ownership Model

- [ ] Add quota block pointer to `ACCESS_TOKEN`.
- [ ] Add effective quota block pointer to process/task.
- [ ] Job Objects may impose lower aggregate limits; a charge must pass both process and job quota.
- [ ] Token impersonation uses the thread effective token for security checks but process/job quota for resource charges unless API requires client charging.

## 4. Object and Handle Quota Integration

- [ ] Charge handle table entries on insert, return on close.
- [ ] Charge object body and name entry on object creation.
- [ ] Object Manager exposes per-type usage from quota counters, not local-only counters.
- [ ] DuplicateHandle checks target quota before inserting.

## 5. Pool and Allocation Quota Integration

- [ ] Add optional quota owner to tagged allocations.
- [ ] Charge nonpaged/paged pool through allocator provider hooks.
- [ ] Ensure kernel-internal early boot allocations are charged to System.
- [ ] Refuse user-triggered unbounded allocation paths without quota owner.

## 6. Registry, ALPC, Notification Quotas

- [ ] Registry: charge key/value names, data bytes, notification watchers, transactions.
- [ ] ALPC: charge queued messages, port sections, completion-list entries.
- [ ] Notifications: charge state objects, subscriptions, retained payload bytes.
- [ ] Default per-user caps are configurable through TODO-02.

## 7. CPU, I/O, and Wakeup Accounting

- [ ] Track per-process and per-job CPU time with user/kernel split.
- [ ] Track I/O operation count and bytes by read/write/control.
- [ ] Track wakeups/sec and timer creation rate for battery/health policy.
- [ ] Feed power and health policy without duplicating scheduler internals.

## 8. Native Query/Set Quota Syscalls

- [ ] Add `NtQueryQuotaInformationProcess`, `NtSetQuotaInformationProcess`.
- [ ] `ProcessQuotaLimits`: project `QUOTA_LIMITS` from the TODO-21 §9 `rlimits[]` + real VM/working-set counters; reconcile `RLIMIT_NOFILE` with `handle_table.handle_limit`. -> XREF: `TODO-21-process-model-extensions.md §9`
- [ ] Extend Job Object information classes for aggregate limits.
- [ ] Add `SystemResourcePressureInformation`.
- [ ] Require privilege for raising limits; lowering own soft limit is allowed.

## 9. Resource Pressure Events and Recovery Hooks

- [ ] Publish pressure levels through TODO-16: normal, watch, warning, critical.
- [ ] On critical pressure, notify TODO-30 recovery orchestrator before panicking.
- [ ] Trigger targeted cleanup: drain caches, trim logs, ask services to release memory, refuse new handles from offending process.

## 10. Tests, Leak Sweeps, and Dashboards

- [ ] Unit tests: charge/return, rollback, concurrent charges, duplicate handle quota, registry quota, ALPC quota.
- [ ] Boot leak sweep compares all quota blocks before/after test categories.
- [ ] Add `quota_dump()` for serial and crash dumps.
- [ ] Bulletproofing: every charge path must name a resource type and owner.

