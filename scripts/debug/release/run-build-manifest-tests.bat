@echo off
:: run-build-manifest-tests.bat -- Windows-host PS1 manifest tooling tests
:: Owns: scripts/release/test-build-manifest.ps1 (26 assertions; bash peer
:: at scripts/release/test-build-manifest.sh runs 23 assertions on Linux).
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0..\..\release\test-build-manifest.ps1"
set RC=%errorlevel%
if not defined NO_PAUSE pause
exit /b %RC%
