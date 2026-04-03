@echo off
:: run-divide-by-zero-test.bat -- Test #DE (Divide Error) panic
:: Requires: build with -DDIV_ZERO_TEST in CFLAGS
:: The kernel will execute a division by zero instruction.
:: Verifies: #DE exception routing, panic_screen with correct stop code.
echo.
echo  WARNING: This triggers a divide-by-zero panic.
echo  Build with -DDIV_ZERO_TEST first, then run this.
echo.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx
pause
