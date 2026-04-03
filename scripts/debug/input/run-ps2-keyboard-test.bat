@echo off
:: run-ps2-keyboard-test.bat -- Test PS/2 keyboard input on WHPX
:: Kernel now probes i8042 port 0x64 directly, ignoring FADT 8042 flag.
:: Keyboard should work even though QEMU WHPX reports IAPC_BOOT_ARCH.8042=0.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx
pause
