@echo off
:: run-fs-tests.bat -- Filesystem tests (VFS, IXFS)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite fs
pause
