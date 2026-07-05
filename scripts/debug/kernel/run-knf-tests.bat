@echo off
:: run-knf-tests.bat -- Kernel Notification Facility (TEST_CAT_KNF) suite on QEMU WHPX
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite knf
pause
