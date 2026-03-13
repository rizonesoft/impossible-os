@echo off
REM Test 9: ext3
powershell -ExecutionPolicy Bypass -File "%~dp0test-filesystem.ps1" ext3
pause
