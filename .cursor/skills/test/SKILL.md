---
name: test
description: Run kernel unit tests. Use after implementing code, fixing bugs, or when asked to verify the build. Builds the OS, boots QEMU headless with test=1, parses pass/fail from serial output, and reports results. Invoke with /test.
---

# Kernel Unit Test Runner

## When to Use

- After implementing a TODO section to verify correctness
- After any code change that could affect existing functionality
- When the user asks to "run tests", "check if tests pass", or "verify the build"
- Proactively after writing code that touches: memory, scheduler, VFS, drivers, Object Manager, registry

## Quick Path

```bash
bash scripts/test.sh                  # all categories
bash scripts/test.sh SUITE=mm         # only Memory Management
bash scripts/test.sh SUITE=ob         # only Object Manager
bash scripts/test.sh QUIET=1          # summary only (suppress PASS lines)
bash scripts/test.sh SUITE=fs QUIET=1 # combine both
```

Or via Make:

```bash
make test                  # all categories
make test SUITE=mm         # only MM suites
make test QUIET=1          # summary only
make test TIMEOUT=120      # longer timeout
```

Per-category Make targets:

```bash
make test-mm       # Memory Management (PMM, heap, VMM, swap, mmap)
make test-fs       # Filesystem (VFS)
make test-sched    # Scheduler
make test-ob       # Object Manager
make test-security # Security
make test-ipc      # IPC
make test-boot     # Boot & Logging
make test-abi      # ABI Compatibility (registry, PEB/TEB)
make test-storage  # Storage Drivers
```

### What the Script Does

1. Builds the kernel (`scripts/build.sh`)
2. Patches `boot.conf` with `test=1` (+ optional `test_suite`/`test_quiet`)
3. Auto-detects KVM or falls back to TCG (with warning)
4. Boots QEMU headless, serial output to `build/test.log`
5. Polls for the `=== N tests passed ===` summary line (timeout default 60s)
6. Kills QEMU once results are captured
7. Restores `boot.conf` to defaults (even on failure)
8. Parses and displays colored per-suite pass/fail results
9. Exits 0 if all passed, 1 if any failed or timeout

### Exit Codes

- `0` -- all tests passed
- `1` -- one or more tests failed, timeout, or build failure

## Test Categories

Categories are defined in `include/kernel/test/test.h` as `test_category_t`:

| Enum               | Short name | Label              | Make target     |
|---------------------|------------|--------------------|-----------------|
| `TEST_CAT_MM`       | `mm`       | Memory Management  | `make test-mm`  |
| `TEST_CAT_FS`       | `fs`       | Filesystem         | `make test-fs`  |
| `TEST_CAT_SCHED`    | `sched`    | Scheduler          | `make test-sched` |
| `TEST_CAT_OB`       | `ob`       | Object Manager     | `make test-ob`  |
| `TEST_CAT_SECURITY` | `security` | Security           | `make test-security` |
| `TEST_CAT_IPC`      | `ipc`      | IPC                | `make test-ipc` |
| `TEST_CAT_BOOT`     | `boot`     | Boot & Logging     | `make test-boot` |
| `TEST_CAT_ABI`      | `abi`      | ABI Compatibility  | `make test-abi` |
| `TEST_CAT_STORAGE`  | `storage`  | Storage Drivers    | `make test-storage` |
| `TEST_CAT_ALL`      | --          | Runs under any filter | -- |

## Boot Flow

`boot.conf` keys:
- `test=1` -- enables unit test runner
- `test_suite=mm` -- category filter (short name from table above)
- `test_quiet=1` -- suppress per-assertion PASS lines

Kernel dispatch (`src/kernel/main/boot_tests.c`):
1. `boot_phase3()` in `boot_desktop.c` calls `boot_tests_run()`
2. If `test=1` or `debug=1`: calls `test_runner_init()` then `test_runner_run()`
3. `test=1` without `debug=1`: runs unit tests only, suppresses non-TEST log noise, then continues to desktop
4. `debug=1`: runs unit tests AND integration tests (VFS smoke, IXFS CRUD, timer), then desktop

## Interpreting Results

- `=== N tests passed, 0 failed ===` -- ALL PASS
- `=== N passed, M FAILED (of T) ===` -- FAILURES, report which suites failed
- No test output / empty log -- boot crashed before tests ran (check build)
- `KERNEL PANIC` or `triple fault` -- boot failure, not test failure
- Timeout with partial output -- boot too slow (TCG) or hung (kernel bug)

## Manual Path (if script unavailable)

```bash
bash scripts/build.sh
bash scripts/patch-boot-conf.sh test 1
timeout 60 qemu-system-x86_64 \
    -accel kvm -cpu host -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd \
    -drive id=disk0,file=build/system-disk.img,format=raw,if=none \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -m 2G -serial file:build/test.log -display none \
    -device rtl8139,netdev=net0 -netdev user,id=net0 \
    -device virtio-tablet-pci -rtc base=localtime \
    -no-reboot 2>/dev/null; true
bash scripts/patch-boot-conf.sh reset   # ALWAYS restore
grep -E 'TEST:.*::|\[FAIL\]|=== .* test' build/test.log | tail -40
```

If KVM is unavailable, replace `-accel kvm -cpu host` with `-accel tcg -cpu Haswell` (10x slower).

## After Tests

- If all pass: report "All N tests passed" and continue
- If any fail: read `[FAIL]` lines from `build/test.log`, diagnose, fix, re-run
- Always restore boot.conf even if tests fail (the script handles this automatically)

## What NOT to Do

- Do NOT set `debug=1` for automated testing -- it runs integration tests and doesn't shutdown
- Do NOT forget to restore boot.conf (`patch-boot-conf.sh reset`) when running manually
- Do NOT use `bash scripts/build.sh clean` for test runs unless needed -- incremental is faster
