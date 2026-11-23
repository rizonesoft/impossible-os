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
> `08-graphics-ui/TODO-05-widget-library-core.md` §5 -- do not build a custom combo widget here.
>
> Print (§7) depends on `pdf_begin/draw_text/end` from `10-platform-services/TODO-12 §6` --
> implement §7 only after the print subsystem is available.

---

## Inputs

- `include/font_mgr.h` -- `ttf_get(slot, px)`, `ttf_draw_string()`, `ttf_draw_char()`, `ttf_measure_width()` -- §3 layout + rendering
- `include/gfx.h` -- `gfx_fill_rect()`, `gfx_draw_rect()`, `gfx_surface_create()`, `gfx_draw_line()` -- §3 selection + ruler
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_mark_dirty()`
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_TEXTBOX`, `CTRL_SCROLLBAR_VERT`, `CTRL_SCROLLBAR_HORIZ`, `CTRL_COMBOBOX` (→ `08-graphics-ui/TODO-05`)
- `include/kernel/clipboard.h` (→ XREF `09-desktop-shell/TODO-01 §1`) -- `clipboard_set/get(CLIP_TEXT, ...)` -- §5 cut/copy/paste
- `08-graphics-ui/TODO-06-widget-dialogs.md §8` -- `dialog_file_open()`, `dialog_file_save()`, `dialog_color()` -- §4 color picker, §6 file ops
- `include/desktop/file_assoc.h` (→ XREF `09-desktop-shell/TODO-02 §1`) -- `file_assoc_set(ext, prog_id, path)` -- §6
- `include/registry.h` -- `RegGetString`, `RegSetString` -- §6 recent files
- `include/kernel/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create`, `vfs_stat`
- `10-platform-services/TODO-12-long-term-features.md §6` -- `pdf_begin`, `pdf_draw_text`, `pdf_draw_rect`, `pdf_end` -- §2 print stretch

---

## Outcome

`wordpad.exe` opens `.rtf` and `.txt` files with full character formatting (bold, italic, underline, color) and paragraph formatting (alignment, indent). The horizontal ruler with draggable indent/tab markers is shown above the canvas. Format toolbar allows per-selection character format changes. RTF files save and re-open with formatting preserved. Print-to-PDF stretch produces a paginated PDF of the document.

---

## Implementation Order

| Step | Section                             | 💎/⭐ | Dependency                                                                             | Status |
| ---- | ----------------------------------- | ----- | -------------------------------------------------------------------------------------- | :----: |
| 1    | Rich Text Document Model            | 💎    | §8                                                                                     |  [ ]   |
| 2    | RTF File Format (Reader + Writer)   | 💎    | §8, §1                                                                                 |  [ ]   |
| 3    | Rich Text Rendering + Layout Engine | 💎    | §8, §1                                                                                 |  [ ]   |
| 4    | Toolbar + Menus                     | 💎    | §8, §3                                                                                 |  [ ]   |
| 5    | Formatting Interactions             | 💎    | §8, §3, §4                                                                             |  [ ]   |
| 6    | File Operations + File Associations | 💎    | §8, §2                                                                                 |  [ ]   |
| 7    | Print (Stretch)                     | 💎    | §8, §3                                                                                 |  [ ]   |
| 8    | Import WordPad                      | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §3 |  [ ]   |

---

## 1. Rich Text Document Model

**Moved:** wordpad

Built in WordPad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: wordpad brings Rich Text Document Model"`

---

## 2. RTF File Format

**Moved:** wordpad

Built in WordPad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: wordpad brings RTF File Format"`

---

## 3. Rich Text Rendering + Layout Engine

**Moved:** wordpad

Built in WordPad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: wordpad brings Rich Text Rendering + Layout Engine"`

---

## 4. Toolbar + Menus

**Design:** [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout), [`controls.md#button`](../../docs/design/controls.md#button), [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

**Moved:** wordpad

Built in WordPad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: wordpad brings Toolbar + Menus"`

---

## 5. Formatting Interactions

**Moved:** wordpad

Built in WordPad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: wordpad brings Formatting Interactions"`

---

## 6. File Operations + File Associations

**Moved:** wordpad

Built in WordPad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] **File associations**:
  - [ ] `file_assoc_set(".rtf", "ImpossibleOS.WordPad", "C:\\Impossible\\System32\\wordpad.exe")`
  - [ ] `file_assoc_set(".doc", "ImpossibleOS.WordPad", "C:\\Impossible\\System32\\wordpad.exe")` -- opens as RTF (best-effort; `.doc` binary format fallback to RTF header detection)
- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: wordpad brings File Operations + File Associations"`

---

## 7. Print (Stretch)

**Moved:** wordpad

Built in WordPad, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: wordpad brings Print (Stretch)"`

## 8. Import WordPad

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** wordpad

WordPad is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull wordpad` into `src/apps/wordpad/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open WordPad on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose wordpad`
- [ ] Commit: `"apps: WordPad imported"`

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

Impossible OS WordPad uses the same native TTF stack as every other desktop component -- no RichEdit COM object, no GTK, no external layout engine -- giving consistent glyph metrics and a zero-dependency rich-text editing surface from day one.

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
