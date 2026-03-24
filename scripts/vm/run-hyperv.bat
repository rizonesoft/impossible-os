@echo off
:: run-hyperv.bat — Launch Impossible OS in Hyper-V Gen 2
:: Right-click -> "Run as administrator"

:: Elevate to admin if not already
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-hyperv.ps1"
pause
