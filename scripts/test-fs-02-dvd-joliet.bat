@echo off
REM Test 2: ISO 9660 + Joliet (long Unicode filenames)
powershell -ExecutionPolicy Bypass -File "%~dp0test-fs.ps1" optical/joliet
