@echo off
:: run-qemu-kvm.bat — Launch Impossible OS in QEMU with WHPX acceleration
::
:: WHPX (Windows Hypervisor Platform) provides near-native speed.
:: Requires: Hyper-V enabled in Windows Features + QEMU for Windows.
:: UTS timer: LAPIC (hardware accelerated path)
::
:: For Secure Boot testing, use: run-qemu-kvm-secureboot.bat

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-qemu.ps1" -Accel whpx
pause
