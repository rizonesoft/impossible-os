# TODO-03 -- Kernel Test Harness

> **Goal:** Kernel-internal test-time infrastructure that subsystem unit tests (under `src/kernel/test/test_*.c`) can opt into when they need to exercise rare control flow: allocator failure rollback, deterministic race windows between cooperating threads, and bulk scratch buffers that exceed the 8 KiB kernel stack. Each existing subsystem tests the hot path; the hooks in this TODO close the remaining corners without having to stand up a whole kernel debugger.

> [!IMPORTANT]
> **Current state:** `src/kernel/test/` has 436+ unit-test suites wired into the boot-time test runner (`test=1` in `boot.conf`). Every suite must currently test the normal path: there is no way for a test to ask `kmalloc` to fail on its 3rd call, no way to make two cooperating kthreads land on opposite sides of a lock acquisition at a chosen instant, and the 8 KiB kernel task stack caps any test-local buffer at about 4 KiB before overflowing (guard page trip). Concrete gaps that motivated this TODO are collected from TODO-12 §5 "Test gaps (NO current owner)".

---

## Inputs

- `src/kernel/mm/heap.c` -- `kmalloc`/`kfree` -- §1 fault-injection hooks install here
- `src/kernel/test/test.h` -- test macros (`TEST_ASSERT`, `TEST_SKIP`, `TEST_PENDING`) -- §1/§2 add helpers
- `src/kernel/sched/task.c` -- `kthread_create`, `thread_yield`, `thread_join` -- §2 race-fence uses these
- `src/kernel/sched/spinlock.h` -- spinlock primitives -- §2 adds a test-only checkpoint primitive
- `include/kernel/test/test.h` -- public test API (where the new helpers go)
- `CLAUDE.md` "Test Code -- No Live Boot Infrastructure Calls" -- the hooks below must obey the same safety contract (no live boot-path mutation from tests)
- → XREF: `T04 §3` -- user-mode test launcher is complementary; it runs user-mode binaries while this TODO stays in-kernel
- → XREF: `T01 §3, §7` -- canonical local test wrappers and host-side tooling regression policy keep repo-wide deferred-test sweeps on one supported entry path

---

## Outcome

- `kmalloc_fail_countdown(N)` / `kmalloc_fail_next()` -- test-only API that causes the next (or Nth) `kmalloc` call to return `NULL` without touching the real heap. Used by tests that need to prove quota-rollback and failure-cleanup paths.
- `test_race_barrier_t` -- test-only two-thread rendezvous primitive that pins both threads at named checkpoints so a test can observe behaviour at a controlled interleaving. Used by tests that need to prove lock-order or race-window invariants.
- `TEST_SCRATCH_KBUF(name, size)` -- macro that allocates a `kmalloc`'d scratch buffer with automatic free on test exit. Lets a test use >8 KiB buffers without overflowing the kernel stack or leaking on early return.
- Every existing `Test gaps (NO current owner)` entry in `todo/` resolves via one of these three primitives. When §1-§3 ship, those Accepted stamps get pruned via the inbound-XREF sweep.

---

## Implementation Order

| ⭐  | Order | Deliverable                                         | Depends On    | Status |
| --- | :---: | --------------------------------------------------- | ------------- | :----: |
| 💎  |   1   | kmalloc fault injection (`kmalloc_fail_countdown`)  | --            |  [x]   |
| 💎  |   2   | Deterministic race barrier (`test_race_barrier_t`)  | --            |  [ ]   |
| 💎  |   3   | Scratch-buffer helper (`TEST_SCRATCH_KBUF`)         | --            |  [ ]   |
| 💎  |   4   | Retrofit existing "Test gaps" stamps across `todo/` | §1, §2, §3    |  [ ]   |
| 💎  |   5   | Test-scoped klog level demotion (`TEST_KLOG_SUPPRESS`) | T02 T04 §3 |  [ ]   |

> 💎 = parity work -- Linux kernel self-test framework has `fault-injection` (`lib/fault-inject.c`), `kcsan`/`kasan` fences, and KUnit has `kunit_kzalloc()` scratch helpers. This TODO brings the same floor to the Impossible OS kernel test runner.

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

> **Test runner:** `scripts\debug\run-mm-tests.bat` (SUITE=mm)
> **Expected:** 5 new heap fault-injection suites pass, 0 failures

> **Verified:** 2026-04-15 -- Evidence mapped: `include/kernel/smp.h` (per_cpu_data.kmalloc_fail_countdown + _pad, #ifdef KERNEL_TESTS gated), `include/kernel/mm/heap.h` (4 prototypes under KERNEL_TESTS), `src/kernel/mm/heap.c` (s_kmalloc_fault_injections static counter + 4 helpers + in-kmalloc hook with `KeGetCurrentIrql() == PASSIVE_LEVEL` gate + `ifdef KERNEL_TESTS` include block), `src/kernel/test/test_runner.c` (include + auto-clear after every suite), `src/kernel/test/test_heap.c` (5 new TEST_CAT_MM suites: fail_next, countdown(3), clear-disarms, no-state-change, IRQL-gate). Build clean (`=== BUILD OK ===`). Existing asm-referenced per_cpu_data offsets at smp.h:131-145 (self=0, syscall_rsp0=24, user_rsp_scratch=32, kernel_cr3=104, user_cr3=112, kpti_scratch=120, kpti_syscall_target=128, kpti_isr_target=136) unchanged -- new fields live at offset 144+ inside the KERNEL_TESTS-only tail.
> **Quality reviewed:** 2026-04-15 -- kernel-code-quality Gates 1-11 walked clean (no stdlib, SMP-safe via per-CPU storage + RELAXED atomic global counter, no MMIO, every allocation check'd). Codex step-13 adversarial found 1 Medium (IRQ-context kmalloc on same CPU could steal pending injection) fixed pre-commit by `KeGetCurrentIrql() == PASSIVE_LEVEL` gate with regression test (`Heap: fault-inject IRQL gate`). Codex step-8 quality dispatch (dead-code + consistency + perf) returned no findings: all new surface consistently KERNEL_TESTS-gated, static counter reachable via test suites, pre-existing per_cpu_data offsets intact, hot-path cost confined to test builds (3-4 instructions in non-armed case, zero in release builds).

---

## 2. Deterministic Race Barrier

A two-thread rendezvous primitive for tests that need to observe behaviour at a specific interleaving (e.g. "sender reaches just before event_set; receiver reaches just after event_wait returns"). Without this, concurrency tests can only observe eventual behaviour, not specific orderings.

- [ ] `typedef struct test_race_barrier { spinlock_t lock; event_t a_reached; event_t b_reached; uint8_t a_arrived; uint8_t b_arrived; } test_race_barrier_t;`
- [ ] `void test_race_barrier_init(test_race_barrier_t *)` -- initialise both events as AUTO_RESET.
- [ ] `void test_race_barrier_arrive_a(test_race_barrier_t *)` -- thread A marks itself arrived, signals `a_reached`, then waits on `b_reached`. Returns only when both threads have reached the checkpoint.
- [ ] `void test_race_barrier_arrive_b(test_race_barrier_t *)` -- symmetric for thread B.
- [ ] `void test_race_barrier_release(test_race_barrier_t *, int a_first)` -- test decides the release order: if `a_first=1`, wake A's `a_reached` first, yield, then wake B.
- [ ] Unit test in `src/kernel/test/test_sched.c`: two worker kthreads reach a barrier, test releases A first and asserts A's post-barrier work observes the expected state; then reset and release B first and assert the symmetric observation.
- [ ] Document the hazard: do NOT hold other spinlocks while calling `test_race_barrier_arrive_*` -- the wait inside the barrier yields.
- [ ] Commit: `"test/sched: deterministic race barrier for two-thread interleaving tests"`

**Test checkpoint:** Barrier release-a-first causes thread A's post-checkpoint branch to win consistently; release-b-first causes B's branch to win. Exercised over 100 iterations in the unit test to catch scheduler drift.

---

## 3. Scratch-Buffer Helper

`TEST_SCRATCH_KBUF(name, size)` declares a `size`-byte `kmalloc`'d scratch buffer and registers a cleanup handler that frees it on test exit (including early `return` via `TEST_ASSERT` macros). Solves the "I need 64 KiB but stack is 8 KiB" case without leaking on assertion failure.

- [ ] `TEST_SCRATCH_KBUF(name, size)` expands to: `uint8_t *name = kmalloc(size); test_scratch_register_free(name);` with a NULL check that fails the test with `TEST_ASSERT_NOT_NULL(name, "scratch alloc ...")`.
- [ ] `void test_scratch_register_free(void *ptr)` -- appends to a per-test list; `test_runner_run_one` drains and kfrees after the test body returns (pass or fail).
- [ ] List lives in `struct test_state` in `test_runner.c`; bounded at 16 buffers per test (more than any test reasonably needs) with a clear error if exceeded.
- [ ] Unit test: a test that `TEST_SCRATCH_KBUF(buf, 65536)` and immediately `TEST_ASSERT(0, "forced fail")` -- kernel reports the failure AND the scratch buffer is freed (verifiable by checking `heap_stats().used_bytes` before/after).
- [ ] Commit: `"test: scratch-buffer helper for tests needing > 4 KiB local storage"`

**Test checkpoint:** A test that allocates 64 KiB via `TEST_SCRATCH_KBUF` and forces-fails leaves zero leaked bytes in the heap after the test runner moves on.

---

## 4. Retrofit Existing "Test Gaps" Stamps

After §1-§3 ship, sweep the repo for `Test gaps (NO current owner)` blocks and TEST_PENDING placeholders that were deferred only because this infrastructure did not exist. Rewrite each test to use the new primitives and prune the deferral note.

- [ ] TODO-12 §5 "Test gaps": rewrite the three pending items against the new primitives -- (a) allocator kmalloc-failure rollback uses `kmalloc_fail_next()`; (b) ReplyBodyCap clamp with `recv_buf_len > 65528` uses `TEST_SCRATCH_KBUF(rxbuf, 65536 + 64)`; (c) address-ordered two-port locking concurrency stress uses `test_race_barrier_t` between the two sender threads. Close the Accepted stamp per the inbound-XREF sweep in `implement-todo-section` step 18.
- [ ] Repo sweep: `grep -rn "Test gaps (NO current owner)" todo/` -- for every hit, decide whether the gap is closable by one of §1-§3 and convert it. If the gap is unrelated infrastructure, leave it.
- [ ] Commit: `"test: retrofit deferred ALPC §4 test gaps against new kernel test harness"` (and similar per-TODO commits as the sweep runs)

**Test checkpoint:** `grep -rn "Test gaps (NO current owner)" todo/` shrinks to zero hits for gaps that §1-§3 can close; any remaining gaps have a clearly-unrelated reason documented.

---

## 5. Test-Scoped klog Level Demotion (`TEST_KLOG_SUPPRESS`)

Tests that exercise error paths (validators, allocator failure rollback, bad-input rejection) currently emit the validator's `klog(LOG_ERROR, ...)` -- rendered as `[FAIL] subsys: ...` -- directly onto serial output during test runs. Diagnose-serial-log runs then see test-phase `[FAIL]` lines and have to hand-classify them as NOISE per-test, which (as the 2026-04-18 incident showed) leaks occasionally into "real" findings reports. The validator is dual-use: loud on real boot with bad input, quiet under test.

**Files:** `src/kernel/test/test.h` (new macro), `src/kernel/test/test_runner.c` (save/restore).

- [ ] `TEST_KLOG_SUPPRESS(subsystem)` block-scoped macro: on entry, saves current per-tag level via a new `klog_get_level(const char *subsys)` API and calls `klog_set_level(subsys, LOG_FATAL)` (LOG_FATAL is above any normal emitter, effectively silencing the subsystem). On exit (scope end), restores the saved level. Uses `__attribute__((cleanup))` so `TEST_ASSERT` early-return still restores.
- [ ] Add `klog_get_level(const char *subsystem)` to `include/kernel/klog.h` + `src/kernel/klog.c` -- returns the current override value from the 32-entry override table or the global default if no override.
- [ ] Retrofit `test_boot_payload.c` error-injection tests (lines around ~180 and across all 13 cases) to wrap each negative-test assertion in `TEST_KLOG_SUPPRESS("boot")`. Verify `[FAIL] boot: boot_payload:` lines disappear from the serial log during test runs while `[FAIL] boot: boot_payload:` on real-boot bad-payload paths remains loud.
- [ ] Retrofit `test_uthread_rejects_kernel_task` in `test_sched.c` to wrap the `uthread_create(PID 0)` assertion in `TEST_KLOG_SUPPRESS("sched")`. Verify the `[FAIL] sched: uthread_create: PID 0 has no PEB` line disappears from test output.
- [ ] Unit test: a wrapper calling `TEST_KLOG_SUPPRESS("mm")` around `klog(LOG_ERROR, "mm", "...")` emits nothing to serial; after the block, a subsequent `klog(LOG_ERROR, "mm", "...")` outside the block emits normally. Include a forced `TEST_ASSERT(0, ...)` inside the block to verify cleanup handler restores the level even on early exit.
- [ ] Remove the affected entries from the "Known Test Noise" table in `.claude/skills/diagnose-serial-log/SKILL.md` once those sites are silenced at source.
- [ ] Update `diagnose-serial-log/POLICIES.md` P4.3: after §5 ships, any test-phase `[FAIL]` line is a real finding (no more "NOISE because test-owned").
- [ ] Commit: `"test: TEST_KLOG_SUPPRESS block-scoped klog level demotion"`

**Test checkpoint:** A clean `bash scripts/test.sh` run produces zero `[FAIL]` lines in the test-phase window (between `=== KERNEL UNIT TESTS ===` and `N passed, M failed` summary) on a green tree. Diagnose-serial-log P4.3 FAIL/CRIT detector returns zero hits on the resulting serial log.

**Filed by:** diagnose-serial-log 2026-04-18 on debug-tmp/all-tests-serial.log (17 FAIL lines across 5 canonicalized keys: 16 boot_payload, 1 sched uthread_create).

---

## OS Comparison

| ⭐ | Feature                            | 🪟 Win11                   | 🐧 Linux                       | 🚀 Impossible OS                      |
| --- | ---------------------------------- | -------------------------- | ------------------------------- | ------------------------------------- |
| 💎 | Allocator fault injection          | ❌ Driver Verifier (heavy) | ✅ `lib/fault-inject.c`        | ⬜ §1 `kmalloc_fail_countdown`        |
| 💎 | Deterministic concurrency testing  | ⚠️ TAEF with effort        | ✅ KCSAN + KUnit               | ⬜ §2 `test_race_barrier_t`           |
| 💎 | Test-scoped kernel scratch buffers | ⚠️ Manual in TAEF tests    | ✅ `kunit_kzalloc()`           | ⬜ §3 `TEST_SCRATCH_KBUF`             |
| ⭐ | Single-boot 436-suite runner       | ❌ WDK run per-driver      | ❌ KUnit one-module-at-a-time  | ✅ existing `test=1` infrastructure   |

After §1-§4 land, in-kernel test coverage reaches Linux-KUnit-plus-fault-inject parity without requiring the heavyweight Driver Verifier / WDK workflow Windows leans on.

---

## Unit Tests

> Boot tests run with `test=1` in `boot.conf`.

- [ ] `src/kernel/test/test_heap.c` covers §1 `kmalloc_fail_next` + `kmalloc_fail_countdown_set(N)`
- [ ] `src/kernel/test/test_sched.c` covers §2 `test_race_barrier_t` release-a-first and release-b-first orderings
- [ ] `src/kernel/test/test_runner.c` sanity test covers §3 `TEST_SCRATCH_KBUF` cleanup-on-failure path

> **Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot)
> **Expected:** 5 new test-harness suites pass, 0 failures

---

## Verification

- `make test-boot` runs §1 + §3 suites.
- `make test-sched` runs §2 suites.
- `grep -rn "Test gaps (NO current owner)" todo/` returns only entries unrelated to allocator / race-fence / scratch-buffer needs.
