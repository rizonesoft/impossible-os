# TODO-03 -- Kernel Test Harness

> **Goal:** Kernel-internal test-time infrastructure that subsystem unit tests (under `src/kernel/test/test_*.c`) can opt into when they need to exercise rare control flow: allocator failure rollback, deterministic race windows between cooperating threads, and bulk scratch buffers that exceed the 8 KiB kernel stack. Each existing subsystem tests the hot path; the hooks in this TODO close the remaining corners without having to stand up a whole kernel debugger.

> [!IMPORTANT]
> **Current state:** `src/kernel/test/` has 436+ unit-test suites wired into the boot-time test runner (`test=1` in `boot.conf`). Every suite must currently test the normal path: there is no way for a test to ask `kmalloc` to fail on its 3rd call, no way to make two cooperating kthreads land on opposite sides of a lock acquisition at a chosen instant, and the 8 KiB kernel task stack caps any test-local buffer at about 4 KiB before overflowing (guard page trip). Concrete gaps that motivated this TODO are collected from TODO-12 §4 "Test gaps (NO current owner)".

---

## Inputs

- `src/kernel/mm/heap.c` -- `kmalloc`/`kfree` -- §1 fault-injection hooks install here
- `src/kernel/test/test.h` -- test macros (`TEST_ASSERT`, `TEST_SKIP`, `TEST_PENDING`) -- §1/§2 add helpers
- `src/kernel/sched/task.c` -- `kthread_create`, `thread_yield`, `thread_join` -- §2 race-fence uses these
- `src/kernel/sched/spinlock.h` -- spinlock primitives -- §2 adds a test-only checkpoint primitive
- `include/kernel/test/test.h` -- public test API (where the new helpers go)
- `CLAUDE.md` "Test Code -- No Live Boot Infrastructure Calls" -- the hooks below must obey the same safety contract (no live boot-path mutation from tests)
- → XREF: `00-infrastructure/TODO-01-usermode-test-framework.md` -- user-mode test launcher (complementary; runs user-mode binaries, this TODO is in-kernel)

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
| 💎  |   1   | kmalloc fault injection (`kmalloc_fail_countdown`)  | --            |  [ ]   |
| 💎  |   2   | Deterministic race barrier (`test_race_barrier_t`)  | --            |  [ ]   |
| 💎  |   3   | Scratch-buffer helper (`TEST_SCRATCH_KBUF`)         | --            |  [ ]   |
| 💎  |   4   | Retrofit existing "Test gaps" stamps across `todo/` | §1, §2, §3    |  [ ]   |

> 💎 = parity work -- Linux kernel self-test framework has `fault-injection` (`lib/fault-inject.c`), `kcsan`/`kasan` fences, and KUnit has `kunit_kzalloc()` scratch helpers. This TODO brings the same floor to the Impossible OS kernel test runner.

---

## 1. kmalloc Fault Injection

A per-CPU test-only countdown that causes `kmalloc` to return `NULL` on the next (or Nth subsequent) call WITHOUT hitting the real allocator. Tests set the countdown, invoke the code under test, assert the failure-cleanup path executed, then reset the countdown.

- [ ] Add `uint32_t kmalloc_fail_countdown` -- per-CPU (`per_cpu_data_t`) to avoid cross-test interference on SMP; decrements on every `kmalloc` call; when it reaches 1, the call returns NULL and the counter resets to 0.
- [ ] Expose `void kmalloc_fail_countdown_set(uint32_t n)` and `void kmalloc_fail_countdown_clear(void)` in `include/kernel/mm/heap.h` behind `#ifdef KERNEL_TESTS` so release builds see no overhead.
- [ ] `kmalloc` checks the per-CPU counter FIRST before touching the heap. On fault-injection trigger: return NULL, increment `s_fault_injections_triggered` stat, do NOT log (tests expect silent failure).
- [ ] Convenience: `kmalloc_fail_next(void)` == `kmalloc_fail_countdown_set(1)`. Symmetric `kmalloc_fail_clear_on_exit` helper that tests call at the start of each test body so a leftover counter from a failing test never taints the next one.
- [ ] Unit test in `src/kernel/test/test_heap.c`: after `kmalloc_fail_next()`, the next `kmalloc(16)` returns NULL; the one after returns a valid pointer; `s_fault_injections_triggered` increments.
- [ ] Commit: `"test/heap: kmalloc fault-injection countdown for failure-path coverage"`

**Test checkpoint:** `kmalloc_fail_next()` + `kmalloc(16) == NULL` + subsequent `kmalloc(16) != NULL`. Verifies the per-CPU counter decrements correctly and auto-clears.

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

- [ ] TODO-12 §4 "Test gaps": rewrite the three pending items against the new primitives -- (a) allocator kmalloc-failure rollback uses `kmalloc_fail_next()`; (b) ReplyBodyCap clamp with `recv_buf_len > 65528` uses `TEST_SCRATCH_KBUF(rxbuf, 65536 + 64)`; (c) address-ordered two-port locking concurrency stress uses `test_race_barrier_t` between the two sender threads. Close the Accepted stamp per the inbound-XREF sweep in `implement-todo-section` step 18.
- [ ] Repo sweep: `grep -rn "Test gaps (NO current owner)" todo/` -- for every hit, decide whether the gap is closable by one of §1-§3 and convert it. If the gap is unrelated infrastructure, leave it.
- [ ] Commit: `"test: retrofit deferred ALPC §4 test gaps against new kernel test harness"` (and similar per-TODO commits as the sweep runs)

**Test checkpoint:** `grep -rn "Test gaps (NO current owner)" todo/` shrinks to zero hits for gaps that §1-§3 can close; any remaining gaps have a clearly-unrelated reason documented.

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
