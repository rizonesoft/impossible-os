# P0004 — Power Management

> **Goal:** Implement clean shutdown, restart, sleep, hibernate, and power
> management features — from kernel-level ACPI integration through UI controls
> and power profiles.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

> [!IMPORTANT]
> **Consolidated from:** TODO-P0101-General.md §8, TODO-P0202-GUI.md §7.3,
> TODO-Phase-89.md §4.3 (power.spl), TODO-Phase-97.md §11.2,
> TODO-P0103-Filesystem.md §8.1 (cache_flush), TODO-P2501-Installer-ISO.md §2 (Test 8).
> The original sections in those files should be replaced with cross-references
> pointing here.

---

## 1. ACPI Power-Off & Reboot

> **Foundation.** Everything else depends on this — the raw mechanism to power
> off or restart the machine.

**Prompt:** Parse the ACPI RSDP → RSDT/XSDT → FADT to locate `PM1a_CNT_BLK` and `PM1b_CNT_BLK`. To power off, read `SLP_TYPa` from the `\_S5` object in the DSDT/SSDT (or fall back to the QEMU/Bochs shortcut `outw(0x604, 0x2000)`), then write `SLP_TYP | SLP_EN` to PM1a/b control registers. To reboot, write `0x06` to keyboard controller port `0x64` (PS/2 reset), or use ACPI FADT `RESET_REG` if available. Expose two kernel functions: `acpi_poweroff(void)` and `acpi_reboot(void)`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: ACPI power-off and reboot"`. Create or update documentation in `docs/architecture/power.md` covering ACPI table parsing, power-off, and reboot.


- [ ] Parse ACPI RSDP → RSDT/XSDT → FADT for PM1a/b control block addresses
- [ ] Implement `acpi_poweroff(void)`:
  - [ ] Read `SLP_TYPa` from DSDT `\_S5` (or hardcode QEMU fallback `0x2000`)
  - [ ] Write `SLP_TYP | SLP_EN` to `PM1a_CNT_BLK`
  - [ ] If `PM1b_CNT_BLK` exists, write there too
- [ ] Implement `acpi_reboot(void)`:
  - [ ] Try ACPI FADT `RESET_REG` first (if available)
  - [ ] Fallback: write `0x06` to I/O port `0x64` (PS/2 keyboard reset)
- [ ] QEMU shortcut fallback: `outw(0x604, 0x2000)` if FADT not found
- [ ] Commit: `"kernel: ACPI power-off and reboot"`

---

## 2. Clean Shutdown Sequence

> **Depends on:** §1 (ACPI power-off/reboot)
> **Originally:** TODO-P0101-General.md §8.1

**Prompt:** A clean shutdown prevents data loss by flushing everything before powering off. The sequence must be: send WM_CLOSE to all GUI apps (giving them a chance to prompt "Save work?"), wait up to 5 seconds then force-kill remaining apps, flush all open file handles, flush the Registry to disk, release the DHCP lease, unmount all filesystems, sync disk caches, and finally issue the ACPI power-off. For QEMU/Bochs the shortcut is `outw(0x604, 0x2000)` but for real hardware parse the ACPI FADT for the PM1a_CNT_BLK address and write SLP_TYP | SLP_EN. Show a "Shutting down..." screen during cleanup. The `shutdown` and `reboot` shell commands should trigger this sequence. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: clean shutdown sequence"`. Update `README.md` if it contains stale or incorrect references to shutdown or power management. Create or update documentation in `docs/` covering the shutdown sequence, ACPI power-off, and graceful app termination.


- [ ] Implement `system_shutdown(void)` orchestrator function:
  - [ ] Send `WM_CLOSE` to all GUI apps (save work prompt)
  - [ ] Implement shutdown timeout (force-kill apps after 5 seconds)
  - [ ] Flush all open file handles
  - [ ] Flush Registry to disk (`registry_save_all()`)
  - [ ] Stop network services (release DHCP lease)
  - [ ] Flush disk caches (`cache_flush()` — see §6)
  - [ ] Unmount all filesystems
  - [ ] Call `acpi_poweroff()`
- [ ] Implement `system_reboot(void)` — same flush sequence, then `acpi_reboot()`
- [ ] Display "Shutting down..." / "Restarting..." screen during cleanup
- [ ] Commit: `"kernel: clean shutdown sequence"`

---

## 3. Shell Commands: `shutdown` & `reboot`

> **Depends on:** §2 (clean shutdown sequence)

**Prompt:** Add `shutdown` and `reboot` commands to the shell. `shutdown` calls `system_shutdown()`, `reboot` calls `system_reboot()`. Both display a confirmation message before proceeding. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: shutdown and reboot commands"`.


- [ ] `shutdown` command → confirmation → call `system_shutdown()`
- [ ] `reboot` command → confirmation → call `system_reboot()`
- [ ] Commit: `"shell: shutdown and reboot commands"`

---

## 4. Start Menu Power Submenu

> **Depends on:** §2 (clean shutdown sequence)
> **Originally:** TODO-P0202-GUI.md §7.3 (Start Menu Interaction)

**Prompt:** The Power button (⏻) in the Start Menu's bottom-right area opens a fly-out submenu with: Shut Down, Restart, Sleep, Lock. Each action calls the corresponding kernel function. Sleep and Lock may be grayed out until those features are implemented in §7. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: start menu power actions"`.


- [ ] Power button (⏻) → fly-out submenu:
  - [ ] **Shut Down** → `system_shutdown()`
  - [ ] **Restart** → `system_reboot()`
  - [ ] **Sleep** → `acpi_sleep()` (grayed until §7 implemented)
  - [ ] **Lock** → lock screen (grayed until lock screen implemented)
- [ ] Commit: `"desktop: start menu power actions"`

---

## 5. Power Settings Applet (`power.spl`)

> **Depends on:** §2 (shutdown/reboot), Settings Panel framework (TODO-Phase-89.md §4)
> **Originally:** TODO-Phase-89.md §4.3 (Core Applets — power.spl)

**Prompt:** The `power.spl` settings applet provides a UI for power settings: screen timeout slider, shutdown/restart buttons, and sleep settings. Each setting reads/writes Registry keys under `HKLM\SYSTEM\Power\`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: power settings applet"`.


- [ ] `power.spl.c` — Settings Panel applet:
  - [ ] Screen timeout slider (minutes: 1, 2, 5, 10, 15, 30, Never)
  - [ ] Shutdown button → `system_shutdown()`
  - [ ] Restart button → `system_reboot()`
  - [ ] Sleep settings (when §7 is available)
- [ ] Registry: `HKLM\SYSTEM\Power\ScreenTimeout = 10`
- [ ] Commit: `"apps: power settings applet"`

---

## 6. Disk Cache Integration

> **Depends on:** §2 (shutdown calls `cache_flush()`)
> **Originally:** TODO-P0103-Filesystem.md §8.1 (Disk Cache)

> [!NOTE]
> The disk cache itself is implemented in TODO-P0103-Filesystem.md §8.1. This
> section only documents the **shutdown integration** — `cache_flush()` must be
> called during the clean shutdown sequence in §2 to write all dirty blocks.

- [ ] Ensure `system_shutdown()` calls `cache_flush()` before unmount
- [ ] Ensure `system_reboot()` calls `cache_flush()` before unmount
- [ ] Set filesystem "dirty" flag on mount, clear on clean unmount

---

## 7. Sleep & Hibernate (Future)

> **Depends on:** §1 (ACPI), §2 (clean shutdown for state-save)
> **Originally:** TODO-P0101-General.md §8.2

**Prompt:** These are stretch goals. Sleep requires ACPI S3 suspend-to-RAM which involves saving all device state and entering the S3 state via the PM1a control register — on wake, the CPU resumes at the FACS waking vector. Hibernate (S4) writes all physical memory to `C:\Impossible\System\hiberfil.sys`, then powers off — on boot, the bootloader detects the file and restores memory. The lock screen is related: stop rendering the desktop, display the login/password UI overlay, and resume on correct password (check against Registry credentials from Phase 09). For QEMU testing, S3 can be simulated with `-global ICH9-LPC.disable_s3=0`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: sleep and hibernate"`. Create or update documentation in `docs/` covering ACPI S3 suspend, S4 hibernate, waking vector, and lock screen UI flow.


- [ ] *(Stretch)* **Sleep (ACPI S3)** — suspend to RAM:
  - [ ] Save device state (PCI, framebuffer, network)
  - [ ] Set FACS waking vector to resume entry point
  - [ ] Write `SLP_TYP_S3 | SLP_EN` to PM1a control register
  - [ ] On wake: restore device state, resume compositor
  - [ ] QEMU flag: `-global ICH9-LPC.disable_s3=0`
- [ ] *(Stretch)* **Hibernate (ACPI S4)** — suspend to disk:
  - [ ] Save all physical memory to `C:\Impossible\System\hiberfil.sys`
  - [ ] Power off via `acpi_poweroff()`
  - [ ] On boot: detect hiberfil.sys → restore memory → resume
- [ ] *(Stretch)* **Lock Screen** — password prompt, Registry credential check
- [ ] Commit: `"kernel: sleep and hibernate"`

---

## 8. Power Management Profiles

> **Depends on:** §5 (power.spl), §7 (sleep)
> **Originally:** TODO-Phase-97.md §11.2

**Prompt:** Profiles: Balanced (default), Performance (no sleep), Power Saver (aggressive sleep). Control display timeout and CPU throttling. `power.spl` settings applet. Registry: `HKLM\SYSTEM\Power\Profile = "Balanced"`. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"kernel: power management profiles"`.


- [ ] Power profiles: Balanced (default), Performance (no sleep), Power Saver (aggressive sleep)
- [ ] Profiles control: display sleep timeout, CPU throttling (if applicable)
- [ ] Integrate into `power.spl` settings applet (§5): profile dropdown selector
- [ ] Registry: `HKLM\SYSTEM\Power\Profile = "Balanced"`
- [ ] Commit: `"kernel: power management profiles"`

---

## 9. Hyper-V Power Validation

> **Depends on:** §2 (shutdown), §7 (sleep — if implemented)
> **Originally:** TODO-P2501-Installer-ISO.md §2 (Test 8)

- [ ] **Test:** Graceful ACPI shutdown from shell (`shutdown` command)
- [ ] **Test:** Graceful ACPI reboot from shell (`reboot` command)
- [ ] **Test:** Start Menu → Power → Shut Down
- [ ] **Test:** Start Menu → Power → Restart
- [ ] **Test:** BSOD auto-restart — trigger panic (`BSOD_TEST`), verify system reboots automatically after timeout
- [ ] *(If implemented)* **Test:** Sleep → wake → verify state preserved
- [ ] Document any Hyper-V-specific ACPI issues

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1. ACPI Power-Off & Reboot | Foundation — raw mechanism |
| 🔴 P0 | 2. Clean Shutdown Sequence | Data integrity — prevents corruption |
| 🟠 P1 | 3. Shell Commands | User-facing `shutdown`/`reboot` |
| 🟠 P1 | 4. Start Menu Power Submenu | Desktop UI power actions |
| 🟡 P2 | 5. Power Settings Applet | Settings UI |
| 🟡 P2 | 6. Disk Cache Integration | Data safety during shutdown |
| 🟢 P3 | 7. Sleep & Hibernate | ACPI S3/S4 — stretch goal |
| 🔵 P4 | 8. Power Profiles | Polish — Balanced/Performance/Saver |
| 🔵 P4 | 9. Hyper-V Validation | Final testing |
