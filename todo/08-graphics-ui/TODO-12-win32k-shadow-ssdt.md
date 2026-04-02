# TODO-12 — Win32k Shadow SSDT (NtGdi / NtUser)

> **Goal:** Build the Win32k shadow System Service Descriptor Table (SSDT Table 1) — the kernel-mode dispatch layer for all GDI and USER32 syscalls. In Windows, `win32k.sys` handles ~1300 `NtGdiXxx` and `NtUserXxx` entries. User-mode `gdi32.dll` and `user32.dll` call into this table via `syscall` with service numbers starting at `0x1000`. This TODO is the **master registry** for all Win32k shadow SSDT entries — some are implemented here, others are implemented by domain-specific TODOs but get their slots reserved and documented here. Starting with 108 core entries covering the most critical GDI and USER32 functions.

> [!IMPORTANT]
> **Current state:** The compositor, window manager, GDI primitives (`gfx_*`), font system (`ttf_*`), cursor shapes, and icon store all exist as kernel-mode C APIs. There is NO shadow SSDT, no `NtGdiXxx`/`NtUserXxx` dispatch, and no user-mode thunking. Win32 apps currently cannot call GDI/USER32 functions via syscall. The existing `SYS_WAIT_MESSAGE=75`, `SYS_GETMESSAGE=74`, `SYS_REGISTERCLASS=76`, `SYS_FINDWINDOW=77` in TODO-05-win32-subsystem are placeholders that need migration to the shadow SSDT.

## Inputs

- [`include/gfx.h`](../../include/gfx.h) — `gfx_draw_line`, `gfx_fill_rect`, `gfx_blit_alpha`, `gfx_surface_create/destroy`
- [`include/font_mgr.h`](../../include/font_mgr.h) — `ttf_get`, `ttf_draw_string`, `ttf_line_height`
- [`include/desktop/wm.h`](../../include/desktop/wm.h) — `wm_create_window`, `wm_destroy_window`, `wm_move_window`, `wm_focus_window`, `wm_mark_dirty`
- [`include/cursor.h`](../../include/cursor.h) — `cursor_set_shape`, `cursor_shape_t`
- [`include/icon_store.h`](../../include/icon_store.h) — `icon_get`, `icon_for_extension`
- → XREF: `02-kernel-core/TODO-05-native-api-ssdt.md §4` — main SSDT infrastructure; shadow SSDT (Table 1) placeholder allocated there; service numbers 0x1000+ dispatched to this table
- → XREF: `08-graphics-ui/TODO-11-win32-gdi-user32-stubs.md` — user-mode GDI/USER32 stub layer; this TODO provides the kernel-mode dispatch those stubs call into
- → XREF: `12-user-platform-sdk/TODO-05-win32-subsystem.md` — CSRSS loads win32k; message queue infrastructure (SYS_WAIT_MESSAGE etc.) migrates to this shadow SSDT
- → XREF: `08-graphics-ui/TODO-06-window-manager.md` — `wm_minimize/maximize/restore/set_title` wrapped by NtUserXxx
- → XREF: `08-graphics-ui/TODO-04-widget-library-core.md` — control painting routed through GDI DC

## Outcome

- Shadow SSDT (Table 1) with 108 entries at indices `0x1000–0x1063` dispatches `NtGdiXxx` and `NtUserXxx` syscalls.
- `gdi32.dll` stubs do `mov rax, 0x1000 + index; syscall` to reach kernel-mode GDI handlers.
- `user32.dll` stubs do the same for window/message/input handlers.
- The existing `gfx_*`, `ttf_*`, `wm_*`, `cursor_*` primitives are wrapped with NT-compatible signatures and NTSTATUS returns.
- Win32 PE applications can call `CreateWindowExA`, `GetMessageA`, `BeginPaint`, `TextOutA`, `BitBlt` etc. without modification.

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On          | Status |
| --- | :---: | ------------------------------------------------- | ------------------- | :----: |
| 💎  |   1   | Shadow SSDT infrastructure (Table 1 dispatch)     | TODO-05 §4          |  [ ]   |
| 💎  |   2   | GDI device context (DC) object table              | §1                  |  [ ]   |
| 💎  |   3   | GDI drawing syscalls (line, rect, ellipse, blit)  | §2                  |  [ ]   |
| 💎  |   4   | GDI text and font syscalls                        | §2                  |  [ ]   |
| 💎  |   5   | GDI bitmap and DIB syscalls                       | §2                  |  [ ]   |
| 💎  |   6   | GDI pen, brush, and region syscalls               | §2                  |  [ ]   |
| 💎  |   7   | USER window management syscalls                   | §1, TODO-06         |  [ ]   |
| 💎  |   8   | USER message queue syscalls                       | §7                  |  [ ]   |
| 💎  |   9   | USER input and cursor syscalls                    | §7                  |  [ ]   |
| 💎  |  10   | USER menu and accelerator syscalls                | §7                  |  [ ]   |
| 💎  |  11   | USER clipboard syscalls                           | §7                  |  [ ]   |
| ⭐  |  12   | Migrate SYS_GETMESSAGE/etc. to shadow SSDT        | §8                  |  [ ]   |

> 💎 = parity — Windows GDI32/USER32 and Linux Xlib/Wayland both provide equivalent functionality.
> ⭐ = exclusive — clean migration path from legacy SYS_* to proper shadow SSDT.

---

## 1. Shadow SSDT Infrastructure (Table 1 Dispatch)
Register the Win32k shadow SSDT as Table 1 in the SSDT dispatcher. Service numbers `0x1000–0x1FFF` are routed to this table. (→ XREF: TODO-05-native-api-ssdt.md §4)

- [ ] Create `include/kernel/nt/win32k_ssdt.h`:
  - `WIN32K_SSDT_TABLE` — same structure as main SSDT but separate function pointer array
  - `WIN32K_SERVICE_BASE = 0x1000` — offset for shadow table indices
- [ ] In `syscall_dispatch`: if `(service_number & 0x1000)`, dispatch to shadow SSDT at `index = service_number & 0x0FFF`
- [ ] Create `src/kernel/win32k/win32k_init.c`:
  - `win32k_init()` — register all NtGdiXxx and NtUserXxx handlers in shadow SSDT
  - Called during Phase 3 init (after compositor is ready)
- [ ] Unimplemented shadow slots return `STATUS_NOT_IMPLEMENTED`
- [ ] Commit: `"kernel: win32k — shadow SSDT Table 1 dispatch infrastructure"`

**Test checkpoint:** `syscall_dispatch(0x1000)` routes to shadow SSDT, not main SSDT. Unimplemented shadow slot returns `STATUS_NOT_IMPLEMENTED`. Serial: `"win32k: shadow SSDT registered, 108 entries"`.

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
- [ ] Commit: `"kernel: win32k — GDI DC object table, CreateCompatibleDC, GetDC/ReleaseDC"`

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
- [ ] Commit: `"kernel: win32k — GDI drawing syscalls (SetPixel through Polygon)"`

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
- [ ] Commit: `"kernel: win32k — GDI text and font syscalls (CreateFont through SetTextAlign)"`

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
- [ ] Commit: `"kernel: win32k — GDI bitmap and DIB syscalls"`

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
- [ ] Commit: `"kernel: win32k — GDI pen, brush, and region syscalls"`

**Test checkpoint:** `NtGdiCreatePen(PS_SOLID, 2, red)` + `NtGdiSelectObject` + `NtGdiLineTo` draws red 2px line. `NtGdiCreateRectRgn` + `NtGdiSelectClipRgn` clips drawing to region. `NtGdiGetStockObject(WHITE_BRUSH)` returns valid handle.

## 7. USER Window Management Syscalls
Window creation, positioning, and properties — wraps `wm_*` APIs.

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
- [ ] Commit: `"kernel: win32k — USER window management syscalls (CreateWindowEx through IsWindowVisible)"`

**Test checkpoint:** `NtUserCreateWindowEx` returns valid HWND. `NtUserShowWindow(SW_SHOW)` makes window visible. `NtUserBeginPaint` + `NtGdiRectangle` + `NtUserEndPaint` draws on window. `NtUserDestroyWindow` removes it.

## 8. USER Message Queue Syscalls
Windows message loop — the heart of Win32 UI. Wraps the kernel message queue infrastructure.

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
- [ ] Commit: `"kernel: win32k — USER message queue syscalls (GetMessage through KillTimer)"`

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
- [ ] Commit: `"kernel: win32k — USER input and cursor syscalls"`

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
- [ ] Commit: `"kernel: win32k — USER menu and accelerator syscalls"`

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
- [ ] Commit: `"kernel: win32k — USER clipboard syscalls"`

**Test checkpoint:** `NtUserOpenClipboard` + `NtUserSetClipboardData(CF_TEXT, "hello")` + `NtUserCloseClipboard` stores text. Another process `NtUserGetClipboardData(CF_TEXT)` retrieves "hello". `NtUserAddClipboardFormatListener` receives WM_CLIPBOARDUPDATE.

## 12. Migrate Legacy SYS_* to Shadow SSDT
Migrate the 4 existing Win32 UI syscalls from the main SSDT placeholder to proper shadow SSDT entries.

- [ ] Remove `SYS_GETMESSAGE=74` from main SSDT; replace with `NtUserGetMessage` → shadow SSDT 0x1060
- [ ] Remove `SYS_WAIT_MESSAGE=75` from main SSDT; replace with `NtUserWaitMessage` → shadow SSDT 0x1066
- [ ] Remove `SYS_REGISTERCLASS=76` from main SSDT; replace with `NtUserRegisterClassEx` → shadow SSDT 0x1067
- [ ] Remove `SYS_FINDWINDOW=77` from main SSDT; replace with `NtUserFindWindow` → shadow SSDT 0x1068
- [ ] Update `12-user-platform-sdk/TODO-05-win32-subsystem.md` to reference shadow SSDT indices
- [ ] Keep `SYS_*` macros as aliases for transition period
- [ ] Commit: `"kernel: win32k — migrate SYS_GETMESSAGE/WAIT_MESSAGE/REGISTERCLASS/FINDWINDOW to shadow SSDT"`

**Test checkpoint:** Existing desktop shell still works after migration. `syscall` with RAX=0x1060 reaches `NtUserGetMessage`. Old `SYS_GETMESSAGE=74` alias still works during transition.

---

### Shadow SSDT Master Table

> Service numbers in the `0x1000+` range. Organized by functional group. First 108 entries. Will expand toward Win11's ~1300 as features are implemented.

**0x1000–0x100F: GDI Device Context and Object Management**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x1000 | NtGdiCreateCompatibleDC          | §2  | T12                      | [ ]  |
| 0x1001 | NtGdiDeleteObjectApp             | §2  | T12                      | [ ]  |
| 0x1002 | NtGdiSelectObject                | §2  | T12                      | [ ]  |
| 0x1003 | NtGdiGetDC                       | §2  | T12                      | [ ]  |
| 0x1004 | NtGdiReleaseDC                   | §2  | T12                      | [ ]  |
| 0x1005 | NtGdiSaveDC                      | §2  | T12                      | [ ]  |
| 0x1006 | NtGdiRestoreDC                   | §2  | T12                      | [ ]  |

**0x1010–0x101F: GDI Drawing**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x1010 | NtGdiSetPixel                    | §3  | T12                      | [ ]  |
| 0x1011 | NtGdiGetPixel                    | §3  | T12                      | [ ]  |
| 0x1012 | NtGdiMoveTo                      | §3  | T12                      | [ ]  |
| 0x1013 | NtGdiLineTo                      | §3  | T12                      | [ ]  |
| 0x1014 | NtGdiRectangle                   | §3  | T12                      | [ ]  |
| 0x1015 | NtGdiFillRect                    | §3  | T12                      | [ ]  |
| 0x1016 | NtGdiEllipse                     | §3  | T12                      | [ ]  |
| 0x1017 | NtGdiBitBlt                      | §3  | T12                      | [ ]  |
| 0x1018 | NtGdiStretchBlt                  | §3  | T12                      | [ ]  |
| 0x1019 | NtGdiPatBlt                      | §3  | T12                      | [ ]  |
| 0x101A | NtGdiPolyline                    | §3  | T12                      | [ ]  |
| 0x101B | NtGdiPolygon                     | §3  | T12                      | [ ]  |
| 0x101C | NtGdiSetBkColor                  | §3  | T12                      | [ ]  |
| 0x101D | NtGdiSetTextColor                | §3  | T12                      | [ ]  |
| 0x101E | NtGdiSetBkMode                   | §3  | T12                      | [ ]  |

**0x1020–0x102F: GDI Text and Font**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x1020 | NtGdiCreateFont                  | §4  | T12                      | [ ]  |
| 0x1021 | NtGdiTextOut                     | §4  | T12                      | [ ]  |
| 0x1022 | NtGdiExtTextOut                  | §4  | T12                      | [ ]  |
| 0x1023 | NtGdiDrawText                    | §4  | T12                      | [ ]  |
| 0x1024 | NtGdiGetTextMetrics              | §4  | T12                      | [ ]  |
| 0x1025 | NtGdiGetTextExtentPoint          | §4  | T12                      | [ ]  |
| 0x1026 | NtGdiGetTextFace                 | §4  | T12                      | [ ]  |
| 0x1027 | NtGdiSetTextAlign                | §4  | T12                      | [ ]  |

**0x1030–0x103F: GDI Bitmap and DIB**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x1030 | NtGdiCreateCompatibleBitmap      | §5  | T12                      | [ ]  |
| 0x1031 | NtGdiCreateDIBSection            | §5  | T12                      | [ ]  |
| 0x1032 | NtGdiGetDIBits                   | §5  | T12                      | [ ]  |
| 0x1033 | NtGdiSetDIBits                   | §5  | T12                      | [ ]  |
| 0x1034 | NtGdiSetDIBitsToDevice           | §5  | T12                      | [ ]  |
| 0x1035 | NtGdiStretchDIBits               | §5  | T12                      | [ ]  |
| 0x1036 | NtGdiGetObject                   | §5  | T12                      | [ ]  |

**0x1040–0x104F: GDI Pen, Brush, and Region**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x1040 | NtGdiCreatePen                   | §6  | T12                      | [ ]  |
| 0x1041 | NtGdiCreateSolidBrush            | §6  | T12                      | [ ]  |
| 0x1042 | NtGdiCreateHatchBrush            | §6  | T12                      | [ ]  |
| 0x1043 | NtGdiCreatePatternBrush          | §6  | T12                      | [ ]  |
| 0x1044 | NtGdiGetStockObject              | §6  | T12                      | [ ]  |
| 0x1045 | NtGdiCreateRectRgn               | §6  | T12                      | [ ]  |
| 0x1046 | NtGdiCombineRgn                  | §6  | T12                      | [ ]  |
| 0x1047 | NtGdiSelectClipRgn               | §6  | T12                      | [ ]  |
| 0x1048 | NtGdiOffsetRgn                   | §6  | T12                      | [ ]  |
| 0x1049 | NtGdiPtInRegion                  | §6  | T12                      | [ ]  |

**0x1050–0x105F: USER Window Management**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x1050 | NtUserCreateWindowEx             | §7  | T12                      | [ ]  |
| 0x1051 | NtUserDestroyWindow              | §7  | T12                      | [ ]  |
| 0x1052 | NtUserShowWindow                 | §7  | T12                      | [ ]  |
| 0x1053 | NtUserMoveWindow                 | §7  | T12                      | [ ]  |
| 0x1054 | NtUserSetWindowPos               | §7  | T12                      | [ ]  |
| 0x1055 | NtUserGetClientRect              | §7  | T12                      | [ ]  |
| 0x1056 | NtUserGetWindowRect              | §7  | T12                      | [ ]  |
| 0x1057 | NtUserSetWindowText              | §7  | T12                      | [ ]  |
| 0x1058 | NtUserGetWindowText              | §7  | T12                      | [ ]  |
| 0x1059 | NtUserInvalidateRect             | §7  | T12                      | [ ]  |
| 0x105A | NtUserBeginPaint                 | §7  | T12                      | [ ]  |
| 0x105B | NtUserEndPaint                   | §7  | T12                      | [ ]  |
| 0x105C | NtUserSetFocus                   | §7  | T12                      | [ ]  |
| 0x105D | NtUserGetFocus                   | §7  | T12                      | [ ]  |
| 0x105E | NtUserIsWindow                   | §7  | T12                      | [ ]  |
| 0x105F | NtUserIsWindowVisible            | §7  | T12                      | [ ]  |

**0x1060–0x106F: USER Message Queue**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x1060 | NtUserGetMessage                 | §8  | T12 (SYS_GETMESSAGE=74) | [ ]  |
| 0x1061 | NtUserPeekMessage               | §8  | T12                      | [ ]  |
| 0x1062 | NtUserTranslateMessage           | §8  | T12                      | [ ]  |
| 0x1063 | NtUserDispatchMessage            | §8  | T12                      | [ ]  |
| 0x1064 | NtUserPostMessage                | §8  | T12                      | [ ]  |
| 0x1065 | NtUserSendMessage                | §8  | T12                      | [ ]  |
| 0x1066 | NtUserWaitMessage                | §8  | T12 (SYS_WAIT_MESSAGE=75)| [ ]  |
| 0x1067 | NtUserRegisterClassEx            | §8  | T12 (SYS_REGISTERCLASS=76)| [ ] |
| 0x1068 | NtUserFindWindow                 | §8  | T12 (SYS_FINDWINDOW=77) | [ ]  |
| 0x1069 | NtUserPostQuitMessage            | §8  | T12                      | [ ]  |
| 0x106A | NtUserDefWindowProc              | §8  | T12                      | [ ]  |
| 0x106B | NtUserSetTimer                   | §8  | T12                      | [ ]  |
| 0x106C | NtUserKillTimer                  | §8  | T12                      | [ ]  |

**0x1070–0x107F: USER Input and Cursor**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x1070 | NtUserGetKeyState                | §9  | T12                      | [ ]  |
| 0x1071 | NtUserGetAsyncKeyState           | §9  | T12                      | [ ]  |
| 0x1072 | NtUserGetCursorPos               | §9  | T12                      | [ ]  |
| 0x1073 | NtUserSetCursorPos               | §9  | T12                      | [ ]  |
| 0x1074 | NtUserSetCursor                  | §9  | T12                      | [ ]  |
| 0x1075 | NtUserLoadCursor                 | §9  | T12                      | [ ]  |
| 0x1076 | NtUserShowCursor                 | §9  | T12                      | [ ]  |
| 0x1077 | NtUserGetCapture                 | §9  | T12                      | [ ]  |
| 0x1078 | NtUserSetCapture                 | §9  | T12                      | [ ]  |
| 0x1079 | NtUserReleaseCapture             | §9  | T12                      | [ ]  |
| 0x107A | NtUserLoadIcon                   | §9  | T12                      | [ ]  |
| 0x107B | NtUserGetSystemMetrics           | §9  | T12                      | [ ]  |

**0x1080–0x108F: USER Menu and Accelerator**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x1080 | NtUserCreateMenu                 | §10 | T12                      | [ ]  |
| 0x1081 | NtUserCreatePopupMenu            | §10 | T12                      | [ ]  |
| 0x1082 | NtUserAppendMenu                 | §10 | T12                      | [ ]  |
| 0x1083 | NtUserInsertMenuItem             | §10 | T12                      | [ ]  |
| 0x1084 | NtUserSetMenu                    | §10 | T12                      | [ ]  |
| 0x1085 | NtUserDestroyMenu                | §10 | T12                      | [ ]  |
| 0x1086 | NtUserTrackPopupMenu             | §10 | T12                      | [ ]  |
| 0x1087 | NtUserCreateAcceleratorTable     | §10 | T12                      | [ ]  |
| 0x1088 | NtUserTranslateAccelerator       | §10 | T12                      | [ ]  |
| 0x1089 | NtUserDestroyAcceleratorTable    | §10 | T12                      | [ ]  |

**0x1090–0x109F: USER Clipboard**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x1090 | NtUserOpenClipboard              | §11 | T12                      | [ ]  |
| 0x1091 | NtUserCloseClipboard             | §11 | T12                      | [ ]  |
| 0x1092 | NtUserEmptyClipboard             | §11 | T12                      | [ ]  |
| 0x1093 | NtUserSetClipboardData           | §11 | T12                      | [ ]  |
| 0x1094 | NtUserGetClipboardData           | §11 | T12                      | [ ]  |
| 0x1095 | NtUserIsClipboardFormatAvailable | §11 | T12                      | [ ]  |
| 0x1096 | NtUserCountClipboardFormats      | §11 | T12                      | [ ]  |
| 0x1097 | NtUserEnumClipboardFormats       | §11 | T12                      | [ ]  |
| 0x1098 | NtUserRegisterClipboardFormat    | §11 | T12                      | [ ]  |
| 0x1099 | NtUserAddClipboardFormatListener | §11 | T12                      | [ ]  |

> **Total: 100 shadow SSDT entries** across 10 functional ranges. Will expand toward Win11's ~1300 as GDI/USER features mature. All entries initially return `STATUS_NOT_IMPLEMENTED` until their owning section is implemented.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                      | 🐧 Linux                      | 🚀 Impossible OS              |
|----|----------------------------|-------------------------------|-------------------------------|-------------------------------|
| 💎 | Kernel GDI dispatch        | ✅ win32k.sys NtGdiXxx        | ❌ No kernel GDI (Mesa UMD)   | ⬜ §2–§6 — 46 GDI entries     |
| 💎 | Kernel USER dispatch       | ✅ win32k.sys NtUserXxx       | ❌ No kernel USER (Wayland)   | ⬜ §7–§11 — 54 USER entries   |
| 💎 | Shadow SSDT (Table 1)      | ✅ ~1300 entries               | ❌ No SSDT concept            | ⬜ §1 — 108 entries (growing) |
| 💎 | DC-based drawing model     | ✅ HDC + GDI objects           | ❌ Direct framebuffer/Vulkan  | ⬜ §2 — DC wraps gfx_surface  |
| 💎 | Message queue syscalls     | ✅ NtUserGetMessage            | ❌ Wayland fd polling         | ⬜ §8 — kernel msg queue      |
| 💎 | Clipboard syscalls         | ✅ NtUserGet/SetClipboardData  | ❌ Wayland clipboard protocol | ⬜ §11 — kernel clipboard     |
| 💎 | Menu/accelerator syscalls  | ✅ NtUserCreateMenu            | ❌ Toolkit-level only         | ⬜ §10 — kernel menus         |
| ⭐ | Unified kernel GDI+USER    | ⚠️ win32k.sys (legacy monolith)| ❌ No equivalent              | ⬜ Clean modular impl         |
| ⭐ | Direct gfx_* wrapping      | ❌ GDI → DirectX translation  | ❌ Mesa userspace             | ⬜ Zero-overhead kernel path  |

> **After §1–§12:** Win32 PE applications can call GDI32/USER32 functions via shadow SSDT syscalls, with each function wrapping the existing Impossible OS compositor/gfx/font primitives. No translation layer — direct kernel-mode rendering.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_win32k()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
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
- [ ] Serial log: `"win32k: shadow SSDT registered, 108 entries"` during Phase 3
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
- [ ] Commit: `"kernel: win32k — shadow SSDT complete (108 entries)"`
