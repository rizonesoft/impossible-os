@echo off
:: write-usb.bat — Write Impossible OS to a USB flash drive
:: Right-click -> "Run as administrator"
::
:: Usage:
::   write-usb.bat           Normal boot (splash screen)
::   write-usb.bat debug     Debug boot (live text output, no splash)

:: Elevate to admin if not already
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process '%~f0' -ArgumentList '%*' -Verb RunAs"
    exit /b
)

:: Check for debug flag
set "EXTRA_ARGS="
if /I "%~1" == "debug" (
    set "EXTRA_ARGS=-DebugBoot"
    echo.
    echo *** DEBUG BOOT MODE ***
    echo.
)

powershell.exe -ExecutionPolicy Bypass -File "%~dp0write-usb.ps1" %EXTRA_ARGS%
pause
