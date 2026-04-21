@echo off
:: run-test-libc.bat -- §10 libc coverage binary (test_libc.exe)
::
:: Smoke-checks user/include/string.h + stdio.h: strlen, strcmp x2,
:: memcpy, memset, snprintf. -NoKernelTests sets boot.conf
:: test_kernel_skip=1 so the launcher fires WITHOUT the kernel
:: TEST_CAT_* sweep first; -UtestFilter limits the launcher to this
:: one binary.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_libc.exe"
pause
