@echo off
:: run-secureboot.bat -- Test Secure Boot with shim + MokManager + signed EFI
::
:: Boots QEMU with Secure Boot enabled (MS keys enrolled).
:: Uses TCG + q35 + SMM (WHPX cannot emulate secboot pflash).
::
:: First run: shim rejects unsigned grubx64.efi -> MokManager appears.
::   1. "Enroll key from disk" -> select MOK.cer from ESP root
::   2. Confirm enrollment -> reboot
:: Second run: shim accepts signed grubx64.efi -> OS boots with SecureBoot=1.
::
:: NVRAM persists in %TEMP% across runs (MOK enrollment survives reboot).
:: Delete %TEMP%\OVMF_VARS_secureboot.fd to reset enrollment.
::
:: Prerequisites:
::   - QEMU installed, ovmf in WSL (apt install ovmf)
::   - keys/MOK.key + keys/MOK.cer generated (see docs/guides/secure-boot-keys.md)
::   - bash scripts/build.sh (signs EFI + copies MOK.cer to ESP)

set BUILD=%~dp0..\..\..\build
set SB_CODE=%BUILD%\OVMF_CODE_4M.secboot.fd
set SB_VARS_SRC=%BUILD%\OVMF_VARS_4M.ms.fd
set SB_VARS=%TEMP%\OVMF_VARS_secureboot.fd
set DISK=%BUILD%\system-disk.img

:: Copy Secure Boot OVMF firmware from WSL if not present
if not exist "%SB_CODE%" (
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_CODE_4M.secboot.fd ~/impossible-os/build/"
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd ~/impossible-os/build/"
)
if not exist "%SB_CODE%" (
    echo [ERROR] Secure Boot OVMF not found. Install in WSL: sudo apt install ovmf
    pause & exit /b 1
)
if not exist "%DISK%" (
    echo [ERROR] System disk not found. Run in WSL: bash scripts/build.sh
    pause & exit /b 1
)

:: Seed NVRAM on first run only (preserves MOK enrollment across reboots)
if not exist "%SB_VARS%" copy /Y "%SB_VARS_SRC%" "%SB_VARS%" >nul

:: Force DPI-unaware so QEMU window stays consistent across monitors
reg add "HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" /v "C:\Program Files\qemu\qemu-system-x86_64.exe" /t REG_SZ /d "~ DPIUNAWARE" /f >nul 2>&1

set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
if not exist "%QEMU%" set QEMU=qemu-system-x86_64.exe

"%QEMU%" ^
    -accel tcg ^
    -machine q35,smm=on ^
    -global ICH9-LPC.disable_s3=1 ^
    -cpu qemu64 ^
    -smp 2 ^
    -m 512 ^
    -drive if=pflash,format=raw,readonly=on,file="%SB_CODE%" ^
    -drive if=pflash,format=raw,file="%SB_VARS%" ^
    -drive format=raw,file="%DISK%" ^
    -serial stdio ^
    -display sdl ^
    -no-reboot ^
    -device isa-debug-exit,iobase=0x501,iosize=2
pause
