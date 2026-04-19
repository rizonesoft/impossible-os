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
| 💎  |   2   |  §7     | Test action/cleanup registry (`test_add_action`)          | --                  |  [ ]   |
| 💎  |   3   |  §3     | Scratch-buffer helper (`TEST_SCRATCH_KBUF`)               | §7                  |  [ ]   |
| 💎  |   4   |  §2     | Deterministic race barrier (`test_race_barrier_t`)        | --                  |  [ ]   |
| 💎  |   5   |  §6     | Fault-injection hardening (task-filter, multi-allocator)  | §1                  |  [ ]   |
| 💎  |   6   |  §5     | Test-scoped klog level demotion (`TEST_KLOG_SUPPRESS`)    | §7                  |  [ ]   |
| 💎  |   7   |  §8     | Per-test heap-leak detection                              | §7                  |  [ ]   |
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

`TEST_SCRATCH_KBUF(name, size)` declares a `size`-byte scratch buffer and registers a cleanup handler that frees it on test exit (including early `return` via `TEST_ASSERT` macros). Solves the "I need 64 KiB but stack is 8 KiB" case without leaking on assertion failure. Respects the `kmalloc <= 4 KB` project rule: for `size > 4096`, dispatches to `pmm_alloc_contiguous()` instead so a 64 KiB test buffer does not consume 3% of the 2 MiB kernel heap per test.

- [ ] `TEST_SCRATCH_KBUF(name, size)` expands to: `void *name = test_scratch_alloc(size); TEST_ASSERT_NOT_NULL(name, "scratch alloc " #name);` with the cleanup registered via §7 `test_add_action`.
- [ ] `void *test_scratch_alloc(size_t bytes)` -- when `bytes <= 4096` calls `kmalloc`; when larger calls `pmm_alloc_contiguous((bytes + 4095) / 4096, 0)` and records the page count alongside the pointer for correct free.
- [ ] `void test_scratch_free(void *ctx)` -- the paired free routine registered via `test_add_action`; looks up the page count (or kmalloc-size bit) to route to `kfree` vs `pmm_free_contiguous`.
- [ ] Migration: once §7 ships, `TEST_SCRATCH_KBUF` becomes `void *name = test_scratch_alloc(size); test_add_action(test_scratch_free, name);` -- no bespoke per-test list needed (§7 owns it).
- [ ] Unit test: (a) `TEST_SCRATCH_KBUF(buf, 65536)` + immediate `TEST_ASSERT(0, "forced fail")` leaks zero bytes in both heap stats and PMM free-page count; (b) `TEST_SCRATCH_KBUF(small, 512)` uses kmalloc (verifiable by heap delta != 0 during the test, zero after exit); (c) `TEST_SCRATCH_KBUF(big, 65536)` uses PMM (verifiable by pmm_used_pages delta during, zero after exit).
- [ ] Commit: `"test: scratch-buffer helper (kmalloc <= 4 KiB, pmm_alloc_contiguous larger) registered via test_add_action"`

**Test checkpoint:** A test that allocates 64 KiB via `TEST_SCRATCH_KBUF` and forces-fails leaves zero leaked bytes in the heap AND zero leaked pages in the PMM bitmap after the test runner moves on.

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

- [ ] `TEST_KLOG_SUPPRESS(subsystem)` block-scoped macro: on entry, saves current per-tag level via a new `klog_get_level(const char *subsys)` API and calls `klog_set_level(subsys, LOG_FATAL)` (LOG_FATAL is above any normal emitter, effectively silencing the subsystem). On exit, the saved state is restored via §7 `test_add_action(restore_klog_level, saved)` so `TEST_ASSERT` early-return still restores (preferred over `__attribute__((cleanup))` because action drain is explicit in the test log).
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

## 6. Fault-Injection Hardening (task-filter, total-hits cap, multi-allocator)

§1 ships a per-CPU kmalloc countdown that fires regardless of which thread calls `kmalloc`. Linux `lib/fault-inject.c` additionally offers task-scoped filtering, a total-hits cap, and symmetric injection across other allocator boundaries. This section closes those gaps: adds task-filter + total-hits cap to §1, then extends the same countdown model to PMM, VMM mapping, and copy_to/from_user. Used by tests that prove rollback paths on > 4 KiB allocations, partial-map cleanup, and user-copy failure propagation.

- [ ] Add `task_id_t kmalloc_fail_task_id` to `per_cpu_data` (KERNEL_TESTS-gated). When non-zero, §1 countdown only decrements when `task_current()->id == kmalloc_fail_task_id`. Setter `kmalloc_fail_task_filter_set(task_id_t)` / `_clear()`. Closes the SMP-migration hole where a test migrating between arm and code-under-test loses the armed injection.
- [ ] Add `uint32_t kmalloc_fail_max_injections` to `per_cpu_data` (global counter, not countdown). Once injections triggered == this cap, countdown auto-disarms. Setter `kmalloc_fail_max_injections_set(N)`. Enables stress patterns: "fail 3 out of the next 100 calls."
- [ ] Extend to PMM: add `pmm_alloc_fail_countdown`, `pmm_alloc_fail_task_id`, `pmm_alloc_fail_max_injections` fields in `per_cpu_data`; expose `pmm_alloc_fail_countdown_set(N)` / `pmm_alloc_fail_next()` / `pmm_alloc_fail_injections_triggered()` in `include/kernel/mm/pmm.h`. Hook `pmm_alloc_frame()` + `pmm_alloc_contiguous()` same pattern as §1 (IRQL gate, auto-clear between suites).
- [ ] Extend to VMM mapping: `vmm_map_fail_next()` / `vmm_map_fail_countdown_set(N)` causes `vmm_map()` and `vmm_alloc_range()` to return NULL / STATUS_INSUFFICIENT_RESOURCES without touching page tables, so a test can prove the partial-mapping rollback path (partially mapped pages must be unmapped on a mid-operation failure).
- [ ] Extend to `copy_to_user` / `copy_from_user`: `copy_user_fail_next()` gates the SMAP-mediated wrappers in `src/kernel/arch/x86_64/usercopy.c` so test-time injection returns STATUS_ACCESS_VIOLATION without attempting the real user-mode memory access. Used by syscall tests that prove user-copy failure propagates as a syscall error without corrupting kernel state.
- [ ] `src/kernel/test/test_heap.c` gets task-filter + total-hits-cap regression tests. `src/kernel/test/test_pmm.c` gets 3 PMM-countdown tests (fail_next, countdown(3), IRQL-gate). `src/kernel/test/test_mm.c` gets a VMM-map fail-next test. `src/kernel/test/test_syscall.c` gets a copy_to_user fail-next test.
- [ ] Commit: `"test/mm,syscall: task-filter + total-hits cap; fault inject for pmm/vmm/copy_user"`

**Test checkpoint:** `kmalloc_fail_task_filter_set(my_tid)` + sibling kthread's kmalloc is NOT failed. `kmalloc_fail_max_injections_set(3)` + 10 calls produces exactly 3 forced NULLs. `pmm_alloc_fail_next()` + `pmm_alloc_contiguous(16, 0)` returns NULL + subsequent call succeeds. `copy_user_fail_next()` + syscall that would copy a buffer returns STATUS_ACCESS_VIOLATION.

---

## 7. Test Action/Cleanup Registry (`test_add_action`)

KUnit built `kunit_kzalloc`, `kunit_kmalloc`, and its resource auto-free machinery on top of a general `kunit_add_action(fn, ctx)` primitive that executes cleanup callbacks in LIFO order at test exit -- pass or fail. §3 `TEST_SCRATCH_KBUF` and §5 `TEST_KLOG_SUPPRESS` are two of many possible consumers; making the primitive first-class unlocks cleaner test code across the board and matches Linux's proven model. Without it, every new test-scoped cleanup pattern reinvents list management inside the test runner.

- [ ] Add `int test_add_action(void (*fn)(void *), void *ctx)` to `include/kernel/test/test.h`. Returns 0 on success, -1 (with a `klog(LOG_WARN, "TEST", ...)` line naming the test) if the per-test action list is full.
- [ ] `struct test_state` in `src/kernel/test/test_runner.c` adds `uint8_t action_count` + `struct { void (*fn)(void*); void *ctx; } actions[32]`. `test_runner_run_one` drains actions in **LIFO** order AFTER the test body returns (pass or fail), BEFORE advancing to the next suite.
- [ ] Action drain runs at PASSIVE_LEVEL. If a test left IRQL elevated, the runner logs `[WARN] TEST: <name> left IRQL elevated` and forcibly lowers IRQL before draining. Actions themselves must be non-blocking (documented constraint).
- [ ] Migrate §3 `TEST_SCRATCH_KBUF` onto `test_add_action`: the macro becomes `void *name = test_scratch_alloc(size); test_add_action(test_scratch_free, name);`. Remove the bespoke per-test scratch list; §7 owns it.
- [ ] Migrate §5 `TEST_KLOG_SUPPRESS` onto `test_add_action`: block-entry calls `test_add_action(restore_klog_level, saved_state_ptr)` where `saved_state_ptr` is a `struct { const char *subsys; klog_level_t prev; }` staged into `test_state` scratch. Replaces `__attribute__((cleanup))` with a visible action-log entry.
- [ ] Sanity test in `src/kernel/test/test_runner.c`: a test registering 3 actions sees them invoked in reverse order after the body returns, even when the body hits `TEST_ASSERT(0)` and short-circuits. A test over-registering past 32 actions gets -1 from the 33rd and the existing 32 drain normally.
- [ ] Commit: `"test: action/cleanup registry (test_add_action) + migrate §3 §5 onto it"`

**Test checkpoint:** `[ OK ] TEST: harness :: 3 actions fire in LIFO order for passing test`. `[ OK ] TEST: harness :: 3 actions fire in LIFO order after forced fail`. `[ OK ] TEST: harness :: action list full returns -1 and existing actions still drain`.

---

## 8. Per-Test Heap-Leak Detection

Record `heap_stats().used_bytes` at test start and end; if the delta is non-zero after §7 action drain, fail the test with a message naming the leaked bytes. Every existing `[ OK ]` test becomes a leak-coverage test for free, catching ~90% of real-world leaks without the weight of KASAN or kmemleak. `TEST_EXPECT_LEAK(bytes, reason)` and `TEST_LEAK_IGNORE` opt-outs handle the two legitimate exceptions (intentional long-lived state, non-heap allocator usage).

- [ ] `struct test_state` adds `uint64_t heap_used_at_entry` and `int64_t expected_leak_bytes`. `test_runner_run_one` reads `heap_stats().used_bytes` BEFORE the test body, reads again AFTER §7 actions drain.
- [ ] Non-zero delta AND `expected_leak_bytes == 0`: log `[LEAK] TEST: <name> :: leaked <delta> bytes (entry <pre> -> exit <post>)` and increment `leaked_tests` in test_state. The test still counts as PASS/FAIL by its assertions; LEAK is a third axis reported in the summary.
- [ ] `TEST_EXPECT_LEAK(bytes, reason_literal)` macro sets `expected_leak_bytes = bytes` so an intentionally-leaking test passes the delta check; logs `[LEAK-OK] TEST: <name> :: intentional <bytes> (reason)` to keep the behaviour visible.
- [ ] `TEST_LEAK_IGNORE()` macro sets a sentinel that disables the delta check entirely (for tests exercising PMM or VMM, where `heap_used_bytes` is not the right oracle). Logs `[LEAK-SKIP] TEST: <name>`.
- [ ] Update the end-of-run summary to `=== N passed, F failed, S skipped, P pending, L leaked (X.Xs) ===`. On a green tree with `L > 0`, the test runner exit code stays 0 (leaks are advisory until all tests are retrofitted); the advisory flag flips to blocking once all existing tests clean up to `L == 0`.
- [ ] Unit test in `src/kernel/test/test_runner.c`: (a) a deliberate `kmalloc(64)` without free inside a test body flags `[LEAK] TEST: harness :: leaked >= 64 bytes` (block-header overhead allowed); (b) a test using `TEST_SCRATCH_KBUF` via §7 actions has zero delta; (c) `TEST_EXPECT_LEAK(64, "intentional")` + `kmalloc(64)` without free logs `[LEAK-OK]` and passes.
- [ ] Commit: `"test: per-test heap-leak detection via heap_stats delta (advisory until retrofits complete)"`

**Test checkpoint:** `bash scripts/test.sh` summary line includes `L=<count>`. A deliberately-leaking sanity test produces exactly one `[LEAK]` line with the expected byte count. A sanity test with matched `TEST_EXPECT_LEAK` produces `[LEAK-OK]` and passes.

---

## OS Comparison

| ⭐ | Feature                            | 🪟 Win11                   | 🐧 Linux                                             | 🚀 Impossible OS                               |
| --- | ---------------------------------- | --------------------------- | ---------------------------------------------------- | ---------------------------------------------- |
| 💎 | Slab/kmalloc fault injection       | ⚠️ DV Low-Resources (heavy) | ✅ `failslab` + fail-nth                            | ✅ §1 `kmalloc_fail_countdown`                 |
| 💎 | Multi-allocator fault injection    | ⚠️ DV LRS (coarse)          | ✅ `failslab` + `fail_page_alloc` + `fail_usercopy` | ⬜ §6 pmm/vmm/copy_user countdowns             |
| 💎 | Task-scoped fault injection        | ❌ Rare                     | ✅ `task_filter` (fault-inject)                     | ⬜ §6 `kmalloc_fail_task_filter`               |
| 💎 | Deterministic concurrency testing  | ⚠️ TAEF with effort         | ⚠️ KCSAN (probabilistic)                            | ⬜ §2 `test_race_barrier_t` (deterministic)    |
| 💎 | Test-scoped cleanup registry       | ❌ Manual in TAEF           | ✅ `kunit_add_action`                               | ⬜ §7 `test_add_action`                        |
| 💎 | Test-scoped scratch allocation     | ⚠️ Manual in TAEF           | ✅ `kunit_kzalloc` (kmalloc-only)                   | ⬜ §3 `TEST_SCRATCH_KBUF` (kmalloc + PMM)      |
| 💎 | Per-test leak detection            | ⚠️ DV verifier pool checks  | ✅ `kmemleak` (kernel-wide)                         | ⬜ §8 heap_used delta (per-test, built-in)     |
| ⭐ | Test-scoped klog level demotion    | ❌ None                     | ❌ None                                             | ⬜ §5 `TEST_KLOG_SUPPRESS`                     |
| ⭐ | Single-boot 436-suite runner       | ❌ WDK run per-driver       | ❌ KUnit one-module-at-a-time                       | ✅ existing `test=1` infrastructure            |

After §1-§8 land, in-kernel test coverage reaches Linux-KUnit-plus-fault-inject parity for allocator-failure, cleanup, and concurrency testing; §5 (klog demotion) and §8 (per-test leak delta, no KASAN required) give Impossible OS two real edges neither Win11 nor Linux offers at the in-kernel-test layer.

---

## Unit Tests

> Boot tests run with `test=1` in `boot.conf`.

- [x] `src/kernel/test/test_heap.c` covers §1 `kmalloc_fail_next` + `kmalloc_fail_countdown_set(N)` + IRQL gate (5 suites; shipped with §1)
- [ ] `src/kernel/test/test_sched.c` covers §2 `test_race_barrier_t` release-a-first and release-b-first orderings (100-iteration drift check)
- [ ] `src/kernel/test/test_runner.c` sanity tests for §3 `TEST_SCRATCH_KBUF` (kmalloc path, PMM path, cleanup-on-failure path)
- [ ] `src/kernel/test/test_boot_payload.c` + `test_sched.c` regression coverage for §5 `TEST_KLOG_SUPPRESS` (error-path klog lines disappear from test-window serial output)
- [ ] `src/kernel/test/test_heap.c` + `test_pmm.c` + `test_mm.c` + `test_syscall.c` cover §6 multi-allocator fault injection: task-filter, total-hits cap, pmm/vmm/copy_user countdowns (min 1 fail-next + 1 IRQL-gate per allocator)
- [ ] `src/kernel/test/test_runner.c` sanity tests for §7 `test_add_action` (LIFO drain on pass, LIFO drain on fail, over-registration returns -1 without corrupting list)
- [ ] `src/kernel/test/test_runner.c` sanity tests for §8 heap-leak detection (unfreed kmalloc flagged, `TEST_EXPECT_LEAK` tolerated, `TEST_LEAK_IGNORE` bypasses delta check)

> **Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot) + `scripts\debug\run-mm-tests.bat` (SUITE=mm) | ~20 new test-harness + allocator suites, 0 failures, summary shows `L=0` on green tree

---

## Verification

- `make test-boot` runs §2, §3, §5, §7, §8 sanity suites (test-harness itself).
- `make test-mm` runs §1 + §6 allocator-fault-injection suites.
- `make test-sched` runs §2 race-barrier and §6 task-filter siblings-isolation suites.
- End-of-run `=== N passed, F failed, S skipped, P pending, L leaked (X.Xs) ===` summary line present; `L=0` on a green tree after §4 retrofit completes.
- `grep -rn "Test gaps (NO current owner)" todo/` returns only entries unrelated to allocator / race-fence / scratch-buffer / klog-suppression / leak-detection needs.
