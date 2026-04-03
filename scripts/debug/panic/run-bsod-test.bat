@echo off
:: run-bsod-test.bat -- Trigger deliberate BSOD to test panic screen
:: Sets crash_test=1 via run-qemu.ps1, kernel panics after desktop init.
:: After BSOD: close QEMU, run run-crash-recovery-test.bat to verify recovery.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -CrashTest
pause
