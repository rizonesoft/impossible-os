---
schema_version: 1
id: notepad
domain: 11-apps
status: active
title: "TODO-08 -- Notepad"
---

# TODO-08 -- Notepad

> **Goal:** Ship `notepad.exe` -- Impossible OS's primary text editor, covering the gap buffer
> text engine, full keyboard/mouse editing, file operations, Find & Replace, undo/redo, and
> file associations for `.txt`, `.log`, `.ini`, and `.conf` files.

> [!IMPORTANT]
> **Canonical implementation spec:** §6 to §12 of this file (moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md`, which is now a pointer) -- those sections contain
> the full implementation detail for all sections below (gap buffer §1, text rendering §2,
> undo/redo §3, file menu §4, editing features §5, find & replace §6, stretch §7). This TODO
> is the app-layer companion that extends the spec with file-type associations not covered
> there, adds explicit CRLF/LF detection and encoding-display requirements, and acts as the
> migration target for `todo-old/310-Core-Apps/TODO-330-Notepad.md`.
>
> Clipboard APIs: `clipboard_set/get(CLIP_TEXT, data, size)` -- `SYS_CLIPBOARD_SET=56`,
> `SYS_CLIPBOARD_GET=57` from `09-desktop-shell/TODO-01-clipboard.md §1`.
> File dialog: `dialog_file_open()` / `dialog_file_save()` from `08-graphics-ui/TODO-06 §8`.

---

## Inputs

- §6 to §12 of this file -- canonical implementation spec (gap buffer, rendering, editing, file ops, find/replace, undo/redo, stretch)
- `include/font_mgr.h` -- `ttf_get(slot, px)`, `ttf_draw_string()`, `ttf_measure_width()` -- §2 rendering
- `include/kernel/clipboard.h` (→ XREF `09-desktop-shell/TODO-01 §1`) -- `clipboard_set(CLIP_TEXT, ...)`, `clipboard_get(CLIP_TEXT, ...)` -- §4 cut/copy/paste
- `include/kernel/fs/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create`, `vfs_stat` -- §3 file ops
- `08-graphics-ui/TODO-06-widget-dialogs.md §8` -- `dialog_file_open()`, `dialog_file_save()` -- §2 open/save
- `include/desktop/file_assoc.h` (→ XREF `09-desktop-shell/TODO-02 §1`) -- `file_assoc_set(ext, prog_id, path)` -- §3
- `include/registry.h` -- `reg_get_string`, `reg_set_string` -- font, word-wrap, encoding prefs
- `include/desktop/controls.h` -- `CTRL_TEXTBOX`, `CTRL_SCROLLBAR_VERT`, `CTRL_STATUSBAR`, `CTRL_MENUBAR`
- `include/desktop/wm.h` -- `wm_create_window()`

---

## Outcome

`notepad.exe` opens `.txt`, `.log`, `.ini`, `.conf` files from File Manager, shell, or drag-drop. The editor provides gap-buffer O(1) insert/delete, blinking I-beam cursor, click+drag selection, clipboard integration, word wrap, status bar showing `Ln N, Col N | UTF-8 | CRLF`, Find & Replace, 200-step undo/redo, and configurable font from Registry.

---

## Implementation Order

| Step | Section                              | 💎/⭐ | Dependency                                                                             | Status |
| ---- | ------------------------------------ | ----- | -------------------------------------------------------------------------------------- | :----: |
| 1    | Gap Buffer Text Engine               | 💎    | §13                                                                                    |  [ ]   |
| 2    | Text Rendering + Cursor              | 💎    | §13, §1                                                                                |  [ ]   |
| 3    | File Operations                      | 💎    | §13, §1                                                                                |  [ ]   |
| 4    | Editing Features                     | 💎    | §13, §2, §3                                                                            |  [ ]   |
| 5    | File Associations + App Registration | ⭐    | §13, §3                                                                                |  [ ]   |
| 6    | Gap Buffer                           | 💎    | §13                                                                                    |  [ ]   |
| 7    | Text Rendering & Cursor              | 💎    | §13                                                                                    |  [ ]   |
| 8    | Undo/Redo                            | 💎    | §13                                                                                    |  [ ]   |
| 9    | File Menu                            | 💎    | §13                                                                                    |  [ ]   |
| 10   | Editing Features                     | 💎    | §13                                                                                    |  [ ]   |
| 11   | Find & Replace                       | 💎    | §13                                                                                    |  [ ]   |
| 12   | Stretch Features                     | 💎    | §13                                                                                    |  [ ]   |
| 13   | Import Notepad                       | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §3 |  [ ]   |

> Full section-by-section implementation detail is in §6 to §12 of this file.
> Sections below record app-specific requirements that supplement that spec.

---

## 1. Gap Buffer Text Engine

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings Gap Buffer Text Engine"`

---

## 2. Text Rendering + Cursor

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings Text Rendering + Cursor"`

---

## 3. File Operations

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings File Operations"`

---

## 4. Editing Features

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings Editing Features"`

---

## 5. File Associations + App Registration

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] Register file associations at first launch (or after OS install):
  - [ ] `file_assoc_set(".txt",  "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".log",  "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".ini",  "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".conf", "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".md",   "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")` (also targets syntax-highlight stretch in TODO-10 §7)
- [ ] **Context menu integration**: right-click on any file in File Manager → `Open with Notepad` entry (registered via `file_assoc_register_verb("Open with Notepad", "notepad.exe %1")`)
- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings File Associations + App Registration"`

## 6. Gap Buffer

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §1** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** n/a -- the gap buffer is a data structure with no visual surface

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings Gap Buffer"`

## 7. Text Rendering & Cursor

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §2** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings Text Rendering & Cursor"`

## 8. Undo/Redo

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §3** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** n/a -- the undo/redo stack has no visual surface; its menu items are drawn in §9

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings Undo/Redo"`

## 9. File Menu

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §4** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control), [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings File Menu"`

## 10. Editing Features

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §5** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#scroll-bar`](../../docs/design/controls.md#scroll-bar), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings Editing Features"`

## 11. Find & Replace

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §6** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`controls.md#text-box-password-box-and-search-box`](../../docs/design/controls.md#text-box-password-box-and-search-box), [`controls.md#dialog`](../../docs/design/controls.md#dialog)

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings Find & Replace"`

## 12. Stretch Features

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §7** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** notepad

Built in Notepad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: notepad brings Stretch Features"`

## 13. Import Notepad

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** notepad

Notepad is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull notepad` into `src/apps/notepad/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Notepad on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose notepad`
- [ ] Commit: `"apps: Notepad imported"`

---

## OS Comparison


| ⭐  | Feature                                              | 🪟 Win11                               | 🐧 Linux                        | 🚀 Impossible OS                                                      |
| --- | ---------------------------------------------------- | -------------------------------------- | ------------------------------- | --------------------------------------------------------------------- |
| 💎  | Gap buffer O(1) insert/delete                        | ✅ Notepad (rope-based in modern; gap  | ✅ gedit / Kate (GtkTextBuffer) | ⬜ §1 -- `pmm_alloc_contiguous`, grow-by-doubling                     |
| 💎  | TTF text rendering + blinking I-beam                 | ✅ Notepad (DirectWrite)               | ✅ gedit (Pango/Cairo)          | ⬜ §2 -- `ttf_draw_string()` per visible line                         |
| 💎  | CRLF/LF detection + status bar encoding display      | ✅ Notepad (bottom status bar: Ln/Col, | ✅ gedit (status bar)           | ⬜ §2 -- status bar + §1 `text_detect_line_ending()`                  |
| 💎  | File open/save + unsaved-changes prompt              | ✅ Notepad                             | ✅ gedit                        | ⬜ §3 -- `dialog_file_open/save`, title asterisk, save-changes dialog |
| 💎  | Click/drag selection + clipboard                     | ✅ Notepad                             | ✅ gedit                        | ⬜ §4 -- `clipboard_set/get(CLIP_TEXT)`                               |
| 💎  | 200-step undo/redo                                   | ✅ Notepad (unlimited undo since Win10 | ✅ gedit / Kate                 | ⬜ §4 -- 200-entry ring, coalesced single-char inserts                |
| 💎  | Find + Replace                                       | ✅ Notepad (with regex in Win11)       | ✅ gedit                        | ⬜ §4 -- + `§11`                                                      |
| ⭐  | `.txt`, `.log`, `.ini`, `.conf`, `.md` all → Notepad | ✅ Notepad (`.txt`); `.ini` → Notepad; | ⚠️ `xdg-open` varies by distro  | ⬜ §5 -- 5× `file_assoc_set` calls, "Open with                        |

Impossible OS Notepad handles every plain-text file type the OS produces out-of-the-box -- including `.ini` config files and `.log` system logs -- with one consistent editor, no third-party plugin or format association needed.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **Gap buffer:** insert 10000 characters at random positions; `text_line_count` returns correct count; buffer grows correctly (no crash, no data corruption); cursor position preserved after grow
- [ ] **CRLF detection:** open a Windows-format file (with `\r\n`); status bar shows `CRLF`; open a Unix file (`\n`); shows `LF`; save preserves original ending
- [ ] **Rendering:** open a 200-line file; scroll to bottom; all lines visible; cursor blinks; status bar shows correct Ln/Col; word wrap wraps visually without inserting `\n`
- [ ] **File ops:** New → `*Untitled` in title; type text; close → "Save changes?" dialog appears; Save As to `C:\Temp\test.txt`; title updates to `test.txt -- Notepad`; reopen confirms content
- [ ] **Clipboard:** type `hello world`; Ctrl+A → selects all (inverted highlight); Ctrl+C; open second notepad.exe; Ctrl+V → `hello world` pasted
- [ ] **Undo/Redo:** type 10 chars; Ctrl+Z × 10 → buffer empty; Ctrl+Y × 10 → all chars back; undo merges consecutive single-char inserts into one step
- [ ] **Find/Replace:** open file with repeated word; Ctrl+F → type word → all occurrences highlighted; F3 cycles; Ctrl+H → Replace All "foo" with "bar" → reports count; all instances replaced
- [ ] **File assoc:** File Manager double-click on `.log` → `notepad.exe` opens file; right-click any file → `Open with Notepad` present
- [ ] Commit: `"apps: Notepad -- gap buffer, rendering, file ops, editing, find/replace, file assoc"`
