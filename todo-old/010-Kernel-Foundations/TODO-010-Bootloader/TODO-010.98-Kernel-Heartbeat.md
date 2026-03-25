# TODO-010.98 — Kernel Heartbeat: Visual Boot Progress & Runtime Diagnostics

> **Goal:** Replace the ad-hoc colored framebuffer bars (used to debug Hyper-V
> boot hangs) with a proper **Kernel Heartbeat** system — a unified API for
> boot progress reporting, visual debugging, POST-style hex codes, and runtime
> kernel vital signs. The system has multiple output modes that activate based
> on context:
>
> | Context            | Output                                              |
> | ------------------ | --------------------------------------------------- |
> | Normal boot        | Splash status text + dot animation (existing)       |
> | Debug/verbose boot | Labeled color bars (full-screen waterfall)           |
> | Always (all modes) | 2-digit hex POST code in screen corner              |
> | Panic / hang       | Color bar pattern = visual stack trace (forensics)  |
> | Post-boot (desktop)| 1px vital signs strip (opt-in via Control Panel)    |

> [!NOTE]
> **Origin story:** During Hyper-V Gen 2 debugging, we manually wrote colored
> pixel bars to the framebuffer at hardcoded y-offsets to trace boot progress
> when serial output wasn't available. Each color meant "boot got this far."
> The technique was crude but **instantly effective** — it diagnosed the PIC
> hang in seconds. This TODO turns that hack into a production-grade feature.

> [!IMPORTANT]
> **Cross-references:**
> → XREF: `TODO-005-Debug.md §7` — Boot Time Profiler (data source)
> → XREF: `TODO-010.99-APIC-First-Boot.md §7` — Boot Time Visualization (Gantt chart)
> → XREF: `TODO-010-Bootloader.md §2` — Boot Splash Screen (integration point)
> → XREF: `TODO-010-Bootloader.md §5.3` — Panic Screen (forensic integration)

---

## Existing Code to Replace

> [!WARNING]
> **22 `HV_BAR` macros** across two files must be replaced by `boot_progress()`:
>
> | File                        | Rows         | Colors Used                                      |
> | --------------------------- | ------------ | ------------------------------------------------ |
> | `boot_hw.c` (lines 45–114) | 72–144       | GREEN, CYAN, YELLOW, BLUE, ORANGE, WHITE, MAGENTA|
> | `boot_interrupts.c` (42–108)| 160–268     | RED, GREEN, CYAN, YELLOW, BLUE, ORANGE, PURPLE, WHITE, MAGENTA |
>
> Additionally, `boot_splash_status()` already has a debug-mode fallback that
> prints `[BOOT] <msg>` to serial via `printk()`. The new `boot_progress()` API
> subsumes both the `HV_BAR` macros and `boot_splash_status()` calls.

---

## 1. Boot Progress API (`boot_progress()`) *(agent)*

**Prompt:** Create a unified `boot_progress()` API that every kernel init function calls to report its progress. The API automatically routes to the correct output based on the current boot mode: splash status text in normal mode, labeled color bars in debug mode, and a hex POST code always. This replaces both the ad-hoc `boot_splash_status()` calls and the manual `HV_BAR` debug macros. Every boot stage gets a unique 8-bit code (like a hardware POST code), a human-readable name, and a category for color-coding. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: boot_progress() unified API"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!TIP]
> **Design principle:** `boot_progress()` is a **zero-cost abstraction** in release
> builds. When splash is active, it's just a status text update. When debug bars
> are disabled, the hex code write is a single 16-pixel blit. No allocations, no
> string formatting in the hot path — everything is compile-time constants.

### 1.1 Core API

- [ ] Create `src/kernel/boot_progress.c` and `include/kernel/boot_progress.h`
- [ ] Define progress code registry — compile-time enum:
  ```c
  /* Category prefixes:
   *   0x1_ = Memory    (green)    0x5_ = Storage    (yellow)
   *   0x2_ = CPU/ACPI  (cyan)     0x6_ = Network    (blue)
   *   0x3_ = Interrupt (orange)   0x7_ = Desktop/UI (purple)
   *   0x4_ = Drivers   (white)    0xF_ = Fatal      (red)    */
  enum boot_code {
      BP_SERIAL_INIT       = 0x10,
      BP_PMM_INIT          = 0x11,
      BP_VMM_INIT          = 0x12,
      BP_HEAP_INIT         = 0x13,
      BP_SIMD_INIT         = 0x14,
      BP_CPUID_INIT        = 0x20,
      BP_ACPI_INIT         = 0x21,
      BP_LAPIC_INIT        = 0x22,
      BP_IOAPIC_INIT       = 0x23,
      BP_SMP_INIT          = 0x24,
      BP_GDT_INIT          = 0x30,
      BP_IDT_INIT          = 0x31,
      BP_PIC_INIT          = 0x32,
      BP_PIT_INIT          = 0x33,
      BP_RTC_INIT          = 0x34,
      BP_KEYBOARD_INIT     = 0x40,
      BP_MOUSE_INIT        = 0x41,
      BP_PCI_SCAN          = 0x42,
      BP_FB_INIT           = 0x43,
      BP_SPLASH_INIT       = 0x44,
      BP_HYPERV_INIT       = 0x45,
      BP_AHCI_INIT         = 0x50,
      BP_VIRTIO_INIT       = 0x51,
      BP_PARTITION_SCAN    = 0x52,
      BP_VFS_MOUNT         = 0x53,
      BP_REGISTRY_INIT     = 0x54,
      BP_NIC_INIT          = 0x60,
      BP_DHCP              = 0x61,
      BP_FONT_INIT         = 0x70,
      BP_ICON_INIT         = 0x71,
      BP_CURSOR_INIT       = 0x72,
      BP_WM_INIT           = 0x73,
      BP_DESKTOP_INIT      = 0x74,
      BP_BOOT_COMPLETE     = 0x00,  /* special: all done */
      BP_PANIC             = 0xFF,  /* special: kernel panic */
  };
  ```
- [ ] Define the boot progress lookup table — compile-time:
  ```c
  struct boot_stage {
      uint8_t     code;
      const char *name;          /* "acpi_init"                   */
      const char *splash_text;   /* "Setting up hardware..."      */
      uint32_t    bar_color;     /* Category color from prefix    */
  };
  static const struct boot_stage g_boot_stages[] = { ... };
  ```
- [ ] Define `boot_progress(uint8_t code)` — the single entry point:
  - [ ] Always: write hex code to POST corner (§2)
  - [ ] Always: append to history buffer for panic forensics (§4)
  - [ ] Always: log to serial: `[BOOT] 0x21 acpi_init`
  - [ ] Always: record timestamp (PIT ticks or TSC) for boot profiling
  - [ ] If splash active: update splash status text from `splash_text` field
  - [ ] If debug bars enabled: draw labeled color bar (§3)
  - [ ] If klog live flush active: issue immediate flush (checkpoint)
- [ ] Build and test: compile, no regressions
- [ ] Commit: `"kernel: boot_progress() unified API"`

### 1.2 Instrument All Boot Stages

- [ ] Replace all `HV_BAR(...)` macros in `boot_hw.c` (10 calls) with `boot_progress(BP_xxx)`
- [ ] Replace all `HV_BAR(...)` macros in `boot_interrupts.c` (10 calls) with `boot_progress(BP_xxx)`
- [ ] Replace all `boot_splash_status("...")` calls in `main.c` with `boot_progress(BP_xxx)`
- [ ] Add `boot_progress()` calls to any init function not currently covered:
  - [ ] `simd_enable_avx()`, `smp_init()`, `hyperv_detect()`
  - [ ] `virtio_blk_init()`, `registry_init()`
- [ ] Delete the `HV_BAR` macro definitions from `boot_hw.c` and `boot_interrupts.c`
- [ ] Verify serial output shows: `[BOOT] 0x10 serial_init ... [BOOT] 0x00 boot_complete`
- [ ] Verify splash status text still updates correctly in normal mode
- [ ] Commit: `"kernel: instrument all boot stages with boot_progress()"`

### 1.3 klog Integration

- [ ] Integrate with klog ring buffer — each `boot_progress()` entry becomes a klog entry:
  - [ ] Level: `LOG_INFO` for normal stages, `LOG_DEBUG` for substages
  - [ ] Subsystem: `"boot"` (matches existing `klog(LOG_INFO, "boot", ...)` pattern)
- [ ] Integrate with klog disk flush — `boot_progress()` triggers immediate flush checkpoint:
  - [ ] → XREF: `TODO-005-Debug.md §1.3` — live flush mode
  - [ ] Ensures the last progress code is persisted to X: before any potential hang
- [ ] Integrate with boot time profiler — delta between consecutive progress calls:
  - [ ] → XREF: `TODO-005-Debug.md §7` — boot time profiler stores per-stage ms
  - [ ] → XREF: `TODO-010.99-APIC-First-Boot.md §7` — Gantt chart data source
- [ ] Commit: `"kernel: boot_progress klog + profiler integration"`

---

## 2. POST-Style Hex Code Display *(agent)*

**Prompt:** Real server motherboards have a 2-digit hex LED display that shows POST codes during boot. If the system hangs, the frozen code tells the technician exactly which subsystem failed. Implement the same concept: render a tiny 2-digit hex code in a fixed corner of the screen, updated on every `boot_progress()` call. The code persists on-screen even after boot splash finishes — it shows `00` when boot is complete. On hang, the frozen hex code is the first thing a developer sees. The hex font is a minimal hardcoded 5×7 bitmap — no TTF dependency. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: POST hex code display"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!TIP]
> **Competitive Edge:** No consumer desktop OS shows POST codes. Windows hides
> boot progress behind a generic spinner. Linux shows scrolling text (if you're
> lucky) or a silent Plymouth splash. Impossible OS shows a **real POST code**
> — power users instantly know where the boot is, without needing serial output.

### 2.1 Minimal Hex Font

- [ ] Hardcode a 5×7 pixel bitmap font for hex digits `0-9`, `A-F` (16 chars × 7 bytes = 112 bytes)
- [ ] No TTF dependency — this must work before `fb_init()` completes (raw framebuffer)
- [ ] Store as `static const uint8_t hex_font[16][7]` — each byte = 5 pixels as bits
- [ ] Render function: `post_draw_digit(uint8_t digit, int x, int y, uint32_t color)`
- [ ] Commit (with §2.2): `"kernel: POST hex code display"`

### 2.2 Corner Renderer

- [ ] Position: bottom-right corner, 8px from edge (non-intrusive)
- [ ] Size: 2 digits × (5px wide + 2px gap) × 2x scale = 24×14 pixels total
- [ ] Background: semi-transparent black (darken underlying pixels by 50%)
- [ ] Foreground: white hex digits (high contrast on any background)
- [ ] `post_code_update(uint8_t code)` — called by `boot_progress()`:
  - [ ] Clear previous digits (restore background or redraw darkened rect)
  - [ ] Draw 2 hex digits at fixed position
  - [ ] Direct framebuffer write — no compositor dependency
- [ ] Works at all stages: pre-splash, during splash, during desktop, during panic
- [ ] Special codes: `00` = boot complete, `FF` = panic
- [ ] On panic screen: POST code rendered in BSOD header alongside stop code
- [ ] Build and test on QEMU: hex codes visible during boot, `00` at desktop
- [ ] Commit: `"kernel: POST hex code display"`

### 2.3 POST Code Reference Card

- [ ] Generate `docs/post-codes.md` — complete reference of all codes:
  ```
  Code  Stage            Category     Description
  ────  ───────────────  ───────────  ─────────────────────────
  0x00  boot_complete    —            Boot finished successfully
  0x10  serial_init      Memory       COM1 serial port setup
  0x11  pmm_init         Memory       Physical memory manager
  ...
  0xFF  panic            Fatal        Kernel panic
  ```
- [ ] Include in ISO as `C:\Impossible\System\Docs\post-codes.txt`
- [ ] Accessible from shell: `postcode --list` shows full table
- [ ] Commit: `"docs: POST code reference card"`

---

## 3. Debug Color Bar Waterfall *(agent)*

**Prompt:** Enhance the raw colored pixel bars from the Hyper-V debugging session into a proper debug visualization. In debug/verbose mode (activated via `boot.conf verbose=1` or holding Shift at boot), the boot splash is replaced by a full-screen **color bar waterfall**: each `boot_progress()` call draws a horizontal bar spanning the screen width, colored by category, with a text label showing the hex code and subsystem name. Bars stack vertically from top to bottom — like a visual boot log. On hang, the last bar is where boot stalled. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: debug color bar waterfall mode"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!NOTE]
> **Design:** The waterfall is NOT the boot splash — it completely replaces it.
> Think of it as a visual equivalent of Linux's scrolling `dmesg` boot, but with
> color-coded categories and zero text parsing needed. At a glance, you see:
> green bars (memory OK), cyan bars (CPU/ACPI OK), orange bars (interrupts OK),
> then the screen stops at a yellow bar → storage is the hang point.

### 3.1 Bar Rendering

- [ ] Bar height: 12px (10px bar + 2px gap) — fits ~60 stages on a 720p screen
- [ ] Bar width: full screen width minus 16px margins
- [ ] Bar color: derived from code category (high nibble → color table):
  ```
  0x1_ Memory     → #27AE60 (green)     0x5_ Storage   → #F39C12 (yellow)
  0x2_ CPU/ACPI   → #00BCD4 (cyan)      0x6_ Network   → #2196F3 (blue)
  0x3_ Interrupts → #FF9800 (orange)    0x7_ Desktop   → #9C27B0 (purple)
  0x4_ Drivers    → #ECEFF1 (white)     0xF_ Fatal     → #F44336 (red)
  ```
- [ ] Each bar contains: `[0xNN]` hex code left-aligned + `subsystem_name` after code
- [ ] Text rendered using the minimal bitmap font (§2.1) extended to ASCII:
  - [ ] Extend hex font to cover A-Z, a-z, 0-9, space, underscore, period (basic ASCII)
  - [ ] Still hardcoded bitmaps — no TTF dependency (must work before font init)
- [ ] Current (latest) bar **pulses** if boot stalls (> 500ms without next progress call)
- [ ] Trailing edge of each bar: timing delta in ms (e.g., `[12ms]`) — right-aligned
- [ ] Commit (with §3.2): `"kernel: debug color bar waterfall mode"`

### 3.2 Mode Activation

- [ ] Activate when `boot.conf` contains `verbose=1`
  - [ ] → XREF: `TODO-005-Debug.md §2.2` — verbose mode decoupling
- [ ] Activate when Shift key is held during early boot
  - [ ] → XREF: `TODO-010-Bootloader.md §5.5` — boot menu key detection
- [ ] When active: skip `boot_splash_init()` entirely — draw bars instead
- [ ] When active: `boot_progress()` draws bar instead of updating splash text
- [ ] POST hex code (§2) still renders in the corner — it's always on
- [ ] After boot complete (`0x00`): show summary line at bottom:
  - [ ] `Boot complete: 34 stages, 1247ms total — Press any key for desktop`
- [ ] Build and test on QEMU with `verbose=1`: color bar waterfall visible
- [ ] Build and test without `verbose=1`: normal splash (no bars)
- [ ] Commit: `"kernel: debug color bar waterfall mode"`

---

## 4. Panic Forensic Evidence *(agent)*

**Prompt:** When the kernel panics, the color bar history serves as **visual forensic evidence** — a stack trace you can read without serial output. Every `boot_progress()` call is stored in a circular history buffer. On panic, the panic screen renders the last N progress bars below the error information, showing the exact execution path that led to the crash. Each bar that was "entered but never exited" (no subsequent progress) is highlighted in red — this is the active function at crash time. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: panic forensic progress bars"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!CAUTION]
> **Real-world scenario:** On real hardware with no serial port, the ONLY
> diagnostic information available is what's visible on the screen. The BSOD
> already shows registers and a stack trace — but the progress bar history
> adds *semantic context*: "the kernel was initializing AHCI when it crashed,"
> not just "RIP was at 0x00123456."

### 4.1 Progress History Buffer

- [ ] Allocate `boot_progress_history[64]` — static array, no kmalloc:
  ```c
  struct boot_progress_entry {
      uint8_t  code;              /* BP_xxx enum value             */
      uint8_t  flags;             /* BPF_ENTERED=1, BPF_COMPLETED=2 */
      uint16_t duration_ms;       /* Time spent in this stage      */
      uint32_t timestamp_ticks;   /* PIT ticks at entry            */
  };
  ```
- [ ] `boot_progress()` appends to history (circular, wraps at 64)
- [ ] Mark previous entry as `BPF_COMPLETED` when a new entry arrives
- [ ] Record `duration_ms` for completed entries (delta between timestamps)
- [ ] On panic: last entry is marked `BPF_ENTERED` but NOT `BPF_COMPLETED`
  → this is the function that was running when the crash occurred

### 4.2 Panic Screen Integration

- [ ] In `panic_screen()` (`panic.c`), after the error text and register dump:
  - [ ] Draw a horizontal separator line with label: `── Boot Progress ──`
  - [ ] Render last 16 progress bars from history (most recent at bottom)
  - [ ] Bars use the same color scheme as debug waterfall (§3)
  - [ ] The bar that was `BPF_ENTERED` but NOT `BPF_COMPLETED` has a **red border**
  - [ ] Label: `[0x50] ahci_init ← CRASHED HERE`
  - [ ] Completed bars show duration: `[0x21] acpi_init [45ms]`
- [ ] If no progress history (crash before first `boot_progress()` call):
  - [ ] Show: `[No boot progress recorded — crash in early init]`
- [ ] POST hex code in corner shows `FF` (panic code)
- [ ] Include progress history in crash dump file (`crashdump.log`)
- [ ] Build and test: trigger panic via `panic("test")` → bars visible on BSOD
- [ ] Commit: `"kernel: panic forensic progress bars"`

### 4.3 Panic QR Code (🚀 Impossible OS Feature)

**Prompt:** When the kernel panics, encode the crash information (stop code, RIP, last 8 boot progress codes, error code) into a QR code displayed on the BSOD. The user can scan it with their phone to instantly open a crash report page. This is something no other OS does — Windows shows a text-only stop code, and Linux dumps a text oops. A scannable QR code is the fastest path from "crash on screen" to "bug report filed." After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: panic QR code"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!TIP]
> **Competitive Edge:** Windows 10/11 added a QR code to BSODs, but it
> **always shows the same generic URL** (support.microsoft.com/stop-error) — it
> contains zero information about the actual crash. Impossible OS encodes the
> **actual crash data** into the QR code, linking to a pre-filled bug report.

- [ ] Implement minimal QR code encoder (Version 3, Error Correction L, alphanumeric mode):
  - [ ] 21×21 modules — fits in ~100×100 pixels at 5x scale
  - [ ] Alphanumeric capacity: 77 characters (sufficient for crash summary)
  - [ ] No dependency on external libraries — pure C, ~300 lines
- [ ] Encode crash data as URL:
  ```
  https://crash.impossibleos.dev/r?c=GPF&r=001311A2&b=50,21,22,23,33,44&e=7B3
  ```
  - [ ] `c=` stop code abbreviation (GPF, PF, DF, etc.)
  - [ ] `r=` RIP address (hex, 8 chars)
  - [ ] `b=` last 8 boot progress codes (comma-separated hex)
  - [ ] `e=` error code (hex)
- [ ] Render QR code in bottom-right of BSOD screen (below the countdown)
- [ ] Web endpoint decodes URL params and shows:
  - [ ] Human-readable crash summary
  - [ ] Pre-filled GitHub issue template
  - [ ] Known issue matching (search existing issues for same stop code + RIP)
- [ ] Build and test: trigger panic → QR code visible → scan with phone → URL works
- [ ] Commit: `"kernel: panic QR code"`

---

## 5. Runtime Vital Signs Strip (🚀 Impossible OS Feature) *(agent)*

**Prompt:** After boot, the color bars are no longer needed — but the kernel is still alive and running. Implement an optional 1-pixel-high strip at the very bottom of the screen that shows real-time kernel vital signs. This strip is invisible at first glance (it's 1 pixel!) but encodes information via color: interrupt activity (flashes), scheduler state, memory pressure, and disk I/O. Enabled via Control Panel → System → Developer Options → Show Kernel Vital Signs. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: runtime vital signs strip"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!TIP]
> **Competitive Edge:** No consumer OS has anything like this. Windows has Task
> Manager (separate app, heavyweight). Linux has htop (CLI). macOS has Activity
> Monitor. Impossible OS shows kernel health at a **literal glance** — a green
> bottom edge means healthy, yellow means busy, red means trouble. Power users
> will love it.

### 5.1 Strip Layout

- [ ] Height: 1 pixel (single scanline at very bottom of screen)
- [ ] Width: full screen, divided into segments:
  ```
  [CPU 0..N (proportional)] [IRQ activity] [Memory] [Disk I/O] [Network]
  ├── per-core: color = load ──┤ flash on ─┤ gradient ┤ flash ──┤ flash ─┤
  ```
- [ ] CPU segments: one pixel per logical core (or proportionally scaled)
  - [ ] Green = idle, Yellow = 25–75% load, Red = >75% load
  - [ ] White flash = interrupt being serviced on this core
- [ ] Memory segment (8 pixels wide):
  - [ ] Gradient from green (plenty free) to red (low memory)
  - [ ] Threshold: <10% free → solid red
- [ ] Disk I/O segment (4 pixels wide):
  - [ ] Flash white on read, flash yellow on write, dark when idle
- [ ] Network segment (4 pixels wide):
  - [ ] Flash blue on RX, flash green on TX, dark when idle

### 5.2 Integration

- [ ] Rendered directly to framebuffer — bypasses compositor (bottom scanline only)
- [ ] Updated by timer callback (LAPIC timer at 10 Hz — once every 100ms)
- [ ] Disabled by default — enabled via:
  - [ ] Registry: `HKLM\SYSTEM\Debug\VitalSigns = 1`
  - [ ] Control Panel → System → Developer Options → Show Kernel Vital Signs
  - [ ] `boot.conf`: `vitals=1`
- [ ] When enabled: compositor reserves bottom 1px (doesn't draw over it)
- [ ] When disabled: bottom scanline is normal desktop — zero overhead
- [ ] Data sources:
  - [ ] CPU load: per-core idle tick counter from scheduler
  - [ ] Memory: `pmm_free_pages()` / `pmm_total_pages()` ratio
  - [ ] Disk: increment counter in `ahci_read()`/`ahci_write()`
  - [ ] Network: increment counter in `rtl8139_tx()`/`rtl8139_rx()`
  - [ ] IRQ: per-vector counters from `irq_dispatch()` (→ XREF: `TODO-010.99 §4`)
- [ ] Build and test: enable via boot.conf, see strip flash during activity
- [ ] Commit: `"kernel: runtime vital signs strip"`

### 5.3 Expanded Vital Signs (Developer Overlay) *(agent)*

**Prompt:** When the user presses a hotkey (e.g., Ctrl+Shift+F12), the 1px strip expands into a 24px overlay at the bottom of the screen showing richer information: per-core load bars, memory graph, IRQ/s counter, and network throughput. Think of it as a transparent HUD — like game FPS overlays (Steam, MSI Afterburner) but for the OS kernel. This is a power-user feature hidden behind a key combo. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: developer overlay HUD"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!TIP]
> **Competitive Edge:** Gaming overlays (Steam FPS, MSI Afterburner) are
> third-party apps that hook into the graphics pipeline. macOS has "floating
> monitor" in Activity Monitor. Windows has nothing built-in. Impossible OS
> provides a **native kernel HUD** — first-party, zero-overhead when hidden,
> instant toggle via hotkey.

- [ ] Hotkey: Ctrl+Shift+F12 toggles the overlay on/off
- [ ] Height: 24px semi-transparent black bar at screen bottom
- [ ] Content (left to right):
  - [ ] Per-CPU mini bar graphs (8px wide each, height = load %)
  - [ ] `| CPU: 23% | MEM: 1.2G/4.0G (30%) | IRQ: 847/s | NET: ↑12K ↓340K |`
  - [ ] All text rendered with minimal bitmap font (§2.1 ASCII extension)
- [ ] Data updated at 4 Hz (250ms interval) — smooth but lightweight
- [ ] Rendered directly to framebuffer (same technique as 1px strip)
- [ ] When hidden: zero overhead (timer callback skips rendering)
- [ ] Build and test: toggle with hotkey, verify overlay appears/disappears
- [ ] Commit: `"kernel: developer overlay HUD"`

---

## 6. Alive Blink (Hang Detection) *(agent)*

**Prompt:** The simplest possible "is the kernel alive?" indicator: a single pixel in the bottom-right corner that toggles between two colors on every timer tick. If the pixel stops alternating, the kernel is hung. This works even when the rest of the screen is frozen, because the timer ISR writes directly to the framebuffer — no compositor, no locks, no allocations. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: alive blink pixel"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!NOTE]
> **Why not visible by default?** A blinking pixel in the corner would be
> distracting for normal users. But for developers and QA, it's invaluable:
> "Is the kernel frozen or just slow?" is answered instantly. Enabled via the
> same Developer Options toggle as Vital Signs.

- [ ] Timer ISR callback (PIT or LAPIC, whichever is active):
  - [ ] Toggle bottom-right pixel between `#00FF00` and `#003300`
  - [ ] Direct framebuffer write — exactly 4 bytes per toggle
  - [ ] No locks, no compositor, no allocations — ISR-safe
- [ ] If pixel stops blinking: kernel is hung (timer ISRs not firing)
- [ ] If pixel blinks but screen is frozen: compositor is hung (not the kernel)
  - [ ] This distinction helps narrow down the problem instantly
- [ ] Enabled when vital signs are enabled (same toggle)
- [ ] Build and test: pixel blinks in QEMU, stops on `cli; hlt` deadlock
- [ ] Commit: `"kernel: alive blink pixel"`

---

## 7. Pre-Kernel POST Codes (UEFI Phase) *(agent)*

**Prompt:** The `boot_progress()` API only works after the kernel is running. But boot can hang **inside the UEFI bootloader** — before the kernel even starts. Implement POST code output in `bootx64.c` using the same hex font and corner position. The UEFI bootloader writes hex codes directly to the GOP framebuffer during its own initialization: EFI table setup, GOP init, kernel loading, ExitBootServices. These codes use the `0x0_` prefix (reserved for UEFI phase). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: UEFI bootloader POST codes"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!NOTE]
> **Why this matters:** The existing `DRAW_BAR` macro in `bootx64.c` solved the
> same problem — but with raw pixel bars. POST hex codes are more compact (24×14
> pixels vs full-width bars) and carry machine-readable meaning.

- [ ] Define UEFI-phase POST codes (prefix `0x0_`):
  ```c
  #define EFI_POST_ENTRY        0x01  /* efi_main entered     */
  #define EFI_POST_GOP          0x02  /* init_gop started     */
  #define EFI_POST_GOP_OK       0x03  /* GOP mode set OK      */
  #define EFI_POST_KERNEL_LOAD  0x04  /* load_kernel started  */
  #define EFI_POST_KERNEL_OK    0x05  /* kernel ELF parsed    */
  #define EFI_POST_MMAP         0x06  /* GetMemoryMap started */
  #define EFI_POST_EXIT_BS      0x07  /* ExitBootServices     */
  #define EFI_POST_JUMP         0x08  /* jump_to_kernel       */
  ```
- [ ] Embed the same 5×7 hex bitmap font in `bootx64.c` (80 bytes — trivial)
- [ ] Write hex code to bottom-right corner of GOP framebuffer after `init_gop()`
- [ ] Before `init_gop()`: no framebuffer available — use serial output only
- [ ] `DRAW_BAR` macro becomes `efi_post_code()` — same position as kernel POST codes
- [ ] Seamless transition: kernel's first `boot_progress(0x10)` overwrites `0x08`
- [ ] Build and test: UEFI hex codes visible before kernel splash appears
- [ ] Commit: `"boot: UEFI bootloader POST codes"`

---

## 8. Antigravity Agent Updates

> [!IMPORTANT]
> **After implementing the Kernel Heartbeat system**, the following Antigravity
> agent configurations should be updated to ensure the AI agent knows about
> and correctly uses the new APIs.

### 8.1 Rules Update (`rules.md`)

- [ ] Add to **Known Gotchas** section:
  ```markdown
  - **Use `boot_progress()` not `boot_splash_status()`.** All boot stage
    announcements go through the unified `boot_progress(BP_xxx)` API.
    Never call `boot_splash_status()` directly — it's now an internal
    implementation detail of `boot_progress()`. Never write raw pixels
    to the framebuffer for debugging — use `boot_progress()` instead.
    *(Learned from Hyper-V debug bar refactor)*
  ```
- [ ] Add to **API Surface** section:
  ```markdown
  - **POST codes are the boot heartbeat.** Every kernel init function
    must call `boot_progress(BP_xxx)` at entry. The enum is defined in
    `include/kernel/boot_progress.h`. Adding a new init function
    requires adding a new `BP_xxx` code.
  ```

### 8.2 Skills Update

- [ ] Consider adding a `boot-progress` skill in `.agent/skills/`:
  - [ ] Documents how to add a new boot stage
  - [ ] Includes the POST code allocation table
  - [ ] Shows the correct pattern for wrapping an init function
  - [ ] Example:
    ```c
    boot_progress(BP_AHCI_INIT);
    ahci_init();
    /* boot_progress(BP_PARTITION_SCAN) marks ahci_init as complete */
    ```

### 8.3 Memory (Knowledge Graph)

- [ ] After implementation, create a knowledge item documenting:
  - [ ] The `boot_progress()` API and its output modes
  - [ ] The POST code allocation scheme (high nibble = category)
  - [ ] Integration points with panic, splash, and klog
  - [ ] This avoids the agent needing to re-discover the API each session

---

## Key Files

| File                                | Purpose                                               |
| ----------------------------------- | ----------------------------------------------------- |
| `src/kernel/boot_progress.c`        | [NEW] Core API, history buffer, serial logging        |
| `include/kernel/boot_progress.h`    | [NEW] `boot_code` enum, `boot_progress()` prototype   |
| `src/kernel/post_code.c`            | [NEW] Hex POST code renderer (5×7 bitmap font)        |
| `src/kernel/vital_signs.c`          | [NEW] Runtime 1px strip + expanded overlay            |
| `src/kernel/qr_encode.c`            | [NEW] Minimal QR code encoder for panic screen        |
| `src/kernel/panic.c`                | Integration: progress bars + QR code on BSOD          |
| `src/kernel/boot_splash.c`          | Integration: route `boot_progress()` → status text    |
| `src/kernel/main.c`                 | Replace `boot_splash_status()` → `boot_progress()`   |
| `src/kernel/main/boot_hw.c`         | Remove `HV_BAR` macros → `boot_progress()`           |
| `src/kernel/main/boot_interrupts.c` | Remove `HV_BAR` macros → `boot_progress()`           |
| `src/boot/uefi/bootx64.c`           | Remove `DRAW_BAR` → `efi_post_code()` (§7)           |
| `docs/post-codes.md`                | [NEW] Complete POST code reference card               |
| `.agent/rules.md`                   | Update: boot_progress gotcha + API surface            |
| `.agent/skills/boot-progress/`      | [NEW] Skill: how to add a new boot stage              |

---

## Priority Order

| ⭐ | Priority | Section                              | Description                                              |
| -- | :------: | ------------------------------------ | -------------------------------------------------------- |
| 💎 | 🔴 P0   | 1.1 Core API                        | Foundation — everything else depends on this              |
| 💎 | 🔴 P0   | 1.2 Instrument boot stages          | Replace ad-hoc HV_BAR and splash_status calls            |
| 💎 | 🟠 P1   | 2. POST hex code display            | Always-on — works on real hardware without serial         |
| 💎 | 🟠 P1   | 4.1–4.2 Panic forensic evidence     | Critical for real hardware debugging                     |
| 💎 | 🟠 P1   | 7. Pre-kernel POST codes (UEFI)     | Covers the gap before kernel starts                      |
| 💎 | 🟠 P1   | 8. Antigravity agent updates        | Agent must know about new API to use it correctly        |
| 💎 | 🟡 P2   | 1.3 klog integration                | Timestamps + live flush checkpoints                      |
| 💎 | 🟡 P2   | 2.3 POST code reference card        | Documentation for developers                             |
| 💎 | 🟡 P2   | 3. Debug color bar waterfall        | Developer UX — visible boot progress                     |
| 💎 | 🟡 P2   | 6. Alive blink pixel                | Simplest possible hang detection                         |
| ⭐ | 🟢 P3   | 4.3 Panic QR code               | Scan crash info with phone — no OS encodes actual data   |
| ⭐ | 🟢 P3   | 5.1–5.2 Vital signs strip       | Power user feature — kernel health at a glance           |
| ⭐ | 🔵 P4   | 5.3 Developer overlay HUD       | Native kernel HUD — like Steam FPS but for the OS        |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to both Windows and Linux.

---

## OS Comparison

| ⭐ | Feature                            | 🪟 Windows 11                        | 🐧 Linux 6.x                         | 🚀 Impossible OS                       |
| -- | ---------------------------------- | ------------------------------------ | ------------------------------------- | --------------------------------------- |
| 💎 | Boot progress API                  | ✅ Internal (hidden from users)      | ✅ `printk` + initcall levels         | ⬜ §1 — unified `boot_progress()`      |
| 💎 | POST codes on screen               | ❌ Server BMC only (not on screen)   | ❌ No equivalent                      | ⬜ §2 — on-screen hex POST codes       |
| 💎 | Verbose boot mode                  | ⚠️ `bcdedit /bootlog` (text only)   | ✅ Remove `quiet` (scrolling text)    | ⬜ §3 — color-coded bar waterfall      |
| 💎 | Boot hang diagnosis                | ❌ Blank screen, no info             | ⚠️ Last dmesg line (if visible)      | ⬜ §2+§3 — frozen hex + last bar       |
| ⭐ | **Panic boot context**         | ❌ BSOD shows registers only         | ❌ Oops shows call stack only         | ⬜ §4 — progress bars on BSOD          |
| ⭐ | **Panic QR code**              | ⚠️ Generic URL (no crash data)      | ❌ No equivalent                      | ⬜ §4.3 — QR encodes actual crash data |
| ⭐ | **Runtime vital signs**        | ❌ Task Manager (separate app)       | ❌ htop (CLI, separate process)       | ⬜ §5 — 1px strip, zero overhead       |
| ⭐ | **Developer overlay HUD**      | ❌ Performance Monitor (hidden)      | ❌ No built-in HUD                    | ⬜ §5.3 — Ctrl+Shift+F12 toggle        |
| 💎 | Kernel alive indicator             | ❌ No equivalent                     | ❌ No equivalent                      | ⬜ §6 — blink pixel (ISR-driven)       |
| 💎 | UEFI-phase diagnostics             | ✅ WinLoad progress internally       | ⚠️ EFI stub has minimal logging      | ⬜ §7 — UEFI POST codes on screen      |
| 💎 | Boot stage timing                  | ❌ ETW + WPA (dev tools required)    | ⚠️ `systemd-analyze` (text CLI)      | ⬜ §1.3 — per-stage ms in progress API |
| 💎 | Agent/tooling awareness            | N/A                                  | N/A                                   | ⬜ §8 — rules + skills for AI agents   |
