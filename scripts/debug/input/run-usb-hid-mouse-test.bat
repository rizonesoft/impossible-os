@echo off
:: run-usb-hid-mouse-test.bat -- Test USB HID mouse via xHCI
:: Adds a USB mouse + tablet device to QEMU. Requires xHCI driver + USB HID class driver.
:: Currently placeholder -- USB mouse will NOT work until USB HID is implemented.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -ExtraArgs "-device qemu-xhci,id=xhci -device usb-mouse,bus=xhci.0"
pause
