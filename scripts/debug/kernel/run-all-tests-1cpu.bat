@echo off
:: run-all-tests-1cpu.bat -- Run all tests with 1 CPU (no AP)
:: Bisect SMP bugs: if tests pass here but fail on 2 CPUs, it's a race.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -Smp 1 -TestOnly
pause
