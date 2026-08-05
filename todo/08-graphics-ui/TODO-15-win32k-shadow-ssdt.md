---
schema_version: 1
id: win32k-shadow-ssdt
domain: 08-graphics-ui
status: active
title: "TODO-15 -- Win32k Shadow SSDT (NtGdi / NtUser)"
---

# TODO-15 -- Win32k Shadow SSDT (NtGdi / NtUser)

> **Goal:** Build the Win32k shadow System Service Descriptor Table (SSDT Table 1) -- the kernel-mode dispatch layer for all GDI and USER32 syscalls. In Windows, `win32k.sys` handles ~1300 `NtGdiXxx` and `NtUserXxx` entries. User-mode `gdi32.dll` and `user32.dll` call into this table via `syscall` with service numbers starting at `0x1000`. The **canonical slot table** for all reserved indices is [`TODO-A-Win32k-Shadow-SSDT-Master-Table.md`](TODO-A-Win32k-Shadow-SSDT-Master-Table.md). This file owns **implementation** (handlers, compositor integration, tests). **Router and syscall contract** for Table 1 live in [`TODO-16-win32k-shadow-native-api.md`](TODO-16-win32k-shadow-native-api.md).

> [!IMPORTANT]
> **Current state:** The compositor, window manager, GDI primitives (`gfx_*`), font system (`ttf_*`), cursor shapes, and icon store all exist as kernel-mode C APIs. There is NO shadow SSDT, no `NtGdiXxx`/`NtUserXxx` dispatch, and no user-mode thunking. Win32 apps currently cannot call GDI/USER32 functions via syscall. The existing `SYS_WAIT_MESSAGE=75`, `SYS_GETMESSAGE=74`, `SYS_REGISTERCLASS=76`, `SYS_FINDWINDOW=77` in TODO-05-win32-subsystem are placeholders that need migration to the shadow SSDT.

## Inputs

- [`include/gfx.h`](../../include/gfx.h) -- `gfx_draw_line`, `gfx_fill_rect`, `gfx_blit_alpha`, `gfx_surface_create/destroy`
- [`include/font_mgr.h`](../../include/font_mgr.h) -- `ttf_get`, `ttf_draw_string`, `ttf_line_height`
- [`include/desktop/wm.h`](../../include/desktop/wm.h) -- `wm_create_window`, `wm_destroy_window`, `wm_move_window`, `wm_focus_window`, `wm_mark_dirty`
- [`include/cursor.h`](../../include/cursor.h) -- `cursor_set_shape`, `cursor_shape_t`
- [`include/icon_store.h`](../../include/icon_store.h) -- `icon_get`, `icon_for_extension`
- → XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §5` -- main SSDT infrastructure; shadow SSDT (Table 1) placeholder allocated there; service numbers 0x1000+ dispatched to this table
- → XREF: `08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md` -- authoritative 1300-row index map (function name, owner, done flag)
- → XREF: `08-graphics-ui/TODO-16-win32k-shadow-native-api.md` -- Table 1 routing, bounds, NTSTATUS contract, static asserts vs `TODO-A`
- → XREF: `08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md` -- user-mode GDI/USER32 stub layer; this TODO provides the kernel-mode dispatch those stubs call into
- → XREF: `12-user-platform-sdk/TODO-05-win32-subsystem.md` -- CSRSS loads win32k; message queue infrastructure (SYS_WAIT_MESSAGE etc.) migrates to this shadow SSDT
- → XREF: `08-graphics-ui/TODO-08-window-manager.md` -- `wm_minimize/maximize/restore/set_title` wrapped by NtUserXxx
- → XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §25` -- KeUserModeCallback dispatch infrastructure; NtUserDispatchMessage and NtUserSendMessage call KeUserModeCallback to invoke user-mode window procedures
- → XREF: `08-graphics-ui/TODO-05-widget-library-core.md` -- control painting routed through GDI DC
- → XREF: `08-graphics-ui/TODO-01-graphics-asset-foundation.md §1-§6` -- drawing, bitmaps, icons, cursors, and display surfaces must reuse the native graphics foundation
- → XREF: `08-graphics-ui/TODO-02-text-font-internationalization.md §1-§6` -- font, text, layout, and run-cache work is owned there and consumed here
- → XREF: `08-graphics-ui/TODO-07-ui-accessibility-automation-ime.md §1-§6` -- USER accessibility, automation, and IME syscall waves consume the shared provider and composition foundation

## Outcome

- Shadow SSDT (Table 1) reserves **1300** contiguous service indices starting at `0x1000` (see `TODO-A`); bring-up implements handlers wave by wave until full `NtGdiXxx` / `NtUserXxx` parity.
- `gdi32.dll` stubs do `mov rax, 0x1000 + index; syscall` to reach kernel-mode GDI handlers.
- `user32.dll` stubs do the same for window/message/input handlers.
- The existing `gfx_*`, `ttf_*`, `wm_*`, `cursor_*` primitives are wrapped with NT-compatible signatures and NTSTATUS returns.
- Win32 PE applications can call `CreateWindowExA`, `GetMessageA`, `BeginPaint`, `TextOutA`, `BitBlt` etc. without modification.

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On      | Status |
| --- | :---: | ---------------------------------------- | --------------- | :----: |
| 💎   |   1   | Shadow SSDT infrastructure (Table 1 dispatch) | TODO-05 §3      |  [ ]   |
| 💎   |   2   | GDI device context (DC) object table     | §1              |  [ ]   |
| 💎   |   3   | GDI drawing syscalls (line, rect, ellipse, blit) | §2              |  [ ]   |
| 💎   |   4   | GDI text and font syscalls               | §2              |  [ ]   |
| 💎   |   5   | GDI bitmap and DIB syscalls              | §2              |  [ ]   |
| 💎   |   6   | GDI pen, brush, and region syscalls      | §2              |  [ ]   |
| 💎   |   7   | USER window management syscalls          | §1, TODO-06     |  [ ]   |
| 💎   |   8   | USER message queue syscalls              | §7              |  [ ]   |
| 💎   |   9   | USER input and cursor syscalls           | §7              |  [ ]   |
| 💎   |  10   | USER menu and accelerator syscalls       | §7              |  [ ]   |
| 💎   |  11   | USER clipboard syscalls                  | §7              |  [ ]   |
| ⭐   |  12   | Migrate SYS_GETMESSAGE/etc. to shadow SSDT | §8              |  [ ]   |
| 💎   |  13   | GDI path, curve, and extended drawing    | §3              |  [ ]   |
| 💎   |  14   | GDI transform, palette, and color management | §2              |  [ ]   |
| 💎   |  15   | GDI print and metafile                   | §2              |  [ ]   |
| 💎   |  16   | GDI font advanced                        | §4              |  [ ]   |
| 💎   |  17   | GDI extended object management           | §2, §6          |  [ ]   |
| 💎   |  18   | USER window properties, styles, enumeration | §7              |  [ ]   |
| 💎   |  19   | USER dialog, caret, and drawing helpers  | §7              |  [ ]   |
| 💎   |  20   | USER scrollbar                           | §7              |  [ ]   |
| 💎   |  21   | USER keyboard, IME, and hook             | §8, §9          |  [ ]   |
| 💎   |  22   | USER DPI, accessibility, system parameters | §7              |  [ ]   |
| 💎   |  23   | USER raw input, touch, and gesture       | §9              |  [ ]   |
| 💎   |  24   | USER multi-monitor and display           | §1              |  [ ]   |
| 💎   |  25   | USER shell integration                   | §7, §8          |  [ ]   |
| 💎   |  26   | GDI/USER DirectX and DXGI kernel integration | §1, TODO-08-gpu |  [ ]   |
| ⭐   |  27   | Impossible OS exclusive graphics extensions | §2, §7, §8      |  [ ]   |

> 💎 = parity -- Windows GDI32/USER32 and Linux Xlib/Wayland both provide equivalent functionality.
> ⭐ = exclusive -- clean migration path from legacy SYS_* to proper shadow SSDT.

---

## 1. Shadow SSDT Infrastructure (Table 1 Dispatch)
Register the Win32k shadow SSDT as Table 1 in the SSDT dispatcher. Service numbers `0x1000–0x1FFF` are routed to this table. (→ XREF: TODO-12-native-api-ssdt.md §5)

- [ ] Create `include/kernel/nt/win32k_ssdt.h`:
  - `WIN32K_SSDT_TABLE` -- same structure as main SSDT but separate function pointer array
  - `WIN32K_SERVICE_BASE = 0x1000` -- offset for shadow table indices
- [ ] In `syscall_dispatch`: if `(service_number & 0x1000)`, dispatch to shadow SSDT at `index = service_number & 0x0FFF`
- [ ] Create `src/kernel/win32k/win32k_init.c`:
  - `win32k_init()` -- register all NtGdiXxx and NtUserXxx handlers in shadow SSDT
  - Called during Phase 3 init (after compositor is ready)
- [ ] Unimplemented shadow slots return `STATUS_NOT_IMPLEMENTED`
- [ ] Commit: `"kernel: win32k -- shadow SSDT Table 1 dispatch infrastructure"`

**Test checkpoint:** `syscall_dispatch(0x1000)` routes to shadow SSDT, not main SSDT. Unimplemented shadow slot returns `STATUS_NOT_IMPLEMENTED`. Serial: `"win32k: shadow SSDT registered, 1300 services"` once the full table from `TODO-A` is registered.

## 2. GDI Device Context (DC) Object Table
The HDC is a kernel-side handle to a drawing surface. All GDI drawing goes through a DC.

- [ ] Create `src/kernel/win32k/gdi_dc.c`:
  - DC object table: 256-slot array of `GDI_DC` structs (or Ob-managed)
  - `GDI_DC`: `gfx_surface_t *surface`, `uint32_t text_color`, `uint32_t bg_color`, `HFONT font`, `HPEN pen`, `HBRUSH brush`, `int32_t pos_x/pos_y`, `RECT clip_rect`
- [ ] `NtGdiCreateCompatibleDC(hdc)` → shadow SSDT 0x1000: create memory DC
- [ ] `NtGdiDeleteObjectApp(handle)` → shadow SSDT 0x1001: delete any GDI object (DC, bitmap, pen, brush, font)
- [ ] `NtGdiSelectObject(hdc, hgdiobj)` → shadow SSDT 0x1002: select pen/brush/font/bitmap into DC
- [ ] `NtGdiGetDC(hwnd)` → shadow SSDT 0x1003: get DC for a window (wraps `wm_get_surface`)
- [ ] `NtGdiReleaseDC(hwnd, hdc)` → shadow SSDT 0x1004: release window DC
- [ ] `NtGdiSaveDC(hdc)` / `NtGdiRestoreDC(hdc, nSavedDC)` → shadow SSDT 0x1005/0x1006
- [ ] Commit: `"kernel: win32k -- GDI DC object table, CreateCompatibleDC, GetDC/ReleaseDC"`

**Test checkpoint:** `NtGdiGetDC(hwnd)` returns valid HDC backed by window surface. `NtGdiSelectObject` swaps pen into DC. `NtGdiDeleteObjectApp` frees the object.

## 3. GDI Drawing Syscalls
Core 2D drawing operations that wrap `gfx_*` primitives.

- [ ] `NtGdiSetPixel(hdc, x, y, color)` → shadow SSDT 0x1010
- [ ] `NtGdiGetPixel(hdc, x, y)` → shadow SSDT 0x1011
- [ ] `NtGdiMoveTo(hdc, x, y, lpPoint)` → shadow SSDT 0x1012: set DC current position
- [ ] `NtGdiLineTo(hdc, x, y)` → shadow SSDT 0x1013: draw line from current position (wraps `gfx_draw_line`)
- [ ] `NtGdiRectangle(hdc, left, top, right, bottom)` → shadow SSDT 0x1014: draw rectangle (wraps `gfx_draw_rect`)
- [ ] `NtGdiFillRect(hdc, rect, hbrush)` → shadow SSDT 0x1015: fill rectangle (wraps `gfx_fill_rect`)
- [ ] `NtGdiEllipse(hdc, left, top, right, bottom)` → shadow SSDT 0x1016: draw ellipse (wraps `gfx_fill_circle` for filled)
- [ ] `NtGdiBitBlt(hdcDest, x, y, w, h, hdcSrc, xSrc, ySrc, rop)` → shadow SSDT 0x1017: bit-block transfer (wraps `gfx_blit_alpha`)
- [ ] `NtGdiStretchBlt(hdcDest, xD, yD, wD, hD, hdcSrc, xS, yS, wS, hS, rop)` → shadow SSDT 0x1018: stretched blit
- [ ] `NtGdiPatBlt(hdc, x, y, w, h, rop)` → shadow SSDT 0x1019: pattern fill with current brush
- [ ] `NtGdiPolyline(hdc, points, count)` → shadow SSDT 0x101A: draw connected line segments
- [ ] `NtGdiPolygon(hdc, points, count)` → shadow SSDT 0x101B: draw filled polygon
- [ ] `NtGdiSetBkColor(hdc, color)` / `NtGdiSetTextColor(hdc, color)` → shadow SSDT 0x101C/0x101D
- [ ] `NtGdiSetBkMode(hdc, mode)` → shadow SSDT 0x101E: TRANSPARENT or OPAQUE background
- [ ] Commit: `"kernel: win32k -- GDI drawing syscalls (SetPixel through Polygon)"`

**Test checkpoint:** `NtGdiGetDC` + `NtGdiRectangle` draws visible rectangle on window. `NtGdiBitBlt` copies region between DCs. `NtGdiSetTextColor` changes subsequent text color.

## 4. GDI Text and Font Syscalls
Text rendering that wraps `ttf_*` primitives.

- [ ] `NtGdiCreateFont(height, width, escapement, orientation, weight, italic, underline, strikeout, charset, outPrecision, clipPrecision, quality, pitchAndFamily, faceName)` → shadow SSDT 0x1020: create logical font (wraps `ttf_get`)
- [ ] `NtGdiTextOut(hdc, x, y, string, count)` → shadow SSDT 0x1021: draw text (wraps `ttf_draw_string`)
- [ ] `NtGdiExtTextOut(hdc, x, y, options, rect, string, count, dx)` → shadow SSDT 0x1022: extended text with clipping
- [ ] `NtGdiDrawText(hdc, string, count, rect, format)` → shadow SSDT 0x1023: formatted text in rectangle
- [ ] `NtGdiGetTextMetrics(hdc, tm)` → shadow SSDT 0x1024: font metrics (height, ascent, descent, average width)
- [ ] `NtGdiGetTextExtentPoint(hdc, string, count, size)` → shadow SSDT 0x1025: measure string pixel dimensions
- [ ] `NtGdiGetTextFace(hdc, count, faceName)` → shadow SSDT 0x1026: get current font face name
- [ ] `NtGdiSetTextAlign(hdc, align)` → shadow SSDT 0x1027: TA_LEFT/TA_CENTER/TA_RIGHT/TA_TOP/TA_BOTTOM
- [ ] Commit: `"kernel: win32k -- GDI text and font syscalls (CreateFont through SetTextAlign)"`

**Test checkpoint:** `NtGdiCreateFont` + `NtGdiSelectObject` + `NtGdiTextOut` renders text on window DC. `NtGdiGetTextMetrics` returns correct line height. `NtGdiGetTextExtentPoint` returns correct string width.

## 5. GDI Bitmap and DIB Syscalls
Bitmap creation, DIB (device-independent bitmap) operations.

- [ ] `NtGdiCreateCompatibleBitmap(hdc, width, height)` → shadow SSDT 0x1030: allocate pixel buffer
- [ ] `NtGdiCreateDIBSection(hdc, bmi, usage, bits, hSection, offset)` → shadow SSDT 0x1031: create DIB with direct pixel access
- [ ] `NtGdiGetDIBits(hdc, hbm, start, lines, bits, bmi, usage)` → shadow SSDT 0x1032: read bitmap pixels
- [ ] `NtGdiSetDIBits(hdc, hbm, start, lines, bits, bmi, usage)` → shadow SSDT 0x1033: write bitmap pixels
- [ ] `NtGdiSetDIBitsToDevice(hdc, xDest, yDest, w, h, xSrc, ySrc, startScan, lines, bits, bmi, usage)` → shadow SSDT 0x1034
- [ ] `NtGdiStretchDIBits(hdc, xD, yD, wD, hD, xS, yS, wS, hS, bits, bmi, usage, rop)` → shadow SSDT 0x1035
- [ ] `NtGdiGetObject(handle, count, buffer)` → shadow SSDT 0x1036: get GDI object properties (BITMAP, LOGFONT, etc.)
- [ ] Commit: `"kernel: win32k -- GDI bitmap and DIB syscalls"`

**Test checkpoint:** `NtGdiCreateCompatibleBitmap` + `NtGdiSelectObject` into memory DC + `NtGdiBitBlt` renders offscreen buffer. `NtGdiCreateDIBSection` returns direct pixel pointer. `NtGdiGetDIBits` reads correct pixel data.

## 6. GDI Pen, Brush, and Region Syscalls
Drawing tool creation and region management.

- [ ] `NtGdiCreatePen(style, width, color)` → shadow SSDT 0x1040: PS_SOLID, PS_DASH, PS_DOT, etc.
- [ ] `NtGdiCreateSolidBrush(color)` → shadow SSDT 0x1041
- [ ] `NtGdiCreateHatchBrush(style, color)` → shadow SSDT 0x1042: HS_HORIZONTAL, HS_VERTICAL, HS_CROSS, etc.
- [ ] `NtGdiCreatePatternBrush(hbitmap)` → shadow SSDT 0x1043
- [ ] `NtGdiGetStockObject(index)` → shadow SSDT 0x1044: WHITE_BRUSH, BLACK_PEN, SYSTEM_FONT, etc.
- [ ] `NtGdiCreateRectRgn(left, top, right, bottom)` → shadow SSDT 0x1045
- [ ] `NtGdiCombineRgn(dest, src1, src2, combineMode)` → shadow SSDT 0x1046: RGN_AND/OR/XOR/DIFF
- [ ] `NtGdiSelectClipRgn(hdc, hrgn)` → shadow SSDT 0x1047: set DC clipping region
- [ ] `NtGdiOffsetRgn(hrgn, x, y)` → shadow SSDT 0x1048
- [ ] `NtGdiPtInRegion(hrgn, x, y)` → shadow SSDT 0x1049
- [ ] Commit: `"kernel: win32k -- GDI pen, brush, and region syscalls"`

**Test checkpoint:** `NtGdiCreatePen(PS_SOLID, 2, red)` + `NtGdiSelectObject` + `NtGdiLineTo` draws red 2px line. `NtGdiCreateRectRgn` + `NtGdiSelectClipRgn` clips drawing to region. `NtGdiGetStockObject(WHITE_BRUSH)` returns valid handle.

## 7. USER Window Management Syscalls
Window creation, positioning, and properties -- wraps `wm_*` APIs.

- [ ] `NtUserCreateWindowEx(dwExStyle, className, windowName, dwStyle, x, y, w, h, hwndParent, hMenu, hInstance, lpParam)` → shadow SSDT 0x1050: create window (wraps `wm_create_window`)
- [ ] `NtUserDestroyWindow(hwnd)` → shadow SSDT 0x1051: destroy window (wraps `wm_destroy_window`)
- [ ] `NtUserShowWindow(hwnd, nCmdShow)` → shadow SSDT 0x1052: SW_SHOW/SW_HIDE/SW_MINIMIZE/SW_MAXIMIZE (wraps `wm_minimize/maximize/restore`)
- [ ] `NtUserMoveWindow(hwnd, x, y, w, h, repaint)` → shadow SSDT 0x1053: wraps `wm_move_window` + `wm_resize_window`
- [ ] `NtUserSetWindowPos(hwnd, hwndInsertAfter, x, y, cx, cy, flags)` → shadow SSDT 0x1054: Z-order + position
- [ ] `NtUserGetClientRect(hwnd, rect)` → shadow SSDT 0x1055
- [ ] `NtUserGetWindowRect(hwnd, rect)` → shadow SSDT 0x1056
- [ ] `NtUserSetWindowText(hwnd, string)` → shadow SSDT 0x1057: wraps `wm_set_title`
- [ ] `NtUserGetWindowText(hwnd, buffer, maxCount)` → shadow SSDT 0x1058
- [ ] `NtUserInvalidateRect(hwnd, rect, erase)` → shadow SSDT 0x1059: wraps `wm_mark_dirty`
- [ ] `NtUserBeginPaint(hwnd, paintStruct)` → shadow SSDT 0x105A: get DC + validate
- [ ] `NtUserEndPaint(hwnd, paintStruct)` → shadow SSDT 0x105B: release paint DC
- [ ] `NtUserSetFocus(hwnd)` → shadow SSDT 0x105C: wraps `wm_focus_window`
- [ ] `NtUserGetFocus()` → shadow SSDT 0x105D
- [ ] `NtUserIsWindow(hwnd)` → shadow SSDT 0x105E
- [ ] `NtUserIsWindowVisible(hwnd)` → shadow SSDT 0x105F
- [ ] Commit: `"kernel: win32k -- USER window management syscalls (CreateWindowEx through IsWindowVisible)"`

**Test checkpoint:** `NtUserCreateWindowEx` returns valid HWND. `NtUserShowWindow(SW_SHOW)` makes window visible. `NtUserBeginPaint` + `NtGdiRectangle` + `NtUserEndPaint` draws on window. `NtUserDestroyWindow` removes it.

## 8. USER Message Queue Syscalls
Windows message loop -- the heart of Win32 UI. Wraps the kernel message queue infrastructure.

- [ ] `NtUserGetMessage(msg, hwnd, msgFilterMin, msgFilterMax)` → shadow SSDT 0x1060: block until message available (migrates SYS_GETMESSAGE=74)
- [ ] `NtUserPeekMessage(msg, hwnd, msgFilterMin, msgFilterMax, removeMsg)` → shadow SSDT 0x1061: non-blocking check
- [ ] `NtUserTranslateMessage(msg)` → shadow SSDT 0x1062: key-down → WM_CHAR translation
- [ ] `NtUserDispatchMessage(msg)` → shadow SSDT 0x1063: call registered WNDPROC
- [ ] `NtUserPostMessage(hwnd, msg, wParam, lParam)` → shadow SSDT 0x1064: async post to queue
- [ ] `NtUserSendMessage(hwnd, msg, wParam, lParam)` → shadow SSDT 0x1065: sync send (cross-thread marshalling)
- [ ] `NtUserWaitMessage()` → shadow SSDT 0x1066: block until any message (migrates SYS_WAIT_MESSAGE=75)
- [ ] `NtUserRegisterClassEx(wndclass)` → shadow SSDT 0x1067: register window class (migrates SYS_REGISTERCLASS=76)
- [ ] `NtUserFindWindow(className, windowName)` → shadow SSDT 0x1068: find by class/title (migrates SYS_FINDWINDOW=77)
- [ ] `NtUserPostQuitMessage(exitCode)` → shadow SSDT 0x1069
- [ ] `NtUserDefWindowProc(hwnd, msg, wParam, lParam)` → shadow SSDT 0x106A: default message handling
- [ ] `NtUserSetTimer(hwnd, idEvent, elapse, timerFunc)` → shadow SSDT 0x106B: WM_TIMER messages
- [ ] `NtUserKillTimer(hwnd, idEvent)` → shadow SSDT 0x106C
- [ ] Commit: `"kernel: win32k -- USER message queue syscalls (GetMessage through KillTimer)"`

**Test checkpoint:** `NtUserRegisterClassEx` + `NtUserCreateWindowEx` + `NtUserGetMessage` loop processes WM_PAINT. `NtUserPostMessage(WM_QUIT)` exits the loop. `NtUserSendMessage` cross-thread delivers and returns result. `NtUserSetTimer` fires WM_TIMER at correct interval.

## 9. USER Input and Cursor Syscalls
Keyboard and mouse input, cursor management.

- [ ] `NtUserGetKeyState(vKey)` → shadow SSDT 0x1070
- [ ] `NtUserGetAsyncKeyState(vKey)` → shadow SSDT 0x1071
- [ ] `NtUserGetCursorPos(point)` → shadow SSDT 0x1072
- [ ] `NtUserSetCursorPos(x, y)` → shadow SSDT 0x1073
- [ ] `NtUserSetCursor(hCursor)` → shadow SSDT 0x1074: wraps `cursor_set_shape`
- [ ] `NtUserLoadCursor(hInstance, cursorName)` → shadow SSDT 0x1075
- [ ] `NtUserShowCursor(show)` → shadow SSDT 0x1076
- [ ] `NtUserGetCapture()` / `NtUserSetCapture(hwnd)` → shadow SSDT 0x1077/0x1078
- [ ] `NtUserReleaseCapture()` → shadow SSDT 0x1079
- [ ] `NtUserLoadIcon(hInstance, iconName)` → shadow SSDT 0x107A: wraps `icon_get`
- [ ] `NtUserGetSystemMetrics(index)` → shadow SSDT 0x107B: SM_CXSCREEN, SM_CYSCREEN, SM_CXICON, etc.
- [ ] Commit: `"kernel: win32k -- USER input and cursor syscalls"`

**Test checkpoint:** `NtUserGetCursorPos` returns current mouse position. `NtUserSetCursor` changes cursor shape. `NtUserGetKeyState(VK_SHIFT)` returns correct state. `NtUserGetSystemMetrics(SM_CXSCREEN)` returns framebuffer width.

## 10. USER Menu and Accelerator Syscalls
Menu creation and keyboard accelerators.

- [ ] `NtUserCreateMenu()` → shadow SSDT 0x1080
- [ ] `NtUserCreatePopupMenu()` → shadow SSDT 0x1081
- [ ] `NtUserAppendMenu(hMenu, flags, idNewItem, newItem)` → shadow SSDT 0x1082
- [ ] `NtUserInsertMenuItem(hMenu, item, fByPosition, mii)` → shadow SSDT 0x1083
- [ ] `NtUserSetMenu(hwnd, hMenu)` → shadow SSDT 0x1084
- [ ] `NtUserDestroyMenu(hMenu)` → shadow SSDT 0x1085
- [ ] `NtUserTrackPopupMenu(hMenu, flags, x, y, reserved, hwnd, rect)` → shadow SSDT 0x1086
- [ ] `NtUserCreateAcceleratorTable(accel, count)` → shadow SSDT 0x1087
- [ ] `NtUserTranslateAccelerator(hwnd, hAccTable, msg)` → shadow SSDT 0x1088
- [ ] `NtUserDestroyAcceleratorTable(hAccTable)` → shadow SSDT 0x1089
- [ ] Commit: `"kernel: win32k -- USER menu and accelerator syscalls"`

**Test checkpoint:** `NtUserCreateMenu` + `NtUserAppendMenu` + `NtUserSetMenu` shows menu bar on window. `NtUserTrackPopupMenu` displays context menu at cursor. `NtUserCreateAcceleratorTable` with Ctrl+S fires WM_COMMAND.

## 11. USER Clipboard Syscalls
Clipboard data exchange between applications.

- [ ] `NtUserOpenClipboard(hwnd)` → shadow SSDT 0x1090
- [ ] `NtUserCloseClipboard()` → shadow SSDT 0x1091
- [ ] `NtUserEmptyClipboard()` → shadow SSDT 0x1092
- [ ] `NtUserSetClipboardData(format, hMem)` → shadow SSDT 0x1093
- [ ] `NtUserGetClipboardData(format)` → shadow SSDT 0x1094
- [ ] `NtUserIsClipboardFormatAvailable(format)` → shadow SSDT 0x1095
- [ ] `NtUserCountClipboardFormats()` → shadow SSDT 0x1096
- [ ] `NtUserEnumClipboardFormats(format)` → shadow SSDT 0x1097
- [ ] `NtUserRegisterClipboardFormat(formatName)` → shadow SSDT 0x1098
- [ ] `NtUserAddClipboardFormatListener(hwnd)` → shadow SSDT 0x1099
- [ ] Commit: `"kernel: win32k -- USER clipboard syscalls"`

**Test checkpoint:** `NtUserOpenClipboard` + `NtUserSetClipboardData(CF_TEXT, "hello")` + `NtUserCloseClipboard` stores text. Another process `NtUserGetClipboardData(CF_TEXT)` retrieves "hello". `NtUserAddClipboardFormatListener` receives WM_CLIPBOARDUPDATE.

## 12. Migrate Legacy SYS_* to Shadow SSDT
Migrate the 4 existing Win32 UI syscalls from the main SSDT placeholder to proper shadow SSDT entries.

- [ ] Remove `SYS_GETMESSAGE=74` from main SSDT; replace with `NtUserGetMessage` → shadow SSDT 0x1060
- [ ] Remove `SYS_WAIT_MESSAGE=75` from main SSDT; replace with `NtUserWaitMessage` → shadow SSDT 0x1066
- [ ] Remove `SYS_REGISTERCLASS=76` from main SSDT; replace with `NtUserRegisterClassEx` → shadow SSDT 0x1067
- [ ] Remove `SYS_FINDWINDOW=77` from main SSDT; replace with `NtUserFindWindow` → shadow SSDT 0x1068
- [ ] Update `12-user-platform-sdk/TODO-05-win32-subsystem.md` to reference shadow SSDT indices
- [ ] Keep `SYS_*` macros as aliases for transition period
- [ ] Commit: `"kernel: win32k -- migrate SYS_GETMESSAGE/WAIT_MESSAGE/REGISTERCLASS/FINDWINDOW to shadow SSDT"`

**Test checkpoint:** Existing desktop shell still works after migration. `syscall` with RAX=0x1060 reaches `NtUserGetMessage`. Old `SYS_GETMESSAGE=74` alias still works during transition.

## 13. GDI Path, Curve, and Extended Drawing
Path operations (BeginPath/EndPath/StrokePath), arcs, Bezier curves, gradient fills, alpha blending, and flood fills. Wraps `gfx_*` primitives with GDI path state tracking.

- [ ] Implement path state machine in `GDI_DC`: `BeginPath` starts recording, `EndPath` closes, `StrokePath`/`FillPath` renders
- [ ] Implement arc/chord/pie using trigonometric approximation on `gfx_surface_t`
- [ ] Implement Bezier curve subdivision (de Casteljau algorithm) for `PolyBezier`/`PolyBezierTo`
- [ ] `NtGdiGradientFill` (0x10C0): horizontal/vertical/triangle gradient using per-scanline color interpolation
- [ ] `NtGdiAlphaBlend` (0x10C1): wraps `gfx_blit_alpha` with per-pixel alpha from source DC
- [ ] `NtGdiTransparentBlt` (0x10C2): blit with color key transparency
- [ ] `NtGdiRoundRect` (0x10B7): rectangle with rounded corners
- [ ] `NtGdiSetROP2` / `NtGdiGetROP2`: raster operation mode (R2_COPYPEN, R2_XORPEN, etc.)
- [ ] Commit: `"kernel: win32k -- GDI path, curve, and extended drawing (0x10A0–0x10CF)"`

**Test checkpoint:** `NtGdiBeginPath` + `NtGdiMoveTo` + `NtGdiLineTo` × 3 + `NtGdiCloseFigure` + `NtGdiEndPath` + `NtGdiStrokePath` draws a triangle. `NtGdiGradientFill` produces visible gradient. `NtGdiAlphaBlend` produces translucent overlay.

## 14. GDI Transform, Palette, and Color Management
Coordinate transforms (viewport/window origin, world transform matrix), color palettes (256-color legacy), ICM color management.

- [ ] Implement 3×2 world transform matrix in `GDI_DC` for `SetWorldTransform`/`ModifyWorldTransform`
- [ ] `DPtoLP`/`LPtoDP`: device-to-logical and logical-to-device point conversion using current map mode
- [ ] Implement palette object: 256-entry `PALETTEENTRY` array for legacy 8-bit color support
- [ ] ICM stubs: `SetICMMode`/`SetICMProfile` -- store mode flag, actual color matching deferred
- [ ] `NtGdiSetLayout` (0x10FF): RTL mirroring for bidirectional text support
- [ ] Commit: `"kernel: win32k -- GDI transform, palette, and color management (0x10D0–0x10FF)"`

**Test checkpoint:** `NtGdiSetWorldTransform` with rotation matrix + `NtGdiLineTo` draws rotated line. `NtGdiCreatePalette` + `NtGdiSelectPalette` + `NtGdiRealizePalette` activates palette.

## 15. GDI Print and Metafile
Printer DC, document lifecycle (StartDoc/EndDoc/StartPage/EndPage), Enhanced Metafile recording and playback. Print spooler integration deferred.

- [ ] Implement printer DC stub: `NtGdiCreateDC("DISPLAY")` returns screen DC; printer names return `STATUS_NOT_SUPPORTED` until print spooler exists
- [ ] `NtGdiGetDeviceCaps` (0x1109): return framebuffer capabilities (HORZRES, VERTRES, BITSPIXEL, PLANES, etc.)
- [ ] Implement Enhanced Metafile as in-memory GDI command list: `CreateEnhMetaFile` records, `CloseEnhMetaFile` returns handle, `PlayEnhMetaFile` replays
- [ ] `NtGdiGdiComment` (0x1119): embed application data in metafile stream
- [ ] Commit: `"kernel: win32k -- GDI print and metafile (0x1100–0x1119)"`

**Test checkpoint:** `NtGdiGetDeviceCaps(HORZRES)` returns framebuffer width. `NtGdiCreateEnhMetaFile` + draw operations + `NtGdiCloseEnhMetaFile` + `NtGdiPlayEnhMetaFile` replays correctly.

## 16. GDI Font Advanced
Font enumeration, glyph metrics, kerning, character placement, and font resource management. Wraps `ttf_*` with Win32-compatible metric structures.

- [ ] `NtGdiGetFontData` (0x1130): read raw TrueType table data from loaded font
- [ ] `NtGdiGetGlyphOutline` (0x1131): return glyph bezier curves and bitmap for individual characters
- [ ] `NtGdiGetCharABCWidths` (0x1136): A (left bearing), B (width), C (right bearing) per character
- [ ] `NtGdiGetKerningPairs` (0x1138): return kern table pairs from loaded TrueType font
- [ ] `NtGdiEnumFontFamiliesEx` (0x113E): enumerate installed font families with charset/pitch filtering
- [ ] `NtGdiAddFontResource` / `NtGdiRemoveFontResource`: dynamic font install/uninstall
- [ ] `NtGdiAddFontMemResourceEx` (0x1144): load font from memory buffer (PE resource embedding)
- [ ] `NtGdiGetCharacterPlacement` (0x114A): complex script glyph shaping (basic LTR; bidi deferred)
- [ ] Commit: `"kernel: win32k -- GDI font advanced (0x1130–0x1151)"`

**Test checkpoint:** `NtGdiEnumFontFamiliesEx` returns at least 1 font family. `NtGdiGetCharABCWidths('A')` returns non-zero B width. `NtGdiAddFontMemResourceEx` loads embedded font; `NtGdiTextOut` renders with it.

## 17. GDI Extended Object Management
Extended pens (geometric with join/cap styles), indirect brush creation, bitmap dimension control, region arithmetic extensions.

- [ ] `NtGdiExtCreatePen` (0x1160): geometric pen with PS_JOIN_ROUND/BEVEL/MITER and PS_ENDCAP_ROUND/SQUARE/FLAT
- [ ] `NtGdiCreateBrushIndirect` (0x1161): create brush from `LOGBRUSH` structure
- [ ] Region constructors: `CreateRoundRectRgn`, `CreateEllipticRgn`, `CreatePolygonRgn`
- [ ] `NtGdiGetRegionData` (0x1178): return region as array of rectangles
- [ ] `NtGdiGetBitmapBits` / `NtGdiSetBitmapBits`: raw pixel access for DDB bitmaps
- [ ] Commit: `"kernel: win32k -- GDI extended object management (0x1160–0x117F)"`

**Test checkpoint:** `NtGdiExtCreatePen` with PS_JOIN_ROUND + `NtGdiPolyline` draws smooth-joined polyline. `NtGdiCreatePolygonRgn` + `NtGdiPtInRegion` correctly tests point containment.

## 18. USER Window Properties, Styles, and Enumeration
Window long values (GWL_STYLE, GWL_EXSTYLE), class properties, per-window user data, window hierarchy traversal, coordinate mapping, layered windows.

- [ ] `NtUserGetWindowLong/SetWindowLong` (0x1180-0x1183): read/write GWL_STYLE, GWL_EXSTYLE, GWLP_WNDPROC, GWLP_USERDATA
- [ ] `NtUserGet/SetProp` (0x1189-0x118C): per-window property list (string-keyed void* storage)
- [ ] `NtUserEnumWindows` / `NtUserEnumChildWindows` (0x1199-0x119A): iterate window hierarchy
- [ ] `NtUserWindowFromPoint` / `NtUserChildWindowFromPoint` (0x11A6-0x11A9): hit testing
- [ ] `NtUserMapWindowPoints` / `NtUserScreenToClient` / `NtUserClientToScreen` (0x11AA-0x11AC): coordinate conversion
- [ ] `NtUserSetLayeredWindowAttributes` (0x11BB): per-window alpha + color key for translucent windows
- [ ] `NtUserScrollWindowEx` (0x11B9): scroll window content with built-in invalidation
- [ ] `NtUserFlashWindowEx` (0x11BF): taskbar flash notification
- [ ] `NtUserAnimateWindow` (0x11C0): show/hide with animation (slide, fade, roll)
- [ ] Commit: `"kernel: win32k -- USER window properties, styles, enumeration (0x1180–0x11CD)"`

**Test checkpoint:** `NtUserSetWindowLong(GWL_STYLE)` changes window style. `NtUserEnumWindows` iterates all top-level windows. `NtUserWindowFromPoint` returns correct HWND at cursor. `NtUserSetLayeredWindowAttributes(alpha=128)` makes window translucent.

## 19. USER Dialog, Caret, and Drawing Helpers
Dialog box creation/management, caret (text cursor), and drawing utility functions (DrawEdge, DrawFrameControl, DrawText).

- [ ] `NtUserCreateDialogParam` / `NtUserDialogBoxParam` (0x11D0-0x11D4): create modeless/modal dialogs from template
- [ ] `NtUserGetDlgItem` / `NtUserSetDlgItemText` (0x11D6-0x11DB): dialog control access
- [ ] `NtUserCreateCaret` / `NtUserShowCaret` / `NtUserSetCaretPos` (0x11E7-0x11EE): text caret with blink timer
- [ ] `NtUserDrawEdge` / `NtUserDrawFrameControl` (0x11F6-0x11F7): 3D edge and frame control rendering
- [ ] `NtUserDrawCaption` (0x11F4): render window title bar
- [ ] Scrollbar: `NtUserSetScrollInfo` / `NtUserGetScrollInfo` (0x11FE-0x1207)
- [ ] Commit: `"kernel: win32k -- USER dialog, caret, and drawing helpers (0x11D0–0x1207)"`

**Test checkpoint:** `NtUserDialogBoxParam` shows modal dialog; `NtUserEndDialog` closes it. `NtUserCreateCaret` + `NtUserShowCaret` shows blinking caret in focused window. `NtUserSetScrollInfo` updates scrollbar thumb position.

## 20. USER Scrollbar
Scrollbar control syscalls -- dedicated section because scrollbar is a core UI primitive.

> [!NOTE]
> Scrollbar entries 0x11FE–0x1207 are listed in the §19 Dialog table but implemented here as a distinct unit.

- [ ] `NtUserSetScrollInfo(hwnd, fnBar, lpsi, fRedraw)`: set scrollbar range, page, position
- [ ] `NtUserGetScrollInfo(hwnd, fnBar, lpsi)`: get current scrollbar state
- [ ] `NtUserShowScrollBar(hwnd, wBar, bShow)`: show/hide horizontal, vertical, or both
- [ ] `NtUserEnableScrollBar(hwnd, wSBflags, wArrows)`: enable/disable scrollbar arrows
- [ ] Commit: `"kernel: win32k -- USER scrollbar (0x11FE–0x1207)"`

**Test checkpoint:** Window with `WS_VSCROLL` style shows vertical scrollbar. `NtUserSetScrollInfo` changes thumb position. `NtUserGetScrollInfo` returns correct range/page/pos.

## 21. USER Keyboard, IME, and Hook
Keyboard state, virtual key mapping, input method (IME) context, window hooks (WH_KEYBOARD, WH_MOUSE, WH_CBT, etc.), hotkey registration.

- [ ] `NtUserGetKeyboardState` / `NtUserSetKeyboardState` (0x1210-0x1211): full 256-key state array
- [ ] `NtUserMapVirtualKey` / `NtUserToUnicode` (0x1217-0x121A): virtual key to scan code / Unicode conversion
- [ ] `NtUserSendInput` (0x121F): inject keyboard/mouse events (accessibility, testing)
- [ ] `NtUserSetWindowsHookEx` / `NtUserCallNextHookEx` (0x1223-0x1225): hook chain for WH_KEYBOARD_LL, WH_MOUSE_LL, WH_CBT, WH_SHELL
- [ ] `NtUserRegisterHotKey` / `NtUserUnregisterHotKey` (0x1229-0x122A): system-wide hotkeys
- [ ] IME context management: `NtUserImmGetContext` / `NtUserImmReleaseContext` (0x122C-0x122D) + composition window/string
- [ ] Commit: `"kernel: win32k -- USER keyboard, IME, and hook (0x1210–0x123D)"`

**Test checkpoint:** `NtUserGetKeyboardState` returns current key state array. `NtUserSetWindowsHookEx(WH_KEYBOARD_LL)` intercepts keystrokes. `NtUserRegisterHotKey(MOD_CONTROL, 'S')` fires WM_HOTKEY.

## 22. USER DPI, Accessibility, and System Parameters
DPI awareness, system-wide parameters (SystemParametersInfo), color scheme, input timing, message queue state.

- [ ] `NtUserSystemParametersInfo` (0x1240): SPI_GETWORKAREA, SPI_GETNONCLIENTMETRICS, SPI_GETDRAGFULLWINDOWS, SPI_GETBEEP, etc.
- [ ] `NtUserGetDpiForWindow` / `NtUserSetProcessDpiAwareness` (0x1241-0x124D): per-monitor DPI awareness
- [ ] `NtUserGetSysColor` / `NtUserSetSysColors` / `NtUserGetSysColorBrush` (0x1257-0x1259): system color scheme
- [ ] `NtUserMsgWaitForMultipleObjects` (0x1262): wait for kernel objects OR messages (Win32 modal loop pattern)
- [ ] `NtUserGetMessagePos` / `NtUserGetMessageTime` (0x1266-0x1267): cursor position and timestamp of last message
- [ ] Commit: `"kernel: win32k -- USER DPI, accessibility, and system parameters (0x1240–0x1267)"`

**Test checkpoint:** `NtUserSystemParametersInfo(SPI_GETWORKAREA)` returns desktop rect minus taskbar. `NtUserGetDpiForWindow` returns 96 (100%) or scaled value. `NtUserGetSysColor(COLOR_WINDOW)` returns theme-appropriate color.

## 23. USER Raw Input, Touch, and Gesture
Raw HID input, multi-touch, pointer input, gesture recognition, deferred window positioning.

- [ ] `NtUserRegisterRawInputDevices` / `NtUserGetRawInputData` (0x1270-0x1276): raw keyboard/mouse/HID without message processing
- [ ] `NtUserRegisterTouchWindow` / `NtUserGetTouchInputInfo` (0x1277-0x127B): multi-touch events
- [ ] `NtUserGetPointerInfo` / `NtUserGetPointerType` (0x127D-0x1282): unified pointer model (Win8+)
- [ ] `NtUserSetGestureConfig` / `NtUserGetGestureInfo` (0x1283-0x1287): pinch/zoom/rotate/pan gestures
- [ ] `NtUserBeginDeferWindowPos` / `NtUserDeferWindowPos` / `NtUserEndDeferWindowPos` (0x1288-0x128A): batch window positioning
- [ ] Commit: `"kernel: win32k -- USER raw input, touch, and gesture (0x1270–0x128A)"`

**Test checkpoint:** `NtUserRegisterRawInputDevices(MOUSE)` + `NtUserGetRawInputData` receives raw mouse delta. `NtUserBeginDeferWindowPos(4)` + 4× `NtUserDeferWindowPos` + `NtUserEndDeferWindowPos` moves 4 windows atomically.

## 24. USER Multi-Monitor and Display
Monitor enumeration, display mode changes, gamma ramp, display configuration.

- [ ] `NtUserEnumDisplayMonitors` / `NtUserGetMonitorInfo` (0x1290-0x1291): enumerate active monitors
- [ ] `NtUserMonitorFromWindow` / `NtUserMonitorFromPoint` (0x1292-0x1294): which monitor owns a window/point
- [ ] `NtUserEnumDisplaySettings` / `NtUserChangeDisplaySettings` (0x1296-0x1299): resolution/refresh rate changes
- [ ] `NtGdiGetDeviceGammaRamp` / `NtGdiSetDeviceGammaRamp` (0x129B-0x129C): night light color temperature
- [ ] `NtUserSetDisplayConfig` / `NtUserQueryDisplayConfig` (0x129D-0x129F): Windows CCD display topology
- [ ] Commit: `"kernel: win32k -- USER multi-monitor and display (0x1290–0x129F)"`

**Test checkpoint:** `NtUserEnumDisplayMonitors` returns at least 1 monitor. `NtUserGetMonitorInfo` returns correct work area. `NtGdiSetDeviceGammaRamp` shifts display to warm tones.

## 25. USER Shell Integration
Shell hook windows, broadcast system messages, message timeouts, power/suspend notifications, message filtering.

- [ ] `NtUserRegisterShellHookWindow` (0x12A0): receive shell events (window create/destroy/activate)
- [ ] `NtUserBroadcastSystemMessage` (0x12A9): send to all top-level windows (WM_SETTINGCHANGE, WM_THEMECHANGED)
- [ ] `NtUserSendMessageTimeout` (0x12AB): send with timeout to prevent hung-app hangs
- [ ] `NtUserSetCoalescableTimer` (0x12AF): power-efficient timer with coalescing tolerance
- [ ] `NtUserRegisterPowerSettingNotification` (0x12B0): receive power state change notifications
- [ ] `NtUserChangeWindowMessageFilterEx` (0x12B5): UIPI message filter for UAC elevation scenarios
- [ ] Commit: `"kernel: win32k -- USER shell integration (0x12A0–0x12B5)"`

**Test checkpoint:** `NtUserRegisterShellHookWindow` receives HSHELL_WINDOWCREATED when new window opens. `NtUserBroadcastSystemMessage(WM_SETTINGCHANGE)` reaches all top-level windows. `NtUserSendMessageTimeout` returns `STATUS_TIMEOUT` for hung window.

## 26. GDI/USER DirectX and DXGI Kernel Integration
Direct3D kernel thunks (DXGK) -- the kernel-mode interface between user-mode DirectX runtime and the GPU driver. Wraps WDDM display miniport driver calls.

> [!NOTE]
> These are co-owned with `04-drivers-hardware/TODO-17-gpu-display-drivers.md`. The shadow SSDT dispatch is in this TODO; the GPU driver backend is in TODO-08-gpu.

- [ ] `NtGdiDdDDICreateDevice` / `NtGdiDdDDIDestroyDevice` (0x12C0-0x12C1): create/destroy D3D device context
- [ ] `NtGdiDdDDIPresent` (0x12C4): submit frame for display (swap chain flip/blit)
- [ ] `NtGdiDdDDIRender` (0x12C5): submit command buffer to GPU
- [ ] `NtGdiDdDDICreateAllocation` / `NtGdiDdDDIDestroyAllocation` (0x12C2-0x12C3): GPU memory management
- [ ] `NtGdiDdDDIOpenAdapterFromHdc` (0x12C6): get GPU adapter from display DC
- [ ] `NtGdiDdDDIQueryAdapterInfo` (0x12C8): query GPU capabilities (VRAM, shader model, etc.)
- [ ] Synchronization: `CreateSynchronizationObject`, `WaitForSync`, `SignalSync` (0x12CB-0x12CD): GPU fence/event objects
- [ ] Overlay: `CreateOverlay`, `FlipOverlay`, `UpdateOverlay` (0x12E0-0x12E3): hardware overlay planes
- [ ] Swap chain: `NtGdiDdDDICreateSwapChain` (0x12E4): direct flip swap chain for full-screen rendering
- [ ] Output duplication: `NtGdiDdDDIOutputDuplPresent` (0x12E9): desktop duplication API for screen capture
- [ ] Commit: `"kernel: win32k -- GDI/USER DirectX and DXGI kernel integration (0x12C0–0x12F4)"`

**Test checkpoint:** `NtGdiDdDDIEnumAdapters` returns at least 1 adapter (software rasterizer). `NtGdiDdDDICreateDevice` returns valid device handle. `NtGdiDdDDIQueryAdapterInfo` returns framebuffer size.

## 27. Impossible OS Exclusive Graphics Extensions
Desktop compositor stats, Acrylic/Mica effects, virtual desktops, snap layouts, taskbar control, toast notifications, theme management, font cache, wallpaper, screen capture -- all as first-class kernel APIs.

> [!IMPORTANT]
> **These have NO Windows or Linux equivalent as kernel syscalls.** Windows exposes these through COM/UWP APIs; Linux has no unified equivalent. Impossible OS makes them zero-overhead kernel calls.

- [ ] Compositor: `NtGdiQueryCompositorStats` (0x1300): frame rate, dirty rect count, surface memory usage
- [ ] Effects: `NtGdiSetAcrylicBlur` / `NtGdiSetMicaEffect` (0x1307-0x1308): per-window material effects
- [ ] Night light: `NtGdiSetNightLightStrength` / `NtGdiGetNightLightStrength` (0x130B-0x130C)
- [ ] Virtual desktops: `NtUserCreateVirtualDesktop` / `NtUserSwitchVirtualDesktop` / `NtUserMoveWindowToDesktop` (0x1310-0x1314)
- [ ] Snap layouts: `NtUserQuerySnapLayout` / `NtUserSetSnapLayout` (0x1315-0x1316)
- [ ] Taskbar: `NtUserSetTaskbarProgress` / `NtUserSetTaskbarOverlayIcon` / `NtUserFlashTaskbarEntry` (0x1318-0x131A)
- [ ] Thumbnails: `NtUserRegisterThumbnail` / `NtUserQueryWindowThumbnail` (0x131B-0x131D)
- [ ] Toasts: `NtUserSendToast` / `NtUserDismissToast` / `NtUserQueryToastHistory` (0x131F-0x1321)
- [ ] Theme: `NtUserSetThemeMode` / `NtUserSetAccentColor` (0x1323-0x1324)
- [ ] Font: `NtGdiPreloadFont` / `NtGdiFlushFontCache` (0x1328-0x1329)
- [ ] Screen capture: `NtGdiScreenCapture` / `NtGdiWindowCapture` (0x132F-0x1330)
- [ ] Start menu: `NtUserQueryStartMenuPins` / `NtUserSetStartMenuPin` (0x1331-0x1333)
- [ ] Jump lists: `NtUserQueryJumpList` / `NtUserAddJumpListItem` (0x1334-0x1336)
- [ ] Commit: `"kernel: win32k -- Impossible OS exclusive graphics extensions (0x1300–0x1336)"`

**Test checkpoint:** `NtGdiQueryCompositorStats` returns valid frame rate. `NtGdiSetAcrylicBlur(hwnd, 20)` makes window background blurry. `NtUserCreateVirtualDesktop` creates desktop 2; `NtUserSwitchVirtualDesktop(2)` switches to it. `NtUserSendToast("Hello")` shows notification.

---

## Shadow SSDT slot registry

Canonical **1300-row** Win32k shadow SSDT index table: [`TODO-A-Win32k-Shadow-SSDT-Master-Table.md`](TODO-A-Win32k-Shadow-SSDT-Master-Table.md). When you allocate or rename a service number, update that file first, then implement the handler in the owning section of this TODO.

Shadow Table 1 routing and syscall contract (parallel to kernel `TODO-05`): [`TODO-16-win32k-shadow-native-api.md`](TODO-16-win32k-shadow-native-api.md).

## OS Comparison

| ⭐   | Feature                    | 🪟 Win11                         | 🐧 Linux                      | 🚀 Impossible OS                |
| --- | -------------------------- | ------------------------------- | ---------------------------- | ------------------------------ |
| 💎   | Kernel GDI dispatch        | ✅ win32k.sys NtGdiXxx           | ❌ No kernel GDI (Mesa UMD)   | ⬜ §2–§6,§13–§17 -- GDI entries |
| 💎   | Kernel USER dispatch       | ✅ win32k.sys NtUserXxx          | ❌ No kernel USER (Wayland)   | ⬜ §7–§12,§18–§25 -- USER       |
| 💎   | Shadow SSDT (Table 1)      | ✅ ~1300 entries                 | ❌ No SSDT concept            | ⬜ §1 + `TODO-A` -- 1300 slots  |
| 💎   | DC-based drawing model     | ✅ HDC + GDI objects             | ❌ Direct framebuffer/Vulkan  | ⬜ §2 -- DC wraps gfx_surface   |
| 💎   | Message queue syscalls     | ✅ NtUserGetMessage              | ❌ Wayland fd polling         | ⬜ §8 -- kernel msg queue       |
| 💎   | Clipboard syscalls         | ✅ NtUserGet/SetClipboardData    | ❌ Wayland clipboard protocol | ⬜ §11 -- kernel clipboard      |
| 💎   | Menu/accelerator syscalls  | ✅ NtUserCreateMenu              | ❌ Toolkit-level only         | ⬜ §10 -- kernel menus          |
| ⭐   | Unified kernel GDI+USER    | ⚠️ win32k.sys (legacy monolith) | ❌ No equivalent              | ⬜ Clean modular impl           |
| 💎   | GDI path/curve ops         | ✅ BeginPath/EndPath             | ❌ Cairo (userspace)          | ⬜ §13 -- kernel paths          |
| 💎   | GDI transforms             | ✅ WorldTransform matrix         | ❌ Cairo matrix               | ⬜ §14 -- kernel transforms     |
| 💎   | GDI print/metafile         | ✅ Print spooler + EMF           | ✅ CUPS/PostScript            | ⬜ §15 -- EMF record/playback   |
| 💎   | Font enumeration           | ✅ EnumFontFamiliesEx            | ✅ fontconfig                 | ⬜ §16 -- kernel font enum      |
| 💎   | Dialog boxes               | ✅ DialogBox/EndDialog           | ❌ Toolkit-level only         | ⬜ §19 -- kernel dialogs        |
| 💎   | Keyboard hooks             | ✅ WH_KEYBOARD_LL                | ✅ XInput/libinput            | ⬜ §21 -- kernel hook chain     |
| 💎   | DPI awareness              | ✅ Per-Monitor DPI v2            | ⚠️ Wayland basic             | ⬜ §22 -- per-monitor DPI       |
| 💎   | Multi-touch/gesture        | ✅ WM_TOUCH/WM_GESTURE           | ✅ libinput gestures          | ⬜ §23 -- kernel touch/gesture  |
| 💎   | Multi-monitor              | ✅ EnumDisplayMonitors           | ✅ xrandr/wlr-output          | ⬜ §24 -- kernel monitor enum   |
| 💎   | DXGI/DirectX kernel thunks | ✅ DXGK ~50 calls                | ❌ Mesa/DRM userspace         | ⬜ §26 -- DXGK dispatch         |
| ⭐   | Direct gfx_* wrapping      | ❌ GDI → DirectX translation     | ❌ Mesa userspace             | ⬜ Zero-overhead kernel path    |
| ⭐   | Virtual desktop syscalls   | ❌ COM API only                  | ❌ No standard                | ⬜ §27 -- kernel VD control     |
| ⭐   | Compositor stats syscall   | ❌ No public API                 | ❌ No equivalent              | ⬜ §27 -- NtGdiQueryStats       |
| ⭐   | Acrylic/Mica as syscall    | ❌ DWM internal only             | ❌ No equivalent              | ⬜ §27 -- NtGdiSetAcrylicBlur   |
| ⭐   | Toast notifications kernel | ❌ COM/UWP only                  | ❌ D-Bus notify               | ⬜ §27 -- NtUserSendToast       |

> **After §1–§27:** Full Windows 11 win32k.sys parity (1300 shadow SSDT entries) plus 55 Impossible OS exclusive graphics extensions. Win32 PE applications call GDI32/USER32 via shadow SSDT syscalls wrapping the compositor/gfx/font primitives directly. No translation layer.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_win32k()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_win32k.c` with:
  - Shadow SSDT dispatch: `syscall(0x1000)` routes to `NtGdiCreateCompatibleDC`; `syscall(0xFFFF)` returns `STATUS_NOT_IMPLEMENTED`
  - `NtGdiGetDC(hwnd)` returns valid HDC; `NtGdiReleaseDC` succeeds
  - `NtGdiCreatePen(PS_SOLID, 1, 0xFF0000)` returns valid HPEN; `NtGdiDeleteObjectApp` frees it
  - `NtGdiSelectObject` into DC changes active pen
  - `NtGdiRectangle` draws on window surface (verify pixel at expected position)
  - `NtGdiTextOut` renders text string (verify non-zero pixels in text area)
  - `NtGdiCreateCompatibleBitmap` returns valid HBITMAP with correct dimensions
  - `NtUserCreateWindowEx` returns valid HWND; `NtUserIsWindow` returns TRUE; `NtUserDestroyWindow` returns TRUE
  - `NtUserPostMessage(WM_USER)` + `NtUserPeekMessage` retrieves it
  - `NtUserGetSystemMetrics(SM_CXSCREEN)` returns framebuffer width
  - `NtUserOpenClipboard` + `NtUserSetClipboardData(CF_TEXT)` + `NtUserCloseClipboard` + `NtUserGetClipboardData` round-trip
- [ ] Register in `test_runner_init()`: `test_register_win32k()`
- [ ] Commit: `"test: add Win32k shadow SSDT test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Serial log: `"win32k: shadow SSDT registered, 1300 entries"` during Phase 3
- [ ] `syscall` with RAX=0x1003 (NtGdiGetDC) from ring 3 reaches shadow SSDT handler
- [ ] `NtGdiGetDC` + `NtGdiRectangle` + `NtGdiReleaseDC` draws visible rectangle on screen
- [ ] `NtGdiCreateFont` + `NtGdiTextOut` renders text on window
- [ ] `NtUserCreateWindowEx` + `NtUserShowWindow` creates visible window
- [ ] `NtUserGetMessage` loop processes WM_PAINT, WM_TIMER, WM_QUIT correctly
- [ ] `NtUserPostMessage` cross-thread delivers message to correct window
- [ ] Clipboard round-trip: set CF_TEXT in one process, get in another
- [ ] Legacy `SYS_GETMESSAGE=74` still works during transition
- [ ] Shadow SSDT slot for unimplemented index returns `STATUS_NOT_IMPLEMENTED`
- [ ] All 4 platforms: QEMU WHPX, QEMU TCG, VirtualBox, bare metal
- [ ] Commit: `"kernel: win32k -- shadow SSDT complete (1300 entries)"`
