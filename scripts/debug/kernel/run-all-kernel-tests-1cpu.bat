@echo off
pushd "%~dp0"
:: run-all-kernel-tests-1cpu.bat -- Run all tests with 1 CPU (no AP)
:: Bisect SMP bugs: if tests pass here but fail on 2 CPUs, it's a race.
:: -NoUsermodeTests keeps this kernel-only (matches run-all-kernel-tests.bat).
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -Smp 1 -TestOnly -NoUsermodeTests
popd
pause
