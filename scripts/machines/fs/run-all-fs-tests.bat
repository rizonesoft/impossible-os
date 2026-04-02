@echo off
:: run-all-fs-tests.bat — Run all filesystem tests sequentially
::
:: Tests each filesystem type one at a time. You'll need to close
:: QEMU (or let the OS shut down) between each test.

echo ==============================
echo  Impossible OS — FS Test Suite
echo ==============================
echo.

echo [1/5] Testing NTFS...
powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-fs-test.ps1" -Disk ntfs
echo.

echo [2/5] Testing FAT32...
powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-fs-test.ps1" -Disk fat32
echo.

echo [3/5] Testing ext2...
powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-fs-test.ps1" -Disk ext2
echo.

echo [4/5] Testing ext4...
powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-fs-test.ps1" -Disk ext4
echo.

echo [5/5] Testing IXFS...
powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-fs-test.ps1" -Disk ixfs
echo.

echo ==============================
echo  All filesystem tests complete
echo ==============================
pause
