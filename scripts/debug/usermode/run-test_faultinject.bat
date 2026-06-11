@echo off
pushd "%~dp0"
:: run-test_faultinject.bat -- fault-injection bridge probe (test_faultinject.exe)
::
:: Boots QEMU with test=1 + test_kernel_skip=1 + utest_filter=test_faultinject.exe.
:: The SYS_FAULT_INJECT typed kernel-allocator probe surface is exercised
:: only when this binary runs -- AppVerifier-equivalent injection points
:: that no other test reaches.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_faultinject.exe"
popd
pause
