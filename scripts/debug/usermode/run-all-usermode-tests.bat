@echo off
:: run-all-usermode-tests.bat -- Run every test_*.exe deployed to C:\
::
:: Boots QEMU with `test=1` in boot.conf; the kernel test launcher
:: at src/kernel/test/test_usermode.c scans C:\ for test_*.exe
:: binaries and runs each sequentially in its own task.
:: Per-binary `[UTEST] <name>: PASS|FAIL (exit=N)` lines + a final
:: summary `[UTEST] === N passed, N failed of N total ===` land on
:: serial.
::
:: Today this aggregate runs ALL test_*.exe binaries (no filter). The
:: per-binary bat files that pass `utest_filter=<binary>` to run a
:: single binary in isolation land in this directory once the
:: launcher-manifest section (boot.conf utest_filter parser) + the
:: build-integration section (per-binary bat authoring) of the
:: user-mode test framework roadmap ship. Until then, this aggregate
:: is the only user-mode test runner.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly
pause
