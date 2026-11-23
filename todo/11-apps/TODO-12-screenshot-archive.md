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
> **Scope overlap:** §6 of this file (moved here 2026-11-14 from `09-desktop-shell/TODO-12-utilities.md`) specifies the Win+Shift+S
> region capture flow and §7 specifies Archive Manager; `08-graphics-ui/TODO-09` §5 owns basic
> PrtSc capture. This file owns both since 2026-11-14; the items at the top of §1, §4, §6 and §7 settle the remaining differences inside it.
>
> **Global hotkeys** use `struct hotkey_entry` + `hotkey_table[32]` from
> `08-graphics-ui/TODO-08-window-manager.md §5`.
> PrtSc = full screen, Alt+PrtSc = active window, Win+Shift+S = region select.
>
> **ZIP write API** (`zip_create`, `zip_add_file`, `zip_extract`, `zip_list`) defined in
> `09-desktop-shell/TODO-04-recycle-zip-scheduler.md §4` -- implement §4 of that TODO before
> building Archive Manager write operations.

---

## Inputs

- `include/kernel/drivers/framebuffer.h` -- `fb_get_backbuffer()`, `fb_get_width()`, `fb_get_height()` -- §1 full-screen capture
- `include/kernel/image.h` -- `image_save_png()`, `image_save_bmp()`, `image_load_mem()` -- §1 §2 §3 save
- `include/gfx.h` -- `gfx_fill_rect()`, `gfx_blit()`, `gfx_draw_rect()`, `gfx_surface_create()` -- §2 overlay, §3 annotate
- `include/desktop/wm.h` -- `wm_create_window()` (z_order=32767 for overlay), `wm_mark_dirty()`, `wm_get_focused_window()`, `wm_get_window_rect()` -- §1 §2 §3
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_LISTVIEW`, `CTRL_SCROLLBAR_VERT`, `CTRL_STATUSBAR`, `CTRL_PROGRESSBAR` -- §3 §4
- `08-graphics-ui/TODO-08-window-manager.md §5` (→ XREF) -- `struct hotkey_entry`, `hotkey_table[]`, `MOD_WIN/MOD_ALT/MOD_SHIFT`, `KEY_PRINTSCREEN` -- §1 §2 global hotkeys
- `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §5` (→ XREF) -- `notify_send(title, body, icon_id, timeout_ms)` -- §1 §4 toast
- `09-desktop-shell/TODO-01-clipboard.md §1` (→ XREF) -- `clipboard_set(CLIP_IMAGE, &img)` -- §1 §2 copy to clipboard
- `08-graphics-ui/TODO-06-widget-dialogs.md §8` (→ XREF) -- `dialog_file_open()`, `dialog_file_save()` -- §2 §3 §5
- `include/registry.h` -- `RegGetString`, `RegSetString` -- §3 recent captures, §4 last extract path
- `include/desktop/file_assoc.h` (→ XREF `09-desktop-shell/TODO-02 §1`) -- `file_assoc_set()` -- §3
- `02-kernel-core/TODO-03-kernel-libraries.md §4` (→ XREF) -- `zip_open`, `zip_entry_count`, `zip_find`, `zip_read`, `zip_close` -- §6 §3 read
- `09-desktop-shell/TODO-04-recycle-zip-scheduler.md §4` (→ XREF) -- `zip_create`, `zip_add_file`, `zip_extract`, `zip_extract_file`, `zip_list` -- §6 §5 write
- `include/kernel/fs/vfs.h` -- `vfs_create`, `vfs_write`, `vfs_stat` (`vfs_mkdir` is not declared yet) -- §1 §4 §5 file output
- `include/kernel/timer.h` -- `system_get_ticks()`, `time_now()` -- §1 timestamp filename, §3 delay

---

## Outcome

`sniptool.exe` captures full-screen, active-window, or rubber-band region screenshots and saves timestamped PNGs to `C:\Users\{name}\Pictures\Screenshots\`. Global hotkeys (PrtSc / Alt+PrtSc / Win+Shift+S) work without launching the GUI. `archiver.exe` opens `.zip` files, lists contents, extracts, and creates new archives. Right-click shell verbs "Extract Here" and "Send to → Compressed folder" integrate ZIP into the file manager context menu.

---

## Implementation Order

| Step | Section                         | 💎/⭐ | Dependency                                                                             | Status |
| ---- | ------------------------------- | ----- | -------------------------------------------------------------------------------------- | :----: |
| 1    | Full + Window Capture + Hotkeys | 💎    | §8                                                                                     |  [ ]   |
| 2    | Region Select Overlay           | ⭐    | §8, §1                                                                                 |  [ ]   |
| 3    | Snipping Tool UI                | 💎    | §8, §1, §2                                                                             |  [ ]   |
| 4    | Archive Manager                 | 💎    | §9                                                                                     |  [ ]   |
| 5    | ZIP Shell Integration           | 💎    | §4 stable, `file_assoc_set`, context menu verbs                                        |  [ ]   |
| 6    | Screenshot Enhancements         | 💎    | §8                                                                                     |  [ ]   |
| 7    | Archive Manager                 | 💎    | §9                                                                                     |  [ ]   |
| 8    | Import Screenshot               | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §6 |  [ ]   |
| 9    | Import Zip                      | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §3 |  [ ]   |

---

## 1. Full + Window Capture + Hotkeys

**Moved:** stillframe

Built in Stillframe, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] Settle screenshot ownership (item: "Settle screenshot ownership") before coding: `08-graphics-ui/TODO-09` §5 owns PrtSc and Alt+PrtSc (`src/desktop/screenshot.c`)
  - §6 of this file owns Win+Shift+S; §1 and §2 restate both under `src/apps/snip/`. Keep capture in one place and only the snipping window and annotation here.
- [ ] **Global hotkey registration** (called from `snip_init()` at desktop startup):
  - [ ] `hotkey_table[N] = { .modifiers=0, .scancode=KEY_PRINTSCREEN, .handler=screenshot_full }`
  - [ ] `hotkey_table[N+1] = { .modifiers=MOD_ALT, .scancode=KEY_PRINTSCREEN, .handler=screenshot_active_window }` -- `screenshot_active_window()` = `screenshot_window(wm_get_focused_window())`
  - [ ] `hotkey_table[N+2] = { .modifiers=MOD_WIN|MOD_SHIFT, .scancode=KEY_S, .handler=snip_region_start }` -- launches region select (§2)
- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: stillframe brings Full + Window Capture + Hotkeys"`

---

## 2. Region Select Overlay

**Design:** [`shell.md#snipping-toolbar`](../../docs/design/shell.md#snipping-toolbar), [`shell.md#toast-notifications`](../../docs/design/shell.md#toast-notifications)

**Moved:** stillframe

Built in Stillframe, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: stillframe brings Region Select Overlay"`

---

## 3. Snipping Tool UI

**Design:** [`shell.md#snipping-toolbar`](../../docs/design/shell.md#snipping-toolbar), [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** stillframe

Built in Stillframe, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: stillframe brings Snipping Tool UI"`

---

## 4. Archive Manager

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout), [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)

**Moved:** zip

Built in Zip, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: zip brings Archive Manager"`

---

## 5. ZIP Shell Integration

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
    - [ ] Progress: call `ui_dialog_progress()` from `11-apps/TODO-13 §7`; do not build a second progress window
    - Standard dialog with the progress bar per `docs/design/controls.md#progress`, `"{current_entry} of {total}"` and a Cancel button
- [ ] **`.tar.gz` read (stretch)**:
  - [ ] Detect `.tar.gz` / `.tgz` magic (`\x1f\x8b` gzip header); decompress via `mz_uncompress` to memory buffer
  - [ ] Parse tar header blocks (512-byte blocks; fields: name[100], size (octal), typeflag); list + extract files only (no symlink creation); read-only (no write support)
  - [ ] `file_assoc_set(".tar.gz", ...)` + `file_assoc_set(".tgz", ...)` → `archiver.exe`

## 6. Screenshot Enhancements

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-12-utilities.md` §4** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#snipping-toolbar`](../../docs/design/shell.md#snipping-toolbar), [`shell.md#toast-notifications`](../../docs/design/shell.md#toast-notifications)

**Moved:** stillframe

Built in Stillframe, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] `screenshot_capture_region(x, y, w, h)` -- copy compositor backbuf rect; `image_save_bmp()`; `clipboard_set()`
- [ ] Win+Shift+S global hotkey registration
- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: stillframe brings Screenshot Enhancements"`

## 7. Archive Manager

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-12-utilities.md` §5** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** zip

Built in Zip, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] Double-click entry: `zip_extract_file()` to `C:\Temp\` → `file_assoc_open()`
- [ ] Register `.zip` in file associations (TODO-02 §1 default assocs)
- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: zip brings Archive Manager"`

## 8. Import Screenshot

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** stillframe

Screenshot is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull stillframe` into `src/apps/stillframe/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Screenshot on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose stillframe`
- [ ] Commit: `"apps: Screenshot imported"`

## 9. Import Zip

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** zip

Zip is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull zip` into `src/apps/zip/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Zip on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose zip`
- [ ] Commit: `"apps: Zip imported"`

---

## OS Comparison


| ⭐  | Feature                                                        | 🪟 Win11                                   | 🐧 Linux                               | 🚀 Impossible OS                                                  |
| --- | -------------------------------------------------------------- | ------------------------------------------ | -------------------------------------- | ----------------------------------------------------------------- |
| 💎  | PrtSc → full-screen PNG capture + clipboard                    | ✅ PrtSc copies to clipboard; Win+PrtSc    | ✅ GNOME screenshot / flameshot        | ⬜ §1 -- `fb_get_backbuffer` + `image_save_png` + `clipboard_set` |
| 💎  | Alt+PrtSc → active window capture                              | ✅ Alt+PrtSc copies window to clipboard    | ✅ GNOME screenshot                    | ⬜ §1 -- `wm_get_focused_window` + `wm_get_window_rect` crop      |
| ⭐  | Win+Shift+S → rubber-band region with clear-region dim overlay | ✅ Snipping Tool (Win+Shift+S)             | ✅ flameshot / gnome-screenshot --area | ⬜ §2 -- z_order=32767 overlay, clear-region blit +               |
| 💎  | Snipping Tool with mode/delay selector + annotation            | ✅ Snipping Tool (full app)                | ✅ flameshot (annotate)                | ⬜ §3 -- mode/delay, pen/highlighter strokes, crop, undo          |
| ⭐  | Post-capture floating toolbar                                  | ✅ Snipping Tool post-capture bar          | ⚠️ flameshot (basic)                   | ⬜ §2 -- auto-dismiss 5 s panel with                              |
| 💎  | ZIP browser                                                    | ✅ Explorer (built-in ZIP shell extension) | ✅ GNOME Archive Manager / Ark         | ⬜ §4 -- `zip_list` + `CTRL_LISTVIEW` with icon                   |
| 💎  | Extract All with progress dialog                               | ✅ Explorer extract wizard                 | ✅ Ark / file-roller                   | ⬜ §4 -- `zip_extract` + `CTRL_PROGRESSBAR` modal +               |
| 💎  | Create new ZIP / Add Files                                     | ✅ Right-click → Send to Compressed        | ✅ Ark / file-roller                   | ⬜ §4 -- §5; `zip_create` + `zip_add_file`                        |
| 💎  | "Extract Here" / "Send to Compressed" context verbs            | ✅ Explorer shell extension built-in       | ✅ Nautilus / Dolphin                  | ⬜ §5 -- `context_menu_register_verb` for `.zip` and `*`          |
| 💎  | `.tar.gz` read support                                         | ✅ Explorer (via third-party or WSL)       | ✅ tar built-in                        | ⬜ §5 -- (Stretch) -- ; gzip header                               |

Impossible OS bundles the post-capture annotation toolbar as an intrinsic part of the screenshot flow -- capturing, copying to clipboard, toasting, and offering instant annotation launch in one gesture -- with no separate "Snipping Tool" app required to open. The clear-region dim overlay is composited using the existing WM z-order layer rather than a system-mode overlay hack, keeping the implementation fully app-level.

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
