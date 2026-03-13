@echo off
REM Test 11: NTFS
powershell -ExecutionPolicy Bypass -File "%~dp0test-fs.ps1" ntfs
