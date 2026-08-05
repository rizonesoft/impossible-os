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
> **Canonical implementation spec:** `09-desktop-shell/TODO-10-notepad.md` -- that TODO contains
> the full implementation detail for all sections below (gap buffer §1, text rendering §2,
> undo/redo §3, file menu §4, editing features §5, find & replace §6, stretch §7). This TODO
> is the app-layer companion that extends the spec with file-type associations not covered
> there, adds explicit CRLF/LF detection and encoding-display requirements, and acts as the
> migration target for `todo-old/310-Core-Apps/TODO-330-Notepad.md`.
>
> Clipboard APIs: `clipboard_set/get(CLIP_TEXT, data, size)` -- `SYS_CLIPBOARD_SET=56`,
> `SYS_CLIPBOARD_GET=57` from `09-desktop-shell/TODO-01-clipboard.md §1`.
> File dialog: `dialog_file_open()` / `dialog_file_save()` from `08-graphics-ui/TODO-05 §3`.

---

## Inputs

- `09-desktop-shell/TODO-10-notepad.md` -- canonical implementation spec (gap buffer, rendering, editing, file ops, find/replace, undo/redo, stretch)
- `include/font_mgr.h` -- `ttf_get(slot, px)`, `ttf_draw_string()`, `ttf_measure_width()` -- §2 rendering
- `include/kernel/clipboard.h` (→ XREF `09-desktop-shell/TODO-01 §1`) -- `clipboard_set(CLIP_TEXT, ...)`, `clipboard_get(CLIP_TEXT, ...)` -- §4 cut/copy/paste
- `include/kernel/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create`, `vfs_stat` -- §3 file ops
- `08-graphics-ui/TODO-06-widget-dialogs.md §3` -- `dialog_file_open()`, `dialog_file_save()` -- §2 open/save
- `include/desktop/file_assoc.h` (→ XREF `09-desktop-shell/TODO-02 §1`) -- `file_assoc_set(ext, prog_id, path)` -- §3
- `include/registry.h` -- `reg_get_string`, `reg_set_string` -- font, word-wrap, encoding prefs
- `include/desktop/controls.h` -- `CTRL_TEXTBOX`, `CTRL_SCROLLBAR_VERT`, `CTRL_STATUSBAR`, `CTRL_MENUBAR`
- `include/desktop/wm.h` -- `wm_create_window()`

---

## Outcome

`notepad.exe` opens `.txt`, `.log`, `.ini`, `.conf` files from File Manager, shell, or drag-drop. The editor provides gap-buffer O(1) insert/delete, blinking I-beam cursor, click+drag selection, clipboard integration, word wrap, status bar showing `Ln N, Col N | UTF-8 | CRLF`, Find & Replace, 200-step undo/redo, and configurable font from Registry.

---

## Implementation Order

| Step | Section                              | 💎/⭐ | Dependency                               |
| ---- | ------------------------------------ | ----- | ---------------------------------------- |
| 1    | Gap Buffer Text Engine               | 💎    | `pmm_alloc_contiguous`                   |
| 2    | Text Rendering + Cursor              | 💎    | §1 complete, `ttf_draw_string`           |
| 3    | File Operations                      | 💎    | §1 complete, `dialog_file_open/save`, VFS |
| 4    | Editing Features                     | 💎    | §2 + §3 complete, `clipboard_set/get`    |
| 5    | File Associations + App Registration | ⭐    | §3 complete, `file_assoc_set`            |

> Full section-by-section implementation detail is in `09-desktop-shell/TODO-10-notepad.md §1–§8`.
> Sections below record app-specific requirements that supplement that spec.

---

## 1. Gap Buffer Text Engine `[Opus]`

> → XREF: `09-desktop-shell/TODO-10-notepad.md §1` -- complete implementation spec.
> `[Opus]` justified: gap buffer is a novel data structure with no prior Impossible OS precedent;
> the grow-by-doubling realloc path with `pmm_alloc_contiguous` is an unusual allocation pattern.

**Source file:** `src/apps/notepad/text_buffer.c`; header `include/apps/notepad/text_buffer.h`

- [ ] `struct text_buffer { uint8_t *buf; size_t buf_size; size_t gap_start; size_t gap_end; }` -- initial size 65536 bytes via `pmm_alloc_contiguous(16)` (16 pages)
- [ ] `text_insert(buf, ch)` → drop into gap (`buf[gap_start++] = ch`; gap shrinks); if `gap_start == gap_end` → grow buffer (double via new `pmm_alloc_contiguous`, copy pre-gap + post-gap, free old)
- [ ] `text_delete_before(buf)` → expand gap leftward (`gap_start--`)
- [ ] `text_delete_after(buf)` → expand gap rightward (`gap_end++`)
- [ ] `text_move_cursor(buf, pos)` → shift gap to absolute position by memmove of characters; O(n) worst case but typical edits are O(1)
- [ ] `text_get_line(buf, n, out, max_len)` → scan past `n` newlines in logical buffer (skipping gap region); return line bytes + length
- [ ] `text_line_count(buf)` → count `'\n'` bytes outside gap region
- [ ] **CRLF/LF detection**: `text_detect_line_ending(buf)` → scan first 4 KB for `\r\n` vs `\n`; return `LE_CRLF` or `LE_LF`; used in §2 status bar and §3 save (preserve original line ending on save)

---

## 2. Text Rendering + Cursor `[Sonnet]`

> → XREF: `09-desktop-shell/TODO-10-notepad.md §2`

**Source file:** `src/apps/notepad/notepad_render.c`

- [ ] Font: `ttf_get(FONT_BODY, font_size)` where `font_size` read from `HKCU\Software\Impossible\Notepad\FontSize` (default 14); family from `HKCU\Software\Impossible\Notepad\Font` (default Selawik)
- [ ] Render visible lines: `scroll_y` → start line; iterate `text_get_line(n)` for `(canvas_h / line_h)` lines; `ttf_draw_string(surface, font, left_margin, y, line, GFX_COLOR_TEXT)`
- [ ] Blinking I-beam cursor: 500 ms toggle (compare `system_get_ticks() % 1000 < 500`); draw 1 px vertical line at `(col_x, cursor_y, cursor_y + line_h)`; hidden during selection
- [ ] Navigation: arrow keys (left/right adjust `cursor_col`/`cursor_line` with line-wrap); Home/End; Ctrl+Home/Ctrl+End; Page Up/Page Down (advance `scroll_y` by visible line count)
- [ ] Vertical `CTRL_SCROLLBAR_VERT`: range = `text_line_count(buf)`; step = 1 line; dragging scroll bar updates `scroll_y`
- [ ] **Status bar** (`CTRL_STATUSBAR`): left section `Ln {cursor_line+1}, Col {cursor_col+1}`; center `UTF-8`; right section `CRLF` or `LF` (from `text_detect_line_ending`); updates on every cursor move
- [ ] Word wrap (View menu toggle, stored in `HKCU\Software\Impossible\Notepad\WordWrap`): when enabled, compute visual line breaks at `canvas_w - left_margin` using `ttf_measure_width()`; do not insert `\n` into buffer; `cursor_line`/`cursor_col` remain logical

---

## 3. File Operations `[Sonnet]`

> → XREF: `09-desktop-shell/TODO-10-notepad.md §4`

**Source file:** `src/apps/notepad/notepad_file.c`

- [ ] **New**: if `modified` → `dialog_confirm("Save changes to {filename}?", MB_YESNOCANCEL)` → save or discard; clear buffer; reset `filepath = ""`; window title = `"Untitled -- Notepad"`
- [ ] **Open**: save-changes check if modified; `dialog_file_open("Open", "Text Files|*.txt;*.log;*.ini;*.conf|All Files|*.*")` → `vfs_open` + `vfs_read` into `text_buffer`; detect encoding (UTF-8 BOM `EF BB BF` → strip BOM; else assume UTF-8); detect CRLF/LF; set window title; `modified = 0`
- [ ] **Save**: if `filepath == ""` → Save As; else `vfs_open(WRITE)` + `vfs_write(buf_contents)` (normalize line endings per `text_detect_line_ending` on original); `modified = 0`; update title (remove `*`)
- [ ] **Save As**: `dialog_file_save("Save As", "Text Files|*.txt|All Files|*.*")` → get path; auto-append `.txt` if no extension; save; update `filepath` + title
- [ ] **Window title**: `"{filename} -- Notepad"` (no asterisk) or `"*{filename} -- Notepad"` (modified); `"Untitled -- Notepad"` for new unsaved file
- [ ] **Drag-and-drop**: handle `WM_DROPFILES` message → extract path → open (save-changes check first)
- [ ] **Command-line arg**: `notepad.exe C:\path\file.txt` → open immediately; `notepad.exe` → open with empty Untitled buffer

---

## 4. Editing Features `[Sonnet]`

> → XREF: `09-desktop-shell/TODO-10-notepad.md §3, §5, §7`

**Source file:** `src/apps/notepad/notepad_edit.c`

- [ ] **Mouse click → cursor**: compute `line = (mouse_y - top) / line_h + scroll_y`; compute `col` via binary search over cumulative `ttf_measure_width()` increments → set cursor; request focus
- [ ] **Click+drag / Shift+Arrow selection**: track `sel_start`/`sel_end` (absolute buffer positions); render selected region with `gfx_fill_rect()` using accent color + inverted text color
- [ ] **Ctrl+A**: `sel_start=0`, `sel_end=text_length(buf)`
- [ ] **Cut** (Ctrl+X): `clipboard_set(CLIP_TEXT, selected_text, len)`; `text_delete_range(sel_start, sel_end)`; clear selection
- [ ] **Copy** (Ctrl+C): `clipboard_set(CLIP_TEXT, selected_text, len)` (no delete)
- [ ] **Paste** (Ctrl+V): `clipboard_get(CLIP_TEXT, tmp_buf, max_len)`; if selection active → delete first; insert chars at cursor
- [ ] **Undo/Redo** (Ctrl+Z / Ctrl+Y): ring of 200 `undo_entry { type: INSERT|DELETE|REPLACE, pos, data[64] }` entries; `kmalloc` per entry (≤ 64 bytes); pop on undo, re-apply on redo; action coalescing: consecutive single-char inserts merge into one entry
- [ ] **Find** (Ctrl+F): sliding search bar at bottom (same pattern as PDF viewer §7); highlight all matches; F3 next / Shift+F3 previous; Escape closes
- [ ] **Replace** (Ctrl+H): Find bar extends with Replace field + `[Replace]` `[Replace All]` buttons; `Replace All` reports count `"N replacements made"`

---

## 5. File Associations + App Registration `[Sonnet]`

**Source file:** `src/apps/notepad/notepad_main.c`

- [ ] Register file associations at first launch (or after OS install):
  - [ ] `file_assoc_set(".txt",  "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".log",  "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".ini",  "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".conf", "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".md",   "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")` (also targets syntax-highlight stretch in TODO-10 §8)
- [ ] **Context menu integration**: right-click on any file in File Manager → `Open with Notepad` entry (registered via `file_assoc_register_verb("Open with Notepad", "notepad.exe %1")`)
- [ ] **Recent files**: `HKCU\Software\Impossible\Notepad\RecentFiles\{0..9}` (10 entries, MRU); `File → Recent Files` submenu
- [ ] **Font preferences**: `Settings → Notepad → Font` (family + size) stored in Registry; Ctrl++ / Ctrl+− zoom (temporary, not persisted); Ctrl+0 resets to Registry value

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                               | 🐧 Linux                        | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | -------------------------------------- | ------------------------------- | ---------------------------------------- |
| 💎  | Gap buffer O(1) insert/delete            | ✅ Notepad (rope-based in modern; gap  | ✅ gedit / Kate (GtkTextBuffer) | ⬜ §1 -- `pmm_alloc_contiguous`, grow-by-doubling |
| 💎  | TTF text rendering + blinking I-beam     | ✅ Notepad (DirectWrite)               | ✅ gedit (Pango/Cairo)          | ⬜ §2 -- `ttf_draw_string()` per visible line |
| 💎  | CRLF/LF detection + status bar encoding display | ✅ Notepad (bottom status bar: Ln/Col, | ✅ gedit (status bar)           | ⬜ §2 -- status bar + §1 `text_detect_line_ending()` |
| 💎  | File open/save + unsaved-changes prompt  | ✅ Notepad                             | ✅ gedit                        | ⬜ §3 -- `dialog_file_open/save`, title asterisk, save-changes dialog |
| 💎  | Click/drag selection + clipboard         | ✅ Notepad                             | ✅ gedit                        | ⬜ §4 -- `clipboard_set/get(CLIP_TEXT)`  |
| 💎  | 200-step undo/redo                       | ✅ Notepad (unlimited undo since Win10 | ✅ gedit / Kate                 | ⬜ §4 -- 200-entry ring, coalesced single-char inserts |
| 💎  | Find + Replace                           | ✅ Notepad (with regex in Win11)       | ✅ gedit                        | ⬜ §4 -- + `09-desktop-shell/TODO-10 §7` |
| ⭐  | `.txt`, `.log`, `.ini`, `.conf`, `.md` all → Notepad | ✅ Notepad (`.txt`); `.ini` → Notepad; | ⚠️ `xdg-open` varies by distro   | ⬜ §5 -- 5× `file_assoc_set` calls, "Open with |

Impossible OS Notepad handles every plain-text file type the OS produces out-of-the-box --
including `.ini` config files and `.log` system logs -- with one consistent editor, no
third-party plugin or format association needed.

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
