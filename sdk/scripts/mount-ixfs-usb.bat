@echo off
REM mount-ixfs-usb.bat -- Detect USB drives with IXFS partitions and mount them.
REM
REM Usage:
REM   mount-ixfs-usb.bat              Auto-detect and mount
REM
REM Unmount: unmount-ixfs.bat

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0mount-ixfs-usb.ps1" %*
pause
