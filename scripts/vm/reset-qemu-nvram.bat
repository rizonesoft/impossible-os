@echo off
:: reset-qemu-nvram.bat - Reset QEMU UEFI NVRAM to factory defaults
:: Use after a clean build or when NVRAM is corrupted.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0reset-qemu-nvram.ps1"
pause
