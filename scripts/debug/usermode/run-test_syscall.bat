@echo off
:: run-test_syscall.bat -- §9 syscall coverage test binary (test_syscall.exe)
::
:: Boots QEMU with test=1 + test_kernel_skip=1 + utest_filter=test_syscall.exe
:: so the §3 launcher only spawns this one user-mode binary and the
:: kernel TEST_CAT_* sweep is skipped.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_syscall.exe"
pause
