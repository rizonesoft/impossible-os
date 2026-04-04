@echo off
:: run-mm-tests.bat -- Memory Management tests (PMM, Heap, VMM, Swap, mmap)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite mm -SerialLog
pause
