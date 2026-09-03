---
schema_version: 1
id: boot-watchdog
domain: 01-boot-platform
status: active
title: "TODO-23 -- Boot Watchdog & Hang Detection"
---

# TODO-23 -- Boot Watchdog & Hang Detection

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

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

| ⭐  | Order | Deliverable                                             | Depends On                                | Status |
| --- | :---: | ------------------------------------------------------- | ----------------------------------------- | :----: |
| 💎  |   1   | Software watchdog via LAPIC NMI timer                   | D01 T10 §2 (nested-NMI), T09 §10 (AP IST) |  [/]   |
| 💎  |   2   | Per-phase timeout configuration                         | §1                                        |  [/]   |
| 💎  |   3   | Watchdog pet at each boot_progress() call               | §1, §2                                    |  [/]   |
| 💎  |   4   | Watchdog-triggered reboot with diagnostics              | §3, T21 §4                                |  [/]   |
| 💎  |   5   | ACPI WDAT hardware watchdog (WDAT-first; iTCO deferred) | --                                        |  [x]   |
| ⭐  |   6   | Watchdog status in VPD display                          | §1-§5                                     |  [/]   |
| ⭐  |   7   | Post-ship follow-up backfill (2026-07-31 cohort)        | --                                        |  [/]   |

> 💎 = parity -- Windows boot watchdog and Linux systemd watchdog both detect hung boots.
> ⭐ = exclusive -- watchdog countdown visible in VPD during boot.

---

## 1. Software Watchdog via LAPIC NMI Timer

Use the LAPIC timer in NMI mode to detect hangs even when interrupts are disabled.

- [/] Configure LAPIC LVT Timer in NMI mode (not periodic) with a one-shot countdown -- BLOCKED on nested-NMI latch/replay + per-CPU AP TSS/IST -> XREF: `01-boot-platform/TODO-10` §2 + `01-boot-platform/TODO-09` §10
- [/] NMI fires when countdown expires → NMI handler logs current RIP, phase, last POST code -- BLOCKED on nested-NMI latch/replay + per-CPU AP TSS/IST -> XREF: `01-boot-platform/TODO-10` §2 + `01-boot-platform/TODO-09` §10
- [/] After logging: trigger ACPI reboot (`acpi_reboot()`) -- BLOCKED on nested-NMI latch/replay + per-CPU AP TSS/IST -> XREF: `01-boot-platform/TODO-10` §2 + `01-boot-platform/TODO-09` §10
- [/] NMI handler is minimal: write to serial, set NVRAM "watchdog_triggered" flag, reboot -- BLOCKED on nested-NMI latch/replay + per-CPU AP TSS/IST -> XREF: `01-boot-platform/TODO-10` §2 + `01-boot-platform/TODO-09` §10
- [/] If LAPIC not available: fall back to PIT-based software watchdog (less reliable, maskable) -- BLOCKED on nested-NMI latch/replay + per-CPU AP TSS/IST -> XREF: `01-boot-platform/TODO-10` §2 + `01-boot-platform/TODO-09` §10
- [/] Nested-NMI safety prerequisite: per-CPU latch/replay (or drop) so a watchdog NMI during an existing NMI/MCE path cannot corrupt the shared IST2 stack (Linux `repeat_nmi` model); must land before enabling the watchdog NMI. → XREF: `D01 T10 §2`
      - BLOCKED on nested-NMI latch/replay + per-CPU AP TSS/IST -> XREF: `01-boot-platform/TODO-10` §2 + `01-boot-platform/TODO-09` §10
- [/] AP-IST safety: arm the LAPIC NMI watchdog on the BSP ONLY until per-CPU AP TSS/IST lands -- the IST is BSP-only today, so a watchdog NMI reaching an AP post-SMP corrupts the shared IST. Add an AP-NMI boot test when it lands. → XREF: `T09 §10`
      - BLOCKED on nested-NMI latch/replay + per-CPU AP TSS/IST -> XREF: `01-boot-platform/TODO-10` §2 + `01-boot-platform/TODO-09` §10
- [/] When this lands, close the NMI return-tail residual it makes reachable and re-weigh the depth-marker cost. -- BLOCKED on nested-NMI latch/replay + per-CPU AP TSS/IST -> XREF: `01-boot-platform/TODO-10` §2 + `01-boot-platform/TODO-09` §10
  - A returning watchdog NMI is the first handler to execute the vector-2 epilogue, where one register restore, the CS test, the conditional VERW block, the `swapgs` and `IRETQ` all run with the NMI depth already lowered.
  - Also re-weigh two costs that are nil for a terminal NMI: a serializing CPUID and a `lock`-prefixed RMW on each of the raise and the lower. At ~1 Hz they are unmeasurable; confirm that before relying on it -> XREF: `01-boot-platform/TODO-10 §22` (item: "PARKED, blocked on a returning NMI handler existing")
- [/] Commit: `"boot: software watchdog via LAPIC NMI -- detect hung boot phases"` -- BLOCKED on nested-NMI latch/replay + per-CPU AP TSS/IST -> XREF: `01-boot-platform/TODO-10` §2 + `01-boot-platform/TODO-09` §10

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

- [/] Phase 0 (early init): 2 seconds (CPU, memory, serial -- should complete in <500ms) -- BLOCKED on this file's §1 (a per-phase-reloadable LAPIC NMI timer)
- [/] Phase 1 (interrupts, timer): 5 seconds (LAPIC calibration can take 1–2s on some hardware) -- BLOCKED on this file's §1 (a per-phase-reloadable LAPIC NMI timer)
- [/] Phase 2 (storage, VFS, SMP): 30 seconds (AHCI spin-up, USB enumeration, filesystem mount) -- BLOCKED on this file's §1 (a per-phase-reloadable LAPIC NMI timer)
- [/] Phase 3 (scheduler, desktop): 60 seconds (font loading, icon store, wallpaper decode) -- BLOCKED on this file's §1 (a per-phase-reloadable LAPIC NMI timer)
- [/] Timeouts stored in `boot_watchdog_timeouts[]` -- configurable via boot.conf `watchdog_timeout=` -- BLOCKED on this file's §1 (a per-phase-reloadable LAPIC NMI timer)
- [/] Commit: `"boot: per-phase watchdog timeouts -- 2s/5s/30s/60s defaults"` -- BLOCKED on this file's §1 (a per-phase-reloadable LAPIC NMI timer)

**Test checkpoint:** Normal boot completes well within all timeouts. Serial shows `"Watchdog: Phase 2 timeout=30s"` at phase entry.

> **Verified:** 2026-06-16 | 0/6 items (deferred) | build N/A | blocked on §1 (LAPIC NMI per-phase timer)
> **Deferred:** [M] per-phase timeout enforcement needs a per-phase-reloadable watchdog; §1 (LAPIC NMI) is deferred and the shipped §5 WDAT is a single coarse boot-wide timeout. Unblock via §1, OR re-scope onto §5 by reprogramming WDAT SET_COUNTDOWN at each phase boundary -> XREF: 01-boot-platform/TODO-23 §1 (item: "watchdog_pet() reload LAPIC NMI timer with current phase's timeout")

---

## 3. Watchdog Pet at boot_progress()

Every `boot_progress()` / `POST16()` call resets the watchdog countdown.

- [/] `watchdog_pet()` -- reload LAPIC NMI timer with current phase's timeout -- BLOCKED on this file's §1 (the LAPIC NMI timer this pet reloads)
- [/] Called automatically from `boot_progress()` macro -- BLOCKED on this file's §1 (the LAPIC NMI timer this pet reloads)
- [/] If a subsystem hangs between two progress calls: watchdog fires -- BLOCKED on this file's §1 (the LAPIC NMI timer this pet reloads)
- [/] The more granular the POST codes, the faster hangs are detected -- BLOCKED on this file's §1 (the LAPIC NMI timer this pet reloads)
- [/] Commit: `"boot: watchdog pet at every boot_progress -- reset countdown on progress"` -- BLOCKED on this file's §1 (the LAPIC NMI timer this pet reloads)

**Test checkpoint:** Add POST codes around a slow operation. Watchdog doesn't fire (pet keeps resetting it).

> **Verified:** 2026-06-16 | 0/5 items (deferred) | build N/A | NMI pet blocked on §1; WDAT pet shipped in §5
> **Deferred:** [M] the boot_progress() pet hook for the HARDWARE WDAT already shipped in §5 (`hw_watchdog_pet()` runs from `boot_progress()`); this section's LAPIC-NMI-timer reload is what remains and is blocked on the deferred §1 -> XREF: 01-boot-platform/TODO-23 §1 (item: "watchdog_pet() reload LAPIC NMI timer with current phase's timeout")

---

## 4. Watchdog-Triggered Reboot with Diagnostics

When watchdog fires, produce useful diagnostics before rebooting.

- [/] NMI handler writes to serial: phase, last POST code, RIP, RSP, last 5 boot_progress entries -- BLOCKED on this file's §1 (a watchdog ISR to produce diagnostics in)
- [/] Set NVRAM flag: `watchdog_triggered = 1`, `watchdog_post = <last POST code>` -- BLOCKED on this file's §1 (a watchdog ISR to produce diagnostics in)
- [/] On next boot: bootloader reads NVRAM flag → `"[WARN] Previous boot hung at POST 0xNNNN"` on serial -- BLOCKED on this file's §1 (a watchdog ISR to produce diagnostics in)
- [/] Do NOT write A/B metadata from the NMI path: TODO-21 §4 already increments tries pre-EBS + resets on mark-good, so a hung boot consumed its try and rollback follows; a watchdog-side increment double-counts. -> XREF: TODO-21 §4
      - BLOCKED on this file's §1 (a watchdog ISR to produce diagnostics in)
- [/] Optional Windows-style consecutive-boot-failure counter is boot-status TELEMETRY (NVRAM `watchdog_fail_count`, cleared on a marked-good boot), never a second A/B try write -- BLOCKED on this file's §1 (a watchdog ISR to produce diagnostics in)
- [/] Boot failure screen (→ XREF: TODO-03 §2) shows hang location if available -- BLOCKED on this file's §1 (a watchdog ISR to produce diagnostics in)
- [/] Commit: `"boot: watchdog reboot with diagnostics -- POST code, RIP, NVRAM flag"` -- BLOCKED on this file's §1 (a watchdog ISR to produce diagnostics in)

**Test checkpoint:** Intentional hang → watchdog fires → reboot → serial shows previous hang location; the slot's pre-EBS try (TODO-21) is already consumed, so 3 hung boots roll back without any watchdog-side A/B write.

> **Verified:** 2026-06-16 | 0/7 items (deferred) | build N/A | NMI-handler diagnostics blocked on §1
> **Deferred:** [M] rich hang diagnostics (phase, RIP, RSP, last POST entries) require a watchdog INTERRUPT handler; a HW WDAT reset (§5) reboots the board with no handler to run, so this is fundamentally §1-dependent. The was-watchdog-reset NVRAM flag has a partial WDAT path (a §5 boot-status-persistence follow-up) -> XREF: 01-boot-platform/TODO-23 §1 (item: "NMI handler is the watchdog ISR")

---

## 5. ACPI WDAT Hardware Watchdog

A hardware watchdog reboots the board even on a total CPU lockup. §5 is STANDALONE (the §1 LAPIC NMI software watchdog is deferred); WDAT-only first cut, direct iTCO is a tracked follow-up.

- [x] Discover WDAT via the validated `acpi_get_raw_table("WDAT")` (gated by `acpi_is_ready()`); `hw_watchdog_wdat_validate()` checks size/header/entry-overflow/enums/GAS/required-actions before any register access
- [x] I/O-space GAS register access (width-correct port I/O); `wdat_run_action()` runs all matching entries in order (PRESERVE = read-modify-write) + returns the run count (no silent no-op). MEM-space GAS deferred
- [x] WDAT actions: SET/GET_RUNNING_STATE, SET_STOPPED_STATE, SET_COUNTDOWN, RESET (pet), GET/SET_STATUS; clamp the boot-wide timeout into [min,max] AND refuse to arm below a 60s representable-timeout floor (logs the ACTUAL timeout)
- [x] Read + clear the WDAT boot-status (was-watchdog-reboot) flag at init + log the previous-boot-hung diagnostic (serial); boot_info/NVRAM persistence is a follow-up
- [x] Arm with a single generous 120s boot-wide timeout (per-phase timeouts are the deferred §2); pet from `boot_progress()` while armed (no-op otherwise)
- [x] `hw_watchdog_boot_handoff()` (int): disarm + readback-verify after the final log flush, before `task_create`/`scheduler_enable`; fail-closed -- unconfirmed disarm returns -1 and the caller `boot_halt`s (never enters the desktop armed)
- [x] If no usable WDAT: log `"HW watchdog: none"` -- NO reboot coverage until §1 (NMI) or the iTCO follow-up lands (QEMU path)
- [x] Commit: `"boot: ACPI WDAT hardware watchdog -- WDAT-only, disarm at boot-handoff"`

**Test checkpoint:** `test_watchdog.c` (TEST_CAT_BOOT, 16 cases / 17 assertions) validates the pure WDAT validator on crafted tables (valid 5-action I/O table + every guard: NULL, size, header_length, zero timer_period, min>max, entry overflow, zero entries, bad GAS, missing action, unknown instruction, wrong-instruction-class for GET_RUNNING_STATE / SET_COUNTDOWN / RESET, and a mixed-class rejection). QEMU smoke: `hw_watchdog_init` logs `"HW watchdog: none (no ACPI WDAT)"` and boot completes (~2.06s) -- the init/pet/handoff wiring is a clean no-op without firmware WDAT. Bare-metal arm/pet/disarm validated on Intel hardware via serial.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | WDAT validator 16 cases / 17 assertions (unchanged); boot suite re-run 2026-09-03: 5299 kernel + 17 user-mode PASS, 0 failures (was 2794 + 16 on 2026-06-16); smoke PASS 2026-09-03, boot complete in 2.740s, serial carries `watchdog: HW watchdog: none (no ACPI WDAT)`

> **Notes:**
> - **What shipped:** `src/kernel/drivers/watchdog.c` + `watchdog.h` (WDAT ABI structs with `_Static_assert`s; `hw_watchdog_init/pet/boot_handoff/kind` + `hw_watchdog_wdat_validate`) + `test_watchdog.c` (16 cases). Validator enforces per-action instruction class (SET writes, SET_COUNTDOWN writes the countdown, GET reads) + WDAT read-compare semantics.
> - **How it runs:** `hw_watchdog_init()` after `acpi_init` (Phase 1) discovers/validates/arms; `hw_watchdog_pet()` from `boot_progress()`; `hw_watchdog_boot_handoff()` disarms (fail-closed -> `boot_halt`) before the scheduler. No-op on QEMU.
> - **Safety:** I/O-space GAS only (no firmware RAM map / per-pet leak); validate-before-access; arm refused below a 60s timeout floor; readback-verified fail-closed disarm. Codex adoptions in commit message.
> - **Scope boundary:** §5 owns the WDAT-I/O hardware watchdog; all three follow-ups it spawned (direct-iTCO PCI, MEM-space GAS, boot_info boot-status persistence) are owned by §7. §1-§4 (LAPIC NMI) blocked on nested-NMI + AP IST.
> - **Canonical doc:** `src/kernel/drivers/watchdog.c` header + the WDAT ABI in `include/kernel/drivers/watchdog.h`.

**Regression risk:** HIGH -- a HW watchdog that fails to disarm at boot-handoff would reboot mid-desktop; mitigated by the readback-verified, fail-closed handoff (`boot_halt` rather than enter the desktop armed).

> **Verified:** 2026-06-16 | 7/9 items (3 deferred follow-ups) | build OK | counts refreshed 2026-09-03: boot suite 5299 kernel + 17 user-mode PASS, smoke PASS (boot complete 2.740s, "HW watchdog: none (no ACPI WDAT)")
> **Quality reviewed:** 2026-06-16 | Codex 8x (design, adversarial, re-adversarial, consistency, perf) | 4H+1M+1L fixed | scope: kernel-code-quality

---

## 6. Watchdog Status in VPD

Show watchdog countdown in the VPD display during boot.

- [/] VPD shows remaining watchdog time for current phase: `"Phase 2: 28s remaining"` -- BLOCKED on this file's §1-§4 (per-phase model + per-second NMI tick)
- [/] If previous boot was watchdog-triggered: `"⚠ Previous boot hung at POST 0xNNNN"` -- BLOCKED on this file's §1-§4 (per-phase model + per-second NMI tick)
- [/] Countdown updates every second (from LAPIC timer tick) -- BLOCKED on this file's §1-§4 (per-phase model + per-second NMI tick)
- [/] Commit: `"boot: watchdog countdown in VPD display"` -- BLOCKED on this file's §1-§4 (per-phase model + per-second NMI tick)

**Test checkpoint:** Boot with `postbars=2` (diagnostic mode) -- watchdog countdown visible.

> **Verified:** 2026-06-16 | 0/4 items (deferred) | build N/A | blocked on §1-§4 (per-phase + per-second NMI tick)
> **Deferred:** [L] the per-phase / per-second countdown display is driven by the LAPIC timer tick and the per-phase model from §1-§4 (all deferred); the shipped §5 WDAT exposes only a coarse boot-wide countdown (GET_CURRENT_COUNTDOWN) with no per-phase breakdown -> XREF: 01-boot-platform/TODO-23 §2 (item: "Timeouts stored in boot_watchdog_timeouts[] -- configurable via boot.conf watchdog_timeout=")

---

## 7. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 5:
- [/] DEFERRED follow-up: direct Intel iTCO PCI fallback (chipset allowlist, LPC TCO base, GCS NO_REBOOT, SMI_EN, two-stage timeout, readback verify, verified-disarm) -- WRITTEN and design-reviewed, parked on the kernel image ceiling
      - Implemented against the primary Intel datasheets ([ICH9] 316972-004, [ICH10] 319973) rather than a driver, because Linux `iTCO_wdt` is GPL-2.0-only and unusable here: TCOBASE is PMBASE+0x60, GCS NO_REBOOT is RCBA+0x3410 bit 5, TCO_LOCK is TCO1_CNT bit 12, and the reset lands on the SECOND expiry.
      - Allowlist is deliberately ICH9 + ICH10 only, the two generations whose layout a datasheet in hand pins; later PCHs moved TCOBASE to an SMBus-function register and NO_REBOOT out of GCS, so an unverified entry would program the wrong hardware. An unrecognised bridge yields `HW_WD_NONE`, which is today's behaviour.
      - Blocker is size, not correctness: `scripts/overnight/bss-headroom.py` at `c6b8af000` reports `__kernel_end` `0x7fead5` with **47 bytes** of `.text` growth before `USER_BASE` `0x800000`; the driver plus its fixtures is ~11 KiB, so this is unreachable by orders of magnitude rather than trimmable. The measured build failed on the BSS-collision guard at `0x803000`.
      - Preserved at `.claude/state/deferred-todo23-s7.patch` (gitignored: survives a rollover, NOT a fresh clone). Apply, do not rewrite -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §7 (item: "Re-run `01-boot-platform/TODO-23` §7 once the guard is gone")
- [/] DEFERRED follow-up: MEM-space GAS support -- positive MMIO authorisation against the memory map + one cached UC mapping per register page at init -- WRITTEN, parked on the same ceiling
      - Shipped shape in the patch is stricter than the item asked for, on the design review's [high] finding: a NEGATIVE "not usable RAM" test still admits ACPI reclaim, ACPI NVS, firmware runtime and persistent memory, because `src/kernel/mm/pmm.c:172` counts only conventional/loader/boot-services as usable. The register must instead be wholly inside ONE `EfiMemoryMappedIO`/`EfiMemoryMappedIOPortSpace` descriptor.
      - The same review found a live defect in the SHIPPED I/O path that the patch also fixes: `gas_usable` reads neither `access_size` nor `bit_offset` (`include/kernel/acpi.h:59`), while `gas_read`/`gas_write` pick the transaction width from `bit_width` alone, so a table asking for a dword transaction over a byte field is accessed at the wrong width. That fix is parked with the rest of the patch.
      - Mappings are bounded at 4 distinct pages and taken only after every range validates, because `vmm_unmap_mmio` (`src/kernel/mm/vmm.c:1598`) clears PTEs without reclaiming virtual address space.
      - Same blocker and same patch as the item above -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §7 (item: "Re-run `01-boot-platform/TODO-23` §7 once the guard is gone")

- [/] DEFERRED follow-up: persist the WDAT boot-status (was-this-boot-a-watchdog-reset) through `boot_info` instead of only logging it -- BLOCKED on the kernel image ceiling and on a protocol bump with a consumer
      - Ownerless until now, and the reason is worth recording: §5's Notes named it a follow-up "(this section)", §7 took only the other two, and `01-boot-platform/TODO-01`:44 pointed at this file's §6 for it -- but §6 is the VPD countdown display and never covers `boot_info` persistence. Nothing in `include/kernel/boot_info.h` carries it; `sticky_watchdog_rollback_request` at `boot_info.h:2064` is TODO-21 A/B rollback plumbing, a different signal.
      - `hw_watchdog_init()` already READS and clears the WDAT status and logs "previous boot was watchdog-reset"; what is missing is a `boot_info` field plus a post-boot consumer, which is a `BOOT_INFO_VERSION` bump in both headers and therefore should land beside other protocol work rather than alone.
      - Same image-ceiling blocker as the two items above -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §7 (item: "Re-run `01-boot-platform/TODO-23` §7 once the guard is gone")

From TODO-04 section 58's re-adversarial review (2026-08-02), filed here because section 1 owns the mechanism and is parked:
- [/] The usermode-test launcher needs a preemption source the dead TIMER cannot take with it -- blocked on this file's §1, which owns the LAPIC NMI watchdog and is itself parked
      - Section 58 gave every launcher wait a TSC watchdog, which escapes any stall where the launcher still gets CPU back -- the mono-epoch and clock-derivation failures, and every wait that yields to a cooperative or absent peer. It cannot escape the one mode where the periodic TICK ITSELF is dead AND the launcher has yielded to a non-cooperative ring-3 child: preemption dies with the clock, so the launcher never runs again to sample its own watchdog. No user-mode-visible mechanism closes that; it needs the LAPIC NMI timer this file's section 1 owns, or an equivalent source unaffected by the failed timer path.
      - Acceptance: with the periodic tick disabled and a spinning ring-3 child, the launcher regains control and reports the stall. Synthetic ops cannot prove this -- a callback that returns is exactly the assumption under test -- so it needs the end-to-end fixture.
      -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §58 (item: "Give every launcher wait a clock-independent escape, not just a deadline")

**Test checkpoint:** per moved item; each carries its original acceptance text.

> **Notes:**
> - Written this pass but NOT shipped: a direct Intel iTCO fallback and memory-space GAS support for the WDAT driver, both in `src/kernel/drivers/watchdog.c` / `include/kernel/drivers/watchdog.h`, with 10 new pure-validator tests in `src/kernel/test/test_watchdog.c`.
> - Register semantics come from the primary Intel ICH9/ICH10 datasheets, never from `iTCO_wdt` (GPL-2.0-only, license-incompatible with this GPL-3.0-only tree); the LPC device IDs come from FreeBSD's BSD-2-Clause `sys/dev/ichwd/ichwd.h` because the datasheets defer the concrete ID to the Specification Update.
> - Safety posture is fail-closed throughout: halt-then-verify before programming, refuse on a strap-forced NO_REBOOT, a firmware TCO_LOCK, or an SMI_EN.TCO_EN that will not clear, and never fall through to iTCO once the WDAT path has written a register.
> - Downstream effect: none today, because nothing landed in the tree; `hw_watchdog_kind()` still returns `HW_WD_NONE` on every platform without a usable WDAT.
> - Canonical doc: [`docs/infrastructure/kernel-address-space.md`](../../docs/infrastructure/kernel-address-space.md) for the `USER_BASE` ceiling that parks this section.
> - Scope boundary: the third item is not ceiling-blocked at all -- it needs this file's §1 LAPIC NMI watchdog, which is independently parked.

> **Verified:** 2026-09-03 | 0/3 items (deferred) | build FAILED on the BSS-collision guard (`__kernel_end` 0x803000 >= `USER_BASE` 0x800000), tree reverted green | design review 20260903-071737 received, 7 findings, all fixed in the parked patch
> **Deferred:** [H] the kernel image admits 47 bytes of `.text` growth and this section costs ~11 KiB, so items 1-2 are blocked by size alone rather than by any missing capability or unresolved design question; item 3 is blocked instead on this file's own §1 -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §7 (item: "Re-run `01-boot-platform/TODO-23` §7 once the guard is gone")

---

## OS Comparison

| ⭐  | Feature                  | 🪟 Win11            | 🐧 Linux               | 🚀 Impossible OS                                                   |
| --- | ------------------------ | ------------------- | ---------------------- | ------------------------------------------------------------------ |
| 💎  | Boot hang detection      | ✅ Boot watchdog    | ✅ systemd watchdog    | ⚠️ §5 WDAT coarse; §1-§3 NMI deferred                              |
| 💎  | Hardware watchdog        | ✅ ACPI WDT driver  | ✅ iTCO_wdt driver     | ⚠️ §5 WDAT I/O only; §7 iTCO + MEM-GAS parked on the image ceiling |
| 💎  | Hang → rollback          | ✅ Automatic Repair | ⚠️ Manual intervention | ⚠️ T21 try-consume; §4 diag deferred                               |
| ⭐  | Watchdog in boot display | ❌ Hidden           | ❌ Hidden              | ⬜ §6 (deferred)                                                   |

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_watchdog()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Watchdog trigger tests use `scripts/test-smoke.sh` with intentional hang builds.

- [/] Create `src/kernel/test/test_watchdog.c` with: -- the FILE exists (16 cases / 17 assertions, TEST_CAT_BOOT), but four of the five described behaviours do not, and each is blocked on a deferred section
  - Per-phase timeout lookup: Phase 0 returns 2s, Phase 1 returns 5s, Phase 2 returns 30s, Phase 3 returns 60s -- BLOCKED on §2; no per-phase lookup exists to test
  - `watchdog_pet()` resets countdown without firing (call pet, verify timer reloaded) -- BLOCKED on §1; the shipped `hw_watchdog_pet()` is a WDAT register write with no unit-testable surface
  - Timeout configuration: `boot_watchdog_timeouts[]` values are all > 0 and within sane range (1s-300s) -- BLOCKED on §2; `boot_watchdog_timeouts[]` does not exist anywhere in `src/kernel`
  - HW watchdog detection: `hw_watchdog_kind()` returns NONE on QEMU (no WDAT/TCO emulation) -- SHIPPED as `test_wdat_kind_none_before_init`; the paired bad-`ACPI_SIG_WDAT`-signature case is not testable through `hw_watchdog_wdat_validate()`, which never inspects the signature (`acpi_get_raw_table` matched it already)
  - NVRAM flag read: `watchdog_triggered` flag readable from boot_info (0 on clean boot) -- BLOCKED on the §7 boot-status-persistence item; no such `boot_info` field exists
- [x] Register in `test_runner_init()`: `test_register_watchdog()` -- shipped with §5; declared at `src/kernel/test/test_runner.c:1010` and called at `:1052`
- [/] Create `scripts/test-boot-watchdog.sh`: -- BLOCKED on §1 and §4; every assertion below is about NMI-handler output that does not exist yet
  - Build a debug kernel with `WATCHDOG_TEST_HANG=1` (intentional `for(;;){}` in Phase 2)
  - Boot QEMU headless with 45s timeout
  - Assert serial contains `"WATCHDOG: boot hung at POST"` (§4 -- NMI fired with diagnostics)
  - Assert QEMU exits (reboot triggered, `-no-reboot` causes shutdown)
- [/] Add smoke test pattern to `scripts/test-smoke.sh`: absence of `"WATCHDOG"` on normal boot (no false triggers) -- BLOCKED on §4
      - §4 owns the only code that can emit that string. Until it lands, asserting the absence of a string nothing can print is a tautological test, which the test policy forbids.
- [/] Commit: `"test: add boot watchdog test suite with intentional-hang smoke test"` -- the test-suite half shipped with §5; the intentional-hang half is blocked on §1 and §4

---

## Verification

- [ ] **Hang detection**: intentional infinite loop in Phase 2 → watchdog fires, system reboots. (manual -- bare metal with a usable ACPI WDAT)
      - Does NOT wait for §1: the shipped §5 WDAT reboots the board by itself, with no handler needed. QEMU publishes no WDAT and arms nothing, so this is unreachable from any emulator here.
- [ ] **Normal boot**: all phases complete within timeouts, no false watchdog triggers. (manual -- bare metal with a usable ACPI WDAT)
      - A QEMU smoke pass does NOT demonstrate this: with no WDAT the watchdog arms nothing, so "no false trigger" is vacuously true.
      - Measured 2026-09-03 on QEMU for what it DOES prove: the init/pet/handoff wiring is a clean no-op and boot completes in 2.740s.
- [ ] **Hang + rollback**: 3 watchdog reboots → A/B rollback to working slot. (manual -- bare metal with a usable ACPI WDAT)
      - Does NOT wait for §1 either: per §4's Notes the slot's pre-EBS try is consumed by TODO-21 before the hang, so no watchdog-side A/B write is needed.
- [ ] **TCO test**: Intel bare metal -- TCO watchdog detected and configured. (manual -- Intel bare metal, AND blocked on §7's direct-iTCO fallback, which is parked on the kernel image ceiling)
- [ ] Commit: `"boot: watchdog system complete -- no more infinite hangs"` (blocked -- §1-§4, §6 and §7 all remain deferred, so the file's promise is not yet met)
