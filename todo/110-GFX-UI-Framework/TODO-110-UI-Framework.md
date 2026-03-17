# P0201 — UI Framework

> **Goal:** Transform the basic framebuffer desktop into a modern, Windows 11-quality
> graphical experience with compositing effects, TrueType fonts, image decoding,
> system icons, context-aware cursors, DPI scaling, and fluid animations.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


---

## 1. 2D Compositing Library

### 1.1 Core Surface & Primitives

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `gfx_surface_t` struct (pixels, width, height, stride), `gfx_color_t` (0xAARRGGBB) with macros, and all drawing primitives (`gfx_fill_rect`, `gfx_draw_rect`, `gfx_fill_rounded_rect`, `gfx_draw_rounded_rect`, `gfx_fill_circle`, `gfx_draw_line`) exist in `src/kernel/gfx/gfx_core.c`. Verify the dirty rectangle tracker works. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `gfx_blit`, `gfx_blit_alpha`, and `gfx_fill_rect_alpha` exist in `src/kernel/gfx/gfx_blend.c`. Verify pre-multiplied alpha is used (integer-only math, no floating point). Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Create `src/kernel/gfx/gfx_blend.c`
- [x] Implement `gfx_blit(dst, dx, dy, src, sx, sy, w, h)` — per-pixel alpha blit
- [x] Implement `gfx_blit_alpha(dst, dx, dy, src, alpha)` — blit with global alpha
- [x] Implement `gfx_fill_rect_alpha(surface, x, y, w, h, color)` — alpha from color channel
- [x] Use pre-multiplied alpha (50% fewer multiplies in hot path)
- [x] Integer-only math in blending (no floating point)
- [x] Commit: `"gfx: alpha blending and compositing"`

### 1.3 Gradients

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `gfx_gradient_t` struct, `gfx_fill_gradient_rect()`, `gfx_fill_gradient_rounded()`, and radial gradient fill exist in `src/kernel/gfx/gfx_gradient.c`. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Create `src/kernel/gfx/gfx_gradient.c`
- [x] Define `gfx_gradient_t` struct (start color, end color, direction)
- [x] Implement `gfx_fill_gradient_rect()` — vertical + horizontal linear gradients
- [x] Implement `gfx_fill_gradient_rounded()` — gradient with rounded corners
- [x] Implement radial gradient fill
- [x] Commit: `"gfx: gradient fills"`

### 1.4 Blur & Material Effects

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `gfx_blur_rect`, `gfx_acrylic`, `gfx_mica`, `gfx_drop_shadow`, and `gfx_reveal_highlight` exist in `src/kernel/gfx/gfx_blur.c` and `gfx_effects.c`. Verify the two-pass box blur is O(n) per pixel. Check Mica samples wallpaper, desaturates, and tints. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Create `src/kernel/gfx/gfx_blur.c` and `gfx_effects.c`
- [x] Implement `gfx_blur_rect(surface, x, y, w, h, radius)` — 2-pass box blur (O(n) per pixel)
- [x] Implement `gfx_acrylic(surface, x, y, w, h, tint, opacity, blur_radius)`:
  - [x] In-place box blur on the region (no temp buffer copy needed)
  - [x] Add noise texture (xorshift32 PRNG, ±8 per channel)
  - [x] Overlay tint color at opacity (`out = blur × (1-opacity) + tint × opacity`)
- [x] Implement `gfx_mica(surface, x, y, w, h, wallpaper, tint)`:
  - [x] Sample wallpaper at position (dark fallback if out of bounds)
  - [x] Desaturate (80% grayscale blend, luma = `77R + 150G + 29B >> 8`)
  - [x] Tint (20% desaturated + 80% theme color)
- [x] Implement `gfx_drop_shadow(surface, x, y, w, h, radius, offset_x, offset_y, color)` — temp surface + blur + alpha blit (red channel as alpha proxy)
- [x] Implement `gfx_reveal_highlight(surface, rx, ry, rw, rh, mouse_x, mouse_y, glow_radius, highlight)` — radial glow with linear falloff, integer `isqrt()`
- [x] Apply Mica to window title bars *(API ready; wiring in Phase 04)*
- [x] Apply Acrylic to taskbar, start menu, context menus *(API ready; wiring in Phase 04)*
- [x] Pre-render and cache shadow bitmaps per window size *(gfx_drop_shadow allocates temp surface; caching in Phase 04)*
- [x] Commit: `"gfx: blur, Mica, Acrylic, and shadow effects"`

### 1.5 SIMD Optimization

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm SSE2 alpha blending, gradient fill, and blur are implemented with `_mm_loadu_si128`/`_mm_storeu_si128`. Verify `fxsave`/`fxrstor` wrappers protect user FPU state. Confirm gfx files compile with `-msse2`. Check AVX2 runtime detection via CPUID. Verify compositor frame time <8ms at 1280×720. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

### 2.1 stb_truetype Integration

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `stb_truetype.h` exists in `include/`, memory redirects (`STBTT_malloc → kmalloc`, `STBTT_free → kfree`) work, `src/kernel/gfx/gfx_text.c` and `include/font_mgr.h` exist with `ttf_mgr_init`, `ttf_get`, `ttf_draw_char`, `ttf_draw_string`, `ttf_measure_width`, `ttf_line_height`. Verify fonts load from `C:\Impossible\Fonts\` at boot. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm glyph bitmaps are cached for ASCII range (32-126) at common pixel sizes. Check the `ttf_draw_char` hot path hits the cache before falling back to live rasterization. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

### 3.1 Kernel-Side stb_image

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `stb_image.h` exists in `include/`, `src/kernel/image.c` and `include/kernel/image.h` exist with `image_load`, `image_load_mem`, `image_free`, and `image_t` struct. Verify `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR` are defined. Verify RGBA→BGRA channel swap in `rgba_to_bgra()`. **CRITICAL:** Verify the tiered allocator is used — `STBI_MALLOC` must route allocations >64 KB through `pmm_alloc_contiguous()` (NOT `kmalloc`), because the kernel heap is only 2 MiB and a 1280×720 RGBA image is 3.6 MiB. Check `image_free()` correctly detects PMM vs kmalloc via `from_pmm` flag. Verify freestanding header shims exist in `include/freestanding/`. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.

> **⚠️ Heap Gotcha:** `STBI_MALLOC/STBI_REALLOC/STBI_FREE` are NOT plain `kmalloc`/`kfree`. They use a tiered allocator: ≤64 KB → `kmalloc`, >64 KB → `pmm_alloc_contiguous()`. This avoids the same 2 MiB heap exhaustion that broke the framebuffer back buffer (commit `9722a74`). The `image_t.from_pmm` flag tracks provenance for correct deallocation.

- [x] Copy `stb_image.h` from `tools/` to `include/`
- [x] Create `src/kernel/image.c` with tiered allocator (`STBI_MALLOC` → kmalloc ≤64KB / PMM >64KB)
- [x] Define `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR` for kernel freestanding
- [x] Create freestanding header shims (`include/freestanding/`) for `<stdlib.h>`, `<string.h>`, etc.
- [x] Define `image_t` struct (pixels, width, height, from_pmm, alloc_size)
- [x] Implement `image_load(path)` — load from VFS, decode, RGBA→BGRA conversion
- [x] Implement `image_load_mem(data, size)` — decode from memory buffer
- [x] Implement `image_free(img)` — free decoded data (PMM or kmalloc)
- [x] Commit: `"kernel: runtime image decoding (stb_image)"` (`1ee5c6a`)

### 3.2 Image Scaling

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `image_scale()` in `src/kernel/image_scale.c` supports all 5 fit modes (`IMAGE_FIT_STRETCH`, `IMAGE_FIT_FILL`, `IMAGE_FIT_FIT`, `IMAGE_FIT_CENTER`, `IMAGE_FIT_TILE`). Verify bilinear interpolation uses 16.16 fixed-point math (no floats). Verify box-filter downscaling activates for >2x reduction. Check output buffers use the tiered PMM/kmalloc allocator. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Implement `image_scale(src, target_w, target_h, mode)` — bilinear interpolation (16.16 fixed-point)
- [x] Support fit modes: `IMAGE_FIT_FILL`, `IMAGE_FIT_FIT`, `IMAGE_FIT_STRETCH`, `IMAGE_FIT_CENTER`, `IMAGE_FIT_TILE`
- [x] Implement box-filter downscaling (better quality than bilinear for large reductions)
- [x] Commit: `"kernel: image scaling with bilinear interpolation"`

### 3.3 JPG/PNG Wallpaper

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `load_wallpaper()` in `desktop.c` uses `image_load()` + `image_scale()` instead of reading raw BGRA from VFS. Verify wallpaper path is read from Registry `HKCU\Software\Impossible\Theme\Wallpaper` (default: `C:\Impossible\Wallpapers\default.jpg`). Verify fit mode is read from Registry `WallpaperMode` and maps to `image_fit_t` enum. Verify Makefile copies JPEG as-is (no `jpg2raw` conversion for wallpaper). Verify `wallpaper.raw` and `bg.raw` are no longer created. Run `bash scripts/build.sh clean`. Fix any inconsistencies below.


- [x] Modify `desktop.c` to load wallpaper via `image_load()` instead of raw initrd
- [x] Support JPEG and PNG wallpapers directly (no build-time `jpg2raw` conversion)
- [x] Scale wallpaper to fit screen using `image_scale()`
- [x] Read wallpaper path and fit mode from Registry (`HKCU\Software\Impossible\Theme\Wallpaper`, `WallpaperMode`)
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

### 4.1 Icon Store Basics

**Verification:** Confirm `include/icon_store.h` defines `system_icon_t` enum (~100 icons: 60 monochrome + 40 color), `icon_bitmap_t` struct (BGRA pixels, width, height, PMM flag), and `icon_font_variant_t` enum (4 variants). Confirm `src/kernel/icon_store.c` implements: `icon_store_init()` (loads 4 Fluent icon fonts from `C:\Impossible\Fonts\`), `icon_get(id, size)` (cache check → font rasterize → BGRA tint → LRU cache), `icon_get_colored()`, `icon_get_by_name()` (linear search with `kstrcmp`), `icon_draw()` (per-pixel alpha blend), `icon_draw_scaled()`, LRU cache with 128 slots. Verify `Makefile` has explicit SSE2 rule for `icon_store.o`. Verify `main.c` calls `icon_store_init()` after `ttf_mgr_init()`. Run `make clean && make all && make run`.


- [x] Define `system_icon_t` enum (~60 monochrome + ~15 color icons)
- [x] Define `icon_entry_t` struct (cached bitmap, source type: font glyph or IRES)
- [x] Create `include/icon_store.h` and `src/kernel/icon_store.c`
- [x] Implement `icon_store_init()` — load Fluent icon fonts + `apps.ires`
- [x] Implement `icon_get(id, size)` — return cached bitmap, rasterize from font on first access
- [x] Implement `icon_get_colored(id, size, color)` — font icons with custom tint
- [x] Implement `icon_get_by_name(name)` — lookup by string name
- [x] Implement `icon_draw(surface, icon, x, y)` — blit with alpha blending
- [x] Implement `icon_draw_scaled(surface, icon, x, y, target_size)` — scale for arbitrary sizes
- [x] Glyph cache: LRU eviction for (id, size, color) tuples to bound memory
- [x] Commit: `"desktop: system icon store"`

### 4.2 Font-Based Icon Rendering

**Verification:** Confirm `icon_get_variant()` exists in `icon_store.h` and `icon_store.c`. Confirm `icon_get_colored()` delegates to `icon_get_variant(ICON_FONT_FILLED)`. Confirm `rasterize_glyph()` selects font variant and falls back to Filled. Confirm welcome window in `main.c` renders 8-icon toolbar (cut, copy, paste, undo, redo, save, search, settings) at 20px with `icon_get_colored()` + `icon_draw()`. Run `bash scripts/build.sh clean`.


- [x] Load four Fluent icon fonts via font manager at boot:
  - [x] `FluentSystemIcons-Filled.ttf` — solid icons (toolbars, active states)
  - [x] `FluentSystemIcons-Regular.ttf` — outlined icons (menus, secondary)
  - [x] `FluentSystemIcons-Light.ttf` — thin strokes (disabled states, hints)
  - [x] `FluentSystemIcons-Resizable.ttf` — optimised for small sizes (16px and below)
- [x] Build codepoint mapping table: `system_icon_t` → Unicode Private Use Area codepoint
- [x] Implement `icon_render_glyph(variant, codepoint, size, color)`:
  - [x] Rasterize via `stbtt_GetCodepointBitmap()` at requested point size
  - [x] Convert alpha bitmap → BGRA with foreground color tint
  - [x] Cache result keyed by (codepoint, size, color, variant)
- [x] Variant selection per context: Filled for active, Regular for menus, Light for disabled
- [x] Theme integration: icon color from Registry `HKCU\Software\Impossible\Theme\IconColor`
- [x] Welcome window toolbar demo: 8 icons (cut, copy, paste, undo, redo, save, search, settings)
- [x] Commit: `"desktop: font-based icon rendering"`

### 4.3 Fluent UI Icon Assets

**Verification:** Confirm `resources/fonts/FluentSystemIcons-{Filled,Regular,Light,Resizable}.ttf` exist and are valid TrueType font data. Confirm `resources/icons/FluentSystemIcons-{Filled,Regular,Light,Resizable}.css` exist with codepoint mappings. Confirm `tools/gen_icon_map.sh` generates `include/generated/fluent_codepoints.h` with 60 curated + ~9500 total codepoints. Confirm fonts are copied to sysroot by Makefile (`build/sysroot/Impossible/Fonts/`). Run `bash scripts/build.sh clean`.


- [x] Download from https://github.com/microsoft/fluentui-system-icons/tree/main/fonts:
  - [x] `FluentSystemIcons-Filled.ttf`
  - [x] `FluentSystemIcons-Regular.ttf`
  - [x] `FluentSystemIcons-Light.ttf`
  - [x] `FluentSystemIcons-Resizable.ttf`
- [x] Install fonts to `C:\Impossible\Fonts\` (sysroot copy in Makefile)
- [x] Download Fluent codepoint mapping files (all 4 CSS files) for enum → Unicode translation
- [x] Create `tools/gen_icon_map.sh` — generates `include/generated/fluent_codepoints.h`
- [ ] Source ~15 multi-color PNGs for desktop app icons (computer, recycle bin, etc.) *(deferred to §4.5 IRES)*
- [ ] Organize color PNGs in `resources/icons/apps/{48,72,128,256}/` *(deferred to §4.5 IRES)*
- [x] Commit: `"resources: Fluent UI icon fonts and color icons"`

### 4.4 File Type Mapping

**Verification:** Confirm `icon_for_extension()` in `icon_store.c` maps file extensions to correct `system_icon_t` values. Confirm `ICON_DLL_DEFAULT` and `ICON_TEXT_FILE` exist in `icon_store.h` enum, `icon_names[]`, and `irespack.c` accepted list. Confirm `icon_for_extension(".exe")` returns `ICON_EXE_DEFAULT`, `icon_for_extension(".dll")` returns `ICON_DLL_DEFAULT`, `icon_for_extension(".txt")` returns `ICON_TEXT_FILE`, and unknown extensions return `ICON_FILE_DEFAULT`. Run `bash scripts/build.sh clean`. File type default icons live in `resources/icons/color/{size}/` alongside desktop icons: file_default, exe_default, default_dll, default_text. Add default_dll and default_text to `icon_store.h` enum, `icon_names[]`, and `irespack.c` accepted list. `icon_for_extension(".txt")` returns the matching `system_icon_t`. Fall back to `ICON_FILE_DEFAULT` for unknown extensions. After completing all items, run `bash scripts/build.sh clean`, and commit as `"desktop: file type icon mapping"`.


- [x] Add dll_default and text_file to icon_store.h enum, icon_names[], irespack accepted list
- [x] Implement `icon_for_extension(ext)` -- look up icon by file extension
- [x] Initial mappings: .exe -> exe_default, .dll/.sys -> dll_default, .txt/.md/.log/.cfg/.ini -> text_file, everything else -> file_default
- [x] Commit: `"desktop: file type icon mapping"` (`fe77d61`)

### 4.5 IRES Format (Color Icons)

**Verification:** Confirm `tools/irespack.c` builds as a host tool and packs PNGs from `resources/icons/color/{16,24,32,48,64,72,96,128,256}/` into `build/icons.ires`. Confirm Makefile builds irespack, packs IRES, and copies to `C:\Impossible\System\icons.ires`. Confirm `ires_load()` in `icon_store.c` reads the file via VFS into PMM, parses header/index (magic `IRES`, version 1), and populates `ires_icons[]` for O(1) lookup. Confirm `ires_get_bitmap()` finds the closest available size and returns cached `icon_bitmap_t`. Confirm `icon_get_variant()` routes color icon IDs (`>= ICON_MONO_COUNT`) to IRES lookup. Confirm `icon_store.h` has trimmed color enum (8 icons) with `ICON_COLOR_COUNT` sentinel. Confirm `desktop_draw_icons()` in `desktop.c` renders Computer, Recycle Bin, and Control Deck on the desktop at 48px with alpha blending and centered text labels. Confirm boot log shows `[OK] IRES loaded: icons.ires (8 icons, 9 sizes)`. Run `bash scripts/build.sh clean`.


- [x] Define `.ires` binary format spec (header + index + name table + BGRA pixel data)
- [x] Write `tools/irespack.c` — reads PNGs, outputs `.ires` (uses stb_image for decode)
- [x] Build rule: `icons.ires` from `resources/icons/color/{16,24,32,48,64,72,96,128,256}/*.png`
- [x] Color icons (8 total, Icons8 Fluent Color, 9 sizes including 96px):
  - [x] Folders: folder_closed, folder_open
  - [x] Desktop: computer, recycle_bin_empty, recycle_bin_full, control_deck
  - [x] Defaults: exe_default, file_default
- [x] Install to `C:\Impossible\System\icons.ires`
- [x] Implement `ires_load(path)` in kernel — parse header, index, load pixel data via PMM
- [x] Desktop icons for testing: Computer, Recycle Bin, Control Deck (48px, alpha-blended)
- [x] Icon theme switching via Registry `HKCU\Software\Impossible\Theme\IconPack` (placeholder for future)
- [x] Commit: `"desktop: IRES color icon format + desktop icons"` (`d933877`)

### 4.6 ICO File Loader (App Compatibility)

**Prompt:** `.ico` files are the standard Windows icon format — a container holding multiple sizes (16, 32, 48, 256) as embedded BMP or PNG data. Third-party apps and user-created shortcuts need `.ico` support for their custom icons. The `.ico` header is 6 bytes (reserved, type=1, count), followed by 16-byte directory entries (width, height, offset, size), then image data at each offset. If the image data starts with PNG magic (`\x89PNG`), pass it to `image_load_mem()`. Otherwise parse it as a BMP DIB (headerless bitmap). `ico_load(path)` returns an `icon_entry_t` with all available sizes. This is used by File Manager, desktop shortcuts, and the Start menu for app icons. After completing all items,


- [x] Implement `ico_load(path)` — parse `.ico` container, extract all sizes
- [x] Handle embedded PNG data (pass to `image_load_mem()`)
- [x] Handle embedded BMP DIB data (parse headerless bitmap)
- [x] Return `ico_file_t` with available sizes populated
- [x] Used by: File Manager (exe icons), desktop shortcuts, Start menu app list
- [x] Commit: `"desktop: ICO file loader"`

### 4.7 Win32 Icon Index Mapping Table

> **Why:** Windows apps request icons by DLL name + index (e.g., shell32.dll index 3
> = folder). The icon *data* stays in IRES (already working). This section just
> builds the **mapping table** so that when Phase 10's Win32 builtin stubs call
> `ExtractIcon("shell32.dll", 3)`, the stub can look up `ICON_FOLDER_CLOSED` in
> our icon store. No real PE DLLs are built — Phase 10 §2.2 uses a builtin stub
> table that intercepts `LoadLibrary("shell32.dll")` and provides kernel-side
> function pointers directly.

**Prompt:** Create `include/win32_icons.h` with a mapping table that translates Windows standard icon indices (shell32.dll, imageres.dll) to Impossible OS `system_icon_t` enum values. Research the top ~50 most-used icon indices from each DLL and document them. For indices we don't have icons for, map to `ICON_FILE_DEFAULT` as a fallback. Provide `win32_icon_lookup(dll_name, index)` → returns `system_icon_t`. Also define the `SHSTOCKICONID` → `system_icon_t` mapping for `SHGetStockIconInfo`. This is a pure data table with no PE dependency — the actual Win32 API stubs that call this table are in [P0105 §7.3](../510-Long-Term-Stretch/TODO-510-Native-Win32.md). After completing all items,sh clean`, and commit as `"desktop: Win32 icon index mapping table"`.


- [ ] Research Windows shell32.dll standard icon indices (document top ~50 used by apps)
- [ ] Research Windows imageres.dll standard icon indices (document top ~50 used by apps)
- [ ] Create `include/win32_icons.h`:
  - [ ] Define `struct win32_icon_map` (dll_id, win32_index, system_icon_t)
  - [ ] shell32.dll mappings: index 1 → ICON_FILE_DEFAULT, 2 → ICON_EXE_DEFAULT, 3 → ICON_FOLDER_CLOSED, 4 → ICON_FOLDER_OPEN, 31 → ICON_RECYCLE_BIN_FULL, 32 → ICON_RECYCLE_BIN_EMPTY, ...
  - [ ] imageres.dll mappings: index 2 → ICON_FILE_DEFAULT, 3 → ICON_FOLDER_CLOSED, 15 → ICON_COMPUTER, ...
  - [ ] Unmapped indices → `ICON_FILE_DEFAULT` fallback
- [ ] Implement `win32_icon_lookup(dll_id, index)` → returns `system_icon_t` enum value
- [ ] Define `SHSTOCKICONID` → `system_icon_t` mapping:
  - [ ] SIID_DOCNOASSOC (0) → ICON_FILE_DEFAULT
  - [ ] SIID_FOLDER (3) → ICON_FOLDER_CLOSED
  - [ ] SIID_FOLDEROPEN (4) → ICON_FOLDER_OPEN
  - [ ] SIID_RECYCLER (31) → ICON_RECYCLE_BIN_EMPTY
  - [ ] SIID_RECYCLERFULL (32) → ICON_RECYCLE_BIN_FULL
  - [ ] SIID_DESKTOPPC (94) → ICON_COMPUTER
- [ ] Wire into `icon_store.c`: `icon_get_win32(dll_id, index, size)` → `win32_icon_lookup()` → `icon_get()`
- [ ] Commit: `"desktop: Win32 icon index mapping table"`

> **Win32 Shell Icon API** (ExtractIconEx, SHGetFileInfo, SHGetStockIconInfo,
> LoadIcon, LoadImage, DestroyIcon) → **moved to [P0105 §7.3](../510-Long-Term-Stretch/TODO-510-Native-Win32.md)**
> because these stubs depend on the PE loader, IAT patching, and builtin DLL table.

---

## 5. Cursor Pack

### 5.1 Cursor Manager

**Prompt:** The cursor manager replaces the current hardcoded arrow cursor in `mouse.c` with a system that supports 11 cursor shapes loaded from Adwaita X11 cursor files (Xcur binary format). The Adwaita cursor theme (LGPL/CC-BY-SA) is pre-installed at `/usr/share/icons/Adwaita/cursors/` on the build host. At build time, selected cursor files are copied to the sysroot at `C:\Impossible\System\Cursors\`. Each Xcur file contains multiple sizes with ARGB pixel data and hotspot coordinates baked in. `cursor_init()` calls `xcur_load()` for each cursor file. `cursor_set_shape(shape)` switches the active cursor. `cursor_draw` saves pixels underneath before blitting (so `cursor_restore` can undo without redrawing the entire frame). The hotspot offset must be applied in `wm_handle_mouse` so clicks register at the correct position. Keep an embedded fallback arrow as a C byte array for pre-VFS boot. After completing all items,sh clean`, and commit as `"drivers: cursor manager with Adwaita cursors"`.


- [x] Create `include/cursor.h` with `cursor_shape_t` enum (11 shapes)
- [x] Define `cursor_sprite` struct (width, height, hotspot_x, hotspot_y, pixels per size)
- [x] Create `src/kernel/drivers/cursor.c`
- [x] Implement `xcur_load(path)` -- parse X11 cursor binary (Xcur format), extract ARGB+hotspot per size
- [x] Implement `cursor_init()` -- load cursor files from `C:\Impossible\System\Cursors\`, fall back to embedded arrow
- [x] Implement `cursor_set_shape(shape)` -- switch active cursor
- [x] Implement `cursor_get_shape()` -- get current shape
- [x] Implement `cursor_draw(x, y)` -- draw with alpha blending, save pixels underneath
- [x] Implement `cursor_restore()` -- restore saved pixels
- [x] Implement `cursor_get_hotspot(hx, hy)` -- for click position adjustment
- [x] Commit: `"drivers: cursor manager with Adwaita cursors"`

### 5.2 Cursor Assets (Adwaita X11 Cursors)

**Prompt:** Use the Adwaita cursor theme from `/usr/share/icons/Adwaita/cursors/` (LGPL/CC-BY-SA, pre-installed). These are X11 cursor binary files (Xcur format) containing ARGB pixel data, hotspot coordinates, and multiple sizes per file. At build time, copy the 11 needed cursor files to the sysroot at `C:\Impossible\System\Cursors\`. The Xcur format is: 4-byte magic (`Xcur`), 4-byte header size, 4-byte version, 4-byte TOC count, then TOC entries (type, subtype=size, position), then image chunks (header, type=0xFFFD0002, subtype=size, version, width, height, hotspot_x, hotspot_y, delay, ARGB pixels). `xcur_load()` parses this directly at runtime -- no build-time conversion needed. The fallback arrow must be embedded as a `static const uint32_t cursor_fallback[]` byte array. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"resources: Adwaita cursor integration"`.


- [x] Map 11 cursor shapes to Adwaita filenames:
  - [x] `arrow` -> `default` (or `left_ptr`)
  - [x] `hand` -> `pointer` (or `hand2`)
  - [x] `text` -> `text` (or `xterm`)
  - [x] `move` -> `fleur` (or `move`)
  - [x] `resize_ns` -> `sb_v_double_arrow` (or `ns-resize`)
  - [x] `resize_ew` -> `sb_h_double_arrow` (or `ew-resize`)
  - [x] `resize_nwse` -> `bd_double_arrow` (or `nwse-resize`)
  - [x] `resize_nesw` -> `fd_double_arrow` (or `nesw-resize`)
  - [x] `wait` -> `progress` (or `watch`)
  - [x] `crosshair` -> `crosshair` (or `cross`)
  - [x] `forbidden` -> `not-allowed` (or `no-drop`)
- [x] Add Makefile rule: copy 11 Adwaita cursor files to sysroot `Impossible/System/Cursors/`
- [x] Implement `xcur_load()` -- parse Xcur binary, extract ARGB+hotspot per size, convert ARGB->BGRA
- [x] Embed fallback arrow as byte array for pre-VFS boot
- [x] Commit: `"resources: Adwaita cursor integration"`

### 5.3 Context-Based Cursor Switching

**Prompt:** The window manager must determine the correct cursor shape based on what's under the mouse pointer. Add `wm_get_cursor_context(mx, my)` that checks: is the mouse over a window edge or corner (resize cursors), over a title bar during drag (move cursor), over a text input widget (I-beam), over a button or link (hand), or over the desktop (arrow). This function is called every mouse-move event and updates the cursor shape. The compositor loop must save/restore cursor pixels around the composite step to prevent cursor artifacts. Remove the old cursor rendering from `mouse.c` entirely -- mouse.c should only track position and button state. After completing all items,sh clean`, and commit as `"desktop: context-aware cursor switching"`.


- [x] Remove `cursor_data[]` and rendering from `mouse.c` (keep position/button tracking)
- [x] Add `wm_get_cursor_context(mx, my)` in `wm.c`:
  - [x] Desktop/wallpaper -> `CURSOR_ARROW`
  - [x] Start button hover -> `CURSOR_HAND`
  - [x] Menu item hover -> `CURSOR_HAND`
  - [x] Window title bar -> `CURSOR_MOVE` (while dragging)
  - [x] Window edge (N/S) -> `CURSOR_RESIZE_NS`
  - [x] Window edge (E/W) -> `CURSOR_RESIZE_EW`
  - [x] Window corner -> `CURSOR_RESIZE_NWSE` or `CURSOR_RESIZE_NESW`
  - [x] Text input field -> `CURSOR_TEXT`
  - [x] System busy -> `CURSOR_WAIT`
- [x] Update compositor loop: `cursor_restore()` -> composite -> `cursor_set_shape()` -> `cursor_draw()`
- [x] Adjust click position by hotspot offset in `wm_handle_mouse()`
- [x] Commit: `"desktop: context-aware cursor switching"`


---

> **§6–9 (Animations, OpenGL, DPI Scaling, Theme, Context Menus, Notifications,
> Screenshot, Tooltips) have been moved to [TODO-120-Theme.md](TODO-120-Theme.md)
> as part of the master GUI consolidation.**


---

## Priority Order

| Priority | Section                     | Reason                                                   |
|----------|-----------------------------|----------------------------------------------------------|
| ✅ Done   | §1.1 Core Surface           | gfx_surface_t, primitives                               |
| ✅ Done   | §1.2 Alpha Blending          | Pre-multiplied alpha, gfx_blit                           |
| ✅ Done   | §1.3 Gradients               | Linear + radial gradient fills                           |
| ✅ Done   | §1.4 Blur + Material Effects | Mica, Acrylic, drop shadow                               |
| ✅ Done   | §1.5 SIMD Optimization       | SSE2 + AVX2 runtime dispatch                             |
| ✅ Done   | §2.1 stb_truetype            | TrueType font rasterizer                                 |
| ✅ Done   | §2.2 Font Bundle             | Selawik + Cascadia Code fonts                            |
| ✅ Done   | §2.3 Glyph Caching           | ASCII pre-rasterized at 5 sizes                          |
| ✅ Done   | §2.4 Replace Bitmap Font     | TrueType throughout UI                                   |
| ✅ Done   | §3.1 stb_image               | Runtime JPEG/PNG decode, tiered PMM/kmalloc              |
| ✅ Done   | §3.2 Image Scaling           | Bilinear + box-filter, 5 fit modes                       |
| ✅ Done   | §3.3 JPG/PNG Wallpaper       | Decode from VFS, Registry config                         |
| ✅ Done   | §3.4 Image Saving            | BMP/PNG save to VFS                                      |
| ✅ Done   | §4.1 Icon Store              | LRU cache, 128 slots, font + IRES                        |
| ✅ Done   | §4.2 Font Icon Rendering     | Fluent icons, variant selection, codepoint map           |
| ✅ Done   | §4.3 Fluent UI Assets        | 4 Fluent icon font TTFs                                  |
| ✅ Done   | §4.4 File Type Mapping       | `icon_for_extension()`                                   |
| ✅ Done   | §4.5 IRES Format             | Color icon container, host packer tool                   |
| ✅ Done   | §4.6 ICO File Loader         | Windows .ico container parser                            |
| ✅ Done   | §5.1 Cursor Manager          | xcur_load, 11 cursor shapes, save/restore pixels         |
| ✅ Done   | §5.2 Cursor Assets           | Adwaita Xcur files, embedded fallback                    |
| ✅ Done   | §5.3 Context-Aware Cursors   | wm_get_cursor_context() — resize/move/text/hand          |
| 🟠 P1    | §4.7 Win32 Icon Index Map    | Win32 shell32.dll / imageres.dll index → icon_t          |
| 🟢 P3    | §4.3 Color PNGs (deferred)  | Desktop app icon PNGs for IRES                           |

---

## Already Completed (from Parking Lot) ✅

### ~~AVX2 SIMD for blur and alpha blending~~ ✅

> Done (commit `3e0cc53`). `simd_enable_avx()`, `simd_blend_pixels_avx2()`,
> `simd_blur_accum_avx2()`. Runtime dispatch via `simd_avx2_ok` flag.
> **Note:** QEMU default (`qemu64`) lacks AVX2 — use `-cpu Haswell` to test.

---

## OS Comparison

| Feature                          | Windows 11 (DWM/Win32)             | Linux (GTK/KDE/Wayland)              | Impossible OS                              |
|----------------------------------|-------------------------------------|--------------------------------------|--------------------------------------------|
| 2D compositing surface           | ✅ DWM / D2D1                       | ✅ Cairo / Skia                       | ✅ Done — `gfx_surface_t` §1.1            |
| Alpha blending                   | ✅ DWM ARGB compositing             | ✅ Cairo alpha                        | ✅ Done — pre-multiplied §1.2             |
| Blur / Acrylic / Mica effects    | ✅ DWM Blur Behind, Mica            | ✅ KWin blur, GNOME blur (limited)    | ✅ Done — §1.4 `gfx_mica` / `gfx_acrylic`|
| Drop shadows                     | ✅ DWM                              | ✅ KWin / Mutter                      | ✅ Done — §1.4 `gfx_drop_shadow`         |
| SIMD acceleration (SSE2/AVX2)    | ✅ Direct2D uses SSE2               | ✅ pixman SSE2                        | ✅ Done — §1.5 SSE2 + AVX2 dispatch      |
| TrueType fonts                   | ✅ DirectWrite / FreeType           | ✅ FreeType + HarfBuzz                | ✅ Done — §2.1 stb_truetype              |
| Glyph cache                      | ✅ DirectWrite glyph cache          | ✅ FreeType bitmap cache              | ✅ Done — §2.3 ASCII at 5 sizes          |
| Runtime PNG/JPEG decode          | ✅ WIC (Windows Imaging Component)  | ✅ libpng / libjpeg-turbo             | ✅ Done — §3.1 stb_image                 |
| Image scaling (bilinear)         | ✅ WIC scalers                      | ✅ GDK pixbuf / Cairo                 | ✅ Done — §3.2                           |
| System icon store                | ✅ shell32.dll / imageres.dll IRES  | ✅ hicolor icon theme / SVG           | ✅ Done — §4.1–4.5 IRES + Fluent fonts   |
| ICO file support                 | ✅ Native                           | ✅ xicon / Pixbuf loader              | ✅ Done — §4.6                           |
| Cursor themes                    | ✅ .cur / .ani files                | ✅ X11 Xcursor format                 | ✅ Done — §5 Adwaita Xcur                |
| Context-aware cursor shapes      | ✅ LoadCursor + SetCursor           | ✅ gdk_cursor_new_from_name           | ✅ Done — §5.3 wm_get_cursor_context()   |
| Win32 icon index compat          | ✅ shell32.dll indices              | ❌                                   | ⬜ §4.7 P1 — mapping table              |
| **In-kernel gfx (no GPU needed)**| ❌ DWM requires D3D11               | ❌ Mesa/DRM GPU                      | ✅ **Framebuffer CPU rendering — zero GPU dependency** |
| **Mica on boot filesystem**      | ✅ (NTFS drive)                     | ⚠️ Only with btrfs root              | ✅ **Works on IXFS boot drive by design** |
