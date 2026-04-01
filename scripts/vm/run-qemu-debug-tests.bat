@echo off
:: run-qemu-debug-tests.bat — Boot with debug=1 (unit tests + boot tests)
::
:: Runs kernel unit tests (PMM, heap, VFS, sched, registry) followed by
:: boot integration tests (threading, IPC, mutex, semaphore, pipes).
:: Results visible in serial output. OS continues to desktop after tests.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-qemu.ps1" -Accel whpx -DebugTests
pause
