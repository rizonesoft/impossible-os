@echo off
:: run-secureboot-verify.bat -- Verify Secure Boot state detection and registry
::
:: Runs boot tests (SUITE=boot) which include:
::   - test_uefi_rt_available     (RT services preserved)
::   - test_uefi_var_get_secureboot (SecureBoot variable readable)
::   - test_secureboot_state       (boot_info matches uefi_secureboot_enabled)
::   - test_secureboot_db_mirror   (DbEntries/DbxEntries in registry)
::
:: Also checks serial output for Secure Boot detection lines:
::   [UEFI] Secure Boot: ENABLED/DISABLED
::   [SecureBoot] state=ENABLED/DISABLED
::   [SMBIOS] Registry populated
::
:: Usage:
::   Double-click or run from cmd.exe in the repo root.
::   Requires QEMU + WHPX (native Windows) or use run-all-tests-tcg.bat for WSL.

echo ============================================================
echo  Impossible OS -- Secure Boot Verification
echo ============================================================
echo.
echo Running boot test suite (includes SecureBoot tests)...
echo.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite boot

echo.
echo ============================================================
echo  Check serial output above for:
echo    [UEFI] Secure Boot: ENABLED or DISABLED
echo    [UEFI] Secure Boot keys: PK=enrolled/absent, KEK=enrolled/absent
echo    HKLM\SYSTEM\SecureBoot: State=0 or 1
echo ============================================================
echo.
pause
