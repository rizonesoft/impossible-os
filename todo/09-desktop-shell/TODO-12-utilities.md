---
schema_version: 1
id: utilities
domain: 09-desktop-shell
status: active
title: "TODO-12 -- Task Manager, Device Manager & Core Utilities"
---

# TODO-12 -- Task Manager, Device Manager & Core Utilities

> **Goal:** Deliver the process manager, hardware inspector, and the full suite of small core apps (Calculator, Image Viewer, Screenshot region select, Archive Manager, Calendar, System Info) plus shell command expansion -- everything that completes Impossible OS as a self-sufficient desktop OS.

> [!IMPORTANT]
> **Already exists**: `struct task { pid, state, name, parent_pid, num_threads }` + `task_get_by_pid()` + `SYS_GETPROCS=10` in `task.h/syscall.h`. `pci_scan()` + `struct pci_device { vendor_id, device_id, ... }` + `pci_find_device()` in `pci.h` -- but no `pci_get_all_devices()` accessor. `image_load/scale/save_bmp()` in `image.h`. `kmath_sqrt/pow/fabs/floor` in `kmath.h` -- but no `kmath_sin/cos/tan/log`. `pmm_get_total_frames()`, `cpuid_get()->brand`, `acpi_get_cpu_count()`. `zip_create/extract/list()` (TODO-04 §6 forward dep). `CTRL_TREEVIEW` (08-graphics-ui/TODO-06 §2) / `CTRL_TABSTRIP` (08-graphics-ui/TODO-05 §6) forward deps. `search_query_scoped()` (TODO-05). `context_menu_show()` (D08 T09 §1). **Missing**: `cpu_ticks` field in `struct task` (add for CPU%); `sched_get_task_list(out, max)` helper; `pci_get_all_devices(out, max)` helper; `blkdev_get_stats()` / `net_get_stats()` for performance charts; `kmath_sin/cos/tan/log` for scientific calculator. **New functions needed in §1**: add `uint64_t cpu_ticks` to `struct task`; add `sched_get_task_list(proc_info_t *out, int max)`. Complete sections in order: shell commands → calculator → image viewer → screenshot → archive manager → calendar → task manager → device manager → system info.

## Inputs

- `include/kernel/sched/task.h` -- `struct task`, `task_get_by_pid()`, `SYS_GETPROCS=10` -- §1 process list; add `cpu_ticks` for CPU%
- `include/kernel/drivers/pci.h` -- `pci_scan()`, `struct pci_device`, `pci_find_device()` -- §2 device tree; add `pci_get_all_devices()`
- `include/kernel/image.h` -- `image_load/scale/save_bmp()` -- §5 image viewer, §6 screenshot
- `include/kernel/kmath.h` -- `kmath_sqrt/pow/fabs/cos` -- §2 calculator; `sin/tan/log/exp` already in `include/libc/math.h`
- `include/kernel/mm/pmm.h` -- `pmm_get_total_frames()`, `pmm_get_free_frames()` -- §1 RAM bar, §9 system info
- `include/kernel/cpuid.h` -- `cpuid_get()->brand/vendor/model` -- §9 system info
- `include/kernel/acpi.h` -- `acpi_get_cpu_count()` -- §9 system info
- `include/kernel/timer.h` -- `uptime()`, `system_get_ticks()` -- §1 CPU% rolling sample
- `include/registry.h` -- `HKCU\...\Calendar\Events\*`, `HKLM\...\Uninstall\*` -- §8 calendar events, §9 software env
- `include/kernel/fs/vfs.h` -- `vfs_readdir`, `vfs_stat` -- §5 next/prev image in folder, §8 scan
- `include/desktop/controls.h` (08-graphics-ui/TODO-05, TODO-06) -- `CTRL_TREEVIEW`, `CTRL_TABSTRIP`, `dialog_file_open()` -- §4 device tree, §7 archive, §9 tabs
- `include/kernel/zip.h` (TODO-04 §6) -- `zip_open/extract/list/add_file()` -- §1 archive manager
- `include/kernel/time.h` (TODO-10) -- `time_now()`, `time_to_datetime()`, `time_format()` -- §9 calendar, §1 CPU%
- → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §5` -- §7 Screenshot builds on the existing PrintScreen capture module there
- → XREF: `08-graphics-ui/TODO-12-clock-time.md §3` -- §9 Calendar opened from taskbar clock flyout
- → XREF: `09-desktop-shell/TODO-04-recycle-zip-scheduler.md §6` -- §1 Archive Manager uses ZIP kernel API
- → XREF: `09-desktop-shell/TODO-11-control-panel.md §5` -- §3 System Info opened from `sysdm.cpl` "More info" link

## Outcome

- Shell: `cd/pwd/mkdir/rmdir/cp/mv/rm/touch/whoami/date/free`; output redirect `>`/`>>`; stretch pipe `|`, tab completion, `&&`.
- Calculator: fixed-window button grid, two-operand model, C/CE/⌫/±/1x/x²/√, memory M+/−/R/C/S; stretch Scientific/Programmer/history.
- Image Viewer: `image_load()` + fit/zoom/pan, prev/next in folder, slideshow, set-as-wallpaper.
- Screenshot region select: Win+Shift+S dim overlay + rubber-band + capture; countdown overlay.
- Archive Manager: browse `.zip` contents, Extract All, Add Files, New archive.
- Calendar: month grid with accent today, click-day events pane, Registry-backed events, taskbar clock integration.
- Task Manager: Processes tab (CPU%/RAM/PID list, End Task) + Performance tab (rolling charts), Ctrl+Shift+Esc.
- Device Manager: `CTRL_TREEVIEW` with PCI device categories, properties pane, driver status.
- System Info (`msinfo32`): tabbed System Summary / Components / Software Env, export to text.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                 | Depends On                                                                                                                                            | Status |
| --- | :---: | ------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Shell commands -- `cd/pwd/mkdir/rmdir/cp/mv/rm/touch/whoami/date/free`, `>`/`>>`         | `vfs_create/rename/unlink` (exist; directories via `vfs_create`); `auth_get_current_user()` (TODO-06)                                                 |  [ ]   |
| 💎  |   2   | §2 Calculator -- button grid, two-operand, memory, division-by-zero, keyboard               | `kmath_sqrt/pow` (exist); `sin/cos/tan/log` in `include/libc/math.h` (exist); `gfx_fill_rounded_rect()` (exists)                                      |  [/]   |
| 💎  |   3   | §3 Image Viewer -- `image_load/scale`, fit/zoom/pan, prev/next folder, slideshow            | `image_load/scale` (exist); `vfs_readdir` (exists); `wallpaper_set()` (D08 T09 §3)                                                                    |  [/]   |
| ⭐  |   4   | §4 Screenshot enhancements -- Win+Shift+S region select, dim overlay, rubber-band, escape   | D08 T09 §5 screenshot module; `image_save_bmp()` (exists); `clipboard_set()` (TODO-01)                                                                |  [/]   |
| 💎  |   5   | §5 Archive Manager -- browse `.zip`, Extract All, Add Files, New archive                    | `zip_open/extract/list/add_file()` (TODO-04 §4); `dialog_file_open()` (D08 T06 §8)                                                                    |  [/]   |
| ⭐  |   6   | §6 Calendar -- month grid, events pane, Registry events, taskbar clock integration          | `time_now/to_datetime()` (08-graphics-ui/TODO-12 §1); Registry (exists); 08-graphics-ui/TODO-12 §3 taskbar clock                                      |  [/]   |
| 💎  |   7   | §7 Task Manager -- `cpu_ticks` in task_t, `sched_get_task_list()`, rolling charts, End Task | per-task `user_time_ns`/`kernel_time_ns` already in `struct task` (reuse before adding `cpu_ticks`); `pmm_get_free_frames()` (exists); Ctrl+Shift+Esc |  [ ]   |
| 💎  |   8   | §8 Device Manager -- `pci_get_all_devices()`, `CTRL_TREEVIEW` category tree, properties     | `pci_scan()` (exists); add `pci_get_all_devices()`; `CTRL_TREEVIEW` (D08 T06 §2)                                                                      |  [ ]   |
| 💎  |   9   | §9 System Info -- `CTRL_TABSTRIP` tabs, CPU/RAM/disk/PCI summary, export to text            | §7 + §8 data; `CTRL_TABSTRIP` (D08 T05 §6); `cpuid_get()->brand` (exists)                                                                             |  [/]   |

---

## 1. Shell Command Expansion

**Design:** n/a -- no desktop UI surface (developer tooling, data export, CLI or device plumbing)

`cd`, `pwd`, `mkdir`, `rmdir`, `cp`, `mv`, `rm`, `touch`, `whoami`, `date`, `free`. Output redirect `>` and `>>`. Stretch: tab completion, pipe `|`, `&&` chaining.

**Files:** `user/cmd.c` (extend; the shell is the user-mode `cmd.exe`, so directory and file commands need syscalls, not direct VFS calls)

> [!NOTE]
> **`cd <path>`**: `vfs_finddir_path(path)` → update `g_cwd`; handle relative paths (prepend `g_cwd`). **`pwd`**: print `g_cwd`. **`mkdir <path>`**: `vfs_mkdir(abs_path)`. **`rmdir <path>`**: `vfs_unlink(path)` if directory and empty (check `vfs_readdir()` returns NULL on first call). **`cp <src> <dst>`**: use `filemgr_copy_file()` pattern (PMM 64 KiB buffer). **`mv <src> <dst>`**: `vfs_rename(src, dst)`. **`rm <path>`**: `vfs_unlink(path)` (warn: no trash). **`touch <path>`**: `vfs_stat()` → if exists: update mtime (via `ixfs_set_mtime()` stub); else: `vfs_create(path, VFS_TYPE_FILE)`. **`whoami`**: `auth_get_current_user()->username` (TODO-06). **`date`**: `time_now()` → `time_format()`. **`free`**: `pmm_get_total_frames() * 4096` total; `pmm_get_free_frames() * 4096` free; print in KiB. **Output redirect**: in shell line parser: if token `>` found: open file for write (truncate); route `kprintf` fd to file node. `>>` appends. **Tab completion stretch**: on Tab: scan `g_cwd` via `vfs_readdir()`; filter by prefix; complete unique match or print ambiguous list. **Pipe `|` stretch**: split command at `|`; `pipe_create(fds)`; left side `>` to fds[1]; right side reads from fds[0]. **`&&` stretch**: run left; if exit code 0 → run right.

- [ ] `void cmd_cd(int argc, char **argv)` -- `vfs_finddir_path()`; update `g_cwd`; relative path resolution
- [ ] `void cmd_pwd(int argc, char **argv)` -- print `g_cwd`
- [ ] `void cmd_mkdir(int argc, char **argv)`, `cmd_rmdir`, `cmd_rm`, `cmd_touch`
- [ ] `void cmd_cp(int argc, char **argv)` -- PMM copy buffer; `vfs_read/write` loop
- [ ] `void cmd_mv(int argc, char **argv)` -- `vfs_rename()`
- [ ] `void cmd_whoami(...)`, `cmd_date(...)`, `cmd_free(...)`
- [ ] Output redirect: `>` and `>>` token detection in shell line parser; file-node redirect
- [ ] Stretch: Tab completion: `vfs_readdir()` prefix match; fill command buffer
- [ ] Stretch: `|` pipe: `pipe_create()`; fork two command evaluations; connect stdio
- [ ] Kernel diagnostic commands: `irq list` (print GSI/vector/handler/fire-count/shared/quarantined table from the `irq.c` registry) and `boot-timeline` (render `X:\Perf\boot-timeline.json`) -- deferred from `01-boot-platform/TODO-11-interrupt-timer-arch.md` §5, §9
- [ ] Register all in command dispatch table with usage strings
- [ ] Commit: `"shell: commands -- cd/pwd/mkdir/rmdir/cp/mv/rm/touch/whoami/date/free, >/>> redirect"`

## 2. Calculator

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-09-calculator.md` and the section body moved there -> XREF: 11-apps/TODO-09 §6 (item: "Reconcile Calculator spec with the earlier sections of this file" at line 174)

- [/] Superseded: moved to `11-apps/TODO-09-calculator.md` §6; do not build a second copy here

## 3. Image Viewer

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-11-photos-image-viewer.md` and the section body moved there -> XREF: 11-apps/TODO-11 §9 (item: "void imgview_open" at line 236)

- [/] Superseded: moved to `11-apps/TODO-11-photos-image-viewer.md` §9; do not build a second copy here

## 4. Screenshot Enhancements

**Design:** [`shell.md#snipping-toolbar`](../../docs/design/shell.md#snipping-toolbar), [`shell.md#toast-notifications`](../../docs/design/shell.md#toast-notifications)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-12-screenshot-archive.md` and the section body moved there -> XREF: 11-apps/TODO-12 §6 (item: "Settle screenshot ownership with the earlier sections of this file" at line 214)

- [/] Superseded: moved to `11-apps/TODO-12-screenshot-archive.md` §6; do not build a second copy here

## 5. Archive Manager

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-12-screenshot-archive.md` and the section body moved there -> XREF: 11-apps/TODO-12 §7 (item: "Settle archive manager ownership with the earlier sections of this fil" at line 239)

- [/] Superseded: moved to `11-apps/TODO-12-screenshot-archive.md` §7; do not build a second copy here

## 6. Calendar App

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-13-calendar-utilities.md` and the section body moved there -> XREF: 11-apps/TODO-13 §8 (item: "void calendar_open" at line 264)

- [/] Superseded: moved to `11-apps/TODO-13-calendar-utilities.md` §8; do not build a second copy here

## 7. Task Manager

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

Ctrl+Shift+Esc hotkey. Two tabs: **Processes** (Name/CPU%/RAM/PID/Status, End Task). **Performance** (rolling CPU line chart, RAM bar, Network/Disk KB/s). Status bar process count + CPU% + RAM. Requires `cpu_ticks` in `struct task`.

**Files:** `src/apps/taskmgr/taskmgr.c` (new), `include/kernel/sched/task.h` (extend -- add `cpu_ticks`)

> [!NOTE]
> **`cpu_ticks` addition**: add `uint64_t cpu_ticks` field to `struct task` -- incremented every scheduler tick by the PIT handler when that task is running. `sched_get_task_list(proc_info_t *out, int max)`: new function in `task.c`; iterates global task table; fills `proc_info_t { pid, name[32], state, cpu_ticks_snapshot, ram_bytes }`. **CPU%**: `taskmgr_refresh()` runs every second (via `sched_task_add("taskmgr_refresh", ..., 1, 1)`): capture `cpu_ticks` snapshots; compute `delta_ticks / total_ticks * 100` per process vs previous snapshot; store in `g_proc_cpu[]`. **RAM per process**: not yet tracked; stub `0` -- future: track per-process heap allocations via PMM tags. **Rolling chart**: `uint8_t g_cpu_history[60]` ring buffer; one sample per second; render as polyline. **Performance tab content**: CPU chart (60 s × 60 px high, polyline); RAM: `(total - free) / total * bar_w` filled bar with "X MB / Y MB" label; Network: `net_get_stats(&rx, &tx)` (new helper needed in `net.c`); Disk: `blkdev_get_stats(&read_kb, &write_kb)` (new helper needed in `blkdev.c`). **End Task**: select row → button → `signal_send(pid, SIGKILL)` → `taskmgr_refresh()`.

- [ ] Add `uint64_t cpu_ticks` to `struct task` in `task.h`; increment in PIT interrupt handler
- [ ] `typedef struct { uint32_t pid; char name[32]; uint32_t state; uint64_t cpu_ticks; uint64_t ram_bytes; } proc_info_t;`
- [ ] `int sched_get_task_list(proc_info_t *out, int max)` -- enumerate global task list
- [ ] `void taskmgr_open(void)` -- `wm_create_window()`; `CTRL_TABSTRIP` (2 tabs)
- [ ] `void taskmgr_refresh(void)` -- `sched_get_task_list()`; compute CPU% delta; update listview; schedule next at 1 s
- [ ] Processes tab: `CTRL_LISTVIEW` Name/CPU%/RAM/PID/Status; sortable; selected row + "End Task" button
- [ ] "End Task": `signal_send(selected_pid, SIGKILL)` → refresh
- [ ] Performance tab: CPU polyline from `g_cpu_history[60]`; RAM filled bar; Net/Disk counters
- [ ] `int net_get_stats(uint64_t *rx_bytes, uint64_t *tx_bytes)` stub in `net.c`
  - Per-process network columns come from socket accounting -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md` §20 (Per-Process Network I/O Accounting)
- [ ] `int blkdev_get_stats(uint64_t *read_kb, uint64_t *write_kb)` stub in `blkdev.c`
- [ ] Ctrl+Shift+Esc global hotkey
- [ ] Commit: `"taskmgr: task manager -- cpu_ticks, sched_get_task_list, process list, CPU chart, End Task"`

## 8. Device Manager

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

`CTRL_TREEVIEW` with PCI device categories + device children. Status ✅/⚠/❌. Properties pane (vendor/device/IRQ/BAR/driver). "Update Driver" stub. Accessible from Control Panel.

**Files:** `src/apps/devmgr/devmgr.c` (new), `include/kernel/drivers/pci.h` (extend -- add `pci_get_all_devices()`)

> [!NOTE]
> `pci_get_all_devices(struct pci_device *out, int *count)`: new function in `pci.c`; copies global `g_pci_devices[]` array. Categories: "Display adapters" (vendor class 03xx), "Network adapters" (02xx), "Storage controllers" (01xx), "Input devices" (09xx), "System" (all others). **Device node**: show `"{vendor_name} {device_name}"` (lookup from embedded PCI ID table stub -- use `"{vendor_id:04X} {device_id:04X}"` as fallback). Status icons: if driver registered (`driver_name != NULL`): ✅; if no driver: ⚠; if `TASK_STATE_DEAD` driver task: ❌. **Properties pane** (right panel, 280 px): device name, PCI address `{bus:02X}:{slot:02X}.{func}`, vendor ID, device ID, IRQ, BAR0–3 ranges, driver name ("No driver" if none). **Driver registry**: `HKLM\SYSTEM\Drivers\{vendor_id}_{device_id}\DriverName` -- read at `devmgr_refresh()`. "Update Driver" → `dialog_file_open("Driver files (*.ko)\0*.ko\0")` → stub message "Driver update not yet supported". Opened from Control Panel `sysdm.cpl` "Device Manager" button.

- [ ] `void pci_get_all_devices(struct pci_device *out, int *count)` in `pci.c/h`
- [ ] `void devmgr_open(void)` -- `wm_create_window(80, 50, 800, 560, "Device Manager", ...)`
- [ ] `void devmgr_build_tree(void)` -- `pci_get_all_devices()`; classify by class code; populate `CTRL_TREEVIEW`
- [ ] `void devmgr_render_properties(s, x, y, device_idx)` -- PCI address + IDs + IRQ + BARs + driver name
- [ ] Status icon: ✅ if driver, ⚠ if no driver, ❌ if error state
- [ ] `HKLM\SYSTEM\Drivers\*` driver registry lookup per device
- [ ] "Update Driver" → stub message dialog
- [ ] Control Panel `sysdm.cpl` "Device Manager" button → `devmgr_open()`
- [ ] Commit: `"devmgr: device manager -- pci_get_all_devices, CTRL_TREEVIEW categories, properties pane, driver status"`

## 9. System Info

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-13-calendar-utilities.md` and the section body moved there -> XREF: 11-apps/TODO-13 §9 (item: "src/apps/control/applets/msinfo32.c -- CPlApplet_t registered in c" at line 289)

- [/] Superseded: moved to `11-apps/TODO-13-calendar-utilities.md` §9; do not build a second copy here

---

## OS Comparison


| ⭐  | Feature           | 🪟 Win11                                                                 | 🐧 Linux                                                      | 🚀 Impossible OS                                                           |
| --- | ----------------- | ------------------------------------------------------------------------ | ------------------------------------------------------------- | -------------------------------------------------------------------------- |
| 💎  | Task Manager      | ✅ Task Manager: CPU/memory/disk/network charts; Details                 | ✅ `gnome-system-monitor`; `htop`; `ps`; `top`; process       | ⬜ §1 -- `cpu_ticks` in task_t; rolling 60-s                               |
| 💎  | Device Manager    | ✅ Device Manager: full PnP tree;                                        | ✅ `lspci`, `lshw`; GNOME `gnome-device-manager`; `hwinfo`;   | ⬜ §2 -- `pci_get_all_devices()`; `CTRL_TREEVIEW` categories; ✅⚠❌ driver |
| 💎  | Calculator        | ✅ Calculator: Standard/Scientific/Programmer/Graphing/Converter modes   | ✅ `gnome-calculator`: Standard/Scientific/Financial; `kcalc` | ⬜ §2 -- Standard mode complete; Scientific (kmath_sin/cos/tan/log)        |
| 💎  | Shell commands    | ✅ CMD: built-in commands + PowerShell;                                  | ` pipe; tab completion                                        | ✅ bash/zsh: full POSIX; redirect; pipe                                    |
| 💎  | Image Viewer      | ✅ Photos app: zoom/pan; slideshow; folder                               | ✅ Eog (GNOME): zoom/pan/rotate; folder strip;                | ⬜ §3 -- `image_load/scale`; fit+zoom+pan; folder array; scheduler         |
| ⭐  | Screenshot region | ✅ Snipping Tool: Win+Shift+S; free-form/rect/window/fullscreen; markup; | ✅ Flameshot; GNOME Screenshot: region select;                | ⬜ §4 -- `⭐` in-kernel compositor overlay (no                             |
| 💎  | Archive Manager   | ✅ File Explorer ZIP support; `Compress-Archive`;                        | ✅ File Roller (GNOME); Ark (KDE);                            | ⬜ §5 `zip_open/list/extract/add_file()` (TODO-04); `icon_for_extension()` |
| 💎  | Calendar          | ✅ Calendar app; Outlook integration; Exchange                           | ✅ GNOME Calendar; KOrganizer; iCal format;                   | ⬜ §6 -- Registry-backed events; Zeller formula; taskbar                   |
| 💎  | System Info       | ✅ `msinfo32.exe`: comprehensive; WMI queries; network                   | ✅ `inxi`; `lshw`; `neofetch`; no single                      | ⬜ §9 -- `msinfo32.cpl` 3-tab display; `pci_get_all_devices()`; export     |

> **After §1–§9:** Impossible OS is self-sufficient -- every essential utility exists. The `⭐` differentiators: the screenshot region select runs entirely in the kernel compositor layer (single dim overlay + rubber-band capture, no GPU delegation); and Task Manager CPU% is computed from `cpu_ticks` incremented directly in the PIT interrupt handler (lowest overhead possible, no sampling daemon).

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `mkdir C:\test` → dir created; `touch C:\test\a.txt` → file exists; `cp C:\test\a.txt C:\test\b.txt` → b.txt present; `rm C:\test\b.txt` → removed; `free` → shows total/free RAM in KiB
- [ ] Calculator: type `5 × 3 =` → display shows `15`; `√x` → `3.872…`; division by 0 → "Error"; MC/MS/MR cycle works
- [ ] Open JPEG → Image Viewer displays fit to window; mouse wheel → zooms; arrow key → next image in folder
- [ ] Win+Shift+S → dim overlay; drag rubber-band → region highlighted; release → file saved to Pictures; toast shown
- [ ] Open `.zip` → Archive Manager lists contents; "Extract All" → files extracted to chosen folder
- [ ] Calendar opens → correct month grid; click a day → events pane; "+ Add Event" → fills form → appears in pane next open
- [ ] Ctrl+Shift+Esc → Task Manager; processes listed; sort by CPU% (column click); select a process + "End Task" → process removed from list
- [ ] Device Manager: PCI devices in category tree; click device → properties pane shows vendor/device IDs + BAR ranges
- [ ] `msinfo32.cpl` from Control Panel → System Summary shows CPU brand + RAM; Components lists PCI devices; Export → `.txt` file written
- [ ] Commit: `"utilities: task manager, device manager, calculator, image viewer, screenshot, archiver, calendar, sysinfo -- all complete"`
