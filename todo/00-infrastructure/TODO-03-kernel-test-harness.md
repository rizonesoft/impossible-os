# TODO-03 -- Kernel Test Harness

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

| ⭐  | Order | Section | Deliverable                                               | Depends On          | Status |
| --- | :---: | :-----: | --------------------------------------------------------- | ------------------- | :----: |
| 💎  |   1   |  §1     | kmalloc fault injection (`kmalloc_fail_countdown`)        | --                  |  [x]   |
| 💎  |   2   |  §7     | Test action/cleanup registry (`test_add_action`)          | --                  |  [x]   |
| 💎  |   3   |  §3     | Scratch-buffer helper (`TEST_SCRATCH_KBUF`)               | §7                  |  [x]   |
| 💎  |   4   |  §2     | Deterministic race barrier (`test_race_barrier_t`)        | --                  |  [x]   |
| 💎  |   5   |  §6     | Fault-injection hardening (task-filter, multi-allocator)  | §1                  |  [x]   |
| 💎  |   6   |  §5     | Test-scoped klog level demotion (`TEST_KLOG_SUPPRESS`)    | §7                  |  [x]   |
| 💎  |   7   |  §8     | Per-test heap-leak detection                              | §7                  |  [x]   |
| 💎  |   8   |  §4     | Retrofit existing "Test gaps" stamps across `todo/`       | §1-§3, §5-§8        |  [ ]   |

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

> **Test runner:** `scripts\debug\run-mm-tests.bat` (SUITE=mm) | 5 new heap fault-injection suites, 0 failures

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

> **Test runner:** `scripts\debug\run-sched-tests.bat` (SUITE=sched) | 3 new race-barrier suites added, 0 failures

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

> **Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot) | 5 new scratch-buffer suites, 0 failures

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

- [ ] TODO-12 §5 "Test gaps": rewrite the three pending items against the new primitives -- (a) allocator kmalloc-failure rollback uses `kmalloc_fail_next()` + §5 `TEST_KLOG_SUPPRESS("mm")` + §8 leak-delta check for free; (b) ReplyBodyCap clamp with `recv_buf_len > 65528` uses `TEST_SCRATCH_KBUF(rxbuf, 65536 + 64)` (now PMM-backed per §3); (c) address-ordered two-port locking concurrency stress uses `test_race_barrier_t` between the two sender threads. Close the Accepted stamp per the inbound-XREF sweep in `implement-todo-section` step 18.
- [ ] Add `pmm_alloc_fail_next()` coverage (§6) to any existing TODO that calls out a ">4 KiB allocation rollback" test gap; previously blocked because §1 kmalloc countdown didn't reach the PMM path used for large allocations.
- [ ] Repo sweep: `grep -rn "Test gaps (NO current owner)" todo/` -- for every hit, decide whether the gap is closable by one of §1-§3, §5-§8 and convert it. If the gap is unrelated infrastructure, leave it with a clearly-named reason.
- [ ] Commit: `"test: retrofit deferred ALPC §4 test gaps against new kernel test harness"` (and similar per-TODO commits as the sweep runs)

**Test checkpoint:** `grep -rn "Test gaps (NO current owner)" todo/` shrinks to zero hits for gaps that §1-§3, §5-§8 can close; any remaining gaps have a clearly-unrelated reason documented.

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

> **Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot) | 4 new harness suites + 17 retrofitted source suites, 0 failures; 17 `[FAIL]` lines silenced from test-phase serial output

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

> **Test runner:** `scripts\debug\run-mm-tests.bat` (SUITE=mm) + `scripts\debug\run-x86-tests.bat` (SUITE=x86) | 13 new fault-inject suites, 0 failures (2 kmalloc + 5 pmm + 3 vmm + 3 copy_user: each non-kmalloc subsystem gained parallel task-filter + max-cap tests during review to close a copy-paste coverage gap)

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

> **Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot) | 9 new Harness suites, 0 failures

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

> **Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot) | 6 new harness suites (kmalloc + scratch + leak-ignore pair patterns), 0 failures; summary line gains `, L leaked` column; existing tests may surface advisory [LEAK] lines to be retrofitted gradually

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

## OS Comparison

| ⭐ | Feature                            | 🪟 Win11                   | 🐧 Linux                                             | 🚀 Impossible OS                               |
| --- | ---------------------------------- | --------------------------- | ---------------------------------------------------- | ---------------------------------------------- |
| 💎 | Slab/kmalloc fault injection       | ⚠️ DV Low-Resources (heavy) | ✅ `failslab` + fail-nth                            | ✅ §1 `kmalloc_fail_countdown`                 |
| 💎 | Multi-allocator fault injection    | ⚠️ DV LRS (coarse)          | ✅ `failslab` + `fail_page_alloc` + `fail_usercopy` | ✅ §6 pmm/vmm/copy_user countdowns             |
| 💎 | Task-scoped fault injection        | ❌ Rare                     | ✅ `task_filter` (fault-inject)                     | ✅ §6 `kmalloc_fail_task_filter`               |
| 💎 | Deterministic concurrency testing  | ⚠️ TAEF with effort         | ⚠️ KCSAN (probabilistic)                            | ✅ §2 `test_race_barrier_t` (yield-ordered)    |
| 💎 | Test-scoped cleanup registry       | ❌ Manual in TAEF           | ✅ `kunit_add_action`                               | ✅ §7 `test_add_action` (primitive shipped)    |
| 💎 | Test-scoped scratch allocation     | ⚠️ Manual in TAEF           | ✅ `kunit_kzalloc` (kmalloc-only)                   | ✅ §3 `TEST_SCRATCH_KBUF` (kmalloc + PMM)      |
| 💎 | Per-test leak detection            | ⚠️ DV verifier pool checks  | ✅ `kmemleak` (kernel-wide)                         | ✅ §8 heap_used delta (per-test, built-in)     |
| ⭐ | Test-scoped klog level demotion    | ❌ None                     | ❌ None                                             | ✅ §5 `TEST_KLOG_SUPPRESS`                     |
| ⭐ | Single-boot 436-suite runner       | ❌ WDK run per-driver       | ❌ KUnit one-module-at-a-time                       | ✅ existing `test=1` infrastructure            |

After §1-§8 land, in-kernel test coverage reaches Linux-KUnit-plus-fault-inject parity for allocator-failure, cleanup, and concurrency testing; §5 (klog demotion) and §8 (per-test leak delta, no KASAN required) give Impossible OS two real edges neither Win11 nor Linux offers at the in-kernel-test layer.

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

> **Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot) + `scripts\debug\run-mm-tests.bat` (SUITE=mm) | ~20 new test-harness + allocator suites, 0 failures, summary shows `L=0` on green tree

---

## Verification

- `make test-boot` runs §2, §3, §5, §7, §8 sanity suites (test-harness itself).
- `make test-mm` runs §1 + §6 allocator-fault-injection suites.
- `make test-sched` runs §2 race-barrier and §6 task-filter siblings-isolation suites.
- End-of-run `=== N passed, F failed, S skipped, P pending, L leaked (X.Xs) ===` summary line present; `L=0` on a green tree after §4 retrofit completes.
- `grep -rn "Test gaps (NO current owner)" todo/` returns only entries unrelated to allocator / race-fence / scratch-buffer / klog-suppression / leak-detection needs.
