@echo off
:: run-test_process.bat -- §12 process-lifecycle binary (test_process.exe)
::
:: Exercises SYS_FORK + SYS_EXEC + SYS_WAITPID + SYS_KILL with three
:: child forks per run. -NoKernelTests skips the kernel TEST_CAT_*
:: sweep so usermode iteration is fast.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_process.exe"
pause
