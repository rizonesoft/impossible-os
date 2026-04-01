@echo off
:: run-qemu-1cpu.bat — Launch Impossible OS in QEMU with 1 CPU
::
:: Use this to bisect SMP bugs: if something crashes on 2 CPUs
:: but works here, it's a concurrency issue.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-qemu.ps1" -Accel whpx -Smp 1
pause
