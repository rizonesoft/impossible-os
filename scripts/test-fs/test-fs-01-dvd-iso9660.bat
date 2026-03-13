@echo off
REM Test 1: ISO 9660 (classic CD-ROM filesystem)
powershell -ExecutionPolicy Bypass -File "%~dp0test-fs.ps1" optical/iso9660
pause
