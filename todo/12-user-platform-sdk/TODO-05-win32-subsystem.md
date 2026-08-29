---
schema_version: 1
id: win32-subsystem
domain: 12-user-platform-sdk
status: active
title: "TODO-05 -- Win32 Subsystem Server (CSRSS)"
---

# TODO-05 -- Win32 Subsystem Server (CSRSS)

> **Goal:** Build the Win32 subsystem server that bridges user-mode processes and the
> kernel WM, providing the per-thread message queue, window class registry, WndProc
> dispatch, HDC painting model, accelerator tables, window subclassing, cross-process
> messaging, and common dialogs. This is the architectural substrate that backs the
> API-surface stubs in `10-platform-services/TODO-08` Sections 10 and 11 and
> `08-graphics-ui/TODO-11`. Per-export rows for user32: `10-platform-services/TODO-A-user32-export-master-table.md`.

> [!IMPORTANT]
> **Scope boundary**: `TODO-08` Section 10 contains `user32.dll` function stubs
> (`RegisterClassExA`, `CreateWindowExA`, `GetMessage`, `DispatchMessage`,
> `PostQuitMessage`, `SendMessage`, `SetWindowText`) and `TODO-11` (08-graphics-ui)
> contains the GDI object table and `GetMessageA` per-window blocking queue. This TODO
> specifies the **subsystem architecture** those stubs delegate to: the MSG ring-buffer
> queues, the 256-entry global class table, cross-process delivery via `pipe.h` IPC,
> dirty-rect integration with the compositor surface, and accelerator/subclassing
> infrastructure. Do not re-specify the function signatures already in TODO-08 §10/§11.
>
> **IPC ready**: `include/kernel/ipc/pipe.h` has `pipe_create`/`pipe_write`/`pipe_read`.
> `SYS_SHMEM_CREATE=35`/`SYS_SHMEM_MAP=36` exist. Use these for cross-process messaging (§7).
>
> **Syscall numbers**: Win32 syscall range `SYS_CREATEFILE=60`…`SYS_POSTMESSAGE=73`
> allocated in `TODO-07 §2`. New subsystem syscalls (`SYS_GETMESSAGE`, `SYS_WAIT_MESSAGE`,
> `SYS_REGISTERCLASS`, `SYS_FINDWINDOW`) follow at 74+.
>
> **HDC→gfx_surface_t**: `gfx_surface_t *` is the native rendering type. HDC is an opaque
> 64-bit handle; the GDI object table (→ XREF `08-graphics-ui/TODO-11 §7`) maps HDC handles
> to `gfx_surface_t *`. BeginPaint/EndPaint (§4) use this mapping.

---

## Inputs

- `include/desktop/wm.h` -- `wm_create_window`, `wm_destroy_window`, `WM_FLAG_*` -- §2 §3
- `include/desktop/controls.h` -- `CTRL_BUTTON`/`CTRL_EDIT`/`CTRL_LISTBOX`/`CTRL_COMBOBOX`/`CTRL_SCROLLBAR` -- §2 built-in classes
- `include/kernel/ipc/pipe.h` -- `pipe_create/write/read` -- §7 cross-process messaging
- `include/kernel/ipc/` -- `SYS_SHMEM_CREATE=35`, `SYS_SHMEM_MAP=36` -- §7 `WM_COPYDATA`
- `include/kernel/sched/syscall.h` -- extend with `SYS_GETMESSAGE=74`, `SYS_WAIT_MESSAGE=75`, `SYS_REGISTERCLASS=76`, `SYS_FINDWINDOW=77`
- `include/kernel/sched/task.h` -- per-task message queue pointer -- §1
- `08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md §7` (→ XREF) -- GDI object table, HDC→`gfx_surface_t` mapping
- `08-graphics-ui/TODO-04-ui-controls.md` (→ XREF) -- `CTRL_*` implementations backing built-in window classes (§4)
- `08-graphics-ui/TODO-06-widget-dialogs.md §8` (→ XREF) -- `dialog_color` + file dialogs backing `ChooseColor`/`ChooseFont` (§8)
- `10-platform-services/TODO-08-win32-api-surface.md §10 §11` (→ XREF) -- `user32.dll` / `gdi32.dll` stubs that delegate to subsystem (do not duplicate)
- `10-platform-services/TODO-07-win32-pe-loader.md §2` (→ XREF) -- Win32 syscall range `SYS_CREATEFILE=60`…`SYS_POSTMESSAGE=73`
- `include/kernel/drivers/framebuffer.h` -- `fb_lock_compositor`, dirty-rect API -- §4

---

## Outcome

Every GUI process has a per-thread MSG ring buffer (1000 entries). `GetMessage` blocks via
`SYS_WAIT_MESSAGE` until an entry is available. Window classes register globally (256 slots)
or per-process (64 slots), with built-in classes mapping to `CTRL_*` widgets. HDC from
`BeginPaint` maps to the window's `gfx_surface_t`; `EndPaint` flushes the dirty region to
the compositor. `SendMessage` across processes goes through a kernel IPC pipe with
synchronous reply. `ChooseColor`/`ChooseFont` show the common dialogs from `TODO-05 §8`.

---

## Implementation Order

| Step | Section                                                      | 💎/⭐ | Dependency                                                                                 |
| ---- | ------------------------------------------------------------ | ----- | ------------------------------------------------------------------------------------------ |
| 1    | Win32 subsystem architecture + message queue                 | 💎    | `struct task` queue field; `SYS_WAIT_MESSAGE=75`                                           |
| 2    | Window class registry + built-in classes                     | 💎    | §1; `CTRL_*` (TODO-04); `wm_create_window`                                                 |
| 3    | WndProc dispatch + DefWindowProc                             | 💎    | §1 §2; `TranslateMessage` key tables                                                       |
| 4    | Win32 painting model (HDC + dirty rect)                      | 💎    | §1 §2; HDC table (see `08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md`); compositor      |
| 5    | Message filters + accelerator tables                         | 💎    | §1 §3; `TranslateMessage` complete; `SYS_POSTMESSAGE=73`                                   |
| 6    | Window subclassing + property store                          | 💎    | §2 §3                                                                                      |
| 7    | Inter-process window messaging                               | 💎    | §1; `pipe.h`; `SYS_SHMEM_CREATE/MAP`; §2 `FindWindow`                                      |
| 8    | Common dialog boxes (`ChooseColor`/`ChooseFont`)             | 💎    | §1 msg loop; `TODO-05 §8` dialog system                                                    |
| 9    | Win11 visual opt-in surface (`dwmapi`, `uxtheme`, broadcast) | 💎    | §2 §3 §4 §7; `08-graphics-ui/TODO-03 §9` tokens + keys; `08-graphics-ui/TODO-08` Mica/snap |

---

## 1. Win32 Subsystem Architecture + Message Queue `[Opus]`

> Novel CSRSS-style architecture: per-thread ring-buffer MSG queue bridging user-mode
> Win32 calls and the kernel WM event loop. No prior Impossible OS per-thread message
> queue exists (only the WM's internal event list).

**Source:** `src/user/csrss/csrss.c` (initially kernel-mode subsystem thread; user-mode
process in future iteration)

- [ ] **`struct msg_queue`** in `include/win32/msg_queue.h`:
  ```c
  #define MSG_QUEUE_DEPTH 1000

  typedef struct {
      HWND   hwnd;
      UINT   message;
      WPARAM wParam;
      LPARAM lParam;
      DWORD  time;      /* GetTickCount() at post time */
      POINT  pt;        /* cursor position at post time */
  } MSG;

  typedef struct {
      MSG      ring[MSG_QUEUE_DEPTH]; /* power-of-2 for fast modulo */
      uint32_t head;                  /* next write index */
      uint32_t tail;                  /* next read index */
      uint32_t count;                 /* entries available */
      spinlock_t lock;
      /* Wait token: thread sleeps here when count==0 */
      uint32_t  wait_tid;             /* task ID of blocked thread */
  } msg_queue_t;
  ```
- [ ] **Per-task attachment**: add `msg_queue_t *win32_queue` field to `struct task`; initialized on first `RegisterClassExA` or `CreateWindowExA` call (lazy); freed on process exit
- [ ] **`msg_queue_post(queue, hwnd, msg, wparam, lparam)`**:
  - Acquire `queue->lock`; if `count == MSG_QUEUE_DEPTH`: drop message + log overflow at `WARN` level
  - Write `MSG` into `ring[head % MSG_QUEUE_DEPTH]`; `head++`; `count++`
  - If `wait_tid != 0`: wake the blocked thread (`sched_wake(wait_tid)`); clear `wait_tid`
  - Release lock
- [ ] **`SYS_WAIT_MESSAGE=75`** kernel handler (`sys_wait_message(MSG *out_msg, HWND filter_hwnd, UINT min, UINT max)`):
  - Get current task's `win32_queue`; acquire lock
  - If `count == 0`: record `wait_tid = current->pid`; release lock; `sched_sleep(current)` until woken
  - Re-acquire lock after wake; dequeue one `MSG` matching `filter_hwnd`/`[min,max]` range (skip non-matching, wrap); `count--`; copy to `out_msg`; release lock; return 1 (or 0 for `WM_QUIT`)
- [ ] **`SYS_GETMESSAGE=74`** (`GetMessage` user-mode wrapper) → `SYS_WAIT_MESSAGE` with blocking=true; `PeekMessage` → same handler with `remove` flag (non-blocking: return -1 if queue empty)
- [ ] **`PostMessage(hwnd, msg, w, l)`** → look up owning thread via HWND table; call `msg_queue_post` on that thread's queue; `SYS_POSTMESSAGE=73` for user-mode path
- [ ] **`WM_QUIT` delivery**: `PostQuitMessage(exit_code)` → post `{hwnd=NULL, message=WM_QUIT, wParam=exit_code}` to current thread's queue; `GetMessage` returns 0; app exits message loop

---

## 2. Window Class Registry + Built-in Classes `[Sonnet]`

**Source:** `src/user/csrss/class_registry.c`; header `include/win32/class_registry.h`

- [ ] **Data structures**:
  ```c
  #define MAX_GLOBAL_CLASSES  256
  #define MAX_PROCESS_CLASSES  64
  #define CLASSNAME_MAX        64

  typedef struct {
      char       className[CLASSNAME_MAX];
      HINSTANCE  hInstance;           /* NULL = global/system class */
      WNDPROC    WndProc;
      uint32_t   style;
      int32_t    cbClsExtra;          /* extra bytes per class */
      int32_t    cbWndExtra;          /* extra bytes per window instance */
      HICON      hIcon;
      HCURSOR    hCursor;
      HBRUSH     hbrBackground;
      const char *menuName;
      uint32_t   refcount;            /* windows using this class */
  } wnd_class_entry_t;

  extern wnd_class_entry_t g_global_classes[MAX_GLOBAL_CLASSES]; /* system classes */
  extern int               g_global_class_count;
  ```
- [ ] **`ATOM RegisterClassExA(const WNDCLASSEX *wc)`**:
  - Case-insensitive search for duplicate name in per-process table (64 slots in `struct task`) + global table; return 0 + `SetLastError(ERROR_CLASS_ALREADY_EXISTS)` on collision
  - Write new `wnd_class_entry_t`; return atom (1-based index); `SYS_REGISTERCLASS=76` for user-mode path
- [ ] **`BOOL UnregisterClass(name, hInstance)`**: find entry; if `refcount > 0`: return FALSE + `ERROR_CLASS_HAS_WINDOWS`; else clear entry
- [ ] **`BOOL GetClassInfo(hInstance, name, WNDCLASSEX *out)`**: copy entry to `out`; return TRUE on found
- [ ] **Built-in system classes** (registered at subsystem init with `hInstance=NULL`, `WndProc` = internal handler):
| Class name    | Backing implementation          |
| ------------- | ------------------------------- |
| `"BUTTON"`    | `CTRL_BUTTON` (`controls.h`)    |
| `"EDIT"`      | `CTRL_TEXTBOX` (`controls.h`)   |
| `"STATIC"`    | `CTRL_LABEL` (`controls.h`)     |
| `"LISTBOX"`   | `CTRL_LISTBOX` (`controls.h`)   |
| `"COMBOBOX"`  | `CTRL_COMBOBOX` (`controls.h`)  |
| `"SCROLLBAR"` | `CTRL_SCROLLBAR` (`controls.h`) |
- [ ] **`CreateWindowExA(exStyle, className, title, style, x, y, w, h, parent, menu, hInstance, param)`**:
  - Resolve class from per-process table then global table; `ERROR_CLASS_DOES_NOT_EXIST` if not found
  - `wm_create_window(title, x, y, w, h, flags_from_style)` → internal window handle; allocate `HWND` from global HWND table (`g_hwnd_table[4096]`, dense array, index + 1 = HWND)
  - Allocate `cbWndExtra` bytes appended to HWND table entry for `GWLP_USERDATA` + extra storage
  - Increment class `refcount`; post `WM_CREATE` to thread queue (`lpCreateParams` in lParam as `CREATESTRUCT *`)
  - Return HWND; `INVALID_HANDLE_VALUE` on failure
- [ ] **`DestroyWindow(hwnd)`**: post `WM_DESTROY`; then `WM_NCDESTROY`; call `wm_destroy_window(handle)`; decrement class refcount; free HWND table entry

- [ ] Built-in classes are the ONLY public control ABI: the SDK (`10-platform-services/TODO-09 §1` → XREF) exposes `user32` / `comctl32` classes, never a parallel native widget API; `CTRL_*` stays shell-internal behind this class table

---

## 3. WndProc Dispatch + DefWindowProc `[Sonnet]`

**Source:** `src/user/csrss/dispatch.c`

- [ ] **`DispatchMessage(const MSG *msg)`**:
  - Look up `g_hwnd_table[hwnd - 1]`; validate not destroyed (`ERROR_INVALID_WINDOW_HANDLE` + return 0 on miss)
  - Look up `wnd_class_entry_t` for the window; call `entry->WndProc(hwnd, msg->message, msg->wParam, msg->lParam)`
  - Return WndProc return value
- [ ] **`DefWindowProcA(hwnd, msg, wParam, lParam)`** default message handling:
| Message                        | Default action                                                                                          |
| ------------------------------ | ------------------------------------------------------------------------------------------------------- |
| `WM_PAINT`                     | `BeginPaint` + `EndPaint` (validates region, no drawing)                                                |
| `WM_CLOSE`                     | `DestroyWindow(hwnd)`                                                                                   |
| `WM_DESTROY`                   | `PostQuitMessage(0)`                                                                                    |
| `WM_SIZE`                      | Update stored window rect; return 0                                                                     |
| `WM_MOVE`                      | Update stored position; return 0                                                                        |
| `WM_ERASEBKGND`                | `FillRect(hdc, &rect, hbrBackground)`; return 1                                                         |
| `WM_SETFOCUS` / `WM_KILLFOCUS` | Store focus state in HWND entry; return 0                                                               |
| `WM_KEYDOWN` / `WM_KEYUP`      | Forward to parent if child window; else return 0                                                        |
| `WM_LBUTTONDOWN`               | Hit-test child windows; if hit: `SetFocus(child)`; forward msg                                          |
| `WM_MOUSEMOVE`                 | Update cursor; check `WM_MOUSELEAVE` tracking; return 0                                                 |
| `WM_NCHITTEST`                 | Return `HTCLIENT` for client area, `HTCAPTION` for title bar, `HTCLOSE` etc.                            |
| `WM_SYSCOMMAND`                | `SC_CLOSE` → `SendMessage(hwnd, WM_CLOSE,...)`; `SC_MINIMIZE`/`SC_MAXIMIZE`/`SC_RESTORE` → `wm_*` calls |
| All others                     | Return 0                                                                                                |
- [ ] **`SetWindowTextA(hwnd, text)`**: update stored title in HWND entry; call `wm_set_title(handle, text)`; post `WM_SETTEXT` to queue
- [ ] **`GetWindowTextA(hwnd, buf, max)`**: copy from HWND entry title field; return length
- [ ] **`ShowWindow(hwnd, nCmdShow)`**: map `SW_SHOW/HIDE/MINIMIZE/MAXIMIZE/RESTORE` to `WM_FLAG_*` changes via `wm_set_flags()`; post `WM_SHOWWINDOW`
- [ ] **`GetClientRect(hwnd, rect_out)`** / **`GetWindowRect(hwnd, rect_out)`**: read from HWND entry; client rect = window rect minus title bar and borders

---

## 4. Win32 Painting Model (HDC + Dirty Rect) `[Opus]`

> Novel: maps Win32 HDC to compositor `gfx_surface_t`; integrates dirty-rect tracking
> with the WM compositor update cycle. No prior Impossible OS HDC model exists.

**Source:** `src/user/csrss/paint.c`; extends GDI object table in `08-graphics-ui/TODO-11 §7`

- [ ] **`PAINTSTRUCT`**:
  ```c
  typedef struct {
      HDC   hdc;
      BOOL  fErase;
      RECT  rcPaint;        /* dirty region bounding box */
      BOOL  fRestore;       /* internal */
      BOOL  fIncUpdate;     /* internal */
      BYTE  rgbReserved[32];
  } PAINTSTRUCT;
  ```
- [ ] **Dirty rect tracking per HWND**: add `RECT dirty_rect` + `BOOL needs_paint` to HWND table entry; `InvalidateRect(hwnd, rect, erase)`:
  - Union `rect` into `hwnd_entry->dirty_rect`; set `needs_paint = TRUE`
  - If `erase`: set `hwnd_entry->erase_background = TRUE`
  - Post `WM_PAINT` to owning thread's queue **only if** no `WM_PAINT` already in queue (coalesce multiple invalidations into one `WM_PAINT`)
- [ ] **`ValidateRect(hwnd, rect)`**: subtract `rect` from `dirty_rect`; if result is empty: `needs_paint = FALSE`
- [ ] **`UpdateWindow(hwnd)`**: if `needs_paint`: directly call `WndProc(hwnd, WM_PAINT, 0, 0)` without queuing (synchronous); skip if `needs_paint == FALSE`
- [ ] **`HDC BeginPaint(hwnd, PAINTSTRUCT *ps)`**:
  - Copy `dirty_rect` into `ps->rcPaint`; set `ps->fErase = erase_background`
  - Get window's `gfx_surface_t *` from WM internal surface list (by HWND internal handle)
  - Allocate HDC handle via GDI object table (→ XREF `TODO-11 §7`): `hdc_alloc(surf)` → HDC
  - Set `ps->hdc = hdc`; call `ValidateRect(hwnd, NULL)` to clear dirty region; return HDC
- [ ] **`BOOL EndPaint(hwnd, const PAINTSTRUCT *ps)`**:
  - Mark the `gfx_surface_t` region covering `ps->rcPaint` as compositor-dirty
  - Release HDC via GDI object table: `hdc_release(ps->hdc)`; trigger compositor update (`fb_compositor_invalidate(rect)`)
  - Return TRUE
- [ ] **`HDC GetDC(hwnd)`** / **`ReleaseDC(hwnd, hdc)`**: `GetDC` → same as `BeginPaint` without `PAINTSTRUCT` overhead; `ReleaseDC` → `hdc_release(hdc)` + compositor invalidate full window rect
- [ ] **`CreateCompatibleDC(hdc)`**: allocate off-screen `gfx_surface_t` matching dimensions of `hdc`'s surface; return new HDC (for `BitBlt`/`StretchBlt` off-screen rendering); `DeleteDC(hdc)` → free off-screen surface + HDC handle
- [ ] **`SelectObject(hdc, hobj)`**: store `hobj` (HBITMAP/HBRUSH/HPEN/HFONT) as current GDI object in HDC entry; return previous object; used for `BitBlt` source bitmap association

---

## 5. Message Filters + Accelerator Tables `[Sonnet]`

**Source:** `src/user/csrss/translate.c`

- [ ] **`TranslateMessage(const MSG *msg)`** full implementation:
  - Only processes `WM_KEYDOWN` / `WM_SYSKEYDOWN`
  - Map `wParam` (virtual key) + current modifier state (Shift/Ctrl/Alt from `keyboard_get_modifiers()`) to character:
    - Printable ASCII: use `vk_to_char[256]` table (shifted variant when Shift is held)
    - `WM_SYSKEYDOWN` (Alt held): post `WM_SYSCHAR` instead of `WM_CHAR`
  - Post `WM_CHAR(char, repeat_count)` to thread queue; return TRUE if translated, FALSE otherwise
  - Dead-key combining: if key is a dead-key prefix (e.g., `^` → combined with next key); store in `g_dead_key` per-thread state
- [ ] **`WM_SYSKEYDOWN` → `WM_SYSCOMMAND` mapping** in `TranslateMessage`:
  - `Alt+F4` → post `WM_SYSCOMMAND(SC_CLOSE)` to focused window
  - `Alt+Space` → post `WM_SYSCOMMAND(SC_KEYMENU)` → system menu
  - `Alt+Tab` → post to desktop shell (do not deliver to app)
  - `F10` (no Alt) → same as `Alt+Space` (`SC_KEYMENU`)
- [ ] **Accelerator tables**:
  - `HACCEL LoadAccelerators(hInstance, lpTableName)`: load `ACCEL` array from PE resource section (`RT_ACCELERATOR = 9`); return handle
  - `int TranslateAccelerator(hwnd, haccel, msg)`: if `msg->message` is `WM_KEYDOWN`/`WM_SYSKEYDOWN`: scan `ACCEL[]` for matching `key` + `fVirt` flags (`FALT`, `FSHIFT`, `FCONTROL`, `FVIRTKEY`); if match: post `WM_COMMAND(cmd_id, 0, 0)` + `WM_SYSCOMMAND` for system range; return 1 if consumed, 0 otherwise
  - `BOOL DestroyAcceleratorTable(haccel)`: free table memory
- [ ] **`PostQuitMessage(exit_code)`**: post `{hwnd=NULL, message=WM_QUIT, wParam=(WPARAM)exit_code, lParam=0}` to current thread's queue; `GetMessage` returns FALSE on `WM_QUIT`

---

## 6. Window Subclassing + Property Store `[Sonnet]`

**Source:** `src/user/csrss/subclass.c`

- [ ] **`SetWindowLongPtrA(hwnd, index, value)`**:
  - `GWLP_WNDPROC` (index = -4): replace `WndProc` in HWND entry; return old proc address; used for subclassing
  - `GWLP_USERDATA` (index = -21): store `value` in HWND entry `user_data` field
  - `GWLP_ID` (index = -12): store control ID
  - `GWL_STYLE` (index = -16): update `style` field; call `wm_set_flags()` to sync visible flags
  - `GWL_EXSTYLE` (index = -20): store extended style flags
  - Non-negative index: store in `cbWndExtra` bytes appended to HWND entry (offset = index); bounds check
- [ ] **`GetWindowLongPtrA(hwnd, index)`**: reverse of above; return stored value
- [ ] **`CallWindowProc(old_proc, hwnd, msg, wParam, lParam)`**: call `old_proc(hwnd, msg, wParam, lParam)` directly; enables subclass chain forwarding
- [ ] **Property store** per HWND (chained hash map, max 32 entries):
  ```c
  typedef struct { char name[32]; HANDLE value; struct prop_entry *next; } prop_entry_t;
  ```
  - `BOOL SetProp(hwnd, name, data)`: insert or update entry in HWND property chain; return TRUE
  - `HANDLE GetProp(hwnd, name)`: lookup by name (case-insensitive); return value or NULL
  - `HANDLE RemoveProp(hwnd, name)`: unlink and free entry; return old value
  - `EnumProps(hwnd, callback)`: iterate all entries, call `callback(hwnd, name, handle)` for each

---

## 7. Inter-Process Window Messaging `[Opus]`

> Novel cross-process mechanism: kernel IPC pipe for `SendMessage` synchronous reply;
> kernel-allocated shared buffer for `WM_COPYDATA`. Security-sensitive.

**Source:** `src/user/csrss/ipc_msg.c`

- [ ] **HWND-to-process routing**: HWND table entry stores `owning_pid`; if `owning_pid != current_pid`: cross-process path; else: in-process fast path (§1 queue)
- [ ] **`FindWindow(className, title)`**:
  - `SYS_FINDWINDOW=77` kernel handler: scan `g_hwnd_table[]` for matching class name + window title (both NULL = wildcard); return first matching HWND or NULL
  - Case-insensitive title match; `className=NULL` matches any class
- [ ] **Cross-process `PostMessage(hwnd, msg, w, l)`**: serialize `MSG` struct; `pipe_write(target_process_msg_pipe, &msg, sizeof(MSG))`; kernel delivers to target process's `msg_queue_post`
- [ ] **Cross-process `SendMessage(hwnd, msg, w, l)` (synchronous)**:
  1. Serialize request: `{ MSG msg; LRESULT *reply_slot; }` into `SYS_SHMEM_CREATE` shared buffer
  2. `pipe_write` to target; target thread wakes, dispatches, writes `LRESULT` into `reply_slot`
  3. Calling thread blocks on reply pipe read (`pipe_read`); timeout after 5000 ms → `SetLastError(ERROR_TIMEOUT)`; return 0
  4. Return `reply_slot` value; free shared buffer
- [ ] **`WM_COPYDATA` (`COPYDATASTRUCT` serialization)**:
  - Sender: `shmem_create(size)` → copy data; `SendMessage(target, WM_COPYDATA, (WPARAM)hwnd_sender, (LPARAM)&cds)` -- CDS embedded in shmem
  - Target receives `WM_COPYDATA`; `lParam` = valid pointer to `COPYDATASTRUCT` in mapped shmem; **read-only** for target; kernel unmaps after WndProc returns
  - `cbData` limit: 64 KB; larger transfers → `ERROR_NOT_ENOUGH_MEMORY` + return 0
- [ ] **`BroadcastSystemMessage(flags, recipients, msg, w, l)`**:
  - `BSM_ALLDESKTOPS | BSM_APPLICATIONS`: iterate `g_hwnd_table[]` for all top-level windows (`parent == NULL`); `PostMessage` each (async) or `SendMessage` if `BSF_QUERY` flag
  - `BSM_ALLCOMPONENTS`: also post to desktop shell thread
  - `BSMF_POSTMESSAGE`: use async `PostMessage` for all recipients
  - `BSF_NOHANG`: skip unresponsive target (reply timeout = 100 ms per window)
  - Return bitmask of components that processed message

---

## 8. Common Dialog Boxes `[Sonnet]`

**Source:** `src/user/csrss/comdlg.c`; header `include/win32/comdlg.h`

> Delegates to `08-graphics-ui/TODO-05 §8` dialog implementations via function pointers
> registered at subsystem init. No UI logic here -- this is the Win32 API wrapper layer.

- [ ] **`BOOL ChooseColorA(CHOOSECOLOR *cc)`**:
  - Show `dialog_color()` (→ XREF `08-graphics-ui/TODO-05 §8`) modal dialog with initial color `cc->rgbResult`
  - `cc->lpCustColors[16]` preserved as custom color history
  - On OK: `cc->rgbResult = chosen_color`; return TRUE; on Cancel: return FALSE
  - `cc->Flags & CC_FULLOPEN`: show full expanded picker; `CC_PREVENTFULLOPEN`: hide expand button
- [ ] **`BOOL ChooseFontA(CHOOSEFONT *cf)`**:
  - Show font picker dialog (→ XREF `08-graphics-ui/TODO-02-text-font-internationalization.md §1,§5` font catalog): list system fonts via `font_mgr_list()`; preview with `ttf_draw_string()`; size/style selectors
  - On OK: populate `cf->lpLogFont` (`LOGFONTA`: face name, height, weight, italic, underline); return TRUE
  - `CF_EFFECTS`: show strikethrough + color selector; `CF_FIXEDPITCHONLY`: filter to monospaced only
- [ ] **`DWORD CommDlgExtendedError(void)`**: return per-thread last common dialog error code; errors:
  - `CDERR_GENERALCODES = 0x0000`, `CDERR_STRUCTSIZE`, `CDERR_INITIALIZATION`, `CDERR_NOTEMPLATE`, `CDERR_MEMALLOCFAILURE`, `CDERR_DIALOGFAILURE`
  - Stored in thread-local `g_comdlg_last_error` (use `TlsAlloc` from §4 of `TODO-04`)
- [ ] **`GetOpenFileNameA(OPENFILENAME *ofn)`** / **`GetSaveFileNameA`**: already specced in `08-graphics-ui/TODO-05 §8`; provide thin Win32 struct adapter wrapper (map `OFN_*` flags to `dialog_file_open/save` params)

---

## 9. Win11 Visual Opt-In Surface: `dwmapi`, `uxtheme`, Personalization Broadcast `[Opus]`

> **Spawned-by:** root

**Source:** `src/user/csrss/dwmapi.c`, `src/user/csrss/uxtheme.c`; headers `include/win32/dwmapi.h`, `include/win32/uxtheme.h`

> Windows 11 apps opt into the modern look through a small, well-known API set; matching those calls exactly is what makes a Win32 app look native here with no Impossible-specific code. Everything renders through the same `CTRL_*` widgets and `gfx_*` primitives as the shell (built-in classes, §2), so there is one control implementation, one theme (`08-graphics-ui/TODO-03 §9` → XREF supplies the tokens and the Registry keys) and no visual-style engine to emulate. No export master table exists for `dwmapi.dll` / `uxtheme.dll` yet; the rows below are the inventory until `10-platform-services/TODO-A`-style tables are added for them.

- [ ] **`DwmSetWindowAttribute(hwnd, attr, pv, cb)`** / **`DwmGetWindowAttribute`**: per-window attribute store on the WM window; wrong `cb` or unknown attribute → `E_INVALIDARG`, never silently accepted
  - `DWMWA_USE_IMMERSIVE_DARK_MODE (20)`: titlebar + frame use dark tokens regardless of system mode
  - `DWMWA_WINDOW_CORNER_PREFERENCE (33)`: `DWMWCP_DEFAULT / DONOTROUND / ROUND / ROUNDSMALL` → compositor corner radius 8 / 0 / 8 / 4 px
  - `DWMWA_SYSTEMBACKDROP_TYPE (38)`: `DWMSBT_MAINWINDOW` (Mica) / `DWMSBT_TRANSIENTWINDOW` (Acrylic) / `DWMSBT_TABBEDWINDOW` (Mica Alt) → `gfx_mica` / `gfx_acrylic` per window (`08-graphics-ui/TODO-08` Mica titlebar)
  - `DWMWA_CAPTION_COLOR (35)`, `DWMWA_TEXT_COLOR (36)`, `DWMWA_BORDER_COLOR (34)`: `COLORREF` overrides with the `DWMWA_COLOR_DEFAULT` / `DWMWA_COLOR_NONE` sentinels
  - Get side: `DWMWA_EXTENDED_FRAME_BOUNDS (9)`, `DWMWA_CLOAKED (14)`
- [ ] **`DwmExtendFrameIntoClientArea(hwnd, MARGINS*)`**, **`DwmIsCompositionEnabled`** (always `TRUE`), **`DwmGetColorizationColor`** (from `HKCU\Software\Microsoft\Windows\DWM\ColorizationColor`)
  - Custom-titlebar apps draw into the frame region; `MARGINS{-1,-1,-1,-1}` is sheet-of-glass
- [ ] **Custom-frame protocol** exactly as Win11 documents it, because custom-frame apps depend on the byte-level behavior
  - `WM_NCCALCSIZE` with `wParam=TRUE` returning 0 removes the standard frame and keeps the client rect equal to the window rect
  - `WM_NCHITTEST` returning `HTMAXBUTTON` from an app-drawn caption shows the snap-layouts flyout on hover (`08-graphics-ui/TODO-08` snap); `HTCAPTION` drives drag; `WM_NCMOUSEMOVE` / `WM_NCLBUTTONDOWN` routed to the app
- [ ] **`uxtheme.dll`**: `IsThemeActive` / `IsAppThemed` (`TRUE`), `SetWindowTheme(hwnd, L"DarkMode_Explorer" | L"Explorer" | L"", NULL)` flips a window's built-in controls between the dark and light token sets
  - `OpenThemeData(hwnd, L"BUTTON")` / `CloseThemeData` / `DrawThemeBackground(part, state)` / `GetThemeColor` / `DrawThemeText` / `GetThemePartSize` mapped onto the `CTRL_*` renderer for `BUTTON`, `EDIT`, `COMBOBOX`, `LISTBOX`, `SCROLLBAR`, `TAB`, `HEADER`, `PROGRESS`, `TRACKBAR`, `TOOLTIP`, `MENU`, `WINDOW`
  - Undocumented-but-universal dark-mode ordinals real apps import (`SetPreferredAppMode` #135, `AllowDarkModeForWindow` #133, `RefreshImmersiveColorPolicyState` #104, `ShouldAppsUseDarkMode` #132): exported by ordinal and honored
- [ ] **Personalization broadcast**: any key change in the `08-graphics-ui/TODO-03 §9` contract → `WM_SETTINGCHANGE("ImmersiveColorSet")` + `WM_THEMECHANGED` to every top-level window, exactly once each
  - Delivered through §7 cross-process messaging; the broadcast is the only path, so no app polls the Registry
  - `SystemParametersInfo(SPI_GETCLIENTAREAANIMATION / SPI_GETHIGHCONTRAST)` and `GetSysColor` read the same store, so an app that ignores `dwmapi` still gets the right palette
- [ ] Commit: `"win32: dwmapi + uxtheme Win11 opt-in surface, custom-frame protocol, ImmersiveColorSet broadcast"`

**Test checkpoint:** a Tier 7-style test app calling `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` + `SetWindowTheme(L"DarkMode_Explorer")` renders a dark titlebar and dark `BUTTON` / `EDIT` controls while the system is in light mode; `DWMWCP_DONOTROUND` produces square corners; toggling `AppsUseLightTheme` delivers exactly one `WM_SETTINGCHANGE("ImmersiveColorSet")` per top-level window. This section is the gate for `12-user-platform-sdk/TODO-07 §13` (→ XREF).

---

## OS Comparison


| ⭐  | Feature                                       | 🪟 Win11                                                          | 🐧 Linux                                        | 🚀 Impossible OS                                                                |
| --- | --------------------------------------------- | ----------------------------------------------------------------- | ----------------------------------------------- | ------------------------------------------------------------------------------- |
| 💎  | Per-thread MSG ring-buffer queue              | ✅ Win32k.sys per-thread queue; `NtUserGetMessage` blocking       | ❌ No equivalent (event loops are               | ⬜ §1 -- `msg_queue_t` ring (1000 entries); `SYS_WAIT_MESSAGE`                  |
| 💎  | Window class registry                         | ✅ Win32k system + app classes;                                   | ❌ No concept (toolkit-specific)                | ⬜ §2 -- 256 global + 64 per-process                                            |
| 💎  | `DefWindowProc` default message handling      | ✅ `user32!DefWindowProcW`; full WM_* set                         | ❌ Not applicable                               | ⬜ §3 `WM_CLOSE`, `WM_DESTROY→PostQuitMessage`, `WM_SYSCOMMAND`, `WM_NCHITTEST` |
| 💎  | HDC → compositor surface mapping + dirty rect | ✅ Win32k HDC; GDI `SURFOBJ`; dirty-rect                          | ✅ X11 expose events; Wayland damage            | ⬜ §4 -- HDC → `gfx_surface_t *`; `InvalidateRect`                              |
| 💎  | Accelerator tables                            | ✅ `user32!TranslateAccelerator`; PE `RT_ACCELERATOR` resource    | ✅ GDK accelerators; X11 keysym matching        | ⬜ §5 -- PE resource-loaded ACCEL array; `FALT/FSHIFT/FCONTROL/FVIRTKEY`        |
| 💎  | Window subclassing                            | ✅ Full subclassing + property store                              | ✅ GTK subclass; X11 `XChangeProperty`          | ⬜ §6 `GWLP_WNDPROC` chain; `SetProp/GetProp/RemoveProp`; `CallWindowProc`      |
| 💎  | Cross-process `SendMessage` + `WM_COPYDATA`   | ✅ Win32k cross-process; `WM_COPYDATA` kernel-mapped              | ❌ No standard; X11 `XSendEvent` (unsafe)       | ⬜ §7 -- IPC pipe + shmem; 5s                                                   |
| 💎  | `ChooseColor`/`ChooseFont` common dialogs     | ✅ `comdlg32.dll`                                                 | ✅ GTK `GtkColorChooserDialog`/`GtkFontChooser` | ⬜ §8 -- thin Win32 struct adapter over                                         |
| 💎  | Win11 visual opt-in (`dwmapi` / `uxtheme`)    | ✅ `DwmSetWindowAttribute`, `SetWindowTheme`, `ImmersiveColorSet` | ❌ No equivalent (toolkit CSS)                  | ⬜ §9 -- same `CTRL_*` renderer; Microsoft Registry keys; ordinal exports       |

Impossible OS CSRSS delivers a **native kernel-backed Win32 message loop** -- not a
user-space emulation layer. The MSG ring buffer is allocated and managed in kernel memory;
`SYS_WAIT_MESSAGE` uses the real scheduler (`sched_sleep`/`sched_wake`) so a thread
waiting for messages consumes zero CPU. The HDC→`gfx_surface_t` mapping means GDI calls
translate directly to the same `gfx_*` primitives used by the compositor -- no format
conversion overhead.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **Message queue**: `PostMessage(hwnd, WM_USER+1, 42, 0)`; `GetMessage(&msg, NULL, 0, 0)` returns `msg.message == WM_USER+1` and `msg.wParam == 42`; post 1001 messages → 1001st is dropped (overflow) with WARN log
- [ ] **WM_QUIT**: `PostQuitMessage(0)`; next `GetMessage` returns 0; confirm app exits message loop
- [ ] **RegisterClassEx**: register "TestClass" + `WndProc`; duplicate registration returns 0 + `ERROR_CLASS_ALREADY_EXISTS`; `GetClassInfo` returns registered struct; `UnregisterClass` removes it
- [ ] **CreateWindowEx**: `CreateWindowExA(0, "TestClass", "Hello", WS_OVERLAPPEDWINDOW, 100, 100, 400, 300, NULL, NULL, NULL, NULL)` → non-null HWND; `WM_CREATE` in thread queue; `GetWindowRect` returns `{100,100,500,400}`
- [ ] **DefWindowProc**: `WM_CLOSE` on top-level window → `DestroyWindow` called → `WM_DESTROY` queued → `PostQuitMessage(0)` → `GetMessage` returns 0
- [ ] **BeginPaint/EndPaint**: call `InvalidateRect(hwnd, NULL, FALSE)`; confirm `WM_PAINT` arrives in queue; `BeginPaint` returns non-null HDC; `EndPaint` marks compositor dirty; second `GetMessage` does not have `WM_PAINT` (coalesced)
- [ ] **TranslateMessage**: inject `WM_KEYDOWN(VK_A, ...)` with Shift held; `TranslateMessage` posts `WM_CHAR('A')` to queue
- [ ] **Accelerator**: load accelerator with `Ctrl+S → ID_FILE_SAVE`; `TranslateAccelerator` on `WM_KEYDOWN(VK_S, Ctrl)` → posts `WM_COMMAND(ID_FILE_SAVE)`; returns 1
- [ ] **SetWindowLong / GWLP_USERDATA**: `SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0xDEAD)`; `GetWindowLongPtrA(hwnd, GWLP_USERDATA) == 0xDEAD`
- [ ] **SetProp/GetProp**: `SetProp(hwnd, "TestProp", (HANDLE)99)`; `GetProp(hwnd, "TestProp") == (HANDLE)99`; `RemoveProp` → NULL on next get
- [ ] **Cross-process SendMessage**: two processes; process A sends `WM_USER` to process B window; B's `WndProc` called; reply returned to A; confirm A is blocked until B returns
- [ ] **WM_COPYDATA**: process A sends 1024 bytes to B; B's `WndProc` receives correct `cbData=1024` + matching bytes; B cannot modify data (write triggers fault)
- [ ] **ChooseColor**: `ChooseColorA(&cc)` shows color picker dialog; click OK; `cc.rgbResult` contains selected color; Cancel → FALSE returned
- [ ] Commit: `"win32: CSRSS msg queue, class registry, DefWindowProc, HDC painting, accelerators, subclassing, cross-process messaging, ChooseColor/Font"`
