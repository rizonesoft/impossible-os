@echo off
REM Test 10: ext4
powershell -ExecutionPolicy Bypass -File "%~dp0test-filesystem.ps1" ext4
pause
