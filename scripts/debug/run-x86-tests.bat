@echo off
:: run-x86-tests.bat -- x86-64 architecture tests (CPUID, MSR, KPTI, CPU security)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite x86
pause
