@echo off
:: run-windows-debug.bat — Launch Impossible OS in QEMU with debug boot
:: Splash screen disabled, live text output on screen

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-windows.ps1" -DebugBoot
pause
