@echo off
:: read-usb-log.bat -- Read kernel logs from Impossible OS USB drive

:: Elevate to admin if not already
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

powershell.exe -ExecutionPolicy Bypass -File "%~dp0read-usb-log.ps1"
pause
