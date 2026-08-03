@echo off
pushd "%~dp0"
:: run-test_forge.bat -- launcher-record forgery adversary (test_forge.exe)
::
:: Boots QEMU with test=1 + test_kernel_skip=1 + utest_filter=test_forge.exe
:: so the scenario launcher only spawns this one user-mode binary and the
:: kernel TEST_CAT_* sweep is skipped.
::
:: The binary itself PASSES: it prints launcher-shaped lines on fd 1 so the
:: HOST framing gates are asserted against real adversarial input. A run that
:: goes green here means those forged records were correctly refused.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_forge.exe"
popd
pause
