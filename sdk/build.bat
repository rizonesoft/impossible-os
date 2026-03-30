@echo off
REM sdk\build.bat — Launcher that calls build.ps1 via PowerShell
REM
REM Usage:
REM   sdk\build.bat          Build all SDK tools
REM   sdk\build.bat clean    Clean all SDK tool build artifacts

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set EXIT_CODE=%ERRORLEVEL%

if %EXIT_CODE% NEQ 0 (
    echo.
    echo Build failed with exit code %EXIT_CODE%
)

pause
exit /b %EXIT_CODE%
