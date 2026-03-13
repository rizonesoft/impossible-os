@echo off
REM Test 6: FAT32
powershell -ExecutionPolicy Bypass -File "%~dp0test-filesystem.ps1" fat32
pause
