@echo off
pushd "%~dp0"
:: run-storage-tests.bat -- Storage driver tests (AHCI, VirtIO, NVMe)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite storage
popd
pause
