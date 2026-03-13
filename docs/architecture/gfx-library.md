# GFX Library — Architecture

## Overview

The GFX library (`src/kernel/gfx/`) provides 2D compositing, material effects, and
text rendering for the Impossible OS desktop. All routines operate on `gfx_surface_t`
surfaces and use integer-only math (no floating point outside SSE2 paths).

## Surface Model

```c
typedef struct {
    uint32_t *pixels;   /* BGRA pixel buffer (0xAARRGGBB) */
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;   /* pixels per row (may differ from width for alignment) */
} gfx_surface_t;
```

Color is `gfx_color_t` (`uint32_t`, 0xAARRGGBB) with macros: `GFX_RGBA()`,
`GFX_RGB()`, `GFX_ALPHA()`, `GFX_RED()`, `GFX_GREEN()`, `GFX_BLUE()`.

## Modules

### gfx_core.c — Primitives

| Function | Description |
|----------|-------------|
| `gfx_fill_rect` | Solid rectangle fill |
| `gfx_draw_rect` | Rectangle outline with thickness |
| `gfx_fill_rounded_rect` | Anti-aliased rounded corners |
| `gfx_draw_rounded_rect` | Rounded rectangle outline |
| `gfx_fill_circle` | Solid circle |
| `gfx_draw_line` | Bresenham line with thickness |

### gfx_blend.c — Alpha Blending

| Function | Description |
|----------|-------------|
| `gfx_blit` | Per-pixel alpha blit (pre-multiplied alpha) |
| `gfx_blit_alpha` | Blit with global alpha multiplier |
| `gfx_fill_rect_alpha` | Alpha-aware rectangle fill |

Uses pre-multiplied alpha (50% fewer multiplies). Integer-only `div255()`.

### gfx_gradient.c — Gradients

| Function | Description |
|----------|-------------|
| `gfx_fill_gradient_rect` | Linear gradient (horizontal/vertical) |
| `gfx_fill_gradient_rounded` | Gradient with rounded corners |
| Radial gradient fill | Radial color spread from center |

### gfx_blur.c — Box Blur

| Function | Description |
|----------|-------------|
| `gfx_blur_rect(s, x, y, w, h, radius)` | 2-pass separable box blur |

**Algorithm:** Two-pass (horizontal then vertical) with running sum per
scanline. Each pass is O(width) or O(height) per row/column — total cost is
O(w×h) regardless of blur radius. Uses a temporary scanline buffer from kmalloc.

### gfx_effects.c — Material Effects

| Function | Description |
|----------|-------------|
| `gfx_acrylic(s, x, y, w, h, tint, opacity, blur_radius)` | Windows 11 Acrylic |
| `gfx_mica(s, x, y, w, h, wallpaper, tint)` | Windows 11 Mica |
| `gfx_drop_shadow(s, x, y, w, h, radius, ox, oy, color)` | Soft multi-layer shadow |
| `gfx_reveal_highlight(s, rx, ry, rw, rh, mx, my, glow_r, color)` | Radial cursor glow |

**Acrylic** (taskbar, menus):
1. Box blur the region
2. Add noise texture (±8 per channel, xorshift32 PRNG)
3. Overlay tint color at opacity (`out = blur × (1-opacity) + tint × opacity`)

**Mica** (title bars):
1. Sample wallpaper at window position
2. Desaturate: 80% grayscale blend (`gray = 77R + 150G + 29B >> 8`, then `20% color + 80% gray`)
3. Tint: `20% desaturated + 80% theme color`

**Drop Shadow:**
Creates a temporary surface padded by `2×radius`, fills the center rectangle
with the shadow color, blurs it, then alpha-blits onto the target. The red
channel of the blurred shadow is used as an alpha proxy.

**Reveal Highlight:**
For each pixel within `glow_radius` of the mouse, intensity falls off linearly
with distance. Uses integer `isqrt()` for distance calculation.

## SIMD Acceleration

GFX files compile with `-msse2`. SSE2 paths exist for:
- Alpha blending (4 pixels/cycle)
- Gradient fills (4 pixels/cycle)
- Blur (4 pixels/cycle)

`fxsave`/`fxrstor` wrappers protect user FPU state during kernel SSE2 use.

## Memory Rules

All temporary surfaces and large buffers use `gfx_surface_create()` which
routes through `pmm_alloc_contiguous()` for allocations > 4 KB. Small
per-scanline buffers (e.g., blur temp) use `kmalloc`. See `rules.md` Known
Gotchas.
