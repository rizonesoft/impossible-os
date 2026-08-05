---
schema_version: 1
id: notepad-shell-host
domain: 09-desktop-shell
status: active
title: "TODO-10 -- Notepad Text Editor"
---

# TODO-10 -- Notepad Text Editor

> **Goal:** Build Notepad -- the primary text editor and fallback file association for `.txt`/`.md`/`.c`/`.h`/`.asm` -- as the first real app that exercises the full widget stack. Built on a PMM-backed gap buffer with O(1) insert/delete, full keyboard + mouse editing, clipboard integration, find & replace, undo/redo, and stretch features (line numbers, syntax highlighting, font zoom).

> [!IMPORTANT]
> **Already exists**: `ttf_draw_string(s, f, x, y, text, color)` + `ttf_measure_width(f, text)` + `ttf_get(slot, px)` in `font_mgr.h`. `FONT_MONO=2` (Cascadia Code). `vfs_open/read/write`, `vfs_stat`, `vfs_create` for file I/O. `pmm_alloc_contiguous(pages)` + `pmm_free_contiguous(addr, pages)` for large buffers. `gfx_fill_rect()`, `gfx_surface_t`, `system_get_ticks()` (cursor blink). **Forward deps**: `CTRL_MENUBAR` (TODO-05 §2), `CTRL_STATUSBAR` (TODO-05 §3), `dialog_file_open/save()` (TODO-05 §3), `dialog_input()` (TODO-05 §2) for Go to Line. `clipboard_set/get(CLIP_TEXT)` (TODO-01 §1). **No `src/apps/` yet** -- TODO-09 creates it; Notepad lives at `src/apps/notepad/notepad.c`. **Missing**: gap buffer, text rendering loop, ANSI parser, undo stack, find/replace. Complete sections in order: gap buffer → text rendering → undo/redo → file menu → editing features → find & replace → stretch features.

## Inputs

- `include/font_mgr.h` -- `ttf_get()`, `ttf_draw_string()`, `ttf_measure_width()`, `FONT_MONO` -- §2 line rendering, §7 font zoom
- `include/gfx.h` -- `gfx_fill_rect()`, `gfx_surface_t` -- §2 selection highlight, §7 line number gutter
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()`, `pmm_free_contiguous()` -- §1 gap buffer allocation + doubling
- `include/kernel/fs/vfs.h` -- `vfs_open/read/write`, `vfs_create`, `vfs_stat` -- §3 file load/save
- `include/kernel/drivers/pit.h` -- `system_get_ticks()`, `PIT_TARGET_FREQ` -- §2 cursor blink 500 ms toggle
- `include/desktop/controls.h` (TODO-05 §2/§3) -- `CTRL_MENUBAR`, `CTRL_STATUSBAR`, `dialog_file_open/save()`, `dialog_input()` -- §2 file menu
- `include/kernel/clipboard.h` (TODO-01 §1) -- `clipboard_set/get(CLIP_TEXT)` -- §4 Ctrl+C/X/V
- `include/desktop/wm.h` -- `wm_create_window()`, `WM_KEYDOWN`, `WM_MOUSE_DOWN/MOVE` -- §2 keyboard + mouse events
- `include/desktop/context_menu.h` (TODO-07 §1) -- `context_menu_show()` -- §4 right-click context menu
- → XREF: `08-graphics-ui/TODO-06-widget-dialogs.md §2` -- `CTRL_MENUBAR`, `dialog_file_open/save` must be complete before §2 File Menu; `CTRL_STATUSBAR` before §3
- → XREF: `09-desktop-shell/TODO-02-file-associations-resources.md §1` -- `.txt` default file association registered there; Notepad is the launch target
- → XREF: `08-graphics-ui/TODO-03-theme-system.md` -- `theme_get(THEME_ACCENT)` for selection bg, match highlights, syntax colors

## Outcome

- PMM-backed gap buffer (`text_buffer_t`): O(1) insert/delete; doubles PMM allocation on exhaustion; `text_load/save_file()`.
- Line-by-line render of visible window with `ttf_draw_string()`; I-beam cursor blink; full keyboard navigation.
- 200-action undo/redo stack recording every insert/delete.
- File menu (New/Open/Save/Save As) via `CTRL_MENUBAR` + file dialogs; `modified` flag; "Save changes?" on exit.
- Mouse click-to-cursor, click+drag selection (inverted bg), Ctrl+A/C/X/V, scrollbar, word wrap toggle, status bar.
- Ctrl+F find toolbar (match highlight, Prev/Next); Ctrl+H find+replace dialog; Ctrl+G Go to line.
- Stretch: line number gutter, `.c/.h/.asm/.md` tokenizer-based syntax highlight, Ctrl++/−/0 font zoom.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                      | Depends On                                                                              | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------- | :----: |
| ⭐  |   1   | §1 Gap buffer -- `text_buffer_t`, O(1) insert/delete, PMM backing, doubling, load/save           | `pmm_alloc_contiguous` (exists); `vfs_read/write` (exists)                             |  [ ]   |
| 💎  |   2   | §2 Text rendering -- visible-line loop, `ttf_draw_string`, I-beam blink, keyboard nav            | §1 gap buffer; `ttf_draw_string` + `ttf_measure_width` (exist); `system_get_ticks()`  |  [ ]   |
| 💎  |   3   | §6 Undo/redo -- 200-action stack, every insert/delete logged, Ctrl+Z/Y                           | §1 gap buffer (must be wired from day 1); §2 rendering (visible state after undo)     |  [ ]   |
| 💎  |   4   | §3 File menu -- `CTRL_MENUBAR`, New/Open/Save/Save As, `modified` flag, "Save changes?" dialog   | §1 gap buffer (load/save); `CTRL_MENUBAR` + `dialog_file_open/save` (TODO-05)         |  [ ]   |
| 💎  |   5   | §4 Editing features -- mouse cursor, click+drag select, Ctrl+A/C/X/V, scrollbar, word wrap, status bar | §1-§3; `clipboard_set/get(CLIP_TEXT)` (TODO-01); `CTRL_STATUSBAR` (TODO-05)    |  [ ]   |
| 💎  |   6   | §5 Find & Replace -- Ctrl+F find toolbar, Ctrl+H dialog, match highlight, Ctrl+G Go to line      | §1-§4; `dialog_input()` (TODO-05); §3 selection (Replace All uses select + paste)     |  [ ]   |
| 💎  |   7   | §7 Stretch -- line number gutter, `.c/.h/.asm/.md` syntax highlight, Ctrl++/−/0 font zoom        | §1-§6; `theme_get(THEME_ACCENT)` (TODO-01)                                             |  [ ]   |

---

## 1. Gap Buffer `[Opus]`

`text_buffer_t` (buf, buf_size, gap_start, gap_end). PMM 64 KiB initial allocation. `text_insert/delete` O(1). `text_move_cursor` shifts gap via memcpy. `text_get_line/line_count`. `text_load/save_file`.

**Files:** `src/apps/notepad/text_buffer.c` (new), `include/apps/text_buffer.h` (new)

> [!NOTE]
> `[Opus]` due to novel data structure design (no prior Impossible OS text editor precedent), non-trivial PMM doubling strategy (allocate 2× new buffer + memcpy pre-gap + memcpy post-gap + `pmm_free_contiguous` old), and correctness requirements (gap start/end accounting must be exact or all text operations produce corruption). **Buffer layout**: `[pre-gap text][--- gap ---][post-gap text]` within `buf[0..buf_size-1]`. `gap_start` = end of pre-gap content; `gap_end` = start of post-gap content; free gap space = `gap_end - gap_start`. **Insert** `ch` at cursor: if `gap_start == gap_end` → `text_grow(buf)` first; `buf[gap_start++] = ch`. **Delete** backwards: if `gap_start > 0` → `gap_start--` (just shrink pre-gap). **Move cursor forward** by N: `memcpy(buf + gap_start, buf + gap_end, N)`; `gap_start += N`; `gap_end += N`. **Move cursor back** by N: `gap_start -= N`; `gap_end -= N`; `memcpy(buf + gap_end, buf + gap_start, N)`. **`text_grow(buf)`**: `new_size = buf_size * 2`; `pages = new_size / 4096 + 1`; `new_buf = pmm_alloc_contiguous(pages)`; copy pre-gap + post-gap; update gap_end to maintain gap proportionally; `pmm_free_contiguous(old_buf, old_pages)`; update `buf_size`. Initial allocation: `pmm_alloc_contiguous(16)` = 64 KiB. **`text_get_line(buf, n, out, max)`**: iterate logical chars (skipping gap range); count `\n`; copy nth line bytes to `out`. **`text_load_file(buf, path)`**: `vfs_stat(path, &st)` → if `st.size >= buf_size`: `text_grow()` until enough; `vfs_read()` into `buf` before gap; `gap_start = st.size`; `gap_end = buf_size`. **`text_save_file(buf, path)`**: `vfs_create(path, VFS_TYPE_FILE)` + `vfs_write(pre-gap)` + `vfs_write(post-gap)`.

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

## 2. Text Rendering & Cursor `[Sonnet]`

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
- [ ] Any text key → `text_insert(buf, ch)` + `np->modified = 1` + record to undo stack (§3)
- [ ] Backspace → `text_delete_back()` + `modified` + undo record; Delete → `text_delete_forward()` + undo record
- [ ] `notepad_update_title()`: `wm_set_title(win, modified ? "* filename -- Notepad" : "filename -- Notepad")`
- [ ] Commit: `"notepad: text rendering -- visible line loop, ttf_draw_string, I-beam blink, keyboard nav, selection bg"`

## 3. Undo/Redo `[Sonnet]`

200-action stack. Every insert/delete recorded as `(type, position, text)`. Ctrl+Z undoes; Ctrl+Y redoes.

**Files:** `src/apps/notepad/notepad.c` (extend), `include/apps/notepad.h` (extend)

> [!NOTE]
> Built before file menu (§4) to ensure all edit operations from day 1 are recorded. **Action struct**: `typedef struct { uint8_t type; size_t position; uint8_t text[64]; uint8_t text_len; } undo_action_t;` -- `type`: `UNDO_INSERT=0`, `UNDO_DELETE=1`. For block operations (paste, replace all): store full text block (if ≤ 64 bytes in struct; else allocate a small `kmalloc` buffer and store pointer -- flag `heap_alloc=1`). **Stack**: `undo_action_t g_undo_stack[200]`; `g_undo_top` (next push index); `g_undo_redo_top` (redo boundary). On new action: push to `g_undo_stack[g_undo_top % 200]`; `g_undo_top++`; `g_undo_redo_top = g_undo_top` (clears redo branch). **Ctrl+Z**: if `g_undo_top > 0`: `g_undo_top--`; apply inverse of action (INSERT → delete at pos; DELETE → insert at pos); move cursor to `position`. **Ctrl+Y**: if `g_undo_top < g_undo_redo_top`: replay action at `g_undo_top`; `g_undo_top++`.

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

## 4. File Menu `[Sonnet]`

`CTRL_MENUBAR`: File → New/Open/Save/Save As. `modified` flag on any edit. Window title with `*` suffix. "Save changes?" dialog on New/Open/close-if-modified.

**Files:** `src/apps/notepad/notepad.c` (extend)

> [!NOTE]
> Forward dep on `CTRL_MENUBAR` and `dialog_file_open/save()` from TODO-05. Menu structure: **File** → `{ "New", notepad_new }, { "Open...", notepad_open }, { "---" }, { "Save", notepad_save }, { "Save As...", notepad_save_as }, { "---" }, { "Exit", notepad_exit }`. **Edit** → `{ "Undo", undo_apply }, { "Redo", redo_apply }, { "---" }, { "Cut", notepad_cut }, { "Copy", notepad_copy }, { "Paste", notepad_paste }, { "---" }, { "Find...", notepad_find_open }, { "Replace...", notepad_replace_open }, { "Go To...", notepad_goto_line }, { "---" }, { "Select All", notepad_select_all } `. **View** → `{ "Word Wrap", notepad_toggle_wordwrap }, { "---" }, { "Zoom In", notepad_zoom_in }, { "Zoom Out", notepad_zoom_out }, { "Restore Default Zoom", notepad_zoom_reset }`. **notepad_new()**: if `modified` → `dialog_input("Save changes to <filename>?", "YN")` → yes: `notepad_save()`; clear buffer; `filepath=""`. **notepad_save()**: if `filepath == ""` → `notepad_save_as()`; else: `text_save_file(buf, filepath)`. **notepad_open()**: "Save changes?" if modified; `dialog_file_open("Open", "Text Files (*.txt)\0*.txt\0All Files\0*.*\0")` → path → `text_load_file(buf, path)`.

- [ ] `notepad_setup_menubar(notepad_t *np)` -- register `CTRL_MENUBAR` with File/Edit/View menus + callbacks
- [ ] `void notepad_new(notepad_t *np)` -- save-changes prompt if `modified`; clear buffer; reset filepath
- [ ] `void notepad_open(notepad_t *np)` -- save-changes prompt; `dialog_file_open()`; `text_load_file()`; update title
- [ ] `void notepad_save(notepad_t *np)` -- if no filepath → `notepad_save_as()`; else: `text_save_file()`; `modified=0`
- [ ] `void notepad_save_as(notepad_t *np)` -- `dialog_file_save()`; update `filepath`; `text_save_file()`; `modified=0`
- [ ] `void notepad_exit(notepad_t *np)` -- save-changes prompt if modified; `wm_close_window()`
- [ ] `WM_CLOSE` event handler → `notepad_exit()` (protects unsaved changes on window ×)
- [ ] `notepad_update_title(np)` called after every `modified` state change and filepath change
- [ ] Commit: `"notepad: file menu -- New/Open/Save/SaveAs via CTRL_MENUBAR + dialogs, modified flag, * title, exit guard"`

## 5. Editing Features `[Sonnet]`

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

## 6. Find & Replace `[Sonnet]`

Ctrl+F: find toolbar slides in below menu bar (search field, Prev/Next, close ×). Ctrl+H: find+replace dialog (case-sensitive toggle, Replace, Replace All). Ctrl+G: Go to line. Accent-colored match highlight.

**Files:** `src/apps/notepad/notepad.c` (extend)

> [!NOTE]
> **Find toolbar** (not a dialog -- slides in below menubar, 28 px high): `CTRL_TEXTBOX` + "‹ Prev" + "Next ›" buttons + "✕" close; no modal blocking. `find_matches[]` array of match offsets (`uint32_t matches[512]`; `int match_count`). `notepad_find_all(query)` scans logical buffer; for each match: store start offset. Render: during `notepad_render()`: for each visible char that falls within a match range: fill cell bg with `theme_get(THEME_ACCENT)` at 40% alpha before drawing glyph. **Prev/Next**: advance `g_current_match`; `notepad_offset_to_cursor()` for that match; scroll viewport to show cursor. **Ctrl+H dialog** (`dialog_input`-style, 380×200 modal): search field, replace field, case-sensitive checkbox (`CTRL_CHECKBOX`), Replace button (replace current match → find next), Replace All (iterate all matches, replace in reverse order to preserve offsets). **Replace All**: replace from last match to first (reverse) to avoid index shifts. **Ctrl+G**: `dialog_input("Go to line:", "", buf, 8)` → `n = atoi(buf)`; clamp to `[1, line_count]`; `text_move_to()` to start of that line.

- [ ] `uint32_t g_matches[512]`; `int g_match_count`; `int g_current_match`
- [ ] `void notepad_find_all(notepad_t *np, const char *query, int case_sensitive)` -- scan; fill `g_matches[]`
- [ ] Find toolbar: 28 px strip below menubar; `CTRL_TEXTBOX` + Prev/Next/× buttons; on text change → `notepad_find_all()`
- [ ] Match highlight in render: check if char offset falls in `g_matches[i]..g_matches[i]+qlen` → accent bg
- [ ] Prev/Next: cycle `g_current_match`; `notepad_offset_to_cursor()`; scroll to cursor
- [ ] `void notepad_replace_one(notepad_t *np, const char *replacement)` -- delete match; insert replacement; undo record
- [ ] `void notepad_replace_all(notepad_t *np, const char *query, const char *replacement, int cs)` -- reverse iteration; undo block record
- [ ] Ctrl+G: `dialog_input("Go to line:", ...)` → compute offset of line start; `text_move_to()`; scroll
- [ ] `WM_KEYDOWN(Ctrl+F)` → show find toolbar; `WM_KEYDOWN(Ctrl+H)` → show replace dialog; `WM_KEYDOWN(Escape)` in find toolbar → hide toolbar
- [ ] Commit: `"notepad: find & replace -- find toolbar, Prev/Next, match highlight, Replace/Replace All, Go to Line"`

## 7. Stretch Features `[Sonnet]`

Line number gutter (12 px, right-aligned, muted color). Tokenizer-based syntax highlight for `.c/.h/.asm/.md` (keywords/strings/comments via theme tokens). Ctrl++/−/0 font zoom.

**Files:** `src/apps/notepad/notepad.c` (extend), `src/apps/notepad/syntax.c` (new)

> [!NOTE]
> **Line numbers**: when enabled (View → Line Numbers toggle): render a 48 px left gutter; for each visible line: `ttf_draw_string(s, font, gutter_x, row_y, line_num_str, THEME_TEXT_MUTED)`; right-aligned (measure width, right-pad). Adjust `text_x` to `gutter_w + 4`. **Syntax highlight**: `syntax_highlight(line_buf, tokens_out, max_tokens)` for file extensions `.c/.h` and `.asm` and `.md`. Returns array of `syntax_token_t { int start; int len; uint32_t color; }`. Render: draw char spans in `token.color` instead of default fg. C keywords: `if/else/while/for/return/int/void/static/const/struct/typedef/uint32_t/...` → `THEME_KEYWORD`. Strings `"..."` → `THEME_STRING`. Line comments `//...` + block `/* */` → `THEME_COMMENT`. Numbers → `THEME_NUMBER`. Markdown: `# ` header → bold accent; `**bold**` → bold; `` `code` `` → monospace background. Assembly: `mov/jmp/call/push/pop/ret/...` → keyword color; registers `eax/rbx/...` → accent; `;` comment → comment color. **Font zoom**: `Ctrl++` → `np->font_px = min(np->font_px + 2, 48)`; `Ctrl+-` → `max(font_px - 2, 8)`; `Ctrl+0` → reset to Registry default; `ttf_get(FONT_MONO, np->font_px)` → update `np->font`; recalc `cell_h`; `notepad_resize()`.

- [ ] Line number gutter: 48 px left strip; right-aligned `Ln %d` per visible line; `THEME_TEXT_MUTED` color
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


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| ⭐   | Gap buffer                               | ✅ Notepad: undisclosed internal (likely piece | ✅ gedit: piece table; Kate: custom       | ⬜ §1 -- `⭐` explicit gap buffer design   |
| 💎   | Text rendering                           | ✅ Notepad: hardware-accelerated Direct2D rendering; GDI | ✅ gedit/Kate: Pango + Cairo; GLib;       | ⬜ §2 -- `ttf_draw_string()` per line; software rasterizer |
| 💎   | Undo/redo -- 200-action stack, Ctrl+Z/Y  | ✅ Notepad: unlimited undo since Win10;   | ✅ All editors: undo/redo; gedit unlimited; | ⬜ §3 -- 200-action ring buffer; insert + |
| 💎   | File menu                                | ✅ Notepad: full file menu; "Save         | ✅ gedit/nano: full file menu; modified   | ⬜ §4 -- `CTRL_MENUBAR` + `dialog_file_open/save()`; `modified` flag |
| 💎   | Editing                                  | ✅ Notepad: full mouse select; clipboard; | ✅ All editors: mouse; clipboard; status  | ⬜ §5 -- pixel-to-col binary search; inverted selection |
| 💎   | Find & Replace                           | ✅ Notepad: inline find toolbar; Ctrl+H;  | ✅ gedit: find toolbar; replace dialog;   | ⬜ §6 -- find toolbar (non-modal, slides in) |
| 💎   | Syntax highlight -- C/ASM/Markdown tokenizer-based | ✅ Notepad: no syntax highlighting (it's  | ✅ gedit: GtkSourceView with full syntax; | ⬜ §7 -- `⭐` more than Windows Notepad    |
| 💎   | Font zoom -- Ctrl++/−/0, Registry persist | ✅ Notepad: Ctrl++ zoom (since Win10      | ✅ gedit: View → Zoom; font               | ⬜ §7 -- `ttf_get(FONT_MONO, new_px)` hot-reload; `cell_h` recalc |

> **After §1–§7:** Impossible OS has a text editor that exceeds Windows Notepad (syntax highlighting, line numbers) while using a textbook gap-buffer data structure with PMM flat allocation. The `⭐` differentiators: the gap buffer eliminates heap fragmentation for large text files; syntax highlighting in Notepad is a feature Windows Notepad still doesn't have; and the find toolbar is non-modal (slides in below the menu bar, never blocking text).

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Open Notepad → blank document; type "Hello World" → text appears; gap_start advances
- [ ] Backspace → last char removed; Ctrl+Z → char restored; Ctrl+Y → removed again
- [ ] Arrow keys move cursor; End → cursor at line end; Ctrl+End → last line; PgDn → scrolls down
- [ ] File → Open → select a `.txt` file → contents loaded; cursor at position 0
- [ ] Edit file → title shows `* filename.txt -- Notepad`; File → Save → `*` removed
- [ ] Close window without saving → "Save changes?" dialog; Cancel → window stays open
- [ ] Ctrl+A → all selected (inverted bg); Ctrl+C → clipboard has text; Ctrl+V → text pasted
- [ ] Ctrl+F → find toolbar appears; type "World" → match highlighted in accent color; Next cycles to next match
- [ ] Ctrl+H → replace dialog; Replace All "World" → "Earth" → all occurrences replaced
- [ ] Ctrl+G → Go to line: enter "5" → cursor jumps to line 5
- [ ] View → Line Numbers → line gutter appears with right-aligned numbers
- [ ] Open `kernel.c` → C keywords colored; strings colored; `//` comments in comment color
- [ ] Ctrl++ → font size increases; Ctrl+0 → reset to default
- [ ] Commit: `"notepad: text editor -- all sections complete"`
