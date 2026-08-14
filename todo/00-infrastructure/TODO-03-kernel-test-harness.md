---
schema_version: 1
id: kernel-test-harness
domain: 00-infrastructure
status: active
title: "TODO-03 -- Kernel Test Harness"
---

# TODO-03 -- Kernel Test Harness

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Kernel-internal test-time infrastructure that subsystem unit tests (under `src/kernel/test/test_*.c`) can opt into when they need to exercise rare control flow: allocator failure rollback, deterministic race windows between cooperating threads, and bulk scratch buffers that exceed the 8 KiB kernel stack. Each existing subsystem tests the hot path; the hooks in this TODO close the remaining corners without having to stand up a whole kernel debugger.

> [!IMPORTANT]
> **Current state:** `src/kernel/test/` has 436+ unit-test suites wired into the boot-time test runner (`test=1` in `boot.conf`). Every suite must currently test the normal path: there is no way for a test to ask `kmalloc` to fail on its 3rd call, no way to make two cooperating kthreads land on opposite sides of a lock acquisition at a chosen instant, and the 8 KiB kernel task stack caps any test-local buffer at about 4 KiB before overflowing (guard page trip). Concrete gaps that motivated this TODO are collected from TODO-12 §5 "Test gaps (NO current owner)".

---

## Inputs

- `src/kernel/mm/heap.c` -- `kmalloc`/`kfree` -- §1 fault-injection hooks install here
- `src/kernel/mm/pmm.c` + `include/kernel/mm/pmm.h` -- `pmm_alloc_frame` / `pmm_alloc_contiguous` -- §3 large-buffer dispatch and §6 PMM fault injection hook here
- `src/kernel/mm/vmm.c` + `include/kernel/mm/vmm.h` -- `vmm_map`, `vmm_alloc_range` -- §6 VMM-map fault injection
- `src/kernel/cpu_security.c` + `include/kernel/cpu_security.h` -- `copy_to_user` / `copy_from_user` SMAP-mediated wrappers -- §6 copy_user fault injection
- `include/kernel/test/test.h` -- test macros (`TEST_ASSERT`, `TEST_SKIP`, `TEST_PENDING`) and public test API where §1-§3, §5-§8 helpers go
- `src/kernel/test/test_runner.c` -- runner that drains the §7 action registry and records §8 heap-leak deltas between suites; auto-clears per-CPU countdown from §1/§6
- `src/kernel/sched/task.c` -- `kthread_create`, `thread_yield`, `thread_join` -- §2 race-fence and §6 task-filter use these
- `include/kernel/sched/spinlock.h` -- spinlock primitives -- §2 uses a `spinlock_t` field inside `test_race_barrier_t`
- `src/kernel/klog.c` + `include/kernel/klog.h` -- per-subsystem level override table -- §5 `klog_get_level` and `klog_set_level` hooks here
- `include/kernel/smp.h` `per_cpu_data` -- §1 already adds `kmalloc_fail_countdown` (KERNEL_TESTS-gated); §6 adds task-filter, total-hits cap, and per-allocator countdown fields in the same gated tail
- `CLAUDE.md` "Test Code -- No Live Boot Infrastructure Calls" -- the hooks below must obey the same safety contract (no live boot-path mutation from tests)
- `CLAUDE.md` "Freestanding Kernel -- kmalloc <= 4 KB" -- §3 dispatches to PMM for larger buffers to honour this rule
- -> XREF: `T04 §3` -- user-mode test launcher is complementary; it runs user-mode binaries while this TODO stays in-kernel
- -> XREF: `T04 §5` -- user-mode `SYS_FAULT_INJECT` bridge consumes §1 `kmalloc_fail_next()` and §6 PMM/VMM/copy_user countdowns under a `boot.conf test=1` gate so user-mode tests can probe kernel error paths without a debugfs-style escape hatch
- -> XREF: `T01 §3, §7` -- canonical local test wrappers and host-side tooling regression policy keep repo-wide deferred-test sweeps on one supported entry path

---

## Outcome

- `kmalloc_fail_countdown(N)` / `kmalloc_fail_next()` -- test-only API that causes the next (or Nth) `kmalloc` call to return `NULL` without touching the real heap. Used by tests that need to prove quota-rollback and failure-cleanup paths.
- `test_race_barrier_t` -- test-only two-thread rendezvous primitive that pins both threads at named checkpoints so a test can observe behaviour at a controlled interleaving. Used by tests that need to prove lock-order or race-window invariants.
- `TEST_SCRATCH_KBUF(name, size)` -- macro that allocates a scratch buffer (kmalloc for ≤ 4 KiB, pmm_alloc_contiguous for larger) with automatic free on test exit. Lets a test use >8 KiB buffers without overflowing the kernel stack or leaking on early return.
- `test_add_action(fn, ctx)` -- general LIFO test-scoped cleanup registry; `TEST_SCRATCH_KBUF`, `TEST_KLOG_SUPPRESS`, and any future test-scoped override become thin callers of this primitive (KUnit's `kunit_add_action` model).
- Multi-allocator fault injection: task-filter and total-hits-cap on the §1 kmalloc countdown plus symmetric countdowns for `pmm_alloc_*`, `vmm_map_*`, and `copy_to/from_user`. Matches Linux `lib/fault-inject.c` coverage.
- Per-test heap-leak detection: every `[ OK ]` test automatically becomes a leak-coverage test via a `heap_stats().used_bytes` delta check that runs after the §7 action drain.
- `TEST_KLOG_SUPPRESS(subsystem)` -- block-scoped klog level demotion via §7 action; tests exercising error paths no longer pollute serial with `[FAIL]` lines.
- Every existing `Test gaps (NO current owner)` entry in `todo/` resolves via one of these primitives. When §1-§3, §5-§8 ship, those Accepted stamps get pruned via the inbound-XREF sweep (§4).

---

## Implementation Order

| ⭐  | Order | Section | Deliverable                                              | Depends On   | Status |
| --- | :---: | :-----: | -------------------------------------------------------- | ------------ | :----: |
| 💎  |   1   |   §1    | kmalloc fault injection (`kmalloc_fail_countdown`)       | --           |  [x]   |
| 💎  |   2   |   §7    | Test action/cleanup registry (`test_add_action`)         | --           |  [x]   |
| 💎  |   3   |   §3    | Scratch-buffer helper (`TEST_SCRATCH_KBUF`)              | §7           |  [x]   |
| 💎  |   4   |   §2    | Deterministic race barrier (`test_race_barrier_t`)       | --           |  [x]   |
| 💎  |   5   |   §6    | Fault-injection hardening (task-filter, multi-allocator) | §1           |  [x]   |
| 💎  |   6   |   §5    | Test-scoped klog level demotion (`TEST_KLOG_SUPPRESS`)   | §7           |  [x]   |
| 💎  |   7   |   §8    | Per-test heap-leak detection                             | §7           |  [x]   |
| 💎  |   8   |   §4    | Retrofit existing "Test gaps" stamps across `todo/`      | §1-§3, §5-§8 |  [x]   |
| 💎  |   9   |   §9    | Audit + classify the 50+ [LEAK] failures §8 surfaced     | §8           |  [/]   |
| 💎  |  10   |   §10   | Bound the failing-assertion message (long one wedges)    | --           |  [x]   |
| 💎  |  11   |   §11   | Poisoned-boundary fixture so an overread fails a test    | §10          |  [x]   |

> 💎 = parity work -- Linux kernel self-test framework has `lib/fault-inject.c` (multi-allocator fault injection), `kunit_add_action` (test-scoped cleanup registry), `kcsan`/`kasan` fences, `kunit_kzalloc()` scratch helpers, and `kmemleak` leak detection. This TODO brings the same floor to the Impossible OS kernel test runner without requiring the Driver Verifier / WDK workflow Windows leans on.
> **Order vs section-number:** Implementation Order is execution sequence; section numbers (§N) preserve file stability. §7 (action registry) ships at Order 2 because §3, §5, and §8 all consume it.

---

## 1. kmalloc Fault Injection

A per-CPU test-only countdown that causes `kmalloc` to return `NULL` on the next (or Nth subsequent) call WITHOUT hitting the real allocator. Tests set the countdown, invoke the code under test, assert the failure-cleanup path executed, then reset the countdown.

- [x] Added `uint32_t kmalloc_fail_countdown` to `struct per_cpu_data` in `include/kernel/smp.h` (gated by `#ifdef KERNEL_TESTS` so release builds compile the field out). Decrements on every `kmalloc` call; when the decrement crosses from 1 to 0, that allocation returns NULL.
- [x] `include/kernel/mm/heap.h` exposes `kmalloc_fail_countdown_set(n)`, `kmalloc_fail_countdown_clear()`, `kmalloc_fail_next()`, and `kmalloc_fail_injections_triggered()` -- all under `#ifdef KERNEL_TESTS`.
- [x] `src/kernel/mm/heap.c` `kmalloc()` checks `smp_this_cpu()->kmalloc_fail_countdown` FIRST (before block-list walk) so the heap state is unaltered on a forced failure. On trigger: increment `s_kmalloc_fault_injections` (atomic) and return NULL. No klog -- tests expect silent failure. **Thread-context gate:** the hook is silently bypassed when `KeGetCurrentIrql() != PASSIVE_LEVEL`, so an IRQ / DPC / spinlock-holding caller on the same CPU (e.g. the RTL8139 RX ISR) cannot steal a pending injection from the thread-context test that armed it. Caught by Codex adversarial review; regression test `Heap: fault-inject IRQL gate` locks it in.
- [x] `kmalloc_fail_next()` == `kmalloc_fail_countdown_set(1)` shorthand. Test-runner hygiene: `src/kernel/test/test_runner.c` now calls `kmalloc_fail_countdown_clear()` after every suite, so a suite that armed the trap and then failed an assertion before it fired cannot poison the next suite.
- [x] `src/kernel/test/test_heap.c` covers: (a) `kmalloc_fail_next` fires on the next call and auto-clears; (b) `kmalloc_fail_countdown_set(3)` fires on the 3rd call; (c) `kmalloc_fail_countdown_clear` disarms a pending trap; (d) forced NULL does not mutate `heap_get_used()`; (e) armed countdown is NOT consumed by a kmalloc made at `DISPATCH_LEVEL` (IRQL gate regression). All five tests verify `kmalloc_fail_injections_triggered()` increments exactly once per forced NULL.
- [x] Commit: `"test/heap: kmalloc fault-injection countdown for failure-path coverage"`

**Test checkpoint:** `kmalloc_fail_next()` + `kmalloc(16) == NULL` + subsequent `kmalloc(16) != NULL`. Verifies the per-CPU counter decrements correctly and auto-clears.

> **Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) | 5 new heap fault-injection suites, 0 failures

> **Notes:**
> - Shipped `kmalloc_fail_countdown(n)` / `_clear()` / `_next()` / `_injections_triggered()` in [`include/kernel/mm/heap.h`](../../include/kernel/mm/heap.h) + [`src/kernel/mm/heap.c`](../../src/kernel/mm/heap.c), all `#ifdef KERNEL_TESTS`-gated so release builds compile out the hook entirely.
> - Per-CPU storage lives in [`include/kernel/smp.h`](../../include/kernel/smp.h) `struct per_cpu_data` as a KERNEL_TESTS-only tail (offset 144+); existing asm-referenced offsets (0, 24, 32, 104, 112, 120, 128, 136) untouched, so boot-path ABI is preserved.
> - Hook fires BEFORE the block-list walk in `kmalloc`, so a forced NULL leaves heap state unaltered. Thread-context gate (`KeGetCurrentIrql() == PASSIVE_LEVEL`) stops an ISR/DPC/spinlock-holding caller from stealing a pending injection armed by a different thread-context test on the same CPU.
> - Test-runner auto-clear in [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) after every suite prevents cross-suite poisoning when a test armed a countdown and then hit `TEST_ASSERT` before the trap fired.
> - Downstream consumers: §6 extends this countdown model to PMM / VMM / copy_user with symmetric APIs; §3 `TEST_SCRATCH_KBUF` uses `kmalloc_fail_next()` in its failure-path regression; TODO-12 §5 `Test gaps` entries for allocator failure rollback close via §4 retrofit using this primitive.
> - Scope boundary: §1 only wires the kmalloc path. PMM / VMM / copy_user countdowns and task-filter + total-hits cap ship in §6.

> **Verified:** 2026-04-19 | commit `ea9c40e7` | 6/6 items | build OK | tests 5/5 PASS (TEST_CAT_MM)
> **Quality reviewed:** 2026-04-19 | Codex 2x (adversarial, quality) | 0 findings, 0 open | scope: kernel-code-quality

---

## 2. Deterministic Race Barrier

A two-thread rendezvous primitive for tests that need to observe behaviour at a specific interleaving (e.g. "sender reaches just before event_set; receiver reaches just after event_wait returns"). Without this, concurrency tests can only observe eventual behaviour, not specific orderings.

- [x] `typedef struct test_race_barrier { spinlock_t lock; event_t a_reached; event_t b_reached; uint8_t a_arrived; uint8_t b_arrived; } test_race_barrier_t;` -- defined in [`include/kernel/test/race_barrier.h`](../../include/kernel/test/race_barrier.h); events used as release signals from the driver, `a_arrived` / `b_arrived` carry the worker-to-driver arrival signal polled by release().
- [x] `void test_race_barrier_init(test_race_barrier_t *)` -- initialises both events as `EVENT_AUTO_RESET`, zeroes both arrived flags, resets the spinlock flag ([`race_barrier.c`](../../src/kernel/test/race_barrier.c)).
- [x] `void test_race_barrier_arrive_a(test_race_barrier_t *)` -- sets `a_arrived` under `spin_lock_irqsave`, releases the lock, then `event_wait(&a_reached)`. Lock is released BEFORE the wait to avoid deadlocking other takers during the event_wait yield.
- [x] `void test_race_barrier_arrive_b(test_race_barrier_t *)` -- symmetric: sets `b_arrived`, then `event_wait(&b_reached)`.
- [x] `void test_race_barrier_release(test_race_barrier_t *, int a_first)` -- cooperative-yield polls both arrived flags under the spinlock until both are set, then wakes in chosen order with `thread_yield()` between the two `event_set` calls so the first woken worker advances past its checkpoint before the second is signalled. Header documents the yield-based "empirically deterministic on 2-CPU round-robin; not a hard SMP guarantee" caveat.
- [x] Unit test in [`src/kernel/test/test_sched.c`](../../src/kernel/test/test_sched.c): three new `TEST_CAT_SCHED` suites -- `race-barrier release(a_first=1) wins 100x`, `race-barrier release(a_first=0) wins 100x`, `race-barrier init clears state`. The 100-iteration count is the empirical determinism check the header calls out. Helper `race_barrier_run_once` handles partial `kthread_create` failure by signalling the parked worker's release event before `thread_join` so a failure path never hangs the suite (Codex step-9 finding).
- [x] Header documents the hazard: do NOT hold another spinlock while calling `arrive_a` / `arrive_b` -- the lock is released before `event_wait` but the hazard stands for composition with caller-held locks.
- [x] Commit: `"test/sched: deterministic race barrier for two-thread interleaving tests"`

**Test checkpoint:** Barrier release-a-first causes thread A's post-checkpoint branch to win consistently; release-b-first causes B's branch to win. Exercised over 100 iterations in the unit test to catch scheduler drift.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 3 new race-barrier suites added, 0 failures

> **Notes:**
> - Shipped [`include/kernel/test/race_barrier.h`](../../include/kernel/test/race_barrier.h) + [`src/kernel/test/race_barrier.c`](../../src/kernel/test/race_barrier.c) (4 functions, ~60 lines, KERNEL_TESTS-gated so release builds drop the translation unit entirely).
> - Uses existing primitives only (`spinlock_t`, `event_t` AUTO_RESET, `thread_yield`); no new kernel subsystem surface. Arrive functions release the spinlock BEFORE `event_wait` to honour the "don't yield under a lock" hazard.
> - Release-ordering guarantee is yield-based best-effort under the flat-cyclic round-robin scheduler: on WHPX-2-CPU the 100-iteration test expects 100% match for the chosen order. Regression would surface as sporadic order-inversions (would fail the `hits == iterations` assertion), not a hang.
> - Defensive bounding: arrive_a/arrive_b use `event_wait_timeout(10 s)` (not `event_wait`) to avoid the event.c check-enqueue-block lost-wakeup race; `release()` has a 1,000,000-yield budget and unconditionally event_sets both events + sets a `release_timed_out` flag on timeout; test helper fast-fails via that flag. Runner cannot hang if a worker never arrives.
> - Partial-create cleanup: if the second `kthread_create` fails, the first worker is already parked on its release event; the test helper signals the event before `thread_join` so slot-pressure failure paths surface as a `TEST_ASSERT(r >= 0)` fail rather than a deadlock.
> - Downstream consumers: §7 `test_add_action` will convert the barrier cleanup to a registered action once that section ships; TODO-12 §5 `Test gaps` two-port lock-ordering concurrency test closes via §4 retrofit using this primitive.
> - Scope boundary: §2 owns the two-thread deterministic rendezvous. §7 owns the generic test-scoped cleanup registry; §6 owns SMP ordering hardening (task-filter keeps worker pairs scheduling-predictable); wider concurrency harnesses remain with specific domain tests.

> **Verified:** 2026-04-19 | commit `3c1a03cc` | 7/7 items | build OK | 3 sched suites added (release-a-first/100, release-b-first/100, init-clears-state)
> **Quality reviewed:** 2026-04-19 | Codex 4x (coverage, adversarial x2, quality) | 3H+1M fixed, 1H rejected, 0 open | scope: kernel-code-quality

---

## 3. Scratch-Buffer Helper

`TEST_SCRATCH_KBUF(name, size)` declares a `size`-byte scratch buffer and registers a cleanup handler that frees it on test exit (including early `return` via `TEST_ASSERT` macros). Solves the "I need 64 KiB but stack is 8 KiB" case without leaking on assertion failure. Respects the `kmalloc <= 4 KB` project rule: for `size > 4096`, dispatches to `pmm_alloc_contiguous()` instead so a 64 KiB test buffer does not consume 3% of the 2 MiB kernel heap per test.

- [x] `TEST_SCRATCH_KBUF(name, size)` in [`include/kernel/test/scratch.h`](../../include/kernel/test/scratch.h) expands to `void *name = test_scratch_alloc((size)); TEST_ASSERT_NOT_NULL(name, "scratch alloc " #name); if (!(name)) return; if (test_add_action(test_scratch_free, (name)) < 0) { test_scratch_free((name)); TEST_ASSERT(0, ...); return; }` -- KUnit-style REQUIRE semantics so NULL alloc or failed registration terminates the test safely instead of faulting on a NULL buffer or leaking.
- [x] `void *test_scratch_alloc(size_t bytes)` in [`src/kernel/test/scratch.c`](../../src/kernel/test/scratch.c) -- `bytes <= PMM_FRAME_SIZE` uses `kmalloc(bytes)`; larger rounds up to pages via `pmm_alloc_contiguous((bytes + 4095) / 4096)`. Sidecar `s_recs[32]` records `(ptr, pages)` for routed free. Record-table full returns NULL + `[WARN]`.
- [x] `void test_scratch_free(void *ctx)` -- `ctx == NULL` is a safe no-op; linear scan of `s_recs` routes to `kfree` when `pages == 0` or loops `pmm_free_frame(ctx + j*4096)` when `pages > 0`. Record removed via swap-with-last (O(1)). Unknown ptr logs a `[WARN]` (possible double-free or foreign pointer).
- [x] Migration: §7 shipped with `test_add_action`; `TEST_SCRATCH_KBUF` uses it directly in the macro expansion -- no bespoke per-test scratch list ever existed.
- [x] Unit test -- 5 `TEST_CAT_BOOT` suites in [`src/kernel/test/test_harness.c`](../../src/kernel/test/test_harness.c) (pair pattern): (a) kmalloc route (`TEST_SCRATCH_KBUF(small, 512)` -- mid-suite heap grew, PMM did not; post-drain both clean); (b) PMM route (`TEST_SCRATCH_KBUF(big, 65536)` -- mid-suite PMM grew by exactly 16 pages, heap unchanged; post-drain both clean); (c) rollback regression -- fills the action stack to 32, proves `test_add_action` returns -1 and `test_scratch_free` cleans up heap state independently of the macro. Dedicated forced-fail suite was dropped during review: `TEST_ASSERT(0)` in a shipping test breaks `scripts/test.sh`'s zero-failure summary gate, and the non-longjmp `TEST_ASSERT` means drain-on-fail is path-identical to drain-on-pass in this runner -- the 4 pass-side suites cover both semantics by construction.
- [x] Commit: `"test: scratch-buffer helper (kmalloc <= 4 KiB, pmm_alloc_contiguous larger) registered via test_add_action"`

**Test checkpoint:** A test that allocates 64 KiB via `TEST_SCRATCH_KBUF` leaves zero leaked bytes in the heap AND zero leaked pages in the PMM bitmap after the test runner moves on. Verified by the `Harness: scratch PMM route returns to pre-snapshot` and `Harness: scratch kmalloc route returns to pre-snapshot` pair-suites. Because `TEST_ASSERT` is non-longjmp in this runner, drain-on-fail is path-identical to drain-on-pass -- explicit forced-fail coverage was dropped during review to keep `scripts/test.sh`'s zero-failure gate green.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 5 new scratch-buffer suites, 0 failures

> **Notes:**
> - Shipped [`include/kernel/test/scratch.h`](../../include/kernel/test/scratch.h) + [`src/kernel/test/scratch.c`](../../src/kernel/test/scratch.c) (~95 LOC incl docs, KERNEL_TESTS-gated). `TEST_SCRATCH_KBUF` macro + `test_scratch_alloc` / `test_scratch_free` primitives. Routes by size: `bytes <= 4096` → kmalloc; larger → `pmm_alloc_contiguous` (saves ~3% of the 2 MiB kernel heap per 64 KiB buffer).
> - Sidecar `s_recs[32]` maps returned pointer → `(pages, mode)`. File-scope static, no spinlock (matches §7's sequential test-runner invariant). Swap-with-last record removal keeps free O(1) inside a 32-entry bound.
> - KUnit-style REQUIRE semantics: the macro calls `return` on alloc NULL or `test_add_action` -1. Prevents callers from dereferencing a NULL buffer after a non-longjmp `TEST_ASSERT_NOT_NULL`, and prevents orphaned allocations when the action stack is full. Documented prominently in the header.
> - Closes §7's Deferred item "Migrate §3 TEST_SCRATCH_KBUF onto test_add_action": the macro uses `test_add_action` from day one, no bespoke scratch list ever existed.
> - Downstream consumers: TODO-12 §5 `Test gaps` ReplyBodyCap clamping test (needs > 65528-byte scratch buffer) now unblocked via §4 retrofit using `TEST_SCRATCH_KBUF`. Any test that needs > 4 KiB scratch without stack overflow can adopt this primitive.
> - Canonical doc: the header comment in [`include/kernel/test/scratch.h`](../../include/kernel/test/scratch.h) documents size-routing, REQUIRE semantics, and the in-action-callback usage restriction.
> - Scope boundary: §3 owns scratch buffer dispatch. §7 owns the cleanup registry it uses. §8 (heap-leak detection) will cross-check this primitive's drain cleanliness as an independent observer when that section ships.

> **Verified:** 2026-04-19 | commit `5da3457d` | 6/6 items | build OK | 5 harness scratch suites (kmalloc route + PMM route + rollback; forced-fail pair removed in review)
> **Quality reviewed:** 2026-04-19 | Codex 3x (adversarial x2, quality) | 1C+3H+1M fixed, 0 open | scope: kernel-code-quality

---

## 4. Retrofit Existing "Test Gaps" Stamps

After §1-§3, §5-§8 ship, sweep the repo for `Test gaps (NO current owner)` blocks and TEST_PENDING placeholders that were deferred only because this infrastructure did not exist. Rewrite each test to use the new primitives and prune the deferral note.

- [x] `02-kernel-core/TODO-24` §4 "Test gaps (deferred)" block: the 3 deferred items closed via 3 new TEST_CAT_IPC suites in [`src/kernel/test/test_alpc.c`](../../src/kernel/test/test_alpc.c). (a) `alpc: kmalloc-fail in pending alloc rolls back PoolUsageBytes` uses `kmalloc_fail_next()` + `TEST_KLOG_SUPPRESS("alpc")` + a direct `PoolUsageBytes` invariant check (stronger oracle than §8 heap-delta because PoolUsageBytes specifically tracks the ALPC pool charges the rollback path is supposed to undo). (b) `alpc: ReplyBodyCap clamps recv_buf_len > 65528` uses `TEST_SCRATCH_KBUF(rxbuf, 65536 + 64)` (PMM-backed per §3) to drive `recv_buf_len = 65600` through the clamp branch in `alpc_sync_request`, then verifies the round-trip succeeds and PoolUsageBytes returns to baseline. (c) `alpc: two-port lock-order stress` uses `test_race_barrier_t` to pin TWO senders going in OPPOSITE directions (sender A: client -> server; sender B: server -> client) at the lock-acquisition checkpoint, plus paired receivers on each side; the two paths present the same two ALPC_PORT objects to `alpc_lock_two` in opposite logical order, which is the actual lock-order-inversion regression `alpc_lock_two` is supposed to prevent. (Original v1 of the test had both senders going the same direction and would have passed even with naive locking -- caught by Codex adversarial review and rewritten before commit.) The original deferred block + Accepted stamp pointing at us removed from `02-kernel-core/TODO-24` §4 in the same commit.
- [x] `pmm_alloc_fail_next()` (§6) sweep: `grep -rn ">4 ?KiB.*rollback\|allocation rollback" todo/` returns no other consumer with a deferred test gap pending the PMM-failure primitive. TODO-14 §6 (large registry value names / data via `pmm_alloc_contiguous`) is a future consumer for `pmm_alloc_fail_next()` once those allocation paths land, but no test gap is filed yet because the consumer code itself is unwritten. Re-sweep when TODO-14 §6 ships.
- [x] Repo sweep: `grep -rn "Test gaps \(NO current owner\)\|Test gaps \(deferred to" todo/` -- only matches were TODO-24 §4 (the ALPC block, closed above) and self-references in this section. The "Test gaps (NO current owner)" stamp pattern from older TODO drafts is not in active use anywhere else.
- [x] Commit: `"test: retrofit deferred ALPC §4 test gaps against new kernel test harness"`

**Test checkpoint:** `grep -rn "Test gaps (deferred to" todo/02-kernel-core/TODO-24-alpc-message-ports.md` returns zero hits (the block was removed); `make test-ipc` boot run shows the 3 new suite names in the §4 group with `[OK]` lines and zero failures.

> **Test runner:** `scripts\debug\kernel\run-ipc-tests.bat` (SUITE=ipc) | 3 new alpc retrofit suites in TEST_CAT_IPC, 0 failures

> **Notes:**
> - Shipped 3 ALPC §4 retrofit test suites in [`src/kernel/test/test_alpc.c`](../../src/kernel/test/test_alpc.c): `alpc: kmalloc-fail in pending alloc rolls back PoolUsageBytes` (~85 LOC), `alpc: ReplyBodyCap clamps recv_buf_len > 65528` (~110 LOC), `alpc: two-port lock-order stress` (~120 LOC). All three close test gaps that were filed in TODO-24 §4 as "deferred to TODO-03 §6" because the kernel test harness lacked deterministic kmalloc-fault injection (§1), two-thread race fences (§2), and PMM-backed scratch buffers (§3) at the time §4 shipped.
> - Why `PoolUsageBytes` invariant instead of §8 heap-delta for retrofit (a): the ALPC pool charge is a per-port counter that the rollback path explicitly maintains. A `PoolUsageBytes != pool_before` after a forced kmalloc failure proves the rollback path is broken even when the kmalloc itself returned NULL (so heap_get_used would show no delta). The §8 leak detector and the §1 fault-injection IRQL gate together make this the right oracle.
> - Why `TEST_SCRATCH_KBUF` instead of stack-allocated for retrofit (b): the buffer is 65 KiB + 64 B, far above the 8 KiB kernel stack. Pre-§3 the test would have either overflowed the stack or required a bespoke `pmm_alloc_contiguous` + `pmm_free_frame` cleanup pair on every error path. The macro routes to `pmm_alloc_contiguous(17 pages)` and registers cleanup via §7's `test_add_action`; failure-path cleanup is automatic.
> - Why `test_race_barrier_t` instead of `thread_yield()` loops for retrofit (c): the lock-order regression only manifests when two threads enter `alpc_sync_request` within microseconds of each other. Without a barrier, scheduler skew makes the test pass even when `alpc_lock_two` is broken. The barrier's release-a-first ordering is the empirically deterministic interleaving on the flat-cyclic round-robin scheduler (§2 race-barrier docstring).
> - TODO-24 §4 cleanup: removed the "Test gaps (deferred to TODO-03 §6)" 3-line block AND the matching `Accepted: three §4 test gaps -> XREF` stamp line; the `Verified:` line was extended to mention the 3 retrofit suites and the `Expected:` count went from 14 -> 17. Inbound-XREF sweep complete.
> - Stale-reference cleanup: the original §4 spec text said "TODO-12 §5 'Test gaps'" but TODO-12 §5 (Nt/Zw Naming Migration) has no test gaps stamp. The actual deferred ALPC test gaps lived in TODO-24 §4 (verified by `grep -rn "Test gaps" todo/`). Updated the items above to point at TODO-24 §4 directly.
> - Scope boundary: §4 closes the ONE inbound deferred-test-gaps block that pointed at TODO-03 (TODO-24 §4). It does NOT pre-emptively retrofit other subsystems' tests -- the rule is "convert when an explicit deferred-test-gaps stamp names §1-§3/§5-§8 as the unblocker". TODO-14 §6 (large registry values) is the next likely consumer but has no test surface yet.

> **Verified:** 2026-04-19 | commit `c8585687` | 4/4 items | build OK | 3 new IPC retrofit suites (kmalloc-fail rollback / ReplyBodyCap clamp / two-port lock-order stress)
> **Quality reviewed:** 2026-04-19 | Codex 2x (adversarial, quality) | 1H+1L fixed pre-commit, 1H+1L fixed in review (test (c) loop iter + dead s_sync_worker_done writes) | scope: kernel-code-quality

---

## 5. Test-Scoped klog Level Demotion (`TEST_KLOG_SUPPRESS`)

Tests that exercise error paths (validators, allocator failure rollback, bad-input rejection) currently emit the validator's `klog(LOG_ERROR, ...)` -- rendered as `[FAIL] subsys: ...` -- directly onto serial output during test runs. Diagnose-serial-log runs then see test-phase `[FAIL]` lines and have to hand-classify them as NOISE per-test, which (as the 2026-04-18 incident showed) leaks occasionally into "real" findings reports. The validator is dual-use: loud on real boot with bad input, quiet under test.

**Files:** `src/kernel/test/test.h` (new macro), `src/kernel/test/test_runner.c` (save/restore).

- [x] `TEST_KLOG_SUPPRESS(subsystem)` block-scoped macro in [`include/kernel/test/klog_suppress.h`](../../include/kernel/test/klog_suppress.h). On entry, snapshots BOTH the effective level AND whether an explicit override existed, then calls `klog_set_level(subsys, LOG_FATAL)` to silence the subsystem. On exit the §7 action drain restores via `klog_set_level(subsystem, prev_level)` when a pre-existing override existed, or `klog_remove_override(subsystem)` when the suppress created the only override (so the tag goes back to following the global default exactly).
- [x] Added `klog_get_level`, `klog_has_override`, and `klog_remove_override` to [`include/kernel/klog.h`](../../include/kernel/klog.h) + [`src/kernel/klog.c`](../../src/kernel/klog.c). The had-override flag + remove primitive was added in review to fix the restore-semantics gap that Codex caught -- without them, every suppress on a tag with no pre-existing override would leak a permanent override entry and break later global-default propagation.
- [x] Retrofitted [`src/kernel/test/test_boot_info.c`](../../src/kernel/test/test_boot_info.c) -- 16 payload-negative tests each start with `TEST_KLOG_SUPPRESS("boot")`. `[FAIL] boot: boot_payload:` lines no longer appear on serial during test runs; real-boot validator still emits loudly against a bad payload (level is restored at suite exit).
- [x] Retrofitted [`test_uthread_rejects_kernel_task`](../../src/kernel/test/test_peb_teb.c) -- wraps the `uthread_create(PID 0)` assertion in `TEST_KLOG_SUPPRESS("sched")`. `[FAIL] sched: uthread_create:` line gone from test output.
- [x] Unit test: 4 new `TEST_CAT_BOOT` suites in [`test_harness.c`](../../src/kernel/test/test_harness.c) pair pattern: (a) begin-suite asserts `klog_get_level` returns `LOG_FATAL` after suppress; (b) verify-suite asserts level restored AND no lingering override; (c) begin-suite asserts suppress creates a temp override when none pre-existed; (d) verify-suite asserts override removed on drain. The (c)+(d) pair is the regression Codex recommended for distinguishing effective-level restore from override-state restore.
- [x] Updated [`.claude/skills/diagnose-serial-log/SKILL.md`](../../.claude/skills/diagnose-serial-log/SKILL.md) "Known Test Noise" table -- retired the `sched: uthread_create:` + `boot: boot_payload:` entries (silenced at source now).
- [x] Updated [`.claude/skills/diagnose-serial-log/POLICIES.md`](../../.claude/skills/diagnose-serial-log/POLICIES.md) P4.3 -- names `TEST_KLOG_SUPPRESS` as the canonical remediation. Test-phase `[FAIL]` lines are now real findings (no more "NOISE because test-owned").
- [x] Commit: `"test: TEST_KLOG_SUPPRESS block-scoped klog level demotion"`

**Test checkpoint:** A clean `bash scripts/test.sh` run produces zero `[FAIL] boot: boot_payload:` and zero `[FAIL] sched: uthread_create:` lines in the test-phase window. Diagnose-serial-log P4.3 FAIL/CRIT detector returns zero hits on those keys. Harness regression suites in `test_harness.c` confirm override-state restoration (no overrides leaked after drain when none existed before).

**Filed by:** diagnose-serial-log 2026-04-18 on debug-tmp/all-tests-serial.log (17 FAIL lines across 5 canonicalized keys: 16 boot_payload, 1 sched uthread_create).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 4 new harness suites + 17 retrofitted source suites, 0 failures; 17 `[FAIL]` lines silenced from test-phase serial output

> **Notes:**
> - Shipped `TEST_KLOG_SUPPRESS(subsystem)` in [`klog_suppress.h`](../../include/kernel/test/klog_suppress.h) + [`klog_suppress.c`](../../src/kernel/test/klog_suppress.c) (KERNEL_TESTS-gated). Demotes subsystem to `LOG_FATAL` for the remainder of the suite; §7 action drain restores on suite exit.
> - Restore semantics preserve override-state: if the tag had NO pre-existing override, suppress creates a temporary one that is REMOVED (not just reset) on drain so the tag resumes following the global default. If the tag HAD an override, its prior value is restored. Critical distinction -- the effective-level-only version that Codex caught pre-commit would have leaked permanent overrides for every test.
> - New klog APIs: `klog_get_level`, `klog_has_override`, `klog_remove_override`. The latter two were added in review to fix the restore-semantics finding and are useful on their own for any future save/restore caller.
> - Retrofitted 17 error-path tests: 16 `boot_payload` negative tests in `test_boot_info.c` + 1 `uthread_create(PID 0)` test in `test_peb_teb.c`. Each triggered 1 `[FAIL]` line; all now silenced at source.
> - Diagnose-serial-log docs updated: `SKILL.md` noise table retires the silenced keys; `POLICIES.md` P4.3 names `TEST_KLOG_SUPPRESS` as the canonical remediation for test-phase WARN/ERROR/FAIL/CRIT.
> - Closes §7's Deferred item "Migrate §5 TEST_KLOG_SUPPRESS onto test_add_action": §5 used `test_add_action` from day one for cleanup registration.
> - Scope boundary: §5 owns the block-scoped klog demotion primitive + the two retrofit sites the filed-by incident named. Other test-phase noise sites (exec/pe/ob in the SKILL.md noise table) are NOT retrofitted here -- they're separate incidents each best handled as its own one-line wrap when the owning test is next edited.

> **Verified:** 2026-04-19 | commit `b3085770` | 8/8 items | build OK | 4 harness suites + 17 retrofitted error-path tests (16 boot_payload + 1 uthread); 17 `[FAIL]` lines silenced from test-phase serial
> **Quality reviewed:** 2026-04-19 | Codex 4x (adversarial x2, quality x2) | 2M fixed, 0 open | scope: kernel-code-quality

---

## 6. Fault-Injection Hardening (task-filter, total-hits cap, multi-allocator)

§1 ships a per-CPU kmalloc countdown that fires regardless of which thread calls `kmalloc`. Linux `lib/fault-inject.c` additionally offers task-scoped filtering, a total-hits cap, and symmetric injection across other allocator boundaries. This section closes those gaps: adds task-filter + total-hits cap to §1, then extends the same countdown model to PMM, VMM mapping, and copy_to/from_user. Used by tests that prove rollback paths on > 4 KiB allocations, partial-map cleanup, and user-copy failure propagation.

- [x] Added `uint32_t kmalloc_fail_task_pid` + `kmalloc_fail_fired_counter` to [`per_cpu_data`](../../include/kernel/smp.h) (KERNEL_TESTS-gated). When non-zero, the countdown only decrements for the matching task on the SAME CPU (`task_current()->pid == task_pid`); foreign tasks on the same CPU skip without consuming the countdown. Setters `kmalloc_fail_task_filter_set(pid)` / `_clear()` in [`include/kernel/mm/heap.h`](../../include/kernel/mm/heap.h). Scope is per-CPU; test runner is sequential single-CPU so this matches usage. Cross-CPU migration-aware filtering would need a future broadcast-to-all-CPUs variant.
- [x] Added `uint32_t kmalloc_fail_max_injections` to `per_cpu_data` alongside the fired counter. Delivers multi-fire from a single arm: after each fire, if `fired_counter < max_injections` the countdown auto-reloads to 1, so `max_injections_set(N)` + `kmalloc_fail_next()` once produces exactly N NULLs across the next N qualifying calls. Setter `_max_injections_set(N)` / `_clear()` re-zeroes the fired counter so the cap is relative to the arm-point. `max_injections=0` preserves classic single-shot semantics.
- [x] Extended to PMM: 4 `pmm_alloc_fail_*` fields in `per_cpu_data`; full setter set + static atomic counter in [`src/kernel/mm/pmm.c`](../../src/kernel/mm/pmm.c); hook in both `pmm_alloc_frame()` and `pmm_alloc_contiguous()` (count==1 delegates to _frame to avoid double-decrementing the countdown). Same IRQL gate. `test_runner.c` auto-clears between suites.
- [x] Extended to VMM mapping: `vmm_map_fail_next()` / `vmm_map_fail_countdown_set(N)` in [`include/kernel/mm/vmm.h`](../../include/kernel/mm/vmm.h). Hook in `vmm_map_page()` returns `-1` without touching page tables on forced fire. (Note: the section referred to `vmm_map()` / `vmm_alloc_range()` which do not exist under those names; `vmm_map_page` is the primary single-page map primitive, so the hook sits there. Partial-map rollback tests build multi-page loops around `vmm_map_page` and fire the injection mid-loop.)
- [x] Extended to `copy_to_user` / `copy_from_user` in [`src/kernel/cpu_security.c`](../../src/kernel/cpu_security.c) (the section referred to `src/kernel/arch/x86_64/usercopy.c`; the functions actually live in `cpu_security.c` where the SMAP STAC/CLAC wrappers are). `copy_user_fail_next()` forces either function to return `-1` BEFORE any user-space access is attempted (skipping `KERNEL_ACCESS_USER_BEGIN/END`). Syscall tests can now prove error-path propagation without corrupting target buffers.
- [x] Tests added: [`test_heap.c`](../../src/kernel/test/test_heap.c) 2 new suites (task-filter, max-injections cap); [`test_pmm.c`](../../src/kernel/test/test_pmm.c) 3 new suites (fail_next, countdown(3), IRQL-gate); [`test_vmm.c`](../../src/kernel/test/test_vmm.c) 1 new suite (vmm_map_fail_next with map-read-remap round-trip); [`test_cpu_security.c`](../../src/kernel/test/test_cpu_security.c) 1 new suite (copy_to_user + copy_from_user share the gate; verifies target buffer unchanged on forced-fail). All use existing test categories (TEST_CAT_MM + TEST_CAT_X86) -- no new bat file needed.
- [x] Commit: `"test/mm,syscall: task-filter + total-hits cap; fault inject for pmm/vmm/copy_user"`

**Test checkpoint:** `kmalloc_fail_task_filter_set(my_pid)` + foreign-PID kmalloc is NOT failed + countdown remains armed for a matching call. `kmalloc_fail_max_injections_set(3)` + arm-fire loop of 10 produces exactly 3 forced NULLs + 7 successes. `pmm_alloc_fail_next()` + `pmm_alloc_frame()` returns 0 + subsequent call succeeds + injection counter advances by 1. `copy_user_fail_next()` + `copy_to_user(dst, src, 16)` returns -1 without writing dst; next call returns 0 and copies 16 bytes.

> **Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) + `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 13 new fault-inject suites, 0 failures (2 kmalloc + 5 pmm + 3 vmm + 3 copy_user: each non-kmalloc subsystem gained parallel task-filter + max-cap tests during review to close a copy-paste coverage gap)

> **Notes:**
> - Shipped 4 parallel fault-inject surfaces: kmalloc (§1 + §6 extensions), PMM, VMM `vmm_map_page`, copy_to_user/copy_from_user. Each has the same 4-field per-CPU state (countdown + task_pid + max_injections + fired_counter) and the same 9 public setters (`_countdown_set/clear/_next`, `_injections_triggered`, `_task_filter_set/clear`, `_max_injections_set/clear`, `_fired_counter`).
> - Per-CPU storage lives in the KERNEL_TESTS tail of `struct per_cpu_data` (16 fields × 4 bytes = 64 bytes per CPU). Existing asm-referenced offsets untouched.
> - Hooks all gate on PASSIVE_LEVEL + optional task-pid filter + optional max-injections cap before checking/decrementing the countdown. Same gate ordering everywhere so a test author can reason about one set of semantics across all 4 subsystems. Hooks live AT THE TOP of the target function so a forced fire leaves observable state (heap/PMM bitmap/PTE/user memory) byte-identical to a real failure.
> - `test_runner.c` calls 12 `_clear()` helpers (3 per subsystem) between each suite so one suite's armed state cannot poison the next. Matches the §1 hygiene pattern.
> - 7 new test suites exercise the new API surfaces: task-filter skip-then-fire, max-injections cap yielding exactly N fires in M calls, PMM `fail_next`/countdown/IRQL-gate, VMM `vmm_map_fail_next` round-trip, copy_to_user + copy_from_user error propagation.
> - Downstream consumers: TODO-12 §5 `Test gaps` allocator-rollback test (needs >4 KiB allocation failure path) now unblocked via `pmm_alloc_fail_next()`; syscall error-propagation tests throughout `02-kernel-core` can use `copy_user_fail_next()`.
> - Scope boundary: §6 owns the multi-allocator fault-inject extensions. §8 (heap-leak detection) is independent. `vmm_alloc_range()` referenced in the section spec doesn't exist under that name; the hook lives in the primary `vmm_map_page` primitive.

> **Verified:** 2026-04-19 | commit `b1fbfc20` | 7/7 items | build OK | 13 fault-inject suites across 4 files (TEST_CAT_MM + TEST_CAT_X86); 4 subsystems x 9 setters = 36 public APIs
> **Quality reviewed:** 2026-04-19 | Codex 4x (adversarial x2, quality x2) | 1H+3M+1L fixed, 0 open | scope: kernel-code-quality

---

## 7. Test Action/Cleanup Registry (`test_add_action`)

KUnit built `kunit_kzalloc`, `kunit_kmalloc`, and its resource auto-free machinery on top of a general `kunit_add_action(fn, ctx)` primitive that executes cleanup callbacks in LIFO order at test exit -- pass or fail. §3 `TEST_SCRATCH_KBUF` and §5 `TEST_KLOG_SUPPRESS` are two of many possible consumers; making the primitive first-class unlocks cleaner test code across the board and matches Linux's proven model. Without it, every new test-scoped cleanup pattern reinvents list management inside the test runner.

- [x] Added `int test_add_action(void (*fn)(void *), void *ctx)` to [`include/kernel/test/test.h`](../../include/kernel/test/test.h). Returns 0 on success, -1 on NULL fn / full list / re-entrant-during-drain (each -1 path emits a `[WARN] TEST: <suite> :: ...` line naming the suite).
- [x] Per-suite action stack lives as file-scope static `s_test_actions` in [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) (kept out of public `test_state_t` so the ABI stays minimal): `uint8_t count`, `uint8_t draining`, `struct { fn; ctx } actions[32]`. `test_actions_reset()` runs before each suite body, `test_actions_drain()` runs after in **LIFO** order, before advancing to the next suite.
- [x] Action drain runs at PASSIVE_LEVEL. If a suite left IRQL elevated (forgotten `KeLowerIrql` after `KeRaiseIrql`), the runner logs `[WARN] TEST: <suite> :: left IRQL elevated (<irql>) before drain` and force-lowers via `KeLowerIrql(PASSIVE_LEVEL)` so `kfree` / `klog` / blocking cleanup can run safely. Actions themselves must be non-blocking (documented in the header alongside the draining-reentrancy contract).
- [x] Migrate §3 `TEST_SCRATCH_KBUF` onto `test_add_action`: §3 shipped 2026-04-19 using `test_add_action` from day one (the macro expansion calls `test_add_action(test_scratch_free, name)` explicitly with rollback-on-failure semantics); no bespoke per-test scratch list was ever needed.
- [x] Migrate §5 `TEST_KLOG_SUPPRESS` onto `test_add_action`: §5 shipped 2026-04-19 using `test_add_action` from day one (cleanup record is kmalloc'd in `klog_suppress_begin`, registered via `test_add_action(klog_suppress_restore, rec)`); no bespoke per-test cleanup needed.
- [x] Sanity tests in [`src/kernel/test/test_harness.c`](../../src/kernel/test/test_harness.c) (TEST_CAT_BOOT, 9 suites). Pair-style: register-suite registers actions + records observations into file-scope statics; verify-suite asserts on those observations after the runner's drain fires between suites. Coverage: (a) 3 actions fire LIFO by context value; (b) 33rd `test_add_action` returns -1, first 32 drain; (c) `test_add_action(NULL, ctx)` returns -1; (d) re-entrant `test_add_action` from an action body returns -1 (draining flag blocks it); (e) action observes `PASSIVE_LEVEL` even when the suite leaked `DISPATCH_LEVEL`, next suite also starts at PASSIVE.
- [x] Commit: `"test: action/cleanup registry (test_add_action) + 9 harness suites"`

**Test checkpoint:** `[ OK ] TEST: Harness: 3 actions fired LIFO after suite exit :: LIFO: last-registered (0x3333) fires first`. `[ OK ] TEST: Harness: 32 actions drained after overflow :: exactly 32 actions drained`. `[ OK ] TEST: Harness: re-entrant add rejected during drain :: test_add_action during drain returned -1`. `[ OK ] TEST: Harness: runner force-lowered IRQL for drain :: action ran at PASSIVE_LEVEL after runner forced IRQL down`.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 9 new Harness suites, 0 failures

> **Notes:**
> - Shipped `test_add_action(fn, ctx)` public API in [`include/kernel/test/test.h`](../../include/kernel/test/test.h) + impl in [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c). 32-slot LIFO per-suite stack; NULL fn + full list + re-entrant-during-drain all return -1 with a `[WARN]` line.
> - Runner integration: `test_actions_reset()` zeroes the stack before each suite body; `test_actions_drain()` fires after, force-lowering IRQL to PASSIVE_LEVEL first so elevated-IRQL leaks from one suite don't poison the next. Both hooks sit in the existing suite loop in `test_runner_run` (no new `test_runner_run_one` helper).
> - Draining-reentrancy contract: `test_add_action` returns -1 while a drain is in progress. Prevents the pathological case where an action re-registers itself and the drain loop runs forever. Documented in header + enforced by a regression suite.
> - Downstream consumers: §3 `TEST_SCRATCH_KBUF` and §5 `TEST_KLOG_SUPPRESS` will dispatch their cleanup through `test_add_action` from day one; the two "Migrate §3/§5" items in this section close as those sections ship. No bespoke per-test list code needed in either.
> - Canonical doc: the header comment at [`include/kernel/test/test.h`](../../include/kernel/test/test.h) `test_add_action` block documents the API contract (return codes, reentrancy, IRQL, non-blocking); the impl header comment in [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) documents the single-CPU SMP assumption of the sequential test_runner.
> - Scope boundary: §7 owns the generic registry. §3 (scratch-buffer) and §5 (klog-suppress) are consumers, not owned here. Generalised tracing of action history / debugfs-style introspection is out of scope.

> **Verified:** 2026-04-19 | commit `8a34cc1b` | 7/7 items | build OK | 11 harness suites added (registry + LIFO + overflow + NULL + re-entrant + IRQL x2 levels); §3 + §5 migrations closed 2026-04-19 when those sections shipped
> **Quality reviewed:** 2026-04-19 | Codex 4x (coverage, adversarial x2, quality) | 1H+7M fixed, 0 open | scope: kernel-code-quality

---

## 8. Per-Test Heap-Leak Detection

Record `heap_stats().used_bytes` at test start and end; if the delta is non-zero after §7 action drain, fail the test with a message naming the leaked bytes. Every existing `[ OK ]` test becomes a leak-coverage test for free, catching ~90% of real-world leaks without the weight of KASAN or kmemleak. `TEST_EXPECT_LEAK(bytes, reason)` and `TEST_LEAK_IGNORE` opt-outs handle the two legitimate exceptions (intentional long-lived state, non-heap allocator usage).

- [x] Per-test heap-leak detection state added to [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) as file-scope statics (`s_heap_used_at_entry`, `s_expected_leak_bytes`, `s_leak_ignore`, `s_last_leak_delta`) rather than extending `test_state_t` -- the ABI stays minimal, only the new `uint32_t leaked` counter lives on the public struct. Suite loop snapshots `heap_get_used()` before body and re-reads AFTER the §7 action drain (section refers to `heap_stats().used_bytes`; `heap_get_used()` is the actual API and returns the same counter).
- [x] Non-zero delta without `TEST_EXPECT_LEAK` / `TEST_LEAK_IGNORE` -> log `[LEAK] TEST: <name> :: leaked <delta> bytes (entry <pre> -> exit <post>)` via `klog(LOG_ERROR, "TEST", ...)` and increment `g_test_state.leaked`. The test's pass/fail verdict is independent; leaked is a third axis in the summary.
- [x] `TEST_EXPECT_LEAK(bytes, reason)` macro in [`include/kernel/test/test.h`](../../include/kernel/test/test.h) calls `_test_expect_leak(bytes, reason)` which stores both values in file-scope statics. When the post-drain delta is non-zero the classifier emits `[LEAK-OK] TEST: <name> :: intentional <bytes> (reason)` at `LOG_INFO` and does NOT bump the leaked counter. Byte comparison is tolerant (block-header overhead means a kmalloc(64) unfreed surfaces as ~72-80 byte delta; the match is "non-zero delta vs non-zero expected").
- [x] `TEST_LEAK_IGNORE(reason)` macro sets `s_leak_ignore = 1` for the current suite; the classifier bypasses the delta check entirely and emits `[LEAK-SKIP] TEST: <name> :: <reason>`. For tests that exercise PMM / VMM paths where `heap_get_used()` is the wrong oracle.
- [x] Summary line now includes `, %u leaked` after pending: `=== N tests passed, 0 failed, S skipped, P pending, L leaked (X.Xs) ===`. The "tests passed" prefix is load-bearing for `scripts/test.sh`'s summary regex (kept literal to avoid breaking CI gate). Failure form: `=== N passed, F FAILED, S skipped, P pending, L leaked (of TOTAL) (...) ===`. Leaks stay advisory; runner exit code is unaffected by L.
- [x] 4 `TEST_CAT_BOOT` harness suites in [`test_harness.c`](../../src/kernel/test/test_harness.c) (pair pattern). (a) deliberate `kmalloc(64)` unfreed + `TEST_EXPECT_LEAK(64, ...)`; verify suite reads `test_runner_last_leak_delta()` diagnostic and asserts `>= 64` bytes observed (proves the detector ran + classified as `[LEAK-OK]` without polluting the summary L counter). (b) `TEST_SCRATCH_KBUF(buf, 256)` via §7 action drain; verify asserts `last_leak_delta == 0` (proves drain-path cleanup is visible to the leak detector). Case (c) from the spec ("TEST_EXPECT_LEAK + kmalloc(64) unfreed logs [LEAK-OK] and passes") is structurally identical to (a) -- the diagnostic-getter approach tests the classifier without duplicating the pattern.
- [x] Commit: `"test: per-test heap-leak detection via heap_stats delta (advisory until retrofits complete)"`

**Test checkpoint:** `bash scripts/test.sh` summary line now includes `, L leaked`. The harness suite `Harness: leak detector reported > 64 bytes for kmalloc route` asserts the detector observed the expected delta; `Harness: scratch drain leaves last_leak_delta == 0` asserts drain-path cleanup is detected. L > 0 in the summary is advisory -- existing tests may surface previously-hidden leaks that will be retrofitted with `TEST_EXPECT_LEAK` or genuine leaks fixed in follow-up commits.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 6 new harness suites (kmalloc + scratch + leak-ignore pair patterns), 0 failures; summary line gains `, L leaked` column; existing tests may surface advisory [LEAK] lines to be retrofitted gradually

> **Notes:**
> - Shipped per-test heap-leak detection in [`test_runner.c`](../../src/kernel/test/test_runner.c). Snapshots `heap_get_used()` before each suite body; re-reads after §7 action drain; emits `[LEAK]` / `[LEAK-OK]` / `[LEAK-SKIP]` based on opt-out flags.
> - Public API: `TEST_EXPECT_LEAK(bytes, reason)` + `TEST_LEAK_IGNORE(reason)` in [`test.h`](../../include/kernel/test/test.h). Both are per-suite (flags reset on every new suite); neither alters test pass/fail verdict.
> - Diagnostic: `test_runner_last_leak_delta()` exposes the most recent suite's signed delta to harness regressions. Used by the §8 verify suites to prove the detector ran correctly without emitting a [LEAK] line (which would pollute the summary L counter).
> - Summary line: added `, %u leaked` to both zero-failure and failure forms. The existing `=== N tests passed, 0 failed` prefix is preserved so `scripts/test.sh`'s regex gate still matches.
> - Order of operations in the suite loop: snapshot -> reset opt-out flags -> `s->fn()` -> `test_actions_drain()` -> leak check -> `kmalloc_fail_*_clear()` hygiene. The leak check runs AFTER the drain so primitives like `TEST_SCRATCH_KBUF` and `TEST_KLOG_SUPPRESS` (both use `test_add_action` for cleanup) observe zero delta.
> - Only positive post-drain deltas are classified as leaks. Negative deltas (heap shrank) are silent by design: a verify suite that frees a previously-leaked buffer or a background kthread freeing boot-path state would otherwise spuriously trip [LEAK]. The signed value is still visible via `test_runner_last_leak_delta()` for harness diagnostics.
> - Downstream: with §4 retrofit (the only remaining `[ ]` section), existing TODO `Test gaps (NO current owner)` entries can now use per-test leak detection as the oracle for "did this test actually leak?" -- previously required manual heap-stat inspection.
> - Scope boundary: §8 owns the heap-side delta check only. PMM / VMM detection is out of scope (use `TEST_LEAK_IGNORE` for non-heap-primary tests). Cross-test leak detection (e.g. global state accumulation across the whole run) is also out of scope; §8 is per-suite only.

> **Verified:** 2026-04-19 | commit `603ea0a4` | 7/7 items | build OK | 6 new BOOT harness suites (kmalloc + scratch + leak-ignore pair patterns)
> **Quality reviewed:** 2026-04-19 | Codex 2x (adversarial, quality) | 3H+2M+2L fixed, 0 open | scope: kernel-code-quality

---

## 9. Audit + Classify the 50+ [LEAK] Failures §8 Surfaced

§8 shipped the per-test heap-leak detector with a **Note** that "existing tests may surface previously-hidden leaks that will be retrofitted with `TEST_EXPECT_LEAK` or genuine leaks fixed in follow-up commits." This section IS that follow-up. `bash scripts/test.sh` on TCG surfaces ~55 [LEAK] advisory lines spanning six subsystems; KVM surfaces fewer (timing-sensitive). The detector is not the bug; the detector is doing its job.

**Every [LEAK] failure must close along exactly one of three paths** (verify-before-commit contract: no [LEAK] line ships without a named category):

1. **Real leak** (missing `ObDereferenceObject`, unreleased control block, wrong close path, etc.). Fix the subsystem. Commit message names the specific allocation site and the missing free/dereference. After fix, `bash scripts/test.sh` shows the [LEAK] line GONE (no annotation in the test source).
2. **Intentional leak** (shared fixture, long-lived state that outlives the test by design, boot-path state that a test incidentally touched). Annotate with `TEST_EXPECT_LEAK(<bytes>, "<reason>")` at the test site. The `<reason>` must be a full sentence explaining WHY the leak is intentional so `git blame` answers "why this annotation?" without context from the commit. After annotation, `bash scripts/test.sh` shows `[LEAK-OK]` (classifier matched) instead of `[LEAK]` (unexplained).
3. **Detector edge case** (test exercises a non-heap allocator (PMM / VMM directly) so `heap_get_used()` is the wrong oracle, or test is deliberately probing the leak classifier itself). Annotate with `TEST_LEAK_IGNORE("<narrow reason>")` at the test site. Reason must name the SPECIFIC allocator path that bypasses the heap so a future maintainer does not broaden the scope. After annotation, `bash scripts/test.sh` shows `[LEAK-SKIP]` instead of `[LEAK]`.

No fourth category. If a leak does not fit any of these three, the `[LEAK]` line stays until it does. The post-retrofit gating change (final checklist item) makes `[LEAK]` without classification a CI-failing line.

> [!NOTE]
> §8's Notes explicitly scope leaks as ADVISORY: runner exit code is unaffected by the `L` column. A `[LEAK]` line today does not fail CI -- it pollutes the summary without gating. This section closes that visual debt and, more importantly, fixes real leaks surfaced by the retrofit that would otherwise accumulate silently in production.

**Inventory of current [LEAK] failures** (KVM 2026-04-22 + TCG 2026-04-22 combined; absence on either platform is noted):

- **Sched (5 suites, TCG-only):** `Sched: create thread`, `Sched: cooperative-yield fairness (regression)`, `Sched: joined kthread slot reuse (regression)`, `Sched: race-barrier release(a_first=0/1) wins 100x`. Leak sizes 64-13 KiB per suite. Likely real: scheduler teardown race on TCG where kthread reaping does not complete before the suite's post-drain heap snapshot -- same-suite second run shows different byte counts.
- **ETW (2 suites):** `ETW: NtCreateTrace`, `ETW: event not running`. Consistent 4 KiB delta each -- likely a per-trace control block not freed on NtCloseTrace (or not called in the FAIL path).
- **OB (13 suites):** handle table, duplicate handle, handle inherit, query directory, type stats, callbacks, handle quota, NT create+open directory, NT symlink roundtrip, NT section named open+query+extend, NT timer create+query / set+cancel / open existing. Deltas 160-2160 bytes. Pattern: tests that create Ob objects + close handles but don't `ObDereferenceObject` the final pin.
- **PEB/TEB (1 suite):** `PEB/TEB: kthread_create smoke`. 64 bytes -- likely the same kthread-teardown-race symptom as Sched.
- **IPC (5 suites):** kernel threads, mutex, semaphore, pipe, shared memory. Consistent 128 bytes each -- likely a shared-per-suite fixture allocation (kthread?) that counts as leak on the first leaker and propagates.
- **ALPC (25+ suites):** every ALPC test leaks 768-3400 bytes. Highest-density subsystem; suggests either (a) port+thread tear-down leaks a common-case allocation, or (b) the ALPC test fixtures don't use `TEST_SCRATCH_KBUF` / `test_add_action` drain so non-heap state accumulates per-suite.

- [/] Run `bash scripts/test.sh` on QEMU WHPX (2 CPUs) + QEMU TCG + VirtualBox + bare metal and capture the `[LEAK]` lines from each to a triage sheet. KVM 2026-04-22: 56 lines baseline captured, post-fix 1823/1823 PASS + 0 leaked. WHPX 2026-04-22 (2 CPUs): 1821/1821 PASS + 0 leaked (2-test KVM-only delta expected on MSR-gated cases). TCG 2026-04-23 (via new `FORCE_TCG=1` override in `scripts/test.sh`): 2166/2166 kernel + 16/16 user-mode PASS + 0 leaked. VirtualBox + bare metal still pending; user runs those on their rig.
- [x] Sched suite audit + PEB/TEB kthread_create smoke + IPC 5-suite audit -- all 11 suites closed by a SINGLE subsystem fix, not a test-harness annotation: [`src/kernel/ob/ob_ns.c`](../../src/kernel/ob/ob_ns.c) gained a new `ObpRemoveFromDirectory(dir, object)` helper that unlinks an entry from a namespace directory's linked list, frees the entry node (kfree), clears `OB_FLAG_NAMED` + `hdr->name` on the object header, and drops the directory's reference. [`src/kernel/ob/ob_thread.c`](../../src/kernel/ob/ob_thread.c) `ob_thread_mark_dead` now calls it to actually remove the `\KernelObjects\Thread<pid>.<tid>` entry (previously `ObMakeTemporaryObject` only cleared the PERMANENT flag; the directory kept its ref forever, pinning the THREAD_OBJECT + entry node = ~144 bytes per kthread). Symmetric fix applied to [`src/kernel/ob/ob_process.c`](../../src/kernel/ob/ob_process.c) `ob_process_mark_dead`. Codex adversarial [H] fix: added IRQ-disable (RFLAGS save/restore) around the unlink+free+deref critical section to prevent timer-preemption-triggered UAF against concurrent readers. Before: 56 leaks. After: 45 leaks (11 fixed: 5 Sched + 1 PEB/TEB + 5 IPC). Zero regressions (1815 kernel tests + 16 user-mode binaries still PASS). Commit: `"ob+sched: -9 kthread/ipc leak -- ObpRemoveFromDirectory drops namespace entry on mark_dead"`
- [x] **Broader OB namespace locking pass:** OBJECT_DIRECTORY gained `spinlock_t lock;` in [`include/kernel/ob/ob_ns.h`](../../include/kernel/ob/ob_ns.h); [`src/kernel/ob/ob_ns.c`](../../src/kernel/ob/ob_ns.c) wraps all 4 local call sites (`ObInsertObject`, `ObpRemoveFromDirectory`, `ObpLookupDirectory`, `ObLookupObjectByName`) and [`src/kernel/ob/ob.c`](../../src/kernel/ob/ob.c) wraps `NtQueryDirectoryObject`. The cli/sti stopgap is gone. `ObInsertObject` now takes the directory-owned reference BEFORE publishing the entry (Codex [H] fix: previously a window existed where another CPU could find and remove the entry before the ref existed, UAF'ing the delayed ObReferenceObject). `ObpLookupDirectory` contract changed to return a REF-OWNED directory -- callers MUST deref (Codex [H] fix: previously lookups could return a freed body if a concurrent ObMakeTemporaryObject + ObDereferenceObject raced the walk). `ObLookupObjectByName` consumes the ref on every exit path. New regression test `test_ob_ns_locking_stress` in [`src/kernel/test/test_ob.c`](../../src/kernel/test/test_ob.c) spawns a worker doing 200 insert+remove cycles concurrent with 500 NtQueryDirectoryObject enumerations. 1823 tests pass, 0 leaks. Commit: `"ob: -9 broader namespace locking pass -- per-directory spinlock + ref-owning lookup"`
- [x] ETW 2-suite audit: closed by `NtStopTrace` / `NtTraceControl(STOP)` accepting IDLE state (previously rejected, leaked the buffer). [`src/kernel/etw.c`](../../src/kernel/etw.c) -- 2 leaks fixed. Commit: `"etw: -9 leak -- NtStopTrace + NtTraceControl(STOP) release IDLE sessions"`
- [x] OB 13-suite audit: handle table destroy + named-object unlink helper (`test_ob_cleanup_named`). Commit: `"test: ob -- LEAK retrofit closes 13 suites in -9 cluster"`
- [x] ALPC 25+ suite audit: closed by `test_alpc_cleanup_named` helper + `AlpcDisconnectPort` before NtClose to break ConnectedPort cross-link cycles. 30 leaks fixed. Commit: `"test: alpc -- LEAK retrofit closes last 30 suites, session L=0"`
- [x] After each subsystem lands a fix, re-run `bash scripts/test.sh` and verify the `L leaked` column decreases correspondingly. Final KVM reading: `=== 1823 tests passed, 0 failed, 14 skipped, 31 pending, 0 leaked (1.1s) ===`.
- [x] Post-retrofit: change the runner to fail CI on `L > 0`. [`scripts/test.sh`](../../scripts/test.sh) now extracts the `N leaked` count from the summary and folds it into `FAILED` so any unannotated leak fails the run. Intentional leaks that carry `TEST_EXPECT_LEAK` / `TEST_LEAK_IGNORE` are suppressed at the kernel level and never reach this gate.
- [x] Commit: `"test: -9 [LEAK] retrofit complete -- L column is now CI-gating"`

**Test checkpoint:** `bash scripts/test.sh` on QEMU WHPX + QEMU TCG + VirtualBox + bare metal reports `=== N tests passed, 0 failed, S skipped, P pending, 0 leaked (X.Xs) ===` (the `, 0 leaked` is the new invariant). Every previously-leaking suite either (a) has a concrete subsystem fix with commit hash in this section's stamps, or (b) carries a `TEST_EXPECT_LEAK(<bytes>, "<reason>")` with the reason visible in `git blame`. Runner exit code gates on `L > 0` without an annotation.

> [!WARNING]
> This is a 50-suite debug session split across at least 5 subsystem owners. Realistic scope: one session per subsystem (Sched + PEB/TEB together is probably one, IPC one, ETW one, OB one, ALPC one) plus a final gating-change commit. Do NOT attempt to close all 5 in a single session; each subsystem's fixes need their own Codex adversarial review and regression check.

> **Test runner:** `scripts\debug\kernel\run-all-kernel-tests.bat` | 1823 tests pass, 0 leaked (KVM 2026-04-22)
> **Notes:**
> - Closed all 56 [LEAK] advisory lines across six subsystems (Sched 5, PEB/TEB 1, IPC 5, ETW 2, OB 13, ALPC 25+). Final KVM run: `1823 tests passed, 0 failed, 14 skipped, 31 pending, 0 leaked`.
> - Root-cause fixes dominate over test-side annotations: `ObpRemoveFromDirectory` + per-directory spinlock in `src/kernel/ob/ob_ns.c` closed 11 suites as one subsystem fix rather than 11 separate `TEST_EXPECT_LEAK` stamps; ETW IDLE-state acceptance in `src/kernel/etw.c` closed the per-trace buffer leak; `test_ob_cleanup_named` + `test_alpc_cleanup_named` helpers plus `AlpcDisconnectPort` in `src/kernel/test/test_alpc.c` closed the remaining 38.
> - CI gate promoted: `scripts/test.sh` now folds `N leaked` into `FAILED` and fails-closed on summary format drift (no silent pass on unparseable output). Unannotated leaks are CI-failing from this commit forward.
> - Review pass (2026-04-22) added `ObReferenceObject(dir)` pin around `NtQueryDirectoryObject` enumeration (narrows the pre-existing handle-close UAF window; full fix owned by D02 T05 §3 `ObpReferenceObjectByHandle` retrofit list).
> - Scope boundary: `[/]` Implementation Order reflects the 1 open item (platform coverage beyond KVM/WHPX/TCG: VBox + bare metal runs left to the user's rig; WSL-side KVM + WHPX + TCG all confirmed 0-leak). 7/8 checklist items closed. Teardown perf (O(n) path-lookup in `ob_thread_mark_dead`) is an accepted follow-up owned by D02 T05 §4 (OBJECT_HEADER parent-dir cache).
> **Verified:** 2026-04-22 | commit `0981dd8f` | 7/8 items | build OK | tests 1823/1823 PASS, 0 leaked (KVM); 1821/1821 PASS, 0 leaked (WHPX 2 CPUs); 2026-04-23 TCG: 2166/2166 kernel + 16/16 user-mode PASS, 0 leaked (via new `FORCE_TCG=1` override)
> **Accepted:** [H] NtQueryDirectoryObject residual handle-close UAF window; ObReferenceObject pin narrows it but the atomic-lookup+ref primitive is the real fix (reason: handle-table atomicity is broader than §9 scope) -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle` primitive" at line 112 -- retrofit list now explicitly names `NtQueryDirectoryObject` in `src/kernel/ob/ob.c:437`)
> **Accepted:** [M] `ob_thread_mark_dead` / `ob_process_mark_dead` do O(n) `ObLookupObjectByName` + linear `ObpRemoveFromDirectory` walk under IRQ-off spinlock on every thread exit; not blocking today but a scalability regression under kthread churn (reason: perf refinement, not correctness) -> XREF: 02-kernel-core/TODO-05 §4 (item: "Cache parent-directory + entry linkage in `OBJECT_HEADER` so teardown avoids path relookup" at line 187)
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 1M fixed, 2 open | scope: kernel-code-quality

---

## 10. An Over-Long Failing Assertion Wedges the Boot Instead of Reporting

> **Spawned-by:** root

Found while negative-controlling the emitted-code probes in `01-boot-platform/TODO-10 §22`, and the harness is the owner rather than that section: this reproduces for ANY test, and it fires only on the FAILURE path, which is exactly when the harness has to work. The observed cost was three wasted verification cycles reading a hang as a defect in the code under test -> XREF: `01-boot-platform/TODO-10 §22` (item: "Dedicated vector-2 entry stub").

- [x] A failing `TEST_ASSERT` whose composed klog line exceeds the 256-byte message buffer wedges the boot rather than reporting the failure, so the louder the assertion, the likelier it silently eats its own result.
  - ROOT CAUSE, traced not inferred: `vformat_buf`'s `BUF_PUT` macro (`src/kernel/klog.c:130`) did not evaluate its argument on the full-buffer branch, so `while (*s) BUF_PUT(*s++);` (`klog.c:223`) never advanced `s` past the first byte that did not fit and spun forever -- inside `klog_emit`'s ring lock with interrupts disabled, which is why the machine went silent with no fault. `BUF_PUT(*fmt++)` and the three `BUF_PUT(tmp[--n])` digit loops had the same shape. Fixed by evaluating the argument exactly once into a local before the capacity test, and by guarding every BUF_PUT-calling loop on `!truncated` so total work is bounded by the buffer plus the format string rather than by an argument -> XREF: `00-infrastructure/TODO-03 §11` (item: "A `TEST_POISON_TAIL(buf, n)` fixture").
  - The defect was NEVER harness-specific: a plain `klog(LOG_ERROR, "TEST", "PROBE n=%u %s", ...)` wedged identically. Bisected to one byte -- composed length 255 emits and completes, 256 hangs -- so ANY kernel caller with a long line could halt the boot, not just a loud assertion.
  - Reporting is bounded at the harness layer too, as defense in depth AND because klog's truncation cut the wrong end: `test_fail_record_format` (`include/kernel/test/test.h:253`) reserves the `(file:line)` suffix FIRST, spends the remainder on the author message, and marks a cut record ` trunc=1`. All four failure emitters use it -- `_test_assert`, `_test_assert_eq`, `_test_assert_neq` and the broken-pending-contract path.
  - A second defect on the same path, found by the end-to-end control: `line` is an `int` and klog's 7th ARGUMENT (its 4th variadic one), so it landed in a stack vararg slot whose upper 4 bytes are uninitialized while klog's `%d` read 64 bits -- rendering `test_harness.c:-194693637781585198`. Composing the record without varargs removes it structurally; the kernel-wide class is filed -> XREF: `01-boot-platform/TODO-14 §13` (item: "Move `%d`/`%u`/`%x` to standard C width semantics").
- [x] Regression coverage, with a live negative control: the over-long klog tests would HANG the run on the old code, and four pure formatter suites pin exact fit, one-byte overflow, suffix survival and path-shedding.
  - The `Klog:` suites (`src/kernel/test/test_klog.c`) drive all three `BUF_PUT` callers that carried the defect -- `%s`, a bare literal format, and a digit loop crossing the limit -- plus the exact 255-vs-256 boundary, `INT64_MIN`, and short subsystem tags.
  - The four `Harness: failure-record ...` suites (`src/kernel/test/test_harness.c`) are pure, so the truncation branch -- unreachable from a green run, because a green run has no failing assertions -- is exercised on every boot with synthetic inputs.
  - Control observed live, not assumed: BEFORE the fix the sweep stopped dead at `n=244` with no further serial output; AFTER, all 241 lengths from 100 to 340 emit and the suite completes.
  - ORIGINAL FILING, now superseded and kept only as the report that opened this section: a ~158-char message produced no summary and stopped the boot at ~1.8s, while the SAME assertion under a ~48-char message reported cleanly. That filing marked the mechanism INFERRED and asked for a decision between bounding the message and making the truncation path non-blocking. Both were wrong about the location -- the mechanism is traced above, it is not in the harness, and both fixes shipped.
- [x] Commit: `"test: bound the failing-assertion message so a long one cannot wedge the boot"`

**Test checkpoint:** A test that fails with a message long enough to exceed the 256-byte klog entry still produces a parseable `FAIL: N of M failed` summary and names its own assertion, on the same boot, with no timeout; the existing suites stay green and the summary format `scripts/test.sh` parses is unchanged.

> **Test runner:** `bash scripts/test.sh SUITE=boot` (kernel `TEST_CAT_BOOT`); 8 added suites -- 4 `Klog: ...` (truncation shapes, the 255/256 boundary, INT64_MIN, short-tag classification) and 4 `Harness: failure-record ...`. Full run: 29368 kernel + 17 user-mode, 0 failed.

> **Notes:**
> - Shipped: `vformat_buf` truncation made bounded and side-effect-safe (`src/kernel/klog.c`), ending a kernel-wide boot hang, plus `test_fail_record_format` / `test_fail_detail_format` in `src/kernel/test/test_runner.c` and 8 new suites.
> - Integrates by replacing the six-vararg klog call in all four failure emitters with one pre-composed `"%s"`, so the record's shape is decided by a pure function the tests can drive directly.
> - Downstream: every klog caller, not just tests -- any message over 255 bytes previously hung the machine with the ring lock held and interrupts disabled.
> - Scope boundary: the klog format engine still reads all numeric conversions as 64-bit; converting it to standard C width semantics with `-Wformat` is filed as `01-boot-platform/TODO-14 §13`.
> - Contract doc: `include/kernel/klog.h` now states the 64-bit vararg rule and the truncation behavior at the declaration.

> **Verified:** 2026-08-14 | commit `24097d9db` | 3/3 items | build OK | tests 29373 kernel + 17 user-mode PASS, 0 failed, 0 leaked; smoke matrix 4/4 legs (KVM+TCG x 1+2 CPU); lint 0 errors
> **Accepted:** [M] klog reads every numeric conversion as a full 64-bit vararg, so a caller passing a 32-bit value into a stack vararg slot renders garbage; 3 verified-corrupting sites were cast at source, but the class needs an engine + call-site migration (reason: kernel-wide sweep with real truncation risk, not a section-scope change) -> XREF: 01-boot-platform/TODO-14 §13 (item: "Move `%d`/`%u`/`%x` to standard C width semantics (32-bit by default, 64-bit only under `l`/`ll`)" at line 382)
> **Accepted:** [L] an out-of-bounds READ that still returns the right answer is undetectable by any C-level assertion this harness can write, so the short-tag suite pins behavior rather than memory safety (reason: needs a fault-catching fixture the harness does not have) -- RESOLVED 2026-08-14 by §11 -> XREF: 00-infrastructure/TODO-03 §11 (item: "A poisoned-boundary fixture places a string so its NUL is the last readable byte before a never-mapped page" at line 411)
> **Quality reviewed:** 2026-08-14 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1M+6L fixed, 0 open | scope: kernel-code-quality

---

## 11. A Read One Byte Past a Buffer Is Invisible to Every Test We Can Write

> **Spawned-by:** §10 (review)
> **User impact:** a kernel helper that reads past the end of a caller's string keeps returning the RIGHT answer until the day that byte lands on an unmapped page, and then the machine faults inside the logger while it is reporting some other failure. Today nothing in the harness can tell the safe implementation from the unsafe one.

Found closing §10, which replaced a fixed-offset subsystem probe (`subsystem[4]`/`subsystem[5]`, read for every record under an ordinary short tag like `ob` or `irq`) with a bounded comparison. The behavior-preservation test that shipped with it passes against BOTH implementations, because the comparisons consuming those bytes short-circuit first and the classification answer never differed. That test says so in its own comment rather than implying a coverage it does not have -> XREF: `00-infrastructure/TODO-03 §10` (item: "A failing `TEST_ASSERT` whose composed klog line exceeds the 256-byte message buffer wedges the boot").

- [x] A poisoned-boundary fixture places a string so its NUL is the last readable byte before a never-mapped page, so a one-byte overread faults, is caught, and fails one assertion instead of the boot.
  - Shipped in `include/kernel/test/poison_tail.h` + `src/kernel/test/poison_tail.c` as `test_poison_tail_arm/probe/disarm` plus `TEST_POISON_TAIL_ASSERT_NO_OVERREAD`.
  - Not the drafted `TEST_POISON_TAIL(buf, n)` macro: the probe must run the suspect helper INSIDE a kernel-SEH bracket, which a placement-only macro cannot express.
  - `vmm_install_guard_page()` was rejected as the boundary. `guard_page_lookup()` panics unconditionally at `src/kernel/mm/vmm.c:934-940`, BEFORE `ki_dispatch_exception`, so a guard hit can never reach SEH. Only a plain unmapped VA can.
  - Kernel SEH works from a suite body given one precondition: `ki_seh_register()` refuses a node outside the thread's tracked stack window (`src/kernel/except.c:667`) and the boot thread tracks none, so an unbracketed `KI_TRY` there protects nothing.
  - `test_seh_open_window()`/`test_seh_close_window()` own that bracket; `test_except.c` forwards to them so the fixture and the SEH suite cannot drift apart.
  - The boundary is a 2 MiB carve from the top of the MMIO/fixmap window whose first page is mapped once and never unmapped, with `_Static_assert` containment against the window.
  - A `pmm_alloc_contiguous()` pair in the low identity map was rejected: it needs `vmm_split_huge_page()` on a live identity PDE, which republishes it as PRESENT|WRITABLE only (`src/kernel/mm/vmm.c:1652`), dropping User for 512 pages, unlocked and never reversed.
  - Neither VA is ever recycled, which is what makes it immune to a stale-TLB false GREEN: `vmm_flush_tlb()` is a local invlpg with no shootdown.
  - Fails CLOSED: boundary absence is read from the PTE Present bit via `vmm_query_flags()` (`vmm_get_physical()` cannot tell absent from mapped-to-frame-0), re-checked every arm, latched permanently on conflict.
  - One global page means one owner, so an overlapping arm is refused rather than merged, and a refused arm leaves the caller's fixture untouched.
- [x] Retrofit of the §10 short-tag suite onto the fixture: `klog_tag_is` is verified rather than pinned, with the fixed-offset form it replaced running as a regression control that must be caught.
  - `klog_probe_tag_is()` is a `KERNEL_TESTS`-only forwarder to the static `klog_tag_is`, so the fixture exercises the same function the renderer calls rather than a copy.
  - Poisoned tags cover a first-byte mismatch, a matching prefix that runs out, and an exact match whose terminator check lands on the last readable byte.
  - The full `klog()` path cannot be probed: it renders under `s_klog_lock` with interrupts disabled, and SEH declines to unwind with RFLAGS.IF clear (`src/kernel/except.c:761`).
- [x] Fixed while proving the fixture: kernel SEH published the faulting RIP where its contract promises the DATA address -> XREF: `02-kernel-core/TODO-23 §14`.
  - An address-selective filter therefore declined the very fault its handler was written to take, and the fixture could not tell an overread from an unrelated fault.
  - Two selectors, split by question: `ki_exception_data_address()` reports PRESENCE (so a NULL-deref at address 0 stays distinguishable from "no address"), and `ki_exception_fault_address()` falls back to the instruction.
  - Filters, handler bodies and dispatch telemetry share the handler-facing selector. WER deliberately takes the data-only one (its field means a data address and RIP is reported separately), and `ki_kernel_bugcheck_params` keeps `ExceptionAddress` for the STOP instruction-address parameter. Do NOT unify those two onto the handler selector; the code list is also the two codes `except.h` documents, guard-page excluded.
  - Filed the gap that let this ship green: nothing compiles the `EXCEPT_TELEMETRY=off` flavor -> XREF: `02-kernel-core/TODO-23` §19 (item: "Compile every declared flavor of the kernel in one gate, not just the default")
- [x] Commit: `"test: poisoned-boundary fixture so an overread fails a test instead of a boot"`

**Test checkpoint:** a deliberately reintroduced fixed-offset read of `subsystem[4]` fails the short-tag suite with a named assertion, on the same boot, without halting the run; the suite stays green against the bounded implementation.

> **Test runner:** `make test-boot` + `make test-except` (Windows: `scripts\debug\kernel\run-boot-tests.bat` AND `run-except-tests.bat` -- boot alone skips the three selector suites) -- 10 `Harness: poisoned tail ...` suites, `Klog: short subsystem tags classify without overreading`, and 3 `Except: ... fault address ...` selector suites; expect 0 failed, with the two positive controls (`detects a one-byte overread`, `the fixed-offset classifier this replaced is caught overreading`) passing and NO poisoned-tail skips.

> **Notes:**
> Shipped a poisoned-boundary fixture (`poison_tail.h`/`.c`) that turns a one-byte kernel overread into one failed assertion instead of a dead boot, by placing the NUL against a never-mapped page and catching the #PF with kernel SEH.
> Integrates through `test_seh_open_window()`, now shared with `test_except.c`, because `ki_seh_register()` silently no-ops on the boot thread's untracked stack and an unbracketed `KI_TRY` there protects nothing.
> Retrofitted §10's short-tag suite onto it via the `klog_probe_tag_is()` seam, so `klog_tag_is` is verified rather than pinned, with the fixed-offset predecessor kept as a control that must be caught.
> Fixed kernel SEH publishing the faulting RIP where its contract promises the DATA address; filters, handler bodies and dispatch telemetry now share one code-aware selector, while WER keeps a data-only one and bugcheck keeps its own STOP parameter layout.
> Boundary detection fails closed: PTE-Present-based, re-checked every arm along with the recorded frame, single-owner, and never recycling a VA so no stale TLB entry can fake a clean result.
> Scope boundary: the fixture is for NUL-terminated-string helpers only; APIs that legitimately require readable tail padding or documented SIMD overreads are out of scope by contract.

> **Verified:** 2026-08-14 | commit `e12e1124a` | 4/4 items | build OK (default + EXCEPT_TELEMETRY=off) | tests 29442 kernel PASS, 0 failed, 0 leaked; smoke matrix 4/4 legs (KVM+TCG x 1+2 CPU); lint 0 errors
> **Accepted:** [H] kernel SEH can only be armed by rewriting `thread_current()->stack_base`/`stack_size`, which is unsound because that cursor is global (`src/kernel/sched/task.c:5996`), and the 32 KiB window it publishes exceeds an 8 KiB kthread stack (reason: the cure is SEH registration ownership, owned by the exception-dispatch TODO, not by this harness) -> XREF: 02-kernel-core/TODO-23 §20 (item: "Give `ki_seh_register()` a way to protect the running thread without rewriting `stack_base`/`stack_size`" at line 810)
> **Accepted:** [M] `#GP`/`#NP` records fabricate `ExceptionInformation[1] = 0` because no address is known, so the new presence-reporting selector cannot tell them from a real NULL dereference and a filter keyed on address 0 can match an unrelated fault (reason: the fix is address-known provenance in the exception record, an ABI change) -> XREF: 02-kernel-core/TODO-23 §20 (item: "Distinguish an UNKNOWN fault address from a real fault at address 0" at line 815)
> **Accepted:** [M] the 2 MiB carve is asserted CONTAINED in the MMIO/fixmap window but nothing RESERVES it, and `vmm_map_page()` overwrites PTEs unconditionally (reason: the central kernel-VA allocator already has an owner, which `src/kernel/mm/vmm.c:1490` points at; the fixture defends itself by verifying boundary-absent and data-page-still-ours every arm) -> XREF: 03-memory-concurrency/TODO-01-vmm-memory-protection.md §MMIO mapping (item: "Implement `vmm_map_mmio(phys_base, size)` -- 4 KiB PTEs, `PCD=1`+`PWT=1` (UC), return VA via a central kernel VA allocator with reserved non-overlapping ranges" at line 240)
> **Accepted:** [L] two registered klog/ETW magic tests have empty bodies and assert nothing, so the runner counts two passes that verify nothing (reason: klog/ETW test surface, owned by the logging TODO) -> XREF: 02-kernel-core/TODO-04 §16 (item: "Give the two registered klog/ETW magic tests an assertion, or delete them" at line 486)
> **Quality reviewed:** 2026-08-14 | Codex 25x (design, adversarial x9, consistency x7, perf x2, test-coverage x3, re-adversarial x3) | 2H+9M+6L fixed, 0 open | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                                       | 🪟 Win11                                            | 🐧 Linux                                            | 🚀 Impossible OS                            |
| --- | --------------------------------------------- | --------------------------------------------------- | --------------------------------------------------- | ------------------------------------------- |
| 💎  | Slab/kmalloc fault injection                  | ⚠️ DV Low-Resources (heavy)                         | ✅ `failslab` + fail-nth                            | ✅ §1 `kmalloc_fail_countdown`              |
| 💎  | Multi-allocator fault injection               | ⚠️ DV LRS (coarse)                                  | ✅ `failslab` + `fail_page_alloc` + `fail_usercopy` | ✅ §6 pmm/vmm/copy_user countdowns          |
| 💎  | Task-scoped fault injection                   | ❌ Rare                                             | ✅ `task_filter` (fault-inject)                     | ✅ §6 `kmalloc_fail_task_filter`            |
| 💎  | Deterministic concurrency testing             | ⚠️ TAEF with effort                                 | ⚠️ KCSAN (probabilistic)                            | ✅ §2 `test_race_barrier_t` (yield-ordered) |
| 💎  | Test-scoped cleanup registry                  | ❌ Manual in TAEF                                   | ✅ `kunit_add_action`                               | ✅ §7 `test_add_action` (primitive shipped) |
| 💎  | Test-scoped scratch allocation                | ⚠️ Manual in TAEF                                   | ✅ `kunit_kzalloc` (kmalloc-only)                   | ✅ §3 `TEST_SCRATCH_KBUF` (kmalloc + PMM)   |
| 💎  | Per-test leak detection                       | ⚠️ DV verifier pool checks                          | ✅ `kmemleak` (kernel-wide)                         | ✅ §8 heap_used delta (per-test, built-in)  |
| ⭐  | Test-scoped klog level demotion               | ❌ None                                             | ❌ None                                             | ✅ §5 `TEST_KLOG_SUPPRESS`                  |
| ⭐  | Single-boot 436-suite runner                  | ❌ WDK run per-driver                               | ❌ KUnit one-module-at-a-time                       | ✅ existing `test=1` infrastructure         |
| ⭐  | CI-gated unannotated-leak counter             | ❌ DV advisory, not CI-gate                         | ⚠️ kmemleak is kernel-wide, not per-test CI-gate    | ✅ §9 `L leaked` folds into FAILED          |
| 💎  | Over-long log line cannot hang the kernel     | ✅ `DbgPrint` truncates at 512                      | ✅ `printk` truncates at 1024                       | ✅ §10 truncates + marks, never spins       |
| ⭐  | Failure record keeps its `file:line` when cut | ❌ tail cut first                                   | ❌ tail cut first                                   | ✅ §10 suffix reserved before author text   |
| 💎  | Buffer OVERREAD fails a test, not the boot    | ⚠️ DV special pool (per-driver, no in-test verdict) | ⚠️ KASAN (whole-kernel build flavor)                | ✅ §11 `TEST_POISON_TAIL` per-assertion     |

After §1-§8 land (all shipped 2026-04-19), in-kernel test coverage reaches Linux-KUnit-plus-fault-inject parity for allocator-failure, cleanup, and concurrency testing; §5 (klog demotion) and §8 (per-test leak delta, no KASAN required) give Impossible OS two real edges neither Win11 nor Linux offers at the in-kernel-test layer. §4 (sweep-and-retrofit) closed the inbound TODO-24 §4 ALPC deferred-test-gaps block as the first concrete consumer; future deferred-test-gaps stamps that name §1-§3 / §5-§8 as the unblocker get retrofitted on the same model. §9 (shipped 2026-04-22) promoted the §8 advisory L column into a CI-failing gate by closing all 56 [LEAK] lines (root-cause fixes in OB namespace locking, ETW IDLE state, and ALPC disconnect ordering; two `test_*_cleanup_named` helpers) and hardening `scripts/test.sh` to fail-closed on unparseable summaries -- a third edge neither Win11's Driver Verifier nor Linux's kernel-wide kmemleak delivers at the per-test, CI-gated layer. §11 (shipped 2026-08-14) adds a fourth: a one-byte out-of-bounds READ becomes one failed assertion on the same boot, where Driver Verifier's special pool is a per-driver mode with no in-test verdict and KASAN is a whole-kernel build flavor rather than something an individual suite can point at a single helper.

---

## Unit Tests

> Boot tests run with `test=1` in `boot.conf`.

- [x] `src/kernel/test/test_heap.c` covers §1 `kmalloc_fail_next` + `kmalloc_fail_countdown_set(N)` + IRQL gate (5 suites; shipped with §1)
- [x] `src/kernel/test/test_sched.c` covers §2 `test_race_barrier_t` release-a-first and release-b-first orderings (100-iteration drift check) + init-clears-state sanity
- [x] `src/kernel/test/test_harness.c` sanity tests for §3 `TEST_SCRATCH_KBUF` (7 TEST_CAT_BOOT suites: kmalloc route, PMM route, forced-fail cleanup, rollback-on-full-action-stack)
- [x] `src/kernel/test/test_boot_info.c` (16 payload-negative tests) + `test_peb_teb.c` (`test_uthread_rejects_kernel_task`) wrap their assertions in `TEST_KLOG_SUPPRESS`; 4 `TEST_CAT_BOOT` harness suites in `test_harness.c` prove demote + restore (including the no-pre-existing-override regression)
- [x] `src/kernel/test/test_heap.c` + `test_pmm.c` + `test_vmm.c` + `test_cpu_security.c` cover §6 multi-allocator fault injection: task-filter, max-injections cap, PMM + VMM-map + copy_user countdowns (7 new TEST_CAT_MM + TEST_CAT_X86 suites)
- [x] `src/kernel/test/test_harness.c` sanity tests for §7 `test_add_action` (9 TEST_CAT_BOOT suites: LIFO drain, overflow, NULL-fn reject, re-entrant-during-drain reject, IRQL recovery)
- [x] `src/kernel/test/test_harness.c` sanity tests for §8 heap-leak detection (6 TEST_CAT_BOOT suites: kmalloc unfreed + TEST_EXPECT_LEAK pair, TEST_SCRATCH_KBUF drain pair, TEST_LEAK_IGNORE bypass pair; verify suites consume `test_runner_last_leak_delta()` and `g_test_state.leaked` to confirm classification without polluting the summary L counter)
- [x] `src/kernel/test/test_harness.c` covers §11 poisoned-boundary fixture (10 TEST_CAT_BOOT suites incl. the detects-a-one-byte-overread control); `test_klog.c` retrofits the §10 short-tag suite onto it
- [x] `src/kernel/test/test_except.c` adds a §11 fault-address selector matrix (3 TEST_CAT_EXCEPT suites: the two documented data-address codes, a NULL-deref address of 0, and non-address rejection incl. an explicit guard-page exclusion)
- [x] `src/kernel/test/test_alpc.c` adds 3 §4 retrofit suites in TEST_CAT_IPC (`alpc: kmalloc-fail in pending alloc rolls back PoolUsageBytes`, `alpc: ReplyBodyCap clamps recv_buf_len > 65528`, `alpc: two-port lock-order stress`) consuming TODO-03 §1 + §2 + §3 + §5 primitives to close the TODO-24 §4 deferred test gaps block

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) + `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) + `scripts\debug\kernel\run-ipc-tests.bat` (SUITE=ipc) | ~23 new test-harness + allocator + alpc-retrofit suites, 0 failures, summary shows `L=0` on green tree

---

## Verification

- [x] `make test-boot` runs §2, §3, §5, §7, §8 sanity suites (test-harness itself). Confirmed via `bash scripts/test.sh QUIET=1` 2026-04-19: PASS: 1680 tests passed (TCG run, all categories).
- [x] `make test-mm` runs §1 + §6 allocator-fault-injection suites. Confirmed in same run.
- [x] `make test-sched` runs §2 race-barrier and §6 task-filter siblings-isolation suites. Confirmed in same run.
- [x] End-of-run `=== N tests passed, F failed, S skipped, P pending, L leaked (X.Xs) ===` summary line present. Observed after §9 leak-retrofit closed all 56 [LEAK] lines (2026-04-22): `=== 1823 tests passed, 0 failed, 14 skipped, 31 pending, 0 leaked (1.1s) ===` on KVM; `1821/1821 + 0 leaked` on WHPX; `2166/2166 + 0 leaked` on TCG (2026-04-23). L=0 is now CI-gating (`scripts/test.sh` folds unannotated leaks into FAILED).
- [x] `grep -rn "Test gaps (NO current owner)\|Test gaps (deferred to" todo/` returns only self-references in this section (descriptive text, not active stamps); no inbound deferral to TODO-03 remains. Confirmed 2026-04-19.

> **Closed:** 2026-04-19 | unit tests fully wired across all 8 sections (~23 new test-harness + allocator + retrofit suites in `boot`/`mm`/`sched`/`ipc` categories) | verification 5/5 automated PASS | 0 manual items pending
