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
│            Font Renderer (font.c)           │
│            8×16 PSF bitmap glyphs           │
├─────────────────────────────────────────────┤
│          Framebuffer (framebuffer.c)        │
│  GOP linear framebuffer, double buffering   │
├──────────────────────┬──────────────────────┤
│   Mouse (mouse.c)   │  Keyboard (kbd.c)    │
│   PS/2 IRQ 12       │  PS/2 IRQ 1          │
└──────────────────────┴──────────────────────┘
```

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
| Wallpaper | `wallpaper.raw` from initrd (fallback: gradient) |
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
