@echo off
:: run-fat32-test.bat -- Test FAT32 driver in QEMU
::
:: Double-click to boot with a FAT32 test disk on AHCI port 1.
:: CLI: pass through extra run-fs-test.ps1 args (e.g. -ExtraArgs "-s -S") via %*.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-fs-test.ps1" -Disk fat32 %*
pause
