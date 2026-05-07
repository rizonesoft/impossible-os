@echo off
:: run-to-vhdx.bat -- VHDX converter (qemu-img.exe convert, byte-content
:: parity with the Linux peer). Owns: scripts/release/to-vhdx.ps1.
:: Reads build/release/disk.img, writes build/release/disk.vhdx; runs
:: qemu-img info + qemu-img compare for self-verification.
:: pushd auto-maps UNC working dirs to a drive letter (see comment in
:: run-build-manifest-tests.bat).
pushd "%~dp0"
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0..\..\release\to-vhdx.ps1" %*
set RC=%errorlevel%
popd
if not defined NO_PAUSE pause
exit /b %RC%
