# TODO-02 — Boot Diagnostics, Heartbeat & Spinner

> **Goal:** The arc spinner and boot splash are done. This TODO builds the production diagnostics layer: a named-stage boot progress API that feeds the splash, POST-style hex codes visible on hardware debug cards, a debug color-bar waterfall replacing the raw `HV_BAR` hack, cross-boot panic forensics, a panic QR code, runtime vital-signs overlay, alive-blink hang detection, and a multi-instance compositor-integrated spinner — turning the ad-hoc debug tooling into production-grade features.

> [!NOTE]
> **Origin:** The HV_BAR colored pixel bars were added during Hyper-V Gen 2 debugging — crude but instantly effective. This TODO formalises that approach as an opt-in production debug feature while replacing the unconditional hack with proper structured output.

> [!IMPORTANT]
> **`boot_progress(phase, step, postcode)`** already exists in `boot_init.h` (implemented by `02-kernel-core/TODO-01 §1`). §2 of this TODO extends it with a named-stage API, ring buffer history, elapsed-ms tracking, and splash status integration — do not rewrite the lower-level call, build on it.

## Inputs

- [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h)
- [`include/kernel/boot_splash.h`](../../include/kernel/boot_splash.h)
- [`include/kernel/spinner.h`](../../include/kernel/spinner.h)
- [`include/kernel/panic.h`](../../include/kernel/panic.h)
- [`src/kernel/panic.c`](../../src/kernel/panic.c)
- [`src/kernel/boot_timing.c`](../../src/kernel/boot_timing.c)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §1` — `boot_progress(phase, step, postcode)` in `boot_init.h`; §2 of this TODO wraps it with a named-stage layer
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §7` — `boot_halt()` is the pre-FB panic anchor that §6 extends with forensic evidence
- → XREF: `02-kernel-core/TODO-16-crash-dump-generation.md` — crash dumps complement §6 panic forensics; coordinate PMM page reservation at `0x80000` to avoid collision with minidump workspace
- → XREF: `02-kernel-core/TODO-13-registry-completion.md` — `HKLM\SYSTEM\Boot\DebugBar`, `AliveBlink`, `VitalSigns` registry keys
- → XREF: `TODO-01-uefi-hardening-secureboot.md §7` — boot UX polish calls `boot_splash_status()` via the §2 API
- → XREF: `07-graphics-ui/TODO-06-window-manager.md §8` — compositor frame loop must call `spinner_tick()` on every active `g_active_spinners[]` entry per frame; §8 (Compositor Performance) is the natural owner for this per-frame integration

## Outcome

- Serial log shows `[+NNNms] BOOT_PMM: Physical memory manager ready` style entries for every major stage.
- Two-digit hex POST code visible in the top-right corner of the framebuffer from kernel entry until `BOOT_DESKTOP_READY`; same codes output to I/O port 0x80.
- `DebugBar=1` in `boot.conf` activates a proportional colored bar across the top of the screen; fades after 3 s.
- On panic: `struct panic_evidence` captured at `0x80000`; next boot saves `last-panic.txt` and shows "System shut down unexpectedly" toast.
- Panic BSOD shows a QR code in the bottom-right corner linking to the troubleshooting page.
- `AliveBlink=1` activates a 4×4 px blinking square in the top-left corner driven by the PIT handler.
- `spinner_create()` / `spinner_tick()` multi-instance API available; compositor ticks all active spinners each frame.
- `VitalSigns=1` activates a 20 px bottom-strip showing CPU %, RAM, IRQ rate, uptime, and FPS.

## Implementation Order

| ⭐  | Order | Deliverable                        | Depends On | Status |
| --- | :---: | ---------------------------------- | ---------- | :----: |
| 💎  |   1   | UEFI pre-kernel POST codes         | —          |  [ ]   |
| 💎  |   2   | Boot progress named-stage API      | §1         |  [ ]   |
| 💎  |   3   | POST-style hex code display        | §2         |  [ ]   |
| 💎  |   4   | Alive blink / hang detection       | §2         |  [ ]   |
| ⭐  |   5   | Debug color bar waterfall          | §2         |  [ ]   |
| 💎  |   6   | Panic forensic evidence            | §2         |  [ ]   |
| ⭐  |   7   | Panic QR code                      | §6         |  [ ]   |
| 💎  |   8   | System-wide multi-instance spinner | —          |  [ ]   |
| ⭐  |   9   | Runtime vital signs strip          | §8         |  [ ]   |

> 💎 = parity — Windows and Linux both have equivalent diagnostics; Impossible OS must match them.
> ⭐ = exclusive — the proportional debug waterfall with regression overlay, QR code on BSOD, and always-visible vital-signs strip are not present in either competitor at the kernel level.

---

## 1. UEFI Pre-Kernel POST Codes `[Sonnet]`

Write I/O port 0x80 POST codes from the bootloader so hardware POST-code reader cards decode boot progress before the kernel even starts.

**Files:** `src/boot/uefi/bootx64.c`

- [ ] Add `post_code(uint8_t code)` inline in `bootx64.c`: `outb(0x80, code)` using `__asm__ volatile ("outb %0, $0x80" :: "a"(code))`
- [ ] Insert `post_code()` calls at each bootloader milestone: `0x01`=entry, `0x02`=GOP init, `0x03`=ELF open, `0x04`=ELF load, `0x05`=RSDP found, `0x06`=memory map, `0x07`=ExitBootServices, `0x08`=page tables, `0x09`=kernel jump
- [ ] Verify QEMU does not fault on port 0x80 writes (QEMU ignores writes to 0x80 silently — no change needed)
- [ ] Commit: `"boot: UEFI pre-kernel POST codes to I/O port 0x80"`

## 2. Boot Progress Named-Stage API `[Sonnet]`

High-level named-stage wrapper over the existing `boot_progress()` that adds a 32-entry ring buffer, elapsed-ms tracking, and `boot_splash_status()` forwarding.

**Files:** `include/kernel/boot_progress.h`, `src/kernel/main/boot_progress.c`

- [ ] Define `boot_stage_t` enum: `BOOT_STAGE_UEFI_INIT`, `BOOT_STAGE_ELF_LOADED`, `BOOT_STAGE_KERNEL_ENTRY`, `BOOT_STAGE_GDT_IDT`, `BOOT_STAGE_APIC`, `BOOT_STAGE_PMM`, `BOOT_STAGE_VMM`, `BOOT_STAGE_HEAP`, `BOOT_STAGE_KLOG`, `BOOT_STAGE_VFS`, `BOOT_STAGE_REGISTRY`, `BOOT_STAGE_DRIVERS`, `BOOT_STAGE_NETWORK`, `BOOT_STAGE_SCHEDULER`, `BOOT_STAGE_DESKTOP_READY`
- [ ] Map each `boot_stage_t` to a postcode constant from `boot_init.h` and a percentage (0–100) for splash progress
- [ ] `boot_stage_report(boot_stage_t stage, const char *msg)`: calls `boot_progress(phase, msg, postcode)`, records entry in 32-slot ring buffer `boot_stage_history[]` with TSC tick and stage, calls `boot_splash_status(msg)` if splash is active
- [ ] `boot_get_elapsed_ms()`: returns ms since the `BOOT_STAGE_KERNEL_ENTRY` timestamp using `boot_timing_tsc_freq()`
- [ ] Serial log format: `[+NNNms] BOOT_PMM: <msg>` where NNN = `boot_get_elapsed_ms()` at call time
- [ ] `boot_stage_history_get(out_entries, out_count)` accessor for §6 panic forensics
- [ ] `boot_progress_poll()`: lightweight refresh called from timer callbacks (e.g. `spinner_tick_callback` in TODO-03 §8) when no new stage is reported but the splash progress bar needs a visual update; reads current `boot_stage_history[]` tail and re-calls `boot_splash_status()` with the last stage message
- [ ] Commit: `"kernel: boot progress named-stage API with ring buffer and elapsed-ms tracking"`

## 3. POST-Style Hex Code Display `[Sonnet]`

Render a 2-digit hex POST code in the top-right framebuffer corner visible on every boot, cleared when the desktop is ready.

**Files:** `src/kernel/main/boot_progress.c`, `include/kernel/boot_progress.h`

- [ ] Embed an 8×8 pixel mini hex font for `0–9, A–F` (16 glyphs × 8 bytes = 128 bytes of static data) in `boot_progress.c`
- [ ] `post_display(uint8_t code)`: render two glyphs at 4× scale (= 32×32 px per glyph, 68×36 px total) at 4 px from top-right corner directly via `fb_put_pixel()`; background = black rectangle before each render
- [ ] Call `post_display(postcode)` inside `boot_stage_report()` after the serial write — covers normal boot path
- [ ] Also write to I/O port 0x80: `outb(0x80, postcode)` in `post_display()` for hardware card visibility
- [ ] Clear the display on `BOOT_STAGE_DESKTOP_READY`: fill the rectangle with `fb_fill_rect(x, y, 68, 36, 0x00000000)` + `fb_swap_rect()`
- [ ] `BOOT_DEGRADED` path: if framebuffer not ready (`!fb_init_done()`), skip pixel writes; I/O port write always happens
- [ ] Commit: `"kernel: POST-style hex code display in framebuffer corner + I/O port 0x80"`

## 4. Alive Blink / Hang Detection `[Sonnet]`

A 4×4 px blinking square toggled in the PIT interrupt handler — if it stops blinking, the interrupt handler has stopped.

**Files:** `src/kernel/drivers/pit.c` (or `timer.c`), `src/kernel/main/boot_progress.c`

- [ ] Add `g_alive_blink_enabled` flag (read from `boot.conf` key `AliveBlink=1`; default `1` in debug builds, `0` in release)
- [ ] In the PIT interrupt handler (runs before any kernel code each tick): if `g_alive_blink_enabled`, toggle a 1-byte static `g_blink_state` flag; call `alive_blink_render()`
- [ ] `alive_blink_render()`: write a 4×4 px square at top-left corner (4 px offset): GREEN (`0x00FF00`) when `blink_state=1` and CPU usage ≤ 90%, YELLOW when CPU usage > 90%, BLACK when `blink_state=0`; direct `fb_put_pixel()` calls — no compositor
- [ ] CPU usage threshold: `g_cpu_busy_pct` updated by scheduler once per second; alive blink reads it without locking (single-byte read is atomic on x86-64)
- [ ] Add `HKLM\SYSTEM\Boot\AliveBlink` = DWORD (0/1) as the persistent setting (→ XREF `02-kernel-core/TODO-13-registry-completion.md`)
- [ ] Commit: `"kernel: alive blink PIT-driven hang-detection indicator"`

## 5. Debug Color Bar Waterfall `[Sonnet]`

Opt-in proportional debug overlay: a bar across the top of the screen where each boot stage's width reflects its duration.

**Files:** `src/kernel/main/boot_progress.c`, `include/kernel/boot_progress.h`

- [ ] Activate when `boot.conf` key `DebugBar=1` or `HKLM\SYSTEM\Boot\DebugBar` = 1 (→ XREF `02-kernel-core/TODO-13-registry-completion.md`)
- [ ] Stage color table: PMM=`0x0000FF` (blue), VMM=`0x8000FF` (purple), HEAP=`0x4080FF` (light blue), VFS=`0x00C000` (green), REGISTRY=`0x00FFAA` (teal), DRIVERS=`0xFF8000` (orange), NETWORK=`0x00FFFF` (cyan), SCHEDULER=`0xFF0000` (red), DESKTOP=`0xFFFFFF` (white)
- [ ] At `BOOT_STAGE_DESKTOP_READY`: walk `boot_stage_history[]` and render a 4 px tall horizontal bar across the full screen width; each segment width = `(stage_duration_ticks / total_boot_ticks) × screen_width`; render via direct framebuffer pixel writes before compositor takes over
- [ ] Bar persists for 3 s using a PIT tick countdown, then fades out (50% opacity steps over 500 ms)
- [ ] Regression overlay: save the bar pixel data (RLE-compressed) to `C:\Impossible\System\Logs\boot-bar-last.dat`; on next debug boot, if file exists, render previous bar at 50% opacity underneath the current bar so regressions are visible
- [ ] Commit: `"kernel: opt-in boot diagnostic color bar waterfall with regression overlay"`

## 6. Panic Forensic Evidence `[Opus]`

Capture a `panic_evidence` struct at fault time, survive across soft reboot via a dedicated PMM page, and restore on next boot.

**Files:** `src/kernel/panic.c`, `include/kernel/panic.h`, `src/kernel/main/boot_hw.c`

> [!IMPORTANT]
> The PMM page at `0x80000` must be reserved in `pmm_init()` before it can be used as the cross-boot evidence page. Coordinate with `02-kernel-core/TODO-16-crash-dump-generation.md` to avoid using the same fixed address for the minidump workspace.

- [ ] Define `struct panic_evidence`: `uint32_t magic` (`0xDEADBEEF`), last 16 `boot_stage_history[]` entries, last 8 klog ring entries, last POST code byte, `uint64_t cr0/cr3/cr4` at fault, `uint32_t irq_mask`, `uint32_t pmm_free_pages`, `uint64_t fault_rip`, `char message[256]`
- [ ] `panic_collect_evidence(rip, msg)`: copy data into `struct panic_evidence` at physical `0x80000`; called at the very start of `kernel_panic()` before any screen output or VFS access
- [ ] Reserve physical page `0x80000` in `pmm_init()`: mark as `PMEM_RESERVED` so it is never handed out as a free page
- [ ] In boot Phase 0 (`boot_hw_init`): check `*(uint32_t*)0x80000 == 0xDEADBEEF`; if so, copy evidence to kernel heap buffer, clear the magic, log `[PANIC] Previous crash evidence found`; after VFS is up, write to `C:\Impossible\System\CrashDumps\last-panic.txt`
- [ ] After VFS write: show "System shut down unexpectedly" toast at desktop-ready (set `g_boot_info.had_panic = 1` flag; desktop init reads it)
- [ ] Commit: `"kernel: panic forensic evidence — cross-boot PMM page + last-panic.txt"`

## 7. Panic QR Code `[Sonnet]`

Embed a minimal QR code encoder and render a phone-scannable URL in the BSOD corner.

**Files:** `src/kernel/qr_encode.c`, `include/kernel/qr_encode.h`, `src/kernel/panic.c`

- [ ] Implement or embed a minimal MIT-licensed QR code encoder (~500 lines): `qr_encode(const char *text, uint8_t *matrix, int *size)` where `matrix` is a `size×size` bit grid (1=dark, 0=light); support QR version 3–6 (covers URLs up to ~150 chars); error correction level M
- [ ] `panic_qr_url(buf, bufsize, message, post_code, os_version)`: format `https://docs.impossible-os.dev/panic?msg=<short>&post=0x{post}&v={ver}` (truncate message to 40 chars to keep URL under 120 chars)
- [ ] In `kernel_panic()` BSOD renderer: after drawing the main panic screen, call `qr_encode(url, matrix, &qr_size)`; render QR module grid at bottom-right (12 px from corner), module size = 4 px, white modules on black background; quiet zone = 4 modules
- [ ] Ensure `qr_encode.c` is freestanding: no libc, no floating point; uses only `kernel/types.h` and `kernel/libc/string.h`
- [ ] Commit: `"kernel: minimal QR encoder + panic BSOD QR code for phone-scannable troubleshooting"`

## 8. System-Wide Multi-Instance Spinner `[Sonnet]`

Extend the existing single-instance `spinner.h` to support up to 8 simultaneous named spinner instances for use across the desktop.

**Files:** `include/kernel/spinner.h`, `src/kernel/spinner.c`

> [!NOTE]
> The existing `spinner_init/start/advance/stop` API covers the boot splash single spinner (§1–3 of the old TODO-010.97, already done). This section adds a multi-instance layer on top without breaking the existing boot-splash usage.

- [ ] Define `spinner_t` struct: `int32_t cx, cy, radius, stroke; uint32_t color; int32_t angle, sweep; uint8_t active; uint8_t size_class`
- [ ] `spinner_create(uint8_t size_class, uint32_t color)`: allocate from a static pool of 8 `spinner_t` slots; size classes: `SPINNER_SMALL=16`, `SPINNER_MEDIUM=32`, `SPINNER_LARGE=48`, `SPINNER_XLARGE=64` (px radius); returns `spinner_t*` or NULL if pool full
- [ ] `spinner_destroy(spinner_t *s)`: mark slot as inactive; stop animation
- [ ] `spinner_set_position(spinner_t *s, int32_t cx, int32_t cy)`: update position without restarting
- [ ] `spinner_tick(spinner_t *s)`: advance angle + breathing (port animation logic from existing `spinner_advance()` — single instance → multi); call `spinner_render(s, cx, cy)`
- [ ] `spinner_render(spinner_t *s, struct fb_surface *surface, int32_t x, int32_t y)`: draw arc ring onto `surface` (compositor surface or direct framebuffer)
- [ ] Compositor integration: WM maintains a `spinner_t *g_active_spinners[8]` list; compositor loop calls `spinner_tick()` on each non-NULL entry per frame; used by loading dialogs, Start Menu search, download progress, Service Manager
- [ ] Backward compatibility: existing `spinner_init/start/advance/stop` calls remain valid; they operate on `g_active_spinners[0]` (the boot splash slot)
- [ ] Commit: `"kernel: multi-instance spinner_t pool for compositor-integrated loading indicators"`

## 9. Runtime Vital Signs Strip `[Sonnet]`

An always-visible 20 px overlay strip at the bottom of the desktop showing live system metrics for developers.

**Files:** `src/desktop/vital_signs.c`, `include/desktop/vital_signs.h`

- [ ] Activate when `boot.conf` key `VitalSigns=1` or `HKLM\SYSTEM\Boot\VitalSigns` = 1 (→ XREF `02-kernel-core/TODO-13-registry-completion.md`)
- [ ] `vital_signs_init()`: called from desktop init; allocates a 20 px compositor overlay surface pinned to the bottom of the screen
- [ ] `vital_signs_tick()`: called every 500 ms from a PIT-driven callback; reads: CPU usage % from scheduler stats, RAM used/total from PMM, IRQ count/s from IRQ counter differential, uptime in seconds from PIT ticks, framerate from compositor frame counter
- [ ] Render format (FONT_MONO at 10 px, white on 50% transparent black): `[CPU: 23%] [RAM: 1.2/4.0 GB] [IRQ: 1234/s] [Uptime: 00:03:42] [FPS: 60]`
- [ ] `VitalSignsExtended=1` adds second line: `[Free: 2847 pages] [TCP: 3] [VFS R: 1.2 MB/s W: 0.4 MB/s] [Temp: 62°C]` (CPU temp from ACPI thermal zone if available, 0 if not)
- [ ] The overlay is always rendered above the desktop wallpaper and windows; zorder = top - 1 (below cursor, above everything else)
- [ ] Commit: `"desktop: runtime vital signs strip — CPU/RAM/IRQ/FPS overlay for developers"`

---

## OS Comparison

| ⭐  | Feature                              | 🪟 Windows 11                                     | 🐧 Linux                                          | 🚀 Impossible OS                                           |
| --- | ------------------------------------ | -------------------------------------------------- | ------------------------------------------------- | ----------------------------------------------------------- |
| 💎  | Boot progress POST codes (port 0x80) | ✅ Firmware POST codes; Windows boot manager none | ✅ BIOS POST codes only (kernel does not add)     | ⬜ Planned — §1 + §3; kernel adds named codes to port 0x80 |
| 💎  | Named-stage boot progress log        | ✅ ETW boot trace (binary, WPA required)          | ✅ `dmesg` timestamps + `systemd-analyze`         | ⬜ Planned — §2; human-readable serial `[+NNNms] STAGE`    |
| 💎  | Panic forensics preserved across boot| ✅ WER minidump + EventLog crash record           | ✅ `kdump`/`pstore` crash RAM log                 | ⬜ Planned — §6; `0x80000` evidence page + `last-panic.txt`|
| 💎  | Multi-instance spinner component     | ✅ ProgressRing (WinUI 3) — compositor-managed    | ✅ GTK Spinner, Qt BusyIndicator                  | ⬜ Planned — §8; `spinner_create()` pool                   |
| ⭐  | Proportional debug bar waterfall     | ❌ Not available in production builds             | ❌ `ftrace` events (no visual)                    | ⬜ **Planned — §5; opt-in visual regression indicator**    |
| ⭐  | QR code on panic screen              | ❌ Stop code URL is text only                     | ❌ Not implemented                                | ⬜ **Planned — §7; phone-scannable recovery link**         |
| ⭐  | Alive blink PIT-driven hang indicator| ❌ No visible hang indicator (just freezes)       | ❌ Not implemented in production kernels          | ⬜ **Planned — §4; 4×4 px corner blink stops on hang**     |
| ⭐  | Runtime vital signs overlay          | ⚠️ Task Manager only (separate window)            | ⚠️ `htop`/`conky` (third-party, separate window)  | ⬜ **Planned — §9; always-visible bottom strip**           |

> **After parity items:** Impossible OS matches Windows and Linux on POST codes, named-stage boot logging, cross-boot panic evidence, and multi-instance spinners. The exclusive items elevate it further: the proportional waterfall makes boot regressions visible without a profiler, the BSOD QR code lets anyone diagnose a panic with their phone, the alive blink gives an instant visual indication of kernel hangs, and the vital-signs strip puts developer metrics front-and-center without a separate tool.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Serial log shows `[+Nms] BOOT_PMM: Physical memory manager ready` style entries for at least 8 stages
- [ ] POST code visible in top-right corner during QEMU boot; disappears when desktop loads
- [ ] `DebugBar=1` in `boot.conf` → proportional colored bar appears at top of screen after desktop ready, fades after 3 s
- [ ] `AliveBlink=1` in `boot.conf` → 4×4 green square blinks in top-left corner during boot and desktop
- [ ] Force `kernel_panic("test")` from shell → BSOD shows QR code in bottom-right corner
- [ ] Force panic twice → second boot finds `last-panic.txt` in `C:\Impossible\System\CrashDumps\`
- [ ] `VitalSigns=1` → bottom strip shows CPU/RAM/IRQ/uptime/FPS, updates every 500 ms
- [ ] `spinner_create(SPINNER_MEDIUM, 0x0078D4)` in test harness → spinner renders in compositor frame
- [ ] Commit: `"kernel: boot-diagnostics verified — POST codes, waterfall, panic forensics, QR code, vital signs, multi-instance spinner"`
