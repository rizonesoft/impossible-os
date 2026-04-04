@echo off
:: run-all-tests.bat -- Run all kernel unit test suites, then shutdown
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -SerialLog
pause
