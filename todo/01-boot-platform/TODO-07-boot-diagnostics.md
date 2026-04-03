# TODO-07 -- Boot Diagnostics, Heartbeat & Spinner

> **Goal:** The arc spinner and boot splash are done. This TODO builds the production diagnostics layer: a named-stage boot progress API that feeds the splash, POST-style hex codes visible on hardware debug cards, cross-boot panic forensics, a panic QR code, runtime vital-signs overlay, alive-blink hang detection, and a multi-instance compositor-integrated spinner -- turning the ad-hoc debug tooling into production-grade features.

> [!NOTE]
> **Origin:** The HV_BAR colored pixel bars were added during Hyper-V Gen 2 debugging -- crude but instantly effective. This TODO formalises that approach as an opt-in production debug feature while replacing the unconditional hack with proper structured output.

> [!IMPORTANT]
> **`boot_progress(phase, step, postcode)`** already exists in `boot_init.h` (implemented by `02-kernel-core/TODO-01 §1`). §2 of this TODO extends it with a named-stage API, ring buffer history, elapsed-ms tracking, and splash status integration -- do not rewrite the lower-level call, build on it.

## Inputs

- [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h)
- [`include/kernel/boot_splash.h`](../../include/kernel/boot_splash.h)
- [`include/kernel/spinner.h`](../../include/kernel/spinner.h)
- [`include/kernel/panic.h`](../../include/kernel/panic.h)
- [`src/kernel/panic.c`](../../src/kernel/panic.c)
- [`src/kernel/boot_timing.c`](../../src/kernel/boot_timing.c)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §1` -- `boot_progress(phase, step, postcode)` in `boot_init.h`; §2 of this TODO wraps it with a named-stage layer
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §7` -- `boot_halt()` is the pre-FB panic anchor that §5 extends with forensic evidence
- → XREF: `02-kernel-core/TODO-16-crash-dump-generation.md` -- crash dumps complement §5 panic forensics; coordinate PMM page reservation at `0x80000` to avoid collision with minidump workspace
- → XREF: `02-kernel-core/TODO-13-registry-completion.md` -- `HKLM\SYSTEM\Boot\AliveBlink`, `VitalSigns` registry keys (visual POST / debug bar: [TODO-08 -- Visual POST Display](TODO-08-visual-post-display.md))
- → XREF: `TODO-01-uefi-hardening-secureboot.md §7` -- boot UX polish calls `boot_splash_status()` via the §2 API
- → XREF: `07-graphics-ui/TODO-06-window-manager.md §8` -- compositor frame loop must call `spinner_tick()` on every active `g_active_spinners[]` entry per frame; TODO-06 §8 (Compositor Performance) is the natural owner for this per-frame integration

## Outcome

- Serial log shows `[+NNNms] BOOT_PMM: Physical memory manager ready` style entries for every major stage.
- Two-digit hex POST code visible in the top-right corner of the framebuffer from kernel entry until `BOOT_DESKTOP_READY`; same codes output to I/O port 0x80.
- On panic: `struct panic_evidence` captured at `0x80000`; next boot saves `last-panic.txt` and shows "System shut down unexpectedly" toast.
- Panic BSOD shows a QR code in the bottom-right corner linking to the troubleshooting page.
- `AliveBlink=1` activates a 4×4 px blinking square in the top-left corner driven by the PIT handler.
- `spinner_create()` / `spinner_tick()` multi-instance API available; compositor ticks all active spinners each frame.
- `VitalSigns=1` activates a 20 px bottom-strip showing CPU %, RAM, IRQ rate, uptime, and FPS.

## Implementation Order

| ⭐  | Order | Deliverable                        | Depends On | Status |
| --- | :---: | ---------------------------------- | ---------- | :----: |
| 💎  |   1   | UEFI pre-kernel POST codes         | --          |  [x]   |
| 💎  |   2   | Boot progress named-stage API      | §1         |  [x]   |
| 💎  |   3   | POST-style hex code display        | §2         |  [x]   |
| 💎  |   4   | Alive blink / hang detection       | §2         | defer  |
| 💎  |   5   | Panic forensic evidence            | §2         | defer  |
| ⭐  |   6   | Panic QR code                      | §5         | defer  |
| 💎  |   7   | System-wide multi-instance spinner | --          | defer  |
| ⭐  |   8   | Runtime vital signs strip          | §7         | defer  |

> 💎 = parity -- Windows and Linux both have equivalent diagnostics; Impossible OS must match them.
> ⭐ = exclusive -- the QR code on BSOD and always-visible vital-signs strip are not present in either competitor at the kernel level.

---

## 1. UEFI Pre-Kernel POST Codes
Write I/O port 0x80 POST codes from the bootloader so hardware POST-code reader cards decode boot progress before the kernel even starts.

**Files:** `src/boot/uefi/bootx64.c`

- [x] Add `post_code(uint8_t code)` inline in `bootx64.c`: `outb(0x80, code)` + named constants `POST_ENTRY` through `POST_KERNEL_JUMP`
- [x] Insert `post_code()` calls at 9 milestones: `0x01`=entry, `0x02`=GOP init, `0x03`=ELF open, `0x04`=ELF load, `0x05`=RSDP found, `0x06`=memory map, `0x07`=ExitBootServices, `0x08`=page tables, `0x09`=kernel jump
- [x] QEMU ignores port 0x80 writes silently -- no fault, verified by clean build
- [x] Commit: `"boot: UEFI pre-kernel POST codes to I/O port 0x80"`

## 2. Boot Progress Named-Stage API
High-level named-stage wrapper over the existing `boot_progress()` that adds a 32-entry ring buffer, elapsed-ms tracking, and `boot_splash_status()` forwarding.

**Files:** `include/kernel/boot_progress.h`, `src/kernel/main/boot_progress.c`

- [x] Define `boot_stage_t` enum with 15 stages from `UEFI_INIT` through `DESKTOP_READY`
- [x] Map each stage to phase, postcode, progress %, and name in `s_meta[]` table
- [x] `boot_stage_report()`: calls `boot_progress()`, records in 32-slot ring with TSC + elapsed_ms, serial logs `[+NNNms] STAGE: msg`
- [x] `boot_get_elapsed_ms()`: returns ms since `KERNEL_ENTRY` via `boot_timing_tsc_freq()`
- [x] Serial log format: `[+NNNms] STAGE_NAME: msg`
- [x] `boot_stage_history_get()` accessor for panic forensics
- [x] `boot_progress_poll()`: re-sends last stage to `boot_splash_status()` for timer-driven visual refresh
- [x] Commit: `"kernel: boot progress named-stage API with ring buffer and elapsed-ms tracking"`

## 3. POST-Style Hex Code Display
Render a 2-digit hex POST code in the top-right framebuffer corner visible on every boot, cleared when the desktop is ready.

**Files:** `src/kernel/main/boot_progress.c`, `include/kernel/boot_progress.h`

- [x] 8x8 hex font (16 glyphs, 128 bytes) embedded in `boot_progress.c`
- [x] `post_display(code)`: renders 2 hex digits at 4x scale (32x32 px each, green on black) at top-right corner via `fb_put_pixel()` + `fb_swap_rect()`
- [x] Called from `boot_stage_report()` after serial write
- [x] Also writes I/O port 0x80 via `outb` for hardware POST cards
- [x] Cleared on `BOOT_STAGE_DESKTOP_READY` via `fb_fill_rect()` + `fb_swap_rect()`
- [x] Skips pixel writes when `SUBSYS_FB` not ready; I/O port write always fires
- [x] Commit: `"kernel: POST-style hex code display in framebuffer corner + I/O port 0x80"`

## 4. Alive Blink / Hang Detection *(deferred -- removed during bare-metal debug; `fb_swap_rect` from ISR caused recursive interrupts; needs redesign)*
A 4×4 px blinking square toggled in the PIT interrupt handler -- if it stops blinking, the interrupt handler has stopped.

**Files:** `src/kernel/drivers/pit.c` (or `timer.c`), `src/kernel/main/boot_progress.c`

- [x] Gated by `boot.conf` `heartbeat=` (0=off, 1=on); reads `g_boot_info.config.heartbeat`
- [x] `alive_blink_tick()` in `timer_tick_callback_fire()` -- runs on every timer interrupt (100 Hz PIT or LAPIC)
- [x] 4x4 px green square at top-left (4,4); toggles every 50 ticks (0.5 sec); direct `fb_put_pixel()` + `fb_swap_rect()`
- [x] CPU usage threshold -- deferred (needs scheduler per-second CPU accounting, future enhancement)
- [x] Registry setting -- deferred to TODO-13 (currently boot.conf only)
- [x] Commit: `"kernel: alive blink timer-driven hang-detection indicator"`

## 5. Panic Forensic Evidence *(deferred -- needs stable boot first; crash evidence is useless if boot itself crashes)*
Capture a `panic_evidence` struct at fault time, survive across soft reboot via a dedicated PMM page, and restore on next boot.

**Files:** `src/kernel/panic.c`, `include/kernel/panic.h`, `src/kernel/main/boot_hw.c`

> [!IMPORTANT]
> The PMM page at `0x80000` must be reserved in `pmm_init()` before it can be used as the cross-boot evidence page. Coordinate with `02-kernel-core/TODO-16-crash-dump-generation.md` to avoid using the same fixed address for the minidump workspace.

- [ ] Define `struct panic_evidence`: `uint32_t magic` (`0xDEADBEEF`), last 16 `boot_stage_history[]` entries, last 8 klog ring entries, last POST code byte, `uint64_t cr0/cr3/cr4` at fault, `uint32_t irq_mask`, `uint32_t pmm_free_pages`, `uint64_t fault_rip`, `char message[256]`
- [ ] `panic_collect_evidence(rip, msg)`: copy data into `struct panic_evidence` at physical `0x80000`; called at the very start of `kernel_panic()` before any screen output or VFS access
- [ ] Reserve physical page `0x80000` in `pmm_init()`: mark as `PMEM_RESERVED` so it is never handed out as a free page
- [ ] In boot Phase 0 (`boot_hw_init`): check `*(uint32_t*)0x80000 == 0xDEADBEEF`; if so, copy evidence to kernel heap buffer, clear the magic, log `[PANIC] Previous crash evidence found`; after VFS is up, write to `C:\Impossible\System\CrashDumps\last-panic.txt`
- [ ] After VFS write: show "System shut down unexpectedly" toast at desktop-ready (set `g_boot_info.had_panic = 1` flag; desktop init reads it)
- [ ] Commit: `"kernel: panic forensic evidence -- cross-boot PMM page + last-panic.txt"`

**Test checkpoint:** QEMU: force `kernel_panic("test")` → reboot → serial shows `[PANIC] Previous crash evidence found` → `C:\Impossible\System\CrashDumps\last-panic.txt` contains fault RIP + POST code. Bare metal: same flow, verify evidence survives warm reboot.

## 6. Panic QR Code *(deferred -- depends on §5)*
Embed a minimal QR code encoder and render a phone-scannable URL in the BSOD corner.

**Files:** `src/kernel/qr_encode.c`, `include/kernel/qr_encode.h`, `src/kernel/panic.c`

- [ ] Implement or embed a minimal MIT-licensed QR code encoder (~500 lines): `qr_encode(const char *text, uint8_t *matrix, int *size)` where `matrix` is a `size×size` bit grid (1=dark, 0=light); support QR version 3–6 (covers URLs up to ~150 chars); error correction level M
- [ ] `panic_qr_url(buf, bufsize, message, post_code, os_version)`: format `https://docs.impossible-os.dev/panic?msg=<short>&post=0x{post}&v={ver}` (truncate message to 40 chars to keep URL under 120 chars)
- [ ] In `kernel_panic()` BSOD renderer: after drawing the main panic screen, call `qr_encode(url, matrix, &qr_size)`; render QR module grid at bottom-right (12 px from corner), module size = 4 px, white modules on black background; quiet zone = 4 modules
- [ ] Ensure `qr_encode.c` is freestanding: no libc, no floating point; uses only `kernel/types.h` and `kernel/libc/string.h`
- [ ] Commit: `"kernel: minimal QR encoder + panic BSOD QR code for phone-scannable troubleshooting"`

## 7. System-Wide Multi-Instance Spinner *(deferred -- desktop polish, single spinner works)*
Extend the existing single-instance `spinner.h` to support up to 8 simultaneous named spinner instances for use across the desktop.

**Files:** `include/kernel/spinner.h`, `src/kernel/spinner.c`

> [!NOTE]
> The existing `spinner_init/start/advance/stop` API covers the boot splash single spinner (§1–3 of the old TODO-010.97, already done). This section adds a multi-instance layer on top without breaking the existing boot-splash usage.

- [ ] Define `spinner_t` struct: `int32_t cx, cy, radius, stroke; uint32_t color; int32_t angle, sweep; uint8_t active; uint8_t size_class`
- [ ] `spinner_create(uint8_t size_class, uint32_t color)`: allocate from a static pool of 8 `spinner_t` slots; size classes: `SPINNER_SMALL=16`, `SPINNER_MEDIUM=32`, `SPINNER_LARGE=48`, `SPINNER_XLARGE=64` (px radius); returns `spinner_t*` or NULL if pool full
- [ ] `spinner_destroy(spinner_t *s)`: mark slot as inactive; stop animation
- [ ] `spinner_set_position(spinner_t *s, int32_t cx, int32_t cy)`: update position without restarting
- [ ] `spinner_tick(spinner_t *s)`: advance angle + breathing (port animation logic from existing `spinner_advance()` -- single instance → multi); call `spinner_render(s, cx, cy)`
- [ ] `spinner_render(spinner_t *s, struct fb_surface *surface, int32_t x, int32_t y)`: draw arc ring onto `surface` (compositor surface or direct framebuffer)
- [ ] Compositor integration: WM maintains a `spinner_t *g_active_spinners[8]` list; compositor loop calls `spinner_tick()` on each non-NULL entry per frame; used by loading dialogs, Start Menu search, download progress, Service Manager
- [ ] Backward compatibility: existing `spinner_init/start/advance/stop` calls remain valid; they operate on `g_active_spinners[0]` (the boot splash slot)
- [ ] Commit: `"kernel: multi-instance spinner_t pool for compositor-integrated loading indicators"`

## 8. Runtime Vital Signs Strip *(deferred -- developer tool, needs scheduler stats first)*
An always-visible 20 px overlay strip at the bottom of the desktop showing live system metrics for developers.

**Files:** `src/desktop/vital_signs.c`, `include/desktop/vital_signs.h`

- [ ] Activate when `boot.conf` key `VitalSigns=1` or `HKLM\SYSTEM\Boot\VitalSigns` = 1 (→ XREF `02-kernel-core/TODO-13-registry-completion.md`)
- [ ] `vital_signs_init()`: called from desktop init; allocates a 20 px compositor overlay surface pinned to the bottom of the screen
- [ ] `vital_signs_tick()`: called every 500 ms from a PIT-driven callback; reads: CPU usage % from scheduler stats, RAM used/total from PMM, IRQ count/s from IRQ counter differential, uptime in seconds from PIT ticks, framerate from compositor frame counter
- [ ] Render format (FONT_MONO at 10 px, white on 50% transparent black): `[CPU: 23%] [RAM: 1.2/4.0 GB] [IRQ: 1234/s] [Uptime: 00:03:42] [FPS: 60]`
- [ ] `VitalSignsExtended=1` adds second line: `[Free: 2847 pages] [TCP: 3] [VFS R: 1.2 MB/s W: 0.4 MB/s] [Temp: 62°C]` (CPU temp from ACPI thermal zone if available, 0 if not)
- [ ] The overlay is always rendered above the desktop wallpaper and windows; zorder = top - 1 (below cursor, above everything else)
- [ ] Commit: `"desktop: runtime vital signs strip -- CPU/RAM/IRQ/FPS overlay for developers"`

---

## OS Comparison

| ⭐ | Feature                 | 🪟 Win11                        | 🐧 Linux                          | 🚀 Impossible OS                    |
|----|-------------------------|------------------------------|--------------------------------|-----------------------------------|
| 💎 | Boot POST codes         | ✅ Firmware + boot manager  | ✅ BIOS POST only              | ✅ §1+§3 -- kernel POST + I/O 80  |
| 💎 | Named-stage progress    | ✅ ETW boot trace (binary)  | ✅ dmesg + systemd-analyze     | ✅ §2 -- serial `[+NNNms] STAGE`  |
| 💎 | Panic forensics         | ✅ WER minidump + EventLog  | ✅ kdump / pstore              | ⬜ §5 -- PMM page + last-panic    |
| 💎 | Multi-instance spinner  | ✅ ProgressRing (WinUI 3)   | ✅ GTK/Qt spinners             | ⬜ §7 -- spinner_create pool      |
| ⭐ | Panic QR code           | ❌ Text URL only            | ❌ Not implemented             | ⬜ §6 -- phone-scannable link     |
| ⭐ | Alive blink indicator   | ❌ No visible hang signal   | ❌ Not in production           | ⚠️ §4 -- removed during BM debug  |
| ⭐ | Vital signs overlay     | ⚠️ Task Manager (separate)  | ⚠️ htop/conky (third-party)    | ⬜ §8 -- always-visible strip     |

> **After parity items:** POST codes, named-stage logging, panic evidence, and spinners match Windows/Linux. QR panic code and vital-signs strip go beyond both.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_boot_diag()` (-> XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_boot_diag.c` with:
  - `boot_stage_report()` with a test stage records entry in `boot_stage_history[]` (ring buffer not empty after call)
  - `boot_get_elapsed_ms()` returns monotonically increasing values (two calls 1ms apart, second >= first)
  - `boot_stage_history_get()` returns non-NULL pointer with valid `count > 0` after boot
  - POST hex display: `boot_post_write16(0xAABB)` followed by `boot_post_read16()` returns `0xAABB`
  - `boot_progress_poll()` does not crash when called with no active stage
  - Alive blink toggle counter increments over 100 timer ticks when `heartbeat=1`
  - `spinner_create(SPINNER_MEDIUM, 0x0078D4)` returns non-NULL; `spinner_destroy()` frees the slot; re-create succeeds
  - `spinner_create()` returns NULL after 8 allocations (pool exhausted)
  - Panic evidence struct at `0x80000`: `panic_collect_evidence()` writes magic `0xDEADBEEF`, readback matches
- [ ] Register in `test_runner_init()`: `test_register_boot_diag()`
- [ ] Commit: `"test: add boot_diag test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Serial log shows `[+Nms] BOOT_PMM: Physical memory manager ready` style entries for at least 8 stages
- [ ] POST code visible in top-right corner during QEMU boot; disappears when desktop loads
- [ ] `AliveBlink=1` in `boot.conf` → 4×4 green square blinks in top-left corner during boot and desktop
- [ ] Force `kernel_panic("test")` from shell → BSOD shows QR code in bottom-right corner
- [ ] Force panic twice → second boot finds `last-panic.txt` in `C:\Impossible\System\CrashDumps\`
- [ ] `VitalSigns=1` → bottom strip shows CPU/RAM/IRQ/uptime/FPS, updates every 500 ms
- [ ] `spinner_create(SPINNER_MEDIUM, 0x0078D4)` in test harness → spinner renders in compositor frame
- [ ] Commit: `"kernel: boot-diagnostics verified -- POST codes, panic forensics, QR code, vital signs, multi-instance spinner"`
