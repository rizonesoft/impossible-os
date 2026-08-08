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
> **Already exists**: `struct task { pid, state, name, parent_pid, num_threads }` + `task_get_by_pid()` + `SYS_GETPROCS=10` in `task.h/syscall.h`. `pci_scan()` + `struct pci_device { vendor_id, device_id, ... }` + `pci_find_device()` in `pci.h` -- but no `pci_get_all_devices()` accessor. `image_load/scale/save_bmp()` in `image.h`. `kmath_sqrt/pow/fabs/floor` in `kmath.h` -- but no `kmath_sin/cos/tan/log`. `pmm_get_total_frames()`, `cpuid_get()->brand`, `acpi_get_cpu_count()`. `zip_create/extract/list()` (TODO-04 §6 forward dep). `CTRL_TREEVIEW/TABSTRIP` (TODO-05 forward deps). `search_query_scoped()` (TODO-05). `context_menu_show()` (TODO-07). **Missing**: `cpu_ticks` field in `struct task` (add for CPU%); `sched_get_task_list(out, max)` helper; `pci_get_all_devices(out, max)` helper; `blkdev_get_stats()` / `net_get_stats()` for performance charts; `kmath_sin/cos/tan/log` for scientific calculator. **New functions needed in §1**: add `uint64_t cpu_ticks` to `struct task`; add `sched_get_task_list(proc_info_t *out, int max)`. Complete sections in order: shell commands → calculator → image viewer → screenshot → archive manager → calendar → task manager → device manager → system info.

## Inputs

- `include/kernel/sched/task.h` -- `struct task`, `task_get_by_pid()`, `SYS_GETPROCS=10` -- §1 process list; add `cpu_ticks` for CPU%
- `include/kernel/drivers/pci.h` -- `pci_scan()`, `struct pci_device`, `pci_find_device()` -- §2 device tree; add `pci_get_all_devices()`
- `include/kernel/image.h` -- `image_load/scale/save_bmp()` -- §5 image viewer, §6 screenshot
- `include/kernel/kmath.h` -- `kmath_sqrt/pow/fabs` -- §3 calculator; add `kmath_sin/cos/tan/log`
- `include/kernel/mm/pmm.h` -- `pmm_get_total_frames()`, `pmm_get_free_frames()` -- §1 RAM bar, §9 system info
- `include/kernel/cpuid.h` -- `cpuid_get()->brand/vendor/model` -- §9 system info
- `include/kernel/acpi.h` -- `acpi_get_cpu_count()` -- §9 system info
- `include/kernel/timer.h` -- `uptime()`, `system_get_ticks()` -- §1 CPU% rolling sample
- `include/registry.h` -- `HKCU\...\Calendar\Events\*`, `HKLM\...\Uninstall\*` -- §8 calendar events, §9 software env
- `include/kernel/fs/vfs.h` -- `vfs_readdir`, `vfs_stat` -- §5 next/prev image in folder, §8 scan
- `include/desktop/controls.h` (TODO-05) -- `CTRL_TREEVIEW`, `CTRL_TABSTRIP`, `dialog_file_open()` -- §4 device tree, §7 archive, §9 tabs
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

| ⭐  | Order | Deliverable                                                                               | Depends On                                                                                 | Status |
| --- | :---: | ----------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ | :----: |
| 💎  |   1   | §4 Shell commands -- `cd/pwd/mkdir/rmdir/cp/mv/rm/touch/whoami/date/free`, `>`/`>>`       | `vfs_create/rename/unlink/mkdir` (exist); `auth_get_current_user()` (TODO-06)             |  [ ]   |
| 💎  |   2   | §3 Calculator -- button grid, two-operand, memory, division-by-zero, keyboard              | `kmath_sqrt/pow` (exist); `gfx_fill_rounded_rect()` (exists or TODO-04 WM)               |  [ ]   |
| 💎  |   3   | §5 Image Viewer -- `image_load/scale`, fit/zoom/pan, prev/next folder, slideshow           | `image_load/scale` (exist); `vfs_readdir` (exists); `wallpaper_set()` (TODO-07)          |  [ ]   |
| ⭐  |   4   | §6 Screenshot enhancements -- Win+Shift+S region select, dim overlay, rubber-band, escape  | TODO-07 §5 screenshot module; `image_save_bmp()` (exists); `clipboard_set()` (TODO-01)   |  [ ]   |
| 💎  |   5   | §7 Archive Manager -- browse `.zip`, Extract All, Add Files, New archive                   | `zip_open/extract/list/add_file()` (TODO-04 §6); `dialog_file_open()` (TODO-05)         |  [ ]   |
| ⭐  |   6   | §8 Calendar -- month grid, events pane, Registry events, taskbar clock integration         | `time_now/to_datetime()` (TODO-10); Registry (exists); TODO-10 §3 taskbar clock flyout   |  [ ]   |
| 💎  |   7   | §1 Task Manager -- `cpu_ticks` in task_t, `sched_get_task_list()`, rolling charts, End Task | add `cpu_ticks` to `struct task`; `pmm_get_free_frames()` (exists); Ctrl+Shift+Esc      |  [ ]   |
| 💎  |   8   | §2 Device Manager -- `pci_get_all_devices()`, `CTRL_TREEVIEW` category tree, properties    | `pci_scan()` (exists); add `pci_get_all_devices()`; `CTRL_TREEVIEW` (TODO-05)           |  [ ]   |
| 💎  |   9   | §9 System Info -- `CTRL_TABSTRIP` tabs, CPU/RAM/disk/PCI summary, export to text           | §1 + §2 data; `CTRL_TABSTRIP` (TODO-05); `cpuid_get()->brand` (exists)                  |  [ ]   |

---

## 1. Shell Command Expansion `[Sonnet]`

`cd`, `pwd`, `mkdir`, `rmdir`, `cp`, `mv`, `rm`, `touch`, `whoami`, `date`, `free`. Output redirect `>` and `>>`. Stretch: tab completion, pipe `|`, `&&` chaining.

**Files:** `src/shell/cmds.c` (extend)

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

## 2. Calculator `[Sonnet]`

Compact fixed window; 5×4 button grid; two-operand model; `gfx_fill_rounded_rect` buttons with hover/press; ops: +/−/×/÷/=/C/CE/⌫/±/1x/x²/√; keyboard numpad; Memory M+/−/R/C/S. Stretch: Scientific (sin/cos/tan/log/π), Programmer (hex/bin/oct/bitwise), history.

**Files:** `src/apps/calc/calc.c` (new), `include/apps/calc.h` (new)

> [!NOTE]
> Window: 320×480 px fixed (no resize). Display area: top 80 px -- right-aligned number (`ttf_draw_string(FONT_UI, 28px)`) + small operation preview above it. **Two-operand model**: `g_display` (current entry string), `g_operand` (double), `g_op` (char: +−×÷), `g_after_op` flag. `=` → compute `g_operand op parse(g_display)`; update display; set `g_after_op=1`. C → clear all; CE → clear display only; ⌫ → remove last char. `±` → negate display string; `1/x` → `1.0 / val`; `x²` → `val * val`; `√x` → `kmath_sqrt(val)`. Division by zero → `g_display = "Error"`. **Button grid**: 40×40 px per button; 4 px gap; row order: MC MR M+ M− MS / % CE C ⌫ / 1/x x² √x ÷ / 7 8 9 × / 4 5 6 − / 1 2 3 + / ± 0 . =. Accent color for operators; white for numbers; light gray for memory. **Keyboard**: numpad 0–9 + `.` + operators; Enter = `=`; Backspace = ⌫; Delete = CE; Escape = C. **Scientific stretch**: `kmath_sin/cos/tan/log` (add to kmath.h); buttons: sin/cos/tan/log/10^x/1/x/x^y/π/e/n!/mod. **Programmer stretch**: hex/bin/oct base picker; bit-width dropdown (8/16/32/64); AND/OR/XOR/NOT/SHL/SHR buttons; display shows number in selected base.

- [ ] `typedef struct { char display[32]; double operand; char op; int after_op; double memory; } calc_t;`
- [ ] `void calc_open(void)` -- `wm_create_window(200, 150, 320, 480, "Calculator", WM_FLAG_NO_RESIZE)`
- [ ] `void calc_render(calc_t *c, gfx_surface_t *s)` -- display area + 5×4 button grid
- [ ] `gfx_fill_rounded_rect(s, x, y, w, h, r, color)` -- add if missing (or use `gfx_fill_rect` with clipped corners)
- [ ] Button press: digit → append to `g_display`; operator → store `g_operand` + `g_op`; `=` → evaluate
- [ ] Division by zero, overflow → `"Error"` display; next digit clears
- [ ] Memory ops: MS → `memory = val`; MR → display `memory`; M+ → `memory += val`; M− → `memory -= val`; MC → `memory = 0`
- [ ] Keyboard handler in WM `WM_KEYDOWN`: map VK_NUMPAD0–9 + operators + Enter/Backspace/Delete/Escape
- [ ] Stretch: `double kmath_sin(double x)`, `kmath_cos`, `kmath_tan`, `kmath_log` added to `kmath.h`
- [ ] Stretch: Scientific layout toggle (expand window to 480 px); Programmer base picker
- [ ] Commit: `"calc: calculator -- 5×4 grid, two-operand model, memory, keyboard, rounded-rect buttons"`

## 3. Image Viewer `[Sonnet]`

`image_load()` + fit to window preserving aspect ratio. Mouse wheel / Ctrl+/- zoom. Click+drag pan when zoomed. Prev/Next in same folder. Toolbar. Status bar. Slideshow. Set as wallpaper.

**Files:** `src/apps/imgview/imgview.c` (new), `include/apps/imgview.h` (new)

> [!NOTE]
> Window: initial 800×600 px (resizable). Load: `image_load(&g_img, path)` → `image_scale(&g_scaled, &g_img, win_w, win_h, FIT_ASPECT)` → `gfx_blit(surface, scaled_pixels, 0, 0)`. **Zoom**: `g_zoom` float (1.0 = fit); mouse wheel → `g_zoom *= 1.1` or `/ 1.1`; `Ctrl++/−`; `100%` button → `g_zoom=1.0`; `Fit` button → `g_zoom = fit_factor`. At zoom > fit: show pan offsets; click+drag → update `g_pan_x/y`. **Prev/Next folder**: on open: `vfs_readdir(parent_dir)` → filter for `.jpg/.jpeg/.png/.bmp/.gif` extensions → build `g_folder_images[]` array; `g_folder_idx` = current file index; ← → buttons + Left/Right arrow keys cycle through. **Toolbar** (top 32 px): ← Prev, → Next, Zoom In, Zoom Out, 100%, Fit, Slideshow, Set as Wallpaper, × Close. **Status bar** (bottom 20 px): `"{filename} -- {W}×{H} -- {size KB} -- {idx} of {total}"`. **Slideshow**: timer via `sched_task_add("imgview_slide", imgview_advance, slide_interval_s, 1)` (TODO-04); slide_interval configurable 3–5 s. **Set as Wallpaper**: `wallpaper_set(g_current_path, WALLPAPER_FIT)` (TODO-07 forward dep). **File association**: `.jpg/.jpeg/.png/.bmp` → `imgview.exe` registered in `file_assoc` defaults (TODO-02).

- [ ] `void imgview_open(const char *path)` -- `image_load()`; `wm_create_window()`; build folder list; render
- [ ] `void imgview_render(gfx_surface_t *s, int w, int h)` -- scale to fit/zoom; blit; pan offsets
- [ ] Fit calculation: `scale = min((float)win_w/img_w, (float)win_h/img_h)`
- [ ] Zoom: mouse wheel → `g_zoom *= 1.1`; clamp to [0.1, 20.0]; recalc scale + render
- [ ] Pan: `WM_MOUSE_MOVE` with button → `g_pan_x/y += delta`; clamp to image bounds
- [ ] Folder scan: `vfs_readdir(parent)` → extension filter → sorted name array
- [ ] Prev/Next: `g_folder_idx = (g_folder_idx ± 1) % g_folder_count`; `image_load()` new path
- [ ] Toolbar + status bar rendering
- [ ] Slideshow: `sched_task_add()` 1-shot with `slide_interval_s`; each tick → Next
- [ ] "Set as Wallpaper" → `wallpaper_set(path, WALLPAPER_FIT)`
- [ ] Commit: `"imgview: image viewer -- image_load/scale, fit/zoom/pan, prev/next folder, slideshow, wallpaper"`

## 4. Screenshot Enhancements `[Sonnet]`

Win+Shift+S region select mode: full-screen dim overlay (50% black), click+drag rubber-band selection, release → capture region. 3-second countdown overlay before capture. Escape cancels. Extends TODO-07 §5 PrintScreen module.

**Files:** `src/desktop/screenshot.c` (extend), `include/desktop/screenshot.h` (extend)

> [!NOTE]
> → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §5` -- the existing full-screen PrintScreen capture lives there; §4 here adds Win+Shift+S region mode as an extension. **Region select state machine**: `SCREENSHOT_IDLE → SCREENSHOT_DIM → SCREENSHOT_SELECTING → SCREENSHOT_CAPTURED`. Win+Shift+S → `screenshot_region_start()`: create full-screen overlay window (`z_order=30000`, `WM_FLAG_FULLSCREEN`); fill with 50% alpha black via `gfx_fill_rect(s, 0, 0, sw, sh, 0x80000000)`. **Rubber-band**: `WM_MOUSE_DOWN` → `sel_start_x/y`; `WM_MOUSE_MOVE` → `sel_end_x/y`; render clear rect (XOR-clear or outline) within dim. Release → `screenshot_capture_region(x1, y1, w, h)`: copy compositor back-buffer pixels for that rect → `image_save_bmp()` to `C:\Users\Default\Pictures\Screenshot_{timestamp}.bmp` + `clipboard_set(CLIP_BITMAP, ...)` (TODO-01 multi-format). **3-second countdown**: optional pre-delay for full-screen PrintScreen: overlay countdown "3…2…1" in large centered text; each second via PIT. **Escape** → `screenshot_region_cancel()`: close overlay; restore state to IDLE. Show toast notification after capture: "Screenshot saved to Pictures".

- [ ] `screenshot_region_start()` -- create dim overlay window; transition to SELECTING state
- [ ] `WM_MOUSE_DOWN/MOVE/UP` in overlay → rubber-band rect (outline on dim background)
- [ ] `screenshot_capture_region(x, y, w, h)` -- copy compositor backbuf rect; `image_save_bmp()`; `clipboard_set()`
- [ ] Countdown overlay for full-screen: `sched_task_add()` 1 s ticks; draw "3/2/1" centred; then capture
- [ ] Escape handler → `screenshot_region_cancel()` -- destroy overlay
- [ ] Toast after capture: `notify_send("Screenshot", "Saved to Pictures", ICON_SCREENSHOT, ...)` (TODO-09)
- [ ] Win+Shift+S global hotkey registration
- [ ] Commit: `"screenshot: region select -- Win+Shift+S, dim overlay, rubber-band, countdown, clipboard+file save"`

## 5. Archive Manager `[Sonnet]`

`src/apps/archiver/archiver.c`; open `.zip` via file association; `CTRL_LISTVIEW` (Name/Size/Compressed/Modified); `icon_for_extension()` per entry; Extract All, Add Files, New archive; status bar.

**Files:** `src/apps/archiver/archiver.c` (new), `include/apps/archiver.h` (new)

> [!NOTE]
> Window: 700×500 px. Open archive: `zip_open(path)` → `zip_list(archive, entries, 512)` → populate `CTRL_LISTVIEW` with 4 columns (Name 240/Size 70/Compressed 90/Modified 120). Icon per entry: `icon_for_extension(ext)`. Double-click entry: extract single file to temp dir + `file_assoc_open()`. **Toolbar** (32 px): "Extract All" → `dialog_file_open(dir_mode)` → `zip_extract(archive, dst_dir)` → `filemgr_navigate(dst_dir)`; "Add Files" → `dialog_file_open(multi)` → for each: `zip_add_file(archive, path, basename(path))`; "New" → `dialog_file_save("New Archive", "*.zip")` → `zip_create(path)`. Keyboard: Delete on selected entry → `zip_remove_entry()` stub (note: miniz doesn't support single-entry removal -- must re-pack entire archive). **Status bar**: "{N} items -- {total_size} bytes -- {compressed_size} bytes ({ratio}% compression)". **File association**: `.zip` → archiver registered in TODO-02 `file_assoc` defaults.

- [ ] `void archiver_open(const char *path)` -- `zip_open()`; `zip_list()`; build `CTRL_LISTVIEW`
- [ ] `void archiver_refresh(void)` -- re-`zip_list()`; update listview + status bar
- [ ] Toolbar: Extract All → `dialog_file_open(dir)` → `zip_extract()`; Add Files → multi-select → `zip_add_file()` each; New → `dialog_file_save()` → `zip_create()`
- [ ] Double-click entry: `zip_extract_file()` to `C:\Temp\` → `file_assoc_open()`
- [ ] Status bar: entry count + total size + compressed size + ratio
- [ ] Entry delete: re-pack archive: extract all to temp → remove file → re-create archive from remaining files
- [ ] Register `.zip` in file associations (TODO-02 §1 default assocs)
- [ ] Commit: `"archiver: archive manager -- CTRL_LISTVIEW, Extract All/Add Files/New, status bar, zip API"`

## 6. Calendar App `[Sonnet]`

Month grid (7 cols Mon–Sun × 5–6 week rows). Accent today highlight. Prev/Next month. Click day → events pane. "+ Add Event" dialog (time, title, color). Registry events. Taskbar clock flyout integration.

**Files:** `src/apps/calendar/calendar.c` (new), `include/apps/calendar.h` (new)

> [!NOTE]
> Window: 700×500 px. Left panel (500 px): month grid; right panel (200 px): events for selected day. **Month grid**: `g_view_year/month`; compute `first_weekday_of_month` (Zeller's formula); draw 7-column headers Mon–Sun; fill grid cells with day number; current day → `gfx_fill_rect(accent_bg)`; selected day → accent outline. Prev/Next buttons: `g_view_month--/++` with year rollover. **Events pane**: "Wednesday, 15 March" header; list events for `g_selected_date`; each event: colored dot (event color) + time + title. **Add Event dialog** (modal, 380×260): `CTRL_TEXTBOX` title; `dialog_input` time picker (HH:MM); color preset swatches (8 colors); OK → write to Registry `HKCU\Software\Impossible\Calendar\Events\YYYY-MM-DD\{uid}\{Title,Time,Color}`. **Load events**: scan `HKCU\...\Events\{date}\*` subkeys. **Taskbar clock**: TODO-10 §3 calendar flyout (`WM_CLOCK_CLICKED`) opens this window. `calendar_open_at_date(year, month, day)` public API for taskbar integration.

- [ ] `void calendar_open(void)` -- `wm_create_window(100, 60, 700, 500, "Calendar", ...)`
- [ ] `void calendar_render_month(s, x, y, w, h)` -- Zeller first-weekday; fill grid; accent today/selected
- [ ] `void calendar_render_events(s, x, y, w, h, date)` -- scan `HKU\...\Events\YYYY-MM-DD\*`; list events
- [ ] Click handler: map pixel to grid cell → `g_selected_date`; refresh events pane
- [ ] Prev/Next month buttons: `g_view_month--/++`; year rollover; re-render
- [ ] "+ Add Event" dialog: title textbox + time + color picker swatches; OK → `RegSetValueEx()`
- [ ] `void calendar_open_at_date(int y, int m, int d)` -- public API for taskbar clock
- [ ] Delete event: right-click event → `context_menu_show()` → Delete → `RegDeleteKey()`
- [ ] Commit: `"calendar: month grid, events pane, Registry events, Add dialog, taskbar clock integration"`

## 7. Task Manager `[Sonnet]`

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
- [ ] `int blkdev_get_stats(uint64_t *read_kb, uint64_t *write_kb)` stub in `blkdev.c`
- [ ] Ctrl+Shift+Esc global hotkey
- [ ] Commit: `"taskmgr: task manager -- cpu_ticks, sched_get_task_list, process list, CPU chart, End Task"`

## 8. Device Manager `[Sonnet]`

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

## 9. System Info `[Sonnet]`

`msinfo32.cpl` -- `CTRL_TABSTRIP` tabs: System Summary (OS/CPU/RAM/disk), Components (PCI devices), Software Environment (drivers/processes/startup). Export to text file. From Control Panel.

**Files:** `src/apps/control/applets/msinfo32.c` (new)

> [!NOTE]
> Three tabs via `CTRL_TABSTRIP`. **System Summary**: OS Name "Impossible OS 1.0", Version "1.0", Build date, CPU (`cpuid_get()->brand`), Cores (`acpi_get_cpu_count()`), MHz (stub from `cpuid_get()->max_freq` if available), Total RAM (`pmm_get_total_frames() * 4 / 1024` MiB), Available RAM (`pmm_get_free_frames() * 4 / 1024`), BIOS (UEFI firmware info from `uefi_get_variable()`), Uptime (`uptime()` seconds). **Components**: reuse `pci_get_all_devices()` from §8; display as flat list: PCI addr, vendor/device IDs, class code, driver name. **Software Env**: loaded drivers = scan `HKLM\SYSTEM\Drivers\*`; Running processes = `sched_get_task_list()` count + names; Startup programs = scan `HKLM\SYSTEM\Boot\Run\*` + Startup folder `.lnk` count. **Export**: "Export…" button → `dialog_file_save("System Info*.txt")` → write all three tabs as plain text sections.

- [ ] `src/apps/control/applets/msinfo32.c` -- `CPlApplet_t` registered in control panel
- [ ] `CTRL_TABSTRIP` 3 tabs; tab switch renders appropriate content
- [ ] System Summary: info key-value rows (label + value); `ttf_draw_string()` two-column layout
- [ ] Components: `pci_get_all_devices()` → flat list with class code + driver name
- [ ] Software Env: driver count from Registry + `sched_get_task_list()` count + startup scan
- [ ] "Export…" → format all data as text → `vfs_write()`
- [ ] Register in `g_cpl_entries[]` as "System Information"; accessible from `sysdm.cpl` "More info" link
- [ ] Commit: `"msinfo32: system info -- CTRL_TABSTRIP tabs, CPU/RAM/PCI/drivers/processes, export to text"`

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
