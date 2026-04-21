@echo off
:: run-test_syscall.bat -- §9 syscall coverage test binary (test_syscall.exe)
::
:: Boots QEMU with test=1 + utest_filter=test_syscall.exe so the §3
:: launcher only spawns this user-mode binary. NOTE: test=1 also runs
:: the kernel TEST_CAT_* suites BEFORE the launcher (boot_tests.c
:: dispatches both). True usermode-only iteration would need a future
:: `test_kernel=0` boot.conf switch.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -UtestFilter "test_syscall.exe"
pause
