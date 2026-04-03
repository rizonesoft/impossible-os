@echo off
:: run-fat32-test.bat -- Test FAT32 driver in QEMU
::
:: Double-click to boot with a FAT32 test disk on AHCI port 1.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-fs-test.ps1" -Disk fat32
pause
