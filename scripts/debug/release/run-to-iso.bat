@echo off
:: run-to-iso.bat -- hybrid UEFI ISO producer (xorriso.exe, sha256-equal
:: to the Linux build-iso.sh peer with SOURCE_DATE_EPOCH=0). Owns:
:: scripts/release/to-iso.ps1. Reads build/release/disk.img, writes
:: build/release/disk.iso; manifest-to-ESP cross-check is fail-closed
:: via either bash+verify-esp.sh OR native PS+mtype.
:: pushd auto-maps UNC working dirs to a drive letter (see comment in
:: run-build-manifest-tests.bat).
pushd "%~dp0"
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0..\..\release\to-iso.ps1" %*
set RC=%errorlevel%
popd
if not defined NO_PAUSE pause
exit /b %RC%
