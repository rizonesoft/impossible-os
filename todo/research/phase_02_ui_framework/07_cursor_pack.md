# Cursor Pack — Research & Planning

## Overview

Replace the single hardcoded 12×19 white arrow cursor with a complete cursor set that changes shape based on UI context (hovering, clicking, dragging, resizing, text selection, etc.).

## Current Implementation

| Aspect | Current State |
|--------|--------------|
| **File** | `src/kernel/drivers/mouse.c` |
| **Format** | `uint8_t cursor_data[19][12]` — 1-bit: `0`=transparent, `1`=black outline, `2`=white fill |
| **Shape** | Single arrow pointer (12×19 px) |
| **Hotspot** | Implicit top-left `(0, 0)` — no metadata |
| **Rendering** | `mouse_draw_cursor()` via `fb_put_pixel()`, save/restore under cursor |
| **Alpha** | None — fully opaque pixels only |
| **Context switching** | None — the WM has no cursor shape API |

## Proposed Cursor Shapes

| ID | Shape | Context | Size | Hotspot |
|----|-------|---------|------|---------|
| `CURSOR_ARROW` | Default pointer | Desktop, taskbar, menus | 16×24 | `(1, 1)` |
| `CURSOR_HAND` | Pointing hand | Clickable links, buttons (hover) | 20×24 | `(6, 1)` |
| `CURSOR_TEXT` | I-beam | Text input fields, terminal | 8×20 | `(4, 10)` |
| `CURSOR_MOVE` | 4-way arrows | Window dragging (title bar) | 20×20 | `(10, 10)` |
| `CURSOR_RESIZE_NS` | ↕ vertical | Top/bottom window edge resize | 12×20 | `(6, 10)` |
| `CURSOR_RESIZE_EW` | ↔ horizontal | Left/right window edge resize | 20×12 | `(10, 6)` |
| `CURSOR_RESIZE_NWSE` | ↘ diagonal | Top-left / bottom-right corner | 16×16 | `(8, 8)` |
| `CURSOR_RESIZE_NESW` | ↗ diagonal | Top-right / bottom-left corner | 16×16 | `(8, 8)` |
| `CURSOR_WAIT` | Hourglass / spinner | Busy / loading state | 16×24 | `(8, 12)` |
| `CURSOR_CROSSHAIR` | + crosshair | Selection, drawing (future) | 20×20 | `(10, 10)` |
| `CURSOR_FORBIDDEN` | ⊘ circle-slash | Invalid drop target, disabled | 20×20 | `(10, 10)` |

## Data Format Options

### Option A: Embedded byte arrays (current approach, extended)

```c
struct cursor_sprite {
    uint8_t  width;
    uint8_t  height;
    int8_t   hotspot_x;   /* click point offset from top-left */
    int8_t   hotspot_y;
    const uint8_t *data;  /* 2-bit per pixel: 0=transparent, 1=black, 2=white, 3=accent */
};
```

- **Pros**: No file I/O, instant access, tiny memory footprint (~500 bytes per cursor)
- **Cons**: Hard to edit, no alpha blending, limited to 4 colors

### Option B: Raw BGRA sprites in initrd (like start icon)

```c
struct cursor_sprite {
    uint8_t  width;
    uint8_t  height;
    int8_t   hotspot_x;
    int8_t   hotspot_y;
    const uint32_t *pixels; /* BGRA with full alpha channel */
};
```

- **Pros**: Full 32-bit color + alpha, can load beautiful anti-aliased cursors from PNG, easy to swap themes
- **Cons**: Larger (~2.5 KB per cursor at 24×24), requires initrd loading at boot

### Option C: Hybrid — embedded outlines + initrd themed overlays

Use Option A for boot/fallback, load Option B from initrd when available.

> [!IMPORTANT]
> **Recommendation: Option B (Raw BGRA from initrd).**
> We already have the PNG→raw pipeline (`jpg2raw`) and initrd zero-copy loading. A full cursor set at 24×24 BGRA is only ~27 KB total (11 cursors × 24×24×4). Alpha blending is already implemented in `desktop.c` for the start icon.

## Architecture Changes

### New files

| File | Purpose |
|------|---------|
| `include/cursor.h` | Cursor type enum, `cursor_sprite` struct, API declarations |
| `src/kernel/drivers/cursor.c` | Cursor manager: loading, switching, rendering with alpha |
| `resources/cursors/*.png` | Source PNG cursor images (build-time assets) |

### Modified files

| File | Changes |
|------|---------|
| `mouse.c` | Remove `cursor_data[]`, `mouse_draw_cursor()`, `mouse_save_under()`, `mouse_restore_under()` — delegate to `cursor.c` |
| `mouse.h` | Remove cursor rendering declarations, keep position/button API |
| `wm.c` | Call `cursor_set_shape()` based on hover context (title bar → MOVE, edge → RESIZE, etc.) |
| `desktop.c` | Call `cursor_set_shape(CURSOR_HAND)` on start button hover |
| `main.c` | Call `cursor_init()` during boot, use `cursor_draw()` / `cursor_restore()` in compositor loop |
| `Makefile` | Add PNG→raw conversion for each cursor, include in initrd |

### API Design

```c
/* cursor.h */

typedef enum {
    CURSOR_ARROW = 0,
    CURSOR_HAND,
    CURSOR_TEXT,
    CURSOR_MOVE,
    CURSOR_RESIZE_NS,
    CURSOR_RESIZE_EW,
    CURSOR_RESIZE_NWSE,
    CURSOR_RESIZE_NESW,
    CURSOR_WAIT,
    CURSOR_CROSSHAIR,
    CURSOR_FORBIDDEN,
    CURSOR_COUNT
} cursor_shape_t;

/* Initialize cursor subsystem — loads sprites from initrd */
void cursor_init(void);

/* Set the active cursor shape */
void cursor_set_shape(cursor_shape_t shape);

/* Get the current cursor shape */
cursor_shape_t cursor_get_shape(void);

/* Draw the cursor at (x, y) with alpha blending.
 * Saves pixels underneath for later restore. */
void cursor_draw(int32_t x, int32_t y);

/* Restore saved pixels — call before next composite */
void cursor_restore(void);

/* Get hotspot offset for accurate click positioning */
void cursor_get_hotspot(int8_t *hx, int8_t *hy);
```

## Context-Based Cursor Switching

The WM compositor loop determines the cursor shape based on mouse position:

```
1. If over desktop / wallpaper → CURSOR_ARROW
2. If over start button (hover) → CURSOR_HAND
3. If over menu item (hover) → CURSOR_HAND
4. If over window title bar → CURSOR_MOVE (while dragging: CURSOR_MOVE)
5. If over window edge (N/S) → CURSOR_RESIZE_NS
6. If over window edge (E/W) → CURSOR_RESIZE_EW
7. If over window corner → CURSOR_RESIZE_NWSE or CURSOR_RESIZE_NESW
8. If over window content → CURSOR_ARROW (or app-specific)
9. If system busy → CURSOR_WAIT
```

This logic belongs in `wm_composite()` or a new `wm_get_cursor_context()` function.

## Rendering Pipeline

```
┌─────────────────────────────────────────────────┐
│  Compositor Loop (main.c)                       │
│                                                 │
│  1. cursor_restore()     ← erase old cursor     │
│  2. wm_composite()       ← redraw scene         │
│  3. cursor_shape = wm_get_cursor_context(mx,my) │
│  4. cursor_set_shape(cursor_shape)               │
│  5. cursor_draw(mx, my)  ← draw new cursor      │
│  6. fb_swap()            ← present frame         │
└─────────────────────────────────────────────────┘
```

## Cursor Source Assets

Cursors can be:
1. **Designed by the user** — provide 24×24 or 32×32 PNGs with transparency
2. **Generated programmatically** — use the `generate_image` tool for each shape
3. **Drawn as byte arrays** — embedded fallback cursors for boot (before initrd is loaded)

> [!TIP]
> The user can provide a custom cursor theme as a set of PNGs dropped into `resources/cursors/`. The build system converts them automatically with `jpg2raw`.

## Hotspot Handling

The hotspot is the pixel within the cursor image that corresponds to the actual click point. Without it, all clicks register at the cursor's top-left corner, which is wrong for shapes like the hand, crosshair, or resize arrows.

**Current behavior**: Hotspot is implicitly `(0, 0)` — correct for the arrow cursor only.

**Required change**: The click position sent to `wm_handle_mouse()` must be adjusted by the hotspot offset:

```c
int32_t click_x = mouse_x + hotspot_x;
int32_t click_y = mouse_y + hotspot_y;
```

## Implementation Order

1. **Create `cursor.h` / `cursor.c`** with the sprite struct and loading from initrd
2. **Embed fallback arrow** as a byte array (for pre-initrd boot)
3. **Create cursor PNGs** in `resources/cursors/` (or accept user-provided ones)
4. **Update Makefile** with PNG→raw conversions and initrd inclusion
5. **Update `mouse.c`** — remove rendering, keep position tracking
6. **Update compositor loop** in `main.c` to use `cursor_draw()` / `cursor_restore()`
7. **Add context detection** in `wm.c` — `wm_get_cursor_context(mx, my)`
8. **Wire up context → shape** in the compositor loop
9. **Test each cursor shape** in the correct UI context

## Memory Budget

| Item | Size |
|------|------|
| 11 cursors × 24×24 × 4 bytes (BGRA) | ~25 KB in initrd |
| Active cursor pixel buffer | 2.3 KB (24×24×4) |
| Save-under buffer | 2.3 KB (24×24×4) |
| Sprite metadata (11 entries) | ~88 bytes |
| **Total runtime** | **~5 KB** |

Zero-copy loading from initrd means no heap allocation for cursor sprites.

## Open Questions

1. Should cursor shapes be **user-configurable** at runtime (cursor themes)?
2. Should we support **animated cursors** (e.g., spinning wait cursor)?
3. What **max cursor size** should we support? 24×24 is standard, 32×32 for HiDPI.
4. Should the **terminal app** set its own cursor shape (CURSOR_TEXT) via a syscall?
