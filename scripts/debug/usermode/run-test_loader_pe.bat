@echo off
pushd "%~dp0"
:: run-test_loader_pe.bat -- §15 binary-format loader coverage: PE32+
::
:: Runs only test_loader_pe.exe and exits. Binary is produced by
:: clang-19 --target=x86_64-pc-windows-msvc + lld-link (NOT the ELF
:: crt0+libc pipeline every other test binary uses). Launcher emits
:: `UTEST: test_loader_pe.exe: format=PE32+` before the PASS verdict
:: so a regression that silently routes this through the ELF loader
:: surfaces as `format=ELF`.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_loader_pe.exe"
popd
pause
