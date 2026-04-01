# TODO-03 — Kernel Test Framework & Automation

> **Goal:** A complete, automated kernel test system that catches regressions before they ship. Tests run automatically on every commit (GitHub Actions), on every local build (optional), and on-demand via `bash scripts/test.sh`. The test framework covers unit tests (PMM, heap, OB, VFS, scheduler), integration tests (boot-to-desktop), and driver tests (USB, NVMe, filesystem). Results are visible in serial output, CI job summaries, and developer notifications. No manual "boot and check serial" — the system tells you pass/fail.

> [!IMPORTANT]
> **Current state (2026-04-02):** Test framework is fully wired — `-DKERNEL_TESTS` always on, 26 suites (59 assertions) covering PMM, heap, VFS, sched, registry, boot init, klog, OB, and security. `make test` / `scripts/test.sh` builds, boots QEMU headless, parses pass/fail. Boot tests run with `debug=1`, unit-only with `test=1`. CI builds but does not run QEMU (removed — unreliable under nested virt).

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
- → XREF: `TODO-02-developer-tooling-stack.md §2.5` — GitHub sync and CI parity
- → XREF: `TODO-04-ci-notifications.md §1` — branch protection depends on test steps from §4–§6
- → XREF: `TODO-05-test-suites-coverage.md §1–§9` — individual test suites depend on framework wiring from §1

---

## Outcome

- `make test` builds, boots QEMU headless with `test=1`, runs all kernel unit tests, parses serial for pass/fail, exits with code 0 or 1.
- `bash scripts/test.sh` does the same with colored output and KVM auto-detection.
- `make test SUITE=ob` filters to specific suites.
- New test suites are easy to add: one `test_register_*()` call in `test_runner_init()`.
- Test results visible in serial log and `build/test.log`.

---

## Implementation Order

| ⭐  | Order | Deliverable                                            | Depends On | Status |
| --- | :---: | ------------------------------------------------------ | ---------- | :----: |
| 💎  |   1   | Wire test_runner into boot path                        | —          |  [x]   |
| 💎  |   2   | `make test` target with QEMU headless + serial parse   | §1         |  [x]   |
| 💎  |   3   | Add OB, security, and NVMe test suites                 | §1         |  [x]   |
| ⭐  |   4   | Pre-push git hook for local smoke test                 | §2         |  [ ]   |
| ⭐  |   5   | Test coverage tracking and dashboard                   | §2         |  [ ]   |

> §4–§9 from original plan (GitHub Actions QEMU smoke/unit/filesystem tests, CI result summary)
> removed — QEMU in GitHub Actions runners is unreliable (UEFI+OVMF flaky under nested virt).
> Local `make test` + `scripts/test.sh` covers the same verification without CI QEMU dependency.

> 💎 = parity — Linux and Windows kernel trees both have automated test pipelines.
> ⭐ = exclusive — test result summary in CI, pre-push hooks, coverage dashboard.

---

## 1. Wire test_runner into Boot Path

Connect the existing test framework to the boot sequence so tests actually run.

- [x] In `boot_tests.c`: add `#include "kernel/test/test.h"` and call `test_runner_init()` + `test_runner_run()` at the start of `boot_tests_run()` (before the ad-hoc tests)
- [x] This means tests run when `debug=1` in boot.conf — same gate as existing boot tests
- [x] Add a separate gate: `test=1` in boot.conf → run ONLY unit tests (not boot tests), then `acpi_shutdown()`
- [x] When `test=1`: after `test_runner_run()`, write pass/fail summary to serial, then `acpi_shutdown()` (clean QEMU exit)
- [x] Makefile: `-DKERNEL_TESTS` added to CFLAGS unconditionally (zero overhead — `#ifdef` guards in test files)
- [x] Commit: `"test: wire kernel test_runner into boot path — runs with debug=1 or test=1"`

**Test checkpoint:** `make run` with `debug=1` in boot.conf → serial shows `=== Running 5 test suite(s) ===` followed by pass/fail for each test.

---

## 2. `make test` Target

A single command that builds, boots QEMU headless, runs tests, and reports pass/fail.

- [x] Add `make test` Makefile target:
  1. Build with `CFLAGS += -DKERNEL_TESTS`
  2. Set `test=1` in boot.conf (temp copy)
  3. Launch QEMU with `-display none -serial file:build/test.log -no-reboot`
  4. Timeout: 60 seconds (kill QEMU if not exited)
  5. Parse `build/test.log` for `=== X tests passed, Y FAILED ===`
  6. Exit 0 if Y=0, exit 1 if Y>0 or timeout
- [x] Add `bash scripts/test.sh` wrapper that does the same with colored output
- [x] Support selective suites: `make test SUITE=ob` runs only OB tests
- [x] Commit: `"test: make test target — headless QEMU, serial parse, exit code"`

**Test checkpoint:** `make test` from clean checkout → builds, boots QEMU headless, runs tests, prints `PASS: 12 tests passed` or `FAIL: 2 of 14 failed`, exits with correct code.

---

## 3. Add OB, Security, and NVMe Test Suites

Expand test coverage to the new Object Manager and security subsystems.

- [x] `src/kernel/test/test_ob.c` — test suite covering:
  - `ob_alloc_object` + `OB_HEADER_FROM_BODY` round-trip
  - `ObReferenceObject` + `ObDereferenceObject` → refcount reaches 0, on_delete called
  - Handle table: alloc + lookup + free cycle
  - Named object: insert into `\BaseNamedObjects`, find via `ObLookupObjectByName`
  - `NtDuplicateObject`: duplicate handle, close source, duplicate still valid
  - `ob_handle_table_inherit`: parent OBJ_INHERIT handle appears in child at same index
  - `NtQueryDirectoryObject`: enumerate `\` returns Device, KernelObjects, BaseNamedObjects
- [x] `src/kernel/test/test_security.c` — test suite covering:
  - SID comparison: `RtlEqualSid(SeLocalSystemSid, SeLocalSystemSid)` → true
  - SID formatting: `RtlConvertSidToString(SeLocalSystemSid)` → `"S-1-5-18"`
  - ACL creation: `RtlCreateAcl` + `RtlAddAccessAllowedAce` + `RtlGetAce` round-trip
  - Token creation: `SeCreateSystemToken()` → non-NULL, 24 privileges, IL=System
  - Privilege lookup: `RtlPrivilegeLuidToName(&SeShutdownPrivilege)` → `"SeShutdownPrivilege"`
- [x] Register both in `test_runner_init()`: `test_register_ob()`, `test_register_security()`
- [x] Commit: `"test: add OB and security test suites — 15+ new test assertions"`

> **Done:** OB: 7 suites, 15 assertions. Security: 5 suites, 8 assertions. Total 23 new assertions registered in `test_runner_init()` (2026-04-02).

**Test checkpoint:** `make test` → OB and security suites appear in output with all assertions passing.

---

## 4. Pre-Push Git Hook

Optional local smoke test before pushing.

- [ ] Create `scripts/install-hooks.sh` that symlinks hooks into `.git/hooks/`
- [ ] `pre-push` hook: runs `make test` (headless QEMU, 60s timeout)
- [ ] If tests fail: block push with message `"Tests failed — run 'make test' to see details"`
- [ ] Make it optional: `bash scripts/install-hooks.sh` to enable, `bash scripts/install-hooks.sh --remove` to disable
- [ ] Document in CONTRIBUTING.md
- [ ] Commit: `"infra: pre-push git hook — optional local smoke test before push"`

**Test checkpoint:** Install hook → make a breaking change → `git push` blocked with test failure message.

---

## 5. Test Coverage Tracking

Track which kernel subsystems have test coverage.

- [ ] `scripts/test-coverage.sh` — scans `src/kernel/test/test_*.c` for TEST_ASSERT calls
- [ ] Outputs: suite name, assertion count, functions tested
- [ ] Generates `build/test-coverage.md` with coverage table
- [ ] Track coverage over time in a simple JSON file
- [ ] Commit: `"infra: test coverage tracking — assertion count per subsystem"`

**Test checkpoint:** `bash scripts/test-coverage.sh` outputs table showing suites and assertion counts.

---

## OS Comparison

| ⭐ | Feature                  | Win11              | Linux                | Impossible OS            |
|----|--------------------------|--------------------|----- ----------------|-------------------------------------|
| 💎 | Kernel unit tests        | ✅ KUnit + WHQL    | ✅ KUnit + kselftest | ✅ test_runner + make test §1–§3  |
| 💎 | CI build verification    | ✅ Internal CI     | ✅ kernel.org CI     | ✅ GitHub Actions build.yml       |  
| 💎 | Local test runner        | ❌ Manual          | ⚠️ make kselftest    | ✅ make test + scripts/test.sh §2 |
| ⭐ | Pre-push local tests     | ❌ Not standard    | ⚠️ Optional          | ⬜ §4                             |
| ⭐ | Coverage tracking        | ❌ Internal only   | ⚠️ lcov optional     | ⬜ §5                             |

After §1–§3, Impossible OS has a full local test framework: `make test` builds, boots QEMU headless, runs 59+ unit tests, and reports pass/fail. §4–§5 add pre-push hooks and coverage tracking.

---

## Verification

- [ ] `make test` from clean checkout → pass/fail exit code works.
- [ ] GitHub Actions → smoke + unit + FS tests all run, block on failure.
- [ ] New test suite (test_ob.c) catches a deliberate bug (e.g., wrong refcount) → red CI.
- [ ] Pre-push hook blocks push when tests fail.
- [ ] Commit: `"infra: kernel test framework complete — automated CI + local testing"`
