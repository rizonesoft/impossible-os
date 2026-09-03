@echo off
pushd "%~dp0"
:: run-desktop-tests.bat -- Desktop compositor / controls tests (TEST_CAT_DESKTOP)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite desktop
popd
pause
