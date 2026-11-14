---
schema_version: 1
id: notepad-shell-host
domain: 09-desktop-shell
status: active
title: "TODO-10 -- Notepad Text Editor"
---

# TODO-10 -- Notepad Text Editor

> **Goal:** Superseded 2026-11-14: Notepad is owned by `11-apps/TODO-08-notepad.md`, and every section below is a pointer to the section it moved to (operator decision: `11-apps` owns the apps). The earlier goal, kept for the record: build Notepad -- the primary text editor and fallback file association for `.txt`/`.md`/`.c`/`.h`/`.asm` -- as the first real app that exercises the full widget stack. Built on a PMM-backed gap buffer with O(1) insert/delete, full keyboard + mouse editing, clipboard integration, find & replace, undo/redo, and stretch features (line numbers, syntax highlighting, font zoom).

> [!IMPORTANT]
> **Already exists**: `ttf_draw_string(s, f, x, y, text, color)` + `ttf_measure_width(f, text)` + `ttf_get(slot, px)` in `font_mgr.h`. `FONT_MONO=2` (Cascadia Code). `vfs_open/read/write`, `vfs_stat`, `vfs_create` for file I/O. `pmm_alloc_contiguous(pages)` + `pmm_free_contiguous(addr, pages)` for large buffers. `gfx_fill_rect()`, `gfx_surface_t`, `system_get_ticks()` (cursor blink). **Forward deps**: `CTRL_MENUBAR` (08-graphics-ui/TODO-06 §4), `CTRL_STATUSBAR` (08-graphics-ui/TODO-06 §5), `dialog_file_open/save()` and `dialog_input()` (08-graphics-ui/TODO-06 §8) for Go to Line. `clipboard_set/get(CLIP_TEXT)` (TODO-01 §1). **No `src/apps/` yet** -- TODO-09 creates it; Notepad lives at `src/apps/notepad/notepad.c`. **Missing**: gap buffer, text rendering loop, ANSI parser, undo stack, find/replace. Complete sections in order: gap buffer → text rendering → undo/redo → file menu → editing features → find & replace → stretch features.

## Inputs

- `include/font_mgr.h` -- `ttf_get()`, `ttf_draw_string()`, `ttf_measure_width()`, `FONT_MONO` -- §2 line rendering, §7 font zoom
- `include/gfx.h` -- `gfx_fill_rect()`, `gfx_surface_t` -- §2 selection highlight, §7 line number gutter
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()`, `pmm_free_contiguous()` -- §1 gap buffer allocation + doubling
- `include/kernel/fs/vfs.h` -- `vfs_open/read/write`, `vfs_create`, `vfs_stat` -- §3 file load/save
- `include/kernel/drivers/pit.h` -- `system_get_ticks()`, `PIT_TARGET_FREQ` -- §2 cursor blink 500 ms toggle
- `include/desktop/controls.h` (08-graphics-ui/TODO-06 §4/§5) -- `CTRL_MENUBAR`, `CTRL_STATUSBAR`, `dialog_file_open/save()`, `dialog_input()` -- §2 file menu
- `include/kernel/clipboard.h` (TODO-01 §1) -- `clipboard_set/get(CLIP_TEXT)` -- §4 Ctrl+C/X/V
- `include/desktop/wm.h` -- `wm_create_window()`, `WM_KEYDOWN`, `WM_MOUSE_DOWN/MOVE` -- §2 keyboard + mouse events
- `include/desktop/context_menu.h` (08-graphics-ui/TODO-09 §1) -- `context_menu_show()` -- §4 right-click context menu
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

| ⭐  | Order | Deliverable                                                                                            | Depends On                                                                           | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------ | :----: |
| ⭐  |   1   | §1 Gap buffer -- `text_buffer_t`, O(1) insert/delete, PMM backing, doubling, load/save                 | `pmm_alloc_contiguous` (exists); `vfs_read/write` (exists)                           |  [/]   |
| 💎  |   2   | §2 Text rendering -- visible-line loop, `ttf_draw_string`, I-beam blink, keyboard nav                  | §1 gap buffer; `ttf_draw_string` + `ttf_measure_width` (exist); `system_get_ticks()` |  [/]   |
| 💎  |   3   | §3 Undo/redo -- 200-action stack, every insert/delete logged, Ctrl+Z/Y                                 | §1 gap buffer (must be wired from day 1); §2 rendering (visible state after undo)    |  [/]   |
| 💎  |   4   | §4 File menu -- `CTRL_MENUBAR`, New/Open/Save/Save As, `modified` flag, "Save changes?" dialog         | §1 gap buffer (load/save); `CTRL_MENUBAR` + `dialog_file_open/save` (D08 T06 §4, §8) |  [/]   |
| 💎  |   5   | §5 Editing features -- mouse cursor, click+drag select, Ctrl+A/C/X/V, scrollbar, word wrap, status bar | §1-§4; `clipboard_set/get(CLIP_TEXT)` (TODO-01); `CTRL_STATUSBAR` (D08 T06 §5)       |  [/]   |
| 💎  |   6   | §6 Find & Replace -- Ctrl+F find toolbar, Ctrl+H dialog, match highlight, Ctrl+G Go to line            | §1-§5; `dialog_input()` (D08 T06 §8); §5 selection (Replace All uses select + paste) |  [/]   |
| 💎  |   7   | §7 Stretch -- line number gutter, `.c/.h/.asm/.md` syntax highlight, Ctrl++/−/0 font zoom              | §1-§6; `theme_get()` accent (08-graphics-ui/TODO-03 §2)                              |  [/]   |

---

## 1. Gap Buffer

**Design:** n/a -- the gap buffer is a data structure with no visual surface

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-08-notepad.md` and the section body moved there -> XREF: 11-apps/TODO-08 §6 (item: "Reconcile with the earlier sections of this file" at line 173)

- [/] Superseded: moved to `11-apps/TODO-08-notepad.md` §6; do not build a second copy here

## 2. Text Rendering & Cursor

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-08-notepad.md` and the section body moved there -> XREF: 11-apps/TODO-08 §7 (item: "typedef struct notepad { text_buffer_t *buf" at line 205)

- [/] Superseded: moved to `11-apps/TODO-08-notepad.md` §7; do not build a second copy here

## 3. Undo/Redo

**Design:** n/a -- the undo/redo stack has no visual surface; its menu items are drawn in §4

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-08-notepad.md` and the section body moved there -> XREF: 11-apps/TODO-08 §8 (item: "typedef struct { uint8_t type" at line 232)

- [/] Superseded: moved to `11-apps/TODO-08-notepad.md` §8; do not build a second copy here

## 4. File Menu

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control), [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-08-notepad.md` and the section body moved there -> XREF: 11-apps/TODO-08 §9 (item: "notepad_setup_menubar" at line 258)

- [/] Superseded: moved to `11-apps/TODO-08-notepad.md` §9; do not build a second copy here

## 5. Editing Features

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#scroll-bar`](../../docs/design/controls.md#scroll-bar), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-08-notepad.md` and the section body moved there -> XREF: 11-apps/TODO-08 §10 (item: "Mouse click → pixel-to-line/col conversion" at line 283)

- [/] Superseded: moved to `11-apps/TODO-08-notepad.md` §10; do not build a second copy here

## 6. Find & Replace

**Design:** [`controls.md#text-box-password-box-and-search-box`](../../docs/design/controls.md#text-box-password-box-and-search-box), [`controls.md#dialog`](../../docs/design/controls.md#dialog)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-08-notepad.md` and the section body moved there -> XREF: 11-apps/TODO-08 §11 (item: "uint32_t g_matches[512" at line 309)

- [/] Superseded: moved to `11-apps/TODO-08-notepad.md` §11; do not build a second copy here

## 7. Stretch Features

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Deferred:** [L] superseded 2026-11-14: this app is owned by `11-apps/TODO-08-notepad.md` and the section body moved there -> XREF: 11-apps/TODO-08 §12 (item: "Line number gutter" at line 335)

- [/] Superseded: moved to `11-apps/TODO-08-notepad.md` §12; do not build a second copy here

---

## OS Comparison


| ⭐  | Feature                                            | 🪟 Win11                                                 | 🐧 Linux                                    | 🚀 Impossible OS                                                     |
| --- | -------------------------------------------------- | -------------------------------------------------------- | ------------------------------------------- | -------------------------------------------------------------------- |
| ⭐  | Gap buffer                                         | ✅ Notepad: undisclosed internal (likely piece           | ✅ gedit: piece table; Kate: custom         | ⬜ §1 -- `⭐` explicit gap buffer design                             |
| 💎  | Text rendering                                     | ✅ Notepad: hardware-accelerated Direct2D rendering; GDI | ✅ gedit/Kate: Pango + Cairo; GLib;         | ⬜ §2 -- `ttf_draw_string()` per line; software rasterizer           |
| 💎  | Undo/redo -- 200-action stack, Ctrl+Z/Y            | ✅ Notepad: unlimited undo since Win10;                  | ✅ All editors: undo/redo; gedit unlimited; | ⬜ §3 -- 200-action ring buffer; insert +                            |
| 💎  | File menu                                          | ✅ Notepad: full file menu; "Save                        | ✅ gedit/nano: full file menu; modified     | ⬜ §4 -- `CTRL_MENUBAR` + `dialog_file_open/save()`; `modified` flag |
| 💎  | Editing                                            | ✅ Notepad: full mouse select; clipboard;                | ✅ All editors: mouse; clipboard; status    | ⬜ §5 -- pixel-to-col binary search; inverted selection              |
| 💎  | Find & Replace                                     | ✅ Notepad: inline find toolbar; Ctrl+H;                 | ✅ gedit: find toolbar; replace dialog;     | ⬜ §6 -- find toolbar (non-modal, slides in)                         |
| 💎  | Syntax highlight -- C/ASM/Markdown tokenizer-based | ✅ Notepad: no syntax highlighting (it's                 | ✅ gedit: GtkSourceView with full syntax;   | ⬜ §7 -- `⭐` more than Windows Notepad                              |
| 💎  | Font zoom -- Ctrl++/−/0, Registry persist          | ✅ Notepad: Ctrl++ zoom (since Win10                     | ✅ gedit: View → Zoom; font                 | ⬜ §7 -- `ttf_get(FONT_MONO, new_px)` hot-reload; `cell_h` recalc    |

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
