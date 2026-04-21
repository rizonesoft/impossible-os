@echo off
:: run-all-usermode-tests.bat -- Boot once, run every user-mode test_*.exe
::
:: -TestOnly + -NoKernelTests + (no -UtestFilter) makes the §3 launcher
:: walk every test_*.exe deployed at C:\ via the §4 manifest (smoke
:: phase first, then correctness/stress/perf) WITHOUT re-running the
:: kernel TEST_CAT_* sweep first.
::
:: For per-binary iteration use one of the run-test-<name>.bat files
:: in this directory. For the cross-layer sweep use the parent
:: scripts\debug\run-all-tests.bat which chains all three layers
:: (kernel + usermode + desktop).
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests
pause
