@echo off
:: run-except-tests.bat -- Exception dispatch / SEH (TEST_CAT_EXCEPT) suite on QEMU WHPX
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite except
pause
