# TODO-05 — Kernel Test Suites & Coverage Expansion

> **Goal:** Every kernel subsystem has test coverage. When a new feature is implemented, a test suite is created alongside it. Test coverage is tracked and visible. The test system is easy to extend — adding a new test suite is one file + one register call. Target: 100+ test assertions covering every subsystem that has shipped code.

> [!IMPORTANT]
> **Current state (2026-04-02):** 26 test suites, 59 assertions. Suites: PMM (2), heap (4), VFS (3), sched (1), registry (2), boot init (8), klog (6), OB (7), security (5). `make test` runs all via headless QEMU. Still needed: timer, IPC, VMM, ELF, NVMe, USB, coverage report.

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
| 💎  |   5   | IPC test suite (event, semaphore)             | §1         |  [ ]   |
| 💎  |   6   | VMM and page table test suite                 | §1         |  [ ]   |
| 💎  |   7   | ELF loader test suite                         | §1         |  [ ]   |
| 💎  |   8   | Migrate boot_tests.c into unit framework      | §1         |  [ ]   |
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

- [ ] `src/kernel/test/test_ipc.c`:
  - `NtCreateEvent(NULL, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE)` → valid handle
  - `NtSetEvent(handle)` + `NtWaitForSingleObject(handle, FALSE, NULL)` → returns `STATUS_SUCCESS` immediately
  - `NtCreateSemaphore(NULL, SEMAPHORE_ALL_ACCESS, NULL, 2, 2)` → valid handle
  - 2× `NtWaitForSingleObject(sem)` → both succeed (count decrements to 0)
- [ ] Register: `test_register_ipc()`
- [ ] Commit: `"test: add IPC test suite — event, semaphore assertions"`

**Test checkpoint:** `make test` → serial shows `IPC: 4 tests, 4 passed, 0 failed`.

---

## 6. VMM and Page Table Test Suite

- [ ] `src/kernel/test/test_vmm.c`:
  - `vmm_map_page(test_vaddr, test_paddr, VMM_FLAG_WRITE)` → write 0xDEAD + read → 0xDEAD
  - `vmm_get_physical(test_vaddr)` → returns test_paddr
  - `vmm_unmap_page(test_vaddr)` + `vmm_get_physical(test_vaddr)` → returns 0
  - `pmm_alloc_page()` + `pmm_free_page()` round-trip → page reusable
- [ ] Register: `test_register_vmm()`
- [ ] Commit: `"test: add VMM test suite — map, unmap, physical lookup"`

**Test checkpoint:** `make test` → serial shows `VMM: 4 tests, 4 passed, 0 failed`.

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

- [ ] Identify each test in `boot_tests.c` (VFS, threading, IPC, sync, ~15 tests)
- [ ] For each: wrap in `TEST_ASSERT` macro, move to appropriate `test_<subsystem>.c` or create new file
- [ ] Remove ad-hoc test code from `boot_tests.c` — replace with `test_runner_run()` call only
- [ ] Verify total assertion count increases by 15+
- [ ] Commit: `"test: migrate boot_tests.c integration tests into unit framework"`

**Test checkpoint:** `make test` → boot tests no longer appear as separate ad-hoc serial output → all appear in unit test summary with pass/fail counts. Total assertions ≥ 50.

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
