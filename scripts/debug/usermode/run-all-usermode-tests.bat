@echo off
:: run-all-usermode-tests.bat -- Boot once, run every user-mode test_*.exe
::
:: -TestOnly + -NoKernelTests + (no -UtestFilter) makes the §3 launcher
:: walk every test_*.exe deployed at C:\ via the §4 manifest (smoke
:: phase first, then correctness/stress/perf) WITHOUT re-running the
:: kernel TEST_CAT_* sweep first.
::
:: For per-binary iteration use one of the run-test_<name>.bat files
:: in this directory. For kernel-side TEST_CAT_* suites run
:: scripts\debug\kernel\run-all-kernel-tests.bat separately.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests
pause
