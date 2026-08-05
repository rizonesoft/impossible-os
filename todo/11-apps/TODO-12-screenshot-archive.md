---
schema_version: 1
id: screenshot-archive
domain: 11-apps
status: active
title: "TODO-12 -- Screenshot Tool & Archive Manager"
---

# TODO-12 -- Screenshot Tool & Archive Manager

> **Goal:** Build `sniptool.exe` (screenshot + annotation) and `archiver.exe` (ZIP manager),
> plus global hotkey wiring for PrtSc/Alt+PrtSc/Win+Shift+S and ZIP shell-context integration.
> Clipboard, `image_save_png`, and miniz ZIP are all complete; this TODO delivers the app layer.

> [!IMPORTANT]
> **Source migration:** §3 (Screenshot Tool) and §4 (Archive Manager) from
> `todo-old/310-Core-Apps/TODO-370-Utility-Apps.md` are migrated here.
> **Do not delete that file** -- remaining sections are covered in TODO-13.
>
> **Scope overlap:** `09-desktop-shell/TODO-12-utilities.md §8` specifies the Win+Shift+S
> region capture flow and `§7` specifies Archive Manager as utility stubs. This TODO is the
> full app-layer companion -- implement here; add `→ XREF` from TODO-12-utilities §8+§2 to
> this file when implementing those sections.
>
> **Global hotkeys** use `struct hotkey_entry` + `hotkey_table[32]` from
> `08-graphics-ui/TODO-08-window-manager.md §5`.
> PrtSc = full screen, Alt+PrtSc = active window, Win+Shift+S = region select.
>
> **ZIP write API** (`zip_create`, `zip_add_file`, `zip_extract`, `zip_list`) defined in
> `09-desktop-shell/TODO-04-recycle-zip-scheduler.md §6` -- implement §6 of that TODO before
> building Archive Manager write operations.

---

## Inputs

- `include/kernel/drivers/framebuffer.h` -- `fb_get_backbuffer()`, `fb_get_width()`, `fb_get_height()` -- §1 full-screen capture
- `include/kernel/image.h` -- `image_save_png()`, `image_save_bmp()`, `image_load_mem()` -- §1 §2 §3 save
- `include/gfx.h` -- `gfx_fill_rect()`, `gfx_blit()`, `gfx_draw_rect()`, `gfx_surface_create()` -- §2 overlay, §3 annotate
- `include/desktop/wm.h` -- `wm_create_window()` (z_order=32767 for overlay), `wm_mark_dirty()`, `wm_get_focused()` -- §1 §2 §3
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_LISTVIEW`, `CTRL_SCROLLBAR_VERT`, `CTRL_STATUSBAR`, `CTRL_PROGRESSBAR` -- §3 §4
- `08-graphics-ui/TODO-08-window-manager.md §5` (→ XREF) -- `struct hotkey_entry`, `hotkey_table[]`, `MOD_WIN/MOD_ALT/MOD_SHIFT`, `KEY_PRINTSCREEN` -- §1 §2 global hotkeys
- `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §6` (→ XREF) -- `notify_send(title, body, icon_id, timeout_ms)` -- §1 §4 toast
- `09-desktop-shell/TODO-01-clipboard.md §1` (→ XREF) -- `clipboard_set(CLIP_IMAGE, &img)` -- §1 §2 copy to clipboard
- `08-graphics-ui/TODO-06-widget-dialogs.md §2` (→ XREF) -- `dialog_file_open()`, `dialog_file_save()` -- §2 §3 §5
- `include/registry.h` -- `reg_get_string`, `reg_set_string` -- §3 recent captures, §4 last extract path
- `include/desktop/file_assoc.h` (→ XREF `09-desktop-shell/TODO-02 §1`) -- `file_assoc_set()` -- §3
- `02-kernel-core/TODO-03-kernel-libraries.md §6` (→ XREF) -- `zip_open`, `zip_entry_count`, `zip_find`, `zip_read`, `zip_close` -- §6 §3 read
- `09-desktop-shell/TODO-04-recycle-zip-scheduler.md §6` (→ XREF) -- `zip_create`, `zip_add_file`, `zip_extract`, `zip_extract_file`, `zip_list` -- §6 §5 write
- `include/kernel/fs/vfs.h` -- `vfs_create`, `vfs_write`, `vfs_mkdir`, `vfs_stat` -- §1 §4 §5 file output
- `include/kernel/timer.h` -- `system_get_ticks()`, `time_now()` -- §1 timestamp filename, §3 delay

---

## Outcome

`sniptool.exe` captures full-screen, active-window, or rubber-band region screenshots and saves timestamped PNGs to `C:\Users\{name}\Pictures\Screenshots\`. Global hotkeys (PrtSc / Alt+PrtSc / Win+Shift+S) work without launching the GUI. `archiver.exe` opens `.zip` files, lists contents, extracts, and creates new archives. Right-click shell verbs "Extract Here" and "Send to → Compressed folder" integrate ZIP into the file manager context menu.

---

## Implementation Order

| Step | Section                         | 💎/⭐ | Dependency                               |
| ---- | ------------------------------- | --- | ---------------------------------------- |
| 1    | Full + Window Capture + Hotkeys | 💎   | `fb_get_backbuffer`, `image_save_png`, `notify_send`, `hotkey_table` |
| 2    | Region Select Overlay           | ⭐   | §1 capture pipeline, `wm_create_window` z_order overlay |
| 3    | Snipping Tool UI                | 💎   | §1 + §2 capture complete, `CTRL_*` widgets |
| 4    | Archive Manager                 | 💎   | `zip_open/create/add_file/extract/list` APIs (D12T01 §4) |
| 5    | ZIP Shell Integration           | 💎   | §4 stable, `file_assoc_set`, context menu verbs |

---

## 1. Full + Window Capture + Hotkeys `[Sonnet]`

**Source file:** `src/apps/snip/screenshot.c`; header `include/apps/snip/screenshot.h`

- [ ] **`screenshot_full()`**:
  - [ ] `uint32_t *fb = fb_get_backbuffer()` → copy `fb_w * fb_h * 4` bytes into `image_t g_capture` (allocated via `pmm_alloc_contiguous`)
  - [ ] Build path: `"C:\\Users\\{name}\\Pictures\\Screenshots\\Screenshot_{YYYY-MM-DD_HH-MM-SS}.png"` using `time_now()` fields
  - [ ] `vfs_mkdir("C:\\Users\\{name}\\Pictures\\Screenshots")` (no-op if exists)
  - [ ] `image_save_png(&g_capture, path)` → save
  - [ ] `clipboard_set(CLIP_IMAGE, &g_capture)` → copy to clipboard simultaneously
  - [ ] `notify_send("Screenshot saved", path, ICON_SCREENSHOT, 3000)` → toast
- [ ] **`screenshot_window(wm_handle_t hwnd)`**:
  - [ ] Query window bounds from `wm_get_window_rect(hwnd, &x, &y, &w, &h)`
  - [ ] Allocate `image_t` `w × h`; copy rect from backbuffer: `memcpy` row by row starting at `fb + y * fb_w + x`
  - [ ] Same save + clipboard + toast flow as `screenshot_full()`
- [ ] **`screenshot_region(int x, int y, int w, int h)`**: same as window but with caller-supplied rect
- [ ] **Global hotkey registration** (called from `snip_init()` at desktop startup):
  - [ ] `hotkey_table[N] = { .modifiers=0, .scancode=KEY_PRINTSCREEN, .handler=screenshot_full }`
  - [ ] `hotkey_table[N+1] = { .modifiers=MOD_ALT, .scancode=KEY_PRINTSCREEN, .handler=screenshot_active_window }` -- `screenshot_active_window()` = `screenshot_window(wm_get_focused())`
  - [ ] `hotkey_table[N+2] = { .modifiers=MOD_WIN|MOD_SHIFT, .scancode=KEY_S, .handler=snip_region_start }` -- launches region select (§2)
- [ ] **`screenshot_active_window()`**: wrapper that calls `screenshot_window(wm_get_focused())`

---

## 2. Region Select Overlay `[Opus]`

> Novel full-screen WM layer with per-pixel clear-region compositing inside a semi-transparent
> dim overlay -- no prior Impossible OS precedent for an interactive screen-capture overlay.

**Source file:** `src/apps/snip/region_select.c`

- [ ] **Overlay window**: `wm_create_window("snip_overlay", 0, 0, fb_w, fb_h, WM_NO_BORDER | WM_NO_TASKBAR)` with `z_order=32767` (above all windows); cursor set to crosshair
- [ ] **Dim layer**: `gfx_fill_rect(overlay_surf, 0, 0, fb_w, fb_h, 0x80000000)` (50% alpha black) -- composited by WM alpha-blend pass
- [ ] **Rubber-band selection**:
  - [ ] `mouse_down` → record `g_sel_x0, g_sel_y0`; set `g_dragging = true`
  - [ ] `mouse_move` while dragging → compute `sel_x = min(x0, cx)`, `sel_y = min(y0, cy)`, `sel_w`, `sel_h`
  - [ ] Each frame: re-draw dim layer; **blit the unmodified backbuffer region** onto the overlay surface at `(sel_x, sel_y)` to make selected area appear clear: `gfx_blit(overlay_surf, sel_x, sel_y, fb + sel_y*fb_w + sel_x, sel_w, sel_h)`
  - [ ] Draw 1 px white border around selection: `gfx_draw_rect(overlay_surf, sel_x, sel_y, sel_w, sel_h, 0xFFFFFFFF)`
  - [ ] Corner handles: 6×6 px white filled squares at corners + edge midpoints
- [ ] **Mouse release** → `screenshot_region(sel_x, sel_y, sel_w, sel_h)` (§1 capture); destroy overlay; show post-capture toolbar (§2 below)
- [ ] **Escape key** → destroy overlay; cancel with no capture
- [ ] **Post-capture toolbar** (small floating panel near top of screen, auto-dismiss after 5 s of no interaction):
  - [ ] `[📋 Copy]` -- clipboard already set; flash confirm
  - [ ] `[💾 Save]` -- already saved; show path in tooltip
  - [ ] `[🖼 Edit in Photos]` -- launch `photos.exe {path}` with edit mode flag
  - [ ] `[✏ Annotate]` -- launch `sniptool.exe` with the captured image pre-loaded for annotation (§3)

---

## 3. Snipping Tool UI `[Sonnet]`

**Source file:** `src/apps/snip/sniptool.c`; binary: `sniptool.exe`

- [ ] **Main window** `wm_create_window("Snipping Tool", 400, 500, WM_FIXED)`:
  - [ ] **Mode selector** (radio buttons): `○ Rectangular` `○ Window` `○ Full Screen`; default Rectangular
  - [ ] **Delay selector** (dropdown / radio): `0 s | 1 s | 3 s | 5 s`; default 0 s
  - [ ] `[New]` button → if delay > 0: countdown OSD overlay (`"3… 2… 1…"` centered, 72 pt bold, semi-transparent); then perform capture per selected mode
  - [ ] Full screen mode → `screenshot_full()` directly; Window mode → `screenshot_window(wm_get_focused())` (minimize Snip Tool first); Rectangular → `snip_region_start()` (§2)
- [ ] **Recent captures grid** (home screen, below mode/delay bar):
  - [ ] 3-column thumbnail grid; load from `HKCU\Software\Impossible\SnipTool\RecentCaptures\{0..9}` paths; `image_scale(&thumb, &img, 120, 80, IMAGE_FIT_FIT)`; click → open in annotation view
- [ ] **Annotation view** (when capture is available):
  - [ ] Canvas showing captured image; toolbar: `[✏ Pen]` `[🖊 Highlighter]` `[✂ Crop]` `[🗑 Eraser]` `[↩ Undo]`
  - [ ] **Pen**: mouse drag → draw 2 px colored stroke; color picker via `dialog_color()`
  - [ ] **Highlighter**: semi-transparent (alpha 0x80) 12 px yellow rect stroke
  - [ ] **Crop**: rubber-band (reuse region-select logic §2 but in-window); on Confirm: `image_crop` (pixel copy as in Photos §7)
  - [ ] Undo: single-level (keep one `image_t g_undo` copy; `Ctrl+Z` swaps)
  - [ ] `[Copy]` → `clipboard_set(CLIP_IMAGE, &annotated)`; `[Save]` → `image_save_png` to original path (overwrite) or `dialog_file_save` for new path
- [ ] **Registry persist**: on each save, prepend path to `HKCU\Software\Impossible\SnipTool\RecentCaptures\{0..9}` (shift existing entries)

---

## 4. Archive Manager `[Sonnet]`

**Source file:** `src/apps/archiver/archiver.c`; binary: `archiver.exe`

- [ ] **Main window** `wm_create_window("Archive Manager -- {filename}", 700, 500, WM_RESIZABLE)`:
  - [ ] Toolbar: `[Extract All]` `[Add Files]` `[New Archive]` `[Delete Selected]` + path bar showing current virtual path inside archive
  - [ ] `CTRL_LISTVIEW` file list -- columns: Name (250 px), Type (80 px), Size (80 px), Compressed (90 px), Modified (130 px); icon from `icon_get_for_ext(entry_name_ext)`
  - [ ] `CTRL_STATUSBAR`: `"{n} items  |  {total_compressed} compressed  |  {total_original} original"`
- [ ] **Open archive**:
  - [ ] `vfs_read(path)` → buffer; `zip_open(&g_zip, buf, size)` → `zip_entry_count(&g_zip)` → `zip_list(&g_zip, entries, max)` → populate `CTRL_LISTVIEW`
  - [ ] Show size, compressed size, modified timestamp from `zip_entry_t`
  - [ ] Folder navigation: double-click folder entry → filter listview to `{prefix}/` entries; back button → up one level; path bar shows `archive.zip\folder\subfolder\`
- [ ] **`[Extract All]`**: `dialog_file_save`-style folder picker → `zip_extract(&g_zip, dest_dir)` → `CTRL_PROGRESSBAR` modal (entry count / total progress) → `notify_send("Extraction complete", dest_dir, ICON_FOLDER, 3000)` → persist last path to `HKCU\Software\Impossible\Archiver\LastExtractPath`
- [ ] **`[Add Files]`**: `dialog_file_open(multi-select)` → for each path: `zip_add_file(&g_zip, filepath, entry_name)` → refresh listview → update status bar
- [ ] **`[New Archive]`**: `dialog_file_save("ZIP Archive|*.zip")` → `zip_create(path)` → clear listview; update title
- [ ] **`[Delete Selected]`**: confirm dialog `"Delete {n} selected items from archive?"` → rebuild archive without selected entries (re-create in-memory, copy non-selected entries via `zip_read`/`zip_add_file`, then `zip_close` + save)
- [ ] **Drag-and-drop extract**: `WM_DROPFILES` on the archiver window → add dragged files to archive; drag from listview to file manager → `zip_extract_file(&g_zip, entry_name, temp_path)` then pass to file manager
- [ ] **Password-protected archives**: detect `ZIP_ERR_ENCRYPTED` on `zip_open`; show `"Password-protected archives are not supported"` `MessageBox`
- [ ] **CLI arg**: `archiver.exe C:\path\file.zip` → open immediately

---

## 5. ZIP Shell Integration `[Sonnet]`

**Source file:** `src/apps/archiver/zip_shell.c`

- [ ] **File associations**: `file_assoc_set(".zip", "ImpossibleOS.Archiver", "C:\\Impossible\\System32\\archiver.exe")` -- double-click `.zip` → `archiver.exe {path}`
- [ ] **"Extract Here" context verb**:
  - [ ] Register `"extract_here"` verb on `.zip` via `context_menu_register_verb(".zip", "Extract Here", cmd_extract_here)`
  - [ ] `cmd_extract_here(path)`: `zip_open` → `zip_extract(&z, containing_dir_of(path))` → progress dialog → `notify_send("Extracted to {dir}", ...)`
- [ ] **"Extract to {folder}\" context verb**:
  - [ ] Creates `"{zip_basename}\"` subfolder in same directory → `zip_extract` into it → toast
- [ ] **"Send to → Compressed (zipped) folder" context verb**:
  - [ ] Register on `*` (all files/folders) as a "Send To" target
  - [ ] `cmd_compress_to_zip(paths[], count)`: `zip_create("{first_file_basename}.zip")` in same directory → `zip_add_file` for each path → `notify_send("Archive created", path, ICON_ZIP, 3000)`
- [ ] **Progress dialog** (for large archives):
  - [ ] Modal `wm_create_window("Extracting…", 360, 100, WM_FIXED | WM_NO_CLOSE)` -- `CTRL_PROGRESSBAR` (0–100 %) + `"{current_entry} of {total}"` label + `[Cancel]` button (sets `g_cancel = true`; checked in extract loop)
- [ ] **`.tar.gz` read (stretch)**:
  - [ ] Detect `.tar.gz` / `.tgz` magic (`\x1f\x8b` gzip header); decompress via `mz_uncompress` to memory buffer
  - [ ] Parse tar header blocks (512-byte blocks; fields: name[100], size (octal), typeflag); list + extract files only (no symlink creation); read-only (no write support)
  - [ ] `file_assoc_set(".tar.gz", ...)` + `file_assoc_set(".tgz", ...)` → `archiver.exe`

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                               | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ------------------------------------- | ---------------------------------------- |
| 💎   | PrtSc → full-screen PNG capture + clipboard | ✅ PrtSc copies to clipboard; Win+PrtSc   | ✅ GNOME screenshot / flameshot        | ⬜ §1 -- `fb_get_backbuffer` + `image_save_png` + `clipboard_set` |
| 💎   | Alt+PrtSc → active window capture        | ✅ Alt+PrtSc copies window to clipboard   | ✅ GNOME screenshot                    | ⬜ §1 -- `wm_get_focused` + `wm_get_window_rect` crop |
| ⭐   | Win+Shift+S → rubber-band region with clear-region dim overlay | ✅ Snipping Tool (Win+Shift+S)            | ✅ flameshot / gnome-screenshot --area | ⬜ §2 -- z_order=32767 overlay, clear-region blit + |
| 💎   | Snipping Tool with mode/delay selector + annotation | ✅ Snipping Tool (full app)               | ✅ flameshot (annotate)                | ⬜ §3 -- mode/delay, pen/highlighter strokes, crop, undo |
| ⭐   | Post-capture floating toolbar            | ✅ Snipping Tool post-capture bar         | ⚠️ flameshot (basic)                  | ⬜ §2 -- auto-dismiss 5 s panel with      |
| 💎   | ZIP browser                              | ✅ Explorer (built-in ZIP shell extension) | ✅ GNOME Archive Manager / Ark         | ⬜ §4 -- `zip_list` + `CTRL_LISTVIEW` with icon |
| 💎   | Extract All with progress dialog         | ✅ Explorer extract wizard                | ✅ Ark / file-roller                   | ⬜ §4 -- `zip_extract` + `CTRL_PROGRESSBAR` modal + |
| 💎   | Create new ZIP / Add Files               | ✅ Right-click → Send to Compressed       | ✅ Ark / file-roller                   | ⬜ §4 -- §5; `zip_create` + `zip_add_file` |
| 💎   | "Extract Here" / "Send to Compressed" context verbs | ✅ Explorer shell extension built-in      | ✅ Nautilus / Dolphin                  | ⬜ §5 -- `context_menu_register_verb` for `.zip` and `*` |
| 💎   | `.tar.gz` read support                   | ✅ Explorer (via third-party or WSL)      | ✅ tar built-in                        | ⬜ §5 -- (Stretch) -- ; gzip header       |

Impossible OS bundles the post-capture annotation toolbar as an intrinsic part of the
screenshot flow -- capturing, copying to clipboard, toasting, and offering instant annotation
launch in one gesture -- with no separate "Snipping Tool" app required to open. The clear-region
dim overlay is composited using the existing WM z-order layer rather than a system-mode
overlay hack, keeping the implementation fully app-level.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **PrtSc hotkey**: press PrtSc → PNG saved to `C:\Users\{name}\Pictures\Screenshots\Screenshot_{timestamp}.png`; toast appears; clipboard contains screenshot; paste into Photos confirms correct image
- [ ] **Alt+PrtSc**: with a window focused → saves only the window rect; background not captured
- [ ] **Win+Shift+S**: screen dims; cursor crosshair; drag → selected region shows clear; release → capture + post-capture toolbar appears; Click `[Copy]` → paste in Notepad (image format) confirms; Escape cancels with no file saved
- [ ] **3-second delay**: mode = Full Screen, delay = 3 s → "3… 2… 1…" OSD → capture fires; useful for capturing menus
- [ ] **Annotation**: pen stroke visible; color picker changes ink color; highlighter renders semi-transparent; undo reverts last stroke; Save overwrites PNG
- [ ] **Open ZIP**: `archiver.exe` with `.zip` arg → listview populated with entries; Name/Size/Compressed/Modified all correct; folder entry double-click narrows list to subfolder
- [ ] **Extract All**: choose destination → `CTRL_PROGRESSBAR` advances → files appear on disk → toast fired
- [ ] **Add Files**: open dialog → select 2 files → both appear in listview; status bar updates compressed size
- [ ] **New Archive**: Save As → empty ZIP created; Add Files → ZIP gains entries; save + reopen confirms entries persist
- [ ] **Delete selected**: select entry → Delete → confirm → entry no longer in listview; re-open ZIP confirms removal
- [ ] **Extract Here verb**: right-click `.zip` in File Manager → "Extract Here" → files appear in same directory
- [ ] **Send to Compressed**: right-click 3 files → "Send to → Compressed (zipped) folder" → `.zip` created with all 3 entries
- [ ] **File assoc**: `.zip` double-click → `archiver.exe` opens
- [ ] Commit: `"apps: sniptool -- capture/region/annotate hotkeys; archiver -- ZIP browse/extract/create/shell verbs"`
