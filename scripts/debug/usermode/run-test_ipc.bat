@echo off
pushd "%~dp0"
:: run-test_ipc.bat -- IPC coverage binary (test_ipc.exe)
::
:: Exercises SYS_PIPE + SYS_SHMEM_CREATE + SYS_SHMEM_MAP with a full
:: write/read round-trip through each channel. -NoKernelTests skips
:: the kernel TEST_CAT_* sweep so usermode iteration is fast.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_ipc.exe"
popd
pause
