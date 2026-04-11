@echo off
:: run-error-screen-test.bat -- Test boot error screen + NVRAM persistence (TODO-02 §9/§13/§14)
::
:: Default: normal boot with NVRAM persistence check.
:: If a previous boot wrote BootError to NVRAM, serial should show:
::   "[BOOT] Previous boot failed: code=0xNNNN"
::
:: Uses a separate NVRAM file so BootError state persists across runs
:: without affecting other test scripts.
::
:: Usage:
::   run-error-screen-test.bat           -- normal boot (check NVRAM previous error)
::   run-error-screen-test.bat --clean   -- delete temp NVRAM, start fresh
::
:: Prerequisites:
::   - QEMU installed on Windows
::   - bash scripts/build.sh (builds system-disk.img)

setlocal enabledelayedexpansion

:: Avoid UNC path errors when launched from WSL filesystem
cd /d %TEMP%

:: Resolve paths relative to script location
set SCRIPTDIR=%~dp0
set BUILD=%SCRIPTDIR%..\..\build
set DISK=%BUILD%\system-disk.img
set NVRAM=%TEMP%\OVMF_VARS_error_test.fd

:: Parse arguments
if "%1"=="--clean" (
    echo [INFO] Cleaning up temporary NVRAM...
    if exist "%NVRAM%" del "%NVRAM%"
    echo [INFO] Done. Next run will start with fresh NVRAM.
    goto :done
)

if not exist "%DISK%" (
    echo [ERROR] System disk not found at %DISK%
    echo [ERROR] Run in WSL: bash scripts/build.sh
    pause & exit /b 1
)

:: Copy OVMF firmware if not present
set OVMF_CODE=%BUILD%\OVMF_CODE_4M.fd
if not exist "%OVMF_CODE%" (
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/impossible-os/build/ 2>/dev/null || cp /usr/share/qemu/OVMF_CODE.fd ~/impossible-os/build/OVMF_CODE_4M.fd 2>/dev/null"
)
if not exist "%OVMF_CODE%" (
    echo [ERROR] OVMF firmware not found. Install in WSL: sudo apt install ovmf
    pause & exit /b 1
)

:: Seed NVRAM on first run (preserves BootError across reboots)
set OVMF_VARS_SRC=%BUILD%\OVMF_VARS_4M.fd
if not exist "%OVMF_VARS_SRC%" (
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/impossible-os/build/ 2>/dev/null || cp /usr/share/qemu/OVMF_VARS.fd ~/impossible-os/build/OVMF_VARS_4M.fd 2>/dev/null"
)
if not exist "%NVRAM%" copy /Y "%OVMF_VARS_SRC%" "%NVRAM%" >nul

echo.
echo ========================================
echo  Impossible OS -- Error Screen Test
echo ========================================
echo  Boots with intact kernel and separate
echo  NVRAM.  Watch serial output for:
echo    "[BOOT] Previous boot failed: code=0x..."
echo  NVRAM: %NVRAM%
echo  --clean to reset NVRAM state
echo ========================================
echo.

:: Force DPI-unaware for consistent window size
reg add "HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" /v "C:\Program Files\qemu\qemu-system-x86_64.exe" /t REG_SZ /d "~ DPIUNAWARE" /f >nul 2>&1

set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
if not exist "%QEMU%" set QEMU=qemu-system-x86_64.exe

:: Try WHPX first, fall back to TCG (matches run-qemu.ps1 auto mode)
"%QEMU%" ^
    -accel whpx -accel tcg ^
    -cpu Haswell,+invtsc ^
    -smp 2 ^
    -m 512 ^
    -drive if=pflash,format=raw,readonly=on,file="%OVMF_CODE%" ^
    -drive if=pflash,format=raw,file="%NVRAM%" ^
    -drive format=raw,file="%DISK%" ^
    -serial stdio ^
    -display sdl ^
    -no-reboot ^
    -device isa-debug-exit,iobase=0x501,iosize=2

echo.
echo [INFO] QEMU exited. NVRAM state preserved at %NVRAM%
echo [INFO] To reset: run-error-screen-test.bat --clean

:done
pause
