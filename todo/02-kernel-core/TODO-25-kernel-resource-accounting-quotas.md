---
schema_version: 1
id: kernel-resource-accounting-quotas
domain: 02-kernel-core
status: active
title: "TODO-25 -- Kernel Resource Accounting & Quotas"
---

# TODO-25 -- Kernel Resource Accounting & Quotas

> **Validated:** 2026-07-19 | validate-todo-file clean (structure / IO table / XREF / test wiring)
> **Gap-audited:** 2026-07-19 | gap-audit + codex-gap-audit (5 findings verified); filed nested-job chain-charge, inheritance-on-create, per-SID rollup, rate-limit records + CPU/IO enforcement XREFs, PSI stall-time telemetry, quota-failure event contract, OOM victim-select policy, storage-provider XREF, working-set-trim recovery

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
- → XREF: [`TODO-16-kernel-notification-facility.md`](./TODO-16-kernel-notification-facility.md) -- pressure publish + quota-event transport
- → XREF: [`05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md`](../05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md) -- storage/volume quota provider (§12)

## Outcome

- Every chargeable kernel resource has a type, owner, current usage, peak usage, hard limit, and failure status.
- Processes inherit quota blocks from tokens/jobs and can be constrained independently; charges walk the whole nested-job chain.
- Job Objects enforce aggregate process, memory, handle, CPU, and notification limits.
- Quota failures return NTSTATUS-compatible codes and emit a contract-defined diagnostic event (rate-limited, drop-counted).
- System resource pressure is derived from per-resource stall-time telemetry and queryable from user mode for health/recovery policy.

## Implementation Order

| ⭐   | Order | Deliverable                                 | Depends On              | Status |
| --- | :---: | ------------------------------------------- | ----------------------- | :----: |
| 💎   |   1   | Resource type registry                      | --                      |  [x]   |
| 💎   |   2   | Quota block and charge API                  | §1                      |  [x]   |
| 💎   |   3   | Process/token/job ownership model           | T21 §9, T15 §4          |  [x]   |
| 💎   |   4   | Object and handle quota integration         | T05 §14                 |  [ ]   |
| 💎   |   5   | Pool and allocation quota integration       | D03T03 §6,§7            |  [ ]   |
| 💎   |   6   | Registry, ALPC, notification quotas         | T24 §6, T14 §15, T16 §7 |  [ ]   |
| ⭐   |   7   | CPU, I/O, and wakeup accounting             | T08 §6, T21 §9          |  [ ]   |
| 💎   |   8   | Native query/set quota syscalls             | T12 §10                 |  [ ]   |
| ⭐   |   9   | Resource pressure events and recovery hooks | T16 §2, T30 §6          |  [ ]   |
| 💎   |  10   | Tests, leak sweeps, and dashboards          | §1..§9                  |  [ ]   |

## 1. Resource Type Registry

- [x] Define resource types: handles, object bodies, namespace entries, paged pool, nonpaged pool, registry bytes, ALPC messages, notification states, timers, threads, processes, sections, mapped views, crash buffers (`quota_resource_type_t`).
- [x] Each type has accounting unit (COUNT/BYTES), default limit (0=unlimited), override privilege, and name in a static const `quota_resource_desc_t` table; `quota_register_types` validates + halts on drift.
- [x] Validate + register at Phase 2 entry, before SMP/storage/VFS and any quota consumer: `quota_register_types()` + `quota_types_dump()` at the top of `boot_phase2()` (`POST16_QUOTA`), halts boot on a malformed table.
- [x] Persistent storage/volume quota is provider-owned, not a scalar central type (a unified view needs (resource,volume,owner) keying + SID<->uid map -- defer). -> XREF: `05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §12`.
- [x] Commit: quota: resource type registry with per-type unit/limit/name.

**Test checkpoint:** in a test boot (`test=1`) `quota_types_dump()` lists all 14 registered types with name, unit, and default limit (verified: SUITE=quota serial shows the 14-row dump); production boots emit only the one-line validated-count summary; `test=1` shows 9 Quota suites / 211 assertions, 0 failures; smoke boots to `C:\>`.

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 9 suites, 0 failures

> **Notes:**
> - **What shipped** -- `quota.h` + `quota.c`: 14-type static const registry (name/unit/limit/privilege) + bounds-checked accessors + `quota_types_dump`; new `TEST_CAT_QUOTA` wired end to end.
> - **How it runs** -- `quota_register_types()` validates name/unit/privilege/uniqueness at Phase 2 entry, halts boot on drift; `quota_types_dump()` emits the 14-row table to serial; reads are lock-free (const table + release/acquire flag).
> - **Downstream effects** -- the taxonomy is the single source of truth every later section (charge API §2 onward) keys off; no section may invent a type outside this table.
> - **Canonical doc** -- `include/kernel/quota/quota.h` (design invariants).
> - **Scope boundary** -- §1 owns the taxonomy only; concrete numeric caps are TODO-02 config policy (§6); persistent storage/volume quota is provider-owned (TODO-07 §12).
> **Verified:** 2026-07-19 | commit `0df5e3ee` | 5/5 items | build OK | smoke PASS (TCG 2.9s), tests 9/9 PASS
> **Quality reviewed:** 2026-07-19 | Codex 13x (design, adversarial, consistency, perf) | 8M+1L fixed, 0 open | scope: kernel-code-quality

---

## 2. Quota Block and Charge API

- [x] `quota_block_t`, opaque (defined in `quota.c` so no caller can take its lock): `atomic_t` refcount, embedded copied owner SID, per-type `limit/usage/peak/failures` arrays, one `lock` guarding every mutation.
- [x] `quota_charge` / `quota_return` / `quota_try_transfer` in `quota.c`, plus `quota_block_create/ref/deref/owner`, `quota_set_limit`, and `quota_usage/peak/failures/limit` queries.
- [x] Charges are atomic and rollback-safe: limit check and commit share ONE critical section, so a refused charge leaves `usage` byte-identical; transfer holds both block locks in address order and is all-or-nothing.
- [x] Failures return `STATUS_QUOTA_EXCEEDED` (over cap / source short), `STATUS_INTEGER_OVERFLOW` (outside the 0..`QUOTA_AMOUNT_MAX` domain, over-return, or corruption), or `STATUS_INVALID_PARAMETER`; every refusal bumps a saturating counter.
- [x] Diagnostics are recorded under the lock, emitted after it, gated to PASSIVE_LEVEL, with suppressed events counted. -> XREF: `TODO-32-kernel-logging-v2-lockless.md §6` (klog live-disk flush re-enters VFS).
- [x] Commit: quota: quota_block_t + atomic charge/return/transfer API.

**Test checkpoint:** a charge/return round-trip leaves `usage` at 0 and `peak` at the high-water mark; an over-limit charge returns `STATUS_QUOTA_EXCEEDED` and leaves `usage` unchanged; the counter domain is enforced at both ends (oversized amount rejected, `+1` past the max overflows instead of wrapping, an over-return is REFUSED so a stale duplicate return cannot erase a newer live charge); a refused transfer moves no usage in either block; interleaved multi-thread charges sum exactly with no lost updates (verified: SUITE=quota 27 suites / 400 assertions, 0 failures).

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 27 suites, 0 failures

> **Notes:**
> - **What shipped** -- `quota_block_t` + the charge API in `quota.h`/`quota.c`: create/ref/deref, charge/return/transfer, limit set, and four queries over the 14-type taxonomy from §1.
> - **How it runs** -- one per-block spinlock (`spin_lock_irqsave`) makes each operation's several counter updates indivisible; transfer takes both locks in `uintptr_t` address order; queries stay lock-free atomic reads; over-returns fail closed rather than clamping.
> - **Downstream effects** -- §3 wires blocks to tokens/processes/jobs and owns safe publication for the caller-holds-a-reference contract; §4-§7 charge through this API; Codex review adoptions in the commit message.
> - **Canonical doc** -- `include/kernel/quota/quota.h` (counter domain, lifetime, limit-lowering, and transfer-visibility contracts).
> - **Scope boundary** -- §2 owns the mechanism only: no subsystem is charged yet (§4-§7), per-CPU batching for hot paths is §5, and cross-CPU contention test infrastructure is §10.
> **Verified:** 2026-07-19 | commit `860c95ba` | 6/6 items | build OK | smoke PASS (TCG 2.75s), tests 27/27 PASS
> **Accepted:** [M] counter layout: four per-type arrays cost ~3 cache lines per charge; an array-of-records is the candidate, but no consumer exists to benchmark against yet (reason: not-functional-today) -> XREF: `02-kernel-core/TODO-25 §5` (item: "Benchmark contended pool charging" at line 154)
> **Accepted:** [M] klog's live-disk path re-enters VFS, so a PASSIVE caller holding a storage lock can deadlock; kernel-wide, mitigated here by the PASSIVE_LEVEL gate + deferred count (reason: scope) -> XREF: `02-kernel-core/TODO-32 §6` (item: "`klog_v2()` enqueue must never synchronously enter the live-disk flush" at line 156)
> **Deferred:** [M] cross-CPU contention proof: the scheduler is single-CPU today, so the multi-thread suites show interleaving, not parallel contention (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path" at line 203)
> **Deferred:** [L] bounded worker join + CPU-pinned fault injection for the quota tests (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Bounded worker join + CPU-pinned `kmalloc_fail_next`" at line 204)
> **Quality reviewed:** 2026-07-19 | Codex 12x (design, adversarial, re-adversarial, test-coverage, consistency, perf) | 14H+9M+8L fixed, 3M+1L accepted-XREF | scope: kernel-code-quality

---

## 3. Process/Token/Job Ownership Model

- [x] `ACCESS_TOKEN.QuotaBlock`: the CANONICAL per-SID `QUOTA_PRINCIPAL_USER` block from `quota_user_block_acquire`, shared by every token for that SID (duplicates take a reference), released in `token_on_delete`.
- [x] `task->quota` (own `QUOTA_PRINCIPAL_PROCESS` block) + `task->quota_user` (shared), published and cleared under `task->quota_lock`: load-and-reference must be INSIDE that lock, a release store cannot make it teardown-safe.
- [/] `quota_charge_chain` charges process + user + job all-or-nothing (ref-held snapshot, one lock at a time, prefix rollback); ANCESTOR jobs need nesting. -> XREF: `TODO-21-process-model-extensions.md §13` (item: "Nested-job topology").
- [/] Inheritance: child gets its own process block and SHARES the parent user block; teardown mirrors `ob_job_detach_task`; job BREAKAWAY unimplemented. -> XREF: `TODO-21-process-model-extensions.md §13` (item: "Nested-job topology").
- [x] `quota_rollup_by_sid` aggregates by owner SID over the USER layer ONLY: summing every layer would multiply each chain charge by its depth, and a job's owner is its creator, not its members.
- [/] Charges use the process chain, never the thread impersonation token. `QUOTA_CHARGE_CLIENT` is defined but fails closed with `STATUS_NOT_SUPPORTED`: client billing needs an SMP-safe token pin. -> XREF: `TODO-15-security-reference-monitor.md §4`
- [x] Commit: quota: token/process/job-chain ownership, inheritance, per-SID rollup.

**Test checkpoint:** every live task (including PID 0) carries a non-NULL process block plus the shared user block; a chain charge lands on both and its receipt returns exactly those blocks; a charge refused by a later block in the chain rolls the charged prefix back so no usage is stranded; a repeated `quota_return_chain` cannot erase a newer charge; one SID always resolves to one canonical user block while a different SID does not collide; a per-SID rollup counts a chain charge ONCE (verified: SUITE=quota 52 suites / 500 assertions, 0 failures).

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 52 suites, 0 failures

> **Notes:**
> - **What shipped** -- `quota_owner.c` (ownership + chain charge) plus a canonical per-SID USER-block registry, `quota_principal_t`, `quota_block_try_ref`, `quota_user_block_acquire`, and `quota_rollup_by_sid` in `quota.c`.
> - **How it runs** -- blocks are created beside job inheritance on the task-creation paths (fail-closed, unwound on later failure) and released in `task_death_teardown`; the chain is snapshotted under owner locks, then charged one lock at a time.
> - **Downstream effects** -- §4-§7 charge through `quota_charge_chain` and hold its receipt; ancestor-job depth and breakaway wait on TODO-21 §13; Codex adoptions are in the commit message.
> - **Canonical doc** -- `include/kernel/quota/quota.h` (chain-charging contract, principal kinds, canonical-user rule).
> - **Scope boundary** -- §3 owns WHO is charged; nothing is charged yet (§4-§7), privilege-checked limits are §8, nested-job topology is TODO-21 §13.
> **Verified:** 2026-07-20 | commit `20ee7781` + review fixes | 4/6 items | build OK | smoke PASS (TCG 2.7s), tests 52/52 PASS
> **Accepted:** [H] absorbed pre-join usage stays billed to the job until the member departs, and a charge can straddle the absorb-to-publish window; both need a per-task live-receipt registry plus a membership generation (reason: not-functional-today) -> XREF: `02-kernel-core/TODO-25 §4` (item: "Migrate outstanding receipt obligations at `ob_job_assign`" at line 140)
> **Accepted:** [M] a charge+return lifetime costs 7-9 irqsave lock sections; needs a latency budget before the first hot-path consumer (reason: not-functional-today) -> XREF: `02-kernel-core/TODO-25 §5` (item: "Benchmark contended pool charging under a defined latency budget" at line 154)
> **Accepted:** [M] `ACCESS_TOKEN.UserSid` has no recorded extent, so the bounded-capture `owner_len` is caller-derived and cannot detect a truncated SID (reason: scope) -> XREF: `02-kernel-core/TODO-15 §4` (item: "Record a VALIDATED `UserSid` length in `ACCESS_TOKEN`" at line 334)
> **Deferred:** [M] `QUOTA_CHARGE_CLIENT` fails closed with `STATUS_NOT_SUPPORTED`: billing an impersonated client needs a stable per-CPU current-thread cursor and a teardown-safe token-slot pin (reason: infra) -> XREF: `02-kernel-core/TODO-15 §4` (item: "Teardown-safe primary-token READ pin" at line 333)
> **Accepted:** [M] `ob_job_create` inserts a named job into the object namespace before allocating its handle, so a handle-alloc failure leaks the directory entry, the body, and now its quota block (reason: scope, pre-existing Job-Object lifecycle) -> XREF: `02-kernel-core/TODO-21 §14` (item: "`ob_job_create` inserts a named job into `\BaseNamedObjects`" at line 463)
> **Accepted:** [H] two concurrent task constructors can claim the same slot and reset a live lock word; pre-existing and systemic across every per-process inheritance, not introduced here (reason: scope) -> XREF: `03-memory-concurrency/TODO-06 §13` (item: "Atomic task-slot CLAIM" at line 359)
> **Quality reviewed:** 2026-07-20 | Codex 14x (design, adversarial, re-adversarial, consistency, perf, test-coverage) | 12H+13M+4L fixed, 1H+3M accepted-XREF | scope: kernel-code-quality

---

## 4. Object and Handle Quota Integration

- [ ] Charge handle table entries on insert, return on close.
- [ ] Charge object body and name entry on object creation.
- [ ] Object Manager exposes per-type usage from quota counters, not local-only counters.
- [ ] DuplicateHandle checks target quota before inserting.
- [ ] Migrate outstanding receipt obligations at `ob_job_assign` (per-task live-receipt registry) AND close the absorb-to-publish window with a membership generation under `job_lock`. BOTH must land before the first charging consumer. -> XREF: `§3`
- [ ] Add a caller-held generation token to `quota_charge_receipt_t` so recycled receipt STORAGE cannot let a stale returner credit back a later charge; §3 documents the no-recycle rule instead. -> XREF: `§3` (chain charge)
- [ ] Commit: quota: object/handle charge integration in Object Manager.

**Test checkpoint:** inserting a handle increments the handle-type usage and closing it returns the charge; the Object Manager per-type usage query matches the quota counter exactly; `NtDuplicateObject` into a target process at its handle cap fails with `STATUS_QUOTA_EXCEEDED` and inserts no handle.

---

## 5. Pool and Allocation Quota Integration

- [ ] Add optional quota owner to tagged allocations.
- [ ] Charge nonpaged/paged pool through allocator provider hooks.
- [ ] Ensure kernel-internal early boot allocations are charged to System.
- [ ] Refuse user-triggered unbounded allocation paths without quota owner.
- [ ] Benchmark contended pool charging under a defined latency budget BEFORE the first hot-path consumer: a charge+return lifetime currently costs 7-9 irqsave lock sections. Then decide the `quota_block_t` counter layout. -> XREF: `§2` (charge API)
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
- [ ] Define a rate-limit record (weight, min/max, hard-cap, reservation) distinct from cumulative usage; the quota block carries both. Enforcement CONSUMES the record; this section OWNS it.
- [ ] CPU rate enforcement (throttle/weight) is owned by the scheduler. -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §3` (item: "prio_to_weight table").
- [ ] Block-I/O rate enforcement (IOPS/BPS cap) has no owner yet; §7 defines the I/O rate-limit record only. A block-I/O QoS owner must be filed before I/O throttling is claimed.
- [ ] Feed power and health policy without duplicating scheduler internals.
- [ ] Commit: quota: CPU/IO/wakeup accounting + rate-limit records (measurement).

**Test checkpoint:** per-process CPU user/kernel split advances monotonically and the job aggregate equals the sum of member processes; I/O counters break down by read/write/control op and bytes; the rate-limit record stores weight/min/max/hard-cap/reservation separately from usage; wakeup/sec and timer-creation-rate samples are readable by policy without re-reading scheduler-private state.

---

## 8. Native Query/Set Quota Syscalls

- [ ] Add `NtQueryQuotaInformationProcess`, `NtSetQuotaInformationProcess`.
- [ ] `ProcessQuotaLimits`: project `QUOTA_LIMITS` from the TODO-21 §9 `rlimits[]` + real VM/working-set counters; reconcile `RLIMIT_NOFILE` with `handle_table.handle_limit`. -> XREF: `TODO-21-process-model-extensions.md §9`
- [ ] Extend Job Object information classes for aggregate limits.
- [ ] Add `SystemResourcePressureInformation`: per-resource (cpu/mem/io) stall-time totals + rolling windows (some/full, avg10/60/300 style), not a single ratio; §9 derives the 4 levels from these.
- [ ] Require privilege for raising limits; lowering own soft limit is allowed.
- [ ] Token-local limit overlay so a RESTRICTED token can be capped tighter than the user block it shares, without lowering the unrestricted parent's budget. -> XREF: `§3` (canonical user block)
- [ ] Commit: quota: NtQuery/SetQuotaInformationProcess + pressure info class.

**Test checkpoint:** `NtQueryQuotaInformationProcess(ProcessQuotaLimits)` returns a `QUOTA_LIMITS` projected from the TODO-21 §9 rlimits and live VM/working-set counters with `RLIMIT_NOFILE` reconciled to `handle_table.handle_limit`; raising a hard limit without privilege returns `STATUS_PRIVILEGE_NOT_HELD`; lowering the caller's own soft limit succeeds; `SystemResourcePressureInformation` returns per-resource some/full stall-time and rolling-window fields (not a lone ratio).

---

## 9. Resource Pressure Events and Recovery Hooks

- [ ] Derive the 4 levels (normal/watch/warning/critical) from §8 stall-time metrics with hysteresis to prevent flapping; publish level transitions through TODO-16.
- [ ] On critical pressure, notify TODO-30 recovery orchestrator before panicking.
- [ ] Define the quota-failure diagnostic event contract HERE: event ID, process/job/SID/resource, requested/current/limit, result, rate-limit + dropped-event counter. -> XREF: `TODO-16-kernel-notification-facility.md §6` (transport/fanout only).
- [ ] Trigger targeted cleanup: drain caches, trim logs, trim offending process working set (§8 Min/Max WS), ask services to release memory, refuse new handles from the offending process.
- [ ] Last-resort escalation once softer recovery fails: victim-selection policy (highest over-limit ratio, lowest priority, System-protected flag). ARCHITECTURE DECISION: in-kernel termination vs cooperative-only (Win-style).
- [ ] Commit: quota: pressure levels (hysteresis), quota-failure events, recovery hooks.

**Test checkpoint:** driving usage across thresholds publishes hysteresis-damped normal->watch->warning->critical transitions (derived from §8 stall-time, no flapping) through the TODO-16 notification facility; a per-process quota failure emits the diagnostic event with the full contract fields; reaching critical notifies the TODO-30 recovery orchestrator before any panic path; targeted cleanup trims the offending process working set and refuses its new handles while other processes are unaffected.

---

## 10. Tests, Leak Sweeps, and Dashboards

- [ ] Unit tests: charge/return, rollback, concurrent charges, duplicate handle quota, registry quota, ALPC quota.
- [ ] Boot leak sweep compares all quota blocks before/after test categories.
- [ ] Add `quota_dump()` for serial and crash dumps.
- [ ] Bulletproofing: every charge path must name a resource type and owner.
- [ ] Cross-CPU contention proof for the §2 charge path: needs per-CPU run queues plus an operation-level checkpoint, so single-CPU interleaving cannot show the lock is load-bearing. -> XREF: `§2` (charge API)
- [ ] Bounded worker join + CPU-pinned `kmalloc_fail_next` for quota tests: `thread_join` has no timeout and the injection countdown is per-CPU. -> XREF: `§2` (charge API)
- [ ] Commit: quota: unit tests, boot leak sweep, quota_dump dashboard.

**Test checkpoint:** the quota unit suite passes (charge/return, rollback, concurrent charges, duplicate-handle quota, registry quota, ALPC quota); the boot leak sweep reports zero net quota delta across every test category; `quota_dump()` renders per-type usage/peak/limit on serial and in crash dumps.

---

## OS Comparison

| ⭐   | Feature                         | 🪟 Win11                               | 🐧 Linux                                     | 🚀 Impossible OS                            |
| --- | ------------------------------- | ------------------------------------- | ------------------------------------------- | ------------------------------------------ |
| 💎   | Unified resource-type registry  | ⚠️ scattered across subsystems        | ⚠️ split rlimit/cgroup/quotactl             | ✅ one 14-type registry §1                  |
| 💎   | Central quota/charge API        | ✅ `PsChargeProcessQuota` per pool     | ⚠️ split: rlimits + cgroups, no unified API | ✅ one `quota_charge`/`return` §2           |
| 💎   | Atomic quota transfer           | ⬜ none (charge/return only)           | ⬜ none (no cross-principal move)            | ✅ all-or-nothing two-block transfer §2     |
| 💎   | Per-type peak + failure counts  | ⚠️ peak only, no per-type failures    | ⚠️ `memory.events` per-cgroup, not per-type | ✅ peak + saturating failures per type §2   |
| 💎   | Per-token quota block           | ✅ `EPROCESS`/token `QUOTA_BLOCK`      | ⬜ none (uid/cgroup based)                   | ✅ token+process+job blocks §3              |
| 💎   | All-or-nothing chain charge     | ⚠️ per-block, no cross-layer rollback | ⚠️ per-cgroup, no receipt for the return    | ✅ receipt-bound chain charge §3            |
| ⭐   | Per-user aggregate rollup       | ⚠️ per-process/job, no per-SID view   | ⚠️ per-cgroup, not per-uid across cgroups   | ✅ canonical per-SID block + rollup §3      |
| 💎   | Handle/object quota             | ✅ per-process handle quota            | ⚠️ `RLIMIT_NOFILE` fd-only                  | 🚀 Planned: handle+object body charge §4    |
| 💎   | Paged/nonpaged pool quota       | ✅ pool quota per process              | ⚠️ slab accounting via memcg, not per-proc  | 🚀 Planned: allocator-hook charging §5      |
| 💎   | Registry/IPC quota              | ✅ registry + ALPC quotas              | ⚠️ no registry; IPC via `RLIMIT_MSGQUEUE`   | 🚀 Planned: registry/ALPC/notif caps §6     |
| ⭐   | CPU/IO/wakeup accounting        | ✅ Job Objects + power throttling      | ✅ cgroup cpu/io/pids controllers            | 🚀 Planned: per-proc/job split counters §7  |
| 💎   | Native query/set quota syscalls | ✅ `NtQueryInformationProcess` classes | ✅ `getrlimit`/`prlimit64`                   | 🚀 Planned: Nt{Query,Set}Quota + rlimits §8 |
| ⭐   | Resource pressure events        | ✅ low-memory notifications            | ✅ PSI (`/proc/pressure/*`)                  | 🚀 Planned: stall-time-derived 4-level §9   |
| ⭐   | Last-resort OOM recovery        | ⬜ none (cooperative trim only)        | ✅ cgroup `memory.oom.group`                 | 🚀 Planned: victim-select policy §9         |
| ⭐   | Unified leak sweep + quota_dump | ⚠️ pool-tag tracking, no boot sweep   | ⚠️ slabinfo, no per-boot delta sweep        | 🚀 Planned: boot delta sweep + dump §10     |

---

## Unit Tests

> Test file: `src/kernel/test/test_quota.c`, registered via `test_register_quota()` in `test_runner_init()`. All quota assertions land under the dedicated `TEST_CAT_QUOTA` category (run via `SUITE=quota`). Use `TEST_PENDING` for assertions gated on a not-yet-shipped section.

- [x] `test_quota_charge_return_roundtrip`: charge then return leaves usage 0 and peak recorded (§2).
- [x] `test_quota_over_limit_rejected`: over-limit charge returns `STATUS_QUOTA_EXCEEDED`, usage unchanged (§2).
- [/] `test_quota_concurrent_charges_atomic`: N INTERLEAVED multi-thread charges sum exactly, no lost update (§2). True cross-CPU contention needs per-CPU run queues. -> XREF: `§10` (test infrastructure).
- [x] `test_quota_rollback_on_partial_failure`: a refused transfer leaves both blocks byte-identical; a refused charge leaves usage unchanged (§2).
- [ ] `test_quota_type_registry`: all resource types registered with name/unit/limit (§1).
- [x] `test_quota_chain_all_or_nothing`: a charge admitted by the process block but refused later in the chain rolls the prefix back, stranding no usage (§3).
- [ ] `test_quota_job_chain_rollback`: charge exceeding an ancestor job rolls back across the whole chain (§3).
- [ ] `test_quota_inherit_on_create`: child auto-joins parent job chain unless breakaway; jobless child charges own block (§3).
- [x] `test_quota_rollup_counts_user_layer_once`: the per-SID query aggregates the USER layer only, so a chain charge is counted once rather than per layer (§3).
- [ ] `test_quota_pressure_hysteresis`: levels derive from stall-time metrics and do not flap at a threshold (§8/§9).
- [ ] `test_quota_failure_event_fields`: a quota failure emits the diagnostic event with all contract fields, rate-limited (§9).
- [ ] `test_quota_handle_insert_close`: handle insert/close increments/decrements handle usage (§4).
- [ ] `test_quota_duplicate_handle_target_cap`: DuplicateHandle into a capped target fails (§4).
- [ ] `test_quota_pool_owner_charged`: tagged pool alloc charges its quota owner; free returns it (§5).
- [ ] `test_quota_registry_data_cap`: registry value over data-byte cap rejected (§6).
- [ ] `test_quota_leak_sweep_zero_delta`: boot leak sweep shows zero net quota delta (§10).

**Test checkpoint:** all `test_quota_*` cases pass under the kernel runner; pending assertions render `[STUB]` via `TEST_PENDING` until their owning section ships; the boot leak sweep prints a zero-delta line for the quota category.

---

## Verification

- [ ] `bash scripts/build.sh` -> `=== BUILD OK ===`.
- [ ] `bash scripts/test.sh SUITE=quota QUIET=1` -> quota suites pass, 0 failures.
- [ ] `grep -R "quota_charge" src/kernel` shows every charge path names a resource type and owner.
- [ ] `quota_dump()` output appears in the boot serial log and in a forced crash dump.
- [ ] Verify on bare metal -- VM behavior differs for pool/working-set counters.

**Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 9 suites, 0 failures
