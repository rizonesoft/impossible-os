# P0307 — Boot Splash Screen

> **Goal:** Graphical boot splash screen with centered logo, smooth progress bar,
> animated boot log, F8 recovery menu, and post-boot transition to desktop.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

> [!NOTE]
> The BSOD icon is already embedded as a C array (`bsod_icon.h`). The boot splash
> uses the same technique: embed the Impossible OS logo as a BGRA C array at
> compile time — no filesystem I/O needed during early boot.

---

## 1. Boot Splash Renderer

**Prompt:** Create the graphical boot splash screen that displays from early boot until the desktop is ready. The splash uses direct framebuffer writes — no gfx library, no font manager (they aren't initialized yet). The logo is a pre-compiled BGRA pixel array. The background is a vertical dark gradient (`#0A0A14` → `#161625`). A centered loading spinner (12 rotating dots, like Windows 11) provides visual feedback. A thin progress bar at the bottom fills from 0→100% as boot milestones are reached. The splash finishes with a fade-out transition when the desktop is ready. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: graphical boot splash"`. Add notes directly in this TODO section covering the early-boot framebuffer constraints, logo embedding, and spinner timing.

> **Beats:** Linux Plymouth uses a separate daemon and scripts. Windows winload.exe boot splash is fixed. Impossible OS: inline kernel boot splash, no separate process, fully customizable.

- [ ] Create `src/kernel/boot_splash.c` and `include/kernel/boot_splash.h`
- [ ] `boot_splash_init()` — clear screen, draw gradient background, center logo
- [ ] Logo: compile `assets/logo/impossible_os_logo.png` → BGRA C array `boot_splash_logo.h`
  - [ ] Tools: add `make splash-logo` rule (python PIL/pillow or imagemagick)
  - [ ] Logo size: 256×256px BGRA, alpha-blended onto gradient background
- [ ] Gradient background: `#0A0A14` at top → `#161625` at bottom (simple scanline interpolation)
- [ ] Loading spinner: 12 dots in a circle (~60px radius), rotate 1 position every 80ms
  - [ ] Use PIT tick counter for timing (no scheduler dependency)
- [ ] `boot_splash_progress(pct)` — update progress bar (0–100)
- [ ] `boot_splash_status(msg)` — update status text below logo (simple PSF font, pre-TrueType)
- [ ] `boot_splash_finish()` — fade to black (8 steps × 16ms), then hand off to desktop
- [ ] Commit: `"kernel: graphical boot splash"`

---

## 2. Boot Progress Milestones

**Prompt:** Define a set of boot milestones that map to progress percentages. `boot_splash_milestone(id)` advances the progress bar to the milestone's percentage. Each milestone briefly shows a status message. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: boot progress milestones"`. Add notes directly in this TODO section covering the milestone table and PIT-based timing.

- [ ] Define milestone table:
  - [ ] `BOOT_MILESTONE_PMM` = 10% — "Initializing memory..."
  - [ ] `BOOT_MILESTONE_ACPI` = 20% — "Reading hardware tables..."
  - [ ] `BOOT_MILESTONE_DRIVERS` = 35% — "Loading drivers..."
  - [ ] `BOOT_MILESTONE_FS` = 50% — "Mounting filesystems..."
  - [ ] `BOOT_MILESTONE_NETWORK` = 60% — "Starting network..."
  - [ ] `BOOT_MILESTONE_REGISTRY` = 70% — "Loading configuration..."
  - [ ] `BOOT_MILESTONE_DESKTOP` = 85% — "Starting desktop..."
  - [ ] `BOOT_MILESTONE_DONE` = 100% — "Welcome"
- [ ] Call `boot_splash_milestone()` from each corresponding init function
- [ ] Commit: `"kernel: boot progress milestones"`

---

## 3. Serial Boot Log *(parallel with splash)*

**Prompt:** During graphical boot, the kernel should still emit all boot messages to the serial port (`-serial stdio` in QEMU) for debugging. The boot splash runs on the primary framebuffer. Serial output continues uninterrupted. `klog()` always writes to serial. Boot splash messages are low-priority visual only. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: boot log to serial"`.

- [ ] `klog(fmt, ...)` writes to serial port (COM1, I/O port 0x3F8)
  - [ ] Is this already implemented? Verify and document current serial output behavior.
- [ ] `boot_splash_status(msg)` also calls `klog(msg)` — single source of truth
- [ ] QEMU: `-serial stdio` flag (already in Makefile/build.sh) captures all boot messages
- [ ] Commit: `"kernel: boot log to serial"`

---

## 4. F8 Boot Menu & Recovery

**Prompt:** Press F8 during boot (within the first 2 seconds) to show a text-mode boot menu. Options: (1) Normal boot, (2) Safe Mode (disable non-essential drivers), (3) Recovery Mode (skip registry + filesystem, boot to recovery shell), (4) Boot Last Known Good Configuration. In safe mode: skip network init, skip desktop shell, boot to terminal only. Menu uses PSF bitmap font (no TrueType dependency). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: F8 boot menu"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> **Production requirement:** Every operating system needs a recovery path when the normal
> boot sequence fails. Windows has F8/WinRE, Linux has GRUB recovery. Without this,
> a bad driver or corrupt registry makes the system permanently unbootable.

- [ ] Poll keyboard during boot splash init (first 2 seconds): F8 → show boot menu
- [ ] Text-mode boot menu: dark background, numbered options (PSF font rendering)
  - [ ] (1) Normal Boot — continue boot sequence
  - [ ] (2) Safe Mode — set `BOOT_MODE_SAFE` flag, skip network + GPU drivers
  - [ ] (3) Recovery Shell — skip desktop, boot to text-mode shell with diagnostic commands
  - [ ] (4) Last Known Good — restore Registry backup `HKLM.backup` from last successful boot
- [ ] In Safe Mode: display "Safe Mode" watermark in corner of desktop
- [ ] Registry: `HKLM\SYSTEM\Boot\LastKnownGoodEnabled` + `HKLM.backup` hive copy
- [ ] Commit: `"kernel: F8 boot menu"``

---

## Priority Order

| Priority | Section                      | Reason                                         |
|----------|------------------------------|------------------------------------------------|
| 🔴 P0    | §1 Boot Splash Renderer      | Core visual — replaces blank screen on boot    |
| 🔴 P0    | §2 Boot Progress Milestones  | Shows meaningful progress rather than spinner  |
| 🟠 P1    | §3 Serial Boot Log           | Parallel debug output — critical for dev       |
| 🟠 P1    | §4 F8 Boot Menu & Recovery  | **Production requirement** — recovery boot path |

---

## Key Files

| File                               | Purpose                               |
|------------------------------------|---------------------------------------|
| `src/kernel/boot_splash.c`         | [NEW] Boot splash renderer            |
| `include/kernel/boot_splash.h`     | [NEW] Boot splash API header          |
| `include/kernel/boot_splash_logo.h`| [NEW] Compiled-in logo BGRA array     |
| `assets/logo/impossible_os_logo.png` | [NEW] Source logo PNG               |
| `tools/png2bootsplash.py`          | [NEW] PNG → BGRA C array converter    |

---

## OS Comparison

| Feature                        | 🪟 Windows 11 (winload/bootsect) | 🐧 Linux (Plymouth / GRUB)         | 🚀 Impossible OS                       |
| ------------------------------ | ------------------------------- | --------------------------------- | ------------------------------------- |
| Graphical boot splash          | ✅ winload.exe + boot animation  | ✅ Plymouth with themes            | ⬜ §1 P0 — embedded logo + gradient    |
| Loading spinner/animation      | ✅ Rotating dots                 | ✅ Plymouth throbber / spinner     | ⬜ §1 P0 — 12-dot spinner              |
| Boot progress bar              | ✅ Thin progress bar             | ✅ Plymouth progress bar           | ⬜ §1-2 P0 — milestone-driven          |
| Status messages during boot    | ❌ (hidden from user)            | ✅ Plymouth + `quiet` kernel param | ⬜ §3 P1 — serial + splash status      |
| Fade-out transition to desktop | ✅ Smooth fade                   | ✅ Plymouth deactivate             | ⬜ §1 P0 — 8-step fade                 |
| F8 boot menu / recovery mode   | ✅ Advanced startup via F8/WinRE | ✅ GRUB + rescue.cfg               | ⬜ §4 P1 — **built into kernel**       |
| Safe Mode                      | ✅ via F8 → WinPE                | ✅ `systemd.unit=rescue.target`    | ⬜ §4 P1 — **production requirement**  |
| **Logo embedded in kernel**    | ✅ (winload)                     | ❌ Plymouth loads from disk        | ⬜ **§1 — zero filesystem dependency** |
| **No separate daemon**         | ❌ winload.exe is separate       | ❌ Plymouth is a separate process  | ⬜ **§1 — inline kernel, no daemon**   |
