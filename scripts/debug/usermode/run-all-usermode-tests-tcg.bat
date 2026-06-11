@echo off
pushd "%~dp0"
:: run-all-usermode-tests-tcg.bat -- Boot once under TCG, run every user-mode test_*.exe
::
:: TCG mirror of run-all-usermode-tests.bat. Use when WHPX is unavailable
:: (GitHub Actions shared runners, CI on hosts without KVM / WHPX), or
:: when a user-mode regression reproduces only on the TCG emulator path.
::
:: -TestOnly + -NoKernelTests + (no -UtestFilter) makes the launcher walk
:: every test_*.exe deployed at C:\ via the manifest (smoke phase first,
:: then correctness / stress / perf) WITHOUT re-running the kernel
:: TEST_CAT_* sweep first. -Accel tcg forces software emulation.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel tcg -TestOnly -NoKernelTests
popd
pause
