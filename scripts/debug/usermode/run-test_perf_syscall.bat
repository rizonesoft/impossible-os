@echo off
pushd "%~dp0"
:: run-test_perf_syscall.bat -- syscall perf binary (test_perf_syscall.exe)
::
:: RDTSC-based sys_yield latency; emits [PERF] sys_yield_ns=<n> line
:: scraped by XML/JSON output. -NoKernelTests skips the kernel
:: TEST_CAT_* sweep so the perf measurement is not preceded by the
:: full unit-test boot warm-up.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_perf_syscall.exe"
popd
pause
