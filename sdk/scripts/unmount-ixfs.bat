@echo off
REM unmount-ixfs.bat -- Unmount an IXFS drive letter
REM
REM Usage:
REM   unmount-ixfs.bat I:

if "%~1"=="" (
    echo Usage: unmount-ixfs.bat I:
    pause
    exit /b 1
)

echo Unmounting %1...
taskkill /f /im ixfs-mount.exe >nul 2>&1
echo Done.
pause
