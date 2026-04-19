@echo off
:: run-error-screen-test.bat -- Trigger boot error screen with QR code
::
:: Sets error_screen_test=1 in boot.conf, which makes the bootloader
:: call boot_fatal() before loading the kernel. Shows:
::   - Blue BSOD-style error screen with "Error screen test"
::   - Error code: 0x0003 (BOOT_ERR_KERNEL_NOT_FOUND)
::   - QR code in bottom-right linking to https://impossibleos.co/err/0003
::   - 10-second timeout then cold reboot (QEMU exits with -no-reboot)
::
:: NVRAM BootError variable is written with 0x0003. On the NEXT normal
:: boot, serial should show "[BOOT] Previous boot failed: code=0x0003".
::
:: Prerequisites:
::   - QEMU installed on Windows
::   - bash scripts/build.sh (builds system-disk.img)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -ErrorScreenTest
pause
