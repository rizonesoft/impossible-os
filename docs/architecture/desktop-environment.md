# Graphics & Desktop Environment

Impossible OS provides a full graphical desktop with a stacking window manager,
taskbar, start menu, mouse cursor, and common GUI controls — all rendered on a
UEFI GOP framebuffer with double buffering.

## Architecture

```
┌─────────────────────────────────────────────┐
│              Desktop Shell                  │
│  (desktop.c — wallpaper, taskbar, start     │
│   menu, app launcher, clock)                │
├────────────────┬────────────────────────────┤
│  GUI Controls  │    Window Manager          │
│ (controls.c)   │  (wm.c — stacking,        │
│ Button, Label, │   dragging, decorations,   │
│ TextBox,       │   z-order, compositing)    │
│ ScrollBar      │                            │
├────────────────┴────────────────────────────┤
│       2D Compositing Library (gfx.h)        │
│  gfx_surface_t, primitives, alpha blend,    │
│  rounded rect, dirty rect tracker           │
├─────────────────────────────────────────────┤
│       TrueType Font System (font_mgr.h)    │
│  Selawik (UI) + Cascadia Code (mono) +      │
│  Inter (fallback) via stb_truetype           │
├─────────────────────────────────────────────┤
│          Framebuffer (framebuffer.c)        │
│  GOP linear framebuffer, double buffering   │
├──────────────────────┬──────────────────────┤
│   Mouse (mouse.c)   │  Keyboard (kbd.c)    │
│   PS/2 IRQ 12       │  PS/2 IRQ 1          │
└──────────────────────┴──────────────────────┘
```

## 2D Compositing Library (GFX)

Hardware-independent surface abstraction for all 2D rendering.

### Key Files

| File | Purpose |
|------|---------|
| `include/gfx.h` | Types, color macros, API declarations |
| `src/kernel/gfx/gfx_core.c` | Primitives implementation |

### Color Format

| Macro | Description |
|-------|-------------|
| `GFX_RGBA(r,g,b,a)` | Construct 0xAARRGGBB color |
| `GFX_RGB(r,g,b)` | Opaque color (alpha=0xFF) |
| `GFX_ALPHA(c)` / `GFX_RED(c)` / `GFX_GREEN(c)` / `GFX_BLUE(c)` | Extract channels |

### Surface API

| Function | Description |
|----------|-------------|
| `gfx_surface_init(s, pixels, w, h, stride)` | Wrap existing buffer |
| `gfx_surface_create(s, w, h)` | Allocate new surface (kmalloc) |
| `gfx_surface_destroy(s)` | Free allocated surface |
| `gfx_clear(s, color)` | Fill entire surface |

### Drawing Primitives

| Function | Description |
|----------|-------------|
| `gfx_fill_rect` | Solid filled rectangle (clipped) |
| `gfx_draw_rect` | Outline rectangle with thickness |
| `gfx_fill_rounded_rect` | Rounded corners (isqrt-based) |
| `gfx_draw_rounded_rect` | Rounded outline |
| `gfx_fill_circle` | Midpoint circle algorithm |
| `gfx_draw_line` | Bresenham line with thickness |
| `gfx_put_pixel` / `gfx_blend_pixel` | Single pixel (opaque / alpha-blended) |

### Dirty Rectangle Tracker

Tracks up to 32 dirty regions for partial redraws. On overflow, merges all into one bounding box.

### Alpha Blending & Compositing

| File | Purpose |
|------|---------|
| `src/kernel/gfx/gfx_blend.c` | Blit, alpha-blit, alpha-filled rect |

| Function | Description |
|----------|-------------|
| `gfx_blit(dst, dx, dy, src, sx, sy, w, h)` | Per-pixel alpha blit with full clipping |
| `gfx_blit_alpha(dst, dx, dy, src, alpha)` | Blit with global alpha multiplier (0–255) |
| `gfx_fill_rect_alpha(s, x, y, w, h, color)` | Alpha-filled rect from color channel |

**Design:** Pre-multiplied alpha (`out = src + dst × (1 − α)`), integer-only `div255()` approximation, no floating point.

### Gradient Fills

| File | Purpose |
|------|---------|
| `src/kernel/gfx/gfx_gradient.c` | Linear and radial gradient rendering |

| Type / Function | Description |
|-----------------|-------------|
| `gfx_gradient_t` | Struct: start color, end color, direction (V/H) |
| `gfx_fill_gradient_rect` | Linear gradient (vertical or horizontal) |
| `gfx_fill_gradient_rounded` | Linear gradient with rounded corners |
| `gfx_fill_radial_gradient` | Radial gradient (center → edge) |

Colors interpolated via integer-only `color_lerp()` (per-channel linear blend at t/255).

### Blur & Material Effects

| File | Purpose |
|------|---------|
| `src/kernel/gfx/gfx_blur.c` | 2-pass separable box blur (O(n) per pixel) |
| `src/kernel/gfx/gfx_effects.c` | Acrylic, Mica, Drop Shadow, Reveal Highlight |

| Function | Description |
|----------|-------------|
| `gfx_blur_rect` | Box blur with running sum (horizontal + vertical pass) |
| `gfx_acrylic` | Blur → noise (±8) → tint overlay at opacity |
| `gfx_mica` | Wallpaper sample → 80% desaturate → 50% tint blend |
| `gfx_drop_shadow` | Blurred rect composited behind element with offset |
| `gfx_reveal_highlight` | Radial glow at cursor (linear falloff within rect) |

**Usage targets:** Acrylic → taskbar, start menu, menus. Mica → window title bars. Shadow → all windows.

### SIMD Acceleration

| File | Purpose |
|------|---------|
| `include/gfx_simd.h` | FPU state types, CPUID detection, SSE2 function signatures |
| `src/kernel/gfx/gfx_simd.c` | SSE2 implementations (compiled with `-msse2`) |

| Function | Description |
|----------|-------------|
| `simd_enable_sse` | Set CR0/CR4 bits for SSE support |
| `simd_save_state` / `simd_restore_state` | `fxsave`/`fxrstor` (512-byte, 16-byte aligned) |
| `simd_has_sse2` / `simd_has_avx2` | CPUID feature detection |
| `simd_blend_pixels_sse2` | 4-pixel alpha blend via XMM unpack/mul/pack |
| `simd_gradient_row_sse2` | 4-pixel gradient interpolation |
| `simd_blur_accum_sse2` | 4-pixel blur accumulation via packed 32-bit add |

**Build:** `gfx_simd.c` uses `SIMD_CFLAGS` (filters out `-mno-sse` and adds `-msse2`).

### TrueType Font System

| File | Purpose |
|------|---------|
| `include/stb_truetype.h` | Sean Barrett's public-domain TTF parser (5079 lines) |
| `include/kernel/kmath.h` | Software float math shim (sqrt, pow, cos, acos, fabs, floor, ceil) |
| `include/font_mgr.h` | Font manager API: slot-based loading, draw, measure |
| `src/kernel/gfx/stb_truetype_impl.c` | Compilation unit with all stdlib→kernel redirects |
| `src/kernel/gfx/gfx_text.c` | Font loading (VFS) + glyph rendering + kerning |

| Function | Description |
|----------|-------------|
| `ttf_mgr_init` | Load TTF fonts from `C:\Impossible\Fonts\` into slots + build glyph cache |
| `ttf_get(slot, px)` | Return scaled font handle for a given pixel size |
| `ttf_draw_char` | Render single glyph via cache or stb_truetype fallback |
| `ttf_draw_string` | Render string with glyph cache + kerning |
| `ttf_measure_width` | Measure text width (cached advance values) |
| `ttf_line_height` | Get line height (ascent − descent + gap, scaled) |

**Rendering architecture:** All GUI text (`desktop.c`, `wm.c`, `controls.c`) uses TrueType via `gfx_surface_t` — screen back buffer wrapped via `fb_get_backbuffer()`, per-window framebuffers wrapped inline. The 8×16 bitmap font (`font.c`/`font.h`) is retained for pre-initrd early boot console output and the terminal emulator.

**Font slots:**

| Slot | Macro | Font | File |
|------|-------|------|------|
| 0 | `FONT_UI` | Selawik Regular | `selawk.ttf` |
| 1 | `FONT_UI_BOLD` | Selawik Semibold | `selawksb.ttf` |
| 2 | `FONT_MONO` | Cascadia Code Regular | `CascadiaCode-Regular.ttf` |
| 3 | `FONT_MONO_BOLD` | Cascadia Code Bold | `CascadiaCode-Bold.ttf` |
| 4 | `FONT_UI_HEAVY` | Selawik Bold | `selawkb.ttf` |

**Fallback chain:** If primary font not found, slots 0–1 fall back to Inter Regular/Bold, slots 2–3 fall back to Selawik.

**Bundled fonts** (in `resources/fonts/`):

| Font | License | Files |
|------|---------|-------|
| Selawik | MIT | `selawk.ttf`, `selawksb.ttf`, `selawkb.ttf` |
| Cascadia Code | OFL 1.1 | `CascadiaCode-Regular.ttf`, `CascadiaCode-Bold.ttf` |
| Inter | OFL 1.1 | `Inter-Regular.ttf`, `Inter-Bold.ttf` |

**Glyph Cache:**

Pre-rasterizes ASCII 32–126 at boot for 5 common pixel sizes (12, 14, 16, 20, 24px) across all loaded font slots. Eliminates per-frame `kmalloc`/`kfree` and stb_truetype rasterization for common text.

| Property | Value |
|----------|-------|
| Cache entries | 5 slots × 5 sizes × 95 chars = 2375 max |
| Entry struct | `glyph_entry_t` — bitmap ptr, width, height, x/y offset, advance |
| Lookup | O(1) by `[slot][size_index][codepoint - 32]` |
| Fallback | Non-ASCII and uncached sizes fall through to stb_truetype |

**Build:** Both `stb_truetype_impl.c` and `gfx_text.c` compiled with `-msse2`. FPU state protected via `fxsave`/`fxrstor`.

## Framebuffer Graphics

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/framebuffer.c` | Pixel framebuffer driver |
| `include/kernel/drivers/framebuffer.h` | Framebuffer API |

### Configuration

| Property | Value |
|----------|-------|
| Resolution | 1280×720 (set via QEMU `-device VGA,xres=1280,yres=720`) |
| Color depth | 32 bpp (ARGB 8888) |
| Source | UEFI GOP via Multiboot2 framebuffer tag |
| Buffering | **Double buffered** (back buffer in kmalloc heap) |

### Drawing API

| Function | Description |
|----------|-------------|
| `fb_put_pixel(x, y, color)` | Set a single pixel |
| `fb_fill_rect(x, y, w, h, color)` | Filled rectangle |
| `fb_draw_rect(x, y, w, h, color)` | Rectangle outline |
| `fb_draw_line(x1, y1, x2, y2, color)` | Line (Bresenham) |
| `fb_draw_circle(cx, cy, r, color)` | Circle outline |
| `fb_fill_circle(cx, cy, r, color)` | Filled circle |
| `fb_blit(x, y, w, h, pixels)` | Block pixel copy |
| `fb_swap()` | Copy back buffer → front buffer |
| `fb_clear()` | Clear screen |

### Double Buffering

All drawing goes to the **back buffer** (in RAM). `fb_swap()` copies the
complete back buffer to the video memory in one operation, eliminating
tearing and flicker.

## Font Rendering

### Key Files

| File | Purpose |
|------|---------|
| `src/desktop/font.c` | Bitmap font renderer |
| `include/desktop/font.h` | Font API |

### Font Properties

| Property | Value |
|----------|-------|
| Format | PSF bitmap |
| Glyph size | 8×16 pixels |
| Character range | ASCII 32–126 (95 printable glyphs) |
| Colors | Configurable foreground + background |

### API

| Function | Description |
|----------|-------------|
| `font_draw_char(x, y, c, fg, bg)` | Draw one character |
| `font_draw_string(x, y, str, fg, bg)` | Draw a string |
| `font_draw_string_n(x, y, str, n, fg, bg)` | Draw N characters |
| `font_measure_string(str)` | Get pixel width of string |

## Mouse Driver (PS/2)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/mouse.c` | PS/2 mouse driver |
| `include/kernel/drivers/mouse.h` | Mouse API |

### Configuration

| Property | Value |
|----------|-------|
| IRQ | 12 (vector 44) |
| Sample rate | 100 samples/second |
| Resolution | 4 counts/mm |
| Packet format | 3 bytes (status, dx, dy) |
| Cursor sprite | 12×19 pixel arrow |

### How It Works

1. IRQ 12 fires → read 3-byte packet from port `0x60`
2. Parse signed 9-bit dx/dy deltas and button states (left, right, middle)
3. Update global cursor position (clamped to screen bounds)
4. Cursor sprite composited on top of the desktop by the window manager

QEMU also provides a `virtio-tablet-pci` device for absolute cursor
positioning (no mouse capture needed).

## Window Manager

### Key Files

| File | Purpose |
|------|---------|
| `src/desktop/wm.c` | Stacking window manager |
| `include/desktop/wm.h` | Window struct and WM API |

### Window Structure

| Field | Description |
|-------|-------------|
| `x`, `y` | Position on screen |
| `width`, `height` | Content area dimensions |
| `title` | Title bar text |
| `framebuffer` | Window's private pixel buffer |
| `z_order` | Stacking depth |
| `flags` | Visible, minimized, maximized, focused |

### Features

| Feature | Description |
|---------|-------------|
| Window creation/destruction | `wm_create_window()` / `wm_destroy_window()` |
| Dragging | Click title bar → drag to move |
| Stacking | Click to bring-to-front (z-order) |
| Decorations | Title bar, close/minimize/maximize buttons, border |
| Compositing | Painter's algorithm (back-to-front) |
| Dirty tracking | Only recompose when content changes |
| Focus | Active window receives keyboard input |

### Compositing

Each frame:
1. Draw wallpaper (background)
2. Draw windows back-to-front (painter's algorithm)
3. Draw taskbar (always on top)
4. Draw mouse cursor (topmost)
5. `fb_swap()` → display

## Desktop Shell

### Key Files

| File | Purpose |
|------|---------|
| `src/desktop/desktop.c` | Desktop environment |
| `include/desktop/desktop.h` | Desktop API |

### Components

| Component | Description |
|-----------|-------------|
| Wallpaper | `wallpaper.raw` from C:\\ (fallback: gradient) |
| Taskbar | Bottom bar: start button + window list + clock |
| Start menu | Terminal, About, Shutdown |
| Clock | Real-time HH:MM from CMOS RTC |
| App launcher | Click start menu items to launch programs |

### Taskbar Layout

```
┌──────────────────────────────────────────────┐
│ [Start] │ Terminal │ About │        │ 19:47  │
└──────────────────────────────────────────────┘
```

## GUI Controls

### Key Files

| File | Purpose |
|------|---------|
| `src/desktop/controls.c` | Common controls library (1228 lines) |
| `include/desktop/controls.h` | Control structs and API |

### Available Controls

| Control | Features |
|---------|----------|
| **Button** | Draw, hover highlight, pressed/disabled states, click callback |
| **Label** | Static text, transparent background |
| **TextBox** | Editable single-line input, cursor, scroll, insert/delete |
| **ScrollBar** | Vertical/horizontal, proportional thumb, drag support |

### Event Routing

1. Mouse/keyboard events enter the window manager
2. WM dispatches to the focused window
3. Window dispatches to the focused control via hit-testing
4. Control handles the event and triggers callbacks
