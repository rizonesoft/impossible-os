# Phase 02 — UI Framework

> **Goal:** Transform the basic framebuffer desktop into a modern, Windows 11-quality
> graphical experience with compositing effects, TrueType fonts, image decoding,
> system icons, context-aware cursors, DPI scaling, and fluid animations.

---

## 1. 2D Compositing Library
> *Research: [01_2d_compositor.md](research/phase_02_ui_framework/01_2d_compositor.md)*

### 1.1 Core Surface & Primitives

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `gfx_surface_t` struct (pixels, width, height, stride), `gfx_color_t` (0xAARRGGBB) with macros, and all drawing primitives (`gfx_fill_rect`, `gfx_draw_rect`, `gfx_fill_rounded_rect`, `gfx_draw_rounded_rect`, `gfx_fill_circle`, `gfx_draw_line`) exist in `src/kernel/gfx/gfx_core.c`. Verify the dirty rectangle tracker works. Check that `docs/architecture/gfx-library.md` exists and covers the surface/primitive API — create or update if missing. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `gfx_blit`, `gfx_blit_alpha`, and `gfx_fill_rect_alpha` exist in `src/kernel/gfx/gfx_blend.c`. Verify pre-multiplied alpha is used (integer-only math, no floating point). Check that `docs/architecture/gfx-library.md` covers the blending API — update if not. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Create `src/kernel/gfx/gfx_blend.c`
- [x] Implement `gfx_blit(dst, dx, dy, src, sx, sy, w, h)` — per-pixel alpha blit
- [x] Implement `gfx_blit_alpha(dst, dx, dy, src, alpha)` — blit with global alpha
- [x] Implement `gfx_fill_rect_alpha(surface, x, y, w, h, color)` — alpha from color channel
- [x] Use pre-multiplied alpha (50% fewer multiplies in hot path)
- [x] Integer-only math in blending (no floating point)
- [x] Commit: `"gfx: alpha blending and compositing"`

### 1.3 Gradients

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `gfx_gradient_t` struct, `gfx_fill_gradient_rect()`, `gfx_fill_gradient_rounded()`, and radial gradient fill exist in `src/kernel/gfx/gfx_gradient.c`. Check that `docs/architecture/gfx-library.md` covers gradients — update if not. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Create `src/kernel/gfx/gfx_gradient.c`
- [x] Define `gfx_gradient_t` struct (start color, end color, direction)
- [x] Implement `gfx_fill_gradient_rect()` — vertical + horizontal linear gradients
- [x] Implement `gfx_fill_gradient_rounded()` — gradient with rounded corners
- [x] Implement radial gradient fill
- [x] Commit: `"gfx: gradient fills"`

### 1.4 Blur & Material Effects

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `gfx_blur_rect`, `gfx_acrylic`, `gfx_mica`, `gfx_drop_shadow`, and `gfx_reveal_highlight` exist in `src/kernel/gfx/gfx_blur.c` and `gfx_effects.c`. Verify the two-pass box blur is O(n) per pixel. Check Mica samples wallpaper, desaturates, and tints. Check that `docs/architecture/gfx-library.md` covers blur/material effects — update if not. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm SSE2 alpha blending, gradient fill, and blur are implemented with `_mm_loadu_si128`/`_mm_storeu_si128`. Verify `fxsave`/`fxrstor` wrappers protect user FPU state. Confirm gfx files compile with `-msse2`. Check AVX2 runtime detection via CPUID. Verify compositor frame time <8ms at 1280×720. Check that `docs/architecture/gfx-library.md` covers SIMD optimizations — update if not. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `stb_truetype.h` exists in `include/`, memory redirects (`STBTT_malloc → kmalloc`, `STBTT_free → kfree`) work, `src/kernel/gfx/gfx_text.c` and `include/font_mgr.h` exist with `ttf_mgr_init`, `ttf_get`, `ttf_draw_char`, `ttf_draw_string`, `ttf_measure_width`, `ttf_line_height`. Verify fonts load from `C:\Impossible\Fonts\` at boot. Check that `docs/architecture/font-rendering.md` exists — create or update if missing. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm Selawik and Cascadia Code .ttf files exist in `resources/fonts/`, the Makefile copies them to the sysroot, and font slots (FONT_UI, FONT_UI_BOLD, FONT_MONO, FONT_MONO_BOLD, FONT_UI_HEAVY) are defined. Check license files exist. Run `make clean && make all && make run` and verify fonts display correctly. Fix any inconsistencies in the TODO items below.


- [x] Download **Selawik** Regular + Semibold + Bold (~132 KB total, MIT license)
- [x] Download **Cascadia Code** Regular + Bold (~1.2 MB total, OFL 1.1)
- [x] Place `.ttf` files in `resources/fonts/`
- [x] Update Makefile to copy fonts into sysroot
- [x] Define font slots: `FONT_UI`, `FONT_UI_BOLD`, `FONT_MONO`, `FONT_MONO_BOLD`, `FONT_UI_HEAVY`
- [x] Add font license files to `resources/fonts/LICENSE-*`
- [x] *(Stretch)* Add **Inter** as an alternative UI font
- [x] Commit: `"resources: Selawik + Cascadia Code font bundle"`

### 2.3 Glyph Caching

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm glyph bitmaps are cached for ASCII range (32-126) at common pixel sizes. Check the `ttf_draw_char` hot path hits the cache before falling back to live rasterization. Check that `docs/architecture/font-rendering.md` covers caching — update if not. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Pre-rasterize ASCII 32–126 at common sizes (12, 14, 16, 20, 24px) at boot
- [x] Cache struct: bitmap, width, height, x/y offset, advance per glyph
- [x] Cache size: ~95 KB (95 chars × 5 sizes × 4 font slots × ~50 bytes)
- [x] Fast lookup in `font_draw_char()` — bypass stb_truetype for cached glyphs
- [x] Benchmark: cached vs. uncached rendering speed
- [x] Commit: `"desktop: glyph cache for fast text rendering"`

### 2.4 Replace Bitmap Font

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm all old bitmap `font_draw_char`/`font_draw_string` calls in `desktop.c`, `wm.c`, `controls.c` have been replaced with TrueType `ttf_draw_string`. Verify the bitmap font is kept as early boot fallback. Check shell uses FONT_MONO, window titles use FONT_UI_BOLD, buttons use FONT_UI. Run `make clean && make all && make run` and verify TrueType fonts render correctly throughout the UI. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `stb_image.h` exists in `include/`, `src/kernel/image.c` and `include/kernel/image.h` exist with `image_load`, `image_load_mem`, `image_free`, and `image_t` struct. Verify `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR` are defined. Verify RGBA→BGRA channel swap in `rgba_to_bgra()`. **CRITICAL:** Verify the tiered allocator is used — `STBI_MALLOC` must route allocations >64 KB through `pmm_alloc_contiguous()` (NOT `kmalloc`), because the kernel heap is only 2 MiB and a 1280×720 RGBA image is 3.6 MiB. Check `image_free()` correctly detects PMM vs kmalloc via `from_pmm` flag. Verify freestanding header shims exist in `include/freestanding/`. Check that `docs/architecture/image-system.md` covers the tiered allocator. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.

> **⚠️ Heap Gotcha:** `STBI_MALLOC/STBI_REALLOC/STBI_FREE` are NOT plain `kmalloc`/`kfree`. They use a tiered allocator: ≤64 KB → `kmalloc`, >64 KB → `pmm_alloc_contiguous()`. This avoids the same 2 MiB heap exhaustion that broke the framebuffer back buffer (commit `9722a74`). The `image_t.from_pmm` flag tracks provenance for correct deallocation.

- [x] Copy `stb_image.h` from `tools/` to `include/`
- [x] Create `src/kernel/image.c` with tiered allocator (`STBI_MALLOC` → kmalloc ≤64KB / PMM >64KB)
- [x] Define `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR` for kernel freestanding
- [x] Create freestanding header shims (`include/freestanding/`) for `<stdlib.h>`, `<string.h>`, etc.
- [x] Define `image_t` struct (pixels, width, height, from_pmm, alloc_size)
- [x] Implement `image_load(path)` — load from VFS, decode, RGBA→BGRA conversion
- [x] Implement `image_load_mem(data, size)` — decode from memory buffer
- [x] Implement `image_free(img)` — free decoded data (PMM or kmalloc)
- [x] Create `docs/architecture/image-system.md`
- [x] Commit: `"kernel: runtime image decoding (stb_image)"` (`1ee5c6a`)

### 3.2 Image Scaling

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `image_scale()` in `src/kernel/image_scale.c` supports all 5 fit modes (`IMAGE_FIT_STRETCH`, `IMAGE_FIT_FILL`, `IMAGE_FIT_FIT`, `IMAGE_FIT_CENTER`, `IMAGE_FIT_TILE`). Verify bilinear interpolation uses 16.16 fixed-point math (no floats). Verify box-filter downscaling activates for >2x reduction. Check output buffers use the tiered PMM/kmalloc allocator. Verify `docs/architecture/image-system.md` covers scaling. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Implement `image_scale(src, target_w, target_h, mode)` — bilinear interpolation (16.16 fixed-point)
- [x] Support fit modes: `IMAGE_FIT_FILL`, `IMAGE_FIT_FIT`, `IMAGE_FIT_STRETCH`, `IMAGE_FIT_CENTER`, `IMAGE_FIT_TILE`
- [x] Implement box-filter downscaling (better quality than bilinear for large reductions)
- [x] Commit: `"kernel: image scaling with bilinear interpolation"`

### 3.3 JPG/PNG Wallpaper

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `load_wallpaper()` in `desktop.c` uses `image_load()` + `image_scale()` instead of reading raw BGRA from VFS. Verify wallpaper path is read from Codex `System\Theme\Wallpaper` (default: `C:\Impossible\Wallpapers\default.jpg`). Verify fit mode is read from Codex `WallpaperMode` and maps to `image_fit_t` enum. Verify Makefile copies JPEG as-is (no `jpg2raw` conversion for wallpaper). Verify `wallpaper.raw` and `bg.raw` are no longer created. Run `make clean && make all && make run`. Fix any inconsistencies below.


- [x] Modify `desktop.c` to load wallpaper via `image_load()` instead of raw initrd
- [x] Support JPEG and PNG wallpapers directly (no build-time `jpg2raw` conversion)
- [x] Scale wallpaper to fit screen using `image_scale()`
- [x] Read wallpaper path and fit mode from Codex (`System\Theme\Wallpaper`, `WallpaperMode`)
- [x] Cache scaled wallpaper (don't re-decode every frame)
- [x] Commit: `"desktop: JPEG/PNG wallpaper loading"`

### 3.4 Image Saving

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `stb_image_write.h` exists in `include/`. Verify `image_save.c` defines `STBI_WRITE_NO_STDIO`, redirects `STBIW_MALLOC/FREE/REALLOC` to kernel heap, and implements `image_save_bmp()` and `image_save_png()` using `stbi_write_*_to_func` with a VFS write callback. Verify BGRA→RGBA channel swap before saving. Verify Makefile compiles `image_save.c` with SSE2 and freestanding shims. Run `make clean && make all && make run`. Fix any inconsistencies below.


- [x] Add `stb_image_write.h` to `include/` (public domain)
- [x] Implement `image_save_bmp(img, path)` — save to VFS
- [x] Implement `image_save_png(img, path)` — save to VFS
- [x] Used by: future Paint app (Save As), screenshot feature
- [x] Commit: `"kernel: image saving (BMP/PNG)"`

---

## 4. System Icon Store
> *Research: [06_icon_store.md](research/phase_02_ui_framework/06_icon_store.md)*

### 4.1 Icon Store Basics

**Prompt:** The icon store is a centralized, hybrid icon system using two rendering backends. **Monochrome icons** (system/toolbar/file type icons — ~60 icons) are rendered from Fluent UI icon fonts (TTF) via the existing stb_truetype font system from §2. This gives resolution-independent vector rendering at any size with zero PNG storage overhead. Icons are rendered on demand, cached as BGRA bitmaps keyed by (id, size, color). **Color icons** (desktop app icons, branded icons — ~15 icons) are stored in `apps.ires` for multi-color detail at large sizes (48, 72, 128, 256). `icon_store_init()` loads the icon fonts and `apps.ires` at boot. `icon_get(id, size)` checks the cache, rasterizes from font if needed. `icon_draw()` blits with alpha blending. Foreground color is themeable via Codex `System\Theme\IconColor`. After completing all items, create `docs/architecture/icon-store.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: system icon store"`.


- [ ] Define `system_icon_t` enum (~60 monochrome + ~15 color icons)
- [ ] Define `icon_entry_t` struct (cached bitmap, source type: font glyph or IRES)
- [ ] Create `include/icon_store.h` and `src/kernel/icon_store.c`
- [ ] Implement `icon_store_init()` — load Fluent icon fonts + `apps.ires`
- [ ] Implement `icon_get(id, size)` — return cached bitmap, rasterize from font on first access
- [ ] Implement `icon_get_colored(id, size, color)` — font icons with custom tint
- [ ] Implement `icon_get_by_name(name)` — lookup by string name
- [ ] Implement `icon_draw(surface, icon, x, y)` — blit with alpha blending
- [ ] Implement `icon_draw_scaled(surface, icon, x, y, target_size)` — scale for arbitrary sizes
- [ ] Glyph cache: LRU eviction for (id, size, color) tuples to bound memory
- [ ] Commit: `"desktop: system icon store"`

### 4.2 Font-Based Icon Rendering

**Prompt:** Integrate the Fluent UI icon fonts with the existing stb_truetype font system from §2. Load four font variants: `FluentSystemIcons-Filled.ttf` (solid icons — primary), `FluentSystemIcons-Regular.ttf` (outlined icons — secondary/inactive states), `FluentSystemIcons-Light.ttf` (thin strokes — subtle UI hints), and `FluentSystemIcons-Resizable.ttf` (optimised for small sizes). Map `system_icon_t` enum values to Unicode codepoints using the Fluent codepoint mapping file. `icon_render_glyph(font_variant, codepoint, size, color)` rasterizes the glyph via `stbtt_GetCodepointBitmap()`, converts the alpha bitmap to BGRA with the specified foreground color, and returns a cached `icon_entry_t`. The variant can be selected per-context: Filled for toolbar buttons, Regular for menus, Light for disabled states. After completing all items, update `docs/architecture/icon-store.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: font-based icon rendering"`.


- [ ] Load four Fluent icon fonts via font manager at boot:
  - [ ] `FluentSystemIcons-Filled.ttf` — solid icons (toolbars, active states)
  - [ ] `FluentSystemIcons-Regular.ttf` — outlined icons (menus, secondary)
  - [ ] `FluentSystemIcons-Light.ttf` — thin strokes (disabled states, hints)
  - [ ] `FluentSystemIcons-Resizable.ttf` — optimised for small sizes (16px and below)
- [ ] Build codepoint mapping table: `system_icon_t` → Unicode Private Use Area codepoint
- [ ] Implement `icon_render_glyph(variant, codepoint, size, color)`:
  - [ ] Rasterize via `stbtt_GetCodepointBitmap()` at requested point size
  - [ ] Convert alpha bitmap → BGRA with foreground color tint
  - [ ] Cache result keyed by (codepoint, size, color, variant)
- [ ] Variant selection per context: Filled for active, Regular for menus, Light for disabled
- [ ] Theme integration: icon color from Codex `System\Theme\IconColor`
- [ ] Commit: `"desktop: font-based icon rendering"`

### 4.3 Fluent UI Icon Assets

**Prompt:** Download the four Fluent UI System Icon font files (MIT, Microsoft) from the GitHub repo (`fonts/` directory). Also source ~15 multi-color PNG icons for desktop/app use that cannot be represented as monochrome glyphs (e.g., app logos, branded folder icons with color accents). Install fonts to `C:\Impossible\Fonts\` alongside the existing text fonts. Install `apps.ires` to `C:\Impossible\System\`. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"resources: Fluent UI icon fonts and color icons"`.


- [ ] Download from https://github.com/microsoft/fluentui-system-icons/tree/main/fonts:
  - [ ] `FluentSystemIcons-Filled.ttf`
  - [ ] `FluentSystemIcons-Regular.ttf`
  - [ ] `FluentSystemIcons-Light.ttf`
  - [ ] `FluentSystemIcons-Resizable.ttf`
- [ ] Install fonts to `C:\Impossible\Fonts\` (sysroot copy in Makefile)
- [ ] Download Fluent codepoint mapping file for enum → Unicode translation
- [ ] Source ~15 multi-color PNGs for desktop app icons (computer, recycle bin, etc.)
- [ ] Organize color PNGs in `resources/icons/apps/{48,72,128,256}/`
- [ ] Commit: `"resources: Fluent UI icon fonts and color icons"`

### 4.4 File Type Mapping

**Prompt:** Map file extensions to icon IDs using Codex entries under `System\FileTypes\{ext}\Icon`. The `icon_for_extension(".txt")` function looks up the extension in Codex and returns the matching `system_icon_t` enum value. Fall back to `ICON_FILE_DEFAULT` for unknown extensions. Font-based icons are used for file types in list views (monochrome, fast). This is used everywhere files are displayed: File Manager, desktop icons, Open/Save dialogs, and the Start menu's app list. Common mappings: txt/md/log → text, c/h/py/js → code, jpg/png/bmp/gif → image, mp3/wav/ogg → audio, zip/tar/gz → archive. After completing all items, update `docs/architecture/icon-store.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: file type icon mapping"`.


- [ ] Implement `icon_for_extension(ext)` — look up icon by file extension
- [ ] Define extension → icon mapping in Codex (`System\FileTypes`)
- [ ] Common mappings: txt/md/log → text, c/h/py/js → code, jpg/png/bmp → image, mp3/wav → audio, zip/tar → archive, exe → executable
- [ ] Commit: `"desktop: file type icon mapping"`

### 4.5 IRES Format (Color Icons)

**Prompt:** `.ires` (Icon Resource) is a custom binary format for all multi-color icons in Impossible OS — everything that needs color, shading, or visual detail beyond what monochrome font glyphs can provide. This includes colored folders (yellow closed, blue documents, green pictures), file type icons (red PDF, green spreadsheet, blue code), drive icons (HDD, USB, network with color accents), desktop icons (computer, recycle bin, printer), and app icons (text editor, media player, settings, terminal). Bundled into a single `icons.ires` with sizes 16, 24, 32, 48, 64, 72, 128, 256 for HiDPI support. Format: 16-byte header (magic `IRES`, version, icon count, size count, flags), index table (icon ID, name offset, per-size pixel data offset + dimensions), name strings, packed BGRA pixel data. A host-side `irespack` build tool creates the file at build time. At runtime, `ires_load()` reads it in one `vfs_read()`. The icon store falls back to font glyphs when a color icon is unavailable for a given ID. After completing all items, update `docs/architecture/icon-store.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: IRES color icon format"`.


- [ ] Define `.ires` binary format spec (header + index + name table + BGRA pixel data)
- [ ] Write `tools/irespack.c` — reads PNGs, outputs `.ires` (uses stb_image for decode)
- [ ] Build rule: `icons.ires` from `resources/icons/color/{16,24,32,48,64,72,128,256}/*.png`
- [ ] Color icons needed:
  - [ ] Folders: folder_closed, folder_open, folder_documents, folder_pictures, folder_music, folder_downloads
  - [ ] File types: file_default, file_text, file_image, file_audio, file_video, file_archive, file_exe, file_code, file_pdf
  - [ ] Drives: drive_local, drive_removable, drive_network, drive_optical
  - [ ] Desktop: computer, recycle_bin_empty, recycle_bin_full, printer, network
  - [ ] Apps: app_default, text_editor, media_player, settings, terminal, file_manager, calculator, paint, browser
- [ ] Install to `C:\Impossible\System\icons.ires`
- [ ] Implement `ires_load(path)` in kernel — parse header, index, load pixel data
- [ ] Icon theme switching via Codex `System\Theme\IconPack`
- [ ] Commit: `"desktop: IRES color icon format"`

### 4.6 ICO File Loader (App Compatibility)

**Prompt:** `.ico` files are the standard Windows icon format — a container holding multiple sizes (16, 32, 48, 256) as embedded BMP or PNG data. Third-party apps and user-created shortcuts need `.ico` support for their custom icons. The `.ico` header is 6 bytes (reserved, type=1, count), followed by 16-byte directory entries (width, height, offset, size), then image data at each offset. If the image data starts with PNG magic (`\x89PNG`), pass it to `image_load_mem()`. Otherwise parse it as a BMP DIB (headerless bitmap). `ico_load(path)` returns an `icon_entry_t` with all available sizes. This is used by File Manager, desktop shortcuts, and the Start menu for app icons. After completing all items, update `docs/architecture/icon-store.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: ICO file loader"`.


- [ ] Implement `ico_load(path)` — parse `.ico` container, extract all sizes
- [ ] Handle embedded PNG data (pass to `image_load_mem()`)
- [ ] Handle embedded BMP DIB data (parse headerless bitmap)
- [ ] Return `icon_entry_t` with available sizes populated
- [ ] Used by: File Manager (exe icons), desktop shortcuts, Start menu app list
- [ ] Commit: `"desktop: ICO file loader"`

---

## 5. Cursor Pack
> *Research: [07_cursor_pack.md](research/phase_02_ui_framework/07_cursor_pack.md)*

### 5.1 Cursor Manager

**Prompt:** The cursor manager replaces the current hardcoded arrow cursor in `mouse.c` with a system that supports 11 different shapes loaded from a `cursors.cres` (Cursor Resource) file. Each `cursor_sprite` has width, height, hotspot coordinates (the pixel that corresponds to the click position), and pixel data at three sizes (24×24, 32×32, 64×64) for HiDPI scaling. `cursor_init()` loads `C:\Impossible\System\cursors.cres` — one `vfs_read()`, all cursors parsed. `cursor_set_shape(shape)` switches the active cursor. `cursor_draw` saves the pixels underneath before blitting (so `cursor_restore` can undo without redrawing the entire frame). The hotspot offset must be applied in `wm_handle_mouse` so clicks register at the correct position. Keep an embedded fallback arrow as a C byte array for pre-initrd boot when VFS isn't available. After completing all items, create `docs/architecture/cursor-system.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: cursor manager with multiple shapes"`.


- [ ] Create `include/cursor.h` with `cursor_shape_t` enum (11 shapes)
- [ ] Define `cursor_sprite` struct (width, height, hotspot_x, hotspot_y, pixels, sizes: 24/32/64)
- [ ] Create `src/kernel/drivers/cursor.c`
- [ ] Implement `cres_load(path)` — parse `.cres` file, load all cursor sprites
- [ ] Implement `cursor_init()` — load `cursors.cres` from `C:\Impossible\System\`, fall back to embedded arrow
- [ ] Implement `cursor_set_shape(shape)` — switch active cursor
- [ ] Implement `cursor_get_shape()` — get current shape
- [ ] Implement `cursor_draw(x, y)` — draw with alpha blending, save pixels underneath
- [ ] Implement `cursor_restore()` — restore saved pixels
- [ ] Implement `cursor_get_hotspot(hx, hy)` — for click position adjustment
- [ ] Commit: `"drivers: cursor manager with multiple shapes"`

### 5.2 Cursor Assets & CRES Format

**Prompt:** Design or source 11 cursor PNGs at three sizes (24×24, 32×32, 64×64) with transparent backgrounds for HiDPI support. The arrow is the default pointer with hotspot at top-left (1,1). The hand cursor indicates clickable elements (hotspot at fingertip). The I-beam (text cursor) has its hotspot at the middle of the vertical line. Resize cursors use cardinal directions matching the resize edge. Organize source PNGs in `resources/cursors/{24,32,64}/`. A host-side `crespack` build tool packs all sizes into a single `cursors.cres` file — format: header (magic `CRES`, version, cursor count, size count), index table (one entry per cursor: shape ID, name, hotspot_x, hotspot_y per size, pixel data offset per size), followed by packed BGRA pixel data. At runtime, `cres_load()` reads the file in one `vfs_read()` and populates the cursor cache. The fallback arrow must be embedded as a `static const uint32_t cursor_fallback[]` byte array. Install `cursors.cres` to `C:\Impossible\System\cursors.cres`. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"resources: cursor sprite pack (CRES format)"`.


- [ ] Create/design cursor PNGs at 24×24, 32×32, 64×64 (transparent background):
  - [ ] `arrow` — default pointer, hotspot (1,1)
  - [ ] `hand` — pointing hand (links, buttons), hotspot (6,1)
  - [ ] `text` — I-beam (text fields, terminal), hotspot (4,10)
  - [ ] `move` — 4-way arrows (window drag), hotspot (10,10)
  - [ ] `resize_ns` — ↕ vertical resize, hotspot (6,10)
  - [ ] `resize_ew` — ↔ horizontal resize, hotspot (10,6)
  - [ ] `resize_nwse` — ↘ diagonal resize, hotspot (8,8)
  - [ ] `resize_nesw` — ↗ diagonal resize, hotspot (8,8)
  - [ ] `wait` — hourglass/spinner, hotspot (8,12)
  - [ ] `crosshair` — + selection, hotspot (10,10)
  - [ ] `forbidden` — ⊘ circle-slash, hotspot (10,10)
- [ ] Organize in `resources/cursors/{24,32,64}/`
- [ ] Define `.cres` binary format (header + index with hotspots + BGRA pixel data, multi-size)
- [ ] Write `tools/crespack.c` — reads PNGs, outputs `cursors.cres`
- [ ] Install to `C:\Impossible\System\cursors.cres`
- [ ] Embed fallback arrow as byte array for pre-initrd boot
- [ ] Commit: `"resources: cursor sprite pack (CRES format)"`

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
