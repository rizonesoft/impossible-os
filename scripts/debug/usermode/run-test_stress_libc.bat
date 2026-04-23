@echo off
:: run-test_stress_libc.bat -- libc stress binary (test_stress_libc.exe)
::
:: In-binary 1000-iteration loop over strlen/strcmp/memcpy/memset;
:: classname=stress in XML + JSON output. -NoKernelTests skips the
:: kernel TEST_CAT_* sweep so the stress invocation iterates fast.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_stress_libc.exe"
pause
