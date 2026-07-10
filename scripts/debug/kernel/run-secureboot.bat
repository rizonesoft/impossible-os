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
::
:: Planned UKI signed-payload cases (deferred to follow-up smoke
:: harness; tracked in TODO-02 UKI signed-payload-sections section
:: as a [ ] item):
::   uki_initrd_signed -- stage a synthetic build/uki-payloads/initrd.img
::     (1 page of deterministic content), rebuild via bash scripts/build.sh
::     so .initrd is embedded in BOOTX64.UKI.efi, boot the UKI artifact
::     directly (not the disk image), capture serial-stdio output, and
::     assert the line:
::         [BOOT] UKI: .initrd embedded size=4096 bytes
::     plus the boot_info publication invariant (uki_initrd_addr != 0,
::     uki_initrd_size == 4096) reflected by test_uki_initrd_section_size_consistent.
::   uki_initrd_disk_override_rejected -- ship a UKI whose .cmdline
::     contains one of the parser keys staged for disk loading
::     ("initrd=disk-payload.img", "module=foo.eif", or
::     "recovery_image=foo.img"), boot it, capture serial, and assert:
::         [FATAL] UKI mode rejects out-of-UKI <token> override; disk payload is unsigned
::     The boot must NOT reach C:\>; boot_fatal aborts before kernel
::     load. Validates the parse_boot_conf cmdline rejection path.
::     Token names match parse_conf_kv parser keys exactly -- module=
::     is singular, recovery_image= is the full key.
:: Both cases need a headless serial-to-file QEMU invocation pattern
:: that the current GUI-launcher shape does not implement; the harness
:: extension lands when a smoke-test driver script is added under
:: scripts/debug/.

set BUILD=%~dp0..\..\..\build
for %%I in ("%BUILD%") do set "BUILD=%%~fI"
set SB_CODE=%BUILD%\OVMF_CODE_4M.secboot.fd
set SB_VARS_SRC=%BUILD%\OVMF_VARS_4M.ms.fd
set SB_VARS=%TEMP%\OVMF_VARS_secureboot.fd
set DISK=%BUILD%\system-disk.img

:: WSL-side absolute path of build/ (derived from this script's location, so
:: it works wherever the repo lives). Computed outside the if-block because
:: cmd expands %VARS% in parenthesized blocks at parse time.
for /f "usebackq delims=" %%I in (`wsl.exe -e wslpath -a "%BUILD%"`) do set "WSL_BUILD=%%I"

:: Copy Secure Boot OVMF firmware from WSL if not present
if not exist "%SB_CODE%" (
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_CODE_4M.secboot.fd '%WSL_BUILD%/'"
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd '%WSL_BUILD%/'"
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
