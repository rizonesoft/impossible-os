@echo off
:: run-secureboot-verify.bat -- Boot with UEFI Secure Boot ENABLED
::
:: Uses OVMF Secure Boot firmware (OVMF_CODE_4M.ms.fd) with pre-enrolled
:: Microsoft keys. The MOK enrollment path allows our shim to chain-load.
::
:: What this verifies:
::   - Serial: [UEFI] Secure Boot: ENABLED (User Mode)
::   - Serial: [UEFI] Secure Boot keys: PK=enrolled, KEK=enrolled
::   - Serial: HKLM\SYSTEM\SecureBoot: State=1
::   - Boot tests: test_uefi_var_get_secureboot, test_secureboot_state,
::                 test_secureboot_db_mirror
::
:: First run: MOK enrollment popup will appear (MokManager). Enroll the
:: MOK certificate to allow our signed BOOTX64.EFI to boot.
::
:: Prerequisites:
::   - QEMU with WHPX acceleration (native Windows)
::   - ovmf package installed in WSL (apt install ovmf)
::   - Signed EFI: bash scripts/build.sh (with keys/MOK.key present)

echo ============================================================
echo  Impossible OS -- Secure Boot Verification (SB ENABLED)
echo ============================================================
echo.

:: Copy Secure Boot OVMF firmware from WSL if not present
set BUILD=%~dp0..\..\build
set SB_CODE=%BUILD%\OVMF_CODE_4M.secboot.fd
set SB_VARS=%BUILD%\OVMF_VARS_4M.ms.fd
set SB_VARS_TEMP=%TEMP%\OVMF_VARS_4M.secboot.fd

if not exist "%SB_CODE%" (
    echo [SB] Copying Secure Boot OVMF firmware from WSL...
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_CODE_4M.secboot.fd ~/impossible-os/build/"
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd ~/impossible-os/build/"
)

if not exist "%SB_CODE%" (
    echo [ERROR] Secure Boot OVMF not found. Install: sudo apt install ovmf
    pause
    exit /b 1
)

:: Fresh NVRAM copy each run (clean SB state -- forces MOK re-enrollment)
:: Remove the 'copy' line below to persist MOK enrollment across runs.
copy /Y "%SB_VARS%" "%SB_VARS_TEMP%" >nul

echo [SB] Using Secure Boot firmware: %SB_CODE%
echo [SB] NVRAM (fresh copy):         %SB_VARS_TEMP%
echo.

:: Find QEMU
set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
if not exist "%QEMU%" set QEMU=qemu-system-x86_64.exe

:: Find system disk
set DISK=%BUILD%\system-disk.img
if not exist "%DISK%" (
    echo [ERROR] System disk not found: %DISK%
    echo         Run: bash scripts/build.sh
    pause
    exit /b 1
)

echo [SB] Launching QEMU with Secure Boot...
echo.

"%QEMU%" ^
    -accel whpx ^
    -cpu host ^
    -smp 2 ^
    -m 256 ^
    -drive if=pflash,format=raw,readonly=on,file="%SB_CODE%" ^
    -drive if=pflash,format=raw,file="%SB_VARS_TEMP%" ^
    -drive format=raw,file="%DISK%" ^
    -serial stdio ^
    -display sdl ^
    -no-reboot ^
    -device isa-debug-exit,iobase=0x501,iosize=2

echo.
echo ============================================================
echo  Secure Boot verification complete. Check serial output for:
echo    [UEFI] Secure Boot: ENABLED (User Mode)
echo    [UEFI] Secure Boot keys: PK=enrolled, KEK=enrolled
echo    HKLM\SYSTEM\SecureBoot: State=1
echo    DeployedMode=1, SetupMode=0
echo ============================================================
echo.
pause
