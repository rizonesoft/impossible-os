@echo off
:: run-test_stress_libc.bat -- §8 stress binary (test_stress_libc.exe)
::
:: In-binary 1000-iteration loop over strlen/strcmp/memcpy/memset;
:: classname=stress in XML + JSON output. NOTE: test=1 also runs the
:: kernel TEST_CAT_* suites BEFORE the launcher; usermode-only
:: iteration would need a future `test_kernel=0` boot.conf switch.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -UtestFilter "test_stress_libc.exe"
pause
