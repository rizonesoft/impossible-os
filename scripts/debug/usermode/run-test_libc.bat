@echo off
:: run-test_libc.bat -- §10 libc coverage binary (test_libc.exe)
::
:: Smoke-checks user/include/string.h + stdio.h: strlen, strcmp x2,
:: memcpy, memset, snprintf. NOTE: test=1 also runs the kernel
:: TEST_CAT_* suites BEFORE the launcher; usermode-only iteration
:: would need a future `test_kernel=0` boot.conf switch.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -UtestFilter "test_libc.exe"
pause
