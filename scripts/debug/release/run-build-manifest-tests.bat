@echo off
:: run-build-manifest-tests.bat -- Windows-host PS1 manifest tooling tests
:: Owns: scripts/release/test-build-manifest.ps1 (26 assertions; bash peer
:: at scripts/release/test-build-manifest.sh runs 23 assertions on Linux).
::
:: pushd "%~dp0" auto-maps a UNC working dir (e.g. \\wsl.localhost\...)
:: to a temporary drive letter so CMD has a real CWD; without this the
:: bat falls back to C:\Windows when launched from \\wsl.localhost and
:: every relative path inside the spawned PS1 fails.
pushd "%~dp0"
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0..\..\release\test-build-manifest.ps1"
set RC=%errorlevel%
popd
if not defined NO_PAUSE pause
exit /b %RC%
