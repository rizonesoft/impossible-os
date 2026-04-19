@echo off
:: run-boot-tests.bat -- Boot init and klog tests
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite boot
pause
