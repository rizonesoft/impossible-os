@echo off
:: run-test_fastpath.bat -- -17 fast-path transport probe (test_fastpath.exe)
::
:: Runs the five isolated ring-0<->3 fast-path probes (gs:0x30, gs:0x40,
:: gs:0x60, KUSD @ 0x7FFE0000, syscall->SSDT_NtClose) via
:: utest_filter=test_fastpath.exe + -NoKernelTests so the only thing
:: QEMU boots for is the probe binary. Exit code is a bitmap of failing
:: probes (0 = all pass); the launcher surfaces the bitmap in its
:: UTEST: verdict line so a drifted probe names itself without
:: re-reading the serial log.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_fastpath.exe"
pause
