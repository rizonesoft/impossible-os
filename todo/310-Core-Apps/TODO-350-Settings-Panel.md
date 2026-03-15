# P0504 — Settings Panel

> **Goal:** A modular settings host application using SPL applets, covering
> display, theme, network, sound, datetime, power, and more.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Settings Panel

### 1.1 SPL Framework

**Prompt:** SPL (Settings Panel Library) defines a standard interface for settings applets. Each applet is a single C function that responds to messages: SPL_INIT (allocate resources), SPL_GETINFO (return name/icon/category), SPL_OPEN (draw UI to provided surface), SPL_CLOSE (cleanup), SPL_SAVE (write changes to Registry). The `spl_panel_t` provides the drawing surface, mouse state, and a Registry root path. This architecture lets settings be modular — new applets can be added without changing the host app. After completing all items, create `docs/architecture/settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: SPL applet interface"`.


- [ ] Create `include/spl.h` — SPL interface
- [ ] Define messages: `SPL_INIT`, `SPL_GETINFO`, `SPL_OPEN`, `SPL_CLOSE`, `SPL_SAVE`
- [ ] Define `spl_info_t` (name, description, icon_path, category, version)
- [ ] Define `spl_panel_t` (surface, width, height, mouse state, registry_root)
- [ ] Define `spl_applet_fn` function pointer type
- [ ] Commit: `"apps: SPL applet interface"`

### 1.2 Settings Host App

**Prompt:** The Settings app is a two-panel window: category sidebar on the left, applet content area on the right. At startup, scan `C:\Impossible\System\Settings\` for .spl files (in practice, these are compiled-in applet functions registered at init). The sidebar groups applets by category (System, Personalization, Apps, Privacy). Clicking an applet name in the sidebar calls SPL_OPEN and renders that applet's UI in the content area. A Back button returns to the applet list. After completing all items, update `docs/architecture/settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Settings Panel host"`.


- [ ] Create `src/apps/settings/settings.c`
- [ ] UI layout: category sidebar (left) + panel area (right)
- [ ] Scan `C:\Impossible\System\Settings\` for `.spl` files at startup
- [ ] For each `.spl`: load, call `SPL_INIT` + `SPL_GETINFO`, add to category list
- [ ] Category sidebar: System, Personalization, Apps, Privacy, Update
- [ ] Click an applet name → call `SPL_OPEN` with panel surface
- [ ] Back button → return to applet list
- [ ] On close → call `SPL_CLOSE` for active applet
- [ ] Commit: `"apps: Settings Panel host"`

### 1.3 Core Applets

**Prompt:** Ship 9 essential applets: `about.spl` (simplest — display OS version, CPU, RAM from CPUID/PMM), `display.spl` (resolution selector, DPI scale dropdown, brightness slider), `theme.spl` (accent color picker, dark/light toggle, corner radius slider), `wallpaper.spl` (browse images in Wallpapers folder, set wallpaper, fit mode), `network.spl` (IP, DHCP toggle, DNS, hostname from network stack), `sound.spl` (volume slider, mute toggle), `datetime.spl` (timezone, 12h/24h, date format, NTP sync button), `power.spl` (screen timeout, shutdown/restart buttons), `taskbar.spl` (height, position, auto-hide). Each applet reads/writes Registry keys and calls the corresponding system functions. After completing all items, update `docs/architecture/settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Settings Panel core applets"`.


- [ ] `about.spl.c` — OS version, CPU, RAM, hardware summary (simplest applet)
- [ ] `display.spl.c` — resolution selector, DPI scale dropdown, brightness slider
- [ ] `theme.spl.c` — accent color picker, dark/light mode toggle, corner radius
- [ ] `wallpaper.spl.c` — wallpaper selector (browse images), fit mode
- [ ] `network.spl.c` — IP address, DHCP toggle, DNS, hostname
- [ ] `sound.spl.c` — volume slider, mute toggle
- [ ] `datetime.spl.c` — timezone selector, 12h/24h toggle, date format, NTP sync button
- [ ] `power.spl.c` — screen timeout, sleep settings, shutdown/restart
- [ ] `taskbar.spl.c` — taskbar height, position, auto-hide toggle
- [ ] Commit: `"apps: Settings Panel core applets"`

### 1.4 Additional Applets

**Prompt:** Additional settings applets for less common configurations: `cursors.spl` (cursor theme, cursor size using DPI scaling from Phase 02 §8), `fonts.spl` (installed fonts list, default system font selector), `accounts.spl` (user management — future, ties into Phase 03 §11.1), `apps.spl` (installed programs list, uninstall), `storage.spl` (disk usage overview, drive info from blkdev layer). After completing all items, update `docs/architecture/settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Settings Panel additional applets"`.


- [ ] `cursors.spl.c` — cursor theme, cursor size
- [ ] `fonts.spl.c` — installed fonts, default system font
- [ ] `accounts.spl.c` — user management (future)
- [ ] `apps.spl.c` — installed programs, uninstall
- [ ] `storage.spl.c` — disk usage, drive information
- [ ] Commit: `"apps: Settings Panel additional applets"`

### 1.5 Win32 .cpl Mapping

**Prompt:** Windows Control Panel applets (.cpl files) are DLLs that export a `CPlApplet` entry point. Map known .cpl names to SPL equivalents: `desk.cpl` → display.spl, `mmsys.cpl` → sound.spl, `sysdm.cpl` → about.spl. When a Win32 program calls `ShellExecute("desk.cpl")`, redirect to the Settings app's corresponding applet. This is a stretch goal for Win32 compatibility (Phase 10). After completing all items, update `docs/architecture/settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: CPL to SPL mapping"`.


- [ ] *(Stretch)* CPL → SPL message translation
- [ ] *(Stretch)* `desk.cpl` → `display.spl`, `mmsys.cpl` → `sound.spl`, etc.
- [ ] *(Stretch)* Add to builtin stub table

