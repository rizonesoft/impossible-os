# Phase 02 — UI Framework

> **Goal:** Transform the basic framebuffer desktop into a modern, Windows 11-quality
> graphical experience with compositing effects, TrueType fonts, image decoding,
> system icons, context-aware cursors, DPI scaling, and fluid animations.

---

## 1. 2D Compositing Library
> *Research: [01_2d_compositor.md](research/phase_02_ui_framework/01_2d_compositor.md)*

### 1.1 Core Surface & Primitives

- [ ] Define `gfx_surface_t` struct (pixels, width, height, stride)
- [ ] Define `gfx_color_t` (0xAARRGGBB) with `GFX_RGBA()`, `GFX_RGB()`, `GFX_ALPHA()` macros
- [ ] Create `include/gfx.h` with all type/API declarations
- [ ] Create `src/kernel/gfx/gfx_core.c`
- [ ] Implement `gfx_fill_rect(surface, x, y, w, h, color)` — solid fill
- [ ] Implement `gfx_draw_rect(surface, x, y, w, h, thickness, color)` — outline
- [ ] Implement `gfx_fill_rounded_rect(surface, x, y, w, h, radius, color)` — anti-aliased corners
- [ ] Implement `gfx_draw_rounded_rect(surface, x, y, w, h, radius, thickness, color)`
- [ ] Implement `gfx_fill_circle(surface, cx, cy, r, color)`
- [ ] Implement `gfx_draw_line(surface, x1, y1, x2, y2, thickness, color)` — Bresenham
- [ ] Implement dirty rectangle tracker for partial redraws
- [ ] Commit: `"gfx: core surface and primitive drawing"`

### 1.2 Alpha Blending & Compositing

- [ ] Create `src/kernel/gfx/gfx_blend.c`
- [ ] Implement `gfx_blit(dst, dx, dy, src, sx, sy, w, h)` — per-pixel alpha blit
- [ ] Implement `gfx_blit_alpha(dst, dx, dy, src, alpha)` — blit with global alpha
- [ ] Implement `gfx_fill_rect_alpha(surface, x, y, w, h, color)` — alpha from color channel
- [ ] Use pre-multiplied alpha (50% fewer multiplies in hot path)
- [ ] Integer-only math in blending (no floating point)
- [ ] Commit: `"gfx: alpha blending and compositing"`

### 1.3 Gradients

- [ ] Create `src/kernel/gfx/gfx_gradient.c`
- [ ] Define `gfx_gradient_t` struct (start color, end color, direction)
- [ ] Implement `gfx_fill_gradient_rect()` — vertical + horizontal linear gradients
- [ ] Implement `gfx_fill_gradient_rounded()` — gradient with rounded corners
- [ ] Implement radial gradient fill
- [ ] Commit: `"gfx: gradient fills"`

### 1.4 Blur & Material Effects

- [ ] Create `src/kernel/gfx/gfx_blur.c` and `gfx_effects.c`
- [ ] Implement `gfx_blur_rect(surface, x, y, w, h, radius)` — 2-pass box blur (O(n) per pixel)
- [ ] Implement `gfx_acrylic(surface, x, y, w, h, tint, opacity, blur_radius)`:
  - [ ] Copy region to temp buffer
  - [ ] Apply box blur
  - [ ] Add noise texture (2–3% random variation)
  - [ ] Overlay tint color at opacity
- [ ] Implement `gfx_mica(surface, x, y, w, h, wallpaper, tint)`:
  - [ ] Sample wallpaper at position
  - [ ] Desaturate (80% grayscale blend)
  - [ ] Tint with theme color
- [ ] Implement `gfx_drop_shadow(surface, x, y, w, h, radius, offset_x, offset_y, color)` — multi-layer soft shadow
- [ ] Implement `gfx_reveal_highlight(surface, rect, mouse_x, mouse_y, glow_radius, highlight)` — radial glow following cursor
- [ ] Apply Mica to window title bars
- [ ] Apply Acrylic to taskbar, start menu, context menus
- [ ] Pre-render and cache shadow bitmaps per window size
- [ ] Commit: `"gfx: blur, Mica, Acrylic, and shadow effects"`

### 1.5 SIMD Optimization

- [ ] Enable SSE2 for gfx module: compile with `-msse2` separately
- [ ] Implement `fxsave`/`fxrstor` wrappers to protect user FPU state
- [ ] SSE2 alpha blending — 4 pixels per cycle
- [ ] SSE2 gradient fill — 4 pixels per cycle
- [ ] SSE2 blur — 4 pixels per cycle
- [ ] *(Stretch)* AVX2 paths — 8 pixels per cycle (detect at runtime with CPUID)
- [ ] Benchmark: target <8ms full compositor frame at 1280×720
- [ ] Commit: `"gfx: SSE2 SIMD acceleration"`

---

## 2. TrueType Font System
> *Research: [02_font_system.md](research/phase_02_ui_framework/02_font_system.md)*

### 2.1 stb_truetype Integration

- [ ] Add `stb_truetype.h` to `include/` (public domain)
- [ ] Redirect memory: `STBTT_malloc → kmalloc`, `STBTT_free → kfree`
- [ ] Create `src/kernel/gfx/gfx_text.c` and `include/font_mgr.h`
- [ ] Implement `font_mgr_init()` — load fonts from initrd at boot
- [ ] Implement `font_get(slot, pixel_size)` — return scaled font handle
- [ ] Implement `font_draw_char(surface, font, x, y, codepoint, color)` — rasterize + alpha blend
- [ ] Implement `font_draw_string(surface, font, x, y, text, color)` — with kerning
- [ ] Implement `font_measure_width(font, text)` — text width measurement
- [ ] Implement `font_line_height(font)` — get line height
- [ ] Commit: `"desktop: stb_truetype integration"`

### 2.2 Font Bundle

- [ ] Download **Selawik** Regular + Semibold (~380 KB total, MIT license)
- [ ] Download **Cascadia Code** Regular + Bold (~580 KB total, OFL 1.1)
- [ ] Place `.ttf` files in `resources/fonts/`
- [ ] Update Makefile to copy fonts into initrd
- [ ] Define font slots: `FONT_UI`, `FONT_UI_BOLD`, `FONT_MONO`, `FONT_MONO_BOLD`
- [ ] Add font license files to `resources/fonts/LICENSE-*`
- [ ] *(Stretch)* Add **Inter** as an alternative UI font
- [ ] Commit: `"resources: Selawik + Cascadia Code font bundle"`

### 2.3 Glyph Caching

- [ ] Pre-rasterize ASCII 32–126 at common sizes (12, 14, 16, 20, 24px) at boot
- [ ] Cache struct: bitmap, width, height, x/y offset, advance per glyph
- [ ] Cache size: ~95 KB (95 chars × 5 sizes × 4 font slots × ~50 bytes)
- [ ] Fast lookup in `font_draw_char()` — bypass stb_truetype for cached glyphs
- [ ] Benchmark: cached vs. uncached rendering speed
- [ ] Commit: `"desktop: glyph cache for fast text rendering"`

### 2.4 Replace Bitmap Font

- [ ] Replace `font_draw_char()` calls in `desktop.c` with TrueType rendering
- [ ] Replace font calls in `wm.c` (window titles, decorations)
- [ ] Replace font calls in `controls.c` (buttons, labels, textboxes)
- [ ] Keep bitmap font as fallback for early boot (pre-initrd)
- [ ] Copy fonts from initrd to `C:\Impossible\Fonts\` on IXFS
- [ ] Commit: `"desktop: TrueType fonts replace bitmap"`

---

## 3. Runtime Image Decoding
> *Research: [04_image_formats.md](research/phase_02_ui_framework/04_image_formats.md), [05_runtime_image_decoding.md](research/phase_02_ui_framework/05_runtime_image_decoding.md)*

### 3.1 Kernel-Side stb_image

- [ ] Copy `stb_image.h` from `tools/` to `include/`
- [ ] Create `src/kernel/image.c` with kernel heap redirects (`STBI_MALLOC → kmalloc`)
- [ ] Define `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR` for kernel freestanding
- [ ] Define `image_t` struct (pixels, width, height, channels)
- [ ] Implement `image_load(path)` — load from VFS, decode, RGBA→BGRA conversion
- [ ] Implement `image_load_mem(data, size)` — decode from memory buffer
- [ ] Implement `image_free(img)` — free decoded data
- [ ] Test: decode a JPEG from initrd at runtime
- [ ] Commit: `"kernel: runtime image decoding (stb_image)"`

### 3.2 Image Scaling

- [ ] Implement `image_scale(src, target_w, target_h, mode)` — bilinear interpolation
- [ ] Support fit modes: `IMAGE_FIT_FILL`, `IMAGE_FIT_FIT`, `IMAGE_FIT_STRETCH`, `IMAGE_FIT_CENTER`, `IMAGE_FIT_TILE`
- [ ] Implement box-filter downscaling (better quality than bilinear for large reductions)
- [ ] Commit: `"kernel: image scaling with bilinear interpolation"`

### 3.3 JPG/PNG Wallpaper

- [ ] Modify `desktop.c` to load wallpaper via `image_load()` instead of raw initrd
- [ ] Support JPEG and PNG wallpapers directly (no build-time `jpg2raw` conversion)
- [ ] Scale wallpaper to fit screen using `image_scale()`
- [ ] Read wallpaper path and fit mode from Codex (`System\Theme\Wallpaper`, `WallpaperMode`)
- [ ] Cache scaled wallpaper (don't re-decode every frame)
- [ ] Commit: `"desktop: JPEG/PNG wallpaper loading"`

### 3.4 Image Saving

- [ ] Add `stb_image_write.h` to `include/` (public domain)
- [ ] Implement `image_save_bmp(img, path)` — save to VFS
- [ ] Implement `image_save_png(img, path)` — save to VFS
- [ ] Used by: future Paint app (Save As), screenshot feature
- [ ] Commit: `"kernel: image saving (BMP/PNG)"`

---

## 4. System Icon Store
> *Research: [06_icon_store.md](research/phase_02_ui_framework/06_icon_store.md)*

### 4.1 Icon Store Basics

- [ ] Define `system_icon_t` enum (~35 icons: file types, folders, drives, system)
- [ ] Define `icon_t` struct (pixels, width, height)
- [ ] Create `include/icon_store.h` and `src/kernel/icon_store.c`
- [ ] Implement `icon_store_init()` — load icons from initrd directory (`icons/`)
- [ ] Implement `icon_get(id)` — return icon by enum ID
- [ ] Implement `icon_get_by_name(name)` — lookup by string name
- [ ] Implement `icon_draw(surface, icon, x, y)` — blit with alpha blending
- [ ] Implement `icon_draw_scaled(surface, icon, x, y, target_size)`
- [ ] Commit: `"desktop: system icon store"`

### 4.2 Fluent UI Icons

- [ ] Download **Fluent UI System Icons** (MIT, Microsoft) for ~35 core icons
- [ ] Convert to 32×32 BGRA with `jpg2raw` in Makefile
- [ ] Icons needed: file_default, file_text, file_image, file_audio, file_video, file_archive, file_exe, file_code, folder_closed, folder_open, folder_documents, folder_pictures, drive_local, drive_removable, drive_network, computer, network, settings, start_menu, trash_empty, trash_full, printer, search, lock, user, power, info, warning, error, question, app_default
- [ ] Place source PNGs in `resources/icons/`
- [ ] Commit: `"resources: Fluent UI system icons"`

### 4.3 File Type Mapping

- [ ] Implement `icon_for_extension(ext)` — look up icon by file extension
- [ ] Define extension → icon mapping in Codex (`System\FileTypes`)
- [ ] Common mappings: txt/md/log → text, c/h/py/js → code, jpg/png/bmp → image, mp3/wav → audio, zip/tar → archive, exe → executable
- [ ] Commit: `"desktop: file type icon mapping"`

### 4.4 Icon Pack Format (Future)

- [ ] *(Stretch)* Define `.iconpack` binary format (header + index + BGRA data)
- [ ] *(Stretch)* Write `tools/iconpacker.c` build tool
- [ ] *(Stretch)* Multi-size support (16×16, 32×32, 48×48)
- [ ] *(Stretch)* Icon theme switching

---

## 5. Cursor Pack
> *Research: [07_cursor_pack.md](research/phase_02_ui_framework/07_cursor_pack.md)*

### 5.1 Cursor Manager

- [ ] Create `include/cursor.h` with `cursor_shape_t` enum (11 shapes)
- [ ] Define `cursor_sprite` struct (width, height, hotspot_x, hotspot_y, pixels)
- [ ] Create `src/kernel/drivers/cursor.c`
- [ ] Implement `cursor_init()` — load BGRA sprites from initrd, fall back to embedded arrow
- [ ] Implement `cursor_set_shape(shape)` — switch active cursor
- [ ] Implement `cursor_get_shape()` — get current shape
- [ ] Implement `cursor_draw(x, y)` — draw with alpha blending, save pixels underneath
- [ ] Implement `cursor_restore()` — restore saved pixels
- [ ] Implement `cursor_get_hotspot(hx, hy)` — for click position adjustment
- [ ] Commit: `"drivers: cursor manager with multiple shapes"`

### 5.2 Cursor Assets (11 Shapes)

- [ ] Create/design cursor PNGs (24×24, transparent background):
  - [ ] `arrow.png` — default pointer, hotspot (1,1)
  - [ ] `hand.png` — pointing hand (links, buttons), hotspot (6,1)
  - [ ] `text.png` — I-beam (text fields, terminal), hotspot (4,10)
  - [ ] `move.png` — 4-way arrows (window drag), hotspot (10,10)
  - [ ] `resize_ns.png` — ↕ vertical resize, hotspot (6,10)
  - [ ] `resize_ew.png` — ↔ horizontal resize, hotspot (10,6)
  - [ ] `resize_nwse.png` — ↘ diagonal resize, hotspot (8,8)
  - [ ] `resize_nesw.png` — ↗ diagonal resize, hotspot (8,8)
  - [ ] `wait.png` — hourglass/spinner, hotspot (8,12)
  - [ ] `crosshair.png` — + selection, hotspot (10,10)
  - [ ] `forbidden.png` — ⊘ circle-slash, hotspot (10,10)
- [ ] Place in `resources/cursors/`, convert in Makefile with `jpg2raw`
- [ ] Embed fallback arrow as byte array for pre-initrd boot
- [ ] Commit: `"resources: cursor sprite pack"`

### 5.3 Context-Based Cursor Switching

- [ ] Remove `cursor_data[]` and rendering from `mouse.c` (keep position/button tracking)
- [ ] Add `wm_get_cursor_context(mx, my)` in `wm.c`:
  - [ ] Desktop/wallpaper → `CURSOR_ARROW`
  - [ ] Start button hover → `CURSOR_HAND`
  - [ ] Menu item hover → `CURSOR_HAND`
  - [ ] Window title bar → `CURSOR_MOVE` (while dragging)
  - [ ] Window edge (N/S) → `CURSOR_RESIZE_NS`
  - [ ] Window edge (E/W) → `CURSOR_RESIZE_EW`
  - [ ] Window corner → `CURSOR_RESIZE_NWSE` or `CURSOR_RESIZE_NESW`
  - [ ] Text input field → `CURSOR_TEXT`
  - [ ] System busy → `CURSOR_WAIT`
- [ ] Update compositor loop: `cursor_restore()` → composite → `cursor_set_shape()` → `cursor_draw()`
- [ ] Adjust click position by hotspot offset in `wm_handle_mouse()`
- [ ] Commit: `"desktop: context-aware cursor switching"`

---

## 6. Window Animations
> *Research: [09_window_animations.md](research/phase_02_ui_framework/09_window_animations.md)*

### 6.1 Animation Engine

- [ ] Create `src/kernel/gfx/gfx_animate.c`
- [ ] Define `gfx_tween_t` struct (from, to, current, duration_ms, elapsed_ms, easing, active)
- [ ] Implement `gfx_tween_start(tw, from, to, duration_ms, easing)`
- [ ] Implement `gfx_tween_update(tw, delta_ms)` — advance by delta time
- [ ] Implement `gfx_tween_value(tw)` — get interpolated current value
- [ ] Implement easing functions:
  - [ ] `GFX_EASE_LINEAR`
  - [ ] `GFX_EASE_IN_QUAD` / `GFX_EASE_OUT_QUAD` / `GFX_EASE_IN_OUT_QUAD`
  - [ ] `GFX_EASE_IN_CUBIC` / `GFX_EASE_OUT_CUBIC` / `GFX_EASE_IN_OUT_CUBIC`
  - [ ] `GFX_EASE_BOUNCE`
- [ ] Commit: `"gfx: animation engine with easing"`

### 6.2 Window Transition Animations

- [ ] Create `src/kernel/wm_anim.c`
- [ ] Window open: scale 90%→100% + fade in (200ms, ease-out-cubic)
- [ ] Window close: scale 100%→90% + fade out (150ms)
- [ ] Minimize: shrink toward taskbar button position (250ms)
- [ ] Restore: expand from taskbar button (250ms)
- [ ] Maximize: expand to fill screen (200ms)
- [ ] *(Stretch)* Snap left/right: slide + resize to half (200ms)
- [ ] *(Stretch)* Focus switch: subtle scale pulse (100ms)
- [ ] Menu popup: scale Y 0→100% from top (150ms)
- [ ] Codex setting: `System\Theme\EnableAnimations`, `System\Theme\AnimationSpeed`
- [ ] "Reduce motion" option disables all animations
- [ ] Commit: `"desktop: window transition animations"`

---

## 7. Graphics API (OpenGL)
> *Research: [03_graphics_apis.md](research/phase_02_ui_framework/03_graphics_apis.md)*

### 7.1 TinyGL Port (Software OpenGL 1.x)

- [ ] Download TinyGL (~5000 lines, Zlib license)
- [ ] Port to Impossible OS framebuffer backend
- [ ] Implement framebuffer output callback (`fb_put_pixel` / `fb_blit`)
- [ ] Basic OpenGL 1.1: `glBegin`/`glEnd`, vertices, colors
- [ ] Textures: `glTexImage2D`, `glBindTexture`
- [ ] Z-buffer for depth testing
- [ ] Lighting: basic Phong
- [ ] Test: render a rotating cube
- [ ] Commit: `"gfx: TinyGL software OpenGL 1.1"`

---

## 8. Display & DPI Scaling
> *Research: [08_display_dpi_multimon.md](research/phase_02_ui_framework/08_display_dpi_multimon.md)*

### 8.1 DPI Scaling System

- [ ] Create `include/dpi.h` with `dpi_scale_t` enum (100%, 125%, 150%, 175%, 200%, 250%, 300%)
- [ ] Create `src/kernel/display/dpi.c`
- [ ] Implement `dpi_get_scale()` — return current scale factor
- [ ] Implement `dpi_auto_detect()` — heuristic from resolution (≥3840 → 200%, ≥2560 → 150%, else 100%)
- [ ] Implement `DPI(px)` macro: `(pixels * scale / 100)`
- [ ] Store scale in Codex: `System\Display\Scale`, `System\Display\AutoScale`
- [ ] Commit: `"display: DPI scaling system"`

### 8.2 DPI-Aware UI

- [ ] Replace all hardcoded pixel sizes in `desktop.c` with `DPI()`:
  - [ ] Taskbar height: `DPI(48)`
  - [ ] Button padding: `DPI(8)` × `DPI(4)`
  - [ ] Menu item height: `DPI(28)`
  - [ ] Margins and spacing
- [ ] Replace hardcoded sizes in `wm.c`:
  - [ ] Title bar height: `DPI(32)`
  - [ ] Window borders: `DPI(1)`
  - [ ] Corner radius: `DPI(8)`
  - [ ] Close/minimize/maximize button sizes
- [ ] Replace hardcoded sizes in `controls.c`:
  - [ ] Scroll bar width: `DPI(16)`
  - [ ] Minimum click target: `DPI(32)`
- [ ] Use scaled font sizes: `font_get(FONT_UI, DPI(14))`
- [ ] Use multi-size icons: `icon_get_scaled(id)` picks 32/48/64 based on DPI
- [ ] Commit: `"desktop: DPI-aware layout"`

### 8.3 Dynamic Resolution

- [ ] Support resolution preference in Multiboot2 header (configurable)
- [ ] Implement `display_enum_modes()` — query available VESA/VBE modes
- [ ] Implement `display_get_mode()` — return current resolution
- [ ] *(Stretch)* `display_set_mode(w, h)` — runtime resolution change (requires virtio-gpu)
- [ ] Test at 1080p, 1440p in QEMU
- [ ] Commit: `"display: resolution management"`

### 8.4 Multi-Monitor (Future)

- [ ] *(Stretch)* Define `struct monitor` (id, resolution, position, DPI, framebuffer)
- [ ] *(Stretch)* `struct display_manager` — up to 4 monitors
- [ ] *(Stretch)* Virtual desktop coordinate space
- [ ] *(Stretch)* Per-monitor DPI with `WM_DPI_CHANGED` notification
- [ ] *(Stretch)* QEMU multi-display with `virtio-vga,max_outputs=2`

---

## 9. Agent-Recommended Additions

> Items not in the research files but important for a polished UI framework.

### 9.1 Theme System

- [ ] Define `theme_t` struct with all UI colors (bg, fg, accent, border, shadow, titlebar, etc.)
- [ ] Load theme from Codex (`System\Theme\*`)
- [ ] Support Dark mode and Light mode presets
- [ ] All drawing functions use theme colors instead of hardcoded values
- [ ] Apply accent color to focused controls, active title bars, selection highlights
- [ ] Commit: `"desktop: theme system"`

### 9.2 Context Menus

- [ ] Implement right-click context menu system
- [ ] Generic `menu_create()`, `menu_add_item()`, `menu_show(x, y)` API
- [ ] Apply Acrylic blur effect to menu background
- [ ] Rounded corners with shadow
- [ ] Keyboard navigation (up/down arrows, Enter, Escape)
- [ ] Submenus with hover-to-open delay
- [ ] Desktop right-click: "Refresh", "Display Settings", "About"
- [ ] Commit: `"desktop: context menu system"`

### 9.3 Notification / Toast System

- [ ] Pop-up notifications from bottom-right corner
- [ ] Auto-dismiss after timeout (5 seconds default)
- [ ] Stack multiple notifications vertically
- [ ] Slide-in animation (from right edge)
- [ ] Used by: DHCP (IP assigned), disk mount, battery, errors
- [ ] Commit: `"desktop: notification toast system"`

### 9.4 Screenshot Capture

- [ ] `SYS_SCREENSHOT` syscall — capture framebuffer to image
- [ ] Print Screen key → save screenshot to `C:\Users\Default\Screenshots\`
- [ ] Save as PNG using `image_save_png()`
- [ ] Notification toast: "Screenshot saved"
- [ ] Commit: `"kernel: screenshot capture"`

### 9.5 Tooltip Support

- [ ] Hover delay (500ms) → show tooltip near cursor
- [ ] Tooltip struct: text, position, timer
- [ ] Rounded rect with shadow, semi-transparent background
- [ ] Auto-dismiss on mouse move
- [ ] Add to all buttons (close, minimize, maximize, start, taskbar items)
- [ ] Commit: `"desktop: tooltip support"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 2. TrueType Fonts | Biggest visual upgrade — anti-aliased text everywhere |
| 🔴 P0 | 1.1–1.2 Primitives + Blending | Foundation for all other effects |
| 🔴 P0 | 5. Cursors | Basic UX — context-appropriate cursor shapes |
| 🟠 P1 | 3. Image Decoding | Runtime wallpapers, icon loading, future apps |
| 🟠 P1 | 4. Icon Store | File type icons, system UI |
| 🟠 P1 | 1.3–1.4 Gradients + Materials | Windows 11 visual quality |
| 🟠 P1 | 9.1 Theme System | Centralized colors, dark/light mode |
| 🟡 P2 | 6. Animations | Polish — fluid transitions |
| 🟡 P2 | 9.2 Context Menus | Essential desktop interaction |
| 🟡 P2 | 8.1–8.2 DPI Scaling | HiDPI display support |
| 🟡 P2 | 9.5 Tooltips | UX polish |
| 🟢 P3 | 1.5 SIMD Optimization | Performance at higher resolutions |
| 🟢 P3 | 9.3 Notifications | System feedback |
| 🟢 P3 | 9.4 Screenshot | Utility feature |
| 🟢 P3 | 8.3 Dynamic Resolution | Resolution management |
| 🔵 P4 | 7. TinyGL (OpenGL) | 3D rendering (future apps) |
| 🔵 P4 | 8.4 Multi-Monitor | Advanced display (future) |
