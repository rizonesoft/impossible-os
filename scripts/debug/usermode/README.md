# scripts\debug\usermode -- User-mode test runners

Empty until the user-mode test framework grows a way to run user-mode
binaries WITHOUT also running the full kernel TEST_CAT_* suite.

## Why empty today

The kernel test launcher (in [`src/kernel/test/test_usermode.c`](../../../src/kernel/test/test_usermode.c))
is currently chained inside the `boot.conf test=1` flow: when test=1
is set, the kernel test runner runs ALL TEST_CAT_* suites and THEN
the user-mode launcher scans `C:\` for `test_*.exe`. There's no boot
flag yet that runs ONLY the user-mode launcher. Every Windows-side
bat that boots with `-TestOnly` therefore runs the full
kernel + user-mode chain -- which is what
[`scripts\debug\kernel\run-all-kernel-tests.bat`](../kernel/run-all-kernel-tests.bat)
already does.

A separate `run-all-usermode-tests.bat` here would just be a
duplicate of the kernel runner with a misleading name. Removed for
that reason on 2026-04-20 (incident: the file existed briefly and
ran the same path as `run-all-kernel-tests.bat`).

## When this directory will fill up

Two prerequisites from the [user-mode test framework TODO](../../../todo/00-infrastructure/TODO-04-usermode-test-framework.md):

1. **Launcher-manifest section** lands a `utest_filter=<name|glob>`
   boot.conf parameter, AND a `usermode_only=1` (or equivalent) knob
   that skips the kernel TEST_CAT_* runner so the launcher runs in
   isolation.
2. **Build-integration section** authors per-binary bat files here:

   ```bat
   :: scripts\debug\usermode\run-test_syscall.bat (example, future)
   powershell.exe -ExecutionPolicy Bypass -File ^
       "%~dp0..\..\machines\run-qemu.ps1" ^
       -Accel whpx -TestOnly -BootArg "utest_filter=test_syscall.exe usermode_only=1"
   pause
   ```

   Plus a `run-all-usermode-tests.bat` aggregate with `utest_filter`
   unset and `usermode_only=1` set.

## What runs the user-mode launcher TODAY

[`scripts\debug\kernel\run-all-kernel-tests.bat`](../kernel/run-all-kernel-tests.bat)
boots with `test=1` -> kernel test runner runs first, then the user-mode
launcher scans `C:\` for `test_*.exe` and runs each, emitting per-binary
`[UTEST] <name>: PASS|FAIL (exit=N)` lines + a `[UTEST] === N passed,
N failed of N total ===` summary on serial.

For the full user-mode sweep run
[`run-all-usermode-tests.bat`](run-all-usermode-tests.bat) directly;
for kernel-side suites run
[`scripts\debug\kernel\run-all-kernel-tests.bat`](../kernel/run-all-kernel-tests.bat)
separately.
