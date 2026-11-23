---
schema_version: 1
id: photos-image-viewer
domain: 11-apps
status: active
title: "TODO-11 -- Photos (Image Viewer)"
---

# TODO-11 -- Photos (Image Viewer)

> **Goal:** Build `photos.exe` -- the system image viewer for JPEG, PNG, BMP, GIF and TGA (WEBP and TIFF need a decoder first; see §1).
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
> `RegSetString()` then calls `wallpaper_set()` from `08-graphics-ui/TODO-09` §3 (planned). Today the desktop reads
> `HKLM\SYSTEM\Theme\Wallpaper` once at start-up and `desktop_draw_wallpaper()` only redraws the loaded image.
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
- `include/registry.h` -- `RegGetString`, `RegSetString` -- §4 Set as Wallpaper, §5 interval, §8 recent files
- `08-graphics-ui/TODO-09-desktop-shell-features.md §3` -- `wallpaper_set(path, mode)` (planned) -- §4 wallpaper reload after set
- `include/desktop/file_assoc.h` (→ XREF `09-desktop-shell/TODO-02 §1`) -- `file_assoc_set()` -- §8
- `08-graphics-ui/TODO-06-widget-dialogs.md §8` -- `dialog_file_open()` -- §1 open dialog
- `include/kernel/timer.h` -- `system_get_ticks()` -- §2 zoom debounce, §5 slideshow timer
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()` -- §6 EXIF IFD buffer, §7 scratch pixels

---

## Outcome

`photos.exe` opens any supported image format, fits it to the window, and allows zoom, pan, and folder navigation with keyboard shortcuts. A thumbnail strip shows all images in the folder. Slideshow with crossfade auto-advances. An EXIF side panel shows camera metadata. Basic non-destructive editing (rotate, crop, auto-enhance) saves to original or copy. All major image formats are registered to `photos.exe`.

---

## Implementation Order

| Step | Section                                  | 💎/⭐ | Dependency                                                                             | Status |
| ---- | ---------------------------------------- | ----- | -------------------------------------------------------------------------------------- | :----: |
| 1    | Image Loading + Display                  | 💎    | §10                                                                                    |  [ ]   |
| 2    | Zoom + Pan                               | 💎    | §10, §1                                                                                |  [ ]   |
| 3    | Folder Navigation + Thumbnail Strip      | 💎    | §10, §1                                                                                |  [ ]   |
| 4    | Toolbar + Menus                          | 💎    | §10, §1, §2, §3                                                                        |  [ ]   |
| 5    | Slideshow                                | 💎    | §10, §3, §1                                                                            |  [ ]   |
| 6    | EXIF Metadata Panel                      | ⭐    | §10, §1                                                                                |  [ ]   |
| 7    | Basic Editing (Rotate/Crop/Auto-enhance) | 💎    | §10, §1                                                                                |  [ ]   |
| 8    | File Associations + App Registration     | 💎    | §10, §1                                                                                |  [ ]   |
| 9    | Image Viewer                             | 💎    | §10                                                                                    |  [ ]   |
| 10   | Import Photos                            | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §3 |  [ ]   |

---

## 1. Image Loading + Display

**Moved:** photos

Built in Photos, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] WebP and TIFF have no decoder: stb_image is built for JPEG, PNG, BMP, GIF and TGA only (`src/kernel/image.c`); pick a licence-checked decoder or drop `.webp`/`.tif` from §1, §3 and §8
- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: photos brings Image Loading + Display"`

---

## 2. Zoom + Pan

**Moved:** photos

Built in Photos, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: photos brings Zoom + Pan"`

---

## 3. Folder Navigation + Thumbnail Strip

**Moved:** photos

Built in Photos, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: photos brings Folder Navigation + Thumbnail Strip"`

---

## 4. Toolbar + Menus

**Design:** [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout), [`controls.md#button`](../../docs/design/controls.md#button), [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

**Moved:** photos

Built in Photos, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: photos brings Toolbar + Menus"`

---

## 5. Slideshow

**Moved:** photos

Built in Photos, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: photos brings Slideshow"`

---

## 6. EXIF Metadata Panel

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** photos

Built in Photos, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: photos brings EXIF Metadata Panel"`

---

## 7. Basic Editing

**Moved:** photos

Built in Photos, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: photos brings Basic Editing"`

---

## 8. File Associations + App Registration

**Moved:** photos

Built in Photos, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] Register file associations via `file_assoc_set()`:
  - `".jpg"`, `".jpeg"` → `photos.exe`
  - `".png"` → `photos.exe`
  - `".bmp"` → `photos.exe`
  - `".gif"` → `photos.exe`
  - `".webp"` → `photos.exe`
  - `".tif"`, `".tiff"` → `photos.exe` (needs the TIFF decoder from §1; stb_image decodes TGA but not TIFF)
- [ ] **"Open with Photos" context menu verb** registered for all above types
- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: photos brings File Associations + App Registration"`

## 9. Image Viewer

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-12-utilities.md` §3** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** photos

Built in Photos, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: photos brings Image Viewer"`

## 10. Import Photos

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** photos

Photos is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull photos` into `src/apps/photos/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Photos on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose photos`
- [ ] Commit: `"apps: Photos imported"`

---

## OS Comparison


| ⭐  | Feature                                           | 🪟 Win11                            | 🐧 Linux                              | 🚀 Impossible OS                                          |
| --- | ------------------------------------------------- | ----------------------------------- | ------------------------------------- | --------------------------------------------------------- |
| 💎  | JPEG/PNG/BMP/GIF image display                    | ✅ Photos (stb_image-like decoders) | ✅ eog / gwenview                     | ⬜ §1 -- `image_load` + `gfx_blit` shipped; no viewer yet |
| 💎  | Fit-to-window with aspect-ratio letterbox         | ✅ Photos (auto-fit on open)        | ✅ eog / feh                          | ⬜ §1 -- `IMAGE_FIT_FIT` + centered blit on               |
| 💎  | Smooth mouse-wheel zoom with cursor pivot         | ✅ Photos                           | ✅ gwenview                           | ⬜ §2 -- pivot math keeps cursor pixel                    |
| 💎  | Folder navigation                                 | ✅ Photos                           | ✅ eog                                | ⬜ §3 -- `vfs_readdir` alphabetic sort, wrap-around       |
| 💎  | Thumbnail strip                                   | ✅ Photos (film-strip view)         | ✅ gwenview                           | ⬜ §3 -- 80×60 lazy-cached thumbs, LRU eviction           |
| 💎  | Set image as desktop wallpaper                    | ✅ Photos (right-click)             | ✅ feh / Nautilus                     | ⬜ §4 -- `RegSetString` + `wallpaper_set()` reload        |
| 💎  | Slideshow with crossfade                          | ✅ Photos (slideshow view)          | ✅ eog / feh                          | ⬜ §5 -- `system_get_ticks` timer, alpha-blend crossfade  |
| ⭐  | EXIF metadata panel                               | ✅ Photos (Properties)              | ⚠️ eog (basic EXIF); gwenview (full)  | ⬜ §6 -- inline TIFF/IFD reader, GPS decimal              |
| 💎  | Rotate 90°/180°/270° with EXIF orientation update | ✅ Photos                           | ✅ eog                                | ⬜ §7 -- pixel-level CW/CCW rotate + EXIF                 |
| ⭐  | Rubber-band crop with rule-of-thirds overlay      | ✅ Photos (crop tool)               | ✅ gwenview                           | ⬜ §7 -- drag handles, inverse-zoom transform, in-place   |
| ⭐  | Auto-enhance via per-channel histogram stretch    | ✅ Photos (auto-enhance)            | ⚠️ eog (no auto-enhance); GIMP (full) | ⬜ §7 -- 1%/99% percentile clamp, linear RGB              |

Impossible OS Photos delivers the same zero-dependency image pipeline (one `image_load` call covers JPEG/PNG/BMP/GIF/TGA) with a richer EXIF side panel and rule-of-thirds crop overlay -- features absent from basic Linux viewers -- without pulling in any external imaging framework.

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
