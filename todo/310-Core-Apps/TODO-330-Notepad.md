# P0505 — Notepad

> **Goal:** Text editor with gap buffer, ANSI rendering, file operations,
> text selection, and editing features.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Notepad

### 1.1 Text Buffer (Gap Buffer)

**Prompt:** The gap buffer is the most efficient data structure for text editing — insertions and deletions at the cursor are O(1). The buffer has a "gap" (unused region) that sits at the cursor position. `text_insert` drops a character into the gap (gap shrinks). `text_delete` expands the gap to "eat" the character before cursor. `text_move_cursor` shifts the gap to the new position by copying characters. Pre-allocate a reasonable buffer size (64K) and grow by doubling when the gap shrinks to zero. `text_get_line(n)` scans for the nth newline, returning a pointer and length. After completing all items,sh clean`, and commit as `"apps: Notepad gap buffer"`.
- [ ] Create `src/apps/notepad/notepad.c`
- [ ] Implement gap buffer: `struct text_buffer` (buf, buf_size, gap_start, gap_end)
- [ ] `text_insert(buf, char)` — insert at cursor (gap start)
- [ ] `text_delete(buf)` — delete char before cursor
- [ ] `text_move_cursor(buf, direction)` — move gap
- [ ] `text_get_line(buf, line_num)` — return line contents
- [ ] `text_line_count(buf)` — count newlines
- [ ] Commit: `"apps: Notepad gap buffer"`

### 1.2 Text Rendering & Cursor

**Prompt:** Render visible lines from the gap buffer using `font_draw_string()` (Selawik or user-configured font from Registry). Track cursor_line and cursor_col. The I-beam cursor blinks at the insertion point (500ms toggle). Arrow keys move the cursor, adjusting line/col and scrolling the viewport if necessary. Home/End jump to line start/end. Ctrl+Home/End jump to file start/end. The scroll position (`scroll_y`) determines which line is at the top of the visible area. After completing all items,sh clean`, and commit as `"apps: Notepad text rendering"`.
- [ ] Define `struct notepad` state (text, cursor_line/col, scroll_y, filepath, modified)
- [ ] Render visible lines from gap buffer using `font_draw_string()`
- [ ] Draw blinking I-beam cursor at insertion point
- [ ] Arrow keys: move cursor left/right/up/down
- [ ] Home/End: jump to line start/end
- [ ] Ctrl+Home/End: jump to file start/end
- [ ] Commit: `"apps: Notepad text rendering"`

### 1.3 File Menu

**Prompt:** The File menu uses the menu bar widget from §1.2. File → New clears the buffer and resets the filepath. File → Open invokes `ui_dialog_open()` from §1.3, loads the file via VFS `read()` into the gap buffer. File → Save writes the buffer to the current filepath via VFS `write()`. File → Save As invokes `ui_dialog_save()`. Track a `modified` flag — set on any edit, cleared on save. If modified, show "Do you want to save changes?" dialog (from §1.3) before New/Open/Close. The window title shows "filename.txt — Notepad" (with asterisk if modified). After completing all items,sh clean`, and commit as `"apps: Notepad file operations"`.
- [ ] Menu bar: File, Edit, View, Help
- [ ] File → New: clear buffer, reset filepath
- [ ] File → Open: `ui_dialog_open()` → load file via VFS
- [ ] File → Save: write buffer to current filepath via VFS
- [ ] File → Save As: `ui_dialog_save()` → choose path + save
- [ ] "Unsaved changes" warning on close or New if `modified` flag set
- [ ] Window title: "filename.txt — Notepad" (asterisk if modified)
- [ ] Commit: `"apps: Notepad file operations"`

### 1.4 Editing Features

**Prompt:** Mouse click sets the cursor position by calculating which line/col the click coordinates map to. Text selection: click + drag or Shift+Arrow marks a start/end range, rendered with inverted colors. Ctrl+A selects all. Cut/Copy/Paste uses the system clipboard (Phase 03 §3). Vertical scrollbar for long files (from §1.1). Word wrap toggle in the View menu — when enabled, lines wrap at the window width without inserting newlines. Status bar shows line:col, encoding (UTF-8), and line ending type (CRLF/LF). Stretch goals: Find/Replace (Ctrl+F/H) with a search bar, Undo/Redo (Ctrl+Z/Y) with an action stack, line numbers in a left gutter, syntax highlighting for .c/.h files. After completing all items,sh clean`, and commit as `"apps: Notepad editing features"`.
- [ ] Mouse click → set cursor position
- [ ] Select text: click + drag, or Shift+Arrow keys
- [ ] Ctrl+A → select all
- [ ] Cut/Copy/Paste via clipboard (Ctrl+X/C/V)
- [ ] Vertical scroll bar for long files
- [ ] Word wrap toggle (View menu)
- [ ] Status bar: line/col, encoding (UTF-8), line ending
- [ ] *(Stretch)* Find/Replace: Ctrl+F, Ctrl+H
- [ ] *(Stretch)* Undo/Redo: Ctrl+Z/Y (action stack)
- [ ] *(Stretch)* Line numbers in left gutter
- [ ] *(Stretch)* Syntax highlighting for .c/.h/.asm
- [ ] Commit: `"apps: Notepad editing features"`

