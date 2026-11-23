---
schema_version: 1
id: paint-app
domain: 10-platform-services
status: active
title: "TODO-02 -- Paint App & Image Tools"
---

# TODO-02 -- Paint App & Image Tools

> **Goal:** Build the bitmap drawing application -- a full Windows Paint equivalent that exercises the complete GFX/widget stack. The drawing primitives, image loader, and compositing engine are already done; this TODO delivers the interactive canvas, tools, color system, undo/redo, and file I/O on top.

> [!IMPORTANT]
> **Already exists**: `gfx_fill_rect/draw_rect/fill_circle/draw_line/put_pixel/blend_pixel/blit()` in `gfx.h`. `gfx_surface_create/init/destroy()`. `image_load/scale/save_bmp/save_png()` in `image.h`. `ttf_draw_string(FONT_UI)`. `pmm_alloc_contiguous()` for canvas + undo buffers. `wm_create_window()`, `WM_MOUSE_DOWN/MOVE/UP`, `WM_KEYDOWN`. **Forward deps**: `dialog_file_open/save()` + `dialog_input()` + `dialog_color()` (08-graphics-ui/TODO-05); `clipboard_set/get(CLIP_IMAGE)` (09-desktop-shell/TODO-01 §1-3); `CTRL_SLIDER` + `CTRL_MENUBAR` + `CTRL_STATUSBAR` (08-graphics-ui controls). **Missing**: entire `src/apps/paint/` tree; flood fill BFS; PMM undo stack; text tool caret; selection tool. **Note on `image_save_png`**: if `stb_image_write.h` isn't yet wired in `image.h`, add `image_save_png()` impl using `stb_image_write_png()`.

## Inputs

- `include/gfx.h` -- `gfx_surface_t`, `gfx_fill_rect/draw_rect/fill_circle/draw_line/put_pixel()` -- §1 canvas, §2 all tools
- `include/kernel/image.h` -- `image_load/save_bmp/save_png()` -- §5 file ops
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous/pmm_free_contiguous()` -- §1 canvas alloc, §4 undo snapshots, §2 flood-fill BFS stack
- `include/font_mgr.h` -- `ttf_draw_string(FONT_UI, size)` -- §2 text tool
- `include/desktop/wm.h` -- `wm_create_window()`, `WM_MOUSE_*`, `WM_KEYDOWN` -- §1 window + events
- `include/desktop/controls.h` (06-desktop-foundation/TODO-04) -- `CTRL_MENUBAR`, `CTRL_STATUSBAR`, `CTRL_SCROLLBAR` -- §1 layout
- `include/desktop/dialogs.h` (08-graphics-ui/TODO-06) -- `dialog_file_open/save()`, `dialog_input()`, `dialog_color()` -- §5 file dialogs, §2 color picker
- `include/desktop/clipboard.h` (09-desktop-shell/TODO-01) -- `clipboard_set/get(CLIP_IMAGE)` -- §6 copy/paste region
- → XREF: `08-graphics-ui/TODO-05` -- dialog APIs (`dialog_file_open`, `dialog_input`, `dialog_color`); §5 and §2 depend on those
- → XREF: `09-desktop-shell/TODO-01` -- `CLIP_IMAGE` format in clipboard; §6 selection copy/paste depends on that
- → XREF: `09-desktop-shell/TODO-02 §1` -- file association `.png/.bmp/.jpg` → Paint; register default editor app

## Outcome

- `struct paint` canvas (PMM `gfx_surface_t`, undo stack, tool, fg/bg color, brush size, zoom, scroll).
- 8 drawing tools: Pencil, Brush, Eraser, Line, Rectangle, Ellipse, Fill Bucket (BFS), Text.
- 20-swatch color bar; fg/bg overlapping squares; right-click bg; swap button; `dialog_color()` stretch.
- 32-level PMM undo/redo stack; Ctrl+Z/Y; per-stroke full-canvas or region snapshots.
- File menu: New (blank w/ dimension dialog), Open (`image_load`), Save/Save As (`image_save_bmp/png`).
- Stretch: Rectangle selection + `CLIP_IMAGE` copy/paste/move/delete.
- Stretch: Mouse-wheel zoom (25–800%), Image→Resize, Image→Crop to selection.

## Implementation Order

| ⭐  | Order | Section | Deliverable                                                                                | Depends On                                                        | Status |
| --- | :---: | :-----: | ------------------------------------------------------------------------------------------ | ----------------------------------------------------------------- | :----: |
| 💎  |   1   |   §1    | Canvas & viewport -- `struct paint`, PMM canvas, tool panel, palette bar, scrollbars       | §8                                                                |  [ ]   |
| 💎  |   2   |   §3    | Color system -- 20 swatches, fg/bg squares, right-click bg, swap, `dialog_color()` stretch | §8                                                                |  [ ]   |
| 💎  |   3   |   §2    | Drawing tools -- Pencil, Brush, Eraser, Line, Rect, Ellipse, Fill BFS, Text                | §8, §1, §3                                                        |  [ ]   |
| ⭐  |   4   |   §4    | Undo/Redo -- 32-level PMM snapshot ring; Ctrl+Z/Y; region vs full-canvas                   | §8, §2                                                            |  [ ]   |
| 💎  |   5   |   §5    | File operations -- New/Open/Save/Save As; `image_load`; bmp/png format picker              | §8                                                                |  [ ]   |
| 💎  |   6   |   §6    | Selection tool (stretch) -- dotted rect, Ctrl+C/V/X/Delete, move, `CLIP_IMAGE`             | §8, §1                                                            |  [ ]   |
| 💎  |   7   |   §7    | Zoom & resize (stretch) -- mouse-wheel zoom 25–800%, Image→Resize, Image→Crop              | §8, §1                                                            |  [ ]   |
| 💎  |   8   |   §8    | Import Paint                                                                               | `10-platform-services/TODO-07` §7 (PE programs run), `TODO-13` §3 |  [ ]   |

---

## 1. Canvas & Viewport

**Moved:** paint

Built in Paint, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: paint brings Canvas & Viewport"`

## 2. Drawing Tools

**Moved:** paint

Built in Paint, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: paint brings Drawing Tools"`

## 3. Color System

**Moved:** paint

Built in Paint, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: paint brings Color System"`

## 4. Undo/Redo

**Moved:** paint

Built in Paint, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: paint brings Undo/Redo"`

## 5. File Operations

**Moved:** paint

Built in Paint, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: paint brings File Operations"`

## 6. Selection Tool (Stretch)

**Moved:** paint

Built in Paint, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: paint brings Selection Tool (Stretch)"`

## 7. Zoom & Resize (Stretch)

**Moved:** paint

Built in Paint, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: paint brings Zoom & Resize (Stretch)"`

## 8. Import Paint

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** paint

Paint is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull paint` into `src/apps/paint/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Paint on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose paint`
- [ ] Commit: `"apps: Paint imported"`

---

## OS Comparison


| ⭐  | Feature           | 🪟 Win11                                            | 🐧 Linux                                                  | 🚀 Impossible OS                                         |
| --- | ----------------- | --------------------------------------------------- | --------------------------------------------------------- | -------------------------------------------------------- |
| 💎  | Canvas & viewport | ✅ MS Paint: GDI-backed canvas; scroll;             | ✅ GIMP, Pinta, KolourPaint; Cairo/GDK backed;            | ⬜ §1 -- `⭐` PMM `gfx_surface_t` canvas (no             |
| 💎  | Drawing tools     | ✅ MS Paint: all standard tools;                    | ✅ Pinta / KolourPaint: full standard                     | ⬜ §2 -- BFS flood fill via PMM                          |
| ⭐  | Fill Bucket       | ✅ MS Paint: fill tool uses                         | ✅ GIMP bucket fill: heap-allocated queue                 | ⬜ §2 -- `⭐` PMM contiguous traversal stack             |
| 💎  | Color system      | ✅ MS Paint: 20 preset swatches;                    | ✅ Pinta / GIMP: full HSV/RGB                             | ⬜ §3 -- matches Windows Paint palette; `dialog_color()` |
| ⭐  | Undo/Redo         | ✅ MS Paint: limited undo (few                      | ✅ GIMP/Pinta: 50+ undo levels; tile-based                | ⬜ §4 -- `⭐` PMM contiguous snapshots (no               |
| 💎  | File I/O          | ✅ MS Paint: BMP/JPEG/GIF/PNG/TIFF save; `modified` | ✅ Pinta / GIMP: broad format                             | ⬜ §5 -- BMP + PNG via `image_save_bmp/save_png()`       |
| 💎  | Selection         | ✅ MS Paint: rect/free-form select; copy/paste;     | ✅ Pinta: rect/freehand/magic wand; clipboard integration | ⬜ §6 -- (stretch) -- ; `CLIP_IMAGE` format              |
| 💎  | Zoom              | ✅ MS Paint Win11: 100–800% zoom;                   | ✅ GIMP: unlimited zoom; Image→Scale Image;               | ⬜ §7 -- (stretch) -- ; nearest-neighbour zoom           |

> **After §1–§7:** Impossible OS Paint matches MS Paint feature-for-feature with two hardware advantages: the canvas lives in `pmm_alloc_contiguous()` memory (direct GPU DMA path, no heap fragmentation) and the BFS flood fill uses a PMM-allocated traversal stack (deterministic latency even on 4K canvases, unlike heap-queue implementations that can stall on large areas).

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Open Paint → 800×600 white canvas; tool panel visible; palette bar at bottom
- [ ] Pencil tool: click+drag → black pixels drawn on canvas
- [ ] Brush size 8 px: drag → thick brush strokes; Eraser → removes to white
- [ ] Line: click+drag → rubber-band preview; release → committed line
- [ ] Fill Bucket: fill white region → entire region becomes fg color; does not leak across drawn lines
- [ ] Text: click canvas → type → Enter → text appears on canvas; Escape → cancels
- [ ] Right-click swatch → bg color changes; Swap (↔) → fg/bg exchange
- [ ] Draw stroke → Ctrl+Z → stroke undone; Ctrl+Y → redo restores; 5 levels deep all correct
- [ ] File→New → dimension dialog → blank canvas created; File→Save As `.png` → file written
- [ ] File→Open `.bmp` → image loaded onto canvas; modified → title shows `*`; close without save → guard dialog
- [ ] Stretch: selection drag → dotted rect; Ctrl+C → Ctrl+V → pasted region appears
- [ ] Stretch: mouse wheel → canvas zooms; `Fit` button → canvas fills viewport
- [ ] Commit: `"paint: full paint app -- canvas, 8 tools, color, undo/redo, file ops, selection, zoom -- complete"`
