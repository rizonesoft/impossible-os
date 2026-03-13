@echo off
REM Test 5: ISO 9660 + UDF bridge (mixed optical)
powershell -ExecutionPolicy Bypass -File "%~dp0test-filesystem.ps1" optical/mixed
pause
