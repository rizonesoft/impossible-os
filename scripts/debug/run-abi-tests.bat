@echo off
:: run-abi-tests.bat -- ABI tests (PEB/TEB, Registry)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite abi -SerialLog
pause
