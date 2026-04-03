@echo off
:: run-crash-recovery-test.bat -- Verify crash log recovery after a panic
::
:: Step 1: Run the BSOD test (requires -DBSOD_TEST build)
:: Step 2: Close QEMU after BSOD renders
:: Step 3: Run this script -- boots normally and checks for [CRASH-PREV] entries
::
:: On warm reboot (ACPI reset), physical memory is preserved and the crash
:: log region survives. On cold reboot (VM restart), memory is zeroed and
:: recovery will not find valid data -- this is expected behavior.
::
:: For QEMU: the VM restarts from scratch, so crash recovery only works if
:: the crash region address was saved to NVRAM and the physical pages happen
:: to not be zeroed by firmware. Best tested on bare metal or VirtualBox
:: with "Reset" instead of power-off.
echo.
echo  Run this AFTER a crash (BSOD test or real panic).
echo  Check serial output for [CRASH-PREV] entries.
echo.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx
pause
