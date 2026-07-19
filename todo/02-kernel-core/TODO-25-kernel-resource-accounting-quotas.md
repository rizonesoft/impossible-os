---
schema_version: 1
id: kernel-resource-accounting-quotas
domain: 02-kernel-core
status: active
title: "TODO-25 -- Kernel Resource Accounting & Quotas"
---

# TODO-25 -- Kernel Resource Accounting & Quotas

> **Validated:** 2026-07-19 | validate-todo-file clean (structure / IO table / XREF / test wiring)

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

| ⭐   | Order | Deliverable                                 | Depends On              | Status |
| --- | :---: | ------------------------------------------- | ----------------------- | :----: |
| 💎   |   1   | Resource type registry                      | --                      |  [ ]   |
| 💎   |   2   | Quota block and charge API                  | §1                      |  [ ]   |
| 💎   |   3   | Process/token/job ownership model           | T21 §9, T15 §4          |  [ ]   |
| 💎   |   4   | Object and handle quota integration         | T05 §14                 |  [ ]   |
| 💎   |   5   | Pool and allocation quota integration       | D03T03 §6,§7            |  [ ]   |
| 💎   |   6   | Registry, ALPC, notification quotas         | T24 §6, T14 §15, T16 §7 |  [ ]   |
| ⭐   |   7   | CPU, I/O, and wakeup accounting             | T08 §6, T21 §9          |  [ ]   |
| 💎   |   8   | Native query/set quota syscalls             | T12 §10                 |  [ ]   |
| ⭐   |   9   | Resource pressure events and recovery hooks | T16 §2, T30 §6          |  [ ]   |
| 💎   |  10   | Tests, leak sweeps, and dashboards          | §1..§9                  |  [ ]   |

## 1. Resource Type Registry

- [ ] Define resource types: handles, object bodies, namespace entries, paged pool, nonpaged pool, registry bytes, ALPC messages, notification states, timers, threads, processes, sections, mapped views, crash buffers.
- [ ] Each type includes accounting unit, default limit, privilege override, and human-readable name.
- [ ] Register types during Phase 2 before subsystem creation.
- [ ] Commit: quota: resource type registry with per-type unit/limit/name.

**Test checkpoint:** `quota_dump()` at boot lists all registered resource types with name, accounting unit, and default limit; registration completes in Phase 2 before the first subsystem creation (serial ordering shows the registry line before Object Manager init).

---

## 2. Quota Block and Charge API

- [ ] Define `quota_block_t`: refcount, owner SID, limits[], usage[], peaks[], failures[].
- [ ] Add `quota_charge(block, type, amount)`, `quota_return(block, type, amount)`, `quota_try_transfer`.
- [ ] Charges are atomic and rollback-safe.
- [ ] All failed charges return `STATUS_QUOTA_EXCEEDED` or a more specific NTSTATUS.
- [ ] Commit: quota: quota_block_t + atomic charge/return/transfer API.

**Test checkpoint:** a charge/return round-trip leaves `usage` and `peak` correct; an over-limit charge returns `STATUS_QUOTA_EXCEEDED` and leaves `usage` unchanged; N concurrent charges from multiple CPUs sum exactly (no lost updates) and a mid-sequence failure rolls back with no residual.

---

## 3. Process/Token/Job Ownership Model

- [ ] Add quota block pointer to `ACCESS_TOKEN`.
- [ ] Add effective quota block pointer to process/task.
- [ ] Job Objects may impose lower aggregate limits; a charge must pass both process and job quota.
- [ ] Token impersonation uses the thread effective token for security checks but process/job quota for resource charges unless API requires client charging.
- [ ] Commit: quota: token/process/job ownership and effective-block resolution.

**Test checkpoint:** a freshly created process has a non-NULL effective quota block inherited from its token; a charge that passes the process limit but exceeds the owning Job Object's aggregate limit fails; an impersonating thread charges process/job quota (not the client token) unless the API opts into client charging.

---

## 4. Object and Handle Quota Integration

- [ ] Charge handle table entries on insert, return on close.
- [ ] Charge object body and name entry on object creation.
- [ ] Object Manager exposes per-type usage from quota counters, not local-only counters.
- [ ] DuplicateHandle checks target quota before inserting.
- [ ] Commit: quota: object/handle charge integration in Object Manager.

**Test checkpoint:** inserting a handle increments the handle-type usage and closing it returns the charge; the Object Manager per-type usage query matches the quota counter exactly; `NtDuplicateObject` into a target process at its handle cap fails with `STATUS_QUOTA_EXCEEDED` and inserts no handle.

---

## 5. Pool and Allocation Quota Integration

- [ ] Add optional quota owner to tagged allocations.
- [ ] Charge nonpaged/paged pool through allocator provider hooks.
- [ ] Ensure kernel-internal early boot allocations are charged to System.
- [ ] Refuse user-triggered unbounded allocation paths without quota owner.
- [ ] Commit: quota: paged/nonpaged pool charging via allocator hooks.

**Test checkpoint:** a tagged allocation with a quota owner charges that owner's paged/nonpaged usage and frees return it; early-boot allocations are attributed to the System quota block (never NULL-owner leaks); a user-triggered unbounded allocation path with no quota owner is refused rather than charged to System.

---

## 6. Registry, ALPC, Notification Quotas

- [ ] Registry: charge key/value names, data bytes, notification watchers, transactions.
- [ ] ALPC: charge queued messages, port sections, completion-list entries.
- [ ] Notifications: charge state objects, subscriptions, retained payload bytes.
- [ ] Default per-user caps are configurable through TODO-02.
- [ ] Commit: quota: registry/ALPC/notification subsystem charge points.

**Test checkpoint:** writing a registry value past the per-user data-byte cap fails with a quota status and leaves the hive unchanged; queuing an ALPC message past the port's message cap fails; retaining notification payload past the cap fails; each cap is read from the TODO-02 config surface (not a hardcoded constant).

---

## 7. CPU, I/O, and Wakeup Accounting

- [ ] Track per-process and per-job CPU time with user/kernel split.
- [ ] Track I/O operation count and bytes by read/write/control.
- [ ] Track wakeups/sec and timer creation rate for battery/health policy.
- [ ] Feed power and health policy without duplicating scheduler internals.
- [ ] Commit: quota: CPU/IO/wakeup accounting counters.

**Test checkpoint:** per-process CPU user/kernel split advances monotonically and the job aggregate equals the sum of member processes; I/O counters break down by read/write/control op and bytes; wakeup/sec and timer-creation-rate samples are readable by policy without re-reading scheduler-private state.

---

## 8. Native Query/Set Quota Syscalls

- [ ] Add `NtQueryQuotaInformationProcess`, `NtSetQuotaInformationProcess`.
- [ ] `ProcessQuotaLimits`: project `QUOTA_LIMITS` from the TODO-21 §9 `rlimits[]` + real VM/working-set counters; reconcile `RLIMIT_NOFILE` with `handle_table.handle_limit`. -> XREF: `TODO-21-process-model-extensions.md §9`
- [ ] Extend Job Object information classes for aggregate limits.
- [ ] Add `SystemResourcePressureInformation`.
- [ ] Require privilege for raising limits; lowering own soft limit is allowed.
- [ ] Commit: quota: NtQuery/SetQuotaInformationProcess + pressure info class.

**Test checkpoint:** `NtQueryQuotaInformationProcess(ProcessQuotaLimits)` returns a `QUOTA_LIMITS` projected from the TODO-21 §9 rlimits and live VM/working-set counters with `RLIMIT_NOFILE` reconciled to `handle_table.handle_limit`; raising a hard limit without privilege returns `STATUS_PRIVILEGE_NOT_HELD`; lowering the caller's own soft limit succeeds; `SystemResourcePressureInformation` is queryable from user mode.

---

## 9. Resource Pressure Events and Recovery Hooks

- [ ] Publish pressure levels through TODO-16: normal, watch, warning, critical.
- [ ] On critical pressure, notify TODO-30 recovery orchestrator before panicking.
- [ ] Trigger targeted cleanup: drain caches, trim logs, ask services to release memory, refuse new handles from offending process.
- [ ] Commit: quota: resource pressure levels + recovery hooks.

**Test checkpoint:** driving usage across thresholds publishes normal->watch->warning->critical transitions through the TODO-16 notification facility; reaching critical notifies the TODO-30 recovery orchestrator before any panic path; the targeted-cleanup hook drains caches and refuses new handles from the offending process while other processes are unaffected.

---

## 10. Tests, Leak Sweeps, and Dashboards

- [ ] Unit tests: charge/return, rollback, concurrent charges, duplicate handle quota, registry quota, ALPC quota.
- [ ] Boot leak sweep compares all quota blocks before/after test categories.
- [ ] Add `quota_dump()` for serial and crash dumps.
- [ ] Bulletproofing: every charge path must name a resource type and owner.
- [ ] Commit: quota: unit tests, boot leak sweep, quota_dump dashboard.

**Test checkpoint:** the quota unit suite passes (charge/return, rollback, concurrent charges, duplicate-handle quota, registry quota, ALPC quota); the boot leak sweep reports zero net quota delta across every test category; `quota_dump()` renders per-type usage/peak/limit on serial and in crash dumps.

---

## OS Comparison

| ⭐   | Feature                         | 🪟 Win11                               | 🐧 Linux                                     | 🚀 Impossible OS                            |
| --- | ------------------------------- | ------------------------------------- | ------------------------------------------- | ------------------------------------------ |
| 💎   | Central quota/charge API        | ✅ `PsChargeProcessQuota` per pool     | ⚠️ split: rlimits + cgroups, no unified API | 🚀 Planned: one `quota_charge`/`return` §2  |
| 💎   | Per-token quota block           | ✅ `EPROCESS`/token `QUOTA_BLOCK`      | ⬜ none (uid/cgroup based)                   | 🚀 Planned: token+process+job block §3      |
| 💎   | Handle/object quota             | ✅ per-process handle quota            | ⚠️ `RLIMIT_NOFILE` fd-only                  | 🚀 Planned: handle+object body charge §4    |
| 💎   | Paged/nonpaged pool quota       | ✅ pool quota per process              | ⚠️ slab accounting via memcg, not per-proc  | 🚀 Planned: allocator-hook charging §5      |
| 💎   | Registry/IPC quota              | ✅ registry + ALPC quotas              | ⚠️ no registry; IPC via `RLIMIT_MSGQUEUE`   | 🚀 Planned: registry/ALPC/notif caps §6     |
| ⭐   | CPU/IO/wakeup accounting        | ✅ Job Objects + power throttling      | ✅ cgroup cpu/io/pids controllers            | 🚀 Planned: per-proc/job split counters §7  |
| 💎   | Native query/set quota syscalls | ✅ `NtQueryInformationProcess` classes | ✅ `getrlimit`/`prlimit64`                   | 🚀 Planned: Nt{Query,Set}Quota + rlimits §8 |
| ⭐   | Resource pressure events        | ✅ low-memory notifications            | ✅ PSI (`/proc/pressure/*`)                  | 🚀 Planned: 4-level pressure + recovery §9  |
| ⭐   | Unified leak sweep + quota_dump | ⚠️ pool-tag tracking, no boot sweep   | ⚠️ slabinfo, no per-boot delta sweep        | 🚀 Planned: boot delta sweep + dump §10     |

---

## Unit Tests

> Test file: `src/kernel/test/test_quota.c`, registered via `test_suite_register_cat(...)` in `test_runner_init()`. Core charge/pool assertions land under `TEST_CAT_MM` (quota accounting is pool-anchored and its bat already exists); object/handle-quota and security-quota assertions may extend `TEST_CAT_OB` / `TEST_CAT_SECURITY` respectively when §4/§3 integrations land. Use `TEST_PENDING` for assertions gated on a not-yet-shipped section.

- [ ] `test_quota_charge_return_roundtrip`: charge then return leaves usage 0 and peak recorded (§2).
- [ ] `test_quota_over_limit_rejected`: over-limit charge returns `STATUS_QUOTA_EXCEEDED`, usage unchanged (§2).
- [ ] `test_quota_concurrent_charges_atomic`: N parallel charges sum exactly, no lost update (§2).
- [ ] `test_quota_rollback_on_partial_failure`: a failing charge in a batch rolls back cleanly (§2).
- [ ] `test_quota_type_registry`: all resource types registered with name/unit/limit (§1).
- [ ] `test_quota_process_job_double_check`: charge passes process but fails on job aggregate (§3).
- [ ] `test_quota_handle_insert_close`: handle insert/close increments/decrements handle usage (§4).
- [ ] `test_quota_duplicate_handle_target_cap`: DuplicateHandle into a capped target fails (§4).
- [ ] `test_quota_pool_owner_charged`: tagged pool alloc charges its quota owner; free returns it (§5).
- [ ] `test_quota_registry_data_cap`: registry value over data-byte cap rejected (§6).
- [ ] `test_quota_leak_sweep_zero_delta`: boot leak sweep shows zero net quota delta (§10).

**Test checkpoint:** all `test_quota_*` cases pass under the kernel runner; pending assertions render `[STUB]` via `TEST_PENDING` until their owning section ships; the boot leak sweep prints a zero-delta line for the quota category.

---

## Verification

- [ ] `bash scripts/build.sh` -> `=== BUILD OK ===`.
- [ ] `bash scripts/test.sh SUITE=mm QUIET=1` -> quota suites pass, 0 failures.
- [ ] `grep -R "quota_charge" src/kernel` shows every charge path names a resource type and owner.
- [ ] `quota_dump()` output appears in the boot serial log and in a forced crash dump.
- [ ] Verify on bare metal -- VM behavior differs for pool/working-set counters.

**Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) | N suites, 0 failures

