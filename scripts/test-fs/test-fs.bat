@echo off
REM test-fs.bat — Launch filesystem test in VirtualBox
REM Usage: test-fs.bat fat32
REM        test-fs.bat optical/iso9660
powershell -ExecutionPolicy Bypass -File "%~dp0test-fs.ps1" %*
pause
