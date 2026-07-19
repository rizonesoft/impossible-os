@echo off
:: run-quota-tests.bat -- Resource accounting & quotas (TEST_CAT_QUOTA) suite on QEMU WHPX
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite quota
pause
