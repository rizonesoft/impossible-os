# Icon Store Architecture

## Overview

The icon store is a centralized, hybrid icon system with two rendering backends:

1. **Font-based (monochrome):** 60 system/toolbar/file type icons rendered
   from Fluent UI icon fonts (TTF) via stb_truetype. Vector glyphs provide
   resolution-independent rendering at any size, tinted with a foreground color.

2. **IRES-based (color):** 10 desktop/folder/file type icons stored as
   pre-rendered BGRA bitmaps in `icons.ires` at 9 sizes (16–256px).
   Name-based resolution at load time — no hardcoded enum IDs in the IRES file.

## Architecture

```
                    icon_get(id, size)
                          │
                ┌─────────┴──────────┐
                │   Bitmap Cache     │   ← LRU, 128 slots
                │   (id,size,color)  │
                └────┬──────────┬────┘
                     │          │
            ┌────────┘          └────────┐
            ▼ cache miss                 ▼ cache miss
   ┌─────────────────┐        ┌─────────────────┐
   │  Font Rasterizer │       │   IRES Loader    │
   │ (stb_truetype)   │       │  (BGRA bitmaps)  │
   │  id < MONO_COUNT │       │ id >= MONO_COUNT  │
   └─────────────────┘        └─────────────────┘
            │                          │
   ┌────────┴─────────┐      ┌────────┴────────┐
   │ FluentIcons-*.ttf│      │  icons.ires     │
   │  (4 variants)    │      │ (color PNGs)    │
   └──────────────────┘      └─────────────────┘
```

## Font Variants

| Variant    | File                              | Use Case                    |
|-----------|------------------------------------|-----------------------------|
| Filled    | `FluentSystemIcons-Filled.ttf`     | Toolbars, active states     |
| Regular   | `FluentSystemIcons-Regular.ttf`    | Menus, secondary items      |
| Light     | `FluentSystemIcons-Light.ttf`      | Disabled states, hints      |
| Resizable | `FluentSystemIcons-Resizable.ttf`  | Small sizes (≤16px)         |

## IRES Format (Color Icons)

Binary format: 16-byte header → size table → index → name table → BGRA pixel data.

- **Name-based resolution:** `irespack` writes `icon_id=0xFFFF`; the kernel resolves
  icon names from the IRES name table against `icon_names[]` at boot. This decouples
  the build tool from the kernel's enum numbering.
- **Sizes:** 16, 24, 32, 48, 64, 72, 96, 128, 256
- **Closest-size matching:** `ires_get_bitmap()` finds the nearest available size.
- **PMM allocation:** entire IRES file loaded into PMM (can be several MB).

### Color Icons (10)

| Icon Name          | Enum                   | Description       |
|-------------------|------------------------|-------------------|
| `folder_closed`   | `ICON_FOLDER_CLOSED`   | Closed folder     |
| `folder_open`     | `ICON_FOLDER_OPEN`     | Open folder       |
| `file_default`    | `ICON_FILE_DEFAULT`    | Unknown file type |
| `exe_default`     | `ICON_EXE_DEFAULT`     | Executable        |
| `dll_default`     | `ICON_DLL_DEFAULT`     | DLL/library       |
| `text_file`       | `ICON_TEXT_FILE`       | Text file         |
| `computer`        | `ICON_DESKTOP_COMPUTER`| Desktop computer  |
| `recycle_bin_empty`| `ICON_RECYCLE_BIN_EMPTY`| Empty bin        |
| `recycle_bin_full` | `ICON_RECYCLE_BIN_FULL` | Full bin         |
| `control_deck`    | `ICON_CONTROL_DECK`    | Control panel     |

## File Type Mapping

`icon_for_extension(ext)` maps file extensions to color icons:

| Extensions           | Icon              |
|---------------------|-------------------|
| `.exe`              | `ICON_EXE_DEFAULT`|
| `.dll`, `.sys`      | `ICON_DLL_DEFAULT`|
| `.txt`, `.md`, `.log`, `.cfg`, `.ini` | `ICON_TEXT_FILE` |
| everything else     | `ICON_FILE_DEFAULT`|

## Memory Management

- Small bitmaps (≤4 KB): `kmalloc`
- Large bitmaps (>4 KB): `pmm_alloc_contiguous()`
- IRES file data: `pmm_alloc_contiguous()` (loaded once, pixel pointers reference this buffer)
- LRU eviction when cache is full (128 slots max)
- Font TTF data: `kmalloc` (loaded once at boot)

## API

```c
icon_bitmap_t *icon_get(system_icon_t id, uint32_t size);
icon_bitmap_t *icon_get_colored(system_icon_t id, uint32_t size, gfx_color_t color);
system_icon_t  icon_get_by_name(const char *name);
system_icon_t  icon_for_extension(const char *ext);
void           icon_draw(gfx_surface_t *s, const icon_bitmap_t *bmp, int32_t x, int32_t y);
void           icon_draw_scaled(gfx_surface_t *s, system_icon_t id, int32_t x, int32_t y, uint32_t target_size);
```

## Files

| File | Purpose |
|------|---------|
| `include/icon_store.h` | Public API, enums, structs |
| `src/kernel/icon_store.c` | Implementation (SSE2 compiled) |
| `tools/irespack.c` | Host-side IRES packer (PNG → .ires) |
| `resources/icons/color/{size}/*.png` | Source PNGs for color icons |
| `docs/architecture/icon-store.md` | This document |
