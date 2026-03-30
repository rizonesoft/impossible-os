@echo off
REM unmount-ixfs.bat -- Unmount IXFS drive (kills ixfs-mount.exe and cleans up)
REM
REM Usage:
REM   unmount-ixfs.bat

echo Unmounting IXFS...
taskkill /f /im ixfs-mount.exe >nul 2>&1
if exist "%TEMP%\ixfs-usb-partition.img" del /q "%TEMP%\ixfs-usb-partition.img" >nul 2>&1
echo Done.
pause
