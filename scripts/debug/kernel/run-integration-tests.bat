@echo off
pushd "%~dp0"
:: run-integration-tests.bat -- Full debug boot: unit tests + IXFS perf + desktop
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -DebugTests
popd
pause
