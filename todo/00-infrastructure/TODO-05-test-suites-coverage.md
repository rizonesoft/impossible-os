# TODO-05 — Kernel Test Suites & Coverage Expansion

> **Goal:** Every kernel subsystem has test coverage. When a new feature is implemented, a test suite is created alongside it. Test coverage is tracked and visible. The test system is easy to extend — adding a new test suite is one file + one register call. Target: 100+ test assertions covering every subsystem that has shipped code.

> [!IMPORTANT]
> **Current state:** 5 test suites exist (PMM, heap, VFS, sched, registry) with ~12 test functions. Missing: Object Manager, security (SID/ACL/token), NVMe, USB, timer, interrupt, IPC (pipe/shmem/signal), process model, page tables, ELF loader. boot_tests.c has 15+ integration tests but they're not in the unit test framework.

---

## Inputs

- `src/kernel/test/test_runner.c` — test framework
- `src/kernel/test/test_*.c` — existing 5 suites
- `src/kernel/main/boot_tests.c` — 15+ integration tests (not in unit framework)
- → XREF: `TODO-03-kernel-test-framework.md §1,§3` — framework wiring and initial new suites

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
| 💎  |   1   | Object Manager test suite (test_ob.c)         | T03 §1     |  [ ]   |
| 💎  |   2   | Security test suite (test_security.c)         | T03 §1     |  [ ]   |
| 💎  |   3   | Timer and interrupt test suite                | T03 §1     |  [ ]   |
| 💎  |   4   | IPC test suite (pipe, shmem, signal)          | T03 §1     |  [ ]   |
| 💎  |   5   | VMM and page table test suite                 | T03 §1     |  [ ]   |
| 💎  |   6   | ELF loader test suite                         | T03 §1     |  [ ]   |
| 💎  |   7   | Migrate boot_tests.c into unit framework      | T03 §1     |  [ ]   |
| 💎  |   8   | NVMe driver test suite                        | T03 §1     |  [ ]   |
| 💎  |   9   | USB MSC driver test suite                     | T03 §1     |  [ ]   |
| ⭐  |  10   | Test coverage report generator                | §1–§9      |  [ ]   |

> 💎 = parity — Linux KUnit and Windows HLK both have per-subsystem test suites.
> ⭐ = exclusive — automated coverage report showing assertion count per subsystem.

---

## 1. Object Manager Test Suite

- [ ] `src/kernel/test/test_ob.c`:
  - `ob_alloc_object(ObpFileType)` → non-NULL body, header recoverable via macro
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
- [ ] Commit: `"test: add Object Manager test suite — 10+ assertions"`

---

## 2. Security Test Suite

- [ ] `src/kernel/test/test_security.c`:
  - `RtlEqualSid(SeLocalSystemSid, SeLocalSystemSid)` → true
  - `RtlEqualSid(SeLocalSystemSid, SeWorldSid)` → false
  - `RtlLengthSid(SeLocalSystemSid)` → 12 (8 + 4*1)
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

---

## 3–9. Additional Test Suites

Each follows the same pattern: create `test_<name>.c`, implement test functions using `TEST_ASSERT`, register in `test_runner_init()`.

- [ ] §3: Timer — `uptime()` increases after `sleep_ms(100)`, `system_get_ticks()` non-zero
- [ ] §4: IPC — pipe create/write/read round-trip, shmem create/map/read, signal handler registration
- [ ] §5: VMM — `vmm_map_page` + read/write + `vmm_unmap_page`, `vmm_get_physical` round-trip
- [ ] §6: ELF — `elf_validate` on known-good binary, reject truncated binary
- [ ] §7: Migrate boot_tests.c integration tests into unit framework suites
- [ ] §8: NVMe — controller count, sector read (if NVMe present, skip otherwise)
- [ ] §9: USB — controller count, device count (if USB present, skip otherwise)

---

## 10. Test Coverage Report

- [ ] `scripts/test-coverage.sh`:
  - Scan `test_*.c` for `TEST_ASSERT` macro calls → count per file
  - Scan `test_runner_init()` for `test_register_*` calls → list registered suites
  - Output markdown table: `| Suite | File | Assertions | Status |`
  - Generate `build/test-coverage.md`
- [ ] Commit: `"infra: test coverage report — assertion count per subsystem"`

---

## OS Comparison

| ⭐ | Feature                  | Win11            | Linux             | Impossible OS       |
|----|--------------------------|------------------|-------------------|---------------------|
| 💎 | Per-subsystem tests      | ✅ HLK suites   | ✅ KUnit + LTP    | ⬜ §1–§9            |
| 💎 | 100+ assertions          | ✅ Thousands     | ✅ Thousands      | ⬜ §1–§9            |
| ⭐ | Coverage report          | ❌ Internal      | ⚠️ lcov optional  | ⬜ §10 🚀           |

---

## Verification

- [ ] `make test` → 15+ suites, 100+ assertions, all pass.
- [ ] New subsystem implementation → test suite created in same commit.
- [ ] Coverage report shows all major subsystems covered.
- [ ] Commit: `"infra: kernel test coverage complete — 100+ assertions across 15 suites"`
