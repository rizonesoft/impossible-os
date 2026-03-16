@echo off
:: write-usb.bat — Write Impossible OS to a USB flash drive
:: Right-click → "Run as administrator"

:: Elevate to admin if not already
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

powershell.exe -ExecutionPolicy Bypass -File "%~dp0write-usb.ps1"
pause
