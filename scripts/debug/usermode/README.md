# scripts\debug\usermode -- User-mode test runners

Empty placeholder for per-binary user-mode test runners.

The user-mode test framework is owned by the [user-mode test framework
TODO](../../../todo/00-infrastructure/TODO-04-usermode-test-framework.md).
When the kernel test launcher (Kernel Test Launcher section), the
`utest_filter=` boot.conf parameter (Launcher Manifest section), and
the build integration (Build Integration section) ship, per-binary
bat files land here -- one per `test_*.exe` plus a
`run-all-usermode-tests.bat` aggregate that runs every test_*.exe
binary.

Each per-binary bat is a one-liner: it boots QEMU with
`utest_filter=<name>` so only the matching binary runs:

```bat
:: scripts\debug\usermode\run-test_syscall.bat (example, future)
powershell.exe -ExecutionPolicy Bypass -File ^
    "%~dp0..\..\machines\run-qemu.ps1" ^
    -Accel whpx -TestOnly -BootArg "utest_filter=test_syscall.exe"
pause
```

The root-level [`scripts\debug\run-all-tests.bat`](../run-all-tests.bat)
already invokes this directory (currently a no-op, since it is empty);
once the bat files exist, it will pick them up automatically.
