@echo off
:: run-qemu-kvm-secureboot.bat — Test Secure Boot chain in QEMU with WHPX acceleration
::
:: Boot chain (all signatures enforced by OVMF secboot firmware):
::   OVMF_CODE_4M.secboot.fd + OVMF_VARS_4M.snakeoil.fd (test keys pre-enrolled)
::     -> EFI\BOOT\BOOTX64.EFI  (shimx64.efi, signed with OVMF snakeoil key)
::     -> EFI\BOOT\grubx64.efi  (our bootloader, signed with MOK.key)
::     -> kernel
::
:: On first run: WSL2 rebuilds system-disk-secureboot.img and signs the shim.
:: No interactive BIOS key enrollment needed for this test path.
::
:: See docs/guides/secure-boot-keys.md for the full signing setup.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-qemu-secureboot.ps1" -Accel whpx
pause
