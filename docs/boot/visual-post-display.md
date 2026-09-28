<!-- docs: covers=todo/01-boot-platform/TODO-15-visual-post-display.md sources=src/kernel/vpd.c,include/kernel/vpd.h,include/kernel/vpd_font.h,src/kernel/main/boot_progress.c,src/kernel/main/boot_hw.c,src/kernel/main/boot_interrupts.c,src/kernel/main/boot_init.c,src/kernel/boot_splash.c,src/kernel/panic.c,src/kernel/test/test_vpd.c,include/kernel/boot_info.h reviewed=2026-09-28 order=15 -->
# Visual POST Display (VPD)

## What is it?

VPD is the kernel's boot-progress and crash-attribution screen: a table of named boot stages with a status, a POST code and TSC timing per stage, drawn straight to the raw framebuffer before any display driver exists. It replaced an earlier debugging aid of four coloured bars. VPD has one tier today, a pre-splash renderer that runs from the moment `boot_info` is parsed until the splash takes over (or for the whole boot in diagnostic mode). A second tier inside the splash is designed but not built.

## How does it work?

`vpd_init()` runs early in `boot_phase0()`, right after `boot_info` is parsed, and stores the framebuffer pointer, pitch and size. It follows the `postbars` key in `boot.conf`: when config has been parsed and `postbars` is off, VPD stays inactive; otherwise it activates, so diagnostics are available on bare metal even before config is read.

Every `boot_progress(phase, step, postcode)` call (the kernel's general milestone hook) writes the POST code to port 0x80, a RAM shadow and the always-on corner overlay (`post_display16()`, independent of VPD), and, when VPD is active, calls `vpd_stage_begin()`. That draws a new row (status square, name dot-leadered to a fixed column, POST code) and marks the previous row done with a green check and right-aligned elapsed time from the TSC. Timing is left blank, not guessed, when the TSC frequency is unknown or the delta would overflow. A phase change draws a heading with an underline and a separator. Up to `VPD_MAX_ROWS` (30) rows fit; after that VPD stops adding rows while the POST path carries on.

Above the table, `vpd_init()` draws an information header: display mode, total RAM, TSC frequency, UEFI boot time, ACPI version, the UEFI `BootCurrent` entry and its description, Secure Boot and Setup Mode state, the firmware boot timeout, the "reboot to firmware" flag, and, on an A/B disk, each slot's state.

Crash attribution works across a reboot. `boot_hw.c` reads the previous boot's POST code from UEFI NVRAM before this boot writes its own, logs `[BOOT] Last boot succeeded` or `[BOOT] Last boot failed`, and calls `vpd_crash_banner()`, which names the code with `vpd_post16_name()`. On a panic, `panic_screen()` calls `vpd_stage_fail()` to mark the current row red before writing NVRAM, so the next boot's banner reports the failure.

`postbars=diag` keeps VPD on screen for the whole boot: `boot_splash_init()` returns early after locking the compositor and restricting on-screen `klog` output to fatal messages, so the splash never draws over the table.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `vpd_init()` | Stores the framebuffer and draws the information header ([`vpd.c`](../../src/kernel/vpd.c), [`vpd.h`](../../include/kernel/vpd.h)) |
| `vpd_stage_begin(phase, name, postcode)` | Draws a new stage row and closes the previous one with its elapsed time ([`vpd.c`](../../src/kernel/vpd.c)) |
| `vpd_stage_done()` / `vpd_stage_fail()` | Marks the current stage done (green) or failed (red) ([`vpd.c`](../../src/kernel/vpd.c)) |
| `vpd_is_active()` / `vpd_stop_tier1()` | Query and end pre-splash rendering, called before the splash takes over ([`vpd.c`](../../src/kernel/vpd.c)) |
| `vpd_crash_banner(last_postcode)` | Shows the previous boot's result from its NVRAM POST code ([`vpd.c`](../../src/kernel/vpd.c)) |
| `vpd_post16_name(code)` | POST code to stage name, `"UNKNOWN"` for unmapped codes ([`vpd.c`](../../src/kernel/vpd.c)) |
| `boot_post_write16()` / `boot_post_nvram_write16()` / `boot_post_read16()` | Volatile POST write, NVRAM milestone write, and cross-boot NVRAM read ([`boot_init.c`](../../src/kernel/main/boot_init.c)) |
| `post_display16(code)` | Always-on corner POST overlay, independent of `postbars` ([`boot_progress.c`](../../src/kernel/main/boot_progress.c)) |
| `boot_progress(phase, step, postcode)` | The milestone hook that drives POST codes, the overlay and VPD together ([`boot_init.c`](../../src/kernel/main/boot_init.c)) |
| `postbars` in `boot.conf` | `off`, `on` or `diag` ([`boot_info.h`](../../include/kernel/boot_info.h)) |
| `vpd_font.h` | Embedded 5x7 bitmap font used for all VPD text ([`vpd_font.h`](../../include/kernel/vpd_font.h)) |

## How do I use it?

Set the key in `boot.conf`:

```
postbars=off    # default: normal splash, no VPD
postbars=on     # VPD during the pre-splash window, then the splash takes over
postbars=diag   # VPD stays on screen for the whole boot; splash art is skipped
```

```bash
bash scripts/test.sh SUITE=boot   # POST code uniqueness and vpd_post16_name tests
make test-boot                    # same, as a make target
```

Serial carries each milestone as a POST line, and the previous boot's result as `[BOOT] Last boot succeeded` or `[BOOT] Last boot failed`. On screen with `postbars=on` or `postbars=diag`, the same stages appear as rows grouped under phase headings, each with its POST code and an `NNNms` time.

## What is not implemented yet?

- The splash-integrated second tier (named stages with timing and a fading history inside the splash text area, and a richer diagnostic layout): [Tier 2: Splash-Integrated Progress](../../todo/01-boot-platform/TODO-15-visual-post-display.md#9-tier-2-splash-integrated-progress-deferred).
- A seamless handoff from the raw-framebuffer table to that second tier; today the splash simply draws over it: [Seamless Tier Transition](../../todo/01-boot-platform/TODO-15-visual-post-display.md#10-seamless-tier-transition-deferred).
- Pixel-format-aware colours: VPD writes BGRX values regardless of the framebuffer's format, so on an RGBX framebuffer red renders blue: [Tier 1: Pre-Splash VPD Renderer](../../todo/01-boot-platform/TODO-15-visual-post-display.md#4-tier-1-pre-splash-vpd-renderer).
- The panic path's NVRAM write uses the sleepable runtime-services lock, which can block if runtime services are held: [4-Digit POST Code System](../../todo/01-boot-platform/TODO-15-visual-post-display.md#1-4-digit-post-code-system-0x0000-0xffff).
- A clean panic writes the generic `POST16_BOOT_FAILED` code, so the next boot knows the boot failed but not the exact stage: [NVRAM Crash Persistence and "Last Boot Failed" Display](../../todo/01-boot-platform/TODO-15-visual-post-display.md#8-nvram-crash-persistence-and-last-boot-failed-display).
- Three POST coverage gaps that can attribute a crash to the wrong stage (kernel entry, boot_info address validation, and the async storage path): [POST Codes in Every Boot Function](../../todo/01-boot-platform/TODO-15-visual-post-display.md#2-post-codes-in-every-boot-function).
- The raw render helpers in `vpd_font.h` have no bounds checks; they are unused, superseded by the checked path in `vpd.c`: [Embedded 5x7 Bitmap Micro-Font](../../todo/01-boot-platform/TODO-15-visual-post-display.md#3-embedded-5x7-bitmap-micro-font).

## How does it compare with Windows 11 and Linux?

The pre-splash table matches the basic progress of the Windows 11 spinner and the Linux Plymouth splash, and goes further in two ways neither offers at kernel level: an embedded font draws named stages with millisecond timing before any font or display driver exists, and the next boot shows which stage the previous boot died in (Windows shows a generic stop code; Linux keeps no persisted stage record). The port 0x80 POST codes mirror a motherboard POST display, which Linux has no equivalent for. The gap is polish: the splash-integrated tier is still deferred.

## See also

- [Visual POST Display roadmap](../../todo/01-boot-platform/TODO-15-visual-post-display.md)
- [Boot Diagnostics](boot-diagnostics.md)
- [`boot-timeline.json` Wire Format](boot-timeline-schema.md)
- [boot_info Field Ownership](boot-info-fields.md)
- [Bare Metal Gotchas](../infrastructure/bare-metal-gotchas.md)
