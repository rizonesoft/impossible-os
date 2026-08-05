---
schema_version: 1
id: win32-gdi-user32-stubs
domain: 08-graphics-ui
status: active
title: "TODO-14 -- Win32 GDI / USER32 Desktop API Stubs"
---

# TODO-14 -- Win32 GDI / USER32 Desktop API Stubs

> **Goal:** Build the Win32 GDI (graphics device context) and USER32 (window/message) stub layer that lets PE32+ applications call standard Windows drawing and windowing APIs against Impossible OS without modification. This is the prerequisite bridge for the Win32 compatibility layer in `02-kernel-core`.

> [!IMPORTANT]
> **Already exists**: All graphics primitives (`gfx_draw_line`, `gfx_fill_rect`, `gfx_draw_rect`, `gfx_fill_circle`, `gfx_blit_alpha`, `gfx_surface_create/destroy`, `gfx_surface_t`) in `gfx.h`. Font system (`ttf_draw_string`, `ttf_get`, `FONT_UI`, `FONT_UI_BOLD`) in `font_mgr.h`. Window manager (`wm_create_window`, `wm_destroy_window`, `wm_move_window`, `wm_resize_window`, `wm_focus_window`, `wm_mark_dirty`) in `wm.h`. Cursor shapes (`cursor_set_shape`, `CURSOR_ARROW/HAND/TEXT/MOVE/WAIT/CROSSHAIR/FORBIDDEN/RESIZE_*`) in `cursor.h`. Icon system (`icon_get`, `icon_for_extension`) in `icon_store.h`. `MessageBox`, `dialog_file_open/save`, `dialog_color` from TODO-05. `wm_minimize/maximize/restore/set_title` from TODO-06. **Missing**: all Win32 handle types (`HDC`, `HBITMAP`, etc.), GDI DC object table, shell icon index maps, USER32 message-loop infrastructure, `WM_*` constant IDs. **Scope boundary**: this TODO is stubs only -- each Win32 API wraps an existing Impossible OS primitive; the full Win32 PE loader and syscall thunking belong to `02-kernel-core`. Complete sections in order: icon map → GDI DC → GDI drawing → GDI text → cursor/icon/metrics → USER32 windows → USER32 message loop → dialogs.
>
> **USER32 export rows:** authoritative per-export checklist and Done bits live in [`../10-platform-services/TODO-A-user32-export-master-table.md`](../10-platform-services/TODO-A-user32-export-master-table.md) (do not maintain a second export inventory in this file).

## Inputs

- `include/gfx.h` -- all `gfx_*` drawing primitives + `gfx_surface_t` -- used by §3 GDI drawing and §2 DC backing
- `include/font_mgr.h` -- `ttf_get()`, `ttf_draw_string()`, `ttf_line_height()` -- used by §4 GDI text
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_destroy_window()`, `wm_move_window()`, `wm_resize_window()`, `wm_focus_window()`, `wm_mark_dirty()` -- used by §5 USER32 windows
- `include/cursor.h` -- `cursor_set_shape()`, `cursor_shape_t` enum -- used by §7 `LoadCursor/SetCursor`
- `include/icon_store.h` -- `icon_get()`, `icon_for_extension()`, `system_icon_t` -- used by §1 shell icon map and §7 `LoadIcon`
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()`, `pmm_free()` -- used by §2 `CreateCompatibleBitmap` to back `HBITMAP`
- `include/desktop/controls.h` (TODO-05) -- `MessageBox()`, `dialog_file_open/save()`, `dialog_color()` -- wrapped by §8
- `include/desktop/wm.h` -- `wm_minimize()`, `wm_maximize()`, `wm_restore()`, `wm_set_title()` (TODO-06) -- used by §5 `ShowWindow`
- Related (no stable XREF target): `02-kernel-core/TODO-*` (Win32 compatibility layer) -- this TODO is a direct prerequisite; PE32+ loader will call these GDI/USER32 stubs from user-mode via syscall thunks
- → XREF: `08-graphics-ui/TODO-06-widget-dialogs.md §8` -- `MessageBox()`, `dialog_file_open/save()`, `dialog_color()` must exist before §8 dialog wrappers
- → XREF: `08-graphics-ui/TODO-08-window-manager.md` -- `wm_minimize/maximize/restore/set_title` must exist before §5 `ShowWindow` wiring
- → XREF: `08-graphics-ui/TODO-01-graphics-asset-foundation.md §1-§5` -- GDI drawing, bitmaps, icons, and cursors must reuse the native graphics/asset foundation rather than fork a second raster stack
- → XREF: `08-graphics-ui/TODO-02-text-font-internationalization.md §1-§5` -- GDI text, font enumeration, and `ChooseFont` wrappers consume the shared text/layout foundation
- → XREF: `08-graphics-ui/TODO-07-ui-accessibility-automation-ime.md §1-§5` -- USER accessibility and IME-facing APIs consume the shared semantics/automation/composition layer

## Outcome

- `shell32_icon_map[]` + `imageres_icon_map[]` map Windows DLL icon indices to `system_icon_t`.
- `HDC/HBITMAP/HBRUSH/HPEN/HFONT/HGDIOBJ` are kernel-side opaque handles backed by a 256-slot object table.
- Full GDI drawing suite: `SetPixel/GetPixel/MoveToEx/LineTo/Rectangle/Ellipse/FillRect/BitBlt/StretchBlt` all wired to `gfx_*` primitives.
- `TextOutA/DrawTextA/CreateFontA/GetTextMetricsA` wired to `ttf_*`.
- `CreateWindowExA/ShowWindow/DestroyWindow/SetWindowTextA/GetClientRect/InvalidateRect` wired to WM.
- `GetMessageA` blocks per-window kernel message queue; `TranslateMessage` synthesizes `WM_CHAR`; `DispatchMessageA` calls the registered `WNDPROC`.
- `MessageBoxA/GetOpenFileNameA/GetSaveFileNameA/ChooseColorA` forward to existing TODO-05 dialogs.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                  | Depends On                                                                  | Status |
| --- | :---: | -------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Shell icon index map -- `shell32_icon_map[]`, `imageres_icon_map[]`, `win32_shell_icon()` | `icon_get()`, `system_icon_t` (both exist)                                 |  [ ]   |
| 💎  |   2   | §2 GDI device context stubs -- handle types, 256-slot object table, `GetDC/CreateCompatibleDC` | `gfx_surface_create()`, `pmm_alloc_contiguous()` (exist)               |  [ ]   |
| 💎  |   3   | §3 GDI drawing -- `SetPixel/LineTo/Rectangle/Ellipse/FillRect/BitBlt/StretchBlt`            | §2 DC table (all drawing ops take `HDC` first)                              |  [ ]   |
| 💎  |   4   | §4 GDI text -- `TextOutA/DrawTextA/CreateFontA/GetTextMetricsA`                              | §2 DC table; `ttf_draw_string/ttf_get` (exist)                             |  [ ]   |
| 💎  |   5   | §7 Cursor/icon/metrics -- `LoadCursor/SetCursor/LoadIcon/GetSystemMetrics`                   | §1 icon map; `cursor_set_shape()` (exists)                                  |  [ ]   |
| 💎  |   6   | §5 USER32 windows -- `CreateWindowExA/ShowWindow/DestroyWindow/SetWindowTextA/GetClientRect` | §2 DC (window DC); WM `wm_minimize/maximize/restore/set_title` (TODO-06)   |  [ ]   |
| ⭐  |   7   | §6 USER32 message loop -- `GetMessageA/TranslateMessage/DispatchMessageA`, per-window queue  | §5 windows (queue is per-window); novel kernel infrastructure               |  [ ]   |
| 💎  |   8   | §8 USER32 dialogs -- `MessageBoxA/GetOpenFileNameA/GetSaveFileNameA/ChooseColorA`            | §6 message loop (modal dialogs run their own inner loop); TODO-05 dialogs  |  [ ]   |

---

## 1. Win32 Shell Icon Index Map `[Sonnet]`

`shell32_icon_map[]` and `imageres_icon_map[]` tables mapping Windows shell32.dll / imageres.dll icon indices to `system_icon_t`. `icon_t *win32_shell_icon(int dll, int index)`. Used by `LoadIcon(NULL, MAKEINTRESOURCE(...))` and any Win32 app that hardcodes shell icon indices.

**Files:** `src/desktop/win32/win32_icons.c` (new), `include/desktop/win32/win32_icons.h` (new)

> [!NOTE]
> Shell32 icon indices are documented in MSDN. Common ones: 0=generic app, 1=document, 2=window, 3=open folder, 4=closed folder, 8=disk drive, 13=music note, 23=video, 28=trash empty, 29=trash full, 51=network, 160=text file, 165=image file. Imageres.dll: 1=computer, 15=folder, 54=user, 97=network. Define two static arrays `shell32_map[]` and `imageres_map[]` of `{ int index; system_icon_t icon; }` pairs; look up with linear search (small table, cache-friendly). `#define WIN32_DLL_SHELL32 1`, `#define WIN32_DLL_IMAGERES 2`. For unmapped indices: return `icon_get(ICON_APP_GENERIC, 16)`.

- [ ] `typedef struct { int index; system_icon_t icon; } win32_icon_map_entry_t;` in `win32_icons.h`
- [ ] `#define WIN32_DLL_SHELL32 1`, `#define WIN32_DLL_IMAGERES 2`
- [ ] `static const win32_icon_map_entry_t shell32_icon_map[]` -- ~30 entries covering the most common shell32 indices
- [ ] `static const win32_icon_map_entry_t imageres_icon_map[]` -- ~15 entries for imageres.dll
- [ ] `icon_bitmap_t *win32_shell_icon(int dll, int index)` -- linear search; fallback to `icon_get(ICON_APP_GENERIC, 16)`
- [ ] `system_icon_t win32_idi_to_sysicon(uintptr_t idi)` -- maps Win32 `IDI_APPLICATION=32512`, `IDI_ERROR=32513`, `IDI_QUESTION=32514`, `IDI_WARNING=32515`, `IDI_INFORMATION=32516` to corresponding `system_icon_t` values
- [ ] Commit: `"win32: shell icon index map -- shell32/imageres tables, win32_shell_icon(), IDI_* mapping"`

## 2. GDI Device Context Stubs `[Sonnet]`

`HDC`, `HBITMAP`, `HBRUSH`, `HPEN`, `HFONT`, `HGDIOBJ` as opaque kernel handles backed by a 256-slot object table. `CreateCompatibleDC/CreateCompatibleBitmap` → allocate `gfx_surface_t`. `SelectObject/DeleteObject/DeleteDC`. `GetDC(hWnd)` / `ReleaseDC(hWnd, hdc)` → window surface handle.

**Files:** `src/desktop/win32/gdi_dc.c` (new), `include/desktop/win32/gdi.h` (new)

> [!NOTE]
> Handle encoding: upper 8 bits = object type tag (`GDI_DC=1, GDI_BITMAP=2, GDI_BRUSH=3, GDI_PEN=4, GDI_FONT=5`); lower 24 bits = slot index in `g_gdi_objects[256]`. `typedef uint32_t HGDIOBJ; typedef HGDIOBJ HDC; typedef HGDIOBJ HBITMAP; ...`. Object table: `struct gdi_object { uint8_t type; union { gfx_surface_t *surface; gfx_color_t color; ttf_font_t *font; } u; } g_gdi_objects[256]`. **DC state**: each DC tracks `current_color`, `bg_color`, `bk_mode` (TRANSPARENT/OPAQUE), `current_font`, `pen_pos_x/y` for `MoveToEx/LineTo`. `GetDC(hWnd)`: look up `wm_window` by handle; return HDC wrapping the window's `gfx_surface_t` (no alloc; refcount incremented). `CreateCompatibleDC(hdc)`: alloc new `gfx_surface_t` matching `hdc`'s dimensions; store in table; return handle. `CreateCompatibleBitmap(hdc, w, h)`: `gfx_surface_create(&surf, w, h)` (PMM-backed); return HBITMAP. `SelectObject(hdc, hobj)`: update DC's current font/pen/brush; return previous selected object. `DeleteObject/DeleteDC`: free surface/font; zero table slot.

- [ ] `typedef uint32_t HGDIOBJ; typedef HGDIOBJ HDC, HBITMAP, HBRUSH, HPEN, HFONT;` in `gdi.h`
- [ ] `#define GDI_OBJ_DC 1`, `GDI_OBJ_BITMAP=2`, `GDI_OBJ_BRUSH=3`, `GDI_OBJ_PEN=4`, `GDI_OBJ_FONT=5`
- [ ] `struct gdi_dc_state { gfx_surface_t *surf; gfx_color_t text_color; gfx_color_t bg_color; uint8_t bk_mode; ttf_font_t *font; int32_t pen_x, pen_y; }` -- per-DC state block
- [ ] `struct gdi_object { uint8_t type; void *ptr; struct gdi_dc_state *dc_state; }` + `g_gdi_objects[256]`
- [ ] `HDC gdi_alloc_handle(uint8_t type, void *ptr)` -- find free slot; set type tag; return handle
- [ ] `void gdi_free_handle(HGDIOBJ h)` -- clear slot
- [ ] `HDC GetDC(int hWnd)` -- `wm_get_window_surface(hWnd)` → wrap in `gdi_alloc_handle`; `g_dc_states[h].surf = win_surf`
- [ ] `int ReleaseDC(int hWnd, HDC hdc)` -- `gdi_free_handle(hdc)` (window surface not freed, only DC slot)
- [ ] `HDC CreateCompatibleDC(HDC hdc)` -- alloc `gfx_surface_t` matching source dims; `gdi_alloc_handle(GDI_OBJ_DC, surf)`
- [ ] `HBITMAP CreateCompatibleBitmap(HDC hdc, int w, int h)` -- `gfx_surface_create(&surf, w, h)`; `gdi_alloc_handle(GDI_OBJ_BITMAP, surf)`
- [ ] `HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h)` -- update DC state (font/brush/pen); return old
- [ ] `int DeleteObject(HGDIOBJ h)` + `int DeleteDC(HDC hdc)` -- free backing memory; zero slot
- [ ] Commit: `"gdi: device context stubs -- 256-slot object table, GetDC/CreateCompatibleDC/HBITMAP, DC state"`

## 3. GDI Drawing Functions `[Sonnet]`

`SetPixel/GetPixel`, `MoveToEx/LineTo`, `Rectangle`, `Ellipse`, `FillRect`, `SetBkColor/SetTextColor/SetBkMode`. `BitBlt` (SRCCOPY → `gfx_blit`; PATCOPY stub). `StretchBlt` → `gfx_blit` with scale via `image_scale`.

**Files:** `src/desktop/win32/gdi_draw.c` (new), `include/desktop/win32/gdi.h` (extend)

> [!NOTE]
> All drawing functions extract `gfx_surface_t *s = dc->surf` from the DC state, then call `gfx_*` primitives. **`Rectangle(hdc, l, t, r, b)`**: `gfx_fill_rect(s, l, t, r-l, b-t, dc->bg_color)` + `gfx_draw_rect(s, l, t, r-l, b-t, 1, dc->text_color)` (Win32 uses outline + fill; use current brush for fill, current pen for border). **`Ellipse`**: `gfx_fill_circle(s, (l+r)/2, (t+b)/2, (r-l)/2, dc->brush_color)`. **`BitBlt` SRCCOPY**: `src_surf = gdi_get_surface(hSrcDC)`; `gfx_blit(dst_surf, dx, dy, src_surf, sx, sy, w, h)`. **`BitBlt` PATCOPY**: fill with current brush color stub. `StretchBlt`: `image_scale(&dst_img, &src_img, dst_w, dst_h, IMAGE_FIT_STRETCH)` then blit. **ROP3 codes**: implement only `SRCCOPY=0xCC0020` and `PATCOPY=0xF00021`; others return `FALSE` (unimplemented).

- [ ] `COLORREF SetTextColor(HDC hdc, COLORREF color)` -- update `dc->text_color`; return old
- [ ] `COLORREF SetBkColor(HDC hdc, COLORREF color)` -- update `dc->bg_color`; return old
- [ ] `int SetBkMode(HDC hdc, int mode)` -- `TRANSPARENT=1`, `OPAQUE=2`; update `dc->bk_mode`
- [ ] `COLORREF SetPixel(HDC hdc, int x, int y, COLORREF c)` -- bounds check; `s->pixels[y*s->stride + x] = c`; return `c`
- [ ] `COLORREF GetPixel(HDC hdc, int x, int y)` -- return `s->pixels[y*s->stride + x]`
- [ ] `int MoveToEx(HDC hdc, int x, int y, POINT *prev)` -- save `prev`; update `dc->pen_x/y`
- [ ] `int LineTo(HDC hdc, int x, int y)` -- `gfx_draw_line(s, dc->pen_x, dc->pen_y, x, y, dc->pen_color)`; update pen pos
- [ ] `int Rectangle(HDC hdc, int l, int t, int r, int b)` -- fill + outline
- [ ] `int Ellipse(HDC hdc, int l, int t, int r, int b)` -- `gfx_fill_circle` + `gfx_draw_circle` (add `gfx_draw_circle` stub if missing)
- [ ] `int FillRect(HDC hdc, const RECT *rc, HBRUSH hbr)` -- `gfx_fill_rect` with brush color
- [ ] `int BitBlt(HDC dst, int dx, int dy, int w, int h, HDC src, int sx, int sy, uint32_t rop)` -- SRCCOPY: `gfx_blit`; PATCOPY: fill; else return 0
- [ ] `int StretchBlt(HDC dst, int dx, int dy, int dw, int dh, HDC src, int sx, int sy, int sw, int sh, uint32_t rop)` -- `image_scale` + blit
- [ ] Commit: `"gdi: drawing functions -- SetPixel/LineTo/Rectangle/Ellipse/FillRect/BitBlt/StretchBlt wired to gfx_*"`

## 4. GDI Text Functions `[Sonnet]`

`TextOutA(hdc, x, y, str, len)` → `ttf_draw_string`. `DrawTextA(hdc, text, len, rect, flags)` → multi-line word-wrap. `CreateFontA(height, ..., facename)` → `ttf_get` nearest match. `GetTextMetricsA(hdc, tm)` → `TEXTMETRIC` from `ttf_line_height`.

**Files:** `src/desktop/win32/gdi_text.c` (new), `include/desktop/win32/gdi.h` (extend)

> [!NOTE]
> `CreateFontA`: Win32 passes `lfHeight` (negative = char height in pixels, positive = cell height), `lfWeight` (400=normal, 700=bold), `lfItalic`, `lfFaceName`. Map: `lfWeight >= 600` → `FONT_UI_BOLD`; else `FONT_UI`; `pixel_size = abs(lfHeight)`. Ignore `lfFaceName` for now (we only have Selawik; log if facename != "Selawik" + "Segoe UI" + variants). `GetTextMetricsA`: fill `TEXTMETRIC.tmHeight = ttf_line_height(font)`, `tmAscent`, `tmDescent` from font metrics. `DrawTextA` flags: `DT_LEFT=0`, `DT_CENTER=1`, `DT_RIGHT=2`, `DT_WORDBREAK=0x10`, `DT_SINGLELINE=0x20`. Multi-line: split at spaces when line exceeds `rect.right - rect.left`; advance `y += line_height`. `DT_CALCRECT`: compute bounding box without drawing (fill `rc` with needed dimensions).

- [ ] `HFONT CreateFontA(int height, int width, int escapement, int orientation, int weight, int italic, int underline, int strikeout, int charset, int out_prec, int clip_prec, int quality, int pitch, const char *facename)` -- map to `ttf_get(slot, abs(height))`; wrap in HFONT handle
- [ ] `int TextOutA(HDC hdc, int x, int y, const char *str, int len)` -- `ttf_draw_string(dc->surf, dc->font, x, y, str_copy, dc->text_color)`; handle `bk_mode == OPAQUE` by filling background rect first
- [ ] `int DrawTextA(HDC hdc, const char *text, int len, RECT *rc, uint32_t flags)` -- word-wrap if `DT_WORDBREAK`; single line if `DT_SINGLELINE`; h-align per `DT_LEFT/CENTER/RIGHT`; `DT_CALCRECT` mode computes size without draw
- [ ] `int GetTextMetricsA(HDC hdc, TEXTMETRIC *tm)` -- fill `tm->tmHeight`, `tm->tmAscent`, `tm->tmDescent`, `tm->tmAveCharWidth` from font metrics
- [ ] `typedef struct { int32_t left, top, right, bottom; } RECT;` + `typedef struct { int32_t x, y; } POINT;` in `gdi.h` (only if not already defined in a Win32 compat header)
- [ ] `BOOL GetTextExtentPoint32A(HDC hdc, const char *str, int len, SIZE *sz)` -- measure string width/height without drawing
- [ ] Commit: `"gdi: text functions -- TextOutA/DrawTextA word-wrap/CreateFontA/GetTextMetricsA wired to ttf_*"`

## 5. USER32 Window Functions `[Sonnet]`

`CreateWindowExA(exstyle, classname, title, style, x, y, w, h, parent, ...)` → `wm_create_window`. `ShowWindow(hWnd, nCmdShow)` → minimize/maximize/restore/show. `DestroyWindow` → `wm_destroy_window`. `SetWindowTextA` → `wm_set_title`. `GetClientRect/GetWindowRect`. `InvalidateRect/UpdateWindow`.

**Files:** `src/desktop/win32/user32_wnd.c` (new), `include/desktop/win32/user32.h` (new)

> [!NOTE]
> `HWND` is just an `int` handle (same as `wm_create_window()` return). `CreateWindowExA`: extract `x/y/w/h`; call `wm_create_window(title, x, y, w, h, flags_mapped)`; return handle as `HWND`. Style mapping: `WS_VISIBLE` → `WM_FLAG_VISIBLE`; `WS_POPUP` → no titlebar flag; `WS_CAPTION` → titlebar. Ignore `classname` (no class registration system -- all windows use the same WM). `nCmdShow` values: `SW_SHOW=5`, `SW_HIDE=0`, `SW_MINIMIZE=6`, `SW_MAXIMIZE=3`, `SW_RESTORE=9`, `SW_SHOWNOACTIVATE=4`. `GetClientRect`: `rc = { 0, 0, win->width, win->height }` (client area excludes titlebar; subtract `WM_TITLEBAR_HEIGHT`). `GetWindowRect`: `rc = { win->x, win->y, win->x + win->width, win->y + win->height }`. `InvalidateRect`: `wm_mark_dirty()` (partial dirty rect for the future; for now full redraw). `UpdateWindow`: no-op (compositor redraws on next tick).

- [ ] `typedef int HWND; typedef int HMENU; typedef void *LPVOID;` in `user32.h`
- [ ] Win32 style constants: `#define WS_VISIBLE 0x10000000`, `WS_CAPTION 0x00C00000`, `WS_POPUP 0x80000000`, `WS_CHILD 0x40000000`, `WS_SYSMENU 0x00080000`, etc.
- [ ] `HWND CreateWindowExA(uint32_t exstyle, const char *classname, const char *title, uint32_t style, int x, int y, int w, int h, HWND parent, void *hmenu, void *hinst, void *param)` -- maps to `wm_create_window()`
- [ ] `int ShowWindow(HWND hWnd, int nCmdShow)` -- SW_HIDE/SHOW/MINIMIZE/MAXIMIZE/RESTORE dispatch
- [ ] `int DestroyWindow(HWND hWnd)` → `wm_destroy_window(hWnd)`
- [ ] `int SetWindowTextA(HWND hWnd, const char *text)` → `wm_set_title(hWnd, text)`
- [ ] `int GetClientRect(HWND hWnd, RECT *rc)` -- fill with client area (win dims minus titlebar)
- [ ] `int GetWindowRect(HWND hWnd, RECT *rc)` -- fill with absolute screen position
- [ ] `int MoveWindow(HWND hWnd, int x, int y, int w, int h, int repaint)` → `wm_move_window()` + `wm_resize_window()`
- [ ] `int InvalidateRect(HWND hWnd, const RECT *rc, int erase)` → `wm_mark_dirty()`
- [ ] `int UpdateWindow(HWND hWnd)` → no-op (compositor drives redraws)
- [ ] `HWND SetFocus(HWND hWnd)` → `wm_focus_window(hWnd)`; return previous focused
- [ ] Commit: `"user32: window functions -- CreateWindowExA/ShowWindow/DestroyWindow/GetClientRect wired to WM"`

## 6. USER32 Message Loop `[Opus]`

`GetMessageA(msg, hWnd, min, max)` blocks on kernel per-window message queue. `TranslateMessage(msg)` synthesizes `WM_CHAR` from `WM_KEYDOWN` + layout. `DispatchMessageA(msg)` calls `WNDPROC`. `PostMessage/SendMessage`. Standard `WM_*` IDs: `WM_CREATE`, `WM_DESTROY`, `WM_PAINT`, `WM_CLOSE`, `WM_KEY*`, `WM_CHAR`, `WM_LBUTTONDOWN/UP`, `WM_MOUSEMOVE`, `WM_SIZE`, `WM_COMMAND`.

**Files:** `src/desktop/win32/user32_msg.c` (new), `include/desktop/win32/user32.h` (extend)

> [!NOTE]
> This is `[Opus]` -- the message loop requires a novel per-window kernel message queue architecture that does not yet exist in Impossible OS. Unlike the existing WM event dispatch (which calls callbacks synchronously in the compositor loop), Win32 requires that `GetMessageA` **blocks the calling user-mode thread** until a message arrives for that window. This means: (1) each `wm_window` needs a `msg_queue_t` ring buffer (32 entries); (2) the compositor pushes mouse/keyboard events as `MSG` structs into the target window's queue; (3) `sys_get_message(hWnd)` blocks the calling task on a semaphore until the queue is non-empty; (4) `DispatchMessageA` calls the registered `WNDPROC` function pointer with (hWnd, msg, wParam, lParam). **WNDPROC registration**: `RegisterClassA` stores `WNDPROC lpfnWndProc`; `CreateWindowExA` looks up the class to get the proc; stored in `wm_window.wnd_proc` (new field). **WM_PAINT**: generated when window is marked dirty AND `wm_window.update_pending=1`; cleared on `BeginPaint/EndPaint`. **WM_SIZE**: generated from `wm_resize_window()` call. **TranslateMessage**: if msg is `WM_KEYDOWN` with a printable key, synthesize `WM_CHAR` with the Unicode character (using current keyboard layout scancode→char table).

- [ ] Win32 message IDs: `#define WM_CREATE 1`, `WM_DESTROY 2`, `WM_MOVE 3`, `WM_SIZE 5`, `WM_ACTIVATE 6`, `WM_PAINT 15`, `WM_CLOSE 16`, `WM_QUIT 18`, `WM_KEYDOWN 0x100`, `WM_KEYUP 0x101`, `WM_CHAR 0x102`, `WM_SYSKEYDOWN 0x104`, `WM_LBUTTONDOWN 0x201`, `WM_LBUTTONUP 0x202`, `WM_RBUTTONDOWN 0x204`, `WM_RBUTTONUP 0x205`, `WM_MOUSEMOVE 0x200`, `WM_COMMAND 0x111`
- [ ] `typedef struct { HWND hwnd; uint32_t message; uintptr_t wParam; intptr_t lParam; uint32_t time; } MSG;`
- [ ] `typedef intptr_t (*WNDPROC)(HWND, uint32_t, uintptr_t, intptr_t);`
- [ ] Per-window message queue: `struct msg_queue { MSG entries[32]; uint8_t head, tail, count; }` -- add to `wm_window` struct
- [ ] `int PostMessageA(HWND hWnd, uint32_t msg, uintptr_t wParam, intptr_t lParam)` -- enqueue into `win->msg_queue`; signal semaphore
- [ ] `intptr_t SendMessageA(HWND hWnd, uint32_t msg, uintptr_t wParam, intptr_t lParam)` -- enqueue + block until `WNDPROC` returns result (synchronous)
- [ ] `int GetMessageA(MSG *msg, HWND hWnd, uint32_t min, uint32_t max)` -- block current task on queue semaphore; dequeue when non-empty; `WM_QUIT` returns 0; else 1
- [ ] `int TranslateMessage(const MSG *msg)` -- if `WM_KEYDOWN` and printable: `PostMessageA(msg->hwnd, WM_CHAR, scancode_to_char(wParam, layout), msg->lParam)`
- [ ] `intptr_t DispatchMessageA(const MSG *msg)` -- look up `wm_window.wnd_proc`; call `wnd_proc(hWnd, msg, wParam, lParam)`
- [ ] `ATOM RegisterClassA(const WNDCLASSA *wc)` -- store `wc->lpfnWndProc` + `wc->lpszClassName` in a 32-entry class table; return class atom (index+1)
- [ ] `void PostQuitMessage(int exit_code)` -- push `WM_QUIT` into calling thread's queue
- [ ] Compositor integration: in `wm_handle_mouse/key()`, after processing input: `PostMessageA(target_wnd, WM_LBUTTONDOWN, ...)` etc. into target window's queue
- [ ] Commit: `"user32: message loop -- per-window MSG queue, GetMessageA blocking, TranslateMessage WM_CHAR, DispatchMessageA WNDPROC"`

## 7. USER32 Cursor, Icon & System Metrics `[Sonnet]`

`LoadCursor(NULL, IDC_ARROW/IDC_WAIT/IDC_IBEAM/IDC_HAND/IDC_SIZEALL/etc.)` → `CURSOR_*` enum. `SetCursor(hCursor)` → `cursor_set_shape()`. `LoadIcon(NULL, IDI_*)` → `win32_shell_icon` lookup. `GetSystemMetrics(SM_*)` → WM/display state.

**Files:** `src/desktop/win32/user32_sys.c` (new), `include/desktop/win32/user32.h` (extend)

> [!NOTE]
> `HCURSOR` is just a `cursor_shape_t` cast to `uintptr_t`. `LoadCursor` maps: `IDC_ARROW=32512` → `CURSOR_ARROW`, `IDC_IBEAM=32513` → `CURSOR_TEXT`, `IDC_WAIT=32514` → `CURSOR_WAIT`, `IDC_CROSS=32515` → `CURSOR_CROSSHAIR`, `IDC_SIZEALL=32646` → `CURSOR_MOVE`, `IDC_NO=32648` → `CURSOR_FORBIDDEN`, `IDC_HAND=32649` → `CURSOR_HAND`, `IDC_SIZENS=32645` → `CURSOR_RESIZE_NS`, `IDC_SIZEWE=32644` → `CURSOR_RESIZE_EW`. `GetSystemMetrics` SM constants: `SM_CXSCREEN=0` → `fb_get_width()`, `SM_CYSCREEN=1` → `fb_get_height()`, `SM_CYCAPTION=4` → `DPI_SCALE(32)` (titlebar height), `SM_CXBORDER=5`/`SM_CYBORDER=6` → 1, `SM_CXFULLSCREEN=16` → `fb_get_width()`, `SM_CYFULLSCREEN=17` → `desktop_get_usable_height()`.

- [ ] `typedef uintptr_t HCURSOR; typedef uintptr_t HICON;`
- [ ] `HCURSOR LoadCursorA(void *hInst, uintptr_t cursor_name)` -- `cursor_name` is `IDC_*`; map to `cursor_shape_t`; return as `HCURSOR`
- [ ] `HCURSOR SetCursor(HCURSOR hCursor)` -- `cursor_set_shape((cursor_shape_t)hCursor)`; return previous
- [ ] `HICON LoadIconA(void *hInst, uintptr_t icon_name)` -- `icon_name` is `IDI_*`; call `win32_idi_to_sysicon()`; return `icon_bitmap_t *` as `HICON`
- [ ] `int GetSystemMetrics(int nIndex)` -- dispatch table for SM_CX/CYSCREEN, SM_CYCAPTION, SM_CXBORDER, SM_CXFULLSCREEN, SM_CYFULLSCREEN; default: return 0
- [ ] `HWND GetDesktopWindow(void)` -- return 0 (desktop is not a real window in our model; stub returns 0)
- [ ] `HWND GetForegroundWindow(void)` → `wm_get_focused_handle()`
- [ ] `int SetForegroundWindow(HWND hWnd)` → `wm_focus_window(hWnd)`
- [ ] Commit: `"user32: cursor/icon/metrics -- LoadCursor/SetCursor IDC_* map, LoadIcon IDI_*, GetSystemMetrics SM_*"`

## 8. USER32 Dialogs `[Sonnet]`

`MessageBoxA(hWnd, text, caption, uType)` → `MessageBox()` (TODO-05). `GetOpenFileNameA`/`GetSaveFileNameA` (OPENFILENAME struct) → `dialog_file_open/save`. `ChooseColorA` (CHOOSECOLOR struct) → `dialog_color`.

**Files:** `src/desktop/win32/user32_dialog.c` (new), `include/desktop/win32/user32.h` (extend)

> [!NOTE]
> `MessageBoxA`: map `uType` flags: `MB_OK=0`, `MB_OKCANCEL=1`, `MB_YESNO=4`, `MB_YESNOCANCEL=3`, `MB_ICONERROR=0x10`, `MB_ICONWARNING=0x30`, `MB_ICONINFORMATION=0x40`, `MB_ICONQUESTION=0x20`; pass to `MessageBox(text, caption, flags)`; return `IDOK/IDCANCEL/IDYES/IDNO`. `GetOpenFileNameA`: read `OPENFILENAME.lpstrFilter` + `lpstrInitialDir`; call `dialog_file_open(filter, init_dir, result_buf, buf_size)`; write path to `ofn->lpstrFile`; return `TRUE/FALSE`. `GetSaveFileNameA`: same but `dialog_file_save()`. `ChooseColorA`: extract `cc->rgbResult` seed color; call `dialog_color(&initial, &result)`; write back to `cc->rgbResult`; return `TRUE/FALSE`. These modal dialogs run the Win32 message loop (`GetMessageA` / `DispatchMessageA`) internally while open.

- [ ] Win32 `MB_*` constants + `ID*` return values in `user32.h`
- [ ] `int MessageBoxA(HWND hWnd, const char *text, const char *caption, uint32_t uType)` -- map `uType` → Impossible OS `MB_*` flags; call `MessageBox()`; map return to Win32 `IDOK/IDCANCEL/IDYES/IDNO`
- [ ] `typedef struct { uint32_t lStructSize; HWND hwndOwner; const char *lpstrFilter; char *lpstrFile; uint32_t nMaxFile; const char *lpstrInitialDir; const char *lpstrTitle; uint32_t Flags; } OPENFILENAMEA;`
- [ ] `int GetOpenFileNameA(OPENFILENAMEA *ofn)` -- call `dialog_file_open(ofn->lpstrFilter, ofn->lpstrInitialDir, ofn->lpstrFile, ofn->nMaxFile)`; return `TRUE` if user confirmed
- [ ] `int GetSaveFileNameA(OPENFILENAMEA *ofn)` -- call `dialog_file_save(...)` same pattern
- [ ] `typedef struct { uint32_t lStructSize; HWND hwndOwner; uint32_t rgbResult; uint32_t *lpCustColors; uint32_t Flags; } CHOOSECOLORA;`
- [ ] `int ChooseColorA(CHOOSECOLORA *cc)` -- extract seed from `cc->rgbResult`; `dialog_color(&seed, &result)`; write back; return `TRUE/FALSE`
- [ ] Commit: `"user32: dialogs -- MessageBoxA/GetOpenFileName/GetSaveFileName/ChooseColorA forwarding to TODO-05 dialogs"`

---

## OS Comparison


| ⭐  | Feature                    | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | -------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | Shell icon index map       | ✅ shell32.dll + imageres.dll; full icon | ✅ XDG icon theme; `gtk_icon_theme_load_icon`; no | ⬜ §1 -- ~45 mapped entries; `win32_shell_icon(dll, index)` |
| 💎  | GDI device context         | ✅ Win32 GDI DC; GDI object              | ✅ Xlib `XCreateGC`; Cairo device contexts; | ⬜ §2 -- 256-slot object table; `gfx_surface_t` backing |
| 💎  | GDI drawing                | ✅ Full GDI drawing API; ROP3            | ✅ Cairo/Xlib; Wine GDI drawing; full    | ⬜ §3 -- SRCCOPY+PATCOPY BitBlt; all wired to |
| 💎  | GDI text                   | ✅ Win32 GDI text; font selection;       | ✅ Pango/Cairo text; Wine GDI text       | ⬜ §4 -- `ttf_draw_string` backend; `CreateFontA` maps weight/size |
| 💎  | USER32 windows             | ✅ Full USER32 window creation; WS_*     | ✅ GTK/Qt window APIs; Wine USER32       | ⬜ §5 -- `wm_create_window` backend; WS_*/SW_* style/show-cmd mapping |
| ⭐  | USER32 message loop        | ✅ Full Win32 message pump; per-thread   | ✅ X11 event loop; Wayland protocol;     | ⬜ §6 -- `⭐` per-window kernel queue (not |
| 💎  | Cursor/icon/system metrics | ✅ Full USER32 cursor/icon management; `GetSystemMetrics` | ✅ `XDefineCursor`; GDK cursor API; no   | ⬜ §7 -- 9 IDC_* cursor mappings; SM_CX/CYSCREEN |
| 💎  | USER32 dialogs             | ✅ Full comdlg32.dll; `MessageBox`, `GetOpenFileName`, `ChooseColor` | ✅ GTK/Qt dialog APIs; Wine comdlg32     | ⬜ §8 -- thin forwarding layer to existing |

> **After §1–§8:** Impossible OS has a functional GDI/USER32 stub surface sufficient for Win32 PE32+ apps to call standard drawing, windowing, and dialog APIs. The `⭐` differentiator is the message queue architecture: unlike Windows (per-thread queue) or Wine (per-thread emulation), Impossible OS uses a per-window kernel queue -- simpler, with no thread-affinity complexity, while still delivering the blocking `GetMessageA` contract Win32 apps expect.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `win32_shell_icon(WIN32_DLL_SHELL32, 4)` → returns valid folder icon bitmap (non-NULL, non-zero pixels)
- [ ] `HDC hdc = CreateCompatibleDC(NULL); HBITMAP bmp = CreateCompatibleBitmap(hdc, 100, 100)` → non-zero handles; `DeleteObject(bmp); DeleteDC(hdc)` → no memory leak (PMM free confirmed via serial log)
- [ ] `HDC hdc = GetDC(hwnd); Rectangle(hdc, 10, 10, 90, 90); ReleaseDC(hwnd, hdc)` → rectangle visible in QEMU window
- [ ] `TextOutA(hdc, 10, 10, "Hello Win32", 11)` → text visible; `CreateFontA(-16, 0, 0, 0, 700, 0, 0, 0, 0, 0, 0, 0, 0, "Segoe UI")` → bold font selected
- [ ] `CreateWindowExA(0, "TestClass", "My Window", WS_VISIBLE|WS_CAPTION, 100, 100, 400, 300, 0, 0, 0, 0)` → WM creates and shows window with titlebar
- [ ] `GetMessageA(&msg, hWnd, 0, 0)` blocks; click the window → `WM_LBUTTONDOWN` message dequeued; `DispatchMessageA` calls registered `WNDPROC`; `TranslateMessage` on `WM_KEYDOWN` 'A' → `WM_CHAR` 'A' synthesized
- [ ] `MessageBoxA(0, "Test", "Title", MB_OKCANCEL|MB_ICONINFORMATION)` → modal dialog appears; OK → returns `IDOK`
- [ ] `GetSystemMetrics(SM_CXSCREEN)` → matches `fb_get_width()`; `SM_CYCAPTION` → matches titlebar height
- [ ] Commit: `"win32: GDI/USER32 stub layer -- DC/drawing/text/windows/message-loop/dialogs complete"`
