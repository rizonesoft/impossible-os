@echo off
:: run-test_smoke_boot.bat -- §8 smoke binary (test_smoke_boot.exe)
::
:: Single sys_uptime()>=0 probe; launcher phase 0 runs it first and
:: aborts the rest of the suite on FAIL. NOTE: test=1 also runs the
:: kernel TEST_CAT_* suites BEFORE the launcher; usermode-only
:: iteration would need a future `test_kernel=0` boot.conf switch.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -UtestFilter "test_smoke_boot.exe"
pause
