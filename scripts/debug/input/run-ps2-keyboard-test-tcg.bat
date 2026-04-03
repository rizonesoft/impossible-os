@echo off
:: run-ps2-keyboard-test-tcg.bat -- Test PS/2 keyboard input on TCG
:: Kernel probes i8042 directly, works regardless of FADT.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel tcg
pause
