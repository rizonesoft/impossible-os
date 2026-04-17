@echo off
:: run-usb-test.bat -- Test xHCI + USB MSC driver in QEMU
::
:: Double-click to boot with a 64 MiB FAT32 USB disk on xHCI controller.
:: CLI: pass through -Accel, -Build, -ExtraArgs "-s -S" etc. via %*.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-usb-test.ps1" %*
pause
