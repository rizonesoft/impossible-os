@echo off
:: run-nvme-test.bat -- Test NVMe storage driver in QEMU
::
:: Double-click to boot with a 128 MiB NVMe drive alongside the AHCI system disk.
:: CLI: pass through -Accel, -Build, -ExtraArgs "-s -S" etc. via %*.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-nvme-test.ps1" %*
pause
