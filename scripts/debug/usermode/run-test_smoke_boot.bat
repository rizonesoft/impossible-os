@echo off
:: run-test_smoke_boot.bat -- §8 smoke binary (test_smoke_boot.exe)
::
:: Single sys_uptime()>=0 probe; launcher phase 0 runs it first and
:: aborts the rest of the suite on FAIL. -NoKernelTests skips the
:: kernel TEST_CAT_* sweep so this iterates fast.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_smoke_boot.exe"
pause
