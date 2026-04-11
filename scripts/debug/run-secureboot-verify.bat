@echo off
:: run-secureboot-verify.bat -- Boot with UEFI Secure Boot firmware
::
:: Two modes:
::   Mode 1 (default): Setup Mode -- SB firmware, no keys enrolled.
::     SecureBoot=0, SetupMode=1. Tests SB detection + registry writes.
::     Our BOOTX64.EFI boots directly (no shim needed in Setup Mode).
::
::   Mode 2 (ENROLL=1): Enrolled Mode -- MS keys + our MOK.
::     Uses shim chain-load. First run shows MokManager for enrollment.
::     After enrollment: SecureBoot=1, SetupMode=0, PK=enrolled.
::
:: Usage:
::   run-secureboot-verify.bat           -- Setup Mode (default)
::   set ENROLL=1 && run-secureboot-verify.bat  -- Enrolled Mode
::
:: Prerequisites:
::   - QEMU + WHPX (native Windows)
::   - ovmf package in WSL (apt install ovmf)
::   - bash scripts/build.sh

echo ============================================================
echo  Impossible OS -- Secure Boot Verification
echo ============================================================
echo.

set BUILD=%~dp0..\..\build
set SB_CODE=%BUILD%\OVMF_CODE_4M.secboot.fd
set DISK=%BUILD%\system-disk.img

:: Copy Secure Boot OVMF firmware from WSL if not present
if not exist "%SB_CODE%" (
    echo [SB] Copying Secure Boot OVMF firmware from WSL...
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_CODE_4M.secboot.fd ~/impossible-os/build/"
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/impossible-os/build/"
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd ~/impossible-os/build/"
)

if not exist "%SB_CODE%" (
    echo [ERROR] Secure Boot OVMF not found. Install in WSL: sudo apt install ovmf
    pause
    exit /b 1
)

if not exist "%DISK%" (
    echo [ERROR] System disk not found: %DISK%
    echo         Run in WSL: bash scripts/build.sh
    pause
    exit /b 1
)

:: Select NVRAM based on ENROLL flag
if "%ENROLL%"=="1" (
    echo [SB] Mode: Enrolled -- MS keys + shim chain-load
    echo [SB] First run will show MokManager for MOK enrollment.
    set SB_VARS_SRC=%BUILD%\OVMF_VARS_4M.ms.fd
    set SB_VARS_TEMP=%TEMP%\OVMF_VARS_4M.secboot.enrolled.fd
) else (
    echo [SB] Mode: Setup -- SB firmware, no keys enrolled
    echo [SB] SecureBoot=0, SetupMode=1 -- direct boot, no shim needed
    set SB_VARS_SRC=%BUILD%\OVMF_VARS_4M.fd
    set SB_VARS_TEMP=%TEMP%\OVMF_VARS_4M.secboot.setup.fd
)

:: Copy NVRAM -- enrolled mode preserves across runs, setup mode resets each time
if "%ENROLL%"=="1" (
    if not exist "%SB_VARS_TEMP%" copy /Y "%SB_VARS_SRC%" "%SB_VARS_TEMP%" >nul
) else (
    copy /Y "%SB_VARS_SRC%" "%SB_VARS_TEMP%" >nul
)

echo [SB] Firmware: %SB_CODE%
echo [SB] NVRAM:    %SB_VARS_TEMP%
echo.

:: Find QEMU
set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
if not exist "%QEMU%" set QEMU=qemu-system-x86_64.exe

echo [SB] Launching QEMU with Secure Boot firmware...
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
echo  Check serial output for:
if "%ENROLL%"=="1" (
    echo    [UEFI] Secure Boot: ENABLED ^(User Mode^)
    echo    [UEFI] Secure Boot keys: PK=enrolled, KEK=enrolled
    echo    HKLM\SYSTEM\SecureBoot: State=1
) else (
    echo    [UEFI] Secure Boot: DISABLED ^(Setup Mode^)
    echo    [UEFI] Secure Boot keys: PK=absent, KEK=absent
    echo    HKLM\SYSTEM\SecureBoot: State=0, SetupMode=1
)
echo ============================================================
echo.
pause
