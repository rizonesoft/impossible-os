# Phase 02 — UI Framework

> **Goal:** Transform the basic framebuffer desktop into a modern, Windows 11-quality
> graphical experience with compositing effects, TrueType fonts, image decoding,
> system icons, context-aware cursors, DPI scaling, and fluid animations.

---

## 1. 2D Compositing Library
> *Research: [01_2d_compositor.md](research/phase_02_ui_framework/01_2d_compositor.md)*

### 1.1 Core Surface & Primitives

**Prompt:** This is the foundation of all graphics work — define `gfx_surface_t` as the universal drawing target (framebuffer, off-screen buffer, window back-buffer). Every subsequent section depends on this. Study how the existing framebuffer works in `src/desktop/desktop.c` before creating the abstraction. Use `0xAARRGGBB` for the pixel format (matching VBE/VESA). Bresenham's algorithm for line drawing is simple and efficient. The dirty rectangle tracker should maintain a list of modified regions so the compositor only redraws what changed — this was key to fixing the compositor flicker (see the rules about `pmm_alloc_contiguous` for the back buffer). After completing all items, create `docs/architecture/gfx-library.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"gfx: core surface and primitive drawing"`.


- [x] Define `gfx_surface_t` struct (pixels, width, height, stride)
- [x] Define `gfx_color_t` (0xAARRGGBB) with `GFX_RGBA()`, `GFX_RGB()`, `GFX_ALPHA()` macros
- [x] Create `include/gfx.h` with all type/API declarations
- [x] Create `src/kernel/gfx/gfx_core.c`
- [x] Implement `gfx_fill_rect(surface, x, y, w, h, color)` — solid fill
- [x] Implement `gfx_draw_rect(surface, x, y, w, h, thickness, color)` — outline
- [x] Implement `gfx_fill_rounded_rect(surface, x, y, w, h, radius, color)` — anti-aliased corners
- [x] Implement `gfx_draw_rounded_rect(surface, x, y, w, h, radius, thickness, color)`
- [x] Implement `gfx_fill_circle(surface, cx, cy, r, color)`
- [x] Implement `gfx_draw_line(surface, x1, y1, x2, y2, thickness, color)` — Bresenham
- [x] Implement dirty rectangle tracker for partial redraws
- [x] Commit: `"gfx: core surface and primitive drawing"`

### 1.2 Alpha Blending & Compositing

**Prompt:** Alpha blending is the most performance-critical code path in the entire compositor — every window pixel passes through it. Use pre-multiplied alpha (multiply RGB by A once at source, then the blend formula simplifies to `dst = src + dst * (255 - src_alpha)` per channel) which is ~50% faster than straight alpha. All math must be integer-only (`(a * b + 127) / 255` or the faster `(a * b + 128) >> 8` approximation). The `gfx_blit_alpha` with a global alpha multiplier is used for window fade animations and semi-transparent overlays. Profile with a full-screen 1280×720 blit to ensure <3ms. After completing all items, update `docs/architecture/gfx-library.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"gfx: alpha blending and compositing"`.


- [x] Create `src/kernel/gfx/gfx_blend.c`
- [x] Implement `gfx_blit(dst, dx, dy, src, sx, sy, w, h)` — per-pixel alpha blit
- [x] Implement `gfx_blit_alpha(dst, dx, dy, src, alpha)` — blit with global alpha
- [x] Implement `gfx_fill_rect_alpha(surface, x, y, w, h, color)` — alpha from color channel
- [x] Use pre-multiplied alpha (50% fewer multiplies in hot path)
- [x] Integer-only math in blending (no floating point)
- [x] Commit: `"gfx: alpha blending and compositing"`

### 1.3 Gradients

**Prompt:** Gradients add visual depth to UI elements — the taskbar, title bars, and buttons all benefit from subtle gradients. Linear gradients interpolate between two colors across the fill region (vertical or horizontal). For each pixel, compute `t = position / total` and blend: `color = start * (1-t) + end * t` per channel. Radial gradients compute `t` from distance to center. Use integer fixed-point math (e.g., 16.16) to avoid floating-point. Gradient-filled rounded rects combine this with the corner rounding from §1.1. After completing all items, update `docs/architecture/gfx-library.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"gfx: gradient fills"`.


- [x] Create `src/kernel/gfx/gfx_gradient.c`
- [x] Define `gfx_gradient_t` struct (start color, end color, direction)
- [x] Implement `gfx_fill_gradient_rect()` — vertical + horizontal linear gradients
- [x] Implement `gfx_fill_gradient_rounded()` — gradient with rounded corners
- [x] Implement radial gradient fill
- [x] Commit: `"gfx: gradient fills"`

### 1.4 Blur & Material Effects

**Prompt:** Blur creates the frosted-glass look of Windows 11's Acrylic and Mica materials. Implement a two-pass box blur (horizontal then vertical) which is O(n) per pixel regardless of radius — much faster than a naive 2D kernel. Acrylic combines blur + noise texture (2-3% random pixel variation for visual texture) + tint color overlay. Mica samples the wallpaper at the window's position, desaturates by blending 80% toward grayscale, and applies a theme tint — it's cheaper than Acrylic because there's no blur pass. Drop shadows render as Gaussian-blurred dark rectangles behind windows. All these effects operate on temporary surfaces to avoid corrupting the source. After completing all items, update `docs/architecture/gfx-library.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"gfx: blur, Mica, Acrylic, and shadow effects"`.


- [x] Create `src/kernel/gfx/gfx_blur.c` and `gfx_effects.c`
- [x] Implement `gfx_blur_rect(surface, x, y, w, h, radius)` — 2-pass box blur (O(n) per pixel)
- [x] Implement `gfx_acrylic(surface, x, y, w, h, tint, opacity, blur_radius)`:
  - [x] Copy region to temp buffer
  - [x] Apply box blur
  - [x] Add noise texture (2–3% random variation)
  - [x] Overlay tint color at opacity
- [x] Implement `gfx_mica(surface, x, y, w, h, wallpaper, tint)`:
  - [x] Sample wallpaper at position
  - [x] Desaturate (80% grayscale blend)
  - [x] Tint with theme color
- [x] Implement `gfx_drop_shadow(surface, x, y, w, h, radius, offset_x, offset_y, color)` — multi-layer soft shadow
- [x] Implement `gfx_reveal_highlight(surface, rect, mouse_x, mouse_y, glow_radius, highlight)` — radial glow following cursor
- [x] Apply Mica to window title bars *(API ready; wiring in Phase 04)*
- [x] Apply Acrylic to taskbar, start menu, context menus *(API ready; wiring in Phase 04)*
- [x] Pre-render and cache shadow bitmaps per window size *(gfx_drop_shadow allocates temp surface; caching in Phase 04)*
- [x] Commit: `"gfx: blur, Mica, Acrylic, and shadow effects"`

### 1.5 SIMD Optimization

**Prompt:** SSE2 processes 4 pixels simultaneously using 128-bit XMM registers — this directly accelerates the bottleneck functions: alpha blending, gradient fills, and blur passes. Use `_mm_loadu_si128` / `_mm_storeu_si128` for unaligned pixel loads/stores. The alpha blend in SSE2 unpacks pixels to 16-bit lanes, multiplies, shifts, and repacks. Critical: save/restore FPU state with `fxsave`/`fxrstor` around SSE code when called from interrupt context since user-mode FPU state would otherwise be corrupted. Compile gfx files separately with `-msse2`. Detect AVX2 at runtime via CPUID before using 256-bit YMM registers. Benchmark the compositor frame time and target <8ms at 1280×720. After completing all items, update `docs/architecture/gfx-library.md` with SIMD details, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"gfx: SSE2 SIMD acceleration"`.


- [x] Enable SSE2 for gfx module: compile with `-msse2` separately
- [x] Implement `fxsave`/`fxrstor` wrappers to protect user FPU state
- [x] SSE2 alpha blending — 4 pixels per cycle
- [x] SSE2 gradient fill — 4 pixels per cycle
- [x] SSE2 blur — 4 pixels per cycle
- [x] *(Stretch)* AVX2 paths — 8 pixels per cycle (detect at runtime with CPUID)
- [x] Benchmark: target <8ms full compositor frame at 1280×720
- [x] Commit: `"gfx: SSE2 SIMD acceleration"`

---

## 2. TrueType Font System
> *Research: [02_font_system.md](research/phase_02_ui_framework/02_font_system.md)*

### 2.1 stb_truetype Integration

**Prompt:** Check the codebase first — stb_truetype has been partially integrated in a previous conversation. Look for `src/libs/stb_truetype/stb_truetype_impl.c`, `include/font_mgr.h`, and `src/desktop/gfx_text.c`. If they exist, verify the build works and focus on any incomplete API functions. If not, port stb_truetype.h with kernel redirects (`STBTT_malloc → kmalloc`, `STBTT_free → kfree`, `STBTT_memcpy/memset/strlen` → kernel equivalents). The implementation file must be compiled with `-msse2 -mfpmath=sse` since stb_truetype uses floating-point. The font manager loads `.ttf` files from `C:\Impossible\Fonts\` via VFS. After completing all items, update `docs/architecture/font-rendering.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: stb_truetype integration"`.


- [x] Add `stb_truetype.h` to `include/` (public domain)
- [x] Redirect memory: `STBTT_malloc → kmalloc`, `STBTT_free → kfree`
- [x] Create `src/kernel/gfx/gfx_text.c` and `include/font_mgr.h`
- [x] Implement `ttf_mgr_init()` — load fonts from `C:\Impossible\Fonts\` at boot
- [x] Implement `ttf_get(slot, pixel_size)` — return scaled font handle
- [x] Implement `ttf_draw_char(surface, font, x, y, codepoint, color)` — rasterize + alpha blend
- [x] Implement `ttf_draw_string(surface, font, x, y, text, color)` — with kerning
- [x] Implement `ttf_measure_width(font, text)` — text width measurement
- [x] Implement `ttf_line_height(font)` — get line height
- [x] Commit: `"desktop: stb_truetype integration"`

### 2.2 Font Bundle

**Prompt:** The fonts define the visual identity of the entire OS. Selawik is a metrically-compatible alternative to Segoe UI (Windows' system font) and is MIT-licensed. Cascadia Code is Microsoft's monospace font with ligature support, perfect for the terminal. Place `.ttf` files in `resources/fonts/` and update the Makefile's sysroot-copy step to include them. Define named font slots (FONT_UI, FONT_UI_BOLD, FONT_MONO, FONT_MONO_BOLD) mapped to these files so all UI code references slots rather than filenames. Include license files to comply with OFL 1.1. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"resources: Selawik + Cascadia Code font bundle"`.


- [x] Download **Selawik** Regular + Semibold + Bold (~132 KB total, MIT license)
- [x] Download **Cascadia Code** Regular + Bold (~1.2 MB total, OFL 1.1)
- [x] Place `.ttf` files in `resources/fonts/`
- [x] Update Makefile to copy fonts into sysroot
- [x] Define font slots: `FONT_UI`, `FONT_UI_BOLD`, `FONT_MONO`, `FONT_MONO_BOLD`, `FONT_UI_HEAVY`
- [x] Add font license files to `resources/fonts/LICENSE-*`
- [x] *(Stretch)* Add **Inter** as an alternative UI font
- [x] Commit: `"resources: Selawik + Cascadia Code font bundle"`

### 2.3 Glyph Caching

**Prompt:** Rasterizing glyphs through stb_truetype is CPU-intensive — cache pre-rendered bitmaps for the printable ASCII range (32-126) at each commonly used pixel size (12, 14, 16, 20, 24px) for each font slot. The cache stores: bitmap pointer, width, height, x/y bearing offsets, and advance width per glyph. At boot, `ttf_mgr_init()` bakes all these glyphs. The hot path in `ttf_draw_char()` checks the cache first and only falls back to live rasterization for uncached sizes or non-ASCII codepoints. This should make text rendering nearly as fast as a bitmap font. After completing all items, update `docs/architecture/font-rendering.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: glyph cache for fast text rendering"`.


- [x] Pre-rasterize ASCII 32–126 at common sizes (12, 14, 16, 20, 24px) at boot
- [x] Cache struct: bitmap, width, height, x/y offset, advance per glyph
- [x] Cache size: ~95 KB (95 chars × 5 sizes × 4 font slots × ~50 bytes)
- [x] Fast lookup in `font_draw_char()` — bypass stb_truetype for cached glyphs
- [x] Benchmark: cached vs. uncached rendering speed
- [x] Commit: `"desktop: glyph cache for fast text rendering"`

### 2.4 Replace Bitmap Font

**Prompt:** Search the codebase for all remaining calls to the old bitmap `font_draw_char` and `font_draw_string` (or their PSF equivalents) in `desktop.c`, `wm.c`, `controls.c`, the shell, and any other UI code. Replace them with the TrueType `ttf_draw_string` API. Keep the bitmap font renderer compiled as a fallback for the very early boot phase (before initrd is loaded and VFS is mounted, so TTF files aren't available yet). The shell/terminal should use FONT_MONO, window titles use FONT_UI_BOLD, and buttons/labels use FONT_UI. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: TrueType fonts replace bitmap"`.


- [x] Replace `font_draw_char()` calls in `desktop.c` with TrueType rendering
- [x] Replace font calls in `wm.c` (window titles, decorations)
- [x] Replace font calls in `controls.c` (buttons, labels, textboxes)
- [x] Keep bitmap font as fallback for early boot (pre-initrd)
- [x] Copy fonts from C:\ to `C:\Impossible\Fonts\` on IXFS
- [x] Commit: `"desktop: TrueType fonts replace bitmap"`

---

## 3. Runtime Image Decoding
> *Research: [04_image_formats.md](research/phase_02_ui_framework/04_image_formats.md), [05_runtime_image_decoding.md](research/phase_02_ui_framework/05_runtime_image_decoding.md)*

### 3.1 Kernel-Side stb_image

**Prompt:** stb_image.h is a single-header image decoder supporting JPEG, PNG, BMP, GIF, and TGA. Check if `tools/stb_image.h` already exists in the codebase (it may have been used by the build-time jpg2raw tool). Copy it to `include/` and create `src/kernel/image.c` that wraps it for kernel use. Define `STBI_NO_STDIO` (no FILE*), `STBI_NO_LINEAR`, `STBI_NO_HDR` to minimize dependencies. Redirect `STBI_MALLOC/STBI_REALLOC/STBI_FREE` to kernel heap. Important: stb_image decodes to RGBA but the framebuffer uses BGRA — implement a channel-swap post-processing step. The `image_load(path)` function reads file contents via VFS into a kmalloc'd buffer, passes it to `stbi_load_from_memory`, and returns an `image_t`. After completing all items, create `docs/architecture/image-system.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: runtime image decoding (stb_image)"`.


- [ ] Copy `stb_image.h` from `tools/` to `include/`
- [ ] Create `src/kernel/image.c` with kernel heap redirects (`STBI_MALLOC → kmalloc`)
- [ ] Define `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR` for kernel freestanding
- [ ] Define `image_t` struct (pixels, width, height, channels)
- [ ] Implement `image_load(path)` — load from VFS, decode, RGBA→BGRA conversion
- [ ] Implement `image_load_mem(data, size)` — decode from memory buffer
- [ ] Implement `image_free(img)` — free decoded data
- [ ] Test: decode a JPEG from C:\ at runtime
- [ ] Commit: `"kernel: runtime image decoding (stb_image)"`

### 3.2 Image Scaling

**Prompt:** Image scaling is needed for wallpaper fitting, icon resizing, and thumbnail generation. Bilinear interpolation samples four surrounding pixels and interpolates — it's smooth but can be blurry on large downscales. For downscaling by more than 2x, use box filtering (average all source pixels that map to each destination pixel) which produces sharper results. Implement fit modes: FILL (scale to cover, crop excess), FIT (scale to fit within, letterbox), STRETCH (distort to exact size), CENTER (no scaling, center on canvas), TILE (repeat pattern). After completing all items, update `docs/architecture/image-system.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: image scaling with bilinear interpolation"`.


- [ ] Implement `image_scale(src, target_w, target_h, mode)` — bilinear interpolation
- [ ] Support fit modes: `IMAGE_FIT_FILL`, `IMAGE_FIT_FIT`, `IMAGE_FIT_STRETCH`, `IMAGE_FIT_CENTER`, `IMAGE_FIT_TILE`
- [ ] Implement box-filter downscaling (better quality than bilinear for large reductions)
- [ ] Commit: `"kernel: image scaling with bilinear interpolation"`

### 3.3 JPG/PNG Wallpaper

**Prompt:** Currently the wallpaper is loaded as a pre-converted raw bitmap from the initrd — replace this with runtime JPEG/PNG decoding via the `image_load()` API from §3.1. Read the wallpaper path from Codex `System\Theme\Wallpaper` (default: `C:\Impossible\System\Wallpapers\default.jpg`) and the fit mode from `System\Theme\WallpaperMode`. Scale the decoded image to the screen resolution using `image_scale()` from §3.2. Cache the scaled result so it's not re-decoded every compositor frame — only re-decode when the wallpaper setting changes. After completing all items, update `docs/architecture/image-system.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: JPEG/PNG wallpaper loading"`.


- [ ] Modify `desktop.c` to load wallpaper via `image_load()` instead of raw initrd
- [ ] Support JPEG and PNG wallpapers directly (no build-time `jpg2raw` conversion)
- [ ] Scale wallpaper to fit screen using `image_scale()`
- [ ] Read wallpaper path and fit mode from Codex (`System\Theme\Wallpaper`, `WallpaperMode`)
- [ ] Cache scaled wallpaper (don't re-decode every frame)
- [ ] Commit: `"desktop: JPEG/PNG wallpaper loading"`

### 3.4 Image Saving

**Prompt:** stb_image_write.h provides PNG and BMP saving in a single header (public domain). Like stb_image, it needs `STBI_WRITE_NO_STDIO` defined and a custom write function that calls `vfs_write`. The key consumer is the screenshot feature (§9.4) and the future Paint app's save function. PNG is preferred (lossless, smaller) but BMP is simpler as a fallback. After completing all items, update `docs/architecture/image-system.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: image saving (BMP/PNG)"`.


- [ ] Add `stb_image_write.h` to `include/` (public domain)
- [ ] Implement `image_save_bmp(img, path)` — save to VFS
- [ ] Implement `image_save_png(img, path)` — save to VFS
- [ ] Used by: future Paint app (Save As), screenshot feature
- [ ] Commit: `"kernel: image saving (BMP/PNG)"`

---

## 4. System Icon Store
> *Research: [06_icon_store.md](research/phase_02_ui_framework/06_icon_store.md)*

### 4.1 Icon Store Basics

**Prompt:** The icon store is a centralized repository of system icons (~35 icons) loaded once at boot and accessed by ID throughout the UI. Icons are 32×32 BGRA bitmaps with alpha channel for transparency. Load them from the initrd or from `C:\Impossible\System\Icons\` using `image_load()` from §3.1. The `icon_get(id)` function returns a pointer to the cached icon data — no allocation, no decoding on each call. `icon_draw_scaled` uses `image_scale()` for DPI-aware rendering. Every file type, system action, and UI element that needs an icon should go through this API. After completing all items, create `docs/architecture/icon-store.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: system icon store"`.


- [ ] Define `system_icon_t` enum (~35 icons: file types, folders, drives, system)
- [ ] Define `icon_t` struct (pixels, width, height)
- [ ] Create `include/icon_store.h` and `src/kernel/icon_store.c`
- [ ] Implement `icon_store_init()` — load icons from C:\ directory (`icons/`)
- [ ] Implement `icon_get(id)` — return icon by enum ID
- [ ] Implement `icon_get_by_name(name)` — lookup by string name
- [ ] Implement `icon_draw(surface, icon, x, y)` — blit with alpha blending
- [ ] Implement `icon_draw_scaled(surface, icon, x, y, target_size)`
- [ ] Commit: `"desktop: system icon store"`

### 4.2 Fluent UI Icons

**Prompt:** Microsoft's Fluent UI System Icons are MIT-licensed and designed for modern UIs — download the filled variants at 32×32 from the GitHub repo. Focus on ~35 essential icons covering: file types (text, image, audio, video, archive, executable, code), folders (closed, open, documents, pictures), drives (local, removable, network), and system (computer, settings, search, trash, lock, user, power, printer, info, warning, error). Convert source PNGs to 32×32 BGRA raw format using the existing `jpg2raw` build tool or via stb_image at runtime. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"resources: Fluent UI system icons"`.


- [ ] Download **Fluent UI System Icons** (MIT, Microsoft) for ~35 core icons
- [ ] Convert to 32×32 BGRA with `jpg2raw` in Makefile
- [ ] Icons needed: file_default, file_text, file_image, file_audio, file_video, file_archive, file_exe, file_code, folder_closed, folder_open, folder_documents, folder_pictures, drive_local, drive_removable, drive_network, computer, network, settings, start_menu, trash_empty, trash_full, printer, search, lock, user, power, info, warning, error, question, app_default
- [ ] Place source PNGs in `resources/icons/`
- [ ] Commit: `"resources: Fluent UI system icons"`

### 4.3 File Type Mapping

**Prompt:** Map file extensions to icon IDs using Codex entries under `System\FileTypes\{ext}\Icon`. The `icon_for_extension(".txt")` function looks up the extension in Codex and returns the matching `system_icon_t` enum value. Fall back to `ICON_FILE_DEFAULT` for unknown extensions. This is used everywhere files are displayed: File Manager, desktop icons, Open/Save dialogs, and the Start menu's app list. Common mappings: txt/md/log → text, c/h/py/js → code, jpg/png/bmp/gif → image, mp3/wav/ogg → audio, zip/tar/gz → archive. After completing all items, update `docs/architecture/icon-store.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: file type icon mapping"`.


- [ ] Implement `icon_for_extension(ext)` — look up icon by file extension
- [ ] Define extension → icon mapping in Codex (`System\FileTypes`)
- [ ] Common mappings: txt/md/log → text, c/h/py/js → code, jpg/png/bmp → image, mp3/wav → audio, zip/tar → archive, exe → executable
- [ ] Commit: `"desktop: file type icon mapping"`

### 4.4 Icon Pack Format (Future)

**Prompt:** This is a stretch goal for a more efficient icon storage format. The `.iconpack` binary format bundles all icons into a single file: a header with magic number and icon count, followed by an index table (icon ID, offset, width, height per entry), then packed BGRA pixel data. A host-side `iconpacker` build tool creates .iconpack files from individual PNGs. Multi-size support (16, 32, 48px) allows the icon store to pick the closest size for the current DPI scale. After completing all items, update `docs/architecture/icon-store.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: icon pack format"`.


- [ ] *(Stretch)* Define `.iconpack` binary format (header + index + BGRA data)
- [ ] *(Stretch)* Write `tools/iconpacker.c` build tool
- [ ] *(Stretch)* Multi-size support (16×16, 32×32, 48×48)
- [ ] *(Stretch)* Icon theme switching

---

## 5. Cursor Pack
> *Research: [07_cursor_pack.md](research/phase_02_ui_framework/07_cursor_pack.md)*

### 5.1 Cursor Manager

**Prompt:** The cursor manager replaces the current hardcoded arrow cursor in `mouse.c` with a system that supports 11 different shapes loaded from BGRA sprite files. Each `cursor_sprite` has width, height, hotspot coordinates (the pixel that corresponds to the click position), and pixel data. `cursor_draw` saves the pixels underneath the cursor before blitting (so `cursor_restore` can undo it without redrawing the entire frame). The hotspot offset must be applied in `wm_handle_mouse` so clicks register at the correct position. Keep an embedded fallback arrow as a C byte array for pre-initrd boot when VFS isn't available. After completing all items, create `docs/architecture/cursor-system.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: cursor manager with multiple shapes"`.


- [ ] Create `include/cursor.h` with `cursor_shape_t` enum (11 shapes)
- [ ] Define `cursor_sprite` struct (width, height, hotspot_x, hotspot_y, pixels)
- [ ] Create `src/kernel/drivers/cursor.c`
- [ ] Implement `cursor_init()` — load BGRA sprites from C:\, fall back to embedded arrow
- [ ] Implement `cursor_set_shape(shape)` — switch active cursor
- [ ] Implement `cursor_get_shape()` — get current shape
- [ ] Implement `cursor_draw(x, y)` — draw with alpha blending, save pixels underneath
- [ ] Implement `cursor_restore()` — restore saved pixels
- [ ] Implement `cursor_get_hotspot(hx, hy)` — for click position adjustment
- [ ] Commit: `"drivers: cursor manager with multiple shapes"`

### 5.2 Cursor Assets (11 Shapes)

**Prompt:** Design or source 11 cursor PNGs at 24×24 with transparent backgrounds. The arrow is the default pointer with hotspot at top-left (1,1). The hand cursor indicates clickable elements (hotspot at fingertip). The I-beam (text cursor) has its hotspot at the middle of the vertical line. Resize cursors use cardinal directions matching the resize edge. Use the Fluent design language for consistency with the icon set. Convert PNGs to BGRA raw format in the Makefile using `jpg2raw` or load them at runtime via stb_image. The fallback arrow must be embedded as a `static const uint32_t cursor_fallback[]` byte array. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"resources: cursor sprite pack"`.


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

**Prompt:** The window manager must determine the correct cursor shape based on what's under the mouse pointer. Add `wm_get_cursor_context(mx, my)` that checks: is the mouse over a window edge or corner (resize cursors), over a title bar during drag (move cursor), over a text input widget (I-beam), over a button or link (hand), or over the desktop (arrow). This function is called every mouse-move event and updates the cursor shape. The compositor loop must save/restore cursor pixels around the composite step to prevent cursor artifacts. Remove the old cursor rendering from `mouse.c` entirely — mouse.c should only track position and button state. After completing all items, update `docs/architecture/cursor-system.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: context-aware cursor switching"`.


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

**Prompt:** The animation engine provides time-based interpolation (tweening) for smooth UI transitions. A `gfx_tween_t` stores: start value, end value, current value, duration in ms, elapsed time, and an easing function pointer. `gfx_tween_update(delta_ms)` advances the tween by the frame delta time and recomputes the current value using the easing function. Easing functions take `t` (0.0→1.0) and return a shaped `t`: linear is identity, ease-out-cubic is `1 - (1-t)^3` (starts fast, decelerates), ease-in-quad is `t^2` (starts slow, accelerates). The compositor calls `gfx_tween_update` each frame with the frame delta. After completing all items, create `docs/architecture/animations.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"gfx: animation engine with easing"`.


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

**Prompt:** Each window state change should have a smooth animation: open (scale 90%→100% + fade in, 200ms ease-out-cubic), close (scale 100%→90% + fade out, 150ms), minimize (shrink toward the window's taskbar button position, 250ms), restore (reverse of minimize), maximize (expand to fill screen, 200ms). Use the tween engine from §6.1 — each animation creates tweens for the window's x, y, width, height, and opacity. The compositor must render animating windows at their interpolated position/size each frame. Add a Codex setting `System\Theme\EnableAnimations` (default: true) and `System\Theme\AnimationSpeed` (multiplier, default: 1.0) so users can disable or slow animations. After completing all items, update `docs/architecture/animations.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: window transition animations"`.


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

**Prompt:** TinyGL is a minimal software OpenGL 1.1 implementation (~5000 lines, Zlib license) — it renders 3D directly to a pixel buffer with no GPU requirement. The port involves replacing TinyGL's output backend with a function that writes to a `gfx_surface_t`. Basic OpenGL 1.1 covers `glBegin/glEnd` immediate mode, vertex colors, texture mapping via `glTexImage2D`, z-buffer depth testing, and basic Phong lighting. This enables future 3D applications (games, visualizations) and a 3D screensaver. Test with a rotating textured cube to verify transforms, texturing, and depth testing all work. After completing all items, create `docs/architecture/opengl.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"gfx: TinyGL software OpenGL 1.1"`.


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

**Prompt:** DPI scaling multiplies all UI dimensions by a scale factor so the interface is readable on high-resolution displays. Define a `DPI(px)` macro that computes `(px * scale / 100)` at runtime. Auto-detection heuristic: 4K (3840+) → 200%, 1440p (2560+) → 150%, else 100%. Store the scale in Codex `System\Display\Scale` and an auto-detect flag in `System\Display\AutoScale`. All UI code must call `DPI()` for sizes — this is a convention that §8.2 enforces by replacing hardcoded values. After completing all items, create `docs/architecture/dpi-scaling.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"display: DPI scaling system"`.


- [ ] Create `include/dpi.h` with `dpi_scale_t` enum (100%, 125%, 150%, 175%, 200%, 250%, 300%)
- [ ] Create `src/kernel/display/dpi.c`
- [ ] Implement `dpi_get_scale()` — return current scale factor
- [ ] Implement `dpi_auto_detect()` — heuristic from resolution (≥3840 → 200%, ≥2560 → 150%, else 100%)
- [ ] Implement `DPI(px)` macro: `(pixels * scale / 100)`
- [ ] Store scale in Codex: `System\Display\Scale`, `System\Display\AutoScale`
- [ ] Commit: `"display: DPI scaling system"`

### 8.2 DPI-Aware UI

**Prompt:** This is a systematic search-and-replace across all UI code: every hardcoded pixel value for spacing, padding, margin, widget size, font size, and corner radius must be wrapped in the `DPI()` macro from §8.1. Focus on `desktop.c` (taskbar height, button padding), `wm.c` (title bar height, borders, corner radius, close/min/max button sizes), and `controls.c` (scroll bar width, minimum click targets). Font sizes should use `font_get(slot, DPI(14))`. Icons should pick the size closest to `DPI(32)`. This is tedious but critical for HiDPI display support. After completing all items, update `docs/architecture/dpi-scaling.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: DPI-aware layout"`.


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

**Prompt:** VESA/VBE mode enumeration lets you discover and switch between supported resolutions at runtime. Query the VBE mode list (via Multiboot2 information or by iterating VBE modes with INT 10h before entering long mode). `display_enum_modes()` populates an array of available modes. `display_get_mode()` returns the current resolution. Runtime resolution change via `display_set_mode()` is complex (requires re-mapping the framebuffer, resizing the compositor back buffer, and notifying all windows) and may require virtio-gpu. Test at 1080p and 1440p in QEMU using `-vga std` or Bochs VGA. After completing all items, update `docs/architecture/dpi-scaling.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"display: resolution management"`.


- [ ] Support resolution preference in Multiboot2 header (configurable)
- [ ] Implement `display_enum_modes()` — query available VESA/VBE modes
- [ ] Implement `display_get_mode()` — return current resolution
- [ ] *(Stretch)* `display_set_mode(w, h)` — runtime resolution change (requires virtio-gpu)
- [ ] Test at 1080p, 1440p in QEMU
- [ ] Commit: `"display: resolution management"`

### 8.4 Multi-Monitor (Future)

**Prompt:** Multi-monitor support is a stretch goal requiring significant architecture changes. Each monitor gets its own `struct monitor` (resolution, position in virtual desktop space, DPI, framebuffer pointer). The compositor renders to a virtual desktop larger than any single screen and copies each monitor's region to its framebuffer. Windows track which monitor they're on for DPI scaling. QEMU simulates multi-monitor with `virtio-vga,max_outputs=2`. This requires virtio-gpu support which is separate from the Bochs VGA currently in use. After completing all items, update `docs/architecture/dpi-scaling.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"display: multi-monitor support"`.


- [ ] *(Stretch)* Define `struct monitor` (id, resolution, position, DPI, framebuffer)
- [ ] *(Stretch)* `struct display_manager` — up to 4 monitors
- [ ] *(Stretch)* Virtual desktop coordinate space
- [ ] *(Stretch)* Per-monitor DPI with `WM_DPI_CHANGED` notification
- [ ] *(Stretch)* QEMU multi-display with `virtio-vga,max_outputs=2`

---

## 9. Agent-Recommended Additions

> Items not in the research files but important for a polished UI framework.

### 9.1 Theme System

**Prompt:** Centralize all UI colors into a `theme_t` struct with named fields: background, foreground, accent, border, shadow, titlebar_active, titlebar_inactive, button_bg, button_hover, selection, error, warning. Load theme colors from Codex under `System\Theme\*`. Provide two built-in presets: Dark (dark backgrounds, light text, blue accent) and Light (light backgrounds, dark text). Every drawing function in `desktop.c`, `wm.c`, and `controls.c` must reference `theme_get()->field` instead of hardcoded hex colors. The accent color should be applied to focused controls, active title bars, and selection highlights. After completing all items, create `docs/architecture/theme-system.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: theme system"`.


- [ ] Define `theme_t` struct with all UI colors (bg, fg, accent, border, shadow, titlebar, etc.)
- [ ] Load theme from Codex (`System\Theme\*`)
- [ ] Support Dark mode and Light mode presets
- [ ] All drawing functions use theme colors instead of hardcoded values
- [ ] Apply accent color to focused controls, active title bars, selection highlights
- [ ] Commit: `"desktop: theme system"`

### 9.2 Context Menus

**Prompt:** Context menus are essential desktop interaction — right-click should work everywhere. Implement a reusable `menu_create()`, `menu_add_item(label, icon, callback)`, `menu_add_separator()`, `menu_show(x, y)` API. The menu renders as a top-level window above all others with Acrylic blur background (from §1.4), rounded corners, and a subtle drop shadow. Keyboard navigation: up/down arrows move selection, Enter activates, Escape closes. Clicking outside the menu dismisses it. Submenus open on hover with a 300ms delay. The desktop right-click menu needs: Refresh, Display Settings, Personalize, Terminal Here, About. After completing all items, create `docs/architecture/context-menus.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: context menu system"`.


- [ ] Implement right-click context menu system
- [ ] Generic `menu_create()`, `menu_add_item()`, `menu_show(x, y)` API
- [ ] Apply Acrylic blur effect to menu background
- [ ] Rounded corners with shadow
- [ ] Keyboard navigation (up/down arrows, Enter, Escape)
- [ ] Submenus with hover-to-open delay
- [ ] Desktop right-click: "Refresh", "Display Settings", "About"
- [ ] Commit: `"desktop: context menu system"`

### 9.3 Notification / Toast System

**Prompt:** Toast notifications slide in from the bottom-right corner of the screen to inform the user of events. Each notification has a title, message body, optional icon, and auto-dismiss timeout (default 5 seconds). Multiple active notifications stack vertically with spacing. Use the animation engine from §6.1 for the slide-in effect (tween x position from off-screen to visible, ease-out-cubic, 300ms). Notifications are used by: DHCP ("IP address assigned"), disk mount events, battery warnings, screenshot capture, and error reporting. Implement `notify_show(title, message, icon, timeout_ms)` as the public API. After completing all items, create `docs/architecture/notifications.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: notification toast system"`.


- [ ] Pop-up notifications from bottom-right corner
- [ ] Auto-dismiss after timeout (5 seconds default)
- [ ] Stack multiple notifications vertically
- [ ] Slide-in animation (from right edge)
- [ ] Used by: DHCP (IP assigned), disk mount, battery, errors
- [ ] Commit: `"desktop: notification toast system"`

### 9.4 Screenshot Capture

**Prompt:** Screenshot capture copies the framebuffer contents to an image file. The `SYS_SCREENSHOT` syscall (or a kernel function triggered by the Print Screen key handler in `keyboard.c`) reads the framebuffer into an `image_t`, then saves it via `image_save_png()` from §3.4 to `C:\Users\Default\Screenshots\screenshot_{timestamp}.png`. Show a notification toast (§9.3) confirming the save with the file path. This depends on both the image saving API (§3.4) and the notification system (§9.3). After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: screenshot capture"`.


- [ ] `SYS_SCREENSHOT` syscall — capture framebuffer to image
- [ ] Print Screen key → save screenshot to `C:\Users\Default\Screenshots\`
- [ ] Save as PNG using `image_save_png()`
- [ ] Notification toast: "Screenshot saved"
- [ ] Commit: `"kernel: screenshot capture"`

### 9.5 Tooltip Support

**Prompt:** Tooltips appear after hovering over a UI element for 500ms and display helpful text near the cursor. Implement a simple tooltip manager: widgets register tooltip text via `tooltip_set(widget, text)`. A global timer tracks hover duration — when it exceeds 500ms without mouse movement, render a small rounded-rect popup (semi-transparent dark background, white text, drop shadow) near the cursor position. Auto-dismiss when the mouse moves. Add tooltips to all window buttons ("Close", "Minimize", "Maximize"), taskbar buttons (window title), and system tray icons ("Volume", "Network", "Clock"). After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: tooltip support"`.


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
