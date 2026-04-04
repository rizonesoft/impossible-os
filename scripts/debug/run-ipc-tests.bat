@echo off
:: run-ipc-tests.bat -- IPC tests (pipes, shared memory, semaphores)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite ipc -SerialLog
pause
