@echo off
:: run-windows.bat — Double-click to launch Impossible OS in Windows QEMU
:: Requires: QEMU for Windows installed and in PATH

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-windows.ps1"
pause
