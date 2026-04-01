---
description: Run kernel unit tests automatically. Use after implementing code to verify nothing broke. Builds the OS, boots QEMU headless with test=1, captures serial output, parses pass/fail results, and reports them.
user_invocable: true
---

# Run Kernel Tests

## When to Use

- After implementing a TODO section to verify correctness
- After any code change that could affect existing functionality
- When the user asks to "run tests", "check if tests pass", or "verify the build"
- Proactively after writing code that touches: Object Manager, security, VFS, scheduler, memory, drivers

## Workflow

1. Build the OS with `bash scripts/build.sh clean` — verify `=== BUILD OK ===`
2. Patch boot.conf to `test=1` for automated test mode
3. Boot QEMU headless with serial captured to file
4. Wait for QEMU to exit (test=1 triggers acpi_shutdown after tests)
5. Parse serial output for test results
6. Report pass/fail to the user
7. Restore boot.conf to defaults

## Commands

```bash
# Step 1: Build
bash scripts/build.sh clean
tail -1 build/build.log  # must show === BUILD OK ===

# Step 2: Patch boot.conf for test mode
bash scripts/patch-boot-conf.sh test 1

# Step 3: Boot headless QEMU, capture serial, timeout 60s
timeout 60 qemu-system-x86_64 \
    -cpu Haswell -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd \
    -drive id=disk0,file=build/system-disk.img,format=raw,if=none \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -m 2G \
    -serial file:build/test.log \
    -display none \
    -no-reboot \
    2>/dev/null || true

# Step 4: Restore boot.conf
bash scripts/patch-boot-conf.sh reset

# Step 5: Parse results
grep -E "\[ OK \]|\[FAIL\]|=== .* test|TEST" build/test.log | tail -30
```

## Interpreting Results

- `=== X tests passed, 0 failed ===` → ALL PASS ✅
- `=== X passed, Y FAILED ===` → FAILURES ❌ — report which suites failed
- No test output at all → test framework not wired or boot crashed before tests ran
- `KERNEL PANIC` or `triple fault` in log → boot failure, not test failure

## What NOT to Do

- Do NOT set `debug=1` — that runs boot tests too and doesn't shutdown (desktop continues)
- Do NOT forget to restore boot.conf (`patch-boot-conf.sh reset`)
- Do NOT run tests on WHPX from WSL (use KVM or TCG from Linux)

## After Tests

- If all pass: report "All tests passed" and continue with the task
- If any fail: read the `[FAIL]` lines, diagnose the root cause, fix it, re-run tests
- Always restore boot.conf even if tests fail
