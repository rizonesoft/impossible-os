@echo off
:: run-security-tests.bat -- Security tests (SIDs, ACLs, tokens, privileges)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite security -SerialLog
pause
