@echo off
:: run-nls-tests.bat -- Atom/NLS/locale subsystem tests (TEST_CAT_NLS, TODO-13)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite nls
pause
