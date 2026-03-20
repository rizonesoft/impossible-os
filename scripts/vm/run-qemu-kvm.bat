@echo off
:: run-qemu-kvm.bat — Launch Impossible OS in Windows QEMU
:: Requires: QEMU for Windows installed and in PATH

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-qemu.ps1"
pause
