@echo off
REM test-filesystem.bat — Launch filesystem test in VirtualBox
REM Usage: test-filesystem.bat fat32
REM        test-filesystem.bat optical/iso9660
powershell -ExecutionPolicy Bypass -File "%~dp0test-filesystem.ps1" %*
pause
