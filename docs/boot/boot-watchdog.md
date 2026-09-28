<!-- docs: covers=todo/01-boot-platform/TODO-23-boot-watchdog.md sources=src/kernel/drivers/watchdog.c,include/kernel/drivers/watchdog.h,src/kernel/test/test_watchdog.c,src/kernel/main/boot_progress.c,src/kernel/main/boot_init.c,src/kernel/main/boot_interrupts.c,src/kernel/main/boot_confirm.c,src/kernel/main/boot_desktop.c reviewed=2026-09-28 order=23 -->
# Boot Watchdog

## What is it?

The boot watchdog is a hardware timer that reboots the machine if boot stalls before the desktop. Today that means the ACPI WDAT (Watchdog Action Table) path: the kernel finds a firmware-described watchdog, validates it field by field, arms it with one generous whole-boot timeout, and disarms it, with a readback check, at the handoff to user mode. There is no software watchdog yet, so a machine without a usable WDAT, which includes every QEMU configuration this repo tests with, has no hang coverage during boot.

The roadmap planned two layers: a LAPIC NMI software timer that can catch a hang even with interrupts disabled, backed by the hardware watchdog for total lockups. Only the hardware layer shipped. The NMI layer is parked because arming it before the kernel has per-CPU nested-NMI handling and per-CPU AP interrupt stacks would corrupt the shared NMI stack the first time a watchdog NMI landed during another NMI or on an AP. Per-phase timeouts, hang diagnostics and the VPD countdown depend on that layer and are parked with it.

## How does it work?

`hw_watchdog_init()` runs once in Phase 1, after `acpi_init()`. It asks for the `WDAT` table; if ACPI is not ready or the table is absent, it logs and leaves the watchdog unarmed.

When a table exists, `hw_watchdog_wdat_validate()` checks it before touching any register: the header fits the reported length, the entry count cannot run past the end, `timer_period` is non-zero, `min_count <= max_count`, every instruction code is known, and every register is an 8, 16 or 32-bit I/O port. Memory-mapped registers are rejected outright. It also checks that each action uses the right instruction class (a stop action must be a write, the running-state query must be a read), and one wrong entry fails the whole table rather than risk writing the wrong register.

For a valid table, the driver divides a fixed 120-second boot budget (`HW_WD_BOOT_TIMEOUT_MS`) by the table's period, clamps it to the table's range, and refuses to arm if the result falls below a 60-second safety floor (`HW_WD_MIN_SAFE_MS`). It reads and clears the firmware's boot-status flag, logging when the previous boot ended in a watchdog reset, then runs the countdown, reset and start actions in order. An action that executed no register writes counts as an arm failure, not a silent success.

`hw_watchdog_pet()` reloads the countdown. It runs from `boot_progress()` in `boot_init.c` at every boot milestone, from the idle poll `boot_progress_poll()`, and from the boot-confirm key-wait loop, so an operator sitting at a confirmation prompt does not trip it.

`hw_watchdog_boot_handoff()` runs in `boot_desktop.c` after the final log flush and before the scheduler starts. It stops the watchdog, then reads the running state back to confirm the stop. If that fails it retries once; if the retry also fails, it returns -1 and the caller halts boot rather than enter the desktop with an armed watchdog that nothing will pet.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `hw_watchdog_init()` | Finds, validates and arms the WDAT watchdog; a no-op when none is usable ([`watchdog.c`](../../src/kernel/drivers/watchdog.c)) |
| `hw_watchdog_pet()` | Reloads the countdown; a no-op unless armed ([`watchdog.c`](../../src/kernel/drivers/watchdog.c)) |
| `hw_watchdog_boot_handoff()` | Stops the watchdog with readback before the scheduler starts; -1 means the caller halts ([`watchdog.c`](../../src/kernel/drivers/watchdog.c)) |
| `hw_watchdog_kind()` | Reports `HW_WD_NONE`, `HW_WD_WDAT` or `HW_WD_ITCO` (the last is unused today) ([`watchdog.h`](../../include/kernel/drivers/watchdog.h)) |
| `hw_watchdog_wdat_validate()` | Pure WDAT validator with no I/O, exposed for unit tests ([`watchdog.c`](../../src/kernel/drivers/watchdog.c)) |
| `struct acpi_wdat`, `struct acpi_wdat_entry` | The WDAT header and entry layout, pinned by `_Static_assert` ([`watchdog.h`](../../include/kernel/drivers/watchdog.h)) |
| `test_watchdog.c` | Validator cases on crafted tables plus a pre-init `hw_watchdog_kind()` check, in the `boot` suite ([`test_watchdog.c`](../../src/kernel/test/test_watchdog.c)) |

## How do I use it?

There is no configuration flag; the driver runs on every boot.

```bash
bash scripts/test.sh SUITE=boot    # WDAT validator cases
bash scripts/test-smoke.sh         # QEMU emulates no WDAT, so nothing is armed
```

On QEMU, serial shows `HW watchdog: none (no ACPI WDAT)` and boot continues without hang coverage.

On hardware with a valid I/O-port WDAT, serial shows `HW watchdog: WDAT armed (<ms> ms; <counts> counts x <period> ms)`, and at the user-mode handoff `HW watchdog disarmed at boot handoff`. After a watchdog-triggered reset, it first logs `previous boot was watchdog-reset (WDAT status condition met)`.

The refusal lines name their reason: `HW watchdog: none (WDAT present but invalid/unusable or not I/O-space)`, `HW watchdog: none (WDAT max timeout <ms> ms < <floor> ms safe floor)` and `HW watchdog: none (WDAT arm action did not execute)`. If the stop cannot be confirmed after the retry, serial shows `HW watchdog DISARM FAILED -- refusing to enter the desktop armed (it would reboot with no runtime petter)` and boot halts.

## What is not implemented yet?

- The LAPIC NMI software watchdog, which could catch a hang with interrupts disabled, waits on per-CPU nested-NMI handling and per-CPU AP interrupt stacks: [Software Watchdog via LAPIC NMI Timer](../../todo/01-boot-platform/TODO-23-boot-watchdog.md#1-software-watchdog-via-lapic-nmi-timer).
- Per-phase timeouts and a `boot.conf` override need a reloadable per-phase timer; only the single 120-second budget exists: [Per-Phase Timeout Configuration](../../todo/01-boot-platform/TODO-23-boot-watchdog.md#2-per-phase-timeout-configuration).
- Reloading a per-phase NMI timer from `boot_progress()` waits on the same NMI prerequisite (the hardware pet from `boot_progress()` already ships): [Watchdog Pet at boot_progress()](../../todo/01-boot-platform/TODO-23-boot-watchdog.md#3-watchdog-pet-at-boot_progress).
- No hang diagnostics: a WDAT reset reboots the board with no handler running first, so nothing logs RIP, RSP or the last boot milestones: [Watchdog-Triggered Reboot with Diagnostics](../../todo/01-boot-platform/TODO-23-boot-watchdog.md#4-watchdog-triggered-reboot-with-diagnostics).
- No countdown or previous-hang display on the VPD screen: [Watchdog Status in VPD](../../todo/01-boot-platform/TODO-23-boot-watchdog.md#6-watchdog-status-in-vpd).
- An Intel iTCO fallback, memory-mapped WDAT registers and recording the watchdog-reset flag in `boot_info` are parked, mainly on kernel image size: [Post-Ship Follow-Up Backfill](../../todo/01-boot-platform/TODO-23-boot-watchdog.md#7-post-ship-follow-up-backfill-orphan-cohort-2026-07-31).
- Bare-metal hang detection, false-trigger and rollback checks cannot run on QEMU, which emulates no WDAT: [Verification](../../todo/01-boot-platform/TODO-23-boot-watchdog.md#verification).

## How does it compare with Windows 11 and Linux?

Windows 11 uses the ACPI WDAT through its watchdog driver and falls back to Automatic Repair after repeated failed boots. Linux uses `iTCO_wdt` or another hardware driver, plus systemd's watchdog support for user-space hangs. Impossible OS covers only the coarse hardware layer: a validated, fail-closed WDAT arm and disarm with one whole-boot timeout. It has no software layer for hangs with interrupts disabled and no coverage on hardware without a usable WDAT.

## See also

- [Boot Watchdog roadmap](../../todo/01-boot-platform/TODO-23-boot-watchdog.md)
- [Boot Diagnostics](boot-diagnostics.md)
- [Boot Health Gate](boot-health.md)
- [A/B Dual-Slot Boot and Automatic Rollback](ab-boot-rollback.md)
