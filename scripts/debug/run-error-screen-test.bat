@echo off
:: run-error-screen-test.bat -- Test boot error screen with QR code (TODO-02 §9/§13/§14)
::
:: Creates a temporary disk image with the kernel removed from the ESP,
:: then boots QEMU. The bootloader should show:
::   - Blue BSOD-style error screen with "Kernel load failed"
::   - Error code: 0x0003 (BOOT_ERR_KERNEL_NOT_FOUND)
::   - QR code in bottom-right linking to https://impossibleos.co/err/0003
::   - Recovery instructions
::   - 10-second timeout then cold reboot (QEMU exits due to -no-reboot)
::
:: The NVRAM variable "BootError" is written with 0x0003.
:: Run this script a SECOND time to verify the next boot shows:
::   "[BOOT] Previous boot failed: code=0x0003" on serial
::
:: Prerequisites:
::   - QEMU installed on Windows
::   - bash scripts/build.sh (builds system-disk.img)
::
:: Variants: change BREAK_MODE to test different error codes:
::   kernel  -- remove kernel.exe (0x0003, default)
::   mmap    -- (future) corrupt memory map
::   elf     -- (future) truncate kernel to test ELF validation

setlocal enabledelayedexpansion

set BUILD=%~dp0..\..\build
set DISK_SRC=%BUILD%\system-disk.img
set DISK_TMP=%TEMP%\impossible-error-test.img
set NVRAM=%TEMP%\OVMF_VARS_error_test.fd
set BREAK_MODE=kernel

:: Parse arguments
if "%1"=="--clean" (
    echo [INFO] Cleaning up temporary files...
    if exist "%DISK_TMP%" del "%DISK_TMP%"
    if exist "%NVRAM%" del "%NVRAM%"
    echo [INFO] Done. Next run will start fresh.
    goto :done
)
if "%1"=="--nvram-test" (
    echo [INFO] NVRAM persistence test -- booting with INTACT kernel
    echo [INFO] If previous run triggered an error, serial should show:
    echo [INFO]   "[BOOT] Previous boot failed: code=0x0003"
    set BREAK_MODE=none
)

if not exist "%DISK_SRC%" (
    echo [ERROR] System disk not found at %DISK_SRC%
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

:: Seed NVRAM on first run (preserves BootError across reboots for NVRAM test)
set OVMF_VARS_SRC=%BUILD%\OVMF_VARS_4M.fd
if not exist "%OVMF_VARS_SRC%" (
    wsl.exe bash -c "cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/impossible-os/build/ 2>/dev/null || cp /usr/share/qemu/OVMF_VARS.fd ~/impossible-os/build/OVMF_VARS_4M.fd 2>/dev/null"
)
if not exist "%NVRAM%" copy /Y "%OVMF_VARS_SRC%" "%NVRAM%" >nul

:: Prepare disk image
if "%BREAK_MODE%"=="none" (
    echo [INFO] Using original disk image (kernel intact)
    set DISK=%DISK_SRC%
    goto :boot
)

echo [INFO] Creating temporary disk image...
copy /Y "%DISK_SRC%" "%DISK_TMP%" >nul

if "%BREAK_MODE%"=="kernel" (
    echo [INFO] Removing kernel from ESP to trigger BOOT_ERR_KERNEL_NOT_FOUND...
    :: Use WSL to mount the ESP partition and delete the kernel
    :: The ESP starts at sector 2048 (offset 1048576) in the GPT image
    wsl.exe bash -c "TMPDIR=$(mktemp -d) && sudo mount -o loop,offset=1048576 '%DISK_TMP%' \"$TMPDIR\" 2>/dev/null && sudo rm -f \"$TMPDIR/boot/kernel.exe\" && sudo umount \"$TMPDIR\" && rmdir \"$TMPDIR\" && echo '[OK] Kernel removed from ESP'"
    if errorlevel 1 (
        echo [WARN] WSL mount failed -- trying mtools fallback...
        wsl.exe bash -c "mdir -i '%DISK_TMP%@@1048576' ::/boot/ 2>/dev/null && mdel -i '%DISK_TMP%@@1048576' ::/boot/kernel.exe 2>/dev/null && echo '[OK] Kernel removed via mtools'"
        if errorlevel 1 (
            echo [ERROR] Could not remove kernel from disk image.
            echo [ERROR] Manual test: copy system-disk.img, mount ESP, delete boot\kernel.exe
            pause & exit /b 1
        )
    )
    set DISK=%DISK_TMP%
)

:boot
echo.
echo ========================================
echo  Impossible OS -- Error Screen Test
echo ========================================
if "%BREAK_MODE%"=="kernel" (
    echo  Mode: kernel removed (expect BSOD + QR)
    echo  Error code: 0x0003
    echo  QR URL: https://impossibleos.co/err/0003
) else (
    echo  Mode: normal boot (NVRAM persistence check)
    echo  Expect serial: "Previous boot failed: code=0x0003"
)
echo ========================================
echo.

:: Force DPI-unaware for consistent window size
reg add "HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" /v "C:\Program Files\qemu\qemu-system-x86_64.exe" /t REG_SZ /d "~ DPIUNAWARE" /f >nul 2>&1

set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
if not exist "%QEMU%" set QEMU=qemu-system-x86_64.exe

"%QEMU%" ^
    -accel whpx,kernel-irqchip=off ^
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
echo [INFO] To test NVRAM persistence: run-error-screen-test.bat --nvram-test
echo [INFO] To clean up: run-error-screen-test.bat --clean

:done
pause
