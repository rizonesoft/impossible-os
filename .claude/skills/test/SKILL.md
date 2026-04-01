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

Run tests via the standalone script — it handles build, boot.conf patching, QEMU, parsing, and cleanup:

```bash
bash scripts/test.sh
```

Or via Make:

```bash
make test                  # all suites
make test SUITE=pmm        # only suites matching "pmm"
```

### What the Script Does

1. Builds the kernel (`scripts/build.sh`)
2. Patches `boot.conf` with `test=1` (unit-test-only mode, auto-shutdown)
3. Auto-detects KVM or falls back to TCG (with warning)
4. Boots QEMU headless, serial output to `build/test.log`
5. Waits for QEMU exit or timeout (default 60s, override with `TIMEOUT=120`)
6. Restores `boot.conf` to defaults (even on failure)
7. Parses the `=== N tests passed, M failed ===` summary line
8. Prints colored per-suite pass/fail results
9. Exits 0 if all passed, 1 if any failed or timeout

### Exit Codes

- `0` — all tests passed
- `1` — one or more tests failed, timeout, or build failure

## Manual Path (if script unavailable)

```bash
# Build
bash scripts/build.sh
tail -1 build/build.log  # must show === BUILD OK ===

# Patch boot.conf for test mode
bash scripts/patch-boot-conf.sh test 1

# Boot headless QEMU (60s timeout, serial to file)
timeout 60 qemu-system-x86_64 \
    -accel kvm -cpu host -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd \
    -drive id=disk0,file=build/system-disk.img,format=raw,if=none \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -m 2G \
    -serial file:build/test.log \
    -display none \
    -device rtl8139,netdev=net0 \
    -netdev user,id=net0 \
    -device virtio-tablet-pci \
    -rtc base=localtime \
    -no-reboot 2>/dev/null; true

# ALWAYS restore boot.conf
bash scripts/patch-boot-conf.sh reset

# Parse results
grep -E "TEST:.*::|\[FAIL\]|=== .* test" build/test.log | tail -40
```

If KVM is unavailable (no `/dev/kvm` access), replace `-accel kvm -cpu host` with `-accel tcg -cpu Haswell` (10x slower).

## Interpreting Results

- `=== N tests passed, 0 failed ===` — ALL PASS
- `=== N passed, M FAILED (of T) ===` — FAILURES, report which suites failed
- No test output / empty log — boot crashed before tests ran (check build)
- `KERNEL PANIC` or `triple fault` — boot failure, not test failure
- Timeout with partial output — boot too slow (TCG) or hung (kernel bug)

## What NOT to Do

- Do NOT set `debug=1` for automated testing — that runs boot tests too and doesn't shutdown
- Do NOT forget to restore boot.conf (`patch-boot-conf.sh reset`) when running manually
- Do NOT use `bash scripts/build.sh clean` for test runs unless the user requests it — incremental builds are faster

## After Tests

- If all pass: report "All N tests passed" and continue with the task
- If any fail: read the `[FAIL]` lines from `build/test.log`, diagnose the root cause, fix it, re-run
- Always restore boot.conf even if tests fail (the script handles this automatically)

## Test Modes Reference

| boot.conf | Behavior |
|---|---|
| `test=1` | Unit tests only, then `acpi_shutdown()` — for CI/automation |
| `debug=1` | Unit tests + boot tests + continue to desktop — for interactive debugging |
| Both `0` | Skip all tests, boot straight to desktop — production |
