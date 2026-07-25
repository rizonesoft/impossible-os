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

| ⭐   | Order | Deliverable                                   | Depends On              | Status |
| --- | :---: | --------------------------------------------- | ----------------------- | :----: |
| 💎   |   1   | Resource type registry                        | --                      |  [x]   |
| 💎   |   2   | Quota block and charge API                    | §1                      |  [x]   |
| 💎   |   3   | Process/token/job ownership model             | T21 §9, T15 §4          |  [x]   |
| 💎   |   4   | Receipt identity (generation-tokened charges) | §3                      |  [x]   |
| 💎   |   5   | Pool and allocation quota integration         | D03T03 §6,§7            |  [/]   |
| 💎   |   6   | Registry, ALPC, notification quotas           | T24 §6, T14 §15, T16 §7 |  [/]   |
| ⭐   |   7   | CPU, I/O, and wakeup accounting               | T08 §6, T21 §9          |  [x]   |
| 💎   |   8   | Native query/set quota syscalls               | T12 §10                 |  [x]   |
| ⭐   |   9   | Resource pressure events and recovery hooks   | T16 §2, T30 §6          |  [/]   |
| 💎   |  10   | Tests, leak sweeps, and dashboards            | §1..§9                  |  [/]   |
| 💎   |  11   | Charge ledger and transactional adjustment    | §4                      |  [x]   |
| ⭐   |  12   | Resource pressure stall telemetry             | D03T07 §3, D03T03 §2    |  [/]   |
| 💎   |  13   | Object Manager charge points                  | §11, T05 §3, T05 §14    |  [/]   |
| ⭐   |  14   | Charge-path cost and lifetime follow-ups      | §6, §7, §9              |  [ ]   |

## 1. Resource Type Registry

- [x] Define resource types: handles, object bodies, namespace entries, paged/nonpaged pool, registry bytes, ALPC messages, notification states, timers, threads, processes, sections, mapped views, crash buffers (14; §6 appended 2 more).
- [x] Each type has accounting unit (COUNT/BYTES), default limit (0=unlimited), override privilege, and name in a static const `quota_resource_desc_t` table; `quota_register_types` validates + halts on drift.
- [x] Validate + register at Phase 2 entry, before SMP/storage/VFS and any quota consumer: `quota_register_types()` + `quota_types_dump()` at the top of `boot_phase2()` (`POST16_QUOTA`), halts boot on a malformed table.
- [x] Persistent storage/volume quota is provider-owned, not a scalar central type (a unified view needs (resource,volume,owner) keying + SID<->uid map -- defer). -> XREF: `05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §12`.
- [x] Commit: quota: resource type registry with per-type unit/limit/name.

**Test checkpoint:** in a test boot (`test=1`) `quota_types_dump()` lists every registered type with name, unit, and default limit (verified at §1: SUITE=quota serial shows the 14-row dump; §6 appended 2 types, so the dump is 16 rows today); production boots emit only the one-line validated-count summary; `test=1` showed 9 Quota suites / 211 assertions, 0 failures; smoke boots to `C:\>`.

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 9 suites, 0 failures

> **Notes:**
> - **What shipped** -- `quota.h` + `quota.c`: static const type registry (name/unit/limit/privilege, 14 types at §1) + bounds-checked accessors + `quota_types_dump`; new `TEST_CAT_QUOTA` wired end to end.
> - **How it runs** -- `quota_register_types()` validates name/unit/privilege/uniqueness at Phase 2 entry, halts boot on drift; `quota_types_dump()` emits the whole table to serial; reads are lock-free (const table + release/acquire flag).
> - **Downstream effects** -- the taxonomy is the single source of truth every later section (charge API §2 onward) keys off; no section may invent a type outside this table.
> - **Canonical doc** -- `include/kernel/quota/quota.h` (design invariants).
> - **Scope boundary** -- §1 owns the taxonomy only; concrete numeric caps are TODO-02 config policy (§6); persistent storage/volume quota is provider-owned (TODO-07 §12).
> **Verified:** 2026-07-19 | commit `0df5e3ee` | 5/5 items | build OK | smoke PASS (TCG 2.9s), tests 9/9 PASS
> **Quality reviewed:** 2026-07-19 | Codex 13x (design, adversarial, consistency, perf) | 8M+1L fixed, 0 open | scope: kernel-code-quality

---

## 2. Quota Block and Charge API

- [x] `quota_block_t`, opaque (defined in `quota.c` so no caller can take its lock): `atomic_t` refcount, embedded copied owner SID, one `limit/usage/peak/failures` record per type (§5 layout), one `lock` guarding every mutation.
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
> **Accepted:** [M] klog's live-disk path re-enters VFS, so a PASSIVE caller holding a storage lock can deadlock; kernel-wide, mitigated here by the PASSIVE_LEVEL gate + deferred count (reason: scope) -> XREF: `02-kernel-core/TODO-32 §6` (item: "`klog_v2()` enqueue must never synchronously enter the live-disk flush" at line 156)
> **Deferred:** [M] cross-CPU contention proof: the scheduler is single-CPU today, so the multi-thread suites show interleaving, not parallel contention (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path" at line 333)
> **Deferred:** [L] bounded worker join + CPU-pinned fault injection for the quota tests (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Bounded worker join + CPU-pinned `kmalloc_fail_next`" at line 334)
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
> **Accepted:** [H] absorbed pre-join usage stays billed to the job until the member departs for a receipt the ledger cannot enumerate (one embedded in an ALPC message or notification state); the charge-straddle half and every LEDGER-held obligation are closed by §11 (reason: needs the embedded-receipt consumers converted to ledger obligations) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Convert the embedded-receipt charge consumers" at line 433)
> **Accepted:** [M] `ACCESS_TOKEN.UserSid` has no recorded extent, so the bounded-capture `owner_len` is caller-derived and cannot detect a truncated SID (reason: scope) -> XREF: `02-kernel-core/TODO-15 §4` (item: "Record a VALIDATED `UserSid` length in `ACCESS_TOKEN`" at line 334)
> **Deferred:** [M] `QUOTA_CHARGE_CLIENT` fails closed with `STATUS_NOT_SUPPORTED`: billing an impersonated client needs a stable per-CPU current-thread cursor and a teardown-safe token-slot pin (reason: infra) -> XREF: `02-kernel-core/TODO-15 §4` (item: "Teardown-safe primary-token READ pin" at line 333)
> **Accepted:** [M] `ob_job_create` inserts a named job into the object namespace before allocating its handle, so a handle-alloc failure leaks the directory entry, the body, and now its quota block (reason: scope, pre-existing Job-Object lifecycle) -> XREF: `02-kernel-core/TODO-21 §14` (item: "`ob_job_create` inserts a named job into `\BaseNamedObjects`" at line 464)
> **Accepted:** [H] two concurrent task constructors can claim the same slot and reset a live lock word; pre-existing and systemic across every per-process inheritance, not introduced here (reason: scope) -> XREF: `03-memory-concurrency/TODO-06 §13` (item: "Atomic task-slot CLAIM" at line 405)
> **Quality reviewed:** 2026-07-20 | Codex 14x (design, adversarial, re-adversarial, consistency, perf, test-coverage) | 12H+13M+4L fixed, 1H+3M accepted-XREF | scope: kernel-code-quality

---

## 4. Receipt Identity (Generation-Tokened Charges)

- [x] `quota_charge_receipt_t.tag` packs a monotonic generation WITH the IDLE/ACTIVE/BUSY state in one 64-bit word: contended claims CAS the pair, its BUSY owner release-stores. Comparing the generation beside the state CAS is an ABA either way.
- [x] `quota_charge_chain` issues that generation as a caller-held token, written ONLY on success; `quota_return_chain(receipt, token)` CASes the exact `{token, ACTIVE}`, so a stale returner no-ops and storage is recyclable. -> closes `§3`
- [x] Generation exhaustion fails closed with `STATUS_INTEGER_OVERFLOW` at `QUOTA_RECEIPT_GEN_MAX` (62 bits) rather than wrapping a token back onto a live one.
- [x] `out_token` is MANDATORY and non-canonical tokens (above the 62-bit generation field) are refused: the tag encoding discards high bits, so `live_token | (1 << 62)` would otherwise build the same tag and win the CAS.
- [x] A refusal raised AFTER the receipt is claimed retires the caller's token to 0; only pre-claim refusals keep it, so an error-path return cannot present a value matching the generation the next charge publishes.
- [x] A zero-amount charge consults the charge gate directly and reports `STATUS_PROCESS_IS_TERMINATING` for a SEALED task: teardown seals before it clears the block, so a block check alone would call a dying task chargeable.
- [x] `atomic64_cmpxchg` added to `kernel/atomic.h` (LOCK CMPXCHG on an aligned qword); `_Static_assert` pins the tag encode/decode round trip at the generation ceiling, where the `int64_t` cast is load-bearing.
- [x] Serializing a charge against a job-membership change SHIPPED as the §11 per-task charge gate: `quota_charge_chain` enters it, so a quiesce drains in-flight chargers and no charge can straddle the absorb-to-publish window. -> XREF: `§11`
- [x] Migrating outstanding receipt obligations at `ob_job_assign` SHIPPED as `quota_ledger_migrate_to_job`, which ADOPTS `job_absorb` (append job block, subtract the same amount) instead of charging beside it. -> XREF: `§11`
- [x] Commit: quota: receipt generation tokens for safe charge identity.

**Test checkpoint:** a token from an already-returned charge cannot return a later charge made through the same receipt storage, while the live token still can; zero, unissued, and non-canonical (high-bit) tokens are all no-ops; a charge refused because the receipt is already live leaves the live charge's token intact; a charge with no `out_token` is refused rather than left unreturnable; a post-claim refusal retires a token seeded with the exact next generation, so the charge that follows survives it; a zero-amount charge on a SEALED gate reports termination and issues no token; stale and duplicate returns leave the wait-exhausted counter untouched (verified: SUITE=quota 1842 assertions, 0 failures).

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 1842 assertions, 0 failures

> **Notes:**
> - **What shipped** -- a tagged `{generation, state}` receipt word in `quota.h`/`quota_owner.c` with a mandatory caller-held token keying the return, plus `atomic64_cmpxchg` in `kernel/atomic.h` as the one primitive it needs.
> - **How it runs** -- a charge CASes IDLE to BUSY at the current generation, retires the caller's stale token, fills the receipt, then release-publishes `{gen+1, ACTIVE}`; a return CASes the exact `{token, ACTIVE}` it was handed, waiting out a same-generation BUSY owner within a bound.
> - **Downstream effects** -- closes the §3 receipt-storage ABA; the absorb-to-publish half is now closed for charges by the §11 per-task gate, and §11 owns the refcounted ledger (§13 owns the charging consumers).
> - **Canonical doc** -- `include/kernel/quota/quota.h` (receipt identity contract).
> - **Scope boundary** -- §4 owns only the IDENTITY of a charge; nothing is charged yet (§13 objects/handles, §5-§7 pool/registry/CPU), membership serialization shipped in §11, and the handle-table lock a charge-on-insert needs is TODO-05 §3.
> **Verified:** 2026-07-25 | commit `740edf7d` + review fixes | 10/10 items | build OK | tests 23927 kernel + 16 user PASS (SUITE=quota 1842), smoke PASS (KVM 2.900s)
> **Accepted:** [M] the receipt-tag guarantees are proven only by sequential tests: concurrent same-token returners, a paused BUSY window, and cross-CPU publication visibility need per-CPU run queues that do not exist yet (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag" at line 344)
> **Accepted:** [M] the per-charge RMW count (a depth-2 charge/return pair is ~15 locked RMWs, more under `KERNEL_TESTS`) is measured only uncontended (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag" at line 344)
> **Deferred:** [H] a return that exhausts its bounded wait on a same-generation BUSY owner still abandons the charge, and the tag cannot say whether that owner is an adjust or a winning duplicate returner; the wait is now counted, not made lossless (reason: needs owner metadata in the tag) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Pending-return handoff so a return colliding with a BUSY owner is never abandoned" at line 445)
> **Deferred:** [H] the charge gate this path enters spins `for(;;)` with no bound or backoff while interrupts may be masked (reason: scope, the gate is §11 code) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Bound the per-task charge-gate CAS loops" at line 447)
> **Deferred:** [M] `quota_charge_current` bills the task named by the global `current_task` cursor, the defect class `quota.h` cites when refusing `QUOTA_CHARGE_CLIENT`; not reachable while the scheduler is single-CPU (reason: infra) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Bill `quota_charge_current` off a per-CPU task cursor" at line 448)
> **Deferred:** [L] `quota_charge_adjust` runs its `KERNEL_TESTS` writer instrumentation inside its own IRQ-off window (reason: scope, the adjust is §11 code) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Move the `KERNEL_TESTS` writer instrumentation out of `quota_charge_adjust`'s IRQ-off window" at line 449)
> **Accepted:** [M] the KNF charge consumer flattens a transient `STATUS_RETRY` to NULL, so a one-shot caller reads a racing job assignment as permanent failure (reason: scope) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Make KNF state creation status-bearing" at line 450)
> **Accepted:** [M] a receipt records only type and amount, so usage cannot be attributed to a charging subsystem the way NT pool tags allow (reason: scope) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Per-charge attribution on `quota_charge_receipt_t`" at line 451)
> **Quality reviewed:** 2026-07-25 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) + kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst | 2H+4M+5L fixed, 1H rejected, 7 accepted-XREF | scope: kernel-code-quality

---

## 5. Pool and Allocation Quota Integration

- [/] Charge-path cost characterized END-TO-END, confirming the 7-section depth-2 lifetime: `QUOTA_BUDGET_*` pin owner AND block sections, asserted for EQUALITY. UNCONTENDED only; a latency budget needs cross-CPU queues. -> XREF: `§10`
- [x] `quota_block_t` counter layout decided from that measurement: one 32-byte record per type, not four parallel arrays, so a charge touches 32 contiguous bytes. No alignment claimed (`kmalloc` gives 16); measured 7 of 14 on one line.
- [/] Add optional quota owner to tagged allocations. BLOCKED: the canonical `PTAG_*` API does not exist; only `kmalloc_tagged`/`kfree_tagged` ship, with no per-tag counters. -> XREF: `03-memory-concurrency/TODO-03 §6`
- [/] Charge nonpaged/paged pool through allocator provider hooks. BLOCKED: no pool-class allocator exists to hook -- one unified arena, no paged/nonpaged split, no provider seam. -> XREF: `03-memory-concurrency/TODO-03 §7`
- [/] Ensure kernel-internal early boot allocations are charged to System. BLOCKED with the hook: `kmalloc` is live from Phase 0, the registry validates at Phase 2, System exists at Phase 3. -> XREF: `03-memory-concurrency/TODO-03 §7`
- [/] Refuse user-triggered unbounded allocation paths without quota owner. BLOCKED with the hook: the refusal belongs at a pool entry point that does not exist yet. -> XREF: `03-memory-concurrency/TODO-03 §7`
- [ ] Separate leak-sweep instrumentation from `KERNEL_TESTS`: the per-CPU writer/epoch RMWs are unconditional, so the default image cannot measure a clean charge path and has no instrumentation-off baseline. -> XREF: `§10` (leak sweep)
- [x] Commit: quota: charge-path cost budget + per-type counter records.

**Test checkpoint:** an admitted charge, a refused charge, an over-return, and a return each enter exactly one critical section, a transfer exactly two (refused included), `set_limit` one, and every SINGLE-BLOCK argument rejection or no-op zero; a chain charge costs the 3 owner-snapshot sections plus one per charged layer while its return costs only the per-layer ones, a mid-chain refusal adds exactly one rollback section, and a zero-amount chain charge still costs one owner section (it is not free -- it validates the task); a counter record is 32 bytes at a pinned block offset and never spans more than two cache lines at the block's real runtime address; advisory TSC suites report rather than assert, and label a sample whose CPU tag cannot be verified (verified: SUITE=quota 62 suites / 593 assertions, 0 failures; measured on TCG: depth-2 chain = 5 sections to charge + 2 to return = the 7-section lifetime, 148 cycles per charge+return pair, 892 per chain pair).

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 62 suites, 0 failures

> **Notes:**
> - **What shipped** -- `test_quota_perf.c` (7 suites) plus the `QUOTA_BUDGET_*` charge-cost contract in `quota.h` and a `quota_counters_t` record-per-type layout in `quota.c` replacing four parallel counter arrays.
> - **How it runs** -- block and owner locks funnel through one counting helper, armed by `quota_test_lock_count_begin` and scoped to the arming THREAD at PASSIVE, and counted on the UNLOCK side so no instrumentation runs inside an IRQ-disabled window.
> - **Downstream effects** -- closes the counter-layout question §2 and §3 deferred here; the four pool-charging items are BLOCKED and now carry reciprocal items in TODO-03 §6/§7.
> - **Canonical doc** -- `include/kernel/quota/quota.h` ("Charge-path cost contract").
> - **Scope boundary** -- §5 owns the cost budget and counter layout ONLY; the pool allocator, its tag API, and the provider seam are TODO-03 §6/§7, and CONTENDED latency needs cross-CPU run queues (§10).
> **Verified:** 2026-07-20 | commit `1907f55c` + review fixes | 1/6 items | build OK | smoke PASS (TCG 4.360s), tests 62 suites/593 PASS
> **Deferred:** [H] pool and tagged-allocation charging: no pool-class allocator exists to hook, so all four charging items stay open (reason: infra) -> XREF: `03-memory-concurrency/TODO-03 §7` (item: "Charge paged/nonpaged pool allocations through a quota provider hook")
> **Deferred:** [H] a real latency budget (throughput + p50/p99 IRQ-disabled hold time under simultaneous chargers) cannot be built on a single-CPU scheduler; the shipped budget is structural only (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag" at line 333)
> **Accepted:** [M] EXACTLY half the counter records straddle a line at every legal block address (24-byte prefix, 32-byte records); fixing the ratio needs the array offset to be a multiple of 32, not just an aligned allocation (reason: not-functional-today) -> XREF: `03-memory-concurrency/TODO-03 §7` (item: "Cache-line-aligned pool allocation for quota counter records")
> **Accepted:** [M] the record co-locates `limit`/`failures` with the `usage`/`peak` a charge writes, so a lock-free reader's line can now be invalidated by an unrelated charge; SoA vs AoS vs hot/cold needs a real SMP benchmark (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag" at line 333)
> **Accepted:** [L] the perf suites read `t->quota`/`t->quota_user` without the owner lock or a block reference; pre-existing house pattern shared with `test_quota.c`, not introduced here (reason: scope) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Bounded worker join + CPU-pinned `kmalloc_fail_next`" at line 334)
> **Quality reviewed:** 2026-07-20 | Codex 10x (design, adversarial x2, test-coverage, re-adversarial x3, consistency, perf x2) | 2H+12M+5L fixed, 2H+4M+1L accepted-XREF | scope: kernel-code-quality

---

## 6. Registry, ALPC, Notification Quotas

- [ ] Registry: charge names, data bytes, watchers, transactions. BLOCKED: value slots are monotonic (delete only tombstones), `hive_parse_value` bypasses the charge point, transactions do not exist. -> XREF: `TODO-14 §15`
- [/] ALPC: queued messages charged (`QUOTA_RES_ALPC_MESSAGE` per entry, receipt in `PORT_MESSAGE_ENTRY`). Port sections and completion-list entries have no allocation site to charge yet. -> XREF: `TODO-24 §6`
- [x] Notifications: `knf_create_state` charges state + retention budget and `knf_subscribe` charges the subscription; all returned at teardown. New types appended at the END of the enum (IDs are ABI, pinned by `_Static_assert`).
- [/] Per-user caps wired to TODO-02: `quota_config.c` registers a privileged `quota.user.<type>` tunable per type whose callback re-limits LIVE blocks. MECHANISM only -- no production setter, so no cap is enforced yet. -> XREF: `TODO-02 §6`
- [x] `quota_charge_current` is the one charge entry point; its boot exemption is bound to `kernel_subsystem_ready(SUBSYS_SCHED)`, never to a task missing a block, so a request racing teardown cannot inherit it.
- [x] Charge precedence ENFORCED (was "intended" in `quota.h`): per-block explicit > kernel-config override > taxonomy default, via a per-type `limit_explicit` provenance flag.
- [x] Commit: quota: registry/ALPC/notification subsystem charge points.

**Test checkpoint:** creating a notification state charges one state plus its full `KNF_MAX_PAYLOAD` retention budget and deleting it returns both; a create whose retention charge is refused returns the state charge it already took; subscribing past a capped budget returns `STATUS_QUOTA_EXCEEDED` with no node and unchanged usage; a queued ALPC message charges the SENDER one message and is returned on receive AND on port teardown with the queue still full, while an over-cap send reports `STATUS_QUOTA_EXCEEDED` (not `INSUFFICIENT_RESOURCES`) and releases its port byte reservation; every `quota.user.<type>` tunable is registered rather than merely falling back, and a `kernel_tunable_set` re-limits a block that already existed while a block created afterwards is seeded with the new value; the re-limit walk crosses more than two 16-block batches and leaves explicit limits at the batch boundaries untouched (verified: SUITE=quota 82 suites / 1004 assertions, SUITE=ipc 361, full 23089 kernel + 16 user-mode, 0 failures).

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 82 suites, 0 failures

> **Notes:**
> - **What shipped** -- `quota_config.c` (per-type `quota.user.<type>` tunables + a live re-limit walk), `quota_charge_current` as the single subsystem charge entry point, two appended resource types, and charge points in `knf.c` and `alpc_port.c`.
> - **How it integrates** -- tunables register in Phase 3; a change callback publishes the new default then walks the USER registry in pinned 16-block batches, so no block lock is taken under `g_registry_lock`.
> - **Downstream effects** -- makes the `quota.h` precedence MECHANISM enforced; the policy source, registry charging, and ALPC port sections are filed on TODO-02 §6, TODO-14 §15, and TODO-24 §6. Codex adoptions in the commit messages.
> - **Canonical doc** -- `include/kernel/quota/quota.h` (precedence contract) + `src/kernel/quota/quota_config.c` header ("what this does not yet do").
> - **Scope boundary** -- §6 owns the charge points and the config mechanism ONLY; no finite cap is enforced until TODO-02 gains a privileged setter, and delta-adjusting a live charge needs §11.
> **Verified:** 2026-07-20 | commit `8a578023` + review fixes | 4/6 items | build OK | smoke PASS (TCG 2.710s), tests 23089 kernel + 16 user PASS (SUITE=quota 82 suites/1004)
> **Deferred:** [H] no production `kernel_tunable_set` caller exists and every taxonomy default is unlimited, so the charge points ACCOUNT but enforce no finite cap; the mechanism and its privilege flag ship, the policy source does not (reason: infra) -> XREF: `02-kernel-core/TODO-02 §6` (item: "Privileged production write path for tunables" at line 198)
> **Deferred:** [H] registry name/data-byte and watcher charging: value slots are a monotonic tombstone pool with no reuse and `hive_parse_value` bypasses the charge point, so returning logical bytes would let slot exhaustion escape the cap (reason: infra) -> XREF: `02-kernel-core/TODO-14 §15` (item: "Charge registry names/data bytes and watchers via `quota_charge_current`" at line 685)
> **Deferred:** [M] ALPC port sections and completion-list entries have no allocation site to charge yet (reason: scope) -> XREF: `02-kernel-core/TODO-24 §6` (item: "Charge port sections and completion-list entries to the creating task" at line 292)
> **Accepted:** [H] the 8-slot receipt embedded per message grows `PORT_MESSAGE_ENTRY` 56 -> 152 bytes for a 3-block chain; a compact chain receipt needs the charge-API rework (reason: infra) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Compact per-chain charge receipt" at line 430)
> **Accepted:** [H] a quota-refused send allocates and zeroes the message before refusing, since a receipt cannot be built before its storage exists; needs a reserve-then-commit charge (reason: infra) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Reserve-then-commit charge so a refused send does not first allocate" at line 431)
> **Accepted:** [H] same-SID processes serialize on one USER block, so ALPC/KNF charge paths contend; sharded or per-CPU credit needs cross-CPU proof (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag" at line 333)
> **Quality reviewed:** 2026-07-20 | Codex 7x (design, adversarial x2, test-coverage, re-adversarial, consistency, perf) | 2H+7M+1L fixed, 3H accepted-XREF | scope: kernel-code-quality

---

## 7. CPU, I/O, and Wakeup Accounting

- [x] Per-process CPU user/kernel split aggregates per job over the MEMBERSHIP INTERVAL: `task_acct_capture_base()` at assign, `task_acct_delta_since()` at rollup and detach (`ob_job.c`).
- [x] CPU accounting is charged on EVERY tick, no longer gated on `sched_enabled`: time spent in `scheduler_disable()` regions (RCU read sections, compositor compose/swap) was previously uncharged.
- [x] I/O split by read/write/control: `task_acct_note_read_io/write_io/control_io` are the canonical seams (file AND pipe paths, legacy + NT), plus `job.acc_other_*` and the `IO_COUNTERS.Other*` fields previously left zeroed.
- [x] Wakeup and timer counters: `task_wake_thread()` is the single wait-grant seam (10 sites incl. `thread_join`; CAS-claimed so concurrent wakers count once); `ObCreateTimerEx` counts at the object-publication boundary.
- [x] `quota_rate_limit_t` (versioned, keyed by `quota_rate_class_t`, carrying unit/period/weight/reservation/max/hard-cap/generation) is stored per block distinctly from usage and published under a seqlock.
- [x] CPU rate enforcement is owned by the scheduler's bandwidth controller, not its weight table. -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §14` (item: "Consume `quota_rate_limit_get()`")
- [x] Block-I/O rate enforcement now has a concrete owner. -> XREF: `05-storage-filesystems/TODO-01-block-storage-hardening.md §8` (item: "Consume `quota_rate_limit_get()`")
- [x] Policy reads metrics via `task_acct_sample()` (all counters under one timestamp) and derives its own rates; the kernel stores no window, so no scheduler-private state is duplicated.
- [/] Control-I/O counters are wired and projected but read zero until a control op completes (`NtDeviceIoControlFile` is a stub). -> XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §14` (item: "task_acct_note_control_io")
- [x] Commit: quota: CPU/IO/wakeup accounting + rate-limit records (measurement).

**Test checkpoint:** per-process CPU user/kernel split advances monotonically across two samples; the job aggregate equals the sum of its members' MEMBERSHIP-INTERVAL deltas (a member's pre-join usage is excluded, and an inverted baseline saturates to zero rather than wrapping); I/O counters break down by read/write/control op and bytes, with a zero-byte control counted as an op contributing no bytes; only a BLOCKED-to-READY transition counts as a wakeup; the rate-limit record round-trips every field, refuses an unsatisfiable or unversioned policy without disturbing the stored one, advances its generation only on success, and is unaffected by charging usage on the same block.

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 1173 assertions, 0 failures

> **Notes:**
> - Shipped `task_acct_*` metrics on `struct task` (control-I/O, wakeup, timer-creation counters plus `task_acct_sample()`) and `quota_rate_limit_t` policy records on the quota block, published under a seqlock by `quota_rate_limit_set/get`.
> - Metric recording is lock-free RELAXED atomics entering ZERO quota critical sections, so it is safe in interrupt context and beneath a caller's spinlock; it does not participate in the `QUOTA_BUDGET_*` charge-path contract.
> - Job CPU/I/O aggregation moved from member LIFETIME totals to the MEMBERSHIP INTERVAL, and the `IO_COUNTERS.Other*` fields previously returned as zero are now populated; design-review adoptions are in the commit message.
> - Canonical doc: [`include/kernel/quota/quota.h`](../../include/kernel/quota/quota.h) (rate-record contract) and the metric block in [`include/kernel/sched/task.h`](../../include/kernel/sched/task.h) (canonical event per metric).
> - Scope boundary: §7 owns MEASUREMENT and the policy RECORD only. CPU bandwidth enforcement is TODO-06 §14, block-I/O QoS is storage TODO-01 §8, the device-control event source is storage TODO-05 §14.
> **Verified:** 2026-07-20 | commit `4b541c54` | 9/10 items | build OK | tests 23264 PASS, smoke PASS (KVM 3.130s)
> **Accepted:** [H] paired (ops,bytes) counters are separate atomics, so a membership snapshot can split one I/O event across the boundary (reason: fix belongs at the completion writers) -> XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §5` (item: "Publish each completion's (ops, bytes) pair coherently" at line 154)
> **Accepted:** [H] wait paths publish a waiter before setting THREAD_BLOCKED and the wake claim tests only the state, not the wait instance (reason: pre-existing, needs a wait-lock transaction) -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §15` (item: "Give each wait a generation token the wake must match" at line 291)
> **Accepted:** [M] the tick charges `tasks[current_task]`, one global cursor, so a tick can bill the wrong process once APs schedule (reason: needs per-CPU scheduler state) -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §15` (item: "Resolve the interrupted task through PER-CPU scheduler state" at line 293)
> **Accepted:** [M] `thread->state` is CASed by the wake seam but plain-stored by ~16 other writers (reason: scheduler-wide change) -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §15` (item: "Make `thread->state` uniformly atomic" at line 292)
> **Accepted:** [M] `ob_job_collect_accounting` holds `job->lock` with IRQs off across up to 32 members of delta math (reason: snapshot-then-compute refactor) -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §14` (item: "Cut the `ob_job_collect_accounting` lock hold" at line 429)
> **Accepted:** [L] the tick quantum is the nominal rate, so a one-shot/tickless arm would mis-charge (reason: not-functional-today, no production one-shot caller) -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §15` (item: "Derive the tick quantum from the ACTUAL elapsed monotonic delta" at line 294)
> **Accepted:** [H] `SYS_READFILE` is uninstrumented and hands its ring-3 `buf` to `vfs_read` for kernel-mode writing (reason: scope, the primitive is slated for retirement and must not be instrumented) -> XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §5` (item: "Closing `SYS_READFILE` must close its hazards" at line 152)
> **Deferred:** [M] control-I/O counters are wired and projected but read zero until a device-control op completes -> XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §14` (item: "task_acct_note_control_io" at line 313)
> **Quality reviewed:** 2026-07-20 | Codex 13x (design, adversarial x8, consistency, perf x2, re-adversarial x2, test-coverage) | 5H+11M+3L fixed, 7 accepted-XREF | scope: kernel-code-quality

---

## 8. Native Query/Set Quota Syscalls

- [x] `ProcessQuotaLimits` (PROCESSINFOCLASS 1) on the EXISTING `NtQueryInformationProcess`/`NtSetInformationProcess`; design review rejected a dedicated `Nt*QuotaInformationProcess` pair (two authorization surfaces, one state).
- [x] Windows-pinned ABI in `include/kernel/nt/quota_syscall_info.h`: `QUOTA_LIMITS` (48 B) + `QUOTA_LIMITS_EX` (88 B), every offset `_Static_assert`ed; the caller's LENGTH selects the form, never a suffix peek.
- [x] The projection reads real state only: pool limits from the process quota block, `TimeLimit` from `RLIMIT_CPU` (checked, saturating, rounds up), the rest from the policy record. -> XREF: `TODO-21-process-model-extensions.md §9`
- [x] `quota_policy_set()` is ONE prevalidate-then-commit transaction over three stores (quota block, rlimits, policy record): a mixed lower+raise request is refused whole, and `rlim_max` survives a soft-limit write.
- [x] Any widening needs `SeIncreaseQuotaPrivilege` -- a bigger number, a removed cap, a raised CPU-rate percent, or a dropped hard-working-set flag; lowering one's own is unprivileged. Self-only, probe + bounce-copy, `ReturnLength` included.
- [x] Token-local overlay: pool limits are written to the task's OWN process block, never the shared user block, so a RESTRICTED token caps tighter without lowering the parent's budget. -> XREF: `§3` (canonical user block)
- [x] Job aggregate limits: `JobObjectQuotaLimitInformation` (0x1000) reports per-resource usage/peak/limit/failures and sets them all-or-nothing under one transaction lock. Frozen 520-byte V1 wire shape (row count is ABI, not the internal enum).
- [x] EVERY job aggregate write needs the privilege, lowering included: job handles carry no granted-access mask, so any opener of a shared named job could otherwise squeeze its members. -> XREF: `TODO-05-object-manager.md §3`
- [/] `RLIMIT_NOFILE` is reconciled with `handle_table.handle_limit` READ-only (they encode "no limit" inversely); pushing the rlimit INTO the table needs its reservation/commit lock. -> XREF: `TODO-05-object-manager.md §3`
- [/] Working-set and pagefile limits are stored and returned but not ENFORCED -- no per-process VM/commit counters exist to enforce them against. -> XREF: `TODO-21-process-model-extensions.md §9`
- [/] `JOBOBJECT_EXTENDED_LIMIT_INFORMATION`'s memory fields stay 0 (no limit set) rather than carrying pool bytes, which mean something else. -> XREF: `TODO-21-process-model-extensions.md §13`
- [x] Commit: quota: ProcessQuotaLimits query/set + job aggregate limit class.

**Test checkpoint:** `ProcessQuotaLimits` returns a `QUOTA_LIMITS` whose pool limits are the process block's, whose `TimeLimit` is `RLIMIT_CPU` converted (a 0-second rlimit projects as the TIGHTEST cap rather than "unlimited", sub-second truncates back to it, an unrepresentable second count saturates positive), and whose working-set fields read back what was stored; raising a limit -- a bigger number, a cap removed by writing 0, a raised CPU-rate percent, or a dropped hard-working-set flag -- returns `STATUS_PRIVILEGE_NOT_HELD` unprivileged while lowering succeeds; a mixed lower-plus-raise request commits NOTHING; a soft `TimeLimit` write leaves `rlim_max` intact; `RLIMIT_NOFILE` 0 reconciles to a deny-all cap rather than "unlimited"; a 48-byte request ignores rather than validates the EX suffix; `JobObjectQuotaLimitInformation` keeps its frozen 520-byte V1 shape, reports one row per registered resource type, refuses a request that writes the kernel-owned Usage column, and refuses an unprivileged write even when it only lowers.

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 16 suites / 105 assertions, 0 failures

> **Notes:**
> - Shipped `include/kernel/nt/quota_syscall_info.h` (Windows-pinned `QUOTA_LIMITS`/`QUOTA_LIMITS_EX` ABI) + `quota_policy.{h,c}` (policy record + commit transaction), wired as `ProcessQuotaLimits` on the two existing process-information syscalls.
> - The set path authorizes the WHOLE request before writing any of its three stores and restores the pre-image on a late failure; a 48-byte request preserves the EX-only suffix rather than destroying a policy it cannot express.
> - Every widening is privileged, including the non-numeric ones: a higher CPU-rate percent, a raised working-set FLOOR, a dropped `_MAX_ENABLE`, an added `_MIN_ENABLE` (the two flags are asymmetric because their bounds are).
> - Serialized per task by a SPINLOCK, not a mutex (`mutex_unlock` clears `locked` before the owner fields); the job setter needs no transaction lock at all while every job write is privileged. Lock order: transaction lock -> `quota_lock` / `rlimit_lock` / block lock.
> - Canonical doc: [`include/kernel/quota/quota_policy.h`](../../include/kernel/quota/quota_policy.h) (which limit lives in which store, and why the zero encodings differ).
> - Scope boundary: §8 owns the QUERY/SET surface and its authorization. Working-set/commit ENFORCEMENT is TODO-21 §9/§13, the handle-limit write-back is TODO-05 §3, pressure telemetry is §12.
> **Verified:** 2026-07-20 | commit `ee8443b9` | 8/11 items | build OK | tests 23400+16 PASS, smoke PASS (KVM 2.58s)
> **Accepted:** [Critical] probe + `copy_to_user` bounds a ring-3 pointer but is not a PTE-ownership boundary, so a low kernel alias is still writable (kernel-wide pattern) -> XREF: 01-boot-platform/TODO-10-bare-metal-hardening.md §8 (item: "Bare-metal SMEP/SMAP unblock is external" at line 378 -- the clean kernel PML4 that stops a user pointer aliasing kernel memory)
> **Accepted:** [H] `job_lookup` returns an unreferenced body, so a concurrent last-handle close races every job class (reason: pre-existing, systemic) -> XREF: 02-kernel-core/TODO-05-object-manager.md §3 (item: "Push `RLIMIT_NOFILE` into `handle_table.handle_limit` under the table lock" -- the same item-17 lock pass that adds atomic lookup+reference)
> **Accepted:** [M] information-class length failures return `STATUS_BUFFER_TOO_SMALL` where Windows returns `STATUS_INFO_LENGTH_MISMATCH` (reason: scope -- repo-wide convention) -> XREF: 02-kernel-core/TODO-12-native-api-ssdt.md §6 (item: "Length failures on Nt*Information{Process,Thread,JobObject}" at line 364)
> **Accepted:** [M] transaction spinlocks hold IRQs off across nested short locks; the sleepable alternative is unsound today -> XREF: 03-memory-concurrency/TODO-08-advanced-sync.md §11 (item: "Consumer -- kernel quota transactions" -- restore a mutex once the ownership-clear race is repaired)
> **Deferred:** [M] cross-process quota query/set (the primary Win32 call shape behind `SetProcessWorkingSetSizeEx`) is refused self-only until process handles carry rights -> XREF: 02-kernel-core/TODO-05-object-manager.md §3 (item: "Give `HANDLE_TABLE` an owning-task back-pointer" -- the same granted-access pass)
> **Quality reviewed:** 2026-07-20 | Codex 7x (adversarial x3, re-adversarial x2, consistency, perf) | 1Crit+5H+8M fixed, 2Crit/H+2M accepted-XREF | scope: kernel-code-quality

---

## 9. Resource Pressure Events and Recovery Hooks

- [/] 4 levels with asymmetric hysteresis (rise 700/850/950, fall 600/780/900 permille; 3-sample rise, 5-sample fall) sampled by a periodic 50 ms timer; transitions publish via KNF + ETW 0x1300. Input is BUDGET saturation. -> XREF: `§12`
- [/] Critical pressure publishes a nomination on `Kernel\QuotaNomination` as the interim carrier; a direct orchestrator call needs a hook that does not exist. -> XREF: `02-kernel-core/TODO-30-system-health-recovery-orchestrator.md §6`
- [x] Quota-failure event contract defined HERE: `QUOTA_FAILURE_RECORD` (packed, versioned, offset-asserted) carrying block id, principal, SID digest, requested/current/limit, status, pid/tid; per-resource token bucket. -> XREF: `TODO-16 §6`
- [/] Targeted cleanup has no seam: no cache-drain, log-trim, or refuse-new-handles entry point exists and working-set Min/Max is unenforced, so each is filed to its owner. -> XREF: `TODO-21-process-model-extensions.md §9`, `TODO-30 §6`
- [/] ARCHITECTURE DECISION: COOPERATIVE-ONLY (Win-style). `quota_pressure_nominate` picks the most-saturated USER principal at or above the watch threshold; the drain publishes it on a critical transition, never terminating it. -> XREF: `§14`
- [x] Commit: quota: pressure levels (hysteresis), quota-failure events, recovery hooks.

**Test checkpoint:** samples across the thresholds walk normal->watch->warning->critical one level per debounce and hold for an arbitrarily long run inside a band (zero transitions recorded); exactly-at-rise enters and exactly-at-fall holds; returning charges walk the level back down while an unlimited cap reads INVALID and never de-escalates; the domain sample tracks the WORST live principal, so an idle user cannot mask a saturated one; a refused charge emits the event carrying usage and limit as they stood at the refusal, and an overflow refusal reports its own status; the token bucket admits a full burst then counts throttled events separately from ring overflow, with the sequence still advancing across a drop; a transition survives a ring already filled by a failure burst; a saturated principal reaching critical is nominated with an expiry and retracted once it returns the charge.

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 42 suites, 0 failures

> **Notes:**
> - Shipped `quota_pressure.h` + `quota_pressure.c`: 4-level hysteresis over 16 domains, 64-slot publish ring drained by a threaded DPC, the failure-event contract, per-resource token buckets, cooperative nomination.
> - A quota mutation only MARKS its domain; a periodic 50 ms sampler is the single producer of samples, deriving each domain from the registry's worst live USER principal, so a debounce completes for steady pressure and sample order is total.
> - Publication is deferred by design: the charge API is DISPATCH/interrupt-legal and `knf_publish` is not, so records are copied to the ring and published at PASSIVE. Codex adoptions are in the commit message.
> - Canonical doc: the contract block at the top of `include/kernel/quota/quota_pressure.h`.
> - Scope boundary: §9 owns levels, the failure-event contract, and nomination; §12 owns stall telemetry, TODO-30 §6 recovery actions, TODO-21 §9 working-set enforcement.

> **Verified:** 2026-07-24 | commit `ede7213f` | 2/5 items | build OK | tests 23607/23607 PASS | smoke PASS (KVM 2.800s)
> **Accepted:** [H] recovery actions (cache drain, log trim, refuse-new-handles) have no seam to call and no orchestrator to dispatch them -> XREF: 02-kernel-core/TODO-30 §6 (item: "Provide the cleanup entry points TODO-25 §9 has no seam for today" at line 89)
> **Accepted:** [H] critical pressure cannot notify an orchestrator directly; the KNF nomination state is the interim carrier -> XREF: 02-kernel-core/TODO-30 §6 (item: "Accept a resource-exhaustion pressure source from TODO-25 §9" at line 88)
> **Accepted:** [M] working-set Min/Max is stored but unenforced, so pressure recovery cannot trim an offending process -> XREF: 02-kernel-core/TODO-21-process-model-extensions.md §9 (item: "Enforce working-set Min/Max so TODO-25 §9 pressure recovery can trim an offending process" at line 301)
> **Accepted:** [M] nomination ranks USER principals only; process-level ranking needs a lifetime-safe task iterator and a System-protected flag, neither of which exists -> XREF: 02-kernel-core/TODO-25 §14 (item: "Lifetime-safe task enumeration for process-level victim nomination" at line 432)
> **Deferred:** [H] the four levels derive from budget saturation, not the stall-time metric the section names; the stall source is tagged and seamed but unwired -> XREF: 02-kernel-core/TODO-25 §12 (item: "Feed the accumulators into the §9 pressure machine via `quota_pressure_submit_stall`" at line 333)
> **Quality reviewed:** 2026-07-24 | Codex 6x (design, adversarial, consistency, perf, test-coverage, re-adversarial) | 11H+13M+1L fixed, 5 accepted-XREF | scope: kernel-code-quality

---

## 10. Tests, Leak Sweeps, and Dashboards

- [/] Unit tests: charge/return, rollback, concurrent charges ship in `test_quota.c`; ALPC quota under `TEST_CAT_IPC`. Duplicate-handle and registry quota have no charge point yet. -> XREF: `§13`, `§6`
- [x] Boot leak sweep compares outstanding USER-block quota across every test CATEGORY (`quota_sweep_close`), gated on positive per-type usage deltas through a `quota_leaked` counter the test driver folds into FAILED.
- [x] `quota_dump()` renders live USER blocks (id, SID digest, per-type usage/peak/limit/failures) on serial; `quota_dump_crash()` is the panic form (try-lock, preallocated rows, header-only fallback), wired into `panic.c`.
- [x] Bulletproofing: a charge naming no owning block or no valid resource type is refused with `STATUS_INVALID_PARAMETER` and moves no counter; the taxonomy half stays in `test_quota.c`.
- [/] Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag (same-token returners, a paused BUSY window, publication visibility): needs per-CPU run queues. -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`
- [/] Bounded worker join + CPU-pinned `kmalloc_fail_next`: `thread_join` has no timeout, and the injection countdown lives in the armed CPU's per-CPU data so a migrating task never trips it. -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`
- [/] Invocation-scoped lock accounting: attributes to the ARMING THREAD at `PASSIVE_LEVEL`, so migration no longer drops a section. NOT SMP-sound: `thread_current()` reads global cursors. -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`
- [/] Bound `quota_dump()` whole-invocation OUTPUT, not just its block count: 32 blocks x 16 types can exceed 64 KiB of serial and evict most of the klog ring. Needs pagination or an async drain. -> XREF: `TODO-27-crash-dump-generation.md §7`
- [x] Commit: quota: unit tests, boot leak sweep, quota_dump dashboard.

**Test checkpoint:** the quota category passes (charge/return, rollback, concurrent charges; ALPC quota via `TEST_CAT_IPC`); the boot leak sweep reports zero net quota delta across every category and the summary carries a `quota-leaked` field the test driver folds into FAILED; every sweep verdict branch (clean, count-only, leaked, indeterminate) is asserted from synthetic snapshots; `quota_dump()` renders a named per-type usage row without mutating the registry, `quota_dump_crash()` takes its header-only fallback and RETURNS when the registry lock is held, and a snapshot spanning two pin batches sums exactly.

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 176 suites, 0 failures

> **Notes:**
> - Shipped `quota_dump()` / `quota_dump_crash()`, `quota_leak_snapshot()` + a registry generation counter, `spin_tryunlock()` (the missing counterpart to `spin_trylock`), and the per-category leak sweep; 20 suites in `test_quota_dashboard.c`.
> - The sweep is CATEGORY-scoped and gates only positive per-type USAGE deltas via a separate `quota_leaked` counter; a count-only change is reported, never gated, since a canonical USER block is legitimately retained.
> - A snapshot is coherent only when registry generation, counter-mutation epoch, ACTIVE-WRITER count zero, and two agreeing walks ALL hold; a transfer parked between its two stores defeats any of them alone.
> - Only USER blocks are enumerable (PROCESS needs a lifetime-safe task iterator, JOB a job registry); chain charges roll up into the USER block, so a leaked process or job charge still surfaces.
> - Canonical doc: the "Dashboards and the leak sweep" contract block in `include/kernel/quota/quota.h`.
> - Scope boundary: §10 owns the dashboards, the sweep, and bulletproofing; §14 owns lifetime-safe task enumeration, TODO-27 §7 the panic-safe emitter, TODO-07 §3 the per-CPU run queues.

> **Verified:** 2026-07-25 | commit `b18c881d` + review fixes | 3/8 items | build OK | tests 23766/23766 PASS, 0 quota-leaked | smoke PASS (KVM 3.120s)
> **Accepted:** [M] the contention test holds the registry lock across a klog that busy-waits the UART, so proving the try-lock fallback costs a bounded IRQ-off window (reason: needs a non-blocking emitter) -> XREF: 02-kernel-core/TODO-27-crash-dump-generation.md §7 (item: "`dump_emit_raw(str)` -- panic-safe emitter replacing `klog` in panic-path dumpers" at line 258)
> **Accepted:** [H] `quota_dump_crash` still emits through `klog`, which takes the blocking `s_klog_lock` and can also flush to disk; pre-existing and repo-wide on this panic path (reason: scope) -> XREF: 02-kernel-core/TODO-27-crash-dump-generation.md §7 (item: "`dump_emit_raw(str)` -- panic-safe emitter replacing `klog` in panic-path dumpers" at line 258)
> **Deferred:** [M] the dashboard suite asserts exact equalities over global registry state, which becomes flaky the moment threads run on more than one CPU (reason: infra) -> XREF: 02-kernel-core/TODO-25 §10 (item: "Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag" at line 333)
> **Deferred:** [M] `quota_dump()` bounds blocks but not total OUTPUT, so a large registry can exceed 64 KiB of serial and evict most of the klog ring (reason: in-scope, needs pagination) -> XREF: 02-kernel-core/TODO-25 §10 (item: "Bound `quota_dump()` whole-invocation OUTPUT" at line 336)
> **Accepted:** [M] the per-CPU writer/epoch RMWs are unconditional under `KERNEL_TESTS`, so the default image's charge timing includes instrumentation and has no off-baseline (reason: needs a build flavor) -> XREF: 02-kernel-core/TODO-25 §5 (item: "Separate leak-sweep instrumentation from `KERNEL_TESTS`" at line 171)
> **Quality reviewed:** 2026-07-25 | Codex 7x (adversarial x3, consistency x2, perf x2, re-adversarial) + kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst | 3H+13M+3L fixed, 5 accepted-XREF | scope: kernel-code-quality

---

## 11. Charge Ledger and Transactional Adjustment

Split out of the original "Object and handle quota integration" by its §11 complexity verdict (14 items, ABI impact). This section owns the OWNER-SIDE machinery only -- a receipt ledger whose lifetime is independent of the task slot, the drain/quiesce protocol that serializes charges against membership transitions, and the atomic multi-block adjust. The Object Manager charge points that consume it are §13; the charge-path cost and lifetime follow-ups filed by other sections' reviews are §14. The ledger must exist before either can bill anything, and it does not depend on the blocked TODO-05 §3 handle-table primitive.

- [x] `quota_ledger_t` (`quota_ledger.h`/`.c`): refcounted, created on first use, append-only slot chunks each embedding a `quota_charge_receipt_t`. A handle is `{ledger ref, slot, token, epoch}`; the epoch stops a stale handle freeing a reused slot.
- [x] Drain/quiesce via a per-task gate: `{state, in-flight}` packed in ONE `task->quota_gate` word, entered by `quota_charge_chain` so EVERY chain charge participates. `quota_gate_quiesce` closes and drains. -> XREF: `§4`
- [x] `quota_charge_adjust(receipt, token, new)`: holds every block lock at once in ascending-address order, prevalidates the whole delta, then commits. A refusal lifts no peak and moves no usage. -> XREF: `§2`
- [x] `quota_ledger_migrate_to_job` ADOPTS: it appends the job block to each outstanding receipt and subtracts that amount from `job_absorb`, issuing no second charge, so it cannot fail partway; `_unmigrate_from_job` reverts. -> XREF: `§4`
- [x] `quota_gate_seal` at `task_death_teardown` (charges refused, returns still legal); `quota_ledger_task_release` after the `task_cleanup` handle sweep drains ONLY orphans -- a slot with a live holder outlives the task.
- [x] Commit: quota: refcounted charge ledger + transactional charge adjust.

**Test checkpoint:** a ledger obligation stays returnable after the task drops its own claim and still moves the owning block's counters exactly; a `quota_charge_adjust` refused past a limit moves no usage and lifts NO PEAK (the prefix-commit hazard); a charge against a quiesced gate is refused with `STATUS_RETRY` and moves no counter, while a SEALED gate reports `STATUS_PROCESS_IS_TERMINATING` and cannot be reopened; a timed-out drain restores OPEN rather than stranding a live task CLOSED; migration leaves the job's usage unchanged and the obligation's later return credits the job; a refused join reverts and `quota_job_unabsorb` then withdraws the whole absorbed amount; an orphaned obligation is reclaimed at release and counted, while one with a live holder is not (verified: SUITE=quota, 23 suites, 0 failures, 0 quota-leaked).

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 23 §11 suites, 0 failures

> **Notes:**
> - **What shipped** -- `quota_ledger.h`/`quota_ledger.c` (refcounted obligation ledger + the per-task charge gate), `quota_charge_adjust` in `quota.c` (multi-block prevalidate-then-commit), and 23 suites in `test_quota_ledger.c`.
> - **How it runs** -- the gate is ONE packed `{state, in-flight}` word in `struct task` entered by `quota_charge_chain`, so every chain charge participates for two atomic RMWs and zero lock sections; `ob_job_assign` holds a quiesce across absorb, adopt, and publish, and reopens only after membership is visible.
> - **Downstream effects** -- closes the §3/§4 absorb-to-publish window for charges and gives §13 the ledger its Object Manager charge points bill against; the Codex adoption trail (4 design + 7 adversarial High) is in this section's commit message.
> - **Canonical doc** -- `include/kernel/quota/quota_ledger.h` (gate state machine + ledger contract).
> - **Scope boundary** -- §11 owns the ledger, the gate, and the adjust; §13 owns the charge points that use them; §14 owns converting the embedded-receipt consumers (ALPC, KNF) so their pre-join receipts become migratable.

> **Verified:** 2026-07-25 | commit `f643f7ce` + review fixes | 5/5 items | build OK | tests 23915/23915 PASS, 0 quota-leaked | smoke PASS (KVM 2.800s)
> **Accepted:** [M] the gate's drain budget cannot BOUND the operation it waits for: `spin_lock_irqsave` has no deadline, so a descheduled lock holder makes an admitted adjust arbitrarily long and a quiesce can expire into a transient `STATUS_RETRY` (reason: needs bounded try-lock + cross-CPU contention to validate) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Bound `quota_charge_adjust`'s IRQ-off window" at line 435)
> **Accepted:** [H] a return colliding with a BUSY owner past its retry budget ABANDONS the obligation to the ledger drain rather than crediting it, so the charge is reclaimed and counted as a leak instead of returned (reason: a lossless handoff needs cross-CPU contention to validate) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Pending-return handoff so a return colliding with a BUSY owner is never abandoned" at line 434)
> **Accepted:** [H] a pre-join receipt the ledger cannot enumerate (embedded in an ALPC message or notification state) still has its absorbed amount held to detach; every LEDGER-held obligation migrates correctly (reason: needs those consumers converted to ledger obligations) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Convert the embedded-receipt charge consumers" at line 433)
> **Quality reviewed:** 2026-07-25 | Codex 12x (design, adversarial x4, consistency x2, perf x2, re-adversarial x3) + kernel-quality-auditor + concurrency-evidence-mapper | 12H+13M+2L fixed, 1H+1M rejected, 3 accepted-XREF | scope: kernel-code-quality

---

## 12. Resource Pressure Stall Telemetry

Split out of §8 by its design review: §8's other items are syscall marshalling over state that already exists, while the pressure metric is new instrumentation across the scheduler, the allocator and the storage layer -- and shipping the info class over uninstrumented seams would report "no pressure" for a system that is actually stalling. §9 derives its four levels from these counters.

- [x] Per-resource (cpu/mem/io) stall accumulators in their own 3-entry domain space (`quota_stall.h`; `quota_resource_type_t` is frozen at 16 with no cpu/io), with some/full totals and avg10/60/300 EWMAs.
- [ ] Per-CPU scheduler state is a HARD prerequisite for the cpu SEAM; the telemetry itself is shipped and CPU-pinned. -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`
- [x] Producers report ENTER/LEAVE transitions and the owning CPU integrates wall time, so overlapping waits accrue one window, not N.
- [x] `cpu.full` is 0 by an explicit "undefined at system level" contract (as Linux reports it); the wire row carries FULL_UNDEFINED so the zero reads as a contract.
- [ ] Memory stall seam: accumulate while a thread waits on reclaim, which needs an allocation path that WAITS instead of failing. -> XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md §2`
- [ ] I/O stall seam: needs a block-layer completion-wait site (§8 throttles SUBMISSION, a different seam). -> XREF: `05-storage-filesystems/TODO-01-block-storage-hardening.md §8`
- [x] `SystemResourcePressureInformation` (info class 0x1003) marshals one coherent snapshot, with a per-resource VALID flag so an uninstrumented seam reads unsupported, never "no pressure".
- [x] The stall lane is INDEPENDENT of the budget lane, not fed into it; the two meet only in `quota_pressure_system_level()`, as a max. -> XREF: `§9`
- [x] Commit: quota: resource-pressure stall telemetry + SystemResourcePressureInformation.

**Test checkpoint:** a synthetic stall interval advances the owning resource's `some` total and no other resource's; the avg10/60/300 windows decay to EXACTLY zero once the stall stops and never exceed 100 percent; a sustained exact-permille sample publishes that permille (not one less) and crosses its hysteresis band; two overlapping waits accrue one window, not two; a token consumed twice cannot cancel a second waiter's stall; an uninstrumented resource reports its VALID flag clear rather than a zero that reads as "no pressure"; `cpu.full` is 0 with the documented undefined-at-system-level contract; the info class round-trips every field through a size-checked buffer.

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 10 suites, 0 failures

> **Notes:**
> - Shipped `quota_stall.c`/`.h` (3 domains, per-CPU cache-line-isolated integrators, Q16 EWMAs) and `nt/quota_pressure_info.h` (248-byte class 0x1003, offsets static-asserted).
> - Aggregation rides the existing 50 ms pressure DPC and folds every 2 s in closed form; producers are CPU-pinned and carry an ownership token, so a task migrating mid-wait still balances.
> - All three seams are owned elsewhere and unwired, so every domain reports VALID clear by design; `quota_pressure_system_level()` folds this lane in as a max. Codex adoptions in the commit message.
> - Canonical doc: the `include/kernel/quota/quota_stall.h` header contract.
> - Scope boundary: §12 owns accumulators, averaging and the query ABI; §9 owns budget-sourced levels; TODO-07 §3, TODO-03 §2 and TODO-01 §8 own the seams.

> **Verified:** 2026-07-25 | commit `bb1cbd25` | 5/8 items | build OK | tests 24511/24511 PASS | smoke PASS (KVM 2.820s)
> **Accepted:** [H] previous-mode is not CPU-local, so a user caller can race `ProbeForWriteIfUser` into being skipped -- pre-existing across every system-info extension class (0x1000-0x1002 predate this section), not introduced here (reason: scope) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Previous-mode is not CPU-local" at line 476)
> **Accepted:** [H] every producer transition reads `uptime_ns()`, which touches a global monotonic floor -- must be replaced before any seam is wired (reason: infra) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Producer clock seam" at line 477)
> **Accepted:** [M] `quota_pressure_system_level()` has no UNKNOWN value, so "unmeasured" and "calm" are indistinguishable at system level (reason: §9 contract) -> XREF: `02-kernel-core/TODO-25 §14` (item: "`quota_pressure_system_level()` has no UNKNOWN value" at line 478)
> **Accepted:** [M] the 16-type budget space carries ONE debounce shared by both sources, so `quota_pressure_submit_stall` has no safe production use and the stall lane deliberately does not touch it (reason: needs per-source lanes) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Per-source lanes in the 16-type budget space" at line 474)
> **Deferred:** [M] the CPU stall seam is unwired: attributing a tick needs a per-CPU current-thread cursor (reason: infra) -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3` (item: "Per-CPU current-thread cursor: `thread_current()` from `g_rq[this_cpu()]`" at line 119)
> **Deferred:** [M] the MEM stall seam is unwired: it needs an allocation path that WAITS on reclaim (reason: infra) -> XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md §2` (item: "Blocking allocation path (wait for reclaim instead of returning NULL)" at line 130)
> **Deferred:** [M] the IO stall seam is unwired: it needs a block-layer completion-wait site (reason: infra) -> XREF: `05-storage-filesystems/TODO-01-block-storage-hardening.md §8` (item: "Wrap the submission delay and any completion wait" at line 183)
> **Deferred:** [M] two-CPU boundary-race regression for the fold (a producer advances a slot between the fold timestamp and the walk) needs the SMP harness (reason: infra) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Two-CPU boundary-race regression for the stall fold" at line 479)
> **Deferred:** [L] KNF publication of stall-lane level transitions: no producer can fire one until a seam lands, so the wire record would ship unexercisable (reason: no producer) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Publish stall-lane level transitions through the §9 ring" at line 475)
> **Quality reviewed:** 2026-07-25 | Codex 16x (design, adversarial x2, test-coverage, consistency, perf, re-adversarial x10) + kernel-quality-auditor + concurrency-evidence-mapper | 16H+20M+5L fixed, 1H rejected, 4 accepted-XREF | scope: kernel-code-quality

---

## 13. Object Manager Charge Points

Split out of the original §11 with §14. The §11 ledger is the owner-side machinery; this section is the CONSUMER side -- the concrete charge/return points inside the Object Manager. Two of its four items are hard-blocked on the TODO-05 §3 reservation/commit primitive (see the NOTE below); the object-body/name-entry charge does not need the handle-table lock and can land on the §11 ledger first.

- [/] Charge handle-table entries on insert, return on close, billing the table OWNER; blocked on the TODO-05 §3 reservation/commit primitive returning NTSTATUS. -> XREF: `02-kernel-core/TODO-05 §3`
- [/] Charge object body and name entry on creation, returning via the ledger at `ob_free_object` / `ObpRemoveFromDirectory`, not a task lookup. Design-reviewed 2026-07-25: FOUR prerequisites, see the NOTE. -> XREF: `§14`
- [/] `NtDuplicateObject` charges the TARGET owner as part of the insert transaction, not a separate precheck (a standalone check is TOCTOU), returning `STATUS_QUOTA_EXCEEDED`. -> XREF: `02-kernel-core/TODO-05 §3`
- [x] Counter-authority boundary pinned in `ob_type.h` + `quota.h`: `OBJECT_TYPE` owns system-wide per-type totals; quota is per-principal over a frozen 16-row taxonomy, charging ONE body-class row per body (orthogonal rows bill separately).
- [x] Commit: quota: object/handle charge integration in Object Manager.

**Test checkpoint:** inserting a handle increments the owner's handle usage and closing it returns the charge exactly; a handle inserted into another process's table bills THAT process, not the caller; `NtDuplicateObject` into a target at its cap fails with `STATUS_QUOTA_EXCEEDED` and inserts no handle; an object body whose creator already died is still returned at `ob_free_object` through the ledger; a task that dies with open handles reaches zero outstanding obligations after `task_cleanup`, and the leak sweep reports no net delta. Shipped now (item 4): repeated body charges accumulate into ONE per-principal `QUOTA_RES_OBJECT_BODY` row that does not alias the name-entry row, while creating an object of one type lifts only that type's live count, each type returns to its own baseline on destruction, and the per-type peak retains its high-water mark. Cross-type folding stays unobservable until the charge points exist, since `quota_charge` takes no type identity (verified: SUITE=quota, 0 failures, 0 quota-leaked).

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 2 §13 suites, 0 failures

> **Notes:**
> - **What shipped** -- the counter-authority contract only: a boundary block at `OBJECT_TYPE.total_objects` (`ob_type.h`), a reciprocal pointer at `QUOTA_RES_OBJECT_BODY`, and 2 suites in `test_quota.c` pinning both halves.
> - **How it runs** -- the per-type test registers its OWN throwaway types (pattern at `test_ob.c:376`), because "the other type did not move" is only race-free for a type no other code can allocate; borrowing `Directory` would race per-process namespace directories.
> - **Downstream effects** -- items 1-3 stay `[/]`: the design review turned one known blocker into five, all owner-side (TODO-05 §3/§4, §14). Codex design adoptions in this section's commit message.
> - **Canonical doc** -- the counter-authority contract at `include/kernel/ob/ob_type.h`.
> - **Scope boundary** -- §13 owns the charge POINTS; §11 the ledger; §14 the ISR-safe return and teardown ordering; TODO-05 §3 the handle-table transaction and §4 namespace teardown plus insert status.

> **Verified:** 2026-07-25 | commit `fc809dac` + review fixes | 1/4 items | build OK | tests 24538/24538 PASS, 0 quota-leaked
> **Deferred:** [H] handle-table entry charging and the `NtDuplicateObject` target-owner charge need a reservation/commit transaction that can report `STATUS_QUOTA_EXCEEDED` (reason: infra) -> XREF: `02-kernel-core/TODO-05 §3` (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 149)
> **Deferred:** [H] the object-body charge needs an ISR-safe ledger return: `ob_free_object` runs in the LAPIC timer ISR via `nt_timer.c:165`, where the last-reference return reaches `kfree` (reason: infra) -> XREF: `02-kernel-core/TODO-25 §14` (item: "ISR-safe ledger return with a deferred destroy" at line 499)
> **Deferred:** [H] the object-body charge needs `quota_ledger_task_release` ordered after the `ACCESS_TOKEN` derefs, or a token body false-leaks on every process exit (reason: infra) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Order `quota_ledger_task_release`" at line 500)
> **Accepted:** [H] a non-empty directory teardown drains no entries, leaking entry nodes and child references today and every name-entry charge later (reason: scope) -> XREF: `02-kernel-core/TODO-05 §4` (item: "Give `Directory` an `on_delete` draining its entry list" at line 185)
> **Accepted:** [H] `ObInsertObject` returns 0/-1, so a name-entry quota refusal cannot be reported as `STATUS_QUOTA_EXCEEDED` (reason: scope) -> XREF: `02-kernel-core/TODO-05 §4` (item: "Status-bearing insertion primitive" at line 186)
> **Accepted:** [H] implicit `task_current()` billing is not SMP-safe; pre-existing class already charged this way by ALPC and KNF, not introduced here (reason: infra) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Bill `quota_charge_current` off a per-CPU task cursor" at line 489)
> **Accepted:** [H] `quota_ledger_return` abandons an obligation after 4 BUSY collisions, which a destructive free cannot retry (reason: needs cross-CPU validation) -> XREF: `02-kernel-core/TODO-25 §14` (item: "Pending-return handoff so a return colliding with a BUSY owner is never abandoned" at line 486)
> **Accepted:** [L] the 64-slot `g_ob_types` table is append-only with no release path, ~37/64 in a full-suite boot (reason: scope) -> XREF: `02-kernel-core/TODO-05 §1` (item: "Budget the `g_ob_types` table" at line 98)
> **Quality reviewed:** 2026-07-25 | Codex 7x (design, adversarial x2, consistency, perf, re-adversarial x2) + kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst | 3M+4L fixed, 5H+1M+1L accepted-XREF | scope: kernel-code-quality

> [!NOTE]
> **Blocked on the handle-table lock (TODO-05 §3)** -- items 1 and 3. `HANDLE_TABLE` has no lock and no owning-task back-pointer, and `ObpAllocateHandle` collapses every failure into `INVALID_HANDLE_VALUE`, so a charge added today would admit quota for an entry a concurrent insert can overwrite, bill `task_current()` on the inherit and cross-process duplicate paths, and be unable to report `STATUS_QUOTA_EXCEEDED` distinctly. The charge must join the reservation/commit transaction, not sit beside it.
>
> **Blocked on four owner-side prerequisites (design review 2026-07-25)** -- item 2. This section is the CONSUMER side; every blocker below is owned elsewhere, so a half-built charge point would ship a known-wrong accounting path rather than an incomplete one:
> 1. **ISR-reachable object free.** `nt_timer_tick` drops the last `TIMER_OBJECT` reference inside the LAPIC timer ISR (`src/kernel/nt/nt_timer.c:165`, from `lapic.c:1180`), so `ob_free_object` runs at ISR. A `quota_ledger_return` there can take the last ledger reference and reach `quota_ledger_destroy` -> `kfree` of up to 64 chunks; skipping the return at elevated IRQL leaks the charge instead. -> XREF: `§14`
> 2. **Teardown ordering.** `quota_ledger_task_release` (`src/kernel/sched/task.c:3489`) runs BEFORE the `ACCESS_TOKEN` derefs at `task.c:3596`/`:3609`, and token bodies are `ob_alloc_object` allocations -- so a body obligation would be reclaimed-as-orphan and counted a leak on EVERY process exit, breaking the green `0 quota-leaked` sweep. -> XREF: `§14`
> 3. **No Directory `on_delete`.** `ObpDirectoryType` is registered bare (`src/kernel/ob/ob.c:809`), so a non-empty directory teardown walks no entries -- it already leaks entry nodes and child references today, and would leak every name-entry charge. -> XREF: `02-kernel-core/TODO-05 §4`
> 4. **No NTSTATUS channel on insert.** `ObInsertObject` returns 0/-1 (`include/kernel/ob/ob_ns.h:65`), conflating name collision, capacity, allocation failure, and a quota refusal -- so the required `STATUS_QUOTA_EXCEEDED` is unreportable for the name-entry charge. -> XREF: `02-kernel-core/TODO-05 §4`
>
> **Implementation constraints for whoever unblocks item 2:**
> - **Charge exactly ONE BODY-CLASS row per body creation** -- the dedicated row where one exists (Timer, Thread, Process, Section, and NotificationState, which KNF already charges), otherwise `QUOTA_RES_OBJECT_BODY`; never both. Orthogonal rows are charged INDEPENDENTLY on their own events (`QUOTA_RES_NAMESPACE_ENTRY` per named entry, `_HANDLE` per table entry, `_MAPPED_VIEW` at map time on a `SECTION_VIEW` -- there is no MappedView object type, `_NOTIFICATION_BYTES` for retention), so one creation may charge a body row AND orthogonal rows. Pinned in the `ob_type.h` counter-authority block; consistency review caught the rule being left implicit and the re-adversarial round corrected a first draft that said "exactly one row" and both misclassified `_MAPPED_VIEW` and omitted NotificationState.
> - The obligation tail-packs into the object block past the optional creator SD, so its offset needs an overflow-checked `ALIGN_UP` to `_Alignof(quota_obligation_t)` and the PMM free-size reconstruction in `ob_free_object` must include that padding (`ob.c:145-156`, `:272`). Attribution also still rides the global `task_current()` cursor.

---

## 14. Charge-Path Cost and Lifetime Follow-ups

Split out of the original §11 with §13. The first four items were each filed by ANOTHER section's review (§6 ALPC, §7 job accounting, §9 victim nomination) as in-scope-but-not-now cost/ABI/lifetime work against an already-shipped charge path. The last three came from §11's own review rounds and concern the ledger and its charge path directly: converting the embedded-receipt consumers so their pre-join receipts become migratable, a pending-return handoff that would remove the abandon path, and bounding the adjust's IRQ-off window. All seven need work or validation infrastructure outside §11's scope, so they are tracked here rather than gating it.

- [ ] Cut the `ob_job_collect_accounting` lock hold: it scans up to 32 members with per-member delta math while holding `job->lock` with IRQs off. Snapshot under the lock, compute outside. -> XREF: `02-kernel-core/TODO-25 §7`
- [ ] Compact per-chain charge receipt: the 8-slot `quota_charge_receipt_t` embedded in every `PORT_MESSAGE_ENTRY` grows it 56 -> 152 bytes for a 3-block chain. -> XREF: `02-kernel-core/TODO-25 §6`
- [ ] Reserve-then-commit charge so a refused send does not first allocate: `AlpcAllocateMessage` kmallocs up to 64 KiB before the quota refusal. -> XREF: `02-kernel-core/TODO-25 §6`
- [ ] Lifetime-safe task enumeration for process-level victim nomination: TODO-25 §9 nominates by USER principal because `task_get_by_pid` returns a raw slot with no reference and no System-protected flag exists. -> XREF: `§9`
- [ ] Convert the embedded-receipt charge consumers (ALPC `PORT_MESSAGE_ENTRY`, KNF notification state) to `quota_ledger_charge` obligations, so a pre-join receipt is enumerable and `quota_ledger_migrate_to_job` can adopt it. -> XREF: `§11`
- [ ] Pending-return handoff so a return colliding with a BUSY owner is never abandoned: the returner marks the receipt, the owner consumes it before republishing. -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`
- [ ] Bound `quota_charge_adjust`'s IRQ-off window: it holds up to `QUOTA_CHAIN_MAX` block locks while masked, so contention makes the interval unbounded. Needs ordered try-lock plus backoff. -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`
- [ ] Bound the per-task charge-gate CAS loops: `quota_gate_enter`/`quota_gate_exit` spin `for(;;)` with no retry bound and no backoff, on a path reachable with interrupts masked. -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`
- [ ] Bill `quota_charge_current` off a per-CPU task cursor: `task_current()` reads the global `current_task`, the defect class `quota.h` cites when it refuses `QUOTA_CHARGE_CLIENT`. -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`
- [ ] Move the `KERNEL_TESTS` writer instrumentation out of `quota_charge_adjust`'s IRQ-off window: `quota_block_lock` runs `quota_writers_enter` before each acquire, contradicting the rule its own exit path states. -> XREF: `§11`
- [ ] Make KNF state creation status-bearing: `knf_create_state` returns a pointer, so a transient `STATUS_RETRY` from the charge gate flattens to NULL and one-shot callers treat it as permanent. -> XREF: `02-kernel-core/TODO-16 §2`
- [ ] Per-charge attribution on `quota_charge_receipt_t`: it carries only type and amount, so a task near a limit cannot be broken down by charging subsystem the way NT pool tags allow. -> XREF: `§10`
- [ ] Per-source lanes in the 16-type budget space so `quota_pressure_submit_stall` becomes usable: today one shared debounce means whichever source samples more often owns the level. -> XREF: `§12`
- [ ] Publish stall-lane level transitions through the §9 ring: needs a stall-domain-keyed record, and no producer can fire one until a §12 seam lands. -> XREF: `§12`
- [ ] Previous-mode is not CPU-local, so a user caller can race `ProbeForWriteIfUser` into being skipped; affects EVERY system-info extension class (0x1000-0x1003), not just §12. -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`
- [ ] Producer clock seam: `quota_stall` producers call `uptime_ns()` on every transition, touching a global monotonic floor; needs a per-CPU or caller-supplied timestamp BEFORE any stall seam is wired. -> XREF: `§12`
- [ ] `quota_pressure_system_level()` has no UNKNOWN value: it returns NORMAL when every domain is invalid, so "unmeasured" and "calm" are indistinguishable at the system level. -> XREF: `§9`
- [ ] Two-CPU boundary-race regression for the stall fold (a producer advances a slot between the fold timestamp and the walk); needs the SMP test harness. -> XREF: `§10`
- [ ] ISR-safe ledger return with a deferred destroy: `ob_free_object` runs at ISR via `nt_timer.c:165`, where the last-reference return reaches `quota_ledger_destroy` -> `kfree` of up to 64 chunks. -> XREF: `§13`
- [ ] Order `quota_ledger_task_release` (`task.c:3489`) AFTER the `ACCESS_TOKEN` derefs (`task.c:3596`, `:3609`): token bodies are `ob_alloc_object` allocations, so a body charge would false-leak on every exit. -> XREF: `§13`
- [ ] Commit: quota: charge-path cost + lifetime follow-ups.

**Test checkpoint:** `ob_job_collect_accounting` computes its per-member deltas outside `job->lock` and its lock-held window is asserted against the §5 charge-path cost budget; a 3-block chain receipt fits the compacted `PORT_MESSAGE_ENTRY` and a chain deeper than the compact form still returns exactly; a send refused by quota performs no `AlpcAllocateMessage` allocation at all (injected-failure count unchanged); victim nomination enumerates tasks under a reference that survives a concurrent exit.

---

## OS Comparison

| ⭐   | Feature                          | 🪟 Win11                               | 🐧 Linux                                     | 🚀 Impossible OS                                              |
| --- | -------------------------------- | ------------------------------------- | ------------------------------------------- | ------------------------------------------------------------ |
| 💎   | Unified resource-type registry   | ⚠️ scattered across subsystems        | ⚠️ split rlimit/cgroup/quotactl             | ✅ one 16-type registry §1                                    |
| 💎   | Central quota/charge API         | ✅ `PsChargeProcessQuota` per pool     | ⚠️ split: rlimits + cgroups, no unified API | ✅ one `quota_charge`/`return` §2                             |
| 💎   | Atomic quota transfer            | ⬜ none (charge/return only)           | ⬜ none (no cross-principal move)            | ✅ all-or-nothing two-block transfer §2                       |
| 💎   | Per-type peak + failure counts   | ⚠️ peak only, no per-type failures    | ⚠️ `memory.events` per-cgroup, not per-type | ✅ peak + saturating failures per type §2                     |
| 💎   | Per-token quota block            | ✅ `EPROCESS`/token `QUOTA_BLOCK`      | ⬜ none (uid/cgroup based)                   | ✅ token+process+job blocks §3                                |
| 💎   | All-or-nothing chain charge      | ⚠️ per-block, no cross-layer rollback | ⚠️ per-cgroup, no receipt for the return    | ✅ receipt-bound chain charge §3                              |
| ⭐   | Per-user aggregate rollup        | ⚠️ per-process/job, no per-SID view   | ⚠️ per-cgroup, not per-uid across cgroups   | ✅ canonical per-SID block + rollup §3                        |
| 💎   | Receipt identity (ABA-proof)     | ⬜ none (no receipt abstraction)       | ⬜ none (no receipt abstraction)             | ✅ tagged generation token per charge §4                      |
| 💎   | Obligation outlives its creator  | ⚠️ quota is per-EPROCESS, no receipt  | ⚠️ `obj_cgroup` pins past exit, no receipt  | ✅ refcounted ledger, slot-independent §11                    |
| 💎   | Charge vs membership barrier     | ⚠️ pre-join usage not absorbed        | ⚠️ cgroup v2 does not move charges on move  | ✅ drain/quiesce gate on every charger §11                    |
| 💎   | Transactional charge resize      | ⬜ none (charge/return only)           | ⬜ none (no resize primitive)                | ✅ prevalidate-then-commit, all-or-none §11                   |
| 💎   | Handle/object quota              | ✅ per-process handle quota            | ⚠️ `RLIMIT_NOFILE` fd-only                  | ⚠️ Partial: dimensions + boundary §13; charge points blocked |
| 💎   | Paged/nonpaged pool quota        | ✅ pool quota per process              | ⚠️ slab accounting via memcg, not per-proc  | 🚀 Planned: allocator-hook charging §5                        |
| 💎   | Enforced charge-path cost budget | ⬜ none (no published charge cost)     | ⬜ none (cost is per-controller, unstated)   | ✅ exact lock-section budget asserted §5                      |
| 💎   | Notification-state quota         | ⚠️ WNF has no per-user state cap      | ⬜ none (inotify caps are per-fd, not user)  | ⚠️ state/sub/retention charged, uncapped §6                  |
| 💎   | Registry/IPC quota               | ✅ registry + ALPC quotas              | ⚠️ no registry; IPC via `RLIMIT_MSGQUEUE`   | ⚠️ ALPC charged §6; registry blocked T14                     |
| 💎   | Admin-configurable per-user caps | ⚠️ registry-set, no live re-limit     | ✅ cgroup limits apply to live cgroups       | ⚠️ live re-limit works; no setter yet §6                     |
| ⭐   | CPU/IO/wakeup accounting         | ✅ Job Objects + power throttling      | ✅ cgroup cpu/io/pids controllers            | ✅ per-proc/job CPU+IO+wakeup+timer §7                        |
| ⭐   | Job aggregate accounting window  | ⚠️ member lifetime totals folded in   | ✅ cgroup counts only while a member         | ✅ membership-interval deltas, baselined §7                   |
| 💎   | Control ("Other") I/O counters   | ✅ `IO_COUNTERS.Other*` populated      | ⚠️ no ioctl split in `/proc/PID/io`         | ⚠️ counters+ABI wired; event source T05 §14                  |
| ⭐   | Rate-limit policy record         | ⚠️ per-Job CPU rate cap only          | ⚠️ per-controller, no shared record shape   | ✅ versioned typed record, seqlock §7                         |
| 💎   | Native query/set quota syscalls  | ✅ `NtQueryInformationProcess` classes | ✅ `getrlimit`/`prlimit64`                   | ✅ `ProcessQuotaLimits` query/set §8                          |
| 💎   | Quota set as one transaction     | ⚠️ per-field, no documented atomicity | ⚠️ one resource per `prlimit64` call        | ✅ prevalidate-then-commit, all-or-none §8                    |
| ⭐   | Job aggregate limit query        | ⚠️ memory fields only, no per-type    | ✅ per-controller cgroup files               | ✅ per-resource usage/peak/limit class §8                     |
| ⭐   | Resource pressure events         | ✅ low-memory notifications            | ✅ PSI (`/proc/pressure/*`)                  | ⚠️ 4-level hysteresis, budget-sourced §9                     |
| ⭐   | Pressure source honesty          | ⬜ single opaque low-memory signal     | ⚠️ PSI has no per-source validity flag      | ✅ source kind + VALID, unknown != calm §9                    |
| 💎   | Structured quota-failure event   | ⚠️ ETW pool events, no per-charge rec | ⬜ none (errno only, no event)               | ✅ versioned record + rate limit + drops §9                   |
| ⭐   | Per-resource stall telemetry     | ⚠️ no PSI equivalent surfaced         | ✅ some/full avg10/60/300 per resource       | ⚠️ PSI-shaped + VALID §12; seams unwired                     |
| 💎   | Stall metric honesty flag        | ⬜ none (no PSI-style metric)          | ⬜ zero and unmeasured are indistinguishable | ✅ per-domain VALID + FULL_UNDEFINED §12                      |
| ⭐   | Last-resort OOM recovery         | ⬜ none (cooperative trim only)        | ✅ cgroup `memory.oom.group`                 | ⚠️ cooperative nomination only, no kill §9                   |
| ⭐   | Unified leak sweep + quota_dump  | ⚠️ pool-tag tracking, no boot sweep   | ⚠️ slabinfo, no per-boot delta sweep        | ✅ per-category delta sweep, CI-gated §10                     |
| ⭐   | Crash-time quota dashboard       | ⚠️ `!poolused` needs a live debugger  | ⬜ none (no quota state in a kernel oops)    | ✅ non-blocking panic-path dump §10                           |

---

## Unit Tests

> Test file: `src/kernel/test/test_quota.c`, registered via `test_register_quota()` in `test_runner_init()`. Sibling files split by surface: `test_quota_owner.c`, `test_quota_perf.c`, `test_quota_config.c`, `test_quota_syscall.c` (§8), `test_quota_pressure.c` (§9), `test_quota_dashboard.c` (§10 dashboards, leak sweep, charge-path bulletproofing), `test_quota_ledger.c` (§11 ledger, gate, adjust, migration), and `test_quota_stall.c` (§12 stall telemetry), each with its own `test_register_*` call. All quota assertions land under the dedicated `TEST_CAT_QUOTA` category (run via `SUITE=quota`). Use `TEST_PENDING` for assertions gated on a not-yet-shipped section.

- [x] `test_quota_charge_return_roundtrip`: charge then return leaves usage 0 and peak recorded (§2).
- [x] `test_quota_over_limit_rejected`: over-limit charge returns `STATUS_QUOTA_EXCEEDED`, usage unchanged (§2).
- [/] `test_quota_concurrent_charges_atomic`: N INTERLEAVED multi-thread charges sum exactly, no lost update (§2). True cross-CPU contention needs per-CPU run queues. -> XREF: `§10` (test infrastructure).
- [x] `test_quota_rollback_on_partial_failure`: a refused transfer leaves both blocks byte-identical; a refused charge leaves usage unchanged (§2).
- [ ] `test_quota_type_registry`: all resource types registered with name/unit/limit (§1).
- [x] `test_quota_chain_all_or_nothing`: a charge admitted by the process block but refused later in the chain rolls the prefix back, stranding no usage (§3).
- [ ] `test_quota_job_chain_rollback`: charge exceeding an ancestor job rolls back across the whole chain (§3).
- [ ] `test_quota_inherit_on_create`: child auto-joins parent job chain unless breakaway; jobless child charges own block (§3).
- [x] `test_quota_rollup_counts_user_layer_once`: the per-SID query aggregates the USER layer only, so a chain charge is counted once rather than per layer (§3).
- [x] `test_quota_receipt_token_blocks_stale_return`: a spent token cannot return a later charge in recycled receipt storage (§4).
- [x] `test_quota_receipt_generation_exhaustion`: the last generation still charges and the ceiling fails closed instead of wrapping (§4).
- [x] `test_quota_zero_charge_reports_no_obligation_token`: a zero charge reports token 0, which cannot return the next real charge (§4).
- [ ] `test_quota_pressure_hysteresis`: levels derive from the §12 stall-time metrics and do not flap at a threshold (§9).
- [ ] `test_quota_failure_event_fields`: a quota failure emits the diagnostic event with all contract fields, rate-limited (§9).
- [x] `test_quota_ledger_obligation_outlives_task_claim`: an obligation stays returnable after the task released its own ledger claim, and is NOT reported as a leak (§11).
- [x] `test_quota_charge_adjust_refused_changes_nothing`: an adjust refused past the limit moves no usage and lifts no peak; `_both_directions` and `_requires_the_token` cover the rest (§11).
- [x] `test_quota_ledger_migrate_adopts_absorb_record`: migration leaves the job's usage unchanged and its return credits the job; `_unmigrate_restores_absorb_record` proves a refused join (§11).
- [ ] `test_quota_handle_insert_close`: handle insert/close increments/decrements the OWNER's handle usage (§13). Blocked with its item on TODO-05 §3.
- [ ] `test_quota_duplicate_handle_target_cap`: DuplicateHandle into a capped target fails (§13). Blocked with its item on TODO-05 §3.
- [x] `test_quota_object_rows_aggregate_without_aliasing`: repeated body charges accumulate into one per-principal row that does not alias the name-entry row (§13).
- [ ] Cross-type folding: bodies of DIFFERENT `OBJECT_TYPE`s land in the one `QUOTA_RES_OBJECT_BODY` row. Unobservable until the charge points exist -- `quota_charge` takes no type identity (§13).
- [x] `test_ob_type_counters_are_per_type_authoritative`: two PRIVATE throwaway types (no built-in type touched); each one's live count moves independently, returns to baseline, and keeps its peak (§13).
- [ ] `test_quota_job_collect_lock_window`: `ob_job_collect_accounting` computes member deltas outside `job->lock` (§14).
- [ ] `test_quota_pool_owner_charged`: tagged pool alloc charges its quota owner; free returns it (§5).
- [ ] `test_quota_registry_data_cap`: registry value over data-byte cap rejected (§6).
- [x] `test_quota_sweep_classification` + the runner's per-category sweep: every verdict branch (clean / count-only / leaked / indeterminate) asserted from synthetic snapshots, and a real run reports `0 quota-leaked` (§10).
- [x] `test_quota_cpu_split_monotonic`: user/kernel CPU time and the sample timestamp never walk backwards (§7).
- [x] `test_quota_io_split_by_op`: a control op moves only the control counters, leaving read/write untouched (§7).
- [x] `test_quota_control_io_zero_byte_counts_op`: a zero-byte control counts as an op and contributes no bytes (§7).
- [x] `test_quota_wakeup_only_from_blocked`: of six wake notifications only the BLOCKED one counts (§7).
- [x] `test_quota_sample_stamped`: a sample carries its own timestamp; a NULL task samples to all zeroes (§7).
- [x] `test_quota_membership_delta_excludes_prejoin`: a job delta excludes pre-baseline usage; a fresh baseline yields zero (§7).
- [x] `test_quota_delta_saturates_on_reset`: an inverted baseline saturates to 0 instead of wrapping to ~2^64 (§7).
- [x] `test_quota_rate_record_roundtrip`: every policy field round-trips and one class does not leak into another (§7).
- [x] `test_quota_rate_record_rejects_bad`: bad version/flag/unit/period/range and reservation-above-cap are refused, stored policy intact (§7).
- [x] `test_quota_rate_generation_advances`: the generation advances on success and never on a refused publication (§7).
- [x] `test_quota_rate_unset_reports_no_policy`: an unset class reports flags 0 and generation 0 (§7).
- [x] `test_quota_rate_separate_from_usage`: publishing policy charges no usage and a charge does not alter policy (§7).
- [x] `test_quota_rate_unflagged_fields_canonicalized`: unflagged amounts and a CPU byte envelope read back as zero, not caller garbage (§7).
- [x] `test_quota_rate_ops_and_bytes_coexist`: an IOPS cap and a bytes/sec cap on one stream do not overwrite each other (§7).
- [x] `test_quota_delta_covers_every_field`: all 8 baseline fields assert positive, equal, and inverted deltas independently (§7).
- [x] `test_quota_rate_get_retry_preserves_output`: a forced in-flux class returns `STATUS_RETRY` leaving the caller's record untouched (§7).
- [x] `test_quota_syscall_abi_pinned`: `QUOTA_LIMITS` 48 B / `QUOTA_LIMITS_EX` 88 B and every field offset, re-checked at runtime (§8).
- [x] `test_quota_syscall_time_conversion`: both spellings of unlimited translate, a sub-second limit rounds UP, an unrepresentable count saturates positive (§8).
- [x] `test_quota_syscall_projection_reads_real_state`: pool limits come from the process block and `TimeLimit` from `RLIMIT_CPU`, not from a fabricated default (§8).
- [x] `test_quota_syscall_raise_needs_privilege`: lowering own limit is unprivileged; raising it, or removing the cap by writing 0, is refused and changes nothing (§8).
- [x] `test_quota_syscall_set_is_all_or_nothing`: a mixed lower-plus-raise request commits neither half, and leaves `RLIMIT_CPU` untouched (§8).
- [x] `test_quota_syscall_set_preserves_hard_limit`: a soft `TimeLimit` write leaves `rlim_max` intact; a soft value above the hard cap clamps (§8).
- [x] `test_quota_syscall_set_rejects_malformed`: negative time, min-above-max, contradictory flags, unknown flags, non-zero Reserved, rate above 100 (§8).
- [x] `test_quota_syscall_non_ex_ignores_suffix`: a 48-byte request ignores rather than validates or stores the EX suffix (§8).
- [x] `test_quota_syscall_handle_limit_reconcile`: the inverted zero encodings reconcile, so `RLIMIT_NOFILE` 0 is deny-all, not unlimited (§8).
- [x] `test_quota_syscall_reset_clears_policy`: a reused task slot presents no limits from the dead tenant (§8).
- [x] `test_quota_syscall_generation_advances_on_commit`: the generation advances once per committed set and never on a refused one (§8).
- [x] `test_quota_syscall_job_report_shape`: a jobless report is well-formed and fully zeroed, one row per registered resource type (§8).
- [x] `test_quota_syscall_job_set_rejects_malformed`: a request writing the kernel-owned Usage column is refused (§8).
- [x] `test_quota_rate_boundary_matrix`: exact-MAX and equal-bound acceptance, both reserved fields, bytes-envelope rules, and per-class isolation (§7).
- [x] `test_stall_domain_isolation`: a stall in one domain advances only that domain's totals and averages (§12).
- [x] `test_stall_overlapping_waits_not_double_counted`: two waiters across one window accrue one window, not two (§12).
- [x] `test_stall_double_consume_cannot_cancel_another_waiter`: consuming a token twice cannot clear a second waiter (§12).
- [x] `test_stall_exact_permille_is_published_exactly`: a sustained exact permille publishes that value, not one less (§12).
- [x] `test_stall_exact_threshold_crosses`: a sustained exact-threshold sample crosses into WATCH (§12).
- [x] `test_stall_sub_permille_is_not_erased`: a 0.1-permille stall still lands in the cumulative total (§12).
- [x] `test_stall_uninstrumented_reports_unknown`: every unwired domain reports VALID clear, never a calm zero (§12).
- [x] `test_stall_cpu_full_is_undefined`: `cpu.full` is 0 with its flag set; `mem.full` accrues and never exceeds some (§12).
- [x] `test_stall_averages_decay_to_exactly_zero`: all three windows reach exactly 0, cumulative totals survive (§12).
- [x] `test_stall_averages_saturate_at_full_scale`: sustained stall converges near 1000 and never past it (§12).
- [x] `test_stall_over_long_stalled_gap_keeps_its_measurement`: a gap longer than every window keeps its pressure (§12).
- [x] `test_stall_non_advancing_clock_closes_no_window` / `_backwards_clock_reanchors`: no phantom windows (§12).
- [x] `test_stall_lane_independent_of_budget_lane`: 40 budget passes cannot move the stall lane (§12).
- [x] `test_stall_info_abi_pinned` / `_roundtrip` / `_rejects_short_buffer`: class 0x1003 layout and marshalling (§12).
- [/] Two-CPU concurrent wakers must count ONE wakeup for one BLOCKED->READY claim; the CAS is in place but a real cross-CPU race needs per-CPU run queues. -> XREF: `§10` (test infrastructure).
- [/] A reader racing a rate publisher must see either the whole old or whole new record, never a mix; needs the same SMP harness. -> XREF: `§10` (test infrastructure).
- [/] A named timer whose handle allocation fails is still published and counted; proving it needs an exhausted handle table. -> XREF: `§13` (handle quota integration).

**Test checkpoint:** all `test_quota_*` cases pass under the kernel runner; pending assertions render `[STUB]` via `TEST_PENDING` until their owning section ships; the boot leak sweep prints a zero-delta line for the quota category.

---

## Verification

- [ ] `bash scripts/build.sh` -> `=== BUILD OK ===`.
- [ ] `bash scripts/test.sh SUITE=quota QUIET=1` -> quota suites pass, 0 failures.
- [ ] `grep -R "quota_charge" src/kernel` shows every charge path names a resource type and owner.
- [ ] `quota_dump()` output appears in the boot serial log and in a forced crash dump.
- [ ] Verify on bare metal -- VM behavior differs for pool/working-set counters.

**Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 9 suites, 0 failures
