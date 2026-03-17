@echo off
:: run-vbox.bat — Launch Impossible OS in VirtualBox
::
:: Usage:
::   run-vbox.bat           Normal boot (splash screen)
::   run-vbox.bat debug     Debug boot (live text output, no splash)

cd /d "%~dp0"

set "EXTRA_ARGS="
if /I "%~1" == "debug" (
    set "EXTRA_ARGS=-DebugBoot"
    echo *** DEBUG BOOT MODE ***
)

powershell -ExecutionPolicy Bypass -File "%~dp0run-vbox.ps1" %EXTRA_ARGS%
pause
