@echo off
REM Test 3: UDF 1.02 (DVD data disc)
powershell -ExecutionPolicy Bypass -File "%~dp0test-filesystem.ps1" optical/udf
pause
