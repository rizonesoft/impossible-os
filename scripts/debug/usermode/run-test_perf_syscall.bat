@echo off
:: run-test_perf_syscall.bat -- §8 perf binary (test_perf_syscall.exe)
::
:: RDTSC-based sys_yield latency; emits [PERF] sys_yield_ns=<n> line
:: scraped by XML/JSON output. NOTE: test=1 also runs the kernel
:: TEST_CAT_* suites BEFORE the launcher; usermode-only iteration
:: would need a future `test_kernel=0` boot.conf switch.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -UtestFilter "test_perf_syscall.exe"
pause
