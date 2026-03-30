@echo off
REM sdk\build.bat — Launcher that calls build.ps1 via PowerShell
REM
REM Usage:
REM   sdk\build.bat          Build all SDK tools
REM   sdk\build.bat clean    Clean all SDK tool build artifacts

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
