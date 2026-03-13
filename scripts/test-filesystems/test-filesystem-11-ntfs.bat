@echo off
REM Test 11: NTFS
powershell -ExecutionPolicy Bypass -File "%~dp0test-filesystem.ps1" ntfs
pause
