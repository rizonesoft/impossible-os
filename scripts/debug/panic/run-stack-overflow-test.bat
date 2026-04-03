@echo off
:: run-stack-overflow-test.bat -- Test stack overflow panic (Double Fault)
:: Requires: build with -DSTACK_OVERFLOW_TEST in CFLAGS
:: The kernel will recurse until the stack overflows, triggering #DF.
:: Verifies: IST-based Double Fault handler, panic screen on alternate stack.
echo.
echo  WARNING: This triggers a stack overflow (Double Fault) panic.
echo  Build with -DSTACK_OVERFLOW_TEST first, then run this.
echo.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx
pause
