<!-- docs: covers=todo/09-desktop-shell/TODO-11-control-panel.md sources=src/desktop/desktop.c,include/icon_store.h,include/desktop/theme_tokens.h,include/registry.h reviewed=2026-09-29 order=17 -->
# Control Panel and Settings

## What is it?

The Control Panel is where you change system settings: display, date and time, users, installed programs, network and the rest. This roadmap plans a Settings-style window that hosts applets written to the Windows `cpl.h` interface, nine core applets (system, display, network, sound, date and time, power, mouse, region and taskbar), four more (user accounts, programs, firewall and a date and time alias) and a search box across all settings. Nothing is implemented yet: the Start menu's Settings button and the Control Panel entries do nothing when clicked.

## How does it work?

**Today.** The desktop shows the way in, but there is nothing behind it ([`desktop.c`](../../src/desktop/desktop.c)):

- The Start menu footer draws a Settings button with `ICON_SETTINGS`; clicking it only closes the menu.
- The Start menu's right column lists Control Panel, and the desktop shows a Control Panel icon (`ICON_CONTROL_PANEL`, [`icon_store.h`](../../include/icon_store.h)); neither responds to a click.

Settings are stored in the Registry, read and written with `RegGetValue()` and `RegSetValueEx()` ([`registry.h`](../../include/registry.h)); there is no separate settings API. The system facts an applet would show are available to kernel code: the CPU brand from `cpuid_get()`, memory from `pmm_get_total_frames()`, the CPU count and `uptime()`. The Settings frame sizes are already design tokens: a 280 pixel navigation pane, 36 pixel navigation items and 64 pixel minimum setting cards (`THEME_SIZE_NAV_PANE_WIDTH`, `THEME_SIZE_NAV_ITEM_HEIGHT`, `THEME_SIZE_SETTINGS_CARD_MIN_HEIGHT` in [`theme_tokens.h`](../../include/desktop/theme_tokens.h)).

**Planned design.** The frame follows the [Settings and Control Panel design](../design/shell.md#settings-and-control-panel-frame).

1. **CPL framework.** `cpl.h` with the Win32 message IDs (`CPL_INIT`, `CPL_GETCOUNT`, `CPL_NEWINQUIRE`, `CPL_DBLCLK`, `CPL_EXIT` and the rest) and a compatible `NEWCPLINFO`.
2. **Host window.** A 980 by 640 Settings frame with the navigation pane and Win+I to open it. Applets are compiled in and looked up by `.cpl` file name; loading `.cpl` files as separate libraries comes later.
3. **Core applets.** System (`sysdm.cpl`: CPU, memory, uptime, computer name), display (`desk.cpl`: resolution, scaling, wallpaper, accent colour, light or dark), network (`ncpa.cpl`), sound (`mmsys.cpl`), date and time (`timedate.cpl`, with an analog clock), power (`powercfg.cpl`), mouse (`main.cpl`), region (`intl.cpl`) and taskbar auto-hide.
4. **More applets.** User accounts (`nusrmgr.cpl`), programs (`appwiz.cpl`, reading `HKLM\...\Uninstall`), firewall, and a date and time alias.
5. **Search.** A case-insensitive substring filter over every setting's name, highlighting matches, with Enter opening the first.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `RegGetValue()`, `RegSetValueEx()` | Shipped: settings storage |
| `THEME_SIZE_NAV_PANE_WIDTH` and the other Settings tokens | Shipped design tokens |
| Settings button and Control Panel icon | Drawn, not wired |
| `cpl.h`, `CPlApplet`, `control.exe`, applets | Planned |

## How do I use it?

It cannot be used yet. Settings that exist today are changed in the Registry or in `boot.conf`.

## What is not implemented yet?

- [CPL Framework](../../todo/09-desktop-shell/TODO-11-control-panel.md#1-cpl-framework-sonnet)
- [Control Panel Host App](../../todo/09-desktop-shell/TODO-11-control-panel.md#2-control-panel-host-app-sonnet)
- [Core Applets](../../todo/09-desktop-shell/TODO-11-control-panel.md#3-core-applets-sonnet), whose wallpaper, scaling and date and time pages depend on [Desktop Shell Features](../graphics/desktop-shell-features.md) and [Kernel Time and Taskbar Clock](../graphics/clock-time.md)
- [Additional Applets](../../todo/09-desktop-shell/TODO-11-control-panel.md#4-additional-applets-sonnet), with accounts from [Security and User Accounts](security-accounts.md)
- [Settings Search](../../todo/09-desktop-shell/TODO-11-control-panel.md#5-settings-search-sonnet)

The toggles, sliders and drop-downs the applets need are owned by [Extended Widget Library: Core Controls](../graphics/widget-library.md).

## How does it compare with Windows 11 and Linux?

Windows 11 has both the Settings app and the classic Control Panel, with `.cpl` applets, System Properties, display scaling from 100 to 500 percent, date and time, user accounts, Programs and Features and an indexed Settings search. Linux desktops use GNOME Settings or KDE System Settings, which have no applet ABI, with pages for display, date and time, users and software and a search bar. Impossible OS has no settings window yet. The plan keeps the Win32 applet interface so applets written for Windows could load, inside the Windows 11 Settings layout.

## See also

- [Control Panel and Settings roadmap](../../todo/09-desktop-shell/TODO-11-control-panel.md)
- [Shell design: Settings and Control Panel frame](../design/shell.md#settings-and-control-panel-frame)
- [Controls design: cards and settings rows](../design/controls.md#cards-and-settings-rows)
- [Desktop Shell Today](desktop-shell.md)
- [Registry](../kernel/registry.md)
