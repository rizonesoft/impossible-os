@echo off
REM Test 7: exFAT
powershell -ExecutionPolicy Bypass -File "%~dp0test-filesystem.ps1" exfat
pause
