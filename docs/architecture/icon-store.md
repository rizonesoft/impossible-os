# Icon Store Architecture

## Overview

The icon store is a centralized, hybrid icon system with two rendering backends:

1. **Font-based (monochrome):** ~60 system/toolbar/file type icons rendered
   from Fluent UI icon fonts (TTF) via stb_truetype. Vector glyphs provide
   resolution-independent rendering at any size, tinted with a foreground color.

2. **IRES-based (color):** ~40 desktop/app/folder/file type icons stored as
   pre-rendered BGRA bitmaps in `icons.ires` at multiple sizes (16–256).

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

## Memory Management

- Small bitmaps (≤4 KB): `kmalloc`
- Large bitmaps (>4 KB): `pmm_alloc_contiguous()`
- LRU eviction when cache is full (128 slots max)
- Font TTF data: `kmalloc` (loaded once at boot)

## API

```c
icon_bitmap_t *icon_get(system_icon_t id, uint32_t size);
icon_bitmap_t *icon_get_colored(system_icon_t id, uint32_t size, gfx_color_t color);
system_icon_t  icon_get_by_name(const char *name);
void           icon_draw(gfx_surface_t *s, const icon_bitmap_t *bmp, int32_t x, int32_t y);
void           icon_draw_scaled(gfx_surface_t *s, system_icon_t id, int32_t x, int32_t y, uint32_t target_size);
```

## Files

| File | Purpose |
|------|---------|
| `include/icon_store.h` | Public API, enums, structs |
| `src/kernel/icon_store.c` | Implementation (SSE2 compiled) |
| `docs/architecture/icon-store.md` | This document |
