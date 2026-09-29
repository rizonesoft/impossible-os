<!-- docs: covers=todo/10-platform-services/TODO-05-screensaver-widgets-display.md sources=include/kernel/boot_info.h,src/kernel/main/boot_interrupts.c,src/boot/uefi/bootx64.c,include/gfx.h,include/kernel/drivers/framebuffer.h,include/kernel/timer.h,include/kernel/mm/pmm.h,include/desktop/wm.h reviewed=2026-09-29 order=5 -->
# Screensaver, Widgets and Display

## What is it?

This roadmap covers three desktop features: screensavers that start when the machine is idle and can hand off to the lock screen, a layer of small desktop widgets (clock, CPU and memory meters, calendar, notes), and display management (listing resolutions, choosing one in the Display applet, and a DPI setting), with multi-monitor enumeration as a stub. None of its six sections has shipped. The data the display work needs, the firmware's list of video modes, already reaches the kernel.

## How does it work?

**Today.** None of the screensaver, widget or display-mode code exists. What does:

- **Video modes.** The bootloader asks UEFI GOP for every mode it offers and hands up to 32 of them to the kernel in `boot_info`, as `gop_modes[]` (width, height, pixels per scan line and pixel format), `gop_mode_count` and `gop_mode_selected` ([`boot_info.h`](../../include/kernel/boot_info.h), filled in [`bootx64.c`](../../src/boot/uefi/bootx64.c)). At boot the kernel logs the active mode and how many modes were enumerated, and nothing else reads the list yet ([`boot_interrupts.c`](../../src/kernel/main/boot_interrupts.c)). GOP gives no refresh rate, and `gop_mode_selected` can equal `gop_mode_count` when the active mode is not in the table, so a reader must check the index.
- **Drawing.** `gfx_acrylic()`, `gfx_fill_rect()`, `gfx_fill_circle()`, `gfx_draw_line()` and `gfx_blit()` ([`gfx.h`](../../include/gfx.h)), and `fb_lock_compositor()` for exclusive drawing ([`framebuffer.h`](../../include/kernel/drivers/framebuffer.h)).
- **Time and memory.** `system_get_ticks()` and `uptime()` ([`timer.h`](../../include/kernel/timer.h)), and `pmm_get_total_frames()` and `pmm_get_free_frames()` for a memory meter ([`pmm.h`](../../include/kernel/mm/pmm.h)).

The mode cannot be changed after boot today: GOP's `SetMode` is gone once the loader exits boot services, so a runtime mode change needs a display driver from the [GPU and Display Drivers](../hardware/gpu-display-drivers.md) roadmap.

**Planned design.**

1. **Screensavers.** The window manager records the time of the last input; after the configured idle time a screensaver takes the screen through a `scr_entry_fn(msg, surface)` callback with init, frame and close messages. Five are planned: blank, starfield, matrix, bouncing logo and clock.
2. **Lock bridge.** If `RequirePassword` is set, dismissing the screensaver opens the [lock screen](../desktop/security-accounts.md).
3. **Widget framework.** A `struct widget` with init, render, tick, click and close callbacks, drawn by the compositor on an acrylic background, draggable, with positions kept in the Registry.
4. **Built-in widgets.** Analog clock, CPU meter (a 60-second rolling bar), RAM monitor, month calendar and quick notes.
5. **Display management.** `display_enum_modes()` from the GOP list, the chosen mode written to the Registry for the next boot, a resolution list in the Display applet, and a DPI slider that defers to the single DPI owner in [Desktop Shell Features](../graphics/desktop-shell-features.md).
6. **Multi-monitor stubs.** `monitor_enum()` and `wm_to_monitor()` returning one monitor until a multi-head driver exists.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `boot_info->gop_modes[]`, `gop_mode_count`, `gop_mode_selected` | Shipped (logged only) |
| `gfx_*` drawing, `fb_lock_compositor()`, `system_get_ticks()`, `pmm_get_free_frames()` | Shipped |
| `scr_entry_fn`, idle tracking, the five screensavers | Planned |
| `struct widget`, `widget_manager_render()`, the five widgets | Planned |
| `display_enum_modes()`, `display_get_current_mode()`, `monitor_enum()` | Planned |
| `HKCU\Software\Impossible\Screensaver\*`, `...\Widgets\{id}\*` | Planned Registry keys |

## How do I use it?

Boot and look for the `GOP` line in the serial log, in the form `<width>x<height> <pixel format> (mode <n> of <count> available)`: it shows the active resolution and how many modes the firmware offered. Nothing else is usable yet.

## What is not implemented yet?

- [Screensaver System](../../todo/10-platform-services/TODO-05-screensaver-widgets-display.md#1-screensaver-system-sonnet) and the [lock screen bridge](../../todo/10-platform-services/TODO-05-screensaver-widgets-display.md#2-screensaver--lock-screen-sonnet)
- [Desktop Widget Framework](../../todo/10-platform-services/TODO-05-screensaver-widgets-display.md#3-desktop-widget-framework-sonnet) and [Built-in Widgets](../../todo/10-platform-services/TODO-05-screensaver-widgets-display.md#4-built-in-widgets-sonnet); the CPU meter needs the per-task list from the [utilities roadmap](../../todo/09-desktop-shell/TODO-12-utilities.md)
- [Display Management](../../todo/10-platform-services/TODO-05-screensaver-widgets-display.md#5-display-management-sonnet), whose Display applet is in the [Control Panel roadmap](../../todo/09-desktop-shell/TODO-11-control-panel.md)
- [Multi-Monitor Stubs](../../todo/10-platform-services/TODO-05-screensaver-widgets-display.md#6-multi-monitor-stubs-sonnet); real multi-head support belongs to the display driver roadmap

## How does it compare with Windows 11 and Linux?

Windows 11 still ships classic screensavers, has a web-based Widgets panel rather than desktop widgets, and supports full multi-monitor layouts and per-monitor DPI. Linux desktops use xscreensaver or the GNOME and KDE lockers, KDE plasmoids for widgets and `xrandr` or Wayland output management. The Impossible OS plan draws widgets as a compositor layer with no process per widget, and reads the display modes straight from the firmware list the bootloader already hands over.

## See also

- [Screensaver, Widgets and Display roadmap](../../todo/10-platform-services/TODO-05-screensaver-widgets-display.md)
- [Desktop Shell Features](../graphics/desktop-shell-features.md)
- [GPU and Display Drivers](../hardware/gpu-display-drivers.md)
- [Security and User Accounts](../desktop/security-accounts.md)
- [Shell design](../design/shell.md)
