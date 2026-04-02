# TODO-05 — Kernel Test Suites & Coverage Expansion

> **Goal:** Every kernel subsystem has test coverage. When a new feature is implemented, a test suite is created alongside it. Test coverage is tracked and visible. The test system is easy to extend — adding a new test suite is one file + one register call. Target: 100+ test assertions covering every subsystem that has shipped code.

> [!IMPORTANT]
> **Current state (2026-04-02):** 50+ test suites across 9 categories, 100+ assertions. Suites: PMM (2), heap (4), VMM (1), swap (1), mmap (1), VFS (3), sched (1), registry (2), boot init (8), klog (6), OB (7), security (5), PEB/TEB (5), IPC (5: threads, mutex, semaphore, pipe, shmem), storage (2: AHCI, VirtIO). Categories filterable via `make test-mm`, `make test-ob`, etc. Boot tests migrated from ad-hoc klog to `TEST_ASSERT`. Still needed: timer, ELF, NVMe, USB, coverage report.

> [!NOTE]
> **Resolved overlap:** 00-infrastructure/TODO-03-kernel-test-framework.md (was TODO-03 §3) created initial `test_ob.c` (7 suites, 15 assertions) and `test_security.c` (5 suites, 8 assertions) on 2026-04-02. The expanded assertions listed in §2–§3 below are additions on top of those.

---

## Inputs

- `src/kernel/test/test_runner.c` — test framework
- `src/kernel/test/test_*.c` — existing 5 suites
- `src/kernel/main/boot_tests.c` — 15+ integration tests (not in unit framework)
- → XREF: `00-infrastructure/TODO-03-kernel-test-framework.md` — full framework wiring; this TODO provides minimal local prerequisite in §1

---

## Outcome

- 15+ test suites covering every major kernel subsystem.
- 100+ individual test assertions.
- Boot tests migrated into the unit test framework (same pass/fail tracking).
- Adding a new test: create `test_foo.c`, implement `test_register_foo()`, add one line to `test_runner_init()`.
- Coverage report shows which subsystems have tests and how many assertions.

---

## Implementation Order

| ⭐  | Order | Deliverable                                  | Depends On | Status |
| --- | :---: | -------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Minimal test_runner wiring                    | —          |  [x]   |
| 💎  |   2   | Object Manager test suite (test_ob.c)         | §1         |  [x]   |
| 💎  |   3   | Security test suite (test_security.c)         | §1         |  [x]   |
| 💎  |   4   | Timer test suite                              | §1         |  [ ]   |
| 💎  |   5   | IPC test suite (threads, mutex, sem, pipe, shmem) | §1     |  [x]   |
| 💎  |   6   | VMM, swap, mmap test suites                   | §1         |  [x]   |
| 💎  |   7   | ELF loader test suite                         | §1         |  [ ]   |
| 💎  |   8   | Migrate boot_tests.c into unit framework      | §1         |  [x]   |
| 💎  |   9   | NVMe driver test suite                        | §1         |  [ ]   |
| 💎  |  10   | USB MSC driver test suite                     | §1         |  [ ]   |
| ⭐  |  11   | Test coverage report generator                | §2–§10     |  [ ]   |

> 💎 = parity — Linux KUnit and Windows HLK both have per-subsystem test suites.
> ⭐ = exclusive — automated coverage report showing assertion count per subsystem.

---

## 1. Minimal test_runner Wiring

> [!NOTE]
> Minimal prerequisite — full implementation in 00-infrastructure/TODO-03-kernel-test-framework.md (was TODO-03 §1). This section adds just enough to compile and run test suites during boot. 00-infrastructure/TODO-03-kernel-test-framework.md (was TODO-03 §1) adds `make test`, headless QEMU, serial parsing, and CI integration.

- [x] Add `-DKERNEL_TESTS` to CFLAGS in Makefile (unconditional)
- [x] In `boot_tests.c`: call `test_runner_init()` + `test_runner_run()` when `debug=1` or `test=1`
- [x] Serial shows `=== Running 26 test suite(s) ===` (verified WHPX 2026-04-02)
- [x] Commit: done via 00-infrastructure/TODO-03-kernel-test-framework.md (was TODO-03 §1)

**Test checkpoint:** `bash scripts/build.sh run` with `debug=1` in boot.conf → serial shows `=== Running 5 test suite(s) ===` with PMM, heap, VFS, sched, registry results.

---

## 2. Object Manager Test Suite

> [!NOTE]
> Initial `test_ob.c` created by 00-infrastructure/TODO-03-kernel-test-framework.md (was TODO-03 §3) (7 suites, 15 assertions, 2026-04-02). Remaining assertions below are expansions.

- [x] `src/kernel/test/test_ob.c`:
  - `ob_alloc_object(ObpFileType)` → non-NULL body, header recoverable via `OB_HEADER_FROM_BODY` macro
  - `ObReferenceObject` × 5 + `ObDereferenceObject` × 6 → on_delete called, object freed
  - Handle alloc → lookup returns same object → free → lookup returns NULL
  - 100 handle alloc/free round trips → count returns to 0
  - Named event in `\BaseNamedObjects\TestOb` → `ObLookupObjectByName` finds it
  - `NtDuplicateObject` → close source → duplicate still valid
  - `ob_handle_table_inherit` → child has parent's OBJ_INHERIT handle at same index
  - `NtQueryDirectoryObject(\)` → returns entries including `Device`, `KernelObjects`
  - `NtQueryObject(ObjectBasicInformation)` → ref_count > 0
  - `NtClose` → handle no longer valid
- [ ] Register: `test_register_ob()` → `test_suite_register("OB", test_ob_all)`
- [ ] Commit: `"test: add Object Manager test suite — 10 assertions"`

**Test checkpoint:** `make test` → serial shows `OB: 10 tests, 10 passed, 0 failed`.

---

## 3. Security Test Suite

> [!NOTE]
> Initial `test_security.c` created by 00-infrastructure/TODO-03-kernel-test-framework.md (was TODO-03 §3) (5 suites, 8 assertions, 2026-04-02). Remaining assertions below are expansions.

- [x] `src/kernel/test/test_security.c`:
  - `RtlEqualSid(SeLocalSystemSid, SeLocalSystemSid)` → true
  - `RtlEqualSid(SeLocalSystemSid, SeWorldSid)` → false
  - `RtlLengthSid(SeLocalSystemSid)` → 12 (8 + 4×1)
  - `RtlConvertSidToString(SeLocalSystemSid)` → `"S-1-5-18"`
  - `RtlCreateAcl` + `RtlAddAccessAllowedAce` + `RtlGetAce` → ACE matches
  - `SeCreateSystemToken()` → non-NULL, 24 privileges, IL=System
  - `SeCreateUserToken(SeLocalSystemSid, 0)` → Medium IL, 3 privileges
  - `SeCreateUserToken(SeLocalSystemSid, 1)` → High IL, 15 privileges
  - `NtAllocateLocallyUniqueId()` × 2 → different LUIDs
  - `RtlPrivilegeLuidToName(&SeShutdownPrivilege)` → `"SeShutdownPrivilege"`
  - `NtAdjustPrivilegesToken` enable → privilege has SE_PRIVILEGE_ENABLED
  - `NtDuplicateToken` effective_only → disabled privileges stripped
- [ ] Register: `test_register_security()`
- [ ] Commit: `"test: add security test suite — SID, ACL, token, privilege assertions"`

**Test checkpoint:** `make test` → serial shows `Security: 12 tests, 12 passed, 0 failed`.

---

## 4. Timer Test Suite

- [ ] `src/kernel/test/test_timer.c`:
  - `system_get_ticks()` → non-zero after boot
  - `uptime()` before + `sleep_ms(100)` + `uptime()` after → delta ≥ 90ms
  - Two consecutive `uptime()` calls → second ≥ first (monotonic)
  - `acpi_get_hpet_base()` → non-zero on platforms with HPET
- [ ] Register: `test_register_timer()`
- [ ] Commit: `"test: add timer test suite — tick, sleep, monotonicity"`

**Test checkpoint:** `make test` → serial shows `Timer: 4 tests, 4 passed, 0 failed`.

---

## 5. IPC Test Suite

- [x] `src/kernel/test/test_ipc.c` — 5 suites:
  - Kernel threads: two threads increment shared counter to 10
  - Mutex: two threads with mutex produce correct counter (200)
  - Semaphore: producer/consumer with 5 items
  - Pipe: writer -> reader data integrity round-trip
  - Shared memory: two threads increment counter to 200
- [x] Register: `test_register_ipc()` with `TEST_CAT_IPC` / `TEST_CAT_SCHED`
- [x] Uses `TEST_ASSERT_EQ` for value comparisons

> **Done:** 5 suites migrated from ad-hoc boot_tests.c into test_runner framework (2026-04-02).

**Test checkpoint:** `make test-ipc` → serial shows IPC suites with assertions passing.

---

## 6. VMM, Swap, and Mmap Test Suites

- [x] `src/kernel/test/test_vmm.c` — map page at 8 GiB, write/read round-trip, verify physical address, unmap
- [x] `src/kernel/test/test_swap.c` — swap_init, write page, swap_out, swap_in, verify data integrity
- [x] `src/kernel/test/test_mmap.c` — mmap a file from C:\, verify content is non-empty, munmap
- [x] All registered with `TEST_CAT_MM`
- [x] Hardware-dependent tests use `TEST_SKIP` when prerequisites unavailable

> **Done:** 3 suites migrated from ad-hoc boot_tests.c (2026-04-02).

**Test checkpoint:** `make test-mm` → serial shows VMM, swap, mmap suites alongside PMM/heap.

---

## 7. ELF Loader Test Suite

- [ ] `src/kernel/test/test_elf.c`:
  - `elf_validate(known_good_elf, size)` → returns 0 (success)
  - `elf_validate(truncated_buf, 16)` → returns error code (too small)
  - `elf_validate(bad_magic_buf, size)` → returns error code (bad magic)
  - `elf_validate(NULL, 0)` → returns error code (NULL pointer)
- [ ] Register: `test_register_elf()`
- [ ] Commit: `"test: add ELF loader test suite — validate good, truncated, bad magic"`

**Test checkpoint:** `make test` → serial shows `ELF: 4 tests, 4 passed, 0 failed`.

---

## 8. Migrate boot_tests.c into Unit Framework

- [x] VMM map/read/unmap → `test_vmm.c` (TEST_CAT_MM)
- [x] Swap out/in round-trip → `test_swap.c` (TEST_CAT_MM)
- [x] mmap file mapping → `test_mmap.c` (TEST_CAT_MM)
- [x] Kernel threads → `test_ipc.c` (TEST_CAT_IPC)
- [x] Mutex counter → `test_ipc.c` (TEST_CAT_SCHED)
- [x] Semaphore producer/consumer → `test_ipc.c` (TEST_CAT_IPC)
- [x] Pipe writer/reader → `test_ipc.c` (TEST_CAT_IPC)
- [x] Shared memory → `test_ipc.c` (TEST_CAT_IPC)
- [x] AHCI sector read → `test_storage.c` (TEST_CAT_STORAGE)
- [x] VirtIO-blk sector read → `test_storage.c` (TEST_CAT_STORAGE)
- [x] `boot_tests.c` now only dispatches to `test_runner_run()` + IXFS perf (debug=1 only)

> **Done:** 10 tests migrated from ad-hoc `klog()` to `TEST_ASSERT` / `TEST_ASSERT_EQ` / `TEST_SKIP` (2026-04-02).

**Test checkpoint:** `make test` → all former boot tests appear in unit test summary with category grouping.

---

## 9. NVMe Driver Test Suite

- [ ] `src/kernel/test/test_nvme.c`:
  - If `nvme_controller_count() == 0`: print `NVMe: SKIPPED (no controller)` and pass
  - `nvme_controller_count()` → ≥ 1
  - `nvme_get_controller(0)` → non-NULL, `nc->ns_count > 0`
  - `nvme_read_sectors(nc, 0, 0, 1, buf)` → returns 0 (success), buf non-zero
- [ ] Register: `test_register_nvme()`
- [ ] Commit: `"test: add NVMe driver test suite — conditional on hardware presence"`

**Test checkpoint:** On QEMU with NVMe: serial shows `NVMe: 3 tests, 3 passed`. Without NVMe: `NVMe: SKIPPED (no controller)`.

---

## 10. USB MSC Driver Test Suite

- [ ] `src/kernel/test/test_usb.c`:
  - If `xhci_controller_count() == 0`: print `USB: SKIPPED (no controller)` and pass
  - `xhci_controller_count()` → ≥ 1
  - `xhci_port_count(hc)` → > 0
  - If USB MSC device attached: `usb_msc_read_capacity()` → returns valid size > 0
- [ ] Register: `test_register_usb()`
- [ ] Commit: `"test: add USB MSC driver test suite — conditional on hardware presence"`

**Test checkpoint:** On QEMU with xHCI: serial shows `USB: 2+ tests, all passed`. Without xHCI: `USB: SKIPPED (no controller)`.

---

## 11. Test Coverage Report

- [ ] `scripts/test-coverage.sh`:
  - Scan `test_*.c` for `TEST_ASSERT` macro calls → count per file
  - Scan `test_runner_init()` for `test_register_*` calls → list registered suites
  - Output markdown table: `| Suite | File | Assertions | Status |`
  - Generate `build/test-coverage.md`
- [ ] Commit: `"infra: test coverage report — assertion count per subsystem"`

**Test checkpoint:** `bash scripts/test-coverage.sh` → outputs table with 11+ suites, 100+ total assertions, no missing suites.

---

## OS Comparison

| ⭐ | Feature                | 🪟 Win11            | 🐧 Linux            | 🚀 Impossible OS |
|----|------------------------|------------------|------------------|---------------|
| 💎 | Per-subsystem tests    | ✅ HLK suites   | ✅ KUnit + LTP   | ⬜ §2–§10     |
| 💎 | 100+ assertions        | ✅ Thousands     | ✅ Thousands     | ⬜ §2–§10     |
| ⭐ | Coverage report        | ❌ Internal      | ⚠️ lcov optional | ⬜ §11 🚀     |
| ⭐ | Hardware-adaptive skip | ❌ Requires HW   | ⚠️ Manual skip   | ⬜ §9–§10 🚀  |

---

## Unit Tests

> [!NOTE]
> This TODO's deliverables ARE the kernel test suites themselves (§2–§10). There is no separate "test the tests" step — each suite's test checkpoint verifies its own correctness via `make test`. The Verification section covers end-to-end acceptance.

---

## Verification

- [ ] `make test` → 15+ suites, 100+ assertions, all pass.
- [ ] New subsystem implementation → test suite created in same commit.
- [ ] Coverage report shows all major subsystems covered.
- [ ] NVMe/USB tests skip gracefully on QEMU without those devices.
- [ ] Commit: `"infra: kernel test coverage complete — 100+ assertions across 15 suites"`
