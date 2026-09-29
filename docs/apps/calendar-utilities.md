<!-- docs: covers=todo/11-apps/TODO-13-calendar-utilities.md sources=user/sysinfo/sysinfo.c,include/kernel/cpuid.h,include/kernel/smbios.h,include/kernel/mm/pmm.h,include/kernel/drivers/blkdev.h,include/kernel/drivers/keyboard.h,include/kernel/drivers/framebuffer.h,include/cursor.h reviewed=2026-09-29 order=13 -->
# Calendar, Sticky Notes and Utility Apps

## What is it?

This roadmap finishes the small accessory apps: recurring events and `.ics` export for the Calendar, Sticky Notes, a graphical System Information window, an On-Screen Keyboard, a screen Color Picker, Unicode coverage details in the Font Manager, and one shared About dialog every app uses. The base Calendar and the Font Manager are specified in the desktop shell roadmaps; this file adds what sits on top. None of these apps exist yet, though most of the kernel calls they read from have shipped.

## How does it work?

**Today.** None of the apps exist. What ships:

- **`sysinfo.exe`** already exists, as a command-line tool with one subcommand, `firmware-updates`, which prints the firmware advisor's report ([`sysinfo.c`](../../user/sysinfo/sysinfo.c)). It is built so more inventory views can be added as subcommands.
- **Hardware facts** for a System Information window: `cpuid_get()` for the CPU brand and features ([`cpuid.h`](../../include/kernel/cpuid.h)), `smbios_get_info()` for the manufacturer, product and serial ([`smbios.h`](../../include/kernel/smbios.h)), `pmm_get_total_frames()` and `pmm_get_free_frames()` for memory ([`pmm.h`](../../include/kernel/mm/pmm.h)), and `blkdev_count()` for disks ([`blkdev.h`](../../include/kernel/drivers/blkdev.h)).
- **Keyboard injection.** `keyboard_inject_scancode()` ([`keyboard.h`](../../include/kernel/drivers/keyboard.h)) feeds a scan code through the same path as the PS/2 interrupt, which is what an On-Screen Keyboard would press keys through.
- **Screen reads.** `fb_get_backbuffer()` ([`framebuffer.h`](../../include/kernel/drivers/framebuffer.h)) for the Color Picker's eyedropper, and a crosshair cursor sprite ([`cursor.h`](../../include/cursor.h)).

**Planned design.**

1. **Calendar.** Daily, weekly, monthly and yearly repeats with an end date or count, editing one occurrence or the whole series, RFC 5545 `.ics` export (`BEGIN:VCALENDAR`, `VEVENT`, `RRULE`), and import as a stretch.
2. **Sticky Notes.** Borderless always-on-top notes in six colours, saved to the Registry 500 ms after you stop typing, restored at sign-in.
3. **System Information.** A read-only two-column table of CPU, cores, memory, disks, firmware, uptime, host name and address, with Copy All and Save As Text. Its gathering function is shared with the System control panel page.
4. **On-Screen Keyboard.** A window that never takes focus, a five-row QWERTY layout, one-shot Shift, sticky Caps Lock, Ctrl and Alt, and Win+Ctrl+O to toggle it.
5. **Color Picker.** Win+Shift+C shows a crosshair and a 12 times magnified loupe, then a popup with RGB, HSL and HEX values to copy and a ten-colour history.
6. **Font Manager details.** Read a font's OS/2 table for its Unicode ranges and embedding rights, and its name table for family, copyright and licence.
7. **Shared dialogs.** `ui_dialog_about()`, `ui_dialog_progress()` and `ui_dialog_confirm()` so every app's Help, About and progress windows look the same.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `sysinfo firmware-updates` | Shipped |
| `cpuid_get()`, `smbios_get_info()`, `pmm_get_*_frames()`, `blkdev_count()` | Shipped |
| `keyboard_inject_scancode()`, `fb_get_backbuffer()` | Shipped |
| Always-on-top and no-focus window flags | Planned; today's flags are visible, decorated, movable, resizable, focused and dialog |
| Global hotkey table, clipboard, toasts | Planned in the window manager, [Clipboard](../desktop/clipboard.md) and [notifications](../graphics/start-menu-tray-notifications.md) roadmaps |
| `ui_dialog_about()` and friends | Planned in this roadmap, section 7 |

## How do I use it?

Only the command-line `sysinfo.exe` runs today:

```text
C:\> sysinfo firmware-updates
```

It prints the firmware advisor's table and ends with the line that Impossible OS does not write firmware. None of the graphical apps exist yet.

## Which roadmap owns what?

- The base Calendar (month grid, events pane, Add dialog) is section 6 of the utilities roadmap ([Task Manager, Device Manager and Core Utilities](../desktop/utilities.md)); the taskbar's calendar flyout belongs to the [Kernel Time and Taskbar Clock](../graphics/clock-time.md) roadmap.
- The Font Manager app is section 8 of the [File Associations, Shortcuts and System Resources](../desktop/file-associations.md) roadmap; the colour chooser dialog (a hue ring for apps) is in [Complex Controls and Dialogs](../graphics/complex-controls-dialogs.md), separate from the screen eyedropper here.
- The System control panel page is section 9 of the utilities roadmap. Sticky Keys, an accessibility feature, is unrelated to Sticky Notes.

## What is not implemented yet?

Nothing in this roadmap has started:

- [Calendar App: Recurring Events and ICS Export](../../todo/11-apps/TODO-13-calendar-utilities.md#1-calendar-app-recurring-events--ics-export-sonnet), which needs the base calendar
- [Sticky Notes](../../todo/11-apps/TODO-13-calendar-utilities.md#2-sticky-notes-sonnet)
- [System Information](../../todo/11-apps/TODO-13-calendar-utilities.md#3-system-information-sysinfoexe-sonnet), which must extend the existing `sysinfo.exe` rather than replace it
- [On-Screen Keyboard](../../todo/11-apps/TODO-13-calendar-utilities.md#4-on-screen-keyboard-sonnet) and the [Color Picker](../../todo/11-apps/TODO-13-calendar-utilities.md#5-color-picker-sonnet)
- [Font Manager: OS/2 and Unicode Coverage](../../todo/11-apps/TODO-13-calendar-utilities.md#6-font-manager-os2--unicode-coverage-sonnet)
- [Shared Help and About Dialog](../../todo/11-apps/TODO-13-calendar-utilities.md#7-shared-help--about-dialog-sonnet)

## How does it compare with Windows 11 and Linux?

Windows 11 ships Outlook's calendar with recurring events and `.ics`, Sticky Notes, `msinfo32`, the On-Screen Keyboard (Win+Ctrl+O) and a basic font list in Settings; the screen colour picker needs PowerToys. Linux desktops offer GNOME Calendar or KOrganizer, KNotes, `inxi`, Onboard, gpick and Font Manager. The Impossible OS plan puts the colour picker in the base system and shows more about a font's Unicode coverage than either. It does not exist yet.

## See also

- [Calendar, Sticky Notes and Utility Apps roadmap](../../todo/11-apps/TODO-13-calendar-utilities.md)
- [Task Manager, Device Manager and Core Utilities](../desktop/utilities.md)
- [Kernel Time and Taskbar Clock](../graphics/clock-time.md)
- [Text and Fonts](../graphics/text-fonts.md)
- [Accessibility Features](../services/accessibility.md)
- [Keyboard and Mouse Input](../desktop/input-system.md)
