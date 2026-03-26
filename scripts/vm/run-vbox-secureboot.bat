@echo off
:: run-vbox-secureboot.bat -- Test Secure Boot chain in VirtualBox
::
:: What this tests:
::   1. Shim (shimx64.efi) loads via UEFI firmware
::   2. Shim verifies BOOTX64.EFI (grubx64.efi) against embedded VENDOR_CERT (MOK.cer)
::   3. MOK-signed bootloader chainloads and boots Impossible OS
::
:: The normal system-disk.img already carries the shim chain:
::   EFI\BOOT\BOOTX64.EFI  = shimx64.efi (shim, with MOK.cer embedded)
::   EFI\BOOT\grubx64.efi  = our bootloader, signed with MOK.key
::   EFI\BOOT\mmx64.efi    = MokManager (key enrollment UI, used when MOK not enrolled)
::
:: VirtualBox 7.0+ Secure Boot note:
::   If VBoxManage supports --uefi-secureboot-enabled, this script enables SB enforcement.
::   With SB on and no MS signature on the shim, VirtualBox will reject BOOTX64.EFI.
::   To pass: enroll keys/MOK.der via the EFI shell (see docs/guides/secure-boot-keys.md)
::   or set the VM to Setup Mode first.
::
:: See docs/guides/secure-boot-keys.md for the full signing setup.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-vbox-secureboot.ps1"
pause
