@echo off
pushd "%~dp0"
:: run-ex-tests.bat -- Executive Support Runtime tests (rundown, callbacks, lookaside, fast-ref, AVL, ERESOURCE, work items, bugcheck, verifier)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite ex
popd
pause
