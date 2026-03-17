@echo off
:: run-windows.bat — Launch Impossible OS in Windows QEMU
:: Requires: QEMU for Windows installed and in PATH
::
:: Usage:
::   run-windows.bat           Normal boot (splash screen)
::   run-windows.bat debug     Debug boot (live text output, no splash)

set "EXTRA_ARGS="
if /I "%~1" == "debug" (
    set "EXTRA_ARGS=-DebugBoot"
    echo *** DEBUG BOOT MODE ***
)

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-windows.ps1" %EXTRA_ARGS%
pause
