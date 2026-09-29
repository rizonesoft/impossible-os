<!-- docs: covers=todo/10-platform-services/TODO-02-paint-app.md sources=src/desktop/desktop.c,include/gfx.h,include/kernel/image.h,src/kernel/image.c,src/kernel/image_scale.c,src/kernel/image_save.c,include/font_mgr.h,include/desktop/wm.h,include/desktop/controls.h reviewed=2026-09-29 order=2 -->
# Paint

## What is it?

Paint is the planned built-in image editor, modelled on Windows Paint: a canvas, eight drawing tools, a colour bar, undo and redo, and opening and saving BMP and PNG files, with selection and zoom as stretch goals. It also serves as a workout for the graphics and widget stack. The app itself has not started, but most of the drawing and image calls it needs already ship in the kernel.

## How does it work?

**Today.** There is no Paint window. The building blocks exist:

- **Surfaces and drawing.** [`gfx.h`](../../include/gfx.h) provides `gfx_surface_t` with `gfx_surface_create()` and `gfx_surface_destroy()`, and the primitives Paint's tools map onto: `gfx_put_pixel()`, `gfx_draw_line()`, `gfx_draw_rect()`, `gfx_fill_rect()`, `gfx_fill_circle()`, `gfx_blit()` and `gfx_blit_alpha()`.
- **Images.** [`image.h`](../../include/kernel/image.h) loads files with `image_load()` (and `image_load_mem()`) into a BGRA `image_t`, scales with `image_scale()` using one of five fit modes (stretch, fill, fit, centre, tile; box filter above 2x downscale, bilinear otherwise), and saves with `image_save_bmp()` and `image_save_png()`. The PNG writer is the vendored `stb_image_write` with its allocator redirected to the kernel ([`image_save.c`](../../src/kernel/image_save.c)).
- **Text.** `ttf_get(FONT_UI, size)` then `ttf_draw_string(surface, font, x, y, text, colour)` ([`font_mgr.h`](../../include/font_mgr.h)).
- **Windows and controls.** `wm_create_window()` ([`wm.h`](../../include/desktop/wm.h)) and the control library, which today has buttons, labels, text boxes and scroll bars only ([`controls.h`](../../include/desktop/controls.h)).

**Planned design.**

1. **Canvas and viewport.** A `struct paint` holding an 800 by 600 canvas surface in page-allocated memory, a scrolling and zooming viewport, a tool panel and a palette bar.
2. **Tools.** Pencil (Bresenham lines between mouse samples), brush (circle stamps), eraser, line, rectangle, ellipse, a breadth-first flood fill with its work stack in page-allocated memory, and text.
3. **Colours.** Twenty preset swatches, foreground and background squares with a swap button, and the colour picker dialog as a stretch.
4. **Undo and redo.** A ring of 32 snapshots, storing only the changed rectangle for large canvases.
5. **Files.** New, Open, Save and Save As through `image_load()` and the BMP and PNG savers, with an unsaved-changes guard.
6. **Stretch.** A rectangular selection with copy, paste, move and delete through the clipboard, and zoom from 25 to 800 percent with resize and crop.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `gfx_surface_create()`, the `gfx_*` drawing primitives | Shipped |
| `image_load()`, `image_scale()`, `image_save_bmp()`, `image_save_png()` | Shipped |
| `gfx_get_pixel()`, `gfx_surface_scale_blit()` | Planned; not declared in `gfx.h` |
| Menu bar, status bar, list view and slider controls | Planned in the widget roadmaps |
| `dialog_file_open()`, `dialog_file_save()`, `dialog_color()` | Planned in [Complex Controls and Dialogs](../graphics/complex-controls-dialogs.md) |
| `clipboard_set()` and `clipboard_get()` with `CLIP_IMAGE` | Planned in the [Clipboard](../desktop/clipboard.md) roadmap |

## How do I use it?

Paint cannot be launched yet. Of the image calls it will use, `image_load()` and `image_scale()` already run at every boot, when the desktop decodes and scales the wallpaper ([`desktop.c`](../../src/desktop/desktop.c)). Nothing calls `image_save_bmp()` or `image_save_png()` yet, so Paint will be their first user. See [Graphics Assets](../graphics/graphics-assets.md) for the image pipeline.

## What is not implemented yet?

- [Canvas and Viewport](../../todo/10-platform-services/TODO-02-paint-app.md#1-canvas--viewport-sonnet), which also needs the menu bar and status bar controls
- [Drawing Tools](../../todo/10-platform-services/TODO-02-paint-app.md#2-drawing-tools-sonnet) and the [Color System](../../todo/10-platform-services/TODO-02-paint-app.md#3-color-system-sonnet)
- [Undo and Redo](../../todo/10-platform-services/TODO-02-paint-app.md#4-undoredo-sonnet)
- [File Operations](../../todo/10-platform-services/TODO-02-paint-app.md#5-file-operations-sonnet), which needs the file dialogs
- [Selection Tool](../../todo/10-platform-services/TODO-02-paint-app.md#6-selection-tool-stretch-sonnet) and [Zoom and Resize](../../todo/10-platform-services/TODO-02-paint-app.md#7-zoom--resize-stretch-sonnet), both stretch goals

## How does it compare with Windows 11 and Linux?

Windows 11 Paint is a GDI-backed editor with 20 swatches, layers, limited undo and BMP, JPEG, GIF, PNG and TIFF formats. On Linux the usual equivalents are GIMP, Pinta and KolourPaint, with 50 or more undo levels and many formats. The Impossible OS plan is smaller: BMP and PNG only, 32 undo snapshots and a flood fill whose stack lives in page-allocated memory so a large fill cannot exhaust the kernel heap. It does not exist yet.

## See also

- [Paint App and Image Tools roadmap](../../todo/10-platform-services/TODO-02-paint-app.md)
- [Photos and Image Viewer roadmap](../../todo/11-apps/TODO-11-photos-image-viewer.md)
- [Graphics Assets](../graphics/graphics-assets.md)
- [Core Widgets](../graphics/widget-library.md)
- [Control Library](../desktop/control-library.md)
