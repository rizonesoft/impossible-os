@echo off
REM test-fs-all.bat — List all available test disks
REM
REM Since VirtualBox can only attach one test disk at a time,
REM this script lists all available disks so you can pick one
REM to double-click from the numbered .bat files above.
REM
REM  01 - DVD ISO 9660       07 - exFAT
REM  02 - DVD Joliet         08 - ext2
REM  03 - DVD UDF 1.02       09 - ext3
REM  04 - DVD UDF 2.50       10 - ext4
REM  05 - DVD Mixed          11 - NTFS
REM  06 - FAT32              12 - IXFS
REM
echo.
echo ===== Filesystem Test Disks =====
echo.
echo  Optical (DVD):            Block Devices (AHCI):
echo  01 - ISO 9660             06 - FAT32
echo  02 - Joliet               07 - exFAT
echo  03 - UDF 1.02             08 - ext2
echo  04 - UDF 2.50             09 - ext3
echo  05 - Mixed ISO+UDF        10 - ext4
echo                             11 - NTFS
echo                             12 - IXFS
echo.
echo  Double-click a numbered .bat file to launch that test.
echo  Generate test disks first in WSL2: bash scripts/test-filesystem.sh gen
echo.
powershell -ExecutionPolicy Bypass -File "%~dp0test-filesystem.ps1" list
pause
