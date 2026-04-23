@echo off
:: run-test_fastpath_fuzz.bat -- -19 3-way transport fuzz (test_fastpath_fuzz.exe)
::
:: Runs 1000 iterations each of INT 0x80 / SYSCALL / INT 0x2E via
:: utest_filter=test_fastpath_fuzz.exe + -NoKernelTests. Any transport
:: divergence (single return value differing) fails the binary with the
:: specific iter + transport + observed value. Transition ring
:: captures the last 64 ring-3 crossings at the next panic for replay,
:: so a transport regression is debuggable without a repro.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_fastpath_fuzz.exe"
pause
