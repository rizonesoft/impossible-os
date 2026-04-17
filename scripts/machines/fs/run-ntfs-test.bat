@echo off
:: run-ntfs-test.bat -- Test NTFS driver in QEMU
::
:: Double-click to:
::   1. Generate NTFS test disk (32 MiB with comprehensive test files)
::   2. Boot Impossible OS with test disk on AHCI port 1
::   3. Watch serial output for [PASS]/[FAIL] test results
:: CLI: pass through extra run-fs-test.ps1 args (e.g. -ExtraArgs "-s -S") via %*.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-fs-test.ps1" -Disk ntfs -GenDisk %*
pause
