@echo off
:: run-ob-tests.bat -- Object Manager tests (alloc, refcount, handles, namespace)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite ob -SerialLog
pause
