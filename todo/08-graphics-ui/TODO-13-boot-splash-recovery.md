---
schema_version: 1
id: boot-splash-recovery
domain: 08-graphics-ui
status: active
title: "TODO-13 -- Boot Splash & F8 Recovery"
---

# TODO-13 -- Boot Splash & F8 Recovery

> **Goal:** Complete the graphical boot experience and the F8 recovery path. BSOD core (panic, crash dump, auto-restart) is done. This TODO adds: `boot_splash_progress(pct)` thin progress bar + 8-step fade-to-black in `boot_splash_finish()`, a logo build pipeline (`tools/png2bootsplash.py`), boot progress milestone constants wired into subsystem inits, an early-boot F8 text menu (PS/2 raw polling before keyboard IRQ), Registry-based crash loop protection, and a BSOD auto-restart validation test plan.

> [!IMPORTANT]
> **Already implemented** -- do not re-implement: arc ring spinner (`include/kernel/spinner.h`, `include/kernel/gfx/arc_ring.h`), `boot_font_render()` TTF status text, `boot_splash_init/status/tick/finish/abort/active` stubs, `boot_splash_start_animation()`. **Missing from `boot_splash.c`**: `boot_splash_progress(pct)` (no thin progress bar yet), milestone constants, fade-to-black (stub exists but fade loop not implemented), logo build pipeline, F8 boot menu, crash loop protection. `klog(level, subsystem, fmt)` from `include/kernel/klog.h` is the serial logging function. `uefi_reboot()` from `include/kernel/uefi_runtime.h` is the reboot call. `keyboard_inject_scancode()` in `include/kernel/drivers/keyboard.h` exists, but F8 polling at early boot requires direct PS/2 port reads (IRQ not yet live). Complete sections in order: build pipeline → splash renderer → spinner confirm → milestones → F8 menu → crash loop → BSOD validation.

## Inputs

- `src/kernel/boot_splash.c` + `include/kernel/boot_splash.h` -- extend with `boot_splash_progress()` and full fade-to-black in §1
- `include/kernel/spinner.h` -- `spinner_draw_faded(uint8_t fade)` used during §2 fade path; `spinner_is_active()`, `spinner_stop()`
- `include/kernel/gfx/arc_ring.h` -- arc ring sizing constants already used by `boot_splash.c`
- `include/kernel/klog.h` -- `klog(LOG_INFO, "boot", msg)` called from `boot_splash_milestone()` so serial output continues during graphical boot
- `include/kernel/uefi_runtime.h` -- `uefi_reboot()` called by crash loop protection when auto-restart is enabled
- `include/registry.h` -- `RegGetValue()`/`RegSetValueEx()` for §6 crash counter and §7 validation
- `tools/` directory -- §3 adds `tools/png2bootsplash.py` alongside existing `tools/convert_boot_font.py`, `tools/convert_bsod_icon.py`
- → XREF: `01-boot-platform/TODO-14-boot-diagnostics.md` -- serial boot log behavior; `boot_splash_status()` must call `klog()` to keep serial output uninterrupted
- → XREF: `02-kernel-core/TODO-27-crash-dump-generation.md` -- BSOD core (done); §7 validates the auto-restart flow end-to-end

## Outcome

- `boot_splash_progress(uint8_t pct)` draws a thin bar at the bottom of the framebuffer; all 8 milestones are wired into their subsystem inits.
- `boot_splash_finish()` performs the 8-step fade-to-black using `spinner_draw_faded()`.
- Logo build pipeline: `make splash-logo` generates `src/kernel/boot_splash_logo.h` from `assets/logo/impossible_os_logo.png`.
- F8 during early boot (within 2 s) shows a text-mode numbered menu: Normal / Safe Mode / Recovery Shell / Last Known Good.
- Crash loop protection and BSOD validation moved to `02-kernel-core/TODO-28-bsod-ux-enhancements.md`.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                         | Depends On                                                                           | Status |
| --- | :---: | --------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ | :----: |
| 💎  |   1   | §3 Build pipeline -- `tools/png2bootsplash.py`, `assets/logo/`, `make splash-logo` Makefile rule   | Nothing; standalone host-side tool                                                   |  [ ]   |
| 💎  |   2   | §1 Boot splash renderer -- `boot_splash_progress(pct)`, thin bar, fade-to-black in `boot_splash_finish()` | §3 (logo C array must exist before splash can show it at runtime)            |  [ ]   |
| 💎  |   3   | §2 Loading spinner -- confirm `spinner_draw_faded()` integrated into fade path; no re-implementation | §1 (fade loop calls `spinner_draw_faded()` at each of 8 steps)                       |  [ ]   |
| 💎  |   4   | §4 Boot progress milestones -- `BOOT_MILESTONE_*` constants + `boot_splash_milestone()` call sites | §1 (`boot_splash_progress()` must exist before milestones can drive it)               |  [ ]   |
| ⭐  |   5   | §5 F8 boot menu -- early PS/2 polling, text-mode menu, `boot_mode_t`, boot flag propagation        | §1 (splash must be functional so F8 path can abort it cleanly)                       |  [ ]   |
| 💎  |   6   | ~~§6 Crash loop protection~~ → MOVED to `TODO-21 §7-§8`   | --       |  N/A   |
| 💎  |   7   | ~~§7 BSOD validation~~ → MOVED to `TODO-21` Verification   | --       |  N/A   |

---

## 1. Boot Splash Renderer `[Sonnet]`

Extend existing `boot_splash.c` with: (a) `boot_splash_progress(uint8_t pct)` thin bar (6 px tall, full-width, accent blue, bottom-8 px of screen), (b) full 8-step fade-to-black in `boot_splash_finish()` using PIT delay between frames, (c) logo alpha-blend from generated `boot_splash_logo.h` C array.

**Files:** `src/kernel/boot_splash.c` (extend), `include/kernel/boot_splash.h` (extend)

> [!NOTE]
> `boot_splash_progress()` draws directly to the framebuffer: scanline-fill `6 px × fb_width` at `fb_height - 8`; bar width = `fb_width * pct / 100`; filled in Fluent Blue (`0xFF0078D4`); unfilled track in dark gray (`0xFF1A1A28`). Must be callable from any subsystem init (no scheduler, no locks). Fade-to-black in `boot_splash_finish()`: 8 iterations; each iteration: call `spinner_draw_faded(255 - i * 32)` where `i` goes 0→7; overlay each pixel with `alpha_blend(pixel, 0xFF000000, i * 32)` scanning the full framebuffer; call `pit_delay_ms(16)` between steps (PIT-based; no scheduler needed). After fade: call `spinner_stop()`; clear framebuffer to black. The logo C array (`boot_splash_logo.h`) is `#include`d in `boot_splash.c`; if the file doesn't exist yet (pre-build-pipeline): provide a 16×16 placeholder solid-blue square so the file compiles.

- [ ] `void boot_splash_progress(uint8_t pct)` in `src/kernel/boot_splash.c`: scanline fill; update static `g_progress_pct` to avoid full redraw if pct unchanged
- [ ] Add `void boot_splash_progress(uint8_t pct);` declaration to `include/kernel/boot_splash.h`
- [ ] `boot_splash_finish()`: implement 8-step fade loop: `spinner_draw_faded(255 - step*32)` + framebuffer alpha-overlay + `pit_delay_ms(16)` per step; then spinner_stop; clear to black
- [ ] `boot_splash_init()`: `#include "kernel/boot_splash_logo.h"` and alpha-blend logo center at `(fb_w/2 - 128, fb_h/2 - 200)` onto gradient; if logo array is zero-size placeholder: skip silently
- [ ] `boot_splash_status(msg)` already calls `boot_font_render`; add `klog(LOG_INFO, "boot", "%s", msg)` at the top of the function body so serial output is never lost
- [ ] Commit: `"boot/splash: progress bar, fade-to-black, logo alpha-blend, klog in status"`

## 2. Loading Spinner `[Sonnet]`

The arc ring spinner is fully implemented via `spinner.c` + `arc_ring.h`. This section confirms `spinner_draw_faded(uint8_t fade)` is integrated into the fade path, documents the existing API for implementors, and adds a `spinner_draw_faded` call-site in the fade loop from §1.

**Files:** `src/kernel/boot_splash.c` (confirm/extend)

> [!NOTE]
> `spinner_draw_faded(fade)` in `include/kernel/spinner.h` renders the arc ring at reduced opacity -- `fade=255` is full brightness, `fade=0` is invisible. It is called in the fade loop from `boot_splash_finish()` (§1 above) to make the spinner fade out simultaneously with the background. No new spinner implementation is needed. If `spinner_draw_faded()` is not yet implemented in `spinner.c` (verify): implement it as `spinner_advance()` with the ring color alpha-scaled by `fade/255` using `gfx_color_t` alpha manipulation. This section is done when the fade-out in §1 makes the spinner smoothly disappear over the 8 frames.

- [ ] Verify `spinner_draw_faded()` is implemented in `src/kernel/spinner.c`; if stub only: implement alpha-scaled arc ring render
- [ ] Confirm `boot_splash_finish()` fade loop (from §1) calls `spinner_draw_faded(255 - step*32)` at each of 8 steps
- [ ] Add a comment in `boot_splash.h`: `/* spinner_draw_faded() defined in spinner.h -- used by boot_splash_finish() fade loop */`
- [ ] QEMU: splash fades to black smoothly over ~128 ms; arc ring fades with background
- [ ] Commit: `"boot/splash: confirm spinner_draw_faded integrated into fade-out path"`

## 3. Build Pipeline `[Sonnet]`

`tools/png2bootsplash.py` converts `assets/logo/impossible_os_logo.png` to a BGRA C array `src/kernel/boot_splash_logo.h`. `make splash-logo` Makefile target runs the tool. Placeholder blue-square logo until final artwork exists.

**Files:** `tools/png2bootsplash.py` (new), `assets/logo/impossible_os_logo.png` (placeholder), Makefile (extend)

> [!NOTE]
> The tool follows the same pattern as `tools/convert_bsod_icon.py` (which generates `bsod_icon.h`). Output format: `static const uint32_t boot_splash_logo_pixels[256*256] = { 0xAARRGGBB, ... };` and `static const uint32_t boot_splash_logo_width = 256;` and `static const uint32_t boot_splash_logo_height = 256;`. Python: `from PIL import Image; img = Image.open(path).convert("RGBA").resize((256,256))`; iterate pixels: ARGB32 = `(a<<24)|(r<<16)|(g<<8)|b`. Placeholder: create a solid `#0078D4` 256×256 PNG as `assets/logo/impossible_os_logo.png` -- satisfies the build dependency so the OS compiles without final artwork. Makefile rule: `.PHONY: splash-logo` → `python3 tools/png2bootsplash.py assets/logo/impossible_os_logo.png src/kernel/boot_splash_logo.h`.

- [ ] Create `assets/logo/` directory and add `impossible_os_logo.png` placeholder (256×256, `#0078D4` solid blue)
- [ ] `tools/png2bootsplash.py`: read PNG → resize 256×256 → RGBA → emit C array `boot_splash_logo.h` in same format as `bsod_icon.h`
- [ ] Add `splash-logo` target to `Makefile`: `$(PYTHON) tools/png2bootsplash.py assets/logo/impossible_os_logo.png src/kernel/boot_splash_logo.h`
- [ ] Add `src/kernel/boot_splash_logo.h` to `.gitignore` (generated file); add `impossible_os_logo.png` to git
- [ ] `bash scripts/build.sh clean` with placeholder → `=== BUILD OK ===`
- [ ] Commit: `"build: splash logo pipeline -- png2bootsplash.py, placeholder logo, make splash-logo target"`

## 4. Boot Progress Milestones `[Sonnet]`

8 `BOOT_MILESTONE_*` constants mapping to progress percentages and status strings. `boot_splash_milestone(id)` calls `boot_splash_progress(pct)` + `boot_splash_status(msg)` + `boot_splash_tick()`. Called from each subsystem's `_init()` function.

**Files:** `include/kernel/boot_splash.h` (extend), `src/kernel/main/boot_hw.c`, `boot_storage.c`, `boot_desktop.c` (extend)

> [!NOTE]
> Milestone table: `PMM=10 "Initializing memory..."`, `ACPI=20 "Reading hardware tables..."`, `DRIVERS=35 "Loading drivers..."`, `FS=50 "Mounting filesystems..."`, `NETWORK=60 "Starting network..."`, `REGISTRY=70 "Loading configuration..."`, `DESKTOP=85 "Starting desktop..."`, `DONE=100 "Welcome to Impossible OS"`. Each milestone call also triggers `klog(LOG_INFO, "boot", "[%u%%] %s", pct, msg)` so serial output mirrors the splash. `boot_splash_tick()` advances the arc ring animation one frame between milestones so the spinner keeps rotating during blocking init.

- [ ] `#define BOOT_MILESTONE_PMM 0` through `BOOT_MILESTONE_DONE 7` enum values in `boot_splash.h`
- [ ] `static const uint8_t BOOT_MILESTONE_PCT[8] = {10, 20, 35, 50, 60, 70, 85, 100}` and `static const char* BOOT_MILESTONE_MSG[8] = {...}` in `boot_splash.c`
- [ ] `void boot_splash_milestone(int id)` in `boot_splash.c`: range-check `id`; call `boot_splash_progress(PCT[id])`; `boot_splash_status(MSG[id])`; `boot_splash_tick()`
- [ ] Add `void boot_splash_milestone(int id);` to `boot_splash.h`
- [ ] Call sites: `pmm_init()` → `BOOT_MILESTONE_PMM`; `acpi_init()` → `ACPI`; `drivers_init()` → `DRIVERS`; `vfs_init()` → `FS`; `net_init()` → `NETWORK`; `registry_init()` → `REGISTRY`; `desktop_init()` → `DESKTOP`; `desktop_ready()` → `DONE` + `boot_splash_finish()`
- [ ] Commit: `"boot/splash: BOOT_MILESTONE_* constants, boot_splash_milestone(), wired into subsystem inits"`

## 5. F8 Boot Menu `[Opus]`

Poll PS/2 keyboard data port (0x60) for 2 seconds during early boot. F8 key detected → draw text-mode boot menu (dark bg, PSF/boot_font, numbered options). Selections: (1) Normal, (2) Safe Mode, (3) Recovery Shell, (4) Last Known Good. Set `g_boot_mode` global; propagate to all subsequent init steps.

**Files:** `src/kernel/boot_f8.c` (new), `include/kernel/boot_f8.h` (new), `src/kernel/main/boot_hw.c` (extend)

> [!NOTE]
> This is `[Opus]` -- early-boot keyboard polling runs before PS/2 IRQ (IRQ 1) is live and before the keyboard driver is initialized. Use direct PS/2 port I/O: `while (inb(0x64) & 1) { scan = inb(0x60); ... }` (status port 0x64, data port 0x60). F8 scan code: `0x42` (make), `0xC2` (break). Poll loop: `uint64_t start = system_get_ticks(); while ((system_get_ticks() - start) < 20) { /* 20 ticks = 200 ms at 100 Hz; do 10 × 200 ms = 2 s total */ ... }`. Text-mode menu: uses `boot_font_render()` to draw numbered options directly to framebuffer (no gfx lib); draw dark background (`gfx_fill_rect` if gfx is available, else manual scanline) then menu lines. **Boot mode flags**: `typedef enum { BOOT_MODE_NORMAL=0, BOOT_MODE_SAFE, BOOT_MODE_RECOVERY, BOOT_MODE_LAST_KNOWN_GOOD } boot_mode_t;` in `boot_f8.h`; `extern boot_mode_t g_boot_mode;` -- checked by all subsequent init functions. Safe mode: `BOOT_MODE_SAFE` skips network init, skips desktop, boots to terminal only, draws "Safe Mode" watermark text in all four screen corners. Recovery Shell: `BOOT_MODE_RECOVERY` skips registry + filesystem init, goes directly to serial-attached `recovery_shell()`. Last Known Good: before normal init, copy `HKLM.backup` over `HKLM` Registry hive file.

- [ ] `boot_mode_t` enum + `extern boot_mode_t g_boot_mode;` in `include/kernel/boot_f8.h`
- [ ] `void boot_f8_poll(void)` in `src/kernel/boot_f8.c`: 10-iteration loop; each: scan PS/2 status+data ports; if F8 scan code `0x42` detected: call `boot_f8_show_menu()`; if no F8 in 2 s: `g_boot_mode = BOOT_MODE_NORMAL`; return
- [ ] `void boot_f8_show_menu(void)` -- clear framebuffer to `0xFF1A1A28`; use `boot_font_render()` to draw header "Impossible OS Recovery" and 4 numbered options; poll for `1`–`4` or arrow key + Enter; set `g_boot_mode`
- [ ] Number key scancodes for early polling: `0x02`=1, `0x03`=2, `0x04`=3, `0x05`=4
- [ ] `boot_f8_apply_mode()`: check `g_boot_mode`; if `LAST_KNOWN_GOOD`: `vfs_copy("C:\\HKLM.backup", "C:\\HKLM")` before registry_init; if `RECOVERY`: skip to recovery shell after minimal init; if `SAFE`: set `g_skip_network=1`, `g_skip_desktop=1`
- [ ] Call `boot_f8_poll()` from `kernel_main()` immediately after `fb_init()` (framebuffer must be live) but before `boot_splash_init()` (so F8 menu can own the screen)
- [ ] Safe Mode watermark: after desktop would start, if `BOOT_MODE_SAFE`: render "Safe Mode" text at all 4 corners in red using `boot_font_render()` every frame
- [ ] Commit: `"boot/f8: early PS/2 poll, text-mode boot menu, boot_mode_t, safe/recovery/LKG modes"`

## 6. [MOVED] Crash Loop Protection

> **Moved to `02-kernel-core/TODO-28-bsod-ux-enhancements.md §7-§8`.** Crash loop tracking (NVRAM stats + consecutive crash counter) and safe mode suggestion are owned by TODO-21 alongside all other panic screen behavior.

## 7. [MOVED] BSOD Auto-Restart Validation

> **Moved to `02-kernel-core/TODO-28-bsod-ux-enhancements.md` Verification section.** BSOD test plan now uses runtime `crash_test=1` boot.conf flag (no recompile) and `scripts/debug/panic/` test runners.

---

## OS Comparison


| ⭐  | Feature                      | 🪟 Win11                                           | 🐧 Linux                                                                                    | 🚀 Impossible OS                                                |
| --- | ---------------------------- | -------------------------------------------------- | ------------------------------------------------------------------------------------------- | --------------------------------------------------------------- |
| 💎  | Graphical boot splash        | ✅ `winload.exe` boot animation; progress spinner; | ✅ Plymouth daemon; themed spinner; distro                                                  | ⬜ §1 -- arc ring spinner + `boot_splash_progress(pct)`         |
| ⭐  | Build pipeline               | ✅ Logo baked into `winload.exe` binary            | ✅ Plymouth compiles SVG/PNG into initrd                                                    | ⬜ §3 -- `⭐` `tools/png2bootsplash.py` same pattern as         |
| 💎  | Boot progress milestones     | ✅ Progress ring advances on milestones;           | ✅ Plymouth `plymouth-update` from init scripts;                                            | ⬜ §4 -- `boot_splash_milestone(id)` called from each subsystem |
| 💎  | F8 boot menu                 | ✅ F8 shows Advanced Startup menu                  | ✅ GRUB recovery entries; systemd rescue/emergency                                          | ⬜ §5 -- raw PS/2 port poll before                              |
| 💎  | Safe Mode                    | ✅ Safe Mode with Networking /                     | ✅ systemd rescue.target; init=/bin/bash; GRUB recovery                                     | ⬜ §5 -- `BOOT_MODE_SAFE` skips network + desktop               |
| 💎  | Last Known Good              | ✅ Last Known Good Configuration; restores         | ✅ No native equivalent; manual `/etc`                                                      | ⬜ §5 -- `HKLM.backup` copied over `HKLM` before                |
| 💎  | Crash loop protection        | ✅ `WinRE` automatic repair after 2                | ✅ systemd `FailureAction=reboot-force`; `MaxStartBurst=3`; `StartLimitAction=reboot-force` | ⬜ §6 -- `HKLM\SYSTEM\Recovery\ConsecutiveCrashes ≥ 3` → halt   |
| 💎  | BSOD auto-restart validation | ✅ Internal Microsoft validation suites; WER       | ✅ `kdump` test; `crash` utility; kernel                                                    | ⬜ §7 -- 6-test QEMU plan documented in                         |

> **After §1–§7:** Impossible OS has a production-quality boot experience: a kernel-native splash with a logo build pipeline identical to the BSOD icon pipeline (`bsod_icon.h`), a zero-filesystem-I/O splash that shows the logo from a compiled-in C array, a raw PS/2 F8 menu that works before any driver is initialized, and a Registry-backed crash loop counter that prevents infinite reboot loops. The `⭐` logo build pipeline advantage is that it requires zero changes to the boot sequence -- the logo is a C array included at compile time, just like Windows bakes its logo into `winload.exe`, but implementable in a single 30-line Python script.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===` (with placeholder logo)
- [ ] `make splash-logo` → `src/kernel/boot_splash_logo.h` generated with correct 256×256 BGRA array
- [ ] QEMU boot: splash shows arc ring spinner + thin blue progress bar advancing through 8 milestones; serial log mirrors each milestone `[boot] [10%] Initializing memory...`
- [ ] QEMU boot: `boot_splash_finish()` fades to black over ~128 ms; spinner fades simultaneously; no white flash
- [ ] F8 during boot: press F8 within 2 s (inject scancode `0x42` via QEMU monitor) → text-mode menu appears with 4 options
- [ ] Select "2 Safe Mode": desktop not launched; terminal boots; "Safe Mode" text in screen corners
- [ ] Select "4 Last Known Good": `HKLM.backup` restored (serial log confirms); registry_init loads backup hive
- [ ] `BSOD_TEST` enabled: BSOD screen → countdown → auto-reboot → clean boot; `ConsecutiveCrashes` reset to 0
- [ ] 3 consecutive `BSOD_TEST` crashes: 3rd crash shows halt message; system does not reboot; serial log shows `[recovery] ConsecutiveCrashes=3`
- [ ] `AutoRestart=0` Registry: BSOD halts; no reboot
- [ ] Commit: `"boot: complete boot splash, F8 recovery menu, crash loop protection -- all tests pass"`
