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
- `include/desktop/controls.h` (TODO-05) -- `CTRL_MENUBAR`, `CTRL_STATUSBAR`, `CTRL_SCROLLBAR` -- §1 layout
- `include/desktop/dialogs.h` (TODO-05) -- `dialog_file_open/save()`, `dialog_input()`, `dialog_color()` -- §5 file dialogs, §2 color picker
- `include/desktop/clipboard.h` (TODO-01) -- `clipboard_set/get(CLIP_IMAGE)` -- §6 copy/paste region
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

| ⭐  | Order | Deliverable                                                                                   | Depends On                                                                          | Status |
| --- | :---: | --------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Canvas & viewport -- `struct paint`, PMM canvas, tool panel, palette bar, scrollbars       | `gfx_surface_create()` (exists); `pmm_alloc_contiguous()`; `CTRL_MENUBAR/STATUSBAR` |  [ ]   |
| 💎  |   2   | §3 Color system -- 20 swatches, fg/bg squares, right-click bg, swap, `dialog_color()` stretch | `gfx_fill_rect/put_pixel()` (exist); `dialog_color()` (TODO-05 forward dep)         |  [ ]   |
| 💎  |   3   | §2 Drawing tools -- Pencil, Brush, Eraser, Line, Rect, Ellipse, Fill BFS, Text                | §1 canvas; §2 color; `gfx_draw_line/fill_circle/fill_rect()` (exist)                |  [ ]   |
| ⭐  |   4   | §4 Undo/Redo -- 32-level PMM snapshot ring; Ctrl+Z/Y; region vs full-canvas                   | §3 tools (stroke begin event); `pmm_alloc_contiguous()`                             |  [ ]   |
| 💎  |   5   | §5 File operations -- New/Open/Save/Save As; `image_load`; bmp/png format picker              | `image_load/save_bmp/save_png()` (exist); `dialog_file_open/save/input()` (TODO-05) |  [ ]   |
| 💎  |   6   | §6 Selection tool (stretch) -- dotted rect, Ctrl+C/V/X/Delete, move, `CLIP_IMAGE`             | §1 canvas; `clipboard_set/get(CLIP_IMAGE)` (TODO-01); `dialog_color()` bg fill      |  [ ]   |
| 💎  |   7   | §7 Zoom & resize (stretch) -- mouse-wheel zoom 25–800%, Image→Resize, Image→Crop              | §1 viewport; `image_scale()` (exists); `dialog_input()` for dimensions              |  [ ]   |

---

## 1. Canvas & Viewport `[Sonnet]`

`struct paint` (PMM canvas `gfx_surface_t`, undo stack, tool ID, fg/bg color, brush_size, zoom_pct, scroll_x/y). Default canvas 800×600 white, `pmm_alloc_contiguous`. Layout: tool panel left (64 px), canvas center, color palette bottom (48 px), status bar (20 px). Scrollbars when canvas > viewport.

**Files:** `src/apps/paint/paint.c` (new), `include/apps/paint.h` (new)

> [!NOTE]
> Window: initial 1024×640 px. **Canvas alloc**: `canvas_w * canvas_h * 4` bytes; > 4 KB → `pmm_alloc_contiguous(ceil(bytes / 4096))`; init with `gfx_clear(&canvas_surf, 0xFFFFFFFF)` (white). `gfx_surface_init()` with the PMM pixel pointer + canvas dimensions. **Viewport**: `view_w = window_w - TOOL_PANEL_W - scrollbar_w`, `view_h = window_h - PALETTE_H - STATUS_H - scrollbar_h`; canvas blit into viewport: `gfx_blit(win_surf, TOOL_PANEL_W, 0, &canvas_surf, scroll_x, scroll_y, view_w, view_h)`. **Scrollbars**: horizontal at bottom-left (if canvas_w > view_w); vertical at right; drag thumb → update `scroll_x/y`. **Zoom**: blit uses `gfx_surface_scale_blit()` stub (or nearest-neighbour zoom in render): `src_w = view_w * 100 / zoom_pct`; map mouse canvas coords: `canvas_x = scroll_x + mouse_x * 100 / zoom_pct`. **Tool panel**: 64 px wide; 32×32 px tool buttons (8 tools); selection highlighted with accent; icons from Fluent codepoints. **Status bar**: zoom%, canvas WxH, cursor canvas X,Y, selected color swatch.

- [ ] `typedef struct { gfx_surface_t canvas; gfx_surface_t *win_surf; int canvas_w, canvas_h; uint32_t fg_color, bg_color; int tool, brush_size, zoom_pct, scroll_x, scroll_y; int modified; } paint_t;`
- [ ] `void paint_open(const char *initial_path)` -- `wm_create_window()`; PMM canvas alloc; `gfx_clear()` white
- [ ] `void paint_render(paint_t *p)` -- blit canvas into viewport; tool panel; palette bar; scrollbars; status bar
- [ ] Viewport-to-canvas coordinate mapping: `canvas_x = scroll_x + mouse_x * 100 / zoom_pct`
- [ ] Scroll bar hit-test + drag → update `scroll_x/y`; clamp to `[0, canvas_w - view_w]`
- [ ] Tool button strip (8 buttons, 32×32): click → set `p->tool`; accent highlight active tool
- [ ] Status bar: `"Zoom: {N}% | {W}×{H} | X:{cx} Y:{cy}"` via `ttf_draw_string(FONT_UI, 12px)`
- [ ] Commit: `"paint: canvas window -- PMM gfx_surface_t, tool panel, palette bar, viewport scroll"`

## 2. Drawing Tools `[Sonnet]`

Pencil (Bresenham), Brush (circle stamps), Eraser (bg color), Line (rubber-band), Rectangle (filled/outline, Shift=square), Ellipse (Shift=circle), Fill Bucket (BFS 4-connectivity), Text (click place + TTF render).

**Files:** `src/apps/paint/tools.c` (new)

> [!NOTE]
> **Mouse event routing**: `WM_MOUSE_DOWN` → `paint_tool_begin(tool, cx, cy)`; `WM_MOUSE_MOVE` with btn → `paint_tool_drag(tool, cx, cy)`; `WM_MOUSE_UP` → `paint_tool_end(tool, cx, cy)`. Before `tool_begin`: if undo-tracked tool: `undo_push_snapshot(p)`. **Pencil**: on `tool_drag`: `gfx_draw_line(&canvas, prev_cx, prev_cy, cx, cy, fg_color)`. **Brush**: on `tool_drag`: `gfx_fill_circle(&canvas, cx, cy, brush_radius, fg_color)`. **Eraser**: same as Brush with `bg_color`. **Line**: on `tool_begin`: save `start_x/y`; on drag: render to temp surface (canvas copy) -- overlay line preview; on `tool_end`: `gfx_draw_line(&canvas, start_x, start_y, cx, cy, fg_color)`. **Rectangle**: `tool_begin` saves `start_x/y`; `tool_end`: if Shift held → `w = h = max(abs(dx), abs(dy))`; filled → `gfx_fill_rect()`; outline → `gfx_draw_rect()`. **Ellipse**: `gfx_fill_circle()` for now; Shift = equal axes. **Fill Bucket (BFS)**: `paint_flood_fill(canvas, cx, cy, fg_color)`: read `target_color = gfx_get_pixel(canvas, cx, cy)`; if `target_color == fg_color` → return; alloc BFS stack via `pmm_alloc_contiguous(1)` (4096 / 8 bytes per coord = 512 entries; if canvas is large may need more: `ceil(canvas_w * canvas_h * 8 / 4096)` pages); BFS 4-connectivity; for each popped pixel: `gfx_put_pixel(fg_color)` + push 4 neighbours if same `target_color`. **Text tool**: `tool_begin` → record `text_x/y`; `WM_KEYDOWN` appends chars to `g_text_buf[256]`; `WM_PAINT`: `ttf_draw_string(FONT_UI, 14px)` preview on overlay; Enter/Escape → commit to canvas.

- [ ] `void paint_tool_begin(paint_t *p, int cx, int cy)` -- dispatch by `p->tool`; save start coords
- [ ] Pencil: `tool_drag` → `gfx_draw_line()` between prev and current canvas coords
- [ ] Brush: `tool_drag` → `gfx_fill_circle(cx, cy, p->brush_size, p->fg_color)`
- [ ] Eraser: same as Brush with `p->bg_color`
- [ ] Line: rubber-band preview (overlay blit); commit `gfx_draw_line()` on mouse-up
- [ ] Rectangle: commit `gfx_fill_rect()` or `gfx_draw_rect()` on mouse-up; Shift → square
- [ ] Ellipse: `gfx_fill_circle()` (radius = max of half-w, half-h); Shift → circle (equal radii)
- [ ] `void paint_flood_fill(paint_t *p, int cx, int cy)` -- BFS with PMM stack; 4-connectivity
- [ ] Text: `g_text_buf`, `WM_KEYDOWN` append; Enter → `ttf_draw_string()` commit to canvas; Escape → cancel
- [ ] Brush size picker in tool panel: 5 radio dots (1/2/4/8/16 px radius)
- [ ] Commit: `"paint: drawing tools -- pencil, brush, eraser, line, rect, ellipse, BFS fill, text"`

## 3. Color System `[Sonnet]`

20 preset color swatches in bottom bar (40×24 px each). Overlapping fg/bg squares (top-left). Left-click swatch → fg; right-click → bg. Swap button (↔). Stretch: `dialog_color()` double-click for custom color.

**Files:** `src/apps/paint/colors.c` (new)

> [!NOTE]
> **Preset palette** (20 colors): black, white, red, green, blue, yellow, cyan, magenta, orange, purple, pink, brown, gray (4 shades), + 4 spare accent slots -- define as `uint32_t g_palette[20]` constant array. **Swatch strip**: drawn in bottom palette bar, 40×24 px each, border on hover. **fg/bg indicator**: two overlapping 28×28 px squares in bottom-left of palette area; bg square at (+4,+4) offset; fg on top; click either square → `dialog_color()` (TODO-05 forward dep) to set custom. **Left-click swatch**: `p->fg_color = g_palette[idx]`; update fg square. **Right-click swatch**: `p->bg_color = g_palette[idx]`; update bg square. **Swap button**: ↔ icon (16×16) between the two squares; click → swap `fg_color` and `bg_color`. Status bar shows selected color as small swatch + hex value.

- [ ] `uint32_t g_paint_palette[20]` -- 20 preset ARGB colors (black, white, primaries, secondaries, grays)
- [ ] `void paint_render_palette_bar(paint_t *p, gfx_surface_t *s)` -- 20 swatches + fg/bg indicator + swap btn
- [ ] Left-click swatch → `p->fg_color`; right-click swatch → `p->bg_color`
- [ ] Swap button click → `uint32_t tmp = p->fg_color; p->fg_color = p->bg_color; p->bg_color = tmp`
- [ ] Hover swatch → outline highlight (1 px accent border)
- [ ] Stretch: double-click fg or bg square → `dialog_color(&result)` → update respective color
- [ ] Status bar color preview: small 12×12 swatch + `"#RRGGBB"` label
- [ ] Commit: `"paint: color system -- 20 swatches, fg/bg squares, swap, dialog_color stretch"`

## 4. Undo/Redo `[Sonnet]`

Before each stroke begin: `pmm_alloc_contiguous()` snapshot of canvas → push to 32-slot ring undo stack; evict oldest. Ctrl+Z → pop + blit restore. Ctrl+Y → redo stack. Undo cleared on New.

**Files:** `src/apps/paint/undo.c` (new)

> [!NOTE]
> **Undo entry**: `typedef struct { uint32_t *pixels; int canvas_w, canvas_h; } undo_entry_t;`. Ring buffer: `g_undo[32]`, `g_undo_head`, `g_undo_count`. **Push**: `bytes = canvas_w * canvas_h * 4`; `pages = ceil(bytes / 4096)`; `entry.pixels = pmm_alloc_contiguous(pages)`; `kmemcpy(entry.pixels, canvas.pixels, bytes)`; push to ring; if `g_undo_count == 32` → free oldest: `pmm_free_contiguous(g_undo[oldest].pixels, pages)`. Also push to redo stack on Undo. **Canvas size constraint**: 800×600 × 4 = 1.92 MB per snapshot × 32 levels = 61.4 MB PMM. This is acceptable for typical usage; for large canvases (> 1920×1080): use region snapshot -- only snapshot the bounding rect of the current stroke (track `dirty_x/y/w/h` during tool_drag). **Ctrl+Z**: if `g_undo_count > 0`: pop entry → `kmemcpy(canvas.pixels, entry.pixels, bytes)` → `pmm_free()` entry; push current canvas to redo stack. **Ctrl+Y**: pop redo → restore → push to undo. **Clear on new/open**: `paint_undo_clear()` frees all PMM entries.

- [ ] `typedef struct { uint32_t *pixels; uint32_t canvas_w, canvas_h; uint32_t pmm_pages; } undo_entry_t;`
- [ ] `g_undo[32]` ring; `g_undo_head`, `g_undo_count`; `g_redo[32]`, `g_redo_count`
- [ ] `void paint_undo_push(paint_t *p)` -- PMM alloc + `kmemcpy` canvas pixels; evict oldest if full
- [ ] Region optimization: track `dirty_rect` during tool_drag; snapshot only dirty region for large canvases (> 1 MP)
- [ ] `void paint_undo(paint_t *p)` -- pop undo → restore canvas; push current to redo
- [ ] `void paint_redo(paint_t *p)` -- pop redo → restore canvas; push to undo
- [ ] `void paint_undo_clear(paint_t *p)` -- free all PMM undo + redo entries
- [ ] `WM_KEYDOWN` Ctrl+Z → `paint_undo()`; Ctrl+Y → `paint_redo()`
- [ ] Call `paint_undo_push()` at the start of: `tool_begin` (Pencil/Brush/Eraser/Line/Rect/Ellipse), flood fill, text commit
- [ ] Commit: `"paint: undo/redo -- 32-level PMM snapshot ring, dirty region opt, Ctrl+Z/Y"`

## 5. File Operations `[Sonnet]`

File→New (blank + `dialog_input` dimensions). File→Open (`dialog_file_open` → `image_load()` → copy to canvas). File→Save (`image_save_bmp` or `image_save_png`). File→Save As (format dropdown). `modified` flag + save-before-close guard.

**Files:** `src/apps/paint/file_ops.c` (new)

> [!NOTE]
> **New**: `dialog_input("Canvas Width:", "800")` + height → `paint_new_canvas(w, h)`; if `p->modified` → ask "Save changes?" first. **Open**: `dialog_file_open("Image files\0*.png;*.bmp;*.jpg;*.jpeg\0")` → `image_load(&img, path)` → copy pixels to canvas: if `img.width > canvas_w || img.height > canvas_h`: resize canvas (`pmm_free_contiguous(old)` + `pmm_alloc_contiguous(new)`) to match; `gfx_clear(&canvas, 0xFFFFFFFF)`; `gfx_blit(&canvas, 0, 0, &img.surface, ...)`. **Save**: if `g_current_path != NULL`: determine format from extension; `.png` → `image_save_png()`; `.bmp` → `image_save_bmp()`; else → "Save As". **Save As**: `dialog_file_save("*.png;*.bmp\0")` + format picker (dropdown or file-extension detection). `p->modified = 0` on successful save; `"*"` title suffix when `modified = 1`. **WM_CLOSE guard**: if `p->modified` → `dialog_confirm("Unsaved changes. Save before closing?", YES/NO/CANCEL)`.

- [ ] `void paint_file_new(paint_t *p)` -- `dialog_input()` for W×H; `paint_undo_clear()`; new PMM canvas; white fill
- [ ] `void paint_file_open(paint_t *p)` -- `dialog_file_open()` → `image_load()` → resize canvas if needed; blit
- [ ] `void paint_file_save(paint_t *p)` -- dispatch by `g_current_format` (BMP or PNG)
- [ ] `void paint_file_save_as(paint_t *p)` -- `dialog_file_save()` → detect format from ext → save
- [ ] `modified` flag: set in every `undo_push()` call; clear on save; `"Paint -- {filename}*"` title when set
- [ ] `WM_CLOSE` handler: `if (p->modified)` → confirm dialog → save / discard / cancel
- [ ] `CTRL_MENUBAR` File menu: New / Open / Save / Save As / Exit (with separators)
- [ ] `CTRL_MENUBAR` Image menu (stub entries for §7 Resize/Crop): filled in stretch
- [ ] Register `.bmp` → Paint as default editor (forward dep: TODO-02 §1 file assoc)
- [ ] Commit: `"paint: file operations -- New/Open/Save/Save As, image_load/save_bmp/png, modified guard"`

## 6. Selection Tool (Stretch) `[Sonnet]`

Rectangle selection: click+drag dotted rect. Ctrl+C → `clipboard_set(CLIP_IMAGE, region)`. Ctrl+V → paste at cursor position. Drag selection → move (replaces with bg). Delete → clear to bg.

**Files:** `src/apps/paint/selection.c` (new)

> [!NOTE]
> **Selection state**: `g_sel_x/y/w/h`, `g_sel_active` flag. Rubber-band during drag: `WM_MOUSE_MOVE` → render canvas + animated dotted outline (alternating 4-px dashes, tick-based offset). On `tool_end`: if `w > 0 && h > 0`: set `g_sel_active = 1`. **Copy** (Ctrl+C): alloc `sel_w * sel_h * 4` via `pmm_alloc_contiguous()`; copy region pixels; `clipboard_set(CLIP_IMAGE, {pixels, sel_w, sel_h})`; do not clear selection. **Paste** (Ctrl+V): `clipboard_get(CLIP_IMAGE, &img)` → blit onto canvas at `paste_x/y`; set `g_sel` rect to pasted region; mark modified. **Move**: `WM_MOUSE_DOWN` inside sel rect → enter MOVE mode; fill vacated region with `bg_color`; on drag: blit floating sel pixels at cursor offset; on release: commit to canvas. **Delete**: `gfx_fill_rect(&canvas, g_sel_x, g_sel_y, g_sel_w, g_sel_h, p->bg_color)`.

- [ ] `g_sel_x/y/w/h`, `g_sel_active`, `g_sel_mode` (SELECT/MOVE/PASTE) -- add to `paint_t`
- [ ] Rubber-band render: animated dotted outline overlay on canvas blit
- [ ] Ctrl+C: PMM copy of region pixels → `clipboard_set(CLIP_IMAGE, ...)`
- [ ] Ctrl+V: `clipboard_get(CLIP_IMAGE)` → paste blit + set sel rect at paste position
- [ ] Move: detect `WM_MOUSE_DOWN` inside sel → MOVE mode; bg-fill vacated; drag float blit
- [ ] Delete: `gfx_fill_rect()` with `bg_color` at sel rect
- [ ] Escape / click outside sel → deactivate selection (`g_sel_active = 0`)
- [ ] Commit: `"paint: selection tool -- dotted rect, Ctrl+C/V/X/Delete, move, CLIP_IMAGE"`

## 7. Zoom & Resize (Stretch) `[Sonnet]`

Mouse-wheel zoom (25%–800%), zoom buttons in status bar. Image→Resize (new W×H, aspect ratio toggle). Image→Crop to selection. Zoom affects viewport blit scale only (canvas pixels unchanged).

**Files:** extend `paint.c` / `file_ops.c`

> [!NOTE]
> **Zoom**: `WM_MOUSE_WHEEL` (or Ctrl+scroll): `p->zoom_pct = clamp(zoom_pct * (delta > 0 ? 110 : 91) / 100, 25, 800)`. Viewport blit scale: `src_rect = {scroll_x, scroll_y, view_w * 100 / zoom_pct, view_h * 100 / zoom_pct}`; nearest-neighbour scale via row/col index multiplication (no `image_scale()` call -- direct pixel loop for performance). **Zoom buttons** in status bar: `[25%]` `[50%]` `[100%]` `[200%]` `[Fit]` labels / click. `Fit`: `zoom_pct = min(view_w * 100 / canvas_w, view_h * 100 / canvas_h)`. **Image→Resize**: `dialog_input("New width:", str(canvas_w))` + height; maintain aspect toggle → adjust other dim; `image_scale(&new_img, &old_img, new_w, new_h, SCALE_BILINEAR)`; free old PMM; `pmm_alloc_contiguous(new)` for canvas. **Image→Crop**: if `g_sel_active`: `paint_new_canvas(sel_w, sel_h)`; copy region pixels; scroll_x/y = 0.

- [ ] `WM_MOUSE_WHEEL` → `zoom_pct *= 1.1 or / 1.1`; clamp `[25, 800]`; re-render
- [ ] Nearest-neighbour zoom blit in `paint_render()`: row/col index scale
- [ ] Zoom preset buttons in status bar: 25/50/100/200/Fit
- [ ] `Image→Resize` menu item: `dialog_input()` W + H + aspect checkbox → `image_scale()` + PMM realloc canvas
- [ ] `Image→Crop to selection`: copy sel region → new PMM canvas → update `canvas_w/h`
- [ ] Commit: `"paint: zoom (25–800%), Image→Resize, Image→Crop -- stretch features complete"`

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
