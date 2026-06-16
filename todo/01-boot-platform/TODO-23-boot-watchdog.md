---
schema_version: 1
id: boot-watchdog
domain: 01-boot-platform
status: active
title: "TODO-23 -- Boot Watchdog & Hang Detection"
---

# TODO-23 -- Boot Watchdog & Hang Detection

> **Goal:** Detect and recover from boot hangs automatically. If any boot phase takes longer than its expected maximum, a hardware or software watchdog triggers a reboot. Combined with A/B rollback (TODO-21), this means a hung boot = automatic reboot = automatic rollback to working version. No infinite hang, no user intervention. Models: embedded systems watchdog best practices, systemd watchdog integration.

> [!IMPORTANT]
> **Current state:** No watchdog of any kind. If boot hangs (e.g., AHCI probe on absent hardware, xHCI takeover timeout, VFS mount of corrupt filesystem), the system stops forever. The only recovery is power cycling. With no boot failure counting, the same hang repeats on every reboot.

---

## Inputs

- `src/kernel/main/boot_storage.c` -- Phase 2 boot (storage, VFS, subsystems)
- `src/kernel/main/boot_desktop.c` -- Phase 3 boot (scheduler, desktop)
- `src/kernel/drivers/lapic.c` -- LAPIC timer (software watchdog source)
- `include/kernel/boot_init.h` -- boot phases and subsystem tracking
- → XREF: `TODO-21-ab-boot-rollback.md §4` -- failure counting + rollback on hang
- → XREF: `TODO-03-bootloader-error-recovery.md §2` -- boot failure screen
- → XREF: `TODO-03-bootloader-error-recovery.md §11` -- UEFI-stage watchdog before ExitBootServices; §1 here covers kernel-stage LAPIC NMI watchdog after ExitBootServices -- complementary coverage

---

## Outcome

- Software watchdog: LAPIC NMI timer fires if boot phase exceeds timeout → logs hang location → reboots.
- Each boot phase has an expected max duration (Phase 0: 2s, Phase 1: 5s, Phase 2: 30s, Phase 3: 60s).
- Watchdog is petted (reset) at each boot_progress() call -- stalled subsystem triggers NMI.
- Combined with A/B boot: hang → watchdog reboot → try counter increments → 3 hangs → rollback.
- ACPI WDAT hardware watchdog (firmware-abstracted; direct iTCO deferred) for hardware-level reset on total lockup.

---

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Software watchdog via LAPIC NMI timer           | D01 T10 §2 (nested-NMI), T09 §10 (AP IST) |  [/]   |
| 💎  |   2   | Per-phase timeout configuration                 | §1         |  [ ]   |
| 💎  |   3   | Watchdog pet at each boot_progress() call       | §1, §2     |  [ ]   |
| 💎  |   4   | Watchdog-triggered reboot with diagnostics      | §3, T21 §4 |  [ ]   |
| 💎  |   5   | ACPI WDAT hardware watchdog (WDAT-first; iTCO deferred) | --     |  [ ]   |
| ⭐  |   6   | Watchdog status in VPD display                  | §1–§5      |  [ ]   |

> 💎 = parity -- Windows boot watchdog and Linux systemd watchdog both detect hung boots.
> ⭐ = exclusive -- watchdog countdown visible in VPD during boot.

---

## 1. Software Watchdog via LAPIC NMI Timer

Use the LAPIC timer in NMI mode to detect hangs even when interrupts are disabled.

- [ ] Configure LAPIC LVT Timer in NMI mode (not periodic) with a one-shot countdown
- [ ] NMI fires when countdown expires → NMI handler logs current RIP, phase, last POST code
- [ ] After logging: trigger ACPI reboot (`acpi_reboot()`)
- [ ] NMI handler is minimal: write to serial, set NVRAM "watchdog_triggered" flag, reboot
- [ ] If LAPIC not available: fall back to PIT-based software watchdog (less reliable, maskable)
- [ ] Nested-NMI safety prerequisite: per-CPU latch/replay (or drop) so a watchdog NMI during an existing NMI/MCE path cannot corrupt the shared IST2 stack (Linux `repeat_nmi` model); must land before enabling the watchdog NMI. → XREF: `D01 T10 §2`
- [ ] AP-IST safety: arm the LAPIC NMI watchdog on the BSP ONLY until per-CPU AP TSS/IST lands -- the IST is BSP-only today, so a watchdog NMI reaching an AP post-SMP corrupts the shared IST. Add an AP-NMI boot test when it lands. → XREF: `T09 §10`
- [ ] Commit: `"boot: software watchdog via LAPIC NMI -- detect hung boot phases"`

**Test checkpoint:** Add `for(;;){}` in boot_phase2 (debug build only). Watchdog fires → serial shows `"WATCHDOG: boot hung at POST 0xNNNN, RIP=0xNNNN"` → system reboots.

**Regression risk:** HIGH -- NMI watchdog fires during normal boot if timeout too aggressive. Start with generous timeouts (2x expected max).

> **Notes:**
> - **Status:** deferred -- the LAPIC NMI watchdog cannot be safely ENABLED until two safety prerequisites land; shipping it now would corrupt the shared IST2 stack on a nested NMI or on an AP, bricking under exactly the stress the watchdog diagnoses.
> - **Blocker 1 (nested-NMI):** no `repeat_nmi`-style latch/replay exists (the IST2 NMI handler is only the fatal `panic_screen()` path). -> XREF: `D01 T10 §2`.
> - **Blocker 2 (AP IST):** APs share one `kernel_tss`/IST (BSP-only, no AP `ltr`), so a watchdog NMI on an AP post-SMP is unsafe. -> XREF: `D01 T09 §10` (per-CPU TSS/IST).
> - **PIT fallback note:** the maskable PIT software watchdog is safe (no IST/NMI hazard) but cannot catch interrupts-disabled (cli-region) hangs -- the common boot-hang case -- so it is not a credible standalone §1.
> - **Scope boundary:** §5 (ACPI WDAT/TCO hardware watchdog) is INDEPENDENT of these blockers and is the next-implementable section; §2-§4/§6 cascade-block on §1.

> **Verified:** 2026-06-16 | 0/7 items (deferred) | build N/A | blocked on nested-NMI replay + per-CPU AP IST
> **Deferred:** [H] LAPIC NMI watchdog blocked on nested-NMI latch/replay + per-CPU AP TSS/IST (enabling it without them corrupts the shared IST2 stack) -> XREF: 01-boot-platform/TODO-10 §2 (item: "AP per-CPU TSS/IST") + 01-boot-platform/TODO-09 §10 (item: "Per-CPU TSS + IST")

---

## 2. Per-Phase Timeout Configuration

Each boot phase gets a maximum allowed duration.

- [ ] Phase 0 (early init): 2 seconds (CPU, memory, serial -- should complete in <500ms)
- [ ] Phase 1 (interrupts, timer): 5 seconds (LAPIC calibration can take 1–2s on some hardware)
- [ ] Phase 2 (storage, VFS, SMP): 30 seconds (AHCI spin-up, USB enumeration, filesystem mount)
- [ ] Phase 3 (scheduler, desktop): 60 seconds (font loading, icon store, wallpaper decode)
- [ ] Timeouts stored in `boot_watchdog_timeouts[]` -- configurable via boot.conf `watchdog_timeout=`
- [ ] Commit: `"boot: per-phase watchdog timeouts -- 2s/5s/30s/60s defaults"`

**Test checkpoint:** Normal boot completes well within all timeouts. Serial shows `"Watchdog: Phase 2 timeout=30s"` at phase entry.

---

## 3. Watchdog Pet at boot_progress()

Every `boot_progress()` / `POST16()` call resets the watchdog countdown.

- [ ] `watchdog_pet()` -- reload LAPIC NMI timer with current phase's timeout
- [ ] Called automatically from `boot_progress()` macro
- [ ] If a subsystem hangs between two progress calls: watchdog fires
- [ ] The more granular the POST codes, the faster hangs are detected
- [ ] Commit: `"boot: watchdog pet at every boot_progress -- reset countdown on progress"`

**Test checkpoint:** Add POST codes around a slow operation. Watchdog doesn't fire (pet keeps resetting it).

---

## 4. Watchdog-Triggered Reboot with Diagnostics

When watchdog fires, produce useful diagnostics before rebooting.

- [ ] NMI handler writes to serial: phase, last POST code, RIP, RSP, last 5 boot_progress entries
- [ ] Set NVRAM flag: `watchdog_triggered = 1`, `watchdog_post = <last POST code>`
- [ ] On next boot: bootloader reads NVRAM flag → `"[WARN] Previous boot hung at POST 0xNNNN"` on serial
- [ ] Do NOT write A/B metadata from the NMI path: TODO-21 §4 already increments tries pre-EBS + resets on mark-good, so a hung boot consumed its try and rollback follows; a watchdog-side increment double-counts. -> XREF: TODO-21 §4
- [ ] Optional Windows-style consecutive-boot-failure counter is boot-status TELEMETRY (NVRAM `watchdog_fail_count`, cleared on a marked-good boot), never a second A/B try write
- [ ] Boot failure screen (→ XREF: TODO-03 §2) shows hang location if available
- [ ] Commit: `"boot: watchdog reboot with diagnostics -- POST code, RIP, NVRAM flag"`

**Test checkpoint:** Intentional hang → watchdog fires → reboot → serial shows previous hang location; the slot's pre-EBS try (TODO-21) is already consumed, so 3 hung boots roll back without any watchdog-side A/B write.

---

## 5. ACPI WDAT Hardware Watchdog

A hardware watchdog reboots the board even on a total CPU lockup. §5 is STANDALONE (the §1 LAPIC NMI software watchdog is deferred); WDAT-only first cut, direct iTCO is a tracked follow-up.

- [ ] Discover WDAT via the validated `acpi_get_raw_table("WDAT")` (gated by `acpi_is_ready()`), not the RSDT-only `find_table_rsdt`; validate header size + entry-count overflow + table length + action/instruction enums + GAS fields before any register access
- [ ] Map WDAT register regions per GAS: system-memory via `vmm_map_mmio_uc()`, system-I/O via width-correct port I/O; execute all instruction entries for an action in order
- [ ] WDAT actions: GET/SET_RUNNING_STATE, SET_COUNTDOWN, RESET (pet), GET/SET_BOOT_STATUS; clamp the boot-wide timeout into the min/max `timer_period` range (refuse + log if un-clampable)
- [ ] Read + clear the WDAT boot-status (was-watchdog-reboot) flag into boot_info/NVRAM for the previous-boot-hung diagnostic; honor watchdog-stopped-in-sleep
- [ ] Arm with a single generous boot-wide timeout (per-phase timeouts are the deferred §2); pet from `boot_progress()` ONLY while a HW watchdog is armed
- [ ] `hw_watchdog_boot_handoff()`: disarm (SET_RUNNING_STATE=stopped) after the final boot-log flush + before `task_create()`/`scheduler_enable()`; never enter the desktop armed (no runtime petter yet); on disarm-failure do NOT proceed armed
- [ ] If no usable WDAT: log `"[BOOT] HW watchdog: none"` -- NO reboot coverage until §1 (NMI) or the iTCO follow-up lands
- [ ] DEFERRED follow-up: direct Intel iTCO PCI fallback (PCH-generation chipset allowlist, LPC/PMC TCO base, GCS NO_REBOOT, SMI_EN, two-stage timeout, readback verify, verified-disarm)
- [ ] Commit: `"boot: ACPI WDAT hardware watchdog -- WDAT-only, disarm at boot-handoff"`

**Test checkpoint:** On Intel bare metal with firmware WDAT: detected + armed, serial `"[BOOT] HW watchdog: WDAT (Ns)"`, disarmed at boot-handoff (no mid-desktop reboot). On QEMU (no WDAT): `"[BOOT] HW watchdog: none"` -- clean, no hang-recovery claim (§1 NMI deferred).

**Regression risk:** HIGH -- a HW watchdog that fails to disarm at boot-handoff reboots the board mid-desktop. The disarm path must be readback-verified before the scheduler starts.

---

## 6. Watchdog Status in VPD

Show watchdog countdown in the VPD display during boot.

- [ ] VPD shows remaining watchdog time for current phase: `"Phase 2: 28s remaining"`
- [ ] If previous boot was watchdog-triggered: `"⚠ Previous boot hung at POST 0xNNNN"`
- [ ] Countdown updates every second (from LAPIC timer tick)
- [ ] Commit: `"boot: watchdog countdown in VPD display"`

**Test checkpoint:** Boot with `postbars=2` (diagnostic mode) -- watchdog countdown visible.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                   | 🐧 Linux                      | 🚀 Impossible OS             |
|----|----------------------------|-------------------------|----------------------------|---------------------------|
| 💎 | Boot hang detection        | ✅ Boot watchdog        | ✅ systemd watchdog        | ⬜ §1–§3                  |
| 💎 | Hardware watchdog          | ✅ ACPI WDT driver      | ✅ iTCO_wdt driver         | ⬜ §5                     |
| 💎 | Hang → rollback            | ✅ Automatic Repair     | ⚠️ Manual intervention     | ⬜ §4 + T21               |
| ⭐ | Watchdog in boot display   | ❌ Hidden               | ❌ Hidden                  | ⬜ §6 🚀                  |

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_watchdog()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Watchdog trigger tests use `scripts/test-smoke.sh` with intentional hang builds.

- [ ] Create `src/kernel/test/test_watchdog.c` with:
  - Per-phase timeout lookup: Phase 0 returns 2s, Phase 1 returns 5s, Phase 2 returns 30s, Phase 3 returns 60s
  - `watchdog_pet()` resets countdown without firing (call pet, verify timer reloaded)
  - Timeout configuration: `boot_watchdog_timeouts[]` values are all > 0 and within sane range (1s-300s)
  - HW watchdog detection: `hw_watchdog_kind()` returns NONE on QEMU (no WDAT/TCO emulation); WDAT parse rejects a bad `ACPI_SIG_WDAT` signature
  - NVRAM flag read: `watchdog_triggered` flag readable from boot_info (0 on clean boot)
- [ ] Register in `test_runner_init()`: `test_register_watchdog()`
- [ ] Create `scripts/test-boot-watchdog.sh`:
  - Build a debug kernel with `WATCHDOG_TEST_HANG=1` (intentional `for(;;){}` in Phase 2)
  - Boot QEMU headless with 45s timeout
  - Assert serial contains `"WATCHDOG: boot hung at POST"` (§4 -- NMI fired with diagnostics)
  - Assert QEMU exits (reboot triggered, `-no-reboot` causes shutdown)
- [ ] Add smoke test pattern to `scripts/test-smoke.sh`: absence of `"WATCHDOG"` on normal boot (no false triggers)
- [ ] Commit: `"test: add boot watchdog test suite with intentional-hang smoke test"`

---

## Verification

- [ ] **Hang detection**: intentional infinite loop in Phase 2 → watchdog fires, system reboots.
- [ ] **Normal boot**: all phases complete within timeouts, no false watchdog triggers.
- [ ] **Hang + rollback**: 3 watchdog reboots → A/B rollback to working slot.
- [ ] **TCO test**: Intel bare metal -- TCO watchdog detected and configured.
- [ ] Commit: `"boot: watchdog system complete -- no more infinite hangs"`
