@echo off
:: run-bsod-test.bat -- Trigger deliberate BSOD to test panic screen rendering
:: Requires: build with -DBSOD_TEST in CFLAGS (uncomment in Makefile)
:: The kernel will panic at boot_desktop.c with "BSOD_TEST: Deliberate panic"
:: After crash, reboot to verify crash recovery log on serial output.
echo.
echo  WARNING: This triggers a deliberate kernel panic (BSOD).
echo  Build with -DBSOD_TEST first, then run this.
echo.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx
pause
