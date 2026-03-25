# P0504 — Control Panel

> **Goal:** A modular Control Panel host application using CPL applets (.cpl files),
> covering display, system, network, sound, datetime, power, and more — matching
> the Windows Control Panel architecture.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Control Panel

### 1.1 CPL Framework

**Prompt:** CPL (Control Panel Library) defines a standard interface for Control Panel applets — matching the Windows `CPlApplet` architecture. Each applet is a single C function that responds to messages: `CPL_INIT` (allocate resources), `CPL_GETCOUNT` (return number of applets in this module), `CPL_INQUIRE` / `CPL_NEWINQUIRE` (return name/icon/description), `CPL_DBLCLK` (user opened the applet — draw UI), `CPL_STOP` (cleanup one applet), `CPL_EXIT` (module unloading). The `NEWCPLINFO` struct provides name, description, icon, and a host-provided drawing surface. This architecture matches Windows exactly so Win32 programs calling `ShellExecute("desk.cpl")` work natively. After completing all items,sh clean`, and commit as `"apps: CPL applet interface"`.
- [ ] Create `include/cpl.h` — CPL interface (matches Windows `cpl.h`)
- [ ] Define messages: `CPL_INIT`, `CPL_GETCOUNT`, `CPL_INQUIRE`, `CPL_NEWINQUIRE`, `CPL_DBLCLK`, `CPL_STOP`, `CPL_EXIT`
- [ ] Define `NEWCPLINFO` struct (name, description, icon, lData)
- [ ] Define `CPlApplet` function pointer type: `LONG CPlApplet(HWND hwndCPl, UINT msg, LPARAM lParam1, LPARAM lParam2)`
- [ ] Commit: `"apps: CPL applet interface"`

### 1.2 Control Panel Host App

**Prompt:** The Control Panel is a two-panel window: category sidebar on the left, applet content area on the right. At startup, scan `C:\Impossible\System32\` for `.cpl` files (in practice, these are compiled-in applet functions registered at init). The sidebar groups applets by category (System, Personalization, Network, Hardware). Clicking an applet sends `CPL_DBLCLK` and renders that applet's UI in the content area. After completing all items,sh clean`, and commit as `"apps: Control Panel host"`.
- [ ] Create `src/apps/control/control.c`
- [ ] UI layout: category sidebar (left) + applet area (right)
- [ ] Scan `C:\Impossible\System32\` for `.cpl` files at startup
- [ ] For each `.cpl`: load, call `CPL_INIT` + `CPL_NEWINQUIRE`, add to category list
- [ ] Category sidebar: System, Personalization, Network and Internet, Hardware and Sound
- [ ] Click an applet → send `CPL_DBLCLK` with host surface
- [ ] Back button → return to applet list
- [ ] On close → call `CPL_STOP` + `CPL_EXIT` for each loaded applet
- [ ] Commit: `"apps: Control Panel host"`

### 1.3 Core Applets

**Prompt:** Ship 9 essential applets using Windows-standard CPL names: `sysdm.cpl` (System Properties — OS version, CPU, RAM from CPUID/PMM), `desk.cpl` (Display — resolution selector, DPI scale, wallpaper, accent color, dark/light mode), `ncpa.cpl` (Network Connections — IP, DHCP toggle, DNS, hostname), `mmsys.cpl` (Sound — volume slider, mute toggle), `timedate.cpl` (Date and Time — timezone, 12h/24h, NTP sync), `powercfg.cpl` (Power Options — screen timeout, shutdown/restart), `main.cpl` (Mouse — cursor theme, cursor size, sensitivity), `intl.cpl` (Region — date format, number format), `taskbar.cpl` (Taskbar — height, position, auto-hide). Each applet reads/writes Registry keys and calls the corresponding system functions. After completing all items,sh clean`, and commit as `"apps: Control Panel core applets"`.
- [ ] `sysdm.cpl.c` — System Properties: OS version, CPU, RAM, hardware summary
- [ ] `desk.cpl.c` — Display: resolution, DPI scale, wallpaper, accent color, dark/light mode
- [ ] `ncpa.cpl.c` — Network Connections: IP address, DHCP toggle, DNS, hostname
- [ ] `mmsys.cpl.c` — Sound: volume slider, mute toggle
- [ ] `timedate.cpl.c` — Date and Time: timezone selector, 12h/24h toggle, NTP sync
- [ ] `powercfg.cpl.c` — Power Options: screen timeout, sleep settings, shutdown/restart
- [ ] `main.cpl.c` — Mouse Properties: cursor theme, cursor size, sensitivity
- [ ] `intl.cpl.c` — Region: date format, number format
- [ ] `taskbar.cpl.c` — Taskbar: height, position, auto-hide toggle
- [ ] Commit: `"apps: Control Panel core applets"`

### 1.4 Additional Applets

**Prompt:** Additional Control Panel applets: `nusrmgr.cpl` (User Accounts — user management, ties into Security §7), `appwiz.cpl` (Programs and Features — installed programs, uninstall), `diskmgmt.cpl` (Disk Management — disk usage, drive info), `firewall.cpl` (Windows Firewall — enable/disable, rules). After completing all items,sh clean`, and commit as `"apps: Control Panel additional applets"`.
- [ ] `nusrmgr.cpl.c` — User Accounts (future)
- [ ] `appwiz.cpl.c` — Programs and Features: installed programs, uninstall
- [ ] `diskmgmt.cpl.c` — Disk Management: disk usage, drive information
- [ ] `firewall.cpl.c` — Firewall: enable/disable toggle, rule list
- [ ] Commit: `"apps: Control Panel additional applets"`

