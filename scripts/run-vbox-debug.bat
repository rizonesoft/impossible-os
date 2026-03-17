@echo off
:: run-vbox-debug.bat — Launch Impossible OS in VirtualBox with debug boot
:: Splash screen disabled, live text output on screen

cd /d "%~dp0"
powershell -ExecutionPolicy Bypass -File "%~dp0run-vbox.ps1" -DebugBoot
pause
