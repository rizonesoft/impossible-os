@echo off
:: run-null-deref-test.bat -- Test #PF (NULL pointer dereference) panic
:: Requires: build with -DNULL_DEREF_TEST in CFLAGS
:: The kernel will trigger a page fault by reading address 0x0.
:: Verifies: panic_screen renders, crash dump written, crash log persisted.
echo.
echo  WARNING: This triggers a NULL pointer dereference panic.
echo  Build with -DNULL_DEREF_TEST first, then run this.
echo.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx
pause
