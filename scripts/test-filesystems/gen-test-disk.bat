@echo off
REM gen-test-disk.bat -- Generate test disk images via WSL2
REM Usage: gen-test-disk.bat              (all disks)
REM        gen-test-disk.bat fat32        (single disk)
powershell -ExecutionPolicy Bypass -File "%~dp0gen-test-disk.ps1" %*
