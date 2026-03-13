@echo off
REM Test 7: exFAT
powershell -ExecutionPolicy Bypass -File "%~dp0test-fs.ps1" exfat
pause
