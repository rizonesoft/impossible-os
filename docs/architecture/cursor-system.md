# Cursor System

The cursor manager replaces the hardcoded 12×19 arrow in `mouse.c` with Adwaita X11 cursors loaded at boot.

## Architecture

```
resources/cursors/     Build host: 11 Adwaita Xcur binaries
        |                Makefile copies to sysroot →
        v
C:\Impossible\System\Cursors\   Runtime: VFS reads at cursor_init()
        |
        v
cursor.c: xcur_parse()    Parses Xcur binary → cursor_image_t
        |
        v
cursor_sprite_t cursors[11]  In-memory: BGRA pixel data + hotspot per shape
        |
        v
cursor_draw(x,y)          Alpha-blended blit with save/restore
```

## Xcur Format

| Offset | Size | Description |
|--------|------|-------------|
| 0      | 4    | Magic: `Xcur` |
| 4      | 4    | Header size |
| 8      | 4    | Version |
| 12     | 4    | TOC entry count |
| 16+    | 12×N | TOC entries (type, subtype=size, position) |

Image chunks (type `0xFFFD0002`):

| Offset | Size | Description |
|--------|------|-------------|
| 0      | 4    | Chunk header size (36) |
| 4      | 4    | Type: `0xFFFD0002` |
| 8      | 4    | Subtype (= pixel size) |
| 12     | 4    | Version |
| 16     | 4    | Width |
| 20     | 4    | Height |
| 24     | 4    | Hotspot X |
| 28     | 4    | Hotspot Y |
| 32     | 4    | Delay (ms, for animated cursors) |
| 36     | W×H×4| ARGB pixel data (little-endian) |

## Shape → Adwaita Filename Map

| Shape | Enum | Adwaita File |
|-------|------|-------------|
| Arrow | `CURSOR_ARROW` | `default` |
| Hand | `CURSOR_HAND` | `pointer` |
| I-beam | `CURSOR_TEXT` | `text` |
| Move | `CURSOR_MOVE` | `fleur` |
| Resize N/S | `CURSOR_RESIZE_NS` | `sb_v_double_arrow` |
| Resize E/W | `CURSOR_RESIZE_EW` | `sb_h_double_arrow` |
| Resize NW/SE | `CURSOR_RESIZE_NWSE` | `bd_double_arrow` |
| Resize NE/SW | `CURSOR_RESIZE_NESW` | `fd_double_arrow` |
| Wait | `CURSOR_WAIT` | `progress` |
| Crosshair | `CURSOR_CROSSHAIR` | `crosshair` |
| Forbidden | `CURSOR_FORBIDDEN` | `not-allowed` |

## API

```c
void cursor_init(void);                        // Load all cursors from VFS
void cursor_set_shape(cursor_shape_t shape);   // Switch active shape
cursor_shape_t cursor_get_shape(void);         // Query current shape
void cursor_get_hotspot(int32_t *hx, int32_t *hy); // Get hotspot offset
void cursor_draw(int32_t x, int32_t y);        // Alpha-blend cursor + save under
void cursor_restore(void);                     // Restore saved pixels
```

## Memory

- Cursor structs: `kmalloc` (< 1 KB each)
- Pixel buffers ≤4 KB: `kmalloc`
- Pixel buffers >4 KB: `pmm_alloc_contiguous` (follows tiered allocation rule)
- Xcur file read buffer: PMM (temporary, freed after parsing)

## Files

| File | Purpose |
|------|---------|
| `include/cursor.h` | Public API, enums, structs |
| `src/kernel/drivers/cursor.c` | Xcur parser, init, draw/restore |
| `resources/cursors/*` | 11 Adwaita cursor files (build host) |
| `Makefile` (sysroot rule) | Copies cursors to disk image |
