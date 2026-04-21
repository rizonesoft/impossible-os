@echo off
:: run-test_fileio.bat -- §13 file-I/O binary (test_fileio.exe)
::
:: Exercises SYS_OPENFILE + SYS_READHANDLE + SYS_CLOSEHANDLE +
:: SYS_OPENDIROBJ + SYS_QUERYDIROBJ from ring 3:
::   - happy path open/read/content-match/close
::   - post-close read returns < 0 (handle invalidation)
::   - open nonexistent file returns INVALID_HANDLE_VALUE
::   - OB root enumeration returns >= 1 named entry
:: -NoKernelTests skips the kernel TEST_CAT_* sweep so usermode
:: iteration is fast.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_fileio.exe"
pause
