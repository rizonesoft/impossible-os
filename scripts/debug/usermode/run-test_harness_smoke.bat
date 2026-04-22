@echo off
:: run-test_harness_smoke.bat -- §1 user-mode harness self-test (test_harness_smoke.exe)
::
:: Boots QEMU with test=1 + test_kernel_skip=1 + utest_filter=test_harness_smoke.exe
:: so the §3 launcher only spawns the UTEST-framework self-check binary
:: and the kernel TEST_CAT_* sweep is skipped. Exercises UTEST_BEGIN /
:: UTEST_ASSERT / UTEST_END macros end-to-end -- a failure here means the
:: user-mode test framework itself is broken, so run this FIRST before
:: investigating any other usermode test regression.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_harness_smoke.exe"
pause
