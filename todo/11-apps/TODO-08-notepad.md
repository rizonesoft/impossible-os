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

| Step | Section                              | 💎/⭐ | Dependency                                |
| ---- | ------------------------------------ | ----- | ----------------------------------------- |
| 1    | Gap Buffer Text Engine               | 💎    | `pmm_alloc_contiguous`                    |
| 2    | Text Rendering + Cursor              | 💎    | §1 complete, `ttf_draw_string`            |
| 3    | File Operations                      | 💎    | §1 complete, `dialog_file_open/save`, VFS |
| 4    | Editing Features                     | 💎    | §2 + §3 complete, `clipboard_set/get`     |
| 5    | File Associations + App Registration | ⭐    | §3 complete, `file_assoc_set`             |
| 6    | Gap Buffer                           | 💎    | --                                        |
| 7    | Text Rendering & Cursor              | 💎    | --                                        |
| 8    | Undo/Redo                            | 💎    | --                                        |
| 9    | File Menu                            | 💎    | --                                        |
| 10   | Editing Features                     | 💎    | --                                        |
| 11   | Find & Replace                       | 💎    | --                                        |
| 12   | Stretch Features                     | 💎    | --                                        |

> Full section-by-section implementation detail is in §6 to §12 of this file.
> Sections below record app-specific requirements that supplement that spec.

---

## 1. Gap Buffer Text Engine

> → XREF: `§6` -- complete implementation spec.
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

## 2. Text Rendering + Cursor

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> → XREF: `§7`

**Source file:** `src/apps/notepad/notepad_render.c`

- [ ] Font: `ttf_get(FONT_UI, font_size)` where `font_size` read from `HKCU\Software\Impossible\Notepad\FontSize` (default 14); family from `HKCU\Software\Impossible\Notepad\Font` (default Selawik)
- [ ] Render visible lines: `scroll_y` → start line; iterate `text_get_line(n)` for `(canvas_h / line_h)` lines; `ttf_draw_string(surface, font, left_margin, y, line, GFX_COLOR_TEXT)`
- [ ] Blinking I-beam cursor: 500 ms toggle (compare `system_get_ticks() % 1000 < 500`); draw 1 px vertical line at `(col_x, cursor_y, cursor_y + line_h)`; hidden during selection
- [ ] Navigation: arrow keys (left/right adjust `cursor_col`/`cursor_line` with line-wrap); Home/End; Ctrl+Home/Ctrl+End; Page Up/Page Down (advance `scroll_y` by visible line count)
- [ ] Vertical `CTRL_SCROLLBAR_VERT`: range = `text_line_count(buf)`; step = 1 line; dragging scroll bar updates `scroll_y`
- [ ] **Status bar** (`CTRL_STATUSBAR`): left section `Ln {cursor_line+1}, Col {cursor_col+1}`; center `UTF-8`; right section `CRLF` or `LF` (from `text_detect_line_ending`); updates on every cursor move
- [ ] Word wrap (View menu toggle, stored in `HKCU\Software\Impossible\Notepad\WordWrap`): when enabled, compute visual line breaks at `canvas_w - left_margin` using `ttf_measure_width()`; do not insert `\n` into buffer; `cursor_line`/`cursor_col` remain logical

---

## 3. File Operations

> → XREF: `§9`

**Source file:** `src/apps/notepad/notepad_file.c`

- [ ] **New**: if `modified` → `dialog_confirm("Save changes to {filename}?", MB_YESNOCANCEL)` → save or discard; clear buffer; reset `filepath = ""`; window title = `"Untitled -- Notepad"`
- [ ] **Open**: save-changes check if modified; `dialog_file_open("Open", "Text Files|*.txt;*.log;*.ini;*.conf|All Files|*.*")` → `vfs_open` + `vfs_read` into `text_buffer`; detect encoding (UTF-8 BOM `EF BB BF` → strip BOM; else assume UTF-8); detect CRLF/LF; set window title; `modified = 0`
- [ ] **Save**: if `filepath == ""` → Save As; else `vfs_open(WRITE)` + `vfs_write(buf_contents)` (normalize line endings per `text_detect_line_ending` on original); `modified = 0`; update title (remove `*`)
- [ ] **Save As**: `dialog_file_save("Save As", "Text Files|*.txt|All Files|*.*")` → get path; auto-append `.txt` if no extension; save; update `filepath` + title
- [ ] **Window title**: `"{filename} -- Notepad"` (no asterisk) or `"*{filename} -- Notepad"` (modified); `"Untitled -- Notepad"` for new unsaved file
- [ ] **Drag-and-drop**: handle `WM_DROPFILES` message → extract path → open (save-changes check first)
- [ ] **Command-line arg**: `notepad.exe C:\path\file.txt` → open immediately; `notepad.exe` → open with empty Untitled buffer

---

## 4. Editing Features

> → XREF: `§8, §10, §11`

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

## 5. File Associations + App Registration

**Source file:** `src/apps/notepad/notepad_main.c`

- [ ] Reconcile §1 to §5 with the base sections §6 to §12 of this file before coding: one source layout and one default extension list
  - Files: `text_buffer.c`/`notepad_*.c` here vs that file's layout; extensions `.txt/.log/.ini/.conf/.md` here vs `.txt/.md/.c/.h/.asm` there. Default associations belong to `09-desktop-shell/TODO-02` §2.
- [ ] Register file associations at first launch (or after OS install):
  - [ ] `file_assoc_set(".txt",  "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".log",  "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".ini",  "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".conf", "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")`
  - [ ] `file_assoc_set(".md",   "ImpossibleOS.Notepad", "C:\\Impossible\\System32\\notepad.exe")` (also targets syntax-highlight stretch in TODO-10 §7)
- [ ] **Context menu integration**: right-click on any file in File Manager → `Open with Notepad` entry (registered via `file_assoc_register_verb("Open with Notepad", "notepad.exe %1")`)
- [ ] **Recent files**: `HKCU\Software\Impossible\Notepad\RecentFiles\{0..9}` (10 entries, MRU); `File → Recent Files` submenu
- [ ] **Font preferences**: `Settings → Notepad → Font` (family + size) stored in Registry; Ctrl++ / Ctrl+− zoom (temporary, not persisted); Ctrl+0 resets to Registry value

## 6. Gap Buffer

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §1** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** n/a -- the gap buffer is a data structure with no visual surface

`text_buffer_t` (buf, buf_size, gap_start, gap_end). PMM 64 KiB initial allocation. `text_insert/delete` O(1). `text_move_cursor` shifts gap via memcpy. `text_get_line/line_count`. `text_load/save_file`.

**Files:** `src/apps/notepad/text_buffer.c` (new), `include/apps/text_buffer.h` (new)

> [!NOTE]
> `[Opus]` due to novel data structure design (no prior Impossible OS text editor precedent), non-trivial PMM doubling strategy (allocate 2× new buffer + memcpy pre-gap + memcpy post-gap + `pmm_free_contiguous` old), and correctness requirements (gap start/end accounting must be exact or all text operations produce corruption). **Buffer layout**: `[pre-gap text][--- gap ---][post-gap text]` within `buf[0..buf_size-1]`. `gap_start` = end of pre-gap content; `gap_end` = start of post-gap content; free gap space = `gap_end - gap_start`. **Insert** `ch` at cursor: if `gap_start == gap_end` → `text_grow(buf)` first; `buf[gap_start++] = ch`. **Delete** backwards: if `gap_start > 0` → `gap_start--` (just shrink pre-gap). **Move cursor forward** by N: `memcpy(buf + gap_start, buf + gap_end, N)`; `gap_start += N`; `gap_end += N`. **Move cursor back** by N: `gap_start -= N`; `gap_end -= N`; `memcpy(buf + gap_end, buf + gap_start, N)`. **`text_grow(buf)`**: `new_size = buf_size * 2`; `pages = new_size / 4096 + 1`; `new_buf = pmm_alloc_contiguous(pages)`; copy pre-gap + post-gap; update gap_end to maintain gap proportionally; `pmm_free_contiguous(old_buf, old_pages)`; update `buf_size`. Initial allocation: `pmm_alloc_contiguous(16)` = 64 KiB. **`text_get_line(buf, n, out, max)`**: iterate logical chars (skipping gap range); count `\n`; copy nth line bytes to `out`. **`text_load_file(buf, path)`**: `vfs_stat(path, &st)` → if `st.size >= buf_size`: `text_grow()` until enough; `vfs_read()` into `buf` before gap; `gap_start = st.size`; `gap_end = buf_size`. **`text_save_file(buf, path)`**: `vfs_create(path, VFS_TYPE_FILE)` + `vfs_write(pre-gap)` + `vfs_write(post-gap)`.

- [ ] Reconcile with the earlier sections of this file: §1 to §5 restate §6-§11 with other source file names and a different default extension list
- [ ] `typedef struct { uint8_t *buf; size_t buf_size; size_t gap_start; size_t gap_end; } text_buffer_t;`
- [ ] `text_buffer_t *text_buffer_create(void)` -- `pmm_alloc_contiguous(16)`; set `gap_start=0, gap_end=buf_size`
- [ ] `void text_buffer_destroy(text_buffer_t *tb)` -- `pmm_free_contiguous(buf, buf_size/4096)`; free struct
- [ ] `void text_grow(text_buffer_t *tb)` -- double PMM alloc; preserve content; free old
- [ ] `void text_insert(text_buffer_t *tb, uint8_t ch)` -- grow if gap empty; `buf[gap_start++] = ch`
- [ ] `void text_delete_back(text_buffer_t *tb)` -- `if (gap_start > 0) gap_start--`
- [ ] `void text_delete_forward(text_buffer_t *tb)` -- `if (gap_end < buf_size) gap_end++`
- [ ] `void text_move_to(text_buffer_t *tb, size_t new_gap_start)` -- memcpy to shift gap; update gap_end
- [ ] `size_t text_length(text_buffer_t *tb)` -- `buf_size - (gap_end - gap_start)`
- [ ] `uint8_t text_char_at(text_buffer_t *tb, size_t pos)` -- logical index (skip gap range)
- [ ] `int text_get_line(text_buffer_t *tb, int n, char *out, size_t max)` -- scan for nth `\n`; copy; return len
- [ ] `int text_line_count(text_buffer_t *tb)` -- scan all logical chars; count `\n`; return count + 1
- [ ] `int text_load_file(text_buffer_t *tb, const char *path)` -- grow until fits; `vfs_read()` before gap
- [ ] `int text_save_file(text_buffer_t *tb, const char *path)` -- write pre-gap + post-gap sequentially
- [ ] Commit: `"notepad: gap buffer -- PMM 64 KiB, O(1) insert/delete/move, line scan, load/save"`

## 7. Text Rendering & Cursor

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §2** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

Render visible lines via `ttf_draw_string(FONT_MONO)`. I-beam cursor blink 500 ms PIT toggle. Arrow keys, Home/End, Ctrl+Home/End, Page Up/Down. Selection highlight (inverted bg).

**Files:** `src/apps/notepad/notepad.c` (new), `include/apps/notepad.h` (new)

> [!NOTE]
> **Visible area**: `visible_rows = (window_h - menubar_h - statusbar_h) / cell_h`; render rows `[scroll_y .. scroll_y + visible_rows]`. For each row: `text_get_line(buf, scroll_y + i, line_buf, 256)` → `ttf_draw_string(s, mono_font, gutter_x + text_x, y + i*cell_h, line_buf, fg_color)`. **Word wrap**: when enabled: if `ttf_measure_width(f, line_buf) > text_area_w`: split at last space before overflow; render continued part on next rendered line (does not insert `\n`). **Cursor**: convert `cursor_line`/`cursor_col` to pixel via `ttf_measure_width(f, line_buf[0..cursor_col])`; draw 1 px vertical bar (I-beam) in `THEME_ACCENT`; blink: `(system_get_ticks() / (PIT_TARGET_FREQ/2)) % 2`. **Keyboard nav**: `WM_KEYDOWN(VK_LEFT)` → `cursor_col--` (wrap to prev line if col=0); `VK_RIGHT` → `cursor_col++` (wrap to next); `VK_UP/DOWN` → `cursor_line++/--`; `VK_HOME` → `cursor_col=0`; `VK_END` → `cursor_col=len`; `Ctrl+HOME` → `cursor_line=0, cursor_col=0`; `Ctrl+END` → last line/col; `VK_PGDN/PGUP` → `scroll_y += visible_rows`. After each move: clamp cursor; auto-scroll if cursor line outside `[scroll_y, scroll_y+visible_rows)`. **Selection render**: if `selection_start != selection_end`: draw accent `gfx_fill_rect` behind selected chars; render text over.

- [ ] `typedef struct notepad { text_buffer_t *buf; int cursor_line; int cursor_col; int scroll_y; char filepath[256]; int modified; int sel_start; int sel_end; int word_wrap; int font_px; ttf_font_t *font; int cell_h; int text_area_w; } notepad_t;`
- [ ] `void notepad_render(notepad_t *np, gfx_surface_t *s, int x, int y, int w, int h)` -- line loop; selection bg; glyphs; cursor
- [ ] `int notepad_cursor_to_offset(notepad_t *np)` -- convert `cursor_line/col` to linear buffer offset
- [ ] `void notepad_offset_to_cursor(notepad_t *np, size_t offset)` -- reverse scan; set `cursor_line/col`
- [ ] Keyboard handler: arrow keys, Home/End, Ctrl+Home/End, PgUp/Dn; auto-scroll to keep cursor visible
- [ ] Word wrap: `g_word_wrap` bool; wrap line in render (visual only, no buffer modification)
- [ ] I-beam cursor: 1 px rect `x = text_x + ttf_measure_width(font, line[0..col])`, height = `cell_h`; blink via ticks
- [ ] Any text key → `text_insert(buf, ch)` + `np->modified = 1` + record to undo stack (§8)
- [ ] Backspace → `text_delete_back()` + `modified` + undo record; Delete → `text_delete_forward()` + undo record
- [ ] `notepad_update_title()`: `wm_set_title(win, modified ? "* filename -- Notepad" : "filename -- Notepad")`
- [ ] Commit: `"notepad: text rendering -- visible line loop, ttf_draw_string, I-beam blink, keyboard nav, selection bg"`

## 8. Undo/Redo

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §3** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** n/a -- the undo/redo stack has no visual surface; its menu items are drawn in §9

200-action stack. Every insert/delete recorded as `(type, position, text)`. Ctrl+Z undoes; Ctrl+Y redoes.

**Files:** `src/apps/notepad/notepad.c` (extend), `include/apps/notepad.h` (extend)

> [!NOTE]
> Built before file menu (§9) to ensure all edit operations from day 1 are recorded. **Action struct**: `typedef struct { uint8_t type; size_t position; uint8_t text[64]; uint8_t text_len; } undo_action_t;` -- `type`: `UNDO_INSERT=0`, `UNDO_DELETE=1`. For block operations (paste, replace all): store full text block (if ≤ 64 bytes in struct; else allocate a small `kmalloc` buffer and store pointer -- flag `heap_alloc=1`). **Stack**: `undo_action_t g_undo_stack[200]`; `g_undo_top` (next push index); `g_undo_redo_top` (redo boundary). On new action: push to `g_undo_stack[g_undo_top % 200]`; `g_undo_top++`; `g_undo_redo_top = g_undo_top` (clears redo branch). **Ctrl+Z**: if `g_undo_top > 0`: `g_undo_top--`; apply inverse of action (INSERT → delete at pos; DELETE → insert at pos); move cursor to `position`. **Ctrl+Y**: if `g_undo_top < g_undo_redo_top`: replay action at `g_undo_top`; `g_undo_top++`.

- [ ] `typedef struct { uint8_t type; size_t position; uint8_t text[64]; uint8_t text_len; uint8_t heap_alloc; uint8_t *heap_text; } undo_action_t;`
- [ ] `#define UNDO_INSERT 0`, `UNDO_DELETE 1`; `#define UNDO_MAX 200`
- [ ] `undo_action_t g_undo_stack[UNDO_MAX]` + `g_undo_top` + `g_undo_redo_top` in notepad state
- [ ] `void undo_push(notepad_t *np, uint8_t type, size_t pos, const uint8_t *text, uint8_t len)` -- push; clear redo
- [ ] `void undo_apply(notepad_t *np)` -- Ctrl+Z: pop; invert; move cursor
- [ ] `void redo_apply(notepad_t *np)` -- Ctrl+Y: replay; advance pointer
- [ ] Every `text_insert()` call → `undo_push(INSERT, offset, ch, 1)` immediately after
- [ ] Every `text_delete_back/forward()` → `undo_push(DELETE, offset, deleted_char, 1)` with char retrieved before delete
- [ ] Block paste → single undo action with full paste text (heap-alloc if > 64 bytes)
- [ ] Commit: `"notepad: undo/redo -- 200-action stack, insert/delete, Ctrl+Z/Y, heap overflow for blocks"`

## 9. File Menu

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §4** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control), [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

`CTRL_MENUBAR`: File → New/Open/Save/Save As. `modified` flag on any edit. Window title with `*` suffix. "Save changes?" dialog on New/Open/close-if-modified.

**Files:** `src/apps/notepad/notepad.c` (extend)

> [!NOTE]
> Forward dep on `CTRL_MENUBAR` and `dialog_file_open/save()` from `08-graphics-ui/TODO-06` §4 and §8. Menu structure: **File** → `{ "New", notepad_new }, { "Open...", notepad_open }, { "---" }, { "Save", notepad_save }, { "Save As...", notepad_save_as }, { "---" }, { "Exit", notepad_exit }`. **Edit** → `{ "Undo", undo_apply }, { "Redo", redo_apply }, { "---" }, { "Cut", notepad_cut }, { "Copy", notepad_copy }, { "Paste", notepad_paste }, { "---" }, { "Find...", notepad_find_open }, { "Replace...", notepad_replace_open }, { "Go To...", notepad_goto_line }, { "---" }, { "Select All", notepad_select_all } `. **View** → `{ "Word Wrap", notepad_toggle_wordwrap }, { "---" }, { "Zoom In", notepad_zoom_in }, { "Zoom Out", notepad_zoom_out }, { "Restore Default Zoom", notepad_zoom_reset }`. **notepad_new()**: if `modified` → `dialog_input("Save changes to <filename>?", "YN")` → yes: `notepad_save()`; clear buffer; `filepath=""`. **notepad_save()**: if `filepath == ""` → `notepad_save_as()`; else: `text_save_file(buf, filepath)`. **notepad_open()**: "Save changes?" if modified; `dialog_file_open("Open", "Text Files (*.txt)\0*.txt\0All Files\0*.*\0")` → path → `text_load_file(buf, path)`.

- [ ] `notepad_setup_menubar(notepad_t *np)` -- register `CTRL_MENUBAR` with File/Edit/View menus + callbacks
- [ ] `void notepad_new(notepad_t *np)` -- save-changes prompt if `modified`; clear buffer; reset filepath
- [ ] `void notepad_open(notepad_t *np)` -- save-changes prompt; `dialog_file_open()`; `text_load_file()`; update title
- [ ] `void notepad_save(notepad_t *np)` -- if no filepath → `notepad_save_as()`; else: `text_save_file()`; `modified=0`
- [ ] `void notepad_save_as(notepad_t *np)` -- `dialog_file_save()`; update `filepath`; `text_save_file()`; `modified=0`
- [ ] `void notepad_exit(notepad_t *np)` -- save-changes prompt if modified; `wm_close_window()`
- [ ] `WM_CLOSE` event handler → `notepad_exit()` (protects unsaved changes on window ×)
- [ ] `notepad_update_title(np)` called after every `modified` state change and filepath change
- [ ] Commit: `"notepad: file menu -- New/Open/Save/SaveAs via CTRL_MENUBAR + dialogs, modified flag, * title, exit guard"`

## 10. Editing Features

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §5** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#scroll-bar`](../../docs/design/controls.md#scroll-bar), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

Mouse click sets cursor. Click+drag selection. Ctrl+A. Ctrl+C/X/V clipboard. Vertical scrollbar. Word wrap toggle (View menu). Status bar: `Ln N, Col N | UTF-8 | CRLF`.

**Files:** `src/apps/notepad/notepad.c` (extend)

> [!NOTE]
> **Mouse click**: `WM_MOUSE_DOWN(x, y)` in text area → compute line = `(y - text_top) / cell_h + scroll_y`; col = binary-search `ttf_measure_width` increments to find closest char boundary; `text_move_to(buf, cursor_offset)`. **Click+drag**: `WM_MOUSE_DOWN` → `sel_start = cursor_offset`; `WM_MOUSE_MOVE` (btn down) → `sel_end = cursor_offset`; render selection (inverted bg across range). **Ctrl+A**: `sel_start = 0`; `sel_end = text_length(buf)`. **Ctrl+C**: copy selected chars to `clipboard_set(CLIP_TEXT, sel_text, sel_len)` (no undo needed). **Ctrl+X**: same as copy; then delete selection range + undo record for the block. **Ctrl+V**: `clipboard_get(CLIP_TEXT, paste_buf, 4096)`; if selection: delete selection first; `text_insert()` each char; single block undo record. **Scrollbar**: 8 px right edge; `thumb_h = (visible_rows * area_h) / total_lines`; `thumb_y = (scroll_y * area_h) / total_lines`; click+drag → `scroll_y`. **Word wrap**: toggle `np->word_wrap`; `Registry_SetValue(HKCU\\...\\Notepad\\WordWrap, val)`; no buffer change -- visual line wrapping only. **Status bar** (`CTRL_STATUSBAR`): update text `"Ln %d, Col %d | UTF-8 | CRLF"` on every cursor move. **Right-click context menu**: `context_menu_show()` with Cut/Copy/Paste/Select All.

- [ ] Mouse click → pixel-to-line/col conversion; `text_move_to()`; clear selection
- [ ] Mouse drag → update `sel_end`; render inverted selection; auto-scroll if dragged to edge
- [ ] `void notepad_select_all(notepad_t *np)` -- `sel_start=0`; `sel_end=text_length(buf)`
- [ ] `void notepad_copy(notepad_t *np)` -- collect sel chars → `clipboard_set(CLIP_TEXT, ...)`
- [ ] `void notepad_cut(notepad_t *np)` -- copy + delete selection + undo record
- [ ] `void notepad_paste(notepad_t *np)` -- `clipboard_get(CLIP_TEXT)`; delete sel if any; insert chars; block undo
- [ ] Scrollbar: draw + click+drag handler; mouse wheel → `scroll_y += 3`; clamp
- [ ] `CTRL_STATUSBAR` update: `"Ln %d, Col %d | UTF-8 | CRLF"` on cursor change
- [ ] Right-click context menu: `context_menu_show()` with Cut/Copy/Paste/Select All entries
- [ ] Commit: `"notepad: editing -- mouse click/drag select, Ctrl+A/C/X/V, scrollbar, word wrap, status bar"`

## 11. Find & Replace

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §6** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`controls.md#text-box-password-box-and-search-box`](../../docs/design/controls.md#text-box-password-box-and-search-box), [`controls.md#dialog`](../../docs/design/controls.md#dialog)

Ctrl+F: find toolbar slides in below menu bar (search field, Prev/Next, close ×). Ctrl+H: find+replace dialog (case-sensitive toggle, Replace, Replace All). Ctrl+G: Go to line. Accent-colored match highlight.

**Files:** `src/apps/notepad/notepad.c` (extend)

> [!NOTE]
> **Find toolbar** (not a dialog -- slides in below the menu bar, 40 px high to hold 32 px controls per `docs/design/controls.md`): `CTRL_TEXTBOX` + "‹ Prev" + "Next ›" buttons + "✕" close; no modal blocking. `find_matches[]` array of match offsets (`uint32_t matches[512]`; `int match_count`). `notepad_find_all(query)` scans logical buffer; for each match: store start offset. Render: during `notepad_render()`: for each visible char that falls within a match range: fill cell bg with `theme_get(THEME_ACCENT)` at 40% alpha before drawing glyph. **Prev/Next**: advance `g_current_match`; `notepad_offset_to_cursor()` for that match; scroll viewport to show cursor. **Ctrl+H dialog** (`dialog_input`-style, 380×200 modal): search field, replace field, case-sensitive checkbox (`CTRL_CHECKBOX`), Replace button (replace current match → find next), Replace All (iterate all matches, replace in reverse order to preserve offsets). **Replace All**: replace from last match to first (reverse) to avoid index shifts. **Ctrl+G**: `dialog_input("Go to line:", "", buf, 8)` → `n = atoi(buf)`; clamp to `[1, line_count]`; `text_move_to()` to start of that line.

- [ ] `uint32_t g_matches[512]`; `int g_match_count`; `int g_current_match`
- [ ] `void notepad_find_all(notepad_t *np, const char *query, int case_sensitive)` -- scan; fill `g_matches[]`
- [ ] Find toolbar: 40 px strip below the menu bar holding 32 px controls; `CTRL_TEXTBOX` + Prev/Next/× buttons; on text change → `notepad_find_all()`
- [ ] Match highlight in render: check if char offset falls in `g_matches[i]..g_matches[i]+qlen` → accent bg
- [ ] Prev/Next: cycle `g_current_match`; `notepad_offset_to_cursor()`; scroll to cursor
- [ ] `void notepad_replace_one(notepad_t *np, const char *replacement)` -- delete match; insert replacement; undo record
- [ ] `void notepad_replace_all(notepad_t *np, const char *query, const char *replacement, int cs)` -- reverse iteration; undo block record
- [ ] Ctrl+G: `dialog_input("Go to line:", ...)` → compute offset of line start; `text_move_to()`; scroll
- [ ] `WM_KEYDOWN(Ctrl+F)` → show find toolbar; `WM_KEYDOWN(Ctrl+H)` → show replace dialog; `WM_KEYDOWN(Escape)` in find toolbar → hide toolbar
- [ ] Commit: `"notepad: find & replace -- find toolbar, Prev/Next, match highlight, Replace/Replace All, Go to Line"`

## 12. Stretch Features

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-10-notepad.md` §7** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

Line number gutter (12 px, right-aligned, muted color). Tokenizer-based syntax highlight for `.c/.h/.asm/.md` (keywords/strings/comments via theme tokens). Ctrl++/−/0 font zoom.

**Files:** `src/apps/notepad/notepad.c` (extend), `src/apps/notepad/syntax.c` (new)

> [!NOTE]
> **Line numbers**: when enabled (View → Line Numbers toggle): render a 48 px left gutter; for each visible line: `ttf_draw_string(s, font, gutter_x, row_y, line_num_str, text_secondary token)`; right-aligned (measure width, right-pad). Adjust `text_x` to `gutter_w + 4`. **Syntax highlight**: `syntax_highlight(line_buf, tokens_out, max_tokens)` for file extensions `.c/.h` and `.asm` and `.md`. Returns array of `syntax_token_t { int start; int len; uint32_t color; }`. Render: draw char spans in `token.color` instead of default fg. C keywords: `if/else/while/for/return/int/void/static/const/struct/typedef/uint32_t/...` → `THEME_KEYWORD`. Strings `"..."` → `THEME_STRING`. Line comments `//...` + block `/* */` → `THEME_COMMENT`. Numbers → `THEME_NUMBER`. Markdown: `# ` header → bold accent; `**bold**` → bold; `` `code` `` → monospace background. Assembly: `mov/jmp/call/push/pop/ret/...` → keyword color; registers `eax/rbx/...` → accent; `;` comment → comment color. **Font zoom**: `Ctrl++` → `np->font_px = min(np->font_px + 2, 48)`; `Ctrl+-` → `max(font_px - 2, 8)`; `Ctrl+0` → reset to Registry default; `ttf_get(FONT_MONO, np->font_px)` → update `np->font`; recalc `cell_h`; `notepad_resize()`.

- [ ] Line number gutter: 48 px left strip; right-aligned `Ln %d` per visible line; `text_secondary` colour (`THEME_DARK_TEXT_SECONDARY` / `THEME_LIGHT_TEXT_SECONDARY`)
- [ ] `void syntax_highlight(const char *line, const char *ext, syntax_token_t *tokens, int *count)` -- dispatch by `ext`
- [ ] `syntax_highlight_c(line, tokens, count)` -- keyword/string/comment/number tokenizer (no full parser)
- [ ] `syntax_highlight_asm(line, tokens, count)` -- instruction keywords, registers, `;` comments
- [ ] `syntax_highlight_md(line, tokens, count)` -- `#` headers, `**bold**`, `` ` ``code`` ` ``
- [ ] Render: per char span: use `token.color`; draw segment via `ttf_draw_string()` for each token range
- [ ] Font zoom: `Ctrl++/-/0` → adjust `font_px`; `ttf_get(FONT_MONO, font_px)` → `cell_h = font_px + 2`; re-render
- [ ] Registry persist: `FontSize` read/write on zoom change
- [ ] `View → Line Numbers` toggle: `np->show_line_numbers` bool; Registry persist
- [ ] Commit: `"notepad: stretch -- line number gutter, C/ASM/Markdown syntax highlight, Ctrl++ font zoom"`

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
