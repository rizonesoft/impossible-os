@echo off
:: run-ps2-keyboard-test-tcg.bat -- Test PS/2 keyboard input on TCG
:: TCG with forced i8042 -- verifies keyboard works under software emulation.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel tcg -ExtraArgs "-machine pc,i8042=on"
pause
