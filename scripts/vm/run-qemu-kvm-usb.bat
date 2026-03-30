@echo off
:: run-qemu-kvm-usb.bat -- Launch Impossible OS in QEMU with WHPX + xHCI USB storage
::
:: Attaches a 64 MiB FAT32 USB disk via an emulated xHCI controller (PCI class 0C:03:30)
:: alongside the regular AHCI system disk. For xHCI + USB MSC driver testing.
::
:: UTS timer: LAPIC (hardware accelerated path)

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-qemu-kvm-usb.ps1"
pause
