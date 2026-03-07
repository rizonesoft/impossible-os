# 2D Compositing Library — Research

## Goal

Build a high-performance software 2D compositing engine that matches and enhances **Windows 11's visual language**: Mica/Acrylic materials, rounded corners, drop shadows, smooth gradients, transparency, blur effects, and fluid animations — all without GPU hardware.

---

## Windows 11 Visual Design System ("WinUI 3 / Fluent Design")

### Effects we need to replicate

| Effect | What it looks like | How it works |
|--------|-------------------|-------------|
| **Mica** | Subtle tinted wallpaper bleed-through in title bars | Sample wallpaper color under window, desaturate, tint |
| **Acrylic** | Frosted glass panels (start menu, context menus) | Gaussian blur of background + noise texture + tint overlay |
| **Rounded corners** | All windows have 8px corner radius | Anti-aliased arc rendering at corners |
| **Drop shadows** | Soft shadows behind windows | Multi-layer semi-transparent dark rects with increasing offset/opacity |
| **Reveal highlight** | Subtle light that follows mouse on buttons | Radial gradient centered on cursor position |
| **Smooth gradients** | Taskbar, menus, header backgrounds | Linear/radial gradient fill |
| **Depth / elevation** | Popups float above content | Z-ordered compositing with per-layer shadows |
| **Animations** | Menu open/close, window minimize/maximize | Eased interpolation over time (ease-in-out curves) |
| **Transparency** | Semi-transparent panels, overlays | Per-pixel alpha blending |
| **Backdrop blur** | Blurred content behind translucent surfaces | Fast box blur or Gaussian on framebuffer region |

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Application Layer                                      │
│  ├─ Windows (wm.c)                                     │
│  ├─ Desktop shell (desktop.c)                          │
│  └─ UI widgets (buttons, menus, panels)                │
├─────────────────────────────────────────────────────────┤
│  Compositing Library (gfx.h / gfx.c)                   │
│  ├─ Primitives: rect, rounded rect, line, circle       │
│  ├─ Fill modes: solid, gradient, textured              │
│  ├─ Effects: alpha blend, blur, shadow, glow           │
│  ├─ Text: TTF rendering via stb_truetype               │
│  └─ Animation: easing, tweening, timer-driven          │
├─────────────────────────────────────────────────────────┤
│  Rendering Backend                                      │
│  ├─ Software rasterizer (default)                      │
│  ├─ SSE2/AVX2 SIMD fast paths                         │
│  └─ Dirty-rectangle tracking                           │
├─────────────────────────────────────────────────────────┤
│  Framebuffer (fb.c)                                     │
│  └─ fb_put_pixel, fb_fill_rect, fb_read_pixel          │
└─────────────────────────────────────────────────────────┘
```

---

## API Design

### Core types

```c
/* gfx.h */

/* ARGB color with alpha */
typedef uint32_t gfx_color_t;  /* 0xAARRGGBB */

#define GFX_RGBA(r,g,b,a) (((uint32_t)(a)<<24)|((uint32_t)(r)<<16)|\
                           ((uint32_t)(g)<<8)|(uint32_t)(b))
#define GFX_RGB(r,g,b)    GFX_RGBA(r,g,b,255)
#define GFX_ALPHA(c)      (((c)>>24)&0xFF)

/* Surface — an in-memory pixel buffer */
typedef struct {
    uint32_t *pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;  /* pixels per row (may differ from width) */
} gfx_surface_t;

/* Gradient definition */
typedef struct {
    gfx_color_t start;
    gfx_color_t end;
    enum { GFX_GRAD_VERTICAL, GFX_GRAD_HORIZONTAL,
           GFX_GRAD_RADIAL } direction;
} gfx_gradient_t;

/* Animation easing */
typedef enum {
    GFX_EASE_LINEAR,
    GFX_EASE_IN_QUAD,
    GFX_EASE_OUT_QUAD,
    GFX_EASE_IN_OUT_QUAD,
    GFX_EASE_IN_CUBIC,
    GFX_EASE_OUT_CUBIC,
    GFX_EASE_IN_OUT_CUBIC,
    GFX_EASE_BOUNCE
} gfx_easing_t;
```

### Primitive drawing

```c
/* Solid fills */
void gfx_fill_rect(gfx_surface_t *s, int x, int y, int w, int h,
                    gfx_color_t color);
void gfx_fill_rounded_rect(gfx_surface_t *s, int x, int y, int w, int h,
                            int radius, gfx_color_t color);
void gfx_fill_circle(gfx_surface_t *s, int cx, int cy, int radius,
                      gfx_color_t color);

/* Outlined shapes */
void gfx_draw_rect(gfx_surface_t *s, int x, int y, int w, int h,
                    int thickness, gfx_color_t color);
void gfx_draw_rounded_rect(gfx_surface_t *s, int x, int y, int w, int h,
                            int radius, int thickness, gfx_color_t color);
void gfx_draw_line(gfx_surface_t *s, int x1, int y1, int x2, int y2,
                    int thickness, gfx_color_t color);

/* Gradient fills */
void gfx_fill_gradient_rect(gfx_surface_t *s, int x, int y, int w, int h,
                              gfx_gradient_t *grad);
void gfx_fill_gradient_rounded(gfx_surface_t *s, int x, int y, int w, int h,
                                 int radius, gfx_gradient_t *grad);
```

### Alpha blending & compositing

```c
/* Blend src onto dst with per-pixel alpha */
void gfx_blit(gfx_surface_t *dst, int dx, int dy,
              const gfx_surface_t *src, int sx, int sy, int w, int h);

/* Blit with additional global alpha (0-255) */
void gfx_blit_alpha(gfx_surface_t *dst, int dx, int dy,
                    const gfx_surface_t *src, uint8_t alpha);

/* Fill rect with alpha transparency */
void gfx_fill_rect_alpha(gfx_surface_t *s, int x, int y, int w, int h,
                          gfx_color_t color);  /* alpha from color's A channel */
```

### Effects (Windows 11 look)

```c
/* ---- Blur ---- */
/* Fast box blur (radius 1-20, O(n) per pixel regardless of radius) */
void gfx_blur_rect(gfx_surface_t *s, int x, int y, int w, int h,
                    int radius);

/* ---- Acrylic material (frosted glass) ---- */
/* Blur background + noise + tint overlay */
void gfx_acrylic(gfx_surface_t *s, int x, int y, int w, int h,
                  gfx_color_t tint, uint8_t tint_opacity,
                  int blur_radius);

/* ---- Mica material (wallpaper tint) ---- */
/* Sample wallpaper, desaturate, tint, apply to region */
void gfx_mica(gfx_surface_t *s, int x, int y, int w, int h,
              const gfx_surface_t *wallpaper, gfx_color_t tint);

/* ---- Drop shadow ---- */
/* Multi-layer soft shadow behind a rectangle */
void gfx_drop_shadow(gfx_surface_t *s, int x, int y, int w, int h,
                      int radius, int offset_x, int offset_y,
                      gfx_color_t shadow_color);

/* ---- Reveal highlight (follows mouse) ---- */
void gfx_reveal_highlight(gfx_surface_t *s, int rect_x, int rect_y,
                           int rect_w, int rect_h,
                           int mouse_x, int mouse_y,
                           int glow_radius, gfx_color_t highlight);
```

### Animation engine

```c
/* Tween — interpolate a value over time */
typedef struct {
    float   from;
    float   to;
    float   current;
    float   duration_ms;
    float   elapsed_ms;
    gfx_easing_t easing;
    uint8_t active;
} gfx_tween_t;

void gfx_tween_start(gfx_tween_t *tw, float from, float to,
                      float duration_ms, gfx_easing_t easing);
void gfx_tween_update(gfx_tween_t *tw, float delta_ms);
float gfx_tween_value(const gfx_tween_t *tw);

/* Example: animate window minimize */
/* gfx_tween_start(&win->anim_y, win->y, taskbar_y, 200, GFX_EASE_OUT_CUBIC); */
```

---

## Algorithm Details

### Fast Box Blur (O(n) per pixel, any radius)

The trick: sliding window. Instead of sampling `radius × radius` pixels per output pixel, maintain a running sum:

```c
/* Horizontal pass */
for each row:
    sum = first_pixel * radius;
    for x = 0..width:
        sum += pixel[x + radius];
        sum -= pixel[x - radius];
        out[x] = sum / (2*radius + 1);

/* Vertical pass: identical, but iterate columns */
```

Two passes (horizontal + vertical) give results visually identical to Gaussian blur at ~1/100th the cost. A 1280×720 blur with radius 15 takes <2ms on modern x86-64.

### Rounded Rectangle (anti-aliased)

```c
void gfx_fill_rounded_rect(surface, x, y, w, h, r, color) {
    /* For each pixel in the corner regions: */
    /*   distance = sqrt((px-cx)² + (py-cy)²) */
    /*   if distance <= r: fully inside → draw solid */
    /*   if distance <= r+1: anti-alias → draw with partial alpha */
    /*   else: outside → skip */
    /* For the center/edge rectangles: draw solid (fast path) */
}
```

### Mica Material

```c
void gfx_mica(surface, x, y, w, h, wallpaper, tint) {
    for each pixel in region:
        /* 1. Sample wallpaper at (x, y) */
        wp = wallpaper_pixel(x, y);
        /* 2. Desaturate (convert to grayscale blend) */
        gray = (R*0.3 + G*0.6 + B*0.1);
        desat = lerp(wp, gray, 0.8);  /* 80% desaturated */
        /* 3. Tint with window theme color */
        out = alpha_blend(desat, tint, tint_alpha);
}
```

### Acrylic Material

```c
void gfx_acrylic(surface, x, y, w, h, tint, opacity, blur_r) {
    /* 1. Copy region to temp buffer */
    /* 2. Apply box blur (2-pass, radius blur_r) */
    /* 3. Add noise texture (subtle 2-3% random variation) */
    /* 4. Overlay tint color at opacity */
    /* 5. Write back to surface */
}
```

---

## Performance Optimization

### Tier 1: Baseline software (works now)

| Technique | Benefit |
|-----------|---------|
| Dirty rectangles | Only redraw changed regions, 5-20× less work |
| Pre-multiplied alpha | 50% fewer multiplies in blending |
| Integer-only math | No floating point in the hot path |
| `memcpy`/`memset` for opaque blits | ~10 GB/s throughput |

### Tier 2: SIMD acceleration

| Technique | Benefit |
|-----------|---------|
| SSE2 alpha blending | 4 pixels per cycle → 4× faster |
| SSE2 gradient fill | 4 pixels per cycle |
| SSE2 blur | 4 pixels per cycle |
| AVX2 (if available) | 8 pixels per cycle → 8× faster |

```c
/* SSE2 alpha blend — 4 pixels at once */
static inline __m128i sse2_blend(__m128i src, __m128i dst) {
    __m128i alpha = _mm_shufflelo_epi16(src, ...); /* broadcast alpha */
    __m128i inv   = _mm_sub_epi16(_mm_set1_epi16(255), alpha);
    __m128i s     = _mm_mullo_epi16(src_lo, alpha);
    __m128i d     = _mm_mullo_epi16(dst_lo, inv);
    return _mm_srli_epi16(_mm_add_epi16(s, d), 8);
}
```

**SSE in kernel**: Compile the `gfx.c` module with `-msse2` separately. Wrap usage with `fxsave`/`fxrstor` to protect user-mode FPU state.

### Tier 3: Compositor-level optimizations

| Technique | Benefit |
|-----------|---------|
| Back buffer double-buffering | No tearing |
| Cached window surfaces | Windows re-render only when content changes |
| Pre-rendered shadows | Shadow bitmap computed once per window size |
| Blur cache | Re-blur only when background changes |
| Compositing tree | Skip occluded windows entirely |

---

## Performance Targets

| Operation | Target (1280×720) | Pixels/frame |
|-----------|-------------------|-------------|
| Full-screen gradient | <1ms | 921,600 |
| Full-screen alpha blit | <2ms | 921,600 |
| Blur (radius 15) | <3ms | 921,600 |
| Rounded rect (200×150) | <0.1ms | 30,000 |
| Drop shadow | <0.5ms | ~60,000 |
| Full compositor frame | **<8ms (120fps)** | varies |

At 16.6ms per frame budget (60fps), we have plenty of headroom.

---

## Visual Enhancements Beyond Windows 11

| Effect | Description |
|--------|-------------|
| **Glassmorphism** | More aggressive blur + translucency than Acrylic |
| **Neon glow** | Colored glow around focused elements |
| **Gradient borders** | Border color transitions along the edge |
| **Parallax wallpaper** | Subtle wallpaper shift when moving windows |
| **Morphing animations** | Windows smoothly transform shape on open/close |
| **Particle effects** | Subtle particles on desktop (snow, fireflies) |
| **Dynamic wallpaper** | Time-of-day tinted wallpaper |

---

## Files

| Action | File | Lines (est.) | Purpose |
|--------|------|-------------|---------|
| **[NEW]** | `include/gfx.h` | ~150 | Types, API declarations |
| **[NEW]** | `src/kernel/gfx/gfx_core.c` | ~400 | Primitives: rect, rounded rect, circle, line |
| **[NEW]** | `src/kernel/gfx/gfx_blend.c` | ~200 | Alpha blending, pre-multiplied, SIMD |
| **[NEW]** | `src/kernel/gfx/gfx_gradient.c` | ~150 | Linear, radial, conic gradients |
| **[NEW]** | `src/kernel/gfx/gfx_blur.c` | ~200 | Box blur, frosted glass |
| **[NEW]** | `src/kernel/gfx/gfx_effects.c` | ~300 | Mica, Acrylic, shadow, reveal, glow |
| **[NEW]** | `src/kernel/gfx/gfx_animate.c` | ~150 | Tweening, easing curves |
| **[NEW]** | `src/kernel/gfx/gfx_text.c` | ~200 | stb_truetype integration |
| **[MODIFY]** | `src/desktop/desktop.c` | ~50 | Use gfx_ API for taskbar/menus |
| **[MODIFY]** | `src/kernel/wm.c` | ~100 | Use gfx_ API for window decorations |

**Total**: ~1750 lines of new compositing code.

---

## Implementation Order

### Phase 1: Core primitives (3-5 days)
- [ ] `gfx_surface_t` and surface management
- [ ] `gfx_fill_rect`, `gfx_fill_rect_alpha` with pre-multiplied alpha
- [ ] `gfx_blit`, `gfx_blit_alpha`
- [ ] `gfx_fill_rounded_rect` with anti-aliasing
- [ ] `gfx_draw_line`, `gfx_draw_rect`
- [ ] Dirty rectangle tracker

### Phase 2: Gradients + shadows (2-3 days)
- [ ] `gfx_fill_gradient_rect` (linear vertical/horizontal)
- [ ] `gfx_fill_gradient_rounded`
- [ ] `gfx_drop_shadow` (multi-layer)
- [ ] Apply to window decorations in `wm.c`

### Phase 3: Blur + materials (3-5 days)
- [ ] `gfx_blur_rect` (2-pass box blur)
- [ ] `gfx_acrylic` (blur + noise + tint)
- [ ] `gfx_mica` (wallpaper sample + desaturate + tint)
- [ ] Apply to taskbar, start menu, context menus

### Phase 4: Animation engine (2-3 days)
- [ ] `gfx_tween_t` with easing functions
- [ ] Window open/close animations
- [ ] Menu slide-in animations
- [ ] Hover reveal highlight

### Phase 5: SIMD optimization (2-3 days)
- [ ] Enable SSE2 for gfx module (`-msse2`)
- [ ] `fxsave`/`fxrstor` wrappers
- [ ] SIMD alpha blending (4 pixels/cycle)
- [ ] SIMD blur + gradient
- [ ] Benchmark and tune

### Phase 6: TrueType fonts (3-5 days)
- [ ] Integrate stb_truetype
- [ ] Font atlas caching (pre-rasterize common sizes)
- [ ] Anti-aliased text rendering
- [ ] Bundle Inter or JetBrains Mono font
