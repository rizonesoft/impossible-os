@echo off
:: run-usb-hid-keyboard-test.bat -- Test USB HID keyboard via xHCI
:: Adds a USB keyboard device to QEMU. Requires xHCI driver + USB HID class driver.
:: Currently placeholder -- keyboard input will NOT work until USB HID is implemented.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -ExtraArgs "-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0"
pause
