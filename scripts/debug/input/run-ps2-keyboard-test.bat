@echo off
:: run-ps2-keyboard-test.bat -- Test PS/2 keyboard input via i8042
:: Forces i8042 controller in QEMU ACPI tables so PS/2 keyboard IRQ fires.
:: Without this, QEMU WHPX reports IAPC_BOOT_ARCH.8042=0 and keyboard is skipped.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -ExtraArgs "-machine pc,i8042=on"
pause
