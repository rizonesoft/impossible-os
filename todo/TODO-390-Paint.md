# P0507 — Paint

> **Goal:** Bitmap drawing app with canvas, drawing tools, color system,
> undo/redo, and file operations.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Paint

### 1.1 Canvas & Viewport

**Prompt:** Paint uses a `gfx_surface_t` as its canvas (default 800×600 white). The canvas may be larger than the window — scroll bars allow panning the viewport. The window layout: tool panel on the left (vertical strip of tool icons), canvas area in the center, color palette at the bottom, and status bar showing canvas dimensions, current tool, and brush size. The undo stack stores canvas snapshots before each stroke (deep-copy the surface, max 32 levels). After completing all items, create `docs/user/paint.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Paint canvas and viewport"`.


- [ ] Create `src/apps/paint/paint.c`
- [ ] Define `struct paint` state (canvas surface, undo stack, current tool, colors, brush size)
- [ ] Canvas: `gfx_surface_t` — default 800×600 white
- [ ] Scroll viewport: horizontal + vertical scroll bars
- [ ] Canvas positioned inside window with toolbar (left) + color palette (bottom) + status bar
- [ ] Commit: `"apps: Paint canvas and viewport"`

### 1.2 Drawing Tools

**Prompt:** All drawing tools operate on the canvas surface. Pencil draws Bresenham lines between consecutive mouse events for smooth freehand drawing. Brush stamps filled circles at each mouse position with configurable radius. Eraser draws with the background color. Line: preview a rubber-band line during drag, render on release. Rectangle/Ellipse: outline or filled, hold Shift for square/circle. Fill bucket uses BFS flood fill from the click point, replacing the target color with the selected color. Text tool: click to place, opens a text input, renders using `font_draw_string()`. Tool selection via toolbar buttons on the left panel. After completing all items, update `docs/user/paint.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Paint drawing tools"`.


- [ ] **Pencil** — freehand drawing (Bresenham line between mouse events)
- [ ] **Brush** — variable-size soft brush (filled circle stamps)
- [ ] **Eraser** — draw with background color
- [ ] **Line** — click + drag → preview rubber-band line → draw on release
- [ ] **Rectangle** — outline or filled rectangle (Shift for square)
- [ ] **Ellipse** — outline or filled ellipse (Shift for circle)
- [ ] **Fill Bucket** — flood fill (BFS/stack-based) with selected color
- [ ] **Text** — click to place, type text, set font size
- [ ] Tool selection via toolbar buttons (left panel)
- [ ] Commit: `"apps: Paint drawing tools"`

### 1.3 Color System

**Prompt:** The color palette bar at the bottom shows 20 preset colors in small squares. Two overlapping squares show the current foreground (on top) and background (behind) colors. Left-click a palette color to set foreground; right-click to set background. Click the swap icon to exchange foreground/background. Stretch goal: a color picker dialog (from §1.3) for custom colors with HSV sliders and hex input. After completing all items, update `docs/user/paint.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Paint color palette"`.


- [ ] Color palette bar at bottom: 20 preset colors
- [ ] Foreground + background color indicators (click to swap)
- [ ] Click palette color → set foreground; right-click → set background
- [ ] *(Stretch)* Color picker dialog for custom colors (Hue/Saturation/Value)
- [ ] Commit: `"apps: Paint color palette"`

### 1.4 Undo & File Operations

**Prompt:** Before each drawing stroke, push a copy of the affected canvas region onto the undo stack (max 32 levels). Ctrl+Z pops the stack and restores. Ctrl+Y re-applies (redo stack). File → Open loads an image via `image_load()` from Phase 02 §3 (JPEG, PNG, BMP). File → Save writes as BMP via `image_save_bmp()`. File → Save As lets the user choose format (BMP or PNG if PNG save is implemented). Stretch goals: rectangle selection tool for move/copy regions, mouse wheel zoom with percentage display. After completing all items, update `docs/user/paint.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Paint undo and file ops"`.


- [ ] Undo stack: save canvas snapshot before each stroke (max 32 levels)
- [ ] Ctrl+Z → undo, Ctrl+Y → redo
- [ ] File → New: create blank canvas (prompt for dimensions)
- [ ] File → Open: load image via `image_load()` (JPG, PNG, BMP)
- [ ] File → Save: save as BMP via `image_save_bmp()`
- [ ] File → Save As: choose format (BMP, PNG)
- [ ] *(Stretch)* Select tool: rectangle selection, move/copy region
- [ ] *(Stretch)* Zoom: mouse wheel zoom, percentage display
- [ ] *(Stretch)* Resize canvas: Image → Resize
- [ ] Status bar: canvas dimensions, current tool, brush size
- [ ] Commit: `"apps: Paint undo and file ops"`

