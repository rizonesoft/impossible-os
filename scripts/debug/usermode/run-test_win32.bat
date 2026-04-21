@echo off
:: run-test_win32.bat -- §14 Win32 API binary (test_win32.exe)
::
:: Exercises the Win32 shim (user/lib/win32.c) from ring 3:
::   - GetCurrentProcessId  (TEB.ClientId.UniqueProcess via gs:0x40)
::   - GetTickCount         (KUSER_SHARED_DATA @ 0x7FFE0000)
::   - CreateFileA          (NtOpenFile via SYSCALL fast path)
::   - ReadFile             (NtReadFile via SYSCALL)
::   - CloseHandle          (NtClose via SYSCALL; double-close test)
:: -NoKernelTests skips the kernel TEST_CAT_* sweep so usermode
:: iteration is fast.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_win32.exe"
pause
