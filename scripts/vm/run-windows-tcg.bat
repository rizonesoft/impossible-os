@echo off
:: run-windows-tcg.bat — Launch Impossible OS in QEMU TCG (software emulation)
::
:: Forces TCG mode: CPUID reports "TCGTCGTCGTCG", UTS selects PIT timer.
:: Use this to validate the PIT timer path on Windows.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-windows.ps1" -Accel tcg
pause
