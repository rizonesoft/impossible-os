# TODO-16 -- Boot Watchdog & Hang Detection

> **Goal:** Detect and recover from boot hangs automatically. If any boot phase takes longer than its expected maximum, a hardware or software watchdog triggers a reboot. Combined with A/B rollback (TODO-14), this means a hung boot = automatic reboot = automatic rollback to working version. No infinite hang, no user intervention. Models: embedded systems watchdog best practices, systemd watchdog integration.

> [!IMPORTANT]
> **Current state:** No watchdog of any kind. If boot hangs (e.g., AHCI probe on absent hardware, xHCI takeover timeout, VFS mount of corrupt filesystem), the system stops forever. The only recovery is power cycling. With no boot failure counting, the same hang repeats on every reboot.

---

## Inputs

- `src/kernel/main/boot_storage.c` -- Phase 2 boot (storage, VFS, subsystems)
- `src/kernel/main/boot_desktop.c` -- Phase 3 boot (scheduler, desktop)
- `src/kernel/drivers/lapic.c` -- LAPIC timer (software watchdog source)
- `include/kernel/boot_init.h` -- boot phases and subsystem tracking
- → XREF: `TODO-14-ab-boot-rollback.md §4` -- failure counting + rollback on hang
- → XREF: `TODO-02-bootloader-error-recovery.md §9` -- boot failure screen
- → XREF: `TODO-02-bootloader-error-recovery.md §11` -- UEFI-stage watchdog before ExitBootServices; §1 here covers kernel-stage LAPIC NMI watchdog after ExitBootServices -- complementary coverage

---

## Outcome

- Software watchdog: LAPIC NMI timer fires if boot phase exceeds timeout → logs hang location → reboots.
- Each boot phase has an expected max duration (Phase 0: 2s, Phase 1: 5s, Phase 2: 30s, Phase 3: 60s).
- Watchdog is petted (reset) at each boot_progress() call -- stalled subsystem triggers NMI.
- Combined with A/B boot: hang → watchdog reboot → try counter increments → 3 hangs → rollback.
- Optional: ACPI hardware watchdog (TCO timer on Intel) for full hardware-level reset.

---

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Software watchdog via LAPIC NMI timer           | --          |  [ ]   |
| 💎  |   2   | Per-phase timeout configuration                 | §1         |  [ ]   |
| 💎  |   3   | Watchdog pet at each boot_progress() call       | §1, §2     |  [ ]   |
| 💎  |   4   | Watchdog-triggered reboot with diagnostics      | §3, T14 §4 |  [ ]   |
| 💎  |   5   | ACPI TCO hardware watchdog (Intel platforms)     | --          |  [ ]   |
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
- [ ] Commit: `"boot: software watchdog via LAPIC NMI -- detect hung boot phases"`

**Test checkpoint:** Add `for(;;){}` in boot_phase2 (debug build only). Watchdog fires → serial shows `"WATCHDOG: boot hung at POST 0xNNNN, RIP=0xNNNN"` → system reboots.

**Regression risk:** HIGH -- NMI watchdog fires during normal boot if timeout too aggressive. Start with generous timeouts (2x expected max).

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
- [ ] Increment A/B try counter (→ XREF: TODO-14 §4) so 3 hangs → rollback
- [ ] Boot failure screen (→ XREF: TODO-02 §9) shows hang location if available
- [ ] Commit: `"boot: watchdog reboot with diagnostics -- POST code, RIP, NVRAM flag"`

**Test checkpoint:** Intentional hang → watchdog fires → reboot → serial shows previous hang location → A/B counter incremented.

---

## 5. ACPI TCO Hardware Watchdog

Intel TCO (Total Cost of Ownership) timer provides hardware-level reboot even if CPU is locked.

- [ ] Detect TCO timer via ACPI FADT / Intel chipset PCI registers
- [ ] If present: configure with boot-phase timeout, pet alongside LAPIC NMI watchdog
- [ ] TCO fires a hardware reboot signal -- survives complete CPU lockup (unlike NMI which needs CPU)
- [ ] If not present: LAPIC NMI watchdog is sufficient
- [ ] Log: `"[BOOT] TCO watchdog: %s (%u seconds)"` with enabled/not-available
- [ ] Commit: `"boot: ACPI TCO hardware watchdog -- hardware-level reboot on total lockup"`

**Test checkpoint:** On Intel bare metal (i5-11600K): TCO watchdog detected and configured. On QEMU: `"TCO: not available"` (expected).

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
| 💎 | Hang → rollback            | ✅ Automatic Repair     | ⚠️ Manual intervention     | ⬜ §4 + T14               |
| ⭐ | Watchdog in boot display   | ❌ Hidden               | ❌ Hidden                  | ⬜ §6 🚀                  |

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_watchdog()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Watchdog trigger tests use `scripts/test-smoke.sh` with intentional hang builds.

- [ ] Create `src/kernel/test/test_watchdog.c` with:
  - Per-phase timeout lookup: Phase 0 returns 2s, Phase 1 returns 5s, Phase 2 returns 30s, Phase 3 returns 60s
  - `watchdog_pet()` resets countdown without firing (call pet, verify timer reloaded)
  - Timeout configuration: `boot_watchdog_timeouts[]` values are all > 0 and within sane range (1s-300s)
  - TCO detection: `tco_watchdog_available()` returns 0 on QEMU (no TCO emulation)
  - NVRAM flag read: `watchdog_triggered` flag readable from boot_info (0 on clean boot)
- [ ] Register in `test_runner_init()`: `test_register_watchdog()`
- [ ] Create `scripts/test-boot-watchdog.sh`:
  - Build a debug kernel with `WATCHDOG_TEST_HANG=1` (intentional `for(;;){}` in Phase 2)
  - Boot QEMU headless with 45s timeout
  - Assert serial contains `"WATCHDOG: boot hung at POST"` (§4 -- NMI fired with diagnostics)
  - Assert QEMU exits (reboot triggered, `-no-reboot` causes shutdown)
- [ ] Add smoke test pattern to `scripts/test-smoke.sh`: absence of `"WATCHDOG"` on normal boot (no false triggers)
- [ ] Commit: `"test: add boot watchdog test suite with intentional-hang smoke test"`

## Verification

- [ ] **Hang detection**: intentional infinite loop in Phase 2 → watchdog fires, system reboots.
- [ ] **Normal boot**: all phases complete within timeouts, no false watchdog triggers.
- [ ] **Hang + rollback**: 3 watchdog reboots → A/B rollback to working slot.
- [ ] **TCO test**: Intel bare metal -- TCO watchdog detected and configured.
- [ ] Commit: `"boot: watchdog system complete -- no more infinite hangs"`
