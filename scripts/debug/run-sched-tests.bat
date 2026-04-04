@echo off
:: run-sched-tests.bat -- Scheduler tests (threads, mutex)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite sched -SerialLog
pause
