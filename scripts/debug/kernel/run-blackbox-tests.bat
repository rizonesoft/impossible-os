@echo off
:: run-blackbox-tests.bat -- BlackBox partition tests (X:\ mount, dirs, label, free space)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite fs
pause
