# scripts\debug\usermode -- User-mode test runners

Windows-side launchers that boot QEMU and run the user-mode `test_*.exe`
binaries WITHOUT also running the full kernel `TEST_CAT_*` sweep.

## What is here

- `run-test_<name>.bat` -- one per user-mode test binary. Boots with
  `-TestOnly -NoKernelTests -UtestFilter "test_<name>.exe"`, so the scenario
  launcher spawns only that binary.
- `run-all-usermode-tests.bat` -- the full user-mode sweep: `-TestOnly
  -NoKernelTests` with no `-UtestFilter`, so the launcher walks every
  `test_*.exe` deployed at `C:\` via the deployment manifest (smoke phase
  first, then correctness / stress / perf). Propagates the run's exit code.
- `run-all-usermode-tests-tcg.bat` -- the same sweep under TCG instead of WHPX.

Kernel-side `TEST_CAT_*` suites are a separate surface:
[`scripts\debug\kernel\run-all-kernel-tests.bat`](../kernel/run-all-kernel-tests.bat).

## How the isolation works

The kernel test launcher lives in
[`src/kernel/test/test_usermode.c`](../../../src/kernel/test/test_usermode.c).
Under `boot.conf test=1` the kernel test runner and the user-mode launcher are
two independently skippable phases, controlled by two boot.conf fields
(documented in [`docs/boot/boot-info-fields.md`](../../../docs/boot/boot-info-fields.md)):

| boot.conf field | Effect |
|---|---|
| `test_kernel_skip=1` | skip the kernel `TEST_CAT_*` sweep |
| `test_usermode_skip=1` | skip the user-mode launcher |
| `utest_filter=<name\|glob>` | launcher runs only the matching binaries |

[`scripts\machines\run-qemu.ps1`](../../machines/run-qemu.ps1) exposes these as
`-NoKernelTests`, `-NoUsermodeTests` and `-UtestFilter`, which is what every bat
in this directory passes.

## Adding a binary

A new `user/test/test_<name>.c` gets one bat here alongside its `Makefile`
`userland` row and its `tests/usermode.manifest` line. Copy an existing
`run-test_<name>.bat` and change the two names in it.

## Output

Each binary emits a framed `[UTEST] <name>: PASS|FAIL (exit=N)` record plus a
run summary on serial; the host-side reconciliation and artifact formats are
documented in
[`docs/testing/usermode-output-formats.md`](../../../docs/testing/usermode-output-formats.md).
