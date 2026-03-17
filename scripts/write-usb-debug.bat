@echo off
:: write-usb-debug.bat — Write Impossible OS to USB with debug boot enabled
:: Splash screen disabled, live text output on screen
:: Right-click -> "Run as administrator"

:: Elevate to admin if not already
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

powershell.exe -ExecutionPolicy Bypass -File "%~dp0write-usb.ps1" -DebugBoot
pause
