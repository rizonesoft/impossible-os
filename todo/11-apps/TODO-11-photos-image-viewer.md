---
schema_version: 1
id: photos-image-viewer
domain: 11-apps
status: active
title: "TODO-11 -- Photos (Image Viewer)"
---

# TODO-11 -- Photos (Image Viewer)

> **Goal:** Build `photos.exe` -- the system image viewer for JPEG, PNG, BMP, GIF, and WEBP.
> `stb_image` and `gfx_blit()` are already complete; this TODO wires them into a full
> Photos-style app with folder navigation, thumbnail strip, slideshow, EXIF panel, and
> non-destructive rotate/crop/auto-enhance editing.

> [!IMPORTANT]
> **Source migration:** §2.1 (Image Viewer) from
> `todo-old/310-Core-Apps/TODO-370-Utility-Apps.md` is migrated here.
> **Do not delete that file** -- remaining sections are migrated in TODO-12 and TODO-13.
>
> `image_load()`, `image_scale()` (`IMAGE_FIT_FIT`, `IMAGE_FIT_FILL`, `IMAGE_FIT_CENTER`),
> `image_free()`, `image_save_bmp()`, `image_save_png()` are all present in
> `include/kernel/image.h` and are complete -- use them directly.
>
> **Set as Wallpaper** writes to `HKCU\Control Panel\Desktop\Wallpaper` via
> `reg_set_string()` then signals the desktop shell to reload via `desktop_draw_wallpaper()`.
>
> **EXIF parsing** (§6) is novel infrastructure with no prior Impossible OS precedent; treat
> it as a minimal inline reader rather than a general-purpose TIFF library.

---

## Inputs

- `include/kernel/image.h` -- `image_load`, `image_load_mem`, `image_scale`, `image_save_bmp`, `image_save_png`, `image_free`, `image_fit_t` (IMAGE_FIT_FIT / IMAGE_FIT_CENTER / IMAGE_FIT_FILL) -- §1 §2 §7
- `include/gfx.h` -- `gfx_blit()`, `gfx_fill_rect()`, `gfx_scale_blit()`, `gfx_surface_create()` -- §1 §2 §3 §5
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_mark_dirty()`, `wm_set_fullscreen()` -- §1 §4
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_SCROLLBAR_HORIZ`, `CTRL_STATUSBAR`, `CTRL_LISTVIEW` -- §3 §4
- `include/kernel/fs/vfs.h` -- `vfs_open`, `vfs_readdir`, `vfs_stat`, `vfs_get_name` -- §3 folder scan
- `include/registry.h` -- `reg_get_string`, `reg_set_string` -- §4 Set as Wallpaper, §5 interval, §8 recent files
- `include/desktop/desktop.h` -- `desktop_draw_wallpaper()` -- §4 wallpaper reload after set
- `include/desktop/file_assoc.h` (→ XREF `09-desktop-shell/TODO-02 §1`) -- `file_assoc_set()` -- §8
- `08-graphics-ui/TODO-06-widget-dialogs.md §2` -- `dialog_file_open()` -- §1 open dialog
- `include/kernel/timer.h` -- `system_get_ticks()` -- §2 zoom debounce, §5 slideshow timer
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()` -- §6 EXIF IFD buffer, §7 scratch pixels

---

## Outcome

`photos.exe` opens any supported image format, fits it to the window, and allows zoom, pan,
and folder navigation with keyboard shortcuts. A thumbnail strip shows all images in the
folder. Slideshow with crossfade auto-advances. An EXIF side panel shows camera metadata.
Basic non-destructive editing (rotate, crop, auto-enhance) saves to original or copy.
All major image formats are registered to `photos.exe`.

---

## Implementation Order

| Step | Section                                  | 💎/⭐ | Dependency                                             |
| ---- | ---------------------------------------- | ----- | ------------------------------------------------------ |
| 1    | Image Loading + Display                  | 💎    | `image_load`, `image_scale(IMAGE_FIT_FIT)`, `gfx_blit` |
| 2    | Zoom + Pan                               | 💎    | §1 display pipeline                                    |
| 3    | Folder Navigation + Thumbnail Strip      | 💎    | `vfs_readdir`, §1 `image_scale` thumbnails             |
| 4    | Toolbar + Menus                          | 💎    | §1–§3 stable                                           |
| 5    | Slideshow                                | 💎    | §3 folder list, §1 display                             |
| 6    | EXIF Metadata Panel                      | ⭐    | §1 JPEG load, inline TIFF/IFD reader                   |
| 7    | Basic Editing (Rotate/Crop/Auto-enhance) | 💎    | §1 `image_save_png`, pixel-level ops                   |
| 8    | File Associations + App Registration     | 💎    | `file_assoc_set`, §1 CLI arg                           |

---

## 1. Image Loading + Display `[Sonnet]`

**Source file:** `src/apps/photos/photos.c`; header `include/apps/photos/photos.h`

- [ ] `wm_create_window("Photos", 900, 650, WM_RESIZABLE)` -- main window
- [ ] On open: `image_load(&g_img, path)` → `image_scale(&g_display, &g_img, canvas_w, canvas_h, IMAGE_FIT_FIT)` → `gfx_blit(surface, 0, 0, display.pixels, display.width, display.height)`
- [ ] Neutral background: `gfx_fill_rect(surface, 0, 0, canvas_w, canvas_h, 0xFF1E1E1E)` (dark gray) before blit -- letterbox/pillarbox bands are always visible
- [ ] Center the scaled image: compute x offset = `(canvas_w - display.w) / 2`, y offset = `(canvas_h - display.h) / 2`
- [ ] On window resize (`WM_RESIZE`): re-run `image_scale` if fit-to-window mode is active; skip if zoom is manually set
- [ ] Status bar (bottom 24 px): `"{filename}  {w} × {h} px  {file_size_kb} KB"`
- [ ] Window title: `"{filename} -- Photos"`
- [ ] Support CLI arg: `photos.exe C:\path\image.jpg` → open immediately; no arg → `dialog_file_open("Images|*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.webp;*.tif;*.tiff|All Files|*.*")`

---

## 2. Zoom + Pan `[Sonnet]`

- [ ] **Zoom state**: `float g_zoom` (1.0 = 100 %); `bool g_fit_mode` (default true); range clamp 0.05–32.0
- [ ] **Mouse wheel zoom**: each notch × 1.1 (in) or ÷ 1.1 (out); pivot around cursor position (adjust pan offset so cursor pixel stays fixed); exit fit-mode on first zoom
- [ ] **Double-click**: toggle between `g_fit_mode = true` (fit-to-window) and `g_zoom = 1.0` (100 %)
- [ ] **Ctrl+`+` / Ctrl+`-`**: zoom in/out by 10 % steps; **Ctrl+0**: reset to fit-to-window
- [ ] **Click+drag pan**: only when `g_zoom > fit_zoom`; accumulate `g_pan_x`, `g_pan_y` offsets; clamp so image edge cannot be dragged past center of canvas
- [ ] **Render pipeline for zoom**: `image_scale(&g_display, &g_img, scaled_w, scaled_h, IMAGE_FIT_STRETCH)` where `scaled_w = g_img.width * g_zoom`; then blit at `(canvas_cx - scaled_w/2 + g_pan_x, ...)`
- [ ] **Status bar zoom %**: update to `"{filename}  {w}×{h}  {zoom:.0f}%"` whenever zoom changes
- [ ] **Horizontal scrollbar** (`CTRL_SCROLLBAR_HORIZ`): visible only when `scaled_w > canvas_w`; dragging updates `g_pan_x`

---

## 3. Folder Navigation + Thumbnail Strip `[Sonnet]`

- [ ] **Folder scan** on open: extract directory from `filepath`; `vfs_readdir()` all entries; filter to `{.jpg,.jpeg,.png,.bmp,.gif,.webp,.tif,.tiff}`; sort alphabetically (case-insensitive `strcmp`); store paths in `g_folder_files[512]`, `g_folder_count`, `g_folder_idx` = index of current file
- [ ] **Left/Right arrow keys**: decrement/increment `g_folder_idx` (wrap); load new image via `image_load`; reset zoom to fit-mode; update title + status bar
- [ ] **Toolbar ◀ / ▶ buttons** (§4): same as arrow keys
- [ ] **Thumbnail strip** (80 px height, bottom of canvas above status bar):
  - [ ] Default hidden; `T` key toggles; View→Thumbnail Strip menu item
  - [ ] Lazily generate 80×60 px thumbnails via `image_scale(&thumb, &img, 80, 60, IMAGE_FIT_FIT)` on first display; cache to `g_thumbs[]` (up to 64 cached; LRU evict oldest with `image_free`)
  - [ ] Horizontal scroll if `folder_count × 88 > canvas_w`; selected thumbnail outlined with accent-color 2 px border
  - [ ] Click thumbnail → navigate to that image

---

## 4. Toolbar + Menus `[Sonnet]`

**Source file:** `src/apps/photos/photos_ui.c`

- [ ] **Toolbar** (40 px top strip):
  - [ ] `[◀]` Previous, `[▶]` Next (§3 folder nav)
  - [ ] `[🔍+]` Zoom In, `[🔍−]` Zoom Out, `[⊡]` Fit to Window (sets `g_fit_mode = true`)
  - [ ] `[↺]` Rotate Left (−90°), `[↻]` Rotate Right (+90°) → §7 rotate
  - [ ] `[🗑]` Delete: `vfs_delete(filepath)`; advance to next image
  - [ ] `[💾]` Save / `[📋]` Copy to Clipboard (`clipboard_set(CLIP_IMAGE, &g_img)`)
- [ ] **Menu bar**: `File  Edit  View  Help`
  - [ ] `File`: Open, Save, Save As (PNG/BMP format selector), Set as Wallpaper, Properties (→ §6 EXIF panel toggle), Recent Files (10-entry), Exit
  - [ ] `Edit`: Copy, Paste (load clipboard image), Rotate Left/Right, Crop (→ §7), Auto-enhance (→ §7), Undo
  - [ ] `View`: Zoom In/Out/100%/Fit, Thumbnail Strip (toggle), EXIF Panel (toggle), Slideshow, Fullscreen (F11 → `wm_set_fullscreen`)
- [ ] **Set as Wallpaper**:
  - [ ] `reg_set_string("HKCU\\Control Panel\\Desktop\\Wallpaper", filepath)`
  - [ ] `reg_set_string("HKCU\\Control Panel\\Desktop\\WallpaperStyle", "2")` (2 = stretch; 6 = fit; 10 = fill)
  - [ ] Call `desktop_draw_wallpaper()` to reload immediately
  - [ ] Toast: `"Wallpaper updated"`

---

## 5. Slideshow `[Sonnet]`

- [ ] **Start slideshow**: View→Slideshow → `wm_set_fullscreen(true)`; hide toolbar; show translucent OSD overlay (current filename + `[⏯]`/`[⏹]`/`[⏮]`/`[⏭]`); auto-hide OSD after 3 s
- [ ] **Advance timer**: `system_get_ticks()` delta; configurable interval N seconds (default 5, stored in `HKCU\Software\Impossible\Photos\SlideshowInterval`); advance `g_folder_idx` on tick
- [ ] **Crossfade transition** (300 ms):
  - [ ] Keep previous `gfx_surface_t *prev_surf`; load next image into `gfx_surface_t *next_surf`
  - [ ] Each frame: alpha = `elapsed_ms / 300.0`; composite: `pixel = prev*(1-alpha) + next*alpha` for each pixel in canvas -- iterate `uint32_t *` pixels; extract BGRA channels; blend; write back
  - [ ] On transition complete: free `prev_surf`; swap `next_surf` → `prev_surf`
- [ ] **Controls during slideshow**: Escape exits to windowed; Space pauses/resumes; Left/Right manual advance; mouse move → show OSD for 3 s
- [ ] **Slideshow interval dialog**: Settings → `dialog_input("Interval (seconds):", "5")` → parse int → clamp 1–60 → `reg_set_string`

---

## 6. EXIF Metadata Panel `[Opus]`

> Novel TIFF/IFD inline reader -- no prior Impossible OS precedent for EXIF parsing.

**Source file:** `src/apps/photos/exif.c`; header `include/apps/photos/exif.h`

- [ ] **EXIF location in JPEG**: scan JPEG markers until `0xFFE1` (APP1); verify `"Exif\0\0"` magic (6 bytes); remaining bytes = embedded TIFF blob; allocate to `pmm_alloc_contiguous` buffer
- [ ] **TIFF/IFD reader** (`exif_parse(buf, len, exif_t *out)`):
  - [ ] Byte order: `"II"` = little-endian, `"MM"` = big-endian; apply throughout
  - [ ] IFD0 offset at `buf+4` (uint32); walk IFD entries (2-byte count, then 12-byte entries: tag(2), type(2), count(4), value/offset(4))
  - [ ] Extract tags: `0x010F` Make, `0x0110` Model, `0x0132` DateTime, `0x8769` ExifIFD offset
  - [ ] Follow `ExifIFD` sub-IFD; extract: `0x829A` ExposureTime (rational), `0x829D` FNumber (rational), `0x8827` ISOSpeedRatings, `0x920A` FocalLength (rational), `0x9003` DateTimeOriginal
  - [ ] Follow `0x8825` GPSInfoIFD if present; extract `0x0002`/`0x0004` GPSLatitude/Longitude (rational array × 3)
  - [ ] Rational: `uint32 numerator / uint32 denominator`; format as `"{num}/{den}"` or convert to float for display
- [ ] **Side panel UI** (240 px right strip, toggled by `View→EXIF Panel` or `P` key):
  - [ ] `[×]` close button; label+value rows (bold label, plain value); `CTRL_SCROLLBAR_VERT` if overflow
  - [ ] Rows: Make, Model, Date Taken, Exposure, F-number (`f/{val:.1f}`), ISO, Focal Length (`{val} mm`), GPS (decimal degrees or "Not available")
  - [ ] Non-JPEG or missing APP1 marker: show `"No EXIF data available"`
  - [ ] PNG/BMP: show only dimensions + file size from `image_t` metadata

---

## 7. Basic Editing `[Opus]`

> Pixel-level rotate, crop rubber-band, and histogram stretch are novel image-processing
> routines with no prior Impossible OS precedent.

**Source file:** `src/apps/photos/photos_edit.c`

- [ ] **Rotate 90°** (`photos_rotate_cw(image_t *src, image_t *dst)`):
  - [ ] Allocate `dst` pixels: `dst.w = src.h`, `dst.h = src.w`
  - [ ] For each `(x, y)` in src: `dst.pixels[x * dst.w + (dst.w - 1 - y)] = src.pixels[y * src.w + x]` (CW 90°)
  - [ ] CCW: `dst.pixels[(dst.h - 1 - x) * dst.w + y] = src.pixels[y * src.w + x]`
  - [ ] For JPEG files: attempt to update EXIF `0x0112` Orientation tag in-place before saving (avoids full re-encode); for all other formats and for mismatched EXIF: pixel-level rotate always
  - [ ] Replace `g_img` with rotated result; reset zoom to fit-mode; mark `g_modified = true`
- [ ] **Rotate 180°**: apply CW 90° twice (or direct mapping `dst[y*w+x] = src[(h-1-y)*w+(w-1-x)]`)
- [ ] **Crop via rubber-band**:
  - [ ] Enter crop mode (`Edit→Crop` or `C` key): overlay grid lines (rule of thirds); cursor becomes crosshair
  - [ ] Mouse drag draws a `gfx_draw_rect` dashed rectangle over the image; handles at corners + edges for resize
  - [ ] On Confirm (`Enter`): compute pixel crop rect in image coordinates (inverse of zoom/pan transform); `image_t cropped` allocated; `memcpy` rows from `g_img.pixels + y*w + x` for `crop_h` rows; replace `g_img`; mark `g_modified = true`
  - [ ] Escape → exit crop mode without applying
- [ ] **Auto-enhance** (`photos_auto_enhance(image_t *img)`):
  - [ ] Compute per-channel (R, G, B) histogram: 256-bucket `uint32_t hist[3][256]`
  - [ ] Find 1st-percentile and 99th-percentile input levels per channel (accumulate bucket counts; find threshold positions)
  - [ ] Apply linear stretch: `out = clamp((in - lo) * 255 / (hi - lo), 0, 255)` for each channel of each pixel
  - [ ] Mark `g_modified = true`; refresh display
- [ ] **Undo for edits**: single-level undo (keep one `image_t g_undo_img` copy before any edit; `Ctrl+Z` swaps `g_img` ↔ `g_undo_img`); multi-edit undo is stretch
- [ ] **Save / Save Copy**:
  - [ ] `File→Save`: if `g_modified && (ext == ".png" || ext == ".bmp")` → `image_save_png` or `image_save_bmp`; if `.jpg`/`.jpeg`: save as PNG with save-changes dialog warning ("JPEG does not preserve edits losslessly -- save as PNG?"); `g_modified = false`
  - [ ] `File→Save Copy`: `dialog_file_save("PNG Image|*.png|BMP Image|*.bmp")` → save to new path without changing `filepath`

---

## 8. File Associations + App Registration `[Sonnet]`

- [ ] Register file associations via `file_assoc_set()`:
  - `".jpg"`, `".jpeg"` → `photos.exe`
  - `".png"` → `photos.exe`
  - `".bmp"` → `photos.exe`
  - `".gif"` → `photos.exe`
  - `".webp"` → `photos.exe`
  - `".tif"`, `".tiff"` → `photos.exe` (load via `image_load`; TGA/TIFF decoded by stb_image)
- [ ] **"Open with Photos" context menu verb** registered for all above types
- [ ] **Recent files**: `HKCU\Software\Impossible\Photos\RecentFiles\{0..9}` (10 MRU); shown in `File→Recent`
- [ ] **Properties dialog** (`File→Properties`): reuse the EXIF panel content (§6) in a `MessageBox`-style modal: dimensions, file size, format, EXIF table

---

## OS Comparison


| ⭐  | Feature                                           | 🪟 Win11                            | 🐧 Linux                              | 🚀 Impossible OS                                              |
| --- | ------------------------------------------------- | ----------------------------------- | ------------------------------------- | ------------------------------------------------------------- |
| 💎  | JPEG/PNG/BMP/GIF image display                    | ✅ Photos (stb_image-like decoders) | ✅ eog / gwenview                     | ✅ §1 -- Done -- `image_load` + `gfx_blit`                    |
| 💎  | Fit-to-window with aspect-ratio letterbox         | ✅ Photos (auto-fit on open)        | ✅ eog / feh                          | ⬜ §1 -- `IMAGE_FIT_FIT` + centered blit on                   |
| 💎  | Smooth mouse-wheel zoom with cursor pivot         | ✅ Photos                           | ✅ gwenview                           | ⬜ §2 -- pivot math keeps cursor pixel                        |
| 💎  | Folder navigation                                 | ✅ Photos                           | ✅ eog                                | ⬜ §3 -- `vfs_readdir` alphabetic sort, wrap-around           |
| 💎  | Thumbnail strip                                   | ✅ Photos (film-strip view)         | ✅ gwenview                           | ⬜ §3 -- 80×60 lazy-cached thumbs, LRU eviction               |
| 💎  | Set image as desktop wallpaper                    | ✅ Photos (right-click)             | ✅ feh / Nautilus                     | ⬜ §4 -- `reg_set_string` + `desktop_draw_wallpaper()` reload |
| 💎  | Slideshow with crossfade                          | ✅ Photos (slideshow view)          | ✅ eog / feh                          | ⬜ §5 -- `system_get_ticks` timer, alpha-blend crossfade      |
| ⭐  | EXIF metadata panel                               | ✅ Photos (Properties)              | ⚠️ eog (basic EXIF); gwenview (full)  | ⬜ §6 -- inline TIFF/IFD reader, GPS decimal                  |
| 💎  | Rotate 90°/180°/270° with EXIF orientation update | ✅ Photos                           | ✅ eog                                | ⬜ §7 -- pixel-level CW/CCW rotate + EXIF                     |
| ⭐  | Rubber-band crop with rule-of-thirds overlay      | ✅ Photos (crop tool)               | ✅ gwenview                           | ⬜ §7 -- drag handles, inverse-zoom transform, in-place       |
| ⭐  | Auto-enhance via per-channel histogram stretch    | ✅ Photos (auto-enhance)            | ⚠️ eog (no auto-enhance); GIMP (full) | ⬜ §7 -- 1%/99% percentile clamp, linear RGB                  |

Impossible OS Photos delivers the same zero-dependency image pipeline (one `image_load` call covers JPEG/PNG/BMP/GIF/WEBP/TGA) with a richer EXIF side panel and rule-of-thirds crop overlay -- features absent from basic Linux viewers -- without pulling in any external imaging framework.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **Load + display**: `photos.exe C:\Impossible\Web\Wallpaper\default.jpg` → image centered on dark background; status bar shows filename, dimensions, and file size
- [ ] **Fit mode**: portrait image on landscape canvas → vertical black bars visible left/right; landscape image on portrait canvas → horizontal bars
- [ ] **Zoom**: mouse-wheel up → image enlarges; cursor pixel stays in place; status bar `%` updates; double-click → toggles 100% ↔ fit; Ctrl+0 → returns to fit
- [ ] **Pan**: zoom to 200%; click+drag → image pans; drag past edge → clamps; scrollbar appears; drag scrollbar → pans identically
- [ ] **Folder nav**: open image from folder with 5+ images; Left/Right arrows cycle all images; thumbnail strip toggle `T` shows 80×60 thumbs; clicking thumb navigates
- [ ] **Wallpaper**: File→Set as Wallpaper → desktop wallpaper updates immediately; `HKCU\Control Panel\Desktop\Wallpaper` registry value matches filepath
- [ ] **Slideshow**: View→Slideshow → fullscreen; auto-advances every 5 s; crossfade visible; Space pauses; Escape exits to windowed
- [ ] **EXIF panel**: open JPEG from a camera (with EXIF) → `P` → panel shows Make, Model, DateTime, ISO, FNumber; open PNG → shows "No EXIF data available"
- [ ] **Rotate**: `[↻]` button → image rotates 90° clockwise; title shows `*`; Save overwrites; reload confirms rotation preserved
- [ ] **Crop**: `Edit→Crop` → drag rubber band → Enter confirms; image cropped to selection; Ctrl+Z restores original
- [ ] **Auto-enhance**: open dark/underexposed photo → `Edit→Auto-enhance` → contrast/brightness visually improved; Ctrl+Z reverts
- [ ] **File assoc**: `.jpg` double-click in File Manager → `photos.exe` opens file
- [ ] Commit: `"apps: Photos -- image viewer, zoom/pan, folder nav, thumbnail strip, EXIF, edit"`
