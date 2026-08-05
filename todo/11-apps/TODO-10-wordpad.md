---
schema_version: 1
id: wordpad
domain: 11-apps
status: active
title: "TODO-10 -- WordPad (Rich Text Editor)"
---

# TODO-10 -- WordPad (Rich Text Editor)

> **Goal:** Build `wordpad.exe` -- a rich text editor bridging Notepad and a full word processor.
> Paragraph-based document model with per-run character formatting, a simplified RTF 1.5
> reader/writer, a multi-run layout engine with a horizontal ruler, a format toolbar, and
> print-to-PDF stretch.

> [!IMPORTANT]
> Notepad (§1 gap buffer, §3 file ops, §4 clipboard, §5 find) is already specified in
> `09-desktop-shell/TODO-10-notepad.md` -- WordPad does **not** share its text engine; the
> paragraph+run model here is purpose-built for mixed formatting and must be implemented from
> scratch.
>
> `CTRL_COMBOBOX` (font family + size combo-boxes in §4) is planned in
> `08-graphics-ui/TODO-06-widget-dialogs.md` -- do not build a custom combo widget here; add
> `CTRL_COMBOBOX` to TODO-05 if it is not yet present when §3 is implemented.
>
> Print (§7) depends on `pdf_begin/draw_text/end` from `10-platform-services/TODO-12 §8` --
> implement §7 only after the print subsystem is available.

---

## Inputs

- `include/font_mgr.h` -- `ttf_get(slot, px)`, `ttf_draw_string()`, `ttf_draw_char()`, `ttf_measure_width()` -- §3 layout + rendering
- `include/gfx.h` -- `gfx_fill_rect()`, `gfx_draw_rect()`, `gfx_surface_create()`, `gfx_draw_line()` -- §3 selection + ruler
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_mark_dirty()`
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_TEXTBOX`, `CTRL_SCROLLBAR_VERT`, `CTRL_SCROLLBAR_HORIZ`, `CTRL_COMBOBOX` (→ `08-graphics-ui/TODO-05`)
- `include/kernel/clipboard.h` (→ XREF `09-desktop-shell/TODO-01 §1`) -- `clipboard_set/get(CLIP_TEXT, ...)` -- §5 cut/copy/paste
- `08-graphics-ui/TODO-06-widget-dialogs.md §3` -- `dialog_file_open()`, `dialog_file_save()`, `dialog_color()` -- §3 color picker, §6 file ops
- `include/desktop/file_assoc.h` (→ XREF `09-desktop-shell/TODO-02 §1`) -- `file_assoc_set(ext, prog_id, path)` -- §6
- `include/registry.h` -- `reg_get_string`, `reg_set_string` -- §6 recent files
- `include/kernel/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create`, `vfs_stat`
- `10-platform-services/TODO-12-long-term-features.md §8` -- `pdf_begin`, `pdf_draw_text`, `pdf_draw_rect`, `pdf_end` -- §2 print stretch

---

## Outcome

`wordpad.exe` opens `.rtf` and `.txt` files with full character formatting (bold, italic, underline, color) and paragraph formatting (alignment, indent). The horizontal ruler with draggable indent/tab markers is shown above the canvas. Format toolbar allows per-selection character format changes. RTF files save and re-open with formatting preserved. Print-to-PDF stretch produces a paginated PDF of the document.

---

## Implementation Order

| Step | Section                             | 💎/⭐ | Dependency                                                      |
| ---- | ----------------------------------- | ----- | --------------------------------------------------------------- |
| 1    | Rich Text Document Model            | 💎    | `pmm_alloc_contiguous`, `kmalloc`                               |
| 2    | RTF File Format (Reader + Writer)   | 💎    | §1 document model                                               |
| 3    | Rich Text Rendering + Layout Engine | 💎    | §1 model, `ttf_measure_width`, `gfx_fill_rect`                  |
| 4    | Toolbar + Menus                     | 💎    | §3 rendering, `CTRL_COMBOBOX`, `dialog_color`                   |
| 5    | Formatting Interactions             | 💎    | §3 + §4 stable                                                  |
| 6    | File Operations + File Associations | 💎    | §2 RTF reader/writer, `dialog_file_open/save`, `file_assoc_set` |
| 7    | Print (Stretch)                     | 💎    | §3 layout, `pdf_begin/draw_text/end` D10T12 §8                  |

---

## 1. Rich Text Document Model `[Opus]`

> Novel data structure: no prior Impossible OS precedent for a paragraph-level editor.
> Doubly-linked paragraph list with per-paragraph run chains; snapshot-based undo must
> serialize and restore arbitrary regions of the paragraph list.

**Source file:** `src/apps/wordpad/doc_model.c`; header `include/apps/wordpad/doc_model.h`

- [ ] **Character format** (`struct char_fmt`): `font_name[32]`, `font_size_pt` (uint16), `bold`, `italic`, `underline`, `strikethrough` (uint8 flags), `fg_color` (uint32 ARGB), `bg_color` (uint32 ARGB, 0 = transparent)
- [ ] **Run** (`struct run_t`): `text[256]` (UTF-8 chars), `len`, `char_fmt fmt`; runs are the atoms of formatted text sharing identical character format; next/prev pointers form per-paragraph run chain
- [ ] **Paragraph format** (`struct para_fmt`): `align` (ALIGN_LEFT/CENTER/RIGHT/JUSTIFY), `left_indent_pt`, `right_indent_pt`, `first_line_indent_pt`, `space_before_pt`, `space_after_pt`, `tab_stops[8]` (positions in points)
- [ ] **Paragraph** (`struct para_t`): doubly-linked list node; `runs` (head of run chain), `run_count`, `para_fmt fmt`
- [ ] **Document** (`struct doc_t`): `head`, `tail` paragraph pointers; `para_count`; `cursor_para`, `cursor_run`, `cursor_offset`; `sel_start`, `sel_end` (absolute char offsets from doc start)
- [ ] `doc_insert_char(doc, ch)` → insert into current run at `cursor_offset`; if run full (256 chars): split run
- [ ] `doc_delete_before(doc)` → delete char before cursor; if at run start: merge with prev run or prev para
- [ ] `doc_split_para(doc)` → at cursor position, split current para into two; new para inherits current para_fmt and run's char_fmt
- [ ] `doc_apply_char_fmt(doc, sel_start, sel_end, fmt, mask)` → split runs at selection boundaries; apply only masked fields to runs within range
- [ ] `doc_apply_para_fmt(doc, para, fmt, mask)` → set para_fmt fields on paragraph
- [ ] **Undo/Redo**: `struct undo_entry { para_snapshot *paras; int count }` ring of 100; `doc_snapshot_range(start_para, end_para)` deep-copies para+run chain (kmalloc each); on undo: restore snapshot; on redo: re-apply; snapshot taken before any formatting or structural edit

---

## 2. RTF File Format `[Sonnet]`

> RTF 1.5 is a well-specified format with clear Windows reference docs.

**Source file:** `src/apps/wordpad/rtf_io.c`; header `include/apps/wordpad/rtf_io.h`

- [ ] **RTF reader** (`rtf_load(path, doc)`):
  - [ ] Stack-based parser: maintain a state stack (pushed on `{`, popped on `}`); state = current char_fmt + para_fmt + current color index
  - [ ] **Font table** `{\fonttbl {\f0\froman\fcharset0 Selawik;}...}` → `g_font_table[32]` maps `\fN` index to font name
  - [ ] **Color table** `{\colortbl ;\red255\green0\blue0;...}` → `g_color_table[32]` maps index to ARGB
  - [ ] **Control words** (apply to current state):
    - `\b` `\b0` -- bold on/off
    - `\i` `\i0` -- italic on/off
    - `\ul` `\ulnone` -- underline on/off
    - `\strike` `\strike0` -- strikethrough on/off
    - `\fsN` -- font size in half-points (`/2` to get pt)
    - `\fN` -- font index → look up `g_font_table`
    - `\cfN` -- foreground color index → look up `g_color_table`
    - `\highlightN` -- background color index
    - `\par` -- emit current run; create new paragraph inheriting default para_fmt
    - `\pard` -- reset para_fmt to defaults
    - `\qc` `\qr` `\qj` -- alignment (default = left)
    - `\liN` `\riN` `\fiN` -- left/right/first-line indent in twips (`/ 1440 × 72` for pt)
    - `\sbN` `\saN` -- space-before / space-after in twips
    - `\tabN` -- tab stop in twips
  - [ ] Unrecognized control words: skip word + optional parameter; do not abort
  - [ ] Plain text bytes (not control) → append to current run buffer
- [ ] **RTF writer** (`rtf_save(path, doc)`): emit minimal valid RTF:
  - [ ] Header: `{\rtf1\ansi\deff0`
  - [ ] Font table: one entry per unique font name in doc
  - [ ] Color table: one entry per unique fg/bg color
  - [ ] For each paragraph: `\pard` + para_fmt control words; for each run: char_fmt control words + plain text (escape `{`, `}`, `\` as `\{`, `\}`, `\\`); `\par`
  - [ ] Footer: `}`
- [ ] **Plain-text fallback**: if file has no `{\rtf1` header → load as plain text into single run per line, default char/para fmt

---

## 3. Rich Text Rendering + Layout Engine `[Opus]`

> Multi-run line-breaking, per-run glyph metrics, and a ruler with draggable tab stop markers
> are novel layout infrastructure with no prior Impossible OS precedent.

**Source file:** `src/apps/wordpad/wordpad_render.c`

- [ ] **Canvas area**: below toolbar (40 px) and ruler (20 px); left margin 60 px (`g_left_margin_px`); right margin 60 px; `CTRL_SCROLLBAR_VERT` on right
- [ ] **Line layout** (`layout_line` struct: run pointers + y-pos + height + baseline):
  - [ ] For each paragraph, iterate runs; measure each run word by word via `ttf_measure_width(font_for_run, word)`
  - [ ] If adding a word would exceed `canvas_w - left_margin - right_margin` → start new visual line
  - [ ] Track line height = `max(run_height for runs on line)`; track baseline = `max(font_ascent)`
  - [ ] Paragraph spacing: add `para_fmt.space_before_pt` before first line, `space_after_pt` after last line
- [ ] **Rendering**: for each visible line, for each run on that line:
  - [ ] `ttf_font_t *f = ttf_get_by_name(run->fmt.font_name, run->fmt.font_size_pt)` (fallback to `FONT_BODY` if name not found)
  - [ ] Apply bold via `ttf_get_bold()` / italic via `ttf_get_italic()` (or TTF style variants if loaded)
  - [ ] `ttf_draw_string(surface, f, x, baseline_y, run->text, run->fmt.fg_color)`
  - [ ] Underline: `gfx_draw_line(surface, x, baseline_y+2, x+run_w, baseline_y+2, fg_color, 1)`
  - [ ] Strikethrough: `gfx_draw_line` at `baseline_y - font_size/4`
  - [ ] Background: `gfx_fill_rect(surface, x, line_y, run_w, line_h, bg_color)` if bg_color != 0
- [ ] **Selection highlight**: for runs between `sel_start` and `sel_end`, compute pixel range; `gfx_fill_rect()` with accent color (partial run = use `ttf_measure_width` for prefix offset)
- [ ] **Cursor**: 1 px vertical line at insertion point; 500 ms blink; hidden when selection active
- [ ] **Alignment**: JUSTIFY spreads words via extra word-spacing; CENTER/RIGHT compute x offset per line
- [ ] **Horizontal ruler** (20 px strip above canvas, same width):
  - [ ] Gray background; tick marks at 0.5-inch intervals; numbers at 1-inch intervals
  - [ ] Left indent marker (downward triangle at `para_fmt.left_indent_pt`); right indent marker (upward triangle); first-line indent (top-down triangle offset from left)
  - [ ] Tab stop markers (small `T` icons at each tab stop position)
  - [ ] Drag: mouse-down on indent marker → drag horizontal → update `para_fmt`; drag tab stop → reposition; drag off ruler → delete tab stop

---

## 4. Toolbar + Menus `[Sonnet]`

**Source file:** `src/apps/wordpad/wordpad_ui.c`

- [ ] **Format toolbar** (40 px fixed height):
  - [ ] Font family `CTRL_COMBOBOX` (120 px wide): lists all loaded TTF fonts from `font_mgr_list()`; change → `doc_apply_char_fmt(doc, sel, {.font_name=selected}, MASK_FONT)`
  - [ ] Font size `CTRL_COMBOBOX` (60 px): common sizes 8/9/10/11/12/14/16/18/20/24/28/36/48/72 + typed; change → apply `font_size_pt`
  - [ ] `[B]` (Ctrl+B), `[I]` (Ctrl+I), `[U]` (Ctrl+U), `[S]` (Ctrl+S for strikethrough) toggle buttons -- show pressed state when cursor is in formatted run; click → toggle on selection
  - [ ] `[A▾]` text color button: click → `dialog_color()` → apply `fg_color`; color swatch shows current selection color
  - [ ] `[🖊▾]` highlight color: click → `dialog_color()` → apply `bg_color`
  - [ ] Alignment buttons: `[≡]` `[≡]` `[≡]` `[≡]` (left/center/right/justify) -- only one active at a time; click → `doc_apply_para_fmt`
  - [ ] `[→]` indent / `[←]` outdent: increment/decrement `left_indent_pt` by 720 (½ inch in twips)
- [ ] **Menu bar**: `File  Edit  View  Insert  Format  Help`
  - [ ] `File`: New, Open, Save, Save As, Recent Files, Print (§7), Exit
  - [ ] `Edit`: Undo, Redo, Cut, Copy, Paste, Select All, Find (Ctrl+F)
  - [ ] `View`: Word Wrap (toggle), Ruler (toggle), Status Bar (toggle), Zoom submenu
  - [ ] `Insert`: Date/Time (`time_now()` → formatted string `YYYY-MM-DD HH:MM` inserted at cursor), horizontal rule (insert `─────` string)
  - [ ] `Format`: Font dialog (extended version of toolbar controls in a dialog), Paragraph dialog (indent + spacing + alignment fields), Bullets (toggle simple bullet list: prepend `• ` to each selected para)
- [ ] **Status bar** (20 px): `Page {n} of {total}` left; `{cursor_line}, {cursor_col}` center; `{encoding}` right

---

## 5. Formatting Interactions `[Sonnet]`

- [ ] **Ctrl+B / I / U / Strikethrough**: check if all chars in selection already have format → if yes, remove; else apply; update toolbar toggle state
- [ ] **Tab key**: insert tab character `\t` into run; advance cursor to next tab stop (scan `para_fmt.tab_stops[]`; default tab stops every 720 twips)
- [ ] **Enter (paragraph split)**: `doc_split_para()` → new paragraph inherits current `para_fmt` and cursor run's `char_fmt`; cursor moves to first position of new para
- [ ] **Backspace at para start**: merge current para with previous (append current para's runs to previous para's run chain; delete current para node)
- [ ] **Delete at para end**: merge next para into current (symmetric to backspace merge)
- [ ] **Mouse click → cursor**: hit-test line then run then char offset (binary search via `ttf_measure_width` prefix sums)
- [ ] **Click+drag selection**: set `sel_start` on mouse-down; extend `sel_end` on mouse-move; render highlight (§3); copy selection text to clipboard on Ctrl+C (flatten runs to plain text)
- [ ] **Shift+Arrow**: extend selection by character or line; Ctrl+Shift+Arrow → extend by word
- [ ] **Ctrl+A**: `sel_start=0`, `sel_end=doc_total_len(doc)`

---

## 6. File Operations + File Associations `[Sonnet]`

- [ ] **File→Open**: `dialog_file_open("Rich Text|*.rtf|Text Files|*.txt|All Files|*.*")` → `rtf_load(path, doc)` or plain-text load; update window title; `modified=0`
- [ ] **File→Save**: if `filepath==""` → Save As; else `rtf_save(filepath, doc)` (always saves as RTF); `modified=0`
- [ ] **File→Save As**: `dialog_file_save("RTF Document|*.rtf|Plain Text|*.txt")` → if `.txt` selected: strip formatting, `vfs_write` plain text; if `.rtf`: `rtf_save`; update `filepath` + title
- [ ] **Modified flag + title**: `"*{filename} -- WordPad"` when unsaved; save-changes dialog on New/Open/Close
- [ ] **Drag-and-drop**: `WM_DROPFILES` → open dropped file (save-changes check first)
- [ ] **CLI arg**: `wordpad.exe C:\path\doc.rtf` → open immediately
- [ ] **Recent files**: `HKCU\Software\Impossible\WordPad\RecentFiles\{0..9}` (10 MRU entries)
- [ ] **File associations**:
  - [ ] `file_assoc_set(".rtf", "ImpossibleOS.WordPad", "C:\\Impossible\\System32\\wordpad.exe")`
  - [ ] `file_assoc_set(".doc", "ImpossibleOS.WordPad", "C:\\Impossible\\System32\\wordpad.exe")` -- opens as RTF (best-effort; `.doc` binary format fallback to RTF header detection)
- [ ] **"Open with WordPad" context verb** registered for any file type

---

## 7. Print (Stretch) `[Sonnet]`

> → XREF: `10-platform-services/TODO-12-long-term-features.md §8` -- `pdf_begin/draw_text/end`.

- [ ] **Pagination**: compute page height in points (A4 = 841.89 pt or Letter = 792 pt); walk layout lines (§3); break when accumulated height exceeds page height minus margins
- [ ] **Print preview window**: `wm_create_window("Print Preview", 800, 600)` -- renders each page as a thumbnail bitmap; `[← Prev]` `[Next →]` navigation; `[Close Preview]`
- [ ] **File→Print**: call `pdf_begin(out_path, page_width_pt, page_height_pt)` from TODO-12 §8; for each page: `pdf_begin_page()` → for each layout line on page: `pdf_draw_text(x, y, text, font_name, size_pt, color)` → for each decorated run: `pdf_draw_rect` for background fills, underlines; `pdf_end_page()`; `pdf_end()`
- [ ] **Print dialog**: `dialog_confirm("Print to PDF?\nOutput: C:\\Users\\{name}\\Documents\\{filename}.pdf", MB_OKCANCEL)` → on OK: run pagination + PDF export; toast `"Document exported to {path}"`
- [ ] **Page Setup**: paper size dropdown (A4/Letter/A5), margin inputs (top/bottom/left/right in mm); stored in `HKCU\Software\Impossible\WordPad\PageSetup\*`

---

## OS Comparison


| ⭐  | Feature                                             | 🪟 Win11                          | 🐧 Linux                                | 🚀 Impossible OS                                                   |
| --- | --------------------------------------------------- | --------------------------------- | --------------------------------------- | ------------------------------------------------------------------ |
| 💎  | Paragraph + run rich text document model            | ✅ WordPad (internal; RTF-backed) | ✅ AbiWord / LibreOffice Writer         | ⬜ §1 -- doubly-linked para list, per-run char_fmt,                |
| 💎  | RTF 1.5 reader                                      | ✅ WordPad (full RTF 1.5)         | ✅ AbiWord (RTF import)                 | ⬜ §2 -- stack-based control word parser, font                     |
| 💎  | RTF writer                                          | ✅ WordPad                        | ✅ AbiWord                              | ⬜ §2 -- minimal valid RTF emission                                |
| 💎  | Multi-run layout engine                             | ✅ WordPad (RichEdit control)     | ✅ GTK TextView / Pango                 | ⬜ §3 -- `ttf_measure_width` word-break, JUSTIFY, per-run baseline |
| 💎  | Horizontal ruler with draggable indent + tab stops  | ✅ WordPad                        | ✅ AbiWord                              | ⬜ §3 -- ruler strip, drag-update para_fmt, tab                    |
| 💎  | Format toolbar                                      | ✅ WordPad                        | ✅ AbiWord / LibreOffice                | ⬜ §4 -- `CTRL_COMBOBOX` font/size, toggle buttons, `dialog_color` |
| 💎  | Paragraph split/merge, Tab, Ctrl+B/I/U interactions | ✅ WordPad                        | ✅ AbiWord                              | ⬜ §5 -- run boundary split/merge, tab-stop advance                |
| 💎  | `.rtf` + `.doc` file associations                   | ✅ WordPad (`.rtf` default)       | ⚠️ AbiWord (`.rtf`; no built-in `.doc`) | ⬜ §6 -- `file_assoc_set` for both                                 |
| 💎  | Print preview + PDF export                          | ✅ WordPad (Print dialog → PDF    | ✅ AbiWord (PDF via evince/cups)        | ⬜ §7 -- (Stretch) -- ; `pdf_begin/draw_text/end` TODO-12          |

Impossible OS WordPad uses the same native TTF stack as every other desktop component --
no RichEdit COM object, no GTK, no external layout engine -- giving consistent glyph metrics
and a zero-dependency rich-text editing surface from day one.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **Document model:** insert 100 chars across 3 runs; `doc_apply_char_fmt` on middle run sets bold; bold flag present only on middle run; undo restores all three to original state
- [ ] **RTF round-trip:** save a multi-format document as `.rtf`; close; reopen; bold/italic/color/indent all match the original; no data loss
- [ ] **Plain text load:** open `notepad.exe`-saved `.txt` file in WordPad; content appears in single default-format run; save as `.rtf` creates valid RTF file
- [ ] **Layout:** 200-char paragraph in narrow window; line breaks at word boundaries; no word split mid-character; justified alignment fills both margins; space-after between paragraphs visible
- [ ] **Ruler:** ruler shows tick marks; drag left-indent marker rightward → paragraph indents visually; drag tab stop → text jumps to new stop position on Tab press
- [ ] **Toolbar:** select text; click `[B]` → text bolds; toolbar `[B]` shows pressed; click again → unbolded; font size combo → change to 24 pt → selected text visually larger
- [ ] **Color picker:** select word; click `[A▾]` → `dialog_color()` opens → select red → word renders in red
- [ ] **Ctrl+B/I/U:** keyboard shortcuts toggle identical to toolbar buttons; mixed selection (some bold, some not) → applies bold to all
- [ ] **Paragraph split/merge:** Enter mid-word splits para; new para at next line; Backspace at para start merges back; no character loss
- [ ] **File ops:** save as `.rtf`; title removes `*`; reopen → formatting intact; `.doc` file → opens (best-effort RTF detection or plain text fallback)
- [ ] **File assoc:** `.rtf` double-click in File Manager → `wordpad.exe` opens file
- [ ] **Print preview (stretch):** A4 document → print preview shows correct page breaks; "Print to PDF" → PDF file created at correct path with formatted text
- [ ] Commit: `"apps: WordPad -- RTF model, layout engine, ruler, toolbar, file ops, print stretch"`
