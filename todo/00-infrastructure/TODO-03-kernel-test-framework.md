# TODO-03 — Kernel Test Framework & Automation

> **Goal:** A complete, automated kernel test system that catches regressions before they ship. Tests run automatically on every commit (GitHub Actions), on every local build (optional), and on-demand via `bash scripts/test.sh`. The test framework covers unit tests (PMM, heap, OB, VFS, scheduler), integration tests (boot-to-desktop), and driver tests (USB, NVMe, filesystem). Results are visible in serial output, CI job summaries, and developer notifications. No manual "boot and check serial" — the system tells you pass/fail.

> [!IMPORTANT]
> **Current state:** A kernel unit test framework exists (`src/kernel/test/`, 5 suites, 12 test functions) but is **never compiled** (`-DKERNEL_TESTS` not in CFLAGS). Boot tests exist (`boot_tests.c`, 15+ tests) but only run with `debug=1`. Smoke test script exists (`test-smoke.sh`) but is **commented out in CI**. Filesystem test script exists (`test-fs.sh`) but not in CI. No pre-commit hooks, no test failure notifications, no test coverage tracking.

---

## Inputs

- `src/kernel/test/test_runner.c` — test framework (TEST_ASSERT, suite registration, runner)
- `include/kernel/test/test.h` — test API header
- `src/kernel/test/test_*.c` — 5 existing test suites (PMM, heap, VFS, sched, registry)
- `src/kernel/main/boot_tests.c` — boot-time integration tests (VFS, threading, IPC, sync)
- `scripts/test-smoke.sh` — QEMU headless boot verification (written, not enabled in CI)
- `scripts/test-fs.sh` — filesystem driver test suite (written, not in CI)
- `.github/workflows/build.yml` — CI pipeline (smoke test commented out)
- `Makefile` — build targets (no `make test` target)
- → XREF: `TODO-02-developer-tooling-stack.md §5` — GitHub sync and CI parity

---

## Outcome

- `make test` builds with `-DKERNEL_TESTS`, boots QEMU headless, runs all kernel unit tests, parses serial for pass/fail, exits with code 0 or 1.
- `bash scripts/test.sh` runs the full test suite: build → smoke test → unit tests → filesystem tests.
- GitHub Actions runs tests on every push to `main` and every PR — blocks merge on failure.
- Pre-push git hook optionally runs smoke test locally before push.
- New test suites for Object Manager, security, NVMe, USB are easy to add (one function + one register call).
- Test results are visible in: serial log, CI job summary, and GitHub PR status checks.

---

## Implementation Order

| ⭐  | Order | Deliverable                                           | Depends On | Status |
| --- | :---: | ----------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Wire test_runner into boot path                        | —          |  [ ]   |
| 💎  |   2   | `make test` target with QEMU headless + serial parse   | §1         |  [ ]   |
| 💎  |   3   | Add OB, security, and NVMe test suites                 | §1         |  [ ]   |
| 💎  |   4   | Enable smoke test in GitHub Actions                    | §2         |  [ ]   |
| 💎  |   5   | Enable filesystem tests in GitHub Actions              | §4         |  [ ]   |
| 💎  |   6   | Enable unit tests in GitHub Actions                    | §2, §4     |  [ ]   |
| ⭐  |   7   | Test result summary in CI job output                   | §4–§6      |  [ ]   |
| ⭐  |   8   | Pre-push git hook for local smoke test                 | §2         |  [ ]   |
| ⭐  |   9   | Test coverage tracking and dashboard                   | §6         |  [ ]   |

> 💎 = parity — Linux and Windows kernel trees both have automated test pipelines.
> ⭐ = exclusive — test result summary in CI, pre-push hooks, coverage dashboard.

---

## 1. Wire test_runner into Boot Path

Connect the existing test framework to the boot sequence so tests actually run.

- [ ] In `boot_tests.c`: add `#include "kernel/test/test.h"` and call `test_runner_init()` + `test_runner_run()` at the start of `boot_tests_run()` (before the ad-hoc tests)
- [ ] This means tests run when `debug=1` in boot.conf — same gate as existing boot tests
- [ ] Add a separate gate: `test=1` in boot.conf → run ONLY unit tests (not boot tests), then halt with exit code
- [ ] When `test=1`: after `test_runner_run()`, write pass/fail summary to serial, then `acpi_shutdown()` (clean QEMU exit)
- [ ] Makefile: add `-DKERNEL_TESTS` to CFLAGS when building test target
- [ ] Commit: `"test: wire kernel test_runner into boot path — runs with debug=1 or test=1"`

**Test checkpoint:** `make run` with `debug=1` in boot.conf → serial shows `=== Running 5 test suite(s) ===` followed by pass/fail for each test.

---

## 2. `make test` Target

A single command that builds, boots QEMU headless, runs tests, and reports pass/fail.

- [ ] Add `make test` Makefile target:
  1. Build with `CFLAGS += -DKERNEL_TESTS`
  2. Set `test=1` in boot.conf (temp copy)
  3. Launch QEMU with `-display none -serial file:build/test.log -no-reboot`
  4. Timeout: 60 seconds (kill QEMU if not exited)
  5. Parse `build/test.log` for `=== X tests passed, Y FAILED ===`
  6. Exit 0 if Y=0, exit 1 if Y>0 or timeout
- [ ] Add `bash scripts/test.sh` wrapper that does the same with colored output
- [ ] Support selective suites: `make test SUITE=ob` runs only OB tests
- [ ] Commit: `"test: make test target — headless QEMU, serial parse, exit code"`

**Test checkpoint:** `make test` from clean checkout → builds, boots QEMU headless, runs tests, prints `PASS: 12 tests passed` or `FAIL: 2 of 14 failed`, exits with correct code.

---

## 3. Add OB, Security, and NVMe Test Suites

Expand test coverage to the new Object Manager and security subsystems.

- [ ] `src/kernel/test/test_ob.c` — test suite covering:
  - `ob_alloc_object` + `OB_HEADER_FROM_BODY` round-trip
  - `ObReferenceObject` + `ObDereferenceObject` → refcount reaches 0, on_delete called
  - Handle table: alloc + lookup + free cycle
  - Named object: insert into `\BaseNamedObjects`, find via `ObLookupObjectByName`
  - `NtDuplicateObject`: duplicate handle, close source, duplicate still valid
  - `ob_handle_table_inherit`: parent OBJ_INHERIT handle appears in child at same index
  - `NtQueryDirectoryObject`: enumerate `\` returns Device, KernelObjects, BaseNamedObjects
- [ ] `src/kernel/test/test_security.c` — test suite covering:
  - SID comparison: `RtlEqualSid(SeLocalSystemSid, SeLocalSystemSid)` → true
  - SID formatting: `RtlConvertSidToString(SeLocalSystemSid)` → `"S-1-5-18"`
  - ACL creation: `RtlCreateAcl` + `RtlAddAccessAllowedAce` + `RtlGetAce` round-trip
  - Token creation: `SeCreateSystemToken()` → non-NULL, 24 privileges, IL=System
  - Privilege lookup: `RtlPrivilegeLuidToName(&SeShutdownPrivilege)` → `"SeShutdownPrivilege"`
- [ ] Register both in `test_runner_init()`: `test_register_ob()`, `test_register_security()`
- [ ] Commit: `"test: add OB and security test suites — 15+ new test assertions"`

**Test checkpoint:** `make test` → OB and security suites appear in output with all assertions passing.

---

## 4. Enable Smoke Test in GitHub Actions

Uncomment and harden the smoke test in CI.

- [ ] In `.github/workflows/build.yml`: uncomment the smoke test step
- [ ] Add timeout: `timeout-minutes: 2` on the smoke test step
- [ ] Upload `build/smoke-test.log` as artifact on failure
- [ ] Add step summary: `echo "### Smoke Test: PASS ✅" >> $GITHUB_STEP_SUMMARY` or FAIL
- [ ] If smoke test fails: mark the job as failed (blocks PR merge)
- [ ] Commit: `"ci: enable smoke test in GitHub Actions — blocks merge on boot failure"`

**Test checkpoint:** Push to `main` → GitHub Actions shows smoke test step → green check if boot succeeds, red X if boot fails.

---

## 5. Enable Filesystem Tests in GitHub Actions

Add filesystem driver testing to CI after smoke test passes.

- [ ] New workflow step after smoke test: `bash scripts/test-fs.sh`
- [ ] Generate test disk images: `bash tools/make-test-disks.sh`
- [ ] Run filesystem tests with timeout: `timeout-minutes: 5`
- [ ] Upload per-filesystem logs as artifacts
- [ ] Mark step as failed if any filesystem test fails
- [ ] Commit: `"ci: enable filesystem tests in GitHub Actions — FAT32, IXFS, ext4"`

**Test checkpoint:** CI runs filesystem tests after smoke test. Each FS shows pass/fail in job log.

---

## 6. Enable Unit Tests in GitHub Actions

Add kernel unit test run to CI.

- [ ] New workflow step: build with `-DKERNEL_TESTS`, boot with `test=1`, parse results
- [ ] Reuse the `make test` target from §2
- [ ] Upload `build/test.log` as artifact
- [ ] Parse test summary line for pass/fail count
- [ ] Add to step summary: `### Unit Tests: 47 passed, 0 failed ✅`
- [ ] Commit: `"ci: enable kernel unit tests in GitHub Actions — automated pass/fail"`

**Test checkpoint:** CI shows unit test results in job summary. Any test failure blocks merge.

---

## 7. Test Result Summary in CI Job Output

Make test results visible at a glance in the GitHub PR.

- [ ] Use `$GITHUB_STEP_SUMMARY` to write markdown tables:
  ```
  ### Test Results
  | Suite | Tests | Passed | Failed |
  |-------|-------|--------|--------|
  | PMM   | 2     | 2      | 0      |
  | OB    | 7     | 7      | 0      |
  | ...   | ...   | ...    | ...    |
  ```
- [ ] Parse serial log to extract per-suite results
- [ ] Script: `scripts/parse-test-results.sh` takes test.log, outputs markdown
- [ ] Commit: `"ci: test result summary table in GitHub PR — per-suite pass/fail"`

**Test checkpoint:** Open a PR → see test result table in the PR checks summary.

---

## 8. Pre-Push Git Hook

Optional local smoke test before pushing.

- [ ] Create `scripts/install-hooks.sh` that symlinks hooks into `.git/hooks/`
- [ ] `pre-push` hook: runs `make test` (headless QEMU, 60s timeout)
- [ ] If tests fail: block push with message `"Tests failed — run 'make test' to see details"`
- [ ] Make it optional: `bash scripts/install-hooks.sh` to enable, `bash scripts/install-hooks.sh --remove` to disable
- [ ] Document in CONTRIBUTING.md
- [ ] Commit: `"infra: pre-push git hook — optional local smoke test before push"`

**Test checkpoint:** Install hook → make a breaking change → `git push` blocked with test failure message.

---

## 9. Test Coverage Tracking

Track which kernel subsystems have test coverage.

- [ ] `scripts/test-coverage.sh` — scans `src/kernel/test/test_*.c` for TEST_ASSERT calls
- [ ] Outputs: suite name, assertion count, functions tested
- [ ] Generates `build/test-coverage.md` with coverage table
- [ ] Upload as CI artifact
- [ ] Track coverage over time in a simple JSON file
- [ ] Commit: `"infra: test coverage tracking — assertion count per subsystem"`

**Test checkpoint:** `bash scripts/test-coverage.sh` outputs table showing suites and assertion counts.

---

## OS Comparison

| ⭐ | Feature                    | Win11                     | Linux                     | Impossible OS              |
|----|----------------------------|---------------------------|---------------------------|----------------------------|
| 💎 | Kernel unit test framework | ✅ KUnit + WHQL           | ✅ KUnit + kselftest      | ⚠️ Framework exists, unwired |
| 💎 | CI build verification      | ✅ Internal CI            | ✅ kernel.org CI          | ✅ GitHub Actions build    |
| 💎 | CI boot test               | ✅ Internal CI            | ✅ LKFT + kernelci        | ⬜ §4 (smoke test ready)   |
| 💎 | CI driver tests            | ✅ HLK/WHQL              | ✅ LTP + blktests         | ⬜ §5–§6                   |
| ⭐ | PR test summary table      | ❌ Internal only          | ⚠️ Bot comments           | ⬜ §7 🚀                   |
| ⭐ | Pre-push local tests       | ❌ Not standard           | ⚠️ Optional               | ⬜ §8 🚀                   |

After §1–§6, Impossible OS has automated testing on par with Linux kernel CI (build + boot + unit + driver tests on every commit). §7–§9 add developer-facing features neither Windows nor Linux provides at the PR level.

---

## Verification

- [ ] `make test` from clean checkout → pass/fail exit code works.
- [ ] GitHub Actions → smoke + unit + FS tests all run, block on failure.
- [ ] New test suite (test_ob.c) catches a deliberate bug (e.g., wrong refcount) → red CI.
- [ ] Pre-push hook blocks push when tests fail.
- [ ] Commit: `"infra: kernel test framework complete — automated CI + local testing"`
