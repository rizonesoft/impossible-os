# Unit-test wiring -- runner notes, bat files, and the live-boot ban

Read on demand from `implement-todo-section` step 8. The step text in SKILL.md carries the mandatory action (read the
TODO's Unit Tests section, create the test file / registration function, wire the category, confirm the build). This
file carries the runner-note shapes, the bat-file layout rules, and the two test-content policies.

> **CRITICAL (incident 2026-04-12): Every section MUST have at least one unit test wired.** Multiple sections (TODO-03
> section 1 through 12, TODO-17 section 1 through 3) shipped without tests, requiring after-the-fact test creation.
> Tests catch real bugs -- the TODO-19 section 1 review found 3 critical FPU context-switch bugs that unit tests would
> have caught earlier. If the section has no testable surface (pure bootloader UEFI code with no kernel-side fields),
> document why in the TODO section with `**Note:** No kernel test surface -- validation via serial log on WHPX.`

## The runner note (one line, after the Test checkpoint paragraph, before the stamps)

The bat lives in the **subdir matching the test layer** (split 2026-04-20). Do NOT split the note across multiple
blockquote lines.

- **Kernel TEST_CAT_* suite** -- `scripts\debug\kernel\run-<cat>-tests.bat`:
  ```
  > **Test runner:** `scripts\debug\kernel\run-<category>-tests.bat` (SUITE=<cat>) | N suites, 0 failures
  ```
- **User-mode `test_*.exe` binary** -- `scripts\debug\usermode\run-<binary>.bat` (each one passes
  `utest_filter=<binary>` so only that binary runs):
  ```
  > **Test runner:** `scripts\debug\usermode\run-<binary>.bat` (utest_filter=<binary>) | N suites, 0 failures
  ```
- **Desktop UI test** -- `scripts\debug\desktop\run-<test>.bat`:
  ```
  > **Test runner:** `scripts\debug\desktop\run-<test>.bat` | N suites, 0 failures
  ```
- **No test surface** (pure UEFI boot code, docs-only):
  ```
  > **Test runner:** N/A (<reason>) | validation: <how-verified>
  ```

**Forbidden:** putting a per-category bat at the `scripts/debug/` root. That location is reserved for
`run-all-tests.bat` (the cross-layer aggregate) ONLY; everything else lives in a kernel/usermode/desktop subdir.

## Creating a missing bat

**If the matching bat does NOT exist on disk yet:** create it before committing the section. Mirror an existing bat's
one-liner shape:

```bat
@echo off
:: run-<name>-tests.bat -- <one-line description>
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite <cat>
pause
```

The `%~dp0..\..\machines\run-qemu.ps1` relative path is correct for any of the three subdirs.

**Aggregate runner per subdir:** if creating a NEW per-category bat AND its subdir's `run-all-<layer>-tests.bat`
aggregate is missing, create that too (`kernel/run-all-kernel-tests.bat`, `usermode/run-all-usermode-tests.bat`,
`desktop/run-all-desktop-tests.bat`). The root `scripts/debug/run-all-tests.bat` chains all three aggregates with
`if exist` so missing ones are silent no-ops; aggregates land when the subdir has at least one per-category bat.

## No tautological constant tests

A test like `TEST_ASSERT_EQ(POST16_FOO, 0xDF20, "POST16_FOO == 0xDF20")` only verifies that you typed `0xDF20` in the
`#define` -- the compiler already enforces that. The real protection is the boot-time uniqueness check that scans
`boot_init.h` for duplicate codes. Same goes for `TEST_ASSERT_EQ(SOME_DEFINE, expected_value)` where the assertion
just echoes the literal: skip it. Tests should exercise behavior, not re-state literals.

## HARD BAN: tests must NEVER call live boot infrastructure

WSL has no working QEMU, so runtime regressions in tests are not caught until the user boots on native Windows or bare
metal -- 3 incidents to date.

Forbidden in `src/kernel/test/test_*.c`: `boot_progress(`, `boot_post_write16(`, `boot_post_nvram_write16(`,
`post_display16(`, `vpd_stage_*(`, `vpd_init(`, `boot_splash_*(`, `boot_halt(`, `panic(`, `KeBugCheckEx(`, any
subsystem `_init(` (`pmm_init`, `vmm_init`, `heap_init`, `serial_init`, `klog_early_init`, `klog_disk_enable`,
`acpi_init`, `lapic_init`, `ioapic_init`, `timer_hal_init`, `gdt_init`, `idt_init`).

**Allowed alternatives:** pure constant checks, save/restore wrappers around `kernel_subsystem_set_ready`/`_ready`,
direct calls to PURE data helpers (`boot_timing_record_step()` is OK -- in-memory append only; `boot_progress()` is
NOT because it ALSO updates VPD/framebuffer), BOOT_REQUIRE/BOOT_STEP via wrapper functions, read-only oracle queries.

The pre-commit hook in `settings.json` enforces this -- a test file with forbidden calls cannot be committed. See
`feedback_test_no_live_boot_calls` memory, CLAUDE.md "Test Code Policy", and
[docs/infrastructure/test-policy.md](../../../../docs/infrastructure/test-policy.md).
