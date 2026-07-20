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
| 💎   |   8   | Native query/set quota syscalls               | T12 §10                 |  [ ]   |
| ⭐   |   9   | Resource pressure events and recovery hooks   | T16 §2, T30 §6          |  [ ]   |
| 💎   |  10   | Tests, leak sweeps, and dashboards            | §1..§9                  |  [ ]   |
| 💎   |  11   | Object and handle quota integration           | §4, T05 §3, T05 §14     |  [ ]   |

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
> **Deferred:** [M] cross-CPU contention proof: the scheduler is single-CPU today, so the multi-thread suites show interleaving, not parallel contention (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path" at line 251)
> **Deferred:** [L] bounded worker join + CPU-pinned fault injection for the quota tests (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Bounded worker join + CPU-pinned `kmalloc_fail_next`" at line 252)
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
> **Accepted:** [H] absorbed pre-join usage stays billed to the job until the member departs, and a charge can straddle the absorb-to-publish window; both need a drain/quiesce transaction plus a refcounted ledger that ADOPTS `job_absorb` (reason: not-functional-today) -> XREF: `02-kernel-core/TODO-25 §11` (item: "Serialize charges against membership transitions" at line 244)
> **Accepted:** [M] `ACCESS_TOKEN.UserSid` has no recorded extent, so the bounded-capture `owner_len` is caller-derived and cannot detect a truncated SID (reason: scope) -> XREF: `02-kernel-core/TODO-15 §4` (item: "Record a VALIDATED `UserSid` length in `ACCESS_TOKEN`" at line 334)
> **Deferred:** [M] `QUOTA_CHARGE_CLIENT` fails closed with `STATUS_NOT_SUPPORTED`: billing an impersonated client needs a stable per-CPU current-thread cursor and a teardown-safe token-slot pin (reason: infra) -> XREF: `02-kernel-core/TODO-15 §4` (item: "Teardown-safe primary-token READ pin" at line 333)
> **Accepted:** [M] `ob_job_create` inserts a named job into the object namespace before allocating its handle, so a handle-alloc failure leaks the directory entry, the body, and now its quota block (reason: scope, pre-existing Job-Object lifecycle) -> XREF: `02-kernel-core/TODO-21 §14` (item: "`ob_job_create` inserts a named job into `\BaseNamedObjects`" at line 463)
> **Accepted:** [H] two concurrent task constructors can claim the same slot and reset a live lock word; pre-existing and systemic across every per-process inheritance, not introduced here (reason: scope) -> XREF: `03-memory-concurrency/TODO-06 §13` (item: "Atomic task-slot CLAIM" at line 405)
> **Quality reviewed:** 2026-07-20 | Codex 14x (design, adversarial, re-adversarial, consistency, perf, test-coverage) | 12H+13M+4L fixed, 1H+3M accepted-XREF | scope: kernel-code-quality

---

## 4. Receipt Identity (Generation-Tokened Charges)

- [x] `quota_charge_receipt_t.tag` packs a monotonic generation WITH the IDLE/ACTIVE/BUSY state in one 64-bit word, mutated only by `atomic64_cmpxchg`: a generation compared beside the state CAS is an ABA in either order.
- [x] `quota_charge_chain` issues that generation as a caller-held token, written ONLY on success; `quota_return_chain(receipt, token)` CASes the exact `{token, ACTIVE}`, so a stale returner no-ops and storage is recyclable. -> closes `§3`
- [x] Generation exhaustion fails closed with `STATUS_INTEGER_OVERFLOW` at `QUOTA_RECEIPT_GEN_MAX` (62 bits) rather than wrapping a token back onto a live one.
- [x] `out_token` is MANDATORY and non-canonical tokens (above the 62-bit generation field) are refused: the tag encoding discards high bits, so `live_token | (1 << 62)` would otherwise build the same tag and win the CAS.
- [x] `atomic64_cmpxchg` added to `kernel/atomic.h` (LOCK CMPXCHG on an aligned qword).
- [/] Serializing a charge against a job-membership change needs a drain/quiesce transaction, NOT a generation revalidated after charging: the absorb has already folded the in-flight charge in, so a retry double-bills the job. -> XREF: `§11`
- [/] Migrating outstanding receipt obligations at `ob_job_assign` needs a refcounted ledger outliving the task slot, and must ADOPT `job_absorb` not charge beside it. -> XREF: `§11`
- [x] Commit: quota: receipt generation tokens for safe charge identity.

**Test checkpoint:** a token from an already-returned charge cannot return a later charge made through the same receipt storage, while the live token still can; zero, unissued, and non-canonical (high-bit) tokens are all no-ops; a charge refused because the receipt is already live leaves the live charge's token intact; a charge with no `out_token` is refused rather than left unreturnable (verified: SUITE=quota 522 assertions, 0 failures).

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 522 assertions, 0 failures

> **Notes:**
> - **What shipped** -- a tagged `{generation, state}` receipt word in `quota.h`/`quota_owner.c` with a mandatory caller-held token keying the return, plus `atomic64_cmpxchg` in `kernel/atomic.h` as the one primitive it needs.
> - **How it runs** -- every receipt transition is ONE `atomic64_cmpxchg` on the packed word: charge claims IDLE at the current generation, fills the receipt, then release-publishes `{gen+1, ACTIVE}`; return CASes the exact `{token, ACTIVE}` it was handed.
> - **Downstream effects** -- closes the §3 receipt-storage ABA; the absorb-to-publish half stays open and moved to §11, which now owns the charging consumers, the refcounted ledger, and the membership drain protocol.
> - **Canonical doc** -- `include/kernel/quota/quota.h` (receipt identity contract).
> - **Scope boundary** -- §4 owns only the IDENTITY of a charge; nothing is charged yet (§11 objects/handles, §5-§7 pool/registry/CPU), membership serialization is §11, and the handle-table lock a charge-on-insert needs is TODO-05 §3.
> **Accepted:** [M] the receipt-tag guarantees are proven only by sequential tests: concurrent same-token returners, a paused BUSY window, and cross-CPU publication visibility need per-CPU run queues that do not exist yet (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag" at line 251)
> **Deferred:** [H] the absorb-to-publish window stays open: a generation revalidated after charging turns the under-count into a DOUBLE count, so closing it needs a drain/quiesce transaction (reason: design) -> XREF: `02-kernel-core/TODO-25 §11` (item: "Serialize charges against membership transitions" at line 244)

---

## 5. Pool and Allocation Quota Integration

- [/] Charge-path cost characterized END-TO-END, confirming the 7-section depth-2 lifetime: `QUOTA_BUDGET_*` pin owner AND block sections, asserted for EQUALITY. UNCONTENDED only; a latency budget needs cross-CPU queues. -> XREF: `§10`
- [x] `quota_block_t` counter layout decided from that measurement: one 32-byte record per type, not four parallel arrays, so a charge touches 32 contiguous bytes. No alignment claimed (`kmalloc` gives 16); measured 7 of 14 on one line.
- [/] Add optional quota owner to tagged allocations. BLOCKED: the canonical `PTAG_*` API does not exist; only `kmalloc_tagged`/`kfree_tagged` ship, with no per-tag counters. -> XREF: `03-memory-concurrency/TODO-03 §6`
- [/] Charge nonpaged/paged pool through allocator provider hooks. BLOCKED: no pool-class allocator exists to hook -- one unified arena, no paged/nonpaged split, no provider seam. -> XREF: `03-memory-concurrency/TODO-03 §7`
- [/] Ensure kernel-internal early boot allocations are charged to System. BLOCKED with the hook: `kmalloc` is live from Phase 0, the registry validates at Phase 2, System exists at Phase 3. -> XREF: `03-memory-concurrency/TODO-03 §7`
- [/] Refuse user-triggered unbounded allocation paths without quota owner. BLOCKED with the hook: the refusal belongs at a pool entry point that does not exist yet. -> XREF: `03-memory-concurrency/TODO-03 §7`
- [x] Commit: quota: charge-path cost budget + per-type counter records.

**Test checkpoint:** an admitted charge, a refused charge, an over-return, and a return each enter exactly one critical section, a transfer exactly two (refused included), `set_limit` one, and every SINGLE-BLOCK argument rejection or no-op zero; a chain charge costs the 3 owner-snapshot sections plus one per charged layer while its return costs only the per-layer ones, a mid-chain refusal adds exactly one rollback section, and a zero-amount chain charge still costs one owner section (it is not free -- it validates the task); a counter record is 32 bytes at a pinned block offset and never spans more than two cache lines at the block's real runtime address; advisory TSC suites report rather than assert, and label a sample whose CPU tag cannot be verified (verified: SUITE=quota 62 suites / 593 assertions, 0 failures; measured on TCG: depth-2 chain = 5 sections to charge + 2 to return = the 7-section lifetime, 148 cycles per charge+return pair, 892 per chain pair).

> **Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 62 suites, 0 failures

> **Notes:**
> - **What shipped** -- `test_quota_perf.c` (7 suites) plus the `QUOTA_BUDGET_*` charge-cost contract in `quota.h` and a `quota_counters_t` record-per-type layout in `quota.c` replacing four parallel counter arrays.
> - **How it runs** -- block and owner locks funnel through one counting helper, armed by `quota_test_lock_count_begin` and CPU-scoped, and counted on the UNLOCK side so no instrumentation runs inside an IRQ-disabled window.
> - **Downstream effects** -- closes the counter-layout question §2 and §3 deferred here; the four pool-charging items are BLOCKED and now carry reciprocal items in TODO-03 §6/§7.
> - **Canonical doc** -- `include/kernel/quota/quota.h` ("Charge-path cost contract").
> - **Scope boundary** -- §5 owns the cost budget and counter layout ONLY; the pool allocator, its tag API, and the provider seam are TODO-03 §6/§7, and CONTENDED latency needs cross-CPU run queues (§10).
> **Verified:** 2026-07-20 | commit `1907f55c` + review fixes | 1/6 items | build OK | smoke PASS (TCG 4.360s), tests 62 suites/593 PASS
> **Deferred:** [H] pool and tagged-allocation charging: no pool-class allocator exists to hook, so all four charging items stay open (reason: infra) -> XREF: `03-memory-concurrency/TODO-03 §7` (item: "Charge paged/nonpaged pool allocations through a quota provider hook")
> **Deferred:** [H] a real latency budget (throughput + p50/p99 IRQ-disabled hold time under simultaneous chargers) cannot be built on a single-CPU scheduler; the shipped budget is structural only (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag" at line 251)
> **Accepted:** [M] EXACTLY half the counter records straddle a line at every legal block address (24-byte prefix, 32-byte records); fixing the ratio needs the array offset to be a multiple of 32, not just an aligned allocation (reason: not-functional-today) -> XREF: `03-memory-concurrency/TODO-03 §7` (item: "Cache-line-aligned pool allocation for quota counter records")
> **Accepted:** [M] the record co-locates `limit`/`failures` with the `usage`/`peak` a charge writes, so a lock-free reader's line can now be invalidated by an unrelated charge; SoA vs AoS vs hot/cold needs a real SMP benchmark (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag" at line 251)
> **Accepted:** [L] the perf suites read `t->quota`/`t->quota_user` without the owner lock or a block reference; pre-existing house pattern shared with `test_quota.c`, not introduced here (reason: scope) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Bounded worker join + CPU-pinned `kmalloc_fail_next`" at line 252)
> **Accepted:** [M] the counter attributes sections by CPU at release time, not to the invocation: a same-CPU interrupt or nested quota op can add one and a migration can drop one, so it guards structure under quiescent test conditions rather than measuring SMP (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Invocation-scoped lock-section accounting" at line 253)
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
> **Accepted:** [H] the 8-slot receipt embedded per message grows `PORT_MESSAGE_ENTRY` 56 -> 152 bytes for a 3-block chain; a compact chain receipt needs the charge-API rework (reason: infra) -> XREF: `02-kernel-core/TODO-25 §11` (item: "Compact per-chain charge receipt" at line 291)
> **Accepted:** [H] a quota-refused send allocates and zeroes the message before refusing, since a receipt cannot be built before its storage exists; needs a reserve-then-commit charge (reason: infra) -> XREF: `02-kernel-core/TODO-25 §11` (item: "Reserve-then-commit charge so a refused send does not first allocate" at line 292)
> **Accepted:** [H] same-SID processes serialize on one USER block, so ALPC/KNF charge paths contend; sharded or per-CPU credit needs cross-CPU proof (reason: infra) -> XREF: `02-kernel-core/TODO-25 §10` (item: "Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag" at line 271)
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
> **Accepted:** [M] `ob_job_collect_accounting` holds `job->lock` with IRQs off across up to 32 members of delta math (reason: snapshot-then-compute refactor) -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §11` (item: "Cut the `ob_job_collect_accounting` lock hold" at line 310)
> **Accepted:** [L] the tick quantum is the nominal rate, so a one-shot/tickless arm would mis-charge (reason: not-functional-today, no production one-shot caller) -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §15` (item: "Derive the tick quantum from the ACTUAL elapsed monotonic delta" at line 294)
> **Accepted:** [H] `SYS_READFILE` is uninstrumented and hands its ring-3 `buf` to `vfs_read` for kernel-mode writing (reason: scope, the primitive is slated for retirement and must not be instrumented) -> XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §5` (item: "Closing `SYS_READFILE` must close its hazards" at line 152)
> **Deferred:** [M] control-I/O counters are wired and projected but read zero until a device-control op completes -> XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §14` (item: "task_acct_note_control_io" at line 313)
> **Quality reviewed:** 2026-07-20 | Codex 13x (design, adversarial x8, consistency, perf x2, re-adversarial x2, test-coverage) | 5H+11M+3L fixed, 7 accepted-XREF | scope: kernel-code-quality

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
- [ ] Cross-CPU contention proof for the §2 charge path AND the §4 receipt tag (concurrent same-token returners, a paused BUSY window, publication visibility): needs per-CPU run queues plus an operation-level checkpoint. -> XREF: `§2`, `§4`
- [ ] Bounded worker join + CPU-pinned `kmalloc_fail_next` for quota tests: `thread_join` has no timeout and the injection countdown is per-CPU. -> XREF: `§2` (charge API)
- [ ] Invocation-scoped lock-section accounting: the §5 counter attributes by CPU at release time, so a same-CPU interrupt or nested quota op can add a section and a migration can drop one. -> XREF: `§5` (cost budget)
- [ ] Commit: quota: unit tests, boot leak sweep, quota_dump dashboard.

**Test checkpoint:** the quota unit suite passes (charge/return, rollback, concurrent charges, duplicate-handle quota, registry quota, ALPC quota); the boot leak sweep reports zero net quota delta across every test category; `quota_dump()` renders per-type usage/peak/limit on serial and in crash dumps.

---

## 11. Object and Handle Quota Integration

- [ ] Refcounted charge ledger, independent of the reusable task slot, holding a task's outstanding receipts; object bodies and namespace entries can outlive their creator, so a task-embedded registry cannot own their obligations. -> XREF: `§4`
- [ ] Serialize charges against membership transitions with a drain/quiesce protocol: block new chargers, drain in-flight ones, absorb, publish membership, reopen. A generation revalidated AFTER charging double-bills instead. -> XREF: `§4`
- [ ] `quota_charge_adjust` as an ATOMIC multi-block operation (canonical lock order, prevalidate all blocks, then commit counters and receipt together): a negative delta cannot prefix-rollback, since a recharge can be refused. -> XREF: `§2`
- [ ] Migrate outstanding obligations at `ob_job_assign` in ONE transaction that ADOPTS the existing `job_absorb` amount and clears it, rather than charging the job a second time for the same usage. -> XREF: `§4`
- [ ] Charge handle-table entries on insert, return on close, billing the table OWNER; blocked on the TODO-05 §3 reservation/commit primitive returning NTSTATUS. -> XREF: `02-kernel-core/TODO-05 §3`
- [ ] Charge object body and name entry on creation, returning at `ob_free_object` / `ObpRemoveFromDirectory` via the ledger, not a task lookup (the creator may already be gone).
- [ ] `NtDuplicateObject` charges the TARGET owner as part of the insert transaction, not a separate precheck (a standalone check is TOCTOU), returning `STATUS_QUOTA_EXCEEDED`. -> XREF: `02-kernel-core/TODO-05 §3`
- [ ] At task death SEAL the ledger against new charges but RETAIN it; return handle charges during the real handle sweep in `task_cleanup`, then drain residual obligations as a leak assertion.
- [ ] Cut the `ob_job_collect_accounting` lock hold: it scans up to 32 members with per-member delta math while holding `job->lock` with IRQs off. Snapshot under the lock, compute outside. -> XREF: `02-kernel-core/TODO-25 §7`
- [ ] Keep `OBJECT_TYPE` counters authoritative for system-wide per-type totals; quota reports per-principal aggregates only. Unifying needs a global Object-Manager-type-keyed dimension that does not exist.
- [ ] Compact per-chain charge receipt: the 8-slot `quota_charge_receipt_t` embedded in every `PORT_MESSAGE_ENTRY` grows it 56 -> 152 bytes for a 3-block chain. -> XREF: `02-kernel-core/TODO-25 §6`
- [ ] Reserve-then-commit charge so a refused send does not first allocate: `AlpcAllocateMessage` kmallocs up to 64 KiB before the quota refusal. -> XREF: `02-kernel-core/TODO-25 §6`
- [ ] Commit: quota: object/handle charge integration in Object Manager.

**Test checkpoint:** inserting a handle increments the owner's handle usage and closing it returns the charge exactly; a handle inserted into another process's table bills THAT process, not the caller; `NtDuplicateObject` into a target at its cap fails with `STATUS_QUOTA_EXCEEDED` and inserts no handle; a task that dies with open handles reaches zero outstanding obligations after `task_cleanup`, and the leak sweep reports no net delta.

> [!NOTE]
> **Blocked on the handle-table lock (TODO-05 §3).** `HANDLE_TABLE` has no lock and no owning-task back-pointer, and `ObpAllocateHandle` collapses every failure into `INVALID_HANDLE_VALUE`, so a charge added today would admit quota for an entry a concurrent insert can overwrite, bill `task_current()` on the inherit and cross-process duplicate paths, and be unable to report `STATUS_QUOTA_EXCEEDED` distinctly. The charge must join the reservation/commit transaction, not sit beside it.

---

## OS Comparison

| ⭐   | Feature                          | 🪟 Win11                               | 🐧 Linux                                     | 🚀 Impossible OS                             |
| --- | -------------------------------- | ------------------------------------- | ------------------------------------------- | ------------------------------------------- |
| 💎   | Unified resource-type registry   | ⚠️ scattered across subsystems        | ⚠️ split rlimit/cgroup/quotactl             | ✅ one 16-type registry §1                   |
| 💎   | Central quota/charge API         | ✅ `PsChargeProcessQuota` per pool     | ⚠️ split: rlimits + cgroups, no unified API | ✅ one `quota_charge`/`return` §2            |
| 💎   | Atomic quota transfer            | ⬜ none (charge/return only)           | ⬜ none (no cross-principal move)            | ✅ all-or-nothing two-block transfer §2      |
| 💎   | Per-type peak + failure counts   | ⚠️ peak only, no per-type failures    | ⚠️ `memory.events` per-cgroup, not per-type | ✅ peak + saturating failures per type §2    |
| 💎   | Per-token quota block            | ✅ `EPROCESS`/token `QUOTA_BLOCK`      | ⬜ none (uid/cgroup based)                   | ✅ token+process+job blocks §3               |
| 💎   | All-or-nothing chain charge      | ⚠️ per-block, no cross-layer rollback | ⚠️ per-cgroup, no receipt for the return    | ✅ receipt-bound chain charge §3             |
| ⭐   | Per-user aggregate rollup        | ⚠️ per-process/job, no per-SID view   | ⚠️ per-cgroup, not per-uid across cgroups   | ✅ canonical per-SID block + rollup §3       |
| 💎   | Receipt identity (ABA-proof)     | ⬜ none (no receipt abstraction)       | ⬜ none (no receipt abstraction)             | ✅ tagged generation token per charge §4     |
| 💎   | Handle/object quota              | ✅ per-process handle quota            | ⚠️ `RLIMIT_NOFILE` fd-only                  | 🚀 Planned: handle+object body charge §11    |
| 💎   | Paged/nonpaged pool quota        | ✅ pool quota per process              | ⚠️ slab accounting via memcg, not per-proc  | 🚀 Planned: allocator-hook charging §5       |
| 💎   | Enforced charge-path cost budget | ⬜ none (no published charge cost)     | ⬜ none (cost is per-controller, unstated)   | ✅ exact lock-section budget asserted §5     |
| 💎   | Notification-state quota         | ⚠️ WNF has no per-user state cap      | ⬜ none (inotify caps are per-fd, not user)  | ⚠️ state/sub/retention charged, uncapped §6 |
| 💎   | Registry/IPC quota               | ✅ registry + ALPC quotas              | ⚠️ no registry; IPC via `RLIMIT_MSGQUEUE`   | ⚠️ ALPC charged §6; registry blocked T14    |
| 💎   | Admin-configurable per-user caps | ⚠️ registry-set, no live re-limit     | ✅ cgroup limits apply to live cgroups       | ⚠️ live re-limit works; no setter yet §6    |
| ⭐   | CPU/IO/wakeup accounting         | ✅ Job Objects + power throttling      | ✅ cgroup cpu/io/pids controllers            | ✅ per-proc/job CPU+IO+wakeup+timer §7       |
| ⭐   | Job aggregate accounting window  | ⚠️ member lifetime totals folded in   | ✅ cgroup counts only while a member         | ✅ membership-interval deltas, baselined §7  |
| 💎   | Control ("Other") I/O counters   | ✅ `IO_COUNTERS.Other*` populated      | ⚠️ no ioctl split in `/proc/PID/io`         | ⚠️ counters+ABI wired; event source T05 §14 |
| ⭐   | Rate-limit policy record         | ⚠️ per-Job CPU rate cap only          | ⚠️ per-controller, no shared record shape   | ✅ versioned typed record, seqlock §7        |
| 💎   | Native query/set quota syscalls  | ✅ `NtQueryInformationProcess` classes | ✅ `getrlimit`/`prlimit64`                   | 🚀 Planned: Nt{Query,Set}Quota + rlimits §8  |
| ⭐   | Resource pressure events         | ✅ low-memory notifications            | ✅ PSI (`/proc/pressure/*`)                  | 🚀 Planned: stall-time-derived 4-level §9    |
| ⭐   | Last-resort OOM recovery         | ⬜ none (cooperative trim only)        | ✅ cgroup `memory.oom.group`                 | 🚀 Planned: victim-select policy §9          |
| ⭐   | Unified leak sweep + quota_dump  | ⚠️ pool-tag tracking, no boot sweep   | ⚠️ slabinfo, no per-boot delta sweep        | 🚀 Planned: boot delta sweep + dump §10      |

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
- [x] `test_quota_receipt_token_blocks_stale_return`: a spent token cannot return a later charge in recycled receipt storage (§4).
- [x] `test_quota_receipt_generation_exhaustion`: the last generation still charges and the ceiling fails closed instead of wrapping (§4).
- [x] `test_quota_zero_charge_reports_no_obligation_token`: a zero charge reports token 0, which cannot return the next real charge (§4).
- [ ] `test_quota_pressure_hysteresis`: levels derive from stall-time metrics and do not flap at a threshold (§8/§9).
- [ ] `test_quota_failure_event_fields`: a quota failure emits the diagnostic event with all contract fields, rate-limited (§9).
- [ ] `test_quota_handle_insert_close`: handle insert/close increments/decrements the OWNER's handle usage (§11).
- [ ] `test_quota_duplicate_handle_target_cap`: DuplicateHandle into a capped target fails (§11).
- [ ] `test_quota_pool_owner_charged`: tagged pool alloc charges its quota owner; free returns it (§5).
- [ ] `test_quota_registry_data_cap`: registry value over data-byte cap rejected (§6).
- [ ] `test_quota_leak_sweep_zero_delta`: boot leak sweep shows zero net quota delta (§10).
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
- [x] `test_quota_rate_boundary_matrix`: exact-MAX and equal-bound acceptance, both reserved fields, bytes-envelope rules, and per-class isolation (§7).
- [/] Two-CPU concurrent wakers must count ONE wakeup for one BLOCKED->READY claim; the CAS is in place but a real cross-CPU race needs per-CPU run queues. -> XREF: `§10` (test infrastructure).
- [/] A reader racing a rate publisher must see either the whole old or whole new record, never a mix; needs the same SMP harness. -> XREF: `§10` (test infrastructure).
- [/] A named timer whose handle allocation fails is still published and counted; proving it needs an exhausted handle table. -> XREF: `§11` (handle quota integration).

**Test checkpoint:** all `test_quota_*` cases pass under the kernel runner; pending assertions render `[STUB]` via `TEST_PENDING` until their owning section ships; the boot leak sweep prints a zero-delta line for the quota category.

---

## Verification

- [ ] `bash scripts/build.sh` -> `=== BUILD OK ===`.
- [ ] `bash scripts/test.sh SUITE=quota QUIET=1` -> quota suites pass, 0 failures.
- [ ] `grep -R "quota_charge" src/kernel` shows every charge path names a resource type and owner.
- [ ] `quota_dump()` output appears in the boot serial log and in a forced crash dump.
- [ ] Verify on bare metal -- VM behavior differs for pool/working-set counters.

**Test runner:** `scripts\debug\kernel\run-quota-tests.bat` (SUITE=quota) | 9 suites, 0 failures
