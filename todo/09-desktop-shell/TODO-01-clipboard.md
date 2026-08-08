---
schema_version: 1
id: clipboard
domain: 09-desktop-shell
status: active
title: "TODO-01 -- Clipboard System"
---

# TODO-01 -- Clipboard System

> **Goal:** Build the kernel clipboard from scratch -- PMM-backed buffer, format enum, syscalls, Ctrl+C/X/V wiring with SIGINT passthrough for terminals, Win32 `SetClipboardData/GetClipboardData` stubs, clipboard history (Win+V popup, 25-entry ring), and multi-format support. This is the P0 prerequisite before text editing, copy/paste in File Manager, terminal selection, and all Win32 clipboard APIs work.

> [!IMPORTANT]
> **Already exists**: `kmalloc(size)` in `include/kernel/mm/heap.h` (≤ 4 KB allocations). `pmm_alloc_contiguous(count)` in `include/kernel/mm/pmm.h` (> 4 KB, page-aligned). `CTRL_TEXTBOX` in `include/desktop/controls.h` (text input widget -- Ctrl+C/V wired here). `wm_get_focused_handle()` from TODO-11 §2 (needed by §7 to identify target control). `context_menu_show()` + `wm_create_window()` (TODO-07/TODO-06) for Win+V history popup. `time_now()` from TODO-10 §1 for history timestamps. Win32 clipboard stubs table in TODO-11's `user32` layer. **Missing**: everything clipboard-related -- no `clipboard_set`, no `CLIP_*` enum, no `SYS_CLIPBOARD_*`, no history, no Ctrl+C/X/V global dispatch. **Syscalls**: `SYS_CLIPBOARD_SET=56`, `SYS_CLIPBOARD_GET=57` (next free after `SYS_TIME=55`). Complete sections in order: kernel buffer → Win32 stubs → multi-format → keyboard wiring → history.

## Inputs

- `include/kernel/mm/heap.h` -- `kmalloc()`, `kfree()` -- used by §1 for ≤ 4 KB clipboard allocations
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()` -- used by §1 for > 4 KB image/file clipboard data
- `include/kernel/sched/syscall.h` -- `SYS_CLIPBOARD_SET=56`, `SYS_CLIPBOARD_GET=57` added in §1
- `include/desktop/controls.h` -- `CTRL_TEXTBOX` -- §2 keyboard wiring dispatches Ctrl+C/V to active textbox control
- `include/desktop/wm.h` -- `wm_get_focused_handle()` (TODO-11 §2), `wm_post_message_all()` -- §7 uses focused window to route copy/paste; §8 Win+V opens popup above focused window
- `include/kernel/gfx/anim_mgr.h` (TODO-02) -- `anim_mgr_add()` -- §5 history popup slide-in animation
- `include/gfx.h` -- `gfx_acrylic()`, `gfx_fill_rounded_rect()` -- §4 history popup background
- `include/desktop/win32/user32.h` (TODO-11) -- `SetClipboardData/GetClipboardData` stub table -- §5 wires Win32 CF_* format IDs to `CLIP_*` enum
- `include/kernel/time.h` (TODO-10 §1) -- `time_now()` -- §4 history entry timestamps
- → XREF: `08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md §4` -- `SetClipboardData`/`GetClipboardData` in the USER32 stub table call into §5 of this TODO
- → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §5` -- screenshot (PrintScreen) calls `clipboard_set_bitmap()` from §1
- Related (no stable XREF target): `10-apps/TODO-*` (terminal, file manager) -- terminal Ctrl+C SIGINT passthrough and File Manager copy/paste depend on §2 keyboard wiring

## Outcome

- `clipboard_set(fmt, data, size)` deep-copies into PMM/kmalloc-backed buffer; `clipboard_get/has/clear()`.
- `SYS_CLIPBOARD_SET=56` + `SYS_CLIPBOARD_GET=57` for user-mode apps.
- Ctrl+C/X/V dispatched to focused textbox control; terminal SIGINT passthrough when no selection.
- Win32 `OpenClipboard/SetClipboardData/GetClipboardData/EmptyClipboard` stubs wired to `CLIP_*`.
- `clipboard_set_multi(entries[], count)` for multi-format (text + rich text stub).
- Win+V opens 25-entry history popup; click entry → restore + paste; "Clear all" button.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                  | Depends On                                                                          | Status |
| --- | :---: | -------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Kernel clipboard buffer -- `clip_format_t`, `clipboard_t`, `set/get/has/clear`, syscalls  | `kmalloc`, `pmm_alloc_contiguous` (both exist)                                      |  [ ]   |
| 💎  |   2   | §3 Win32 clipboard stubs -- `OpenClipboard/SetClipboardData/GetClipboardData/EmptyClipboard` | §1 API must exist; TODO-11 USER32 stub table structure                              |  [ ]   |
| 💎  |   3   | §5 Multi-format clipboard -- `clipboard_set_multi()`, parallel format slots                  | §1 (extends the single-format buffer to multi-slot)                                 |  [ ]   |
| ⭐  |   4   | §2 Keyboard shortcut wiring -- Ctrl+C/X/V to focused control, SIGINT passthrough             | §1 + §3 multi-format (paste can deliver text format); `CTRL_TEXTBOX`                |  [ ]   |
| ⭐  |   5   | §4 Clipboard history -- 25-entry ring, Win+V popup, entry restore + paste                    | §4 keyboard wiring (Win+V hotkey in same dispatch table); §1 `clipboard_set()` hook |  [ ]   |

---

## 1. Kernel Clipboard Buffer `[Sonnet]`

`clip_format_t` enum (`CLIP_TEXT`, `CLIP_IMAGE`, `CLIP_FILES`, `CLIP_HTML`). `struct clipboard_t` (format, `uint8_t *data`, size, source_pid, timestamp). `clipboard_set(fmt, data, size)` deep-copies into PMM/kmalloc-backed buffer. `clipboard_get/has/clear`. `SYS_CLIPBOARD_SET=56` / `SYS_CLIPBOARD_GET=57`.

**Files:** `src/desktop/clipboard.c` (new), `include/desktop/clipboard.h` (new), `include/kernel/sched/syscall.h` (extend)

> [!NOTE]
> Single global clipboard: `static clipboard_t g_clipboard`. Allocation rule: if `size <= 4096` → `kmalloc(size)` for `data`; else → `pmm_alloc_contiguous((size + PAGE_SIZE-1) / PAGE_SIZE)` -- mirrors the `from_pmm` pattern already used in `icon_store.h`. `clipboard_set(fmt, data, size)`: free existing `g_clipboard.data` (using matching free path); `memcpy` new data in; update `g_clipboard.source_pid = current_task->pid`; `g_clipboard.timestamp = time_now()`. **Thread safety**: take a kernel spinlock around `clipboard_set/get/clear` -- clipboard is accessed from WM thread (Ctrl+V paste) and from user-mode via syscall. `clipboard_get(fmt, buf, max)`: if `g_clipboard.format != fmt` → return -1; `memcpy(buf, g_clipboard.data, min(size, max))`; return bytes copied. `clipboard_has(fmt)`: `g_clipboard.format == fmt && g_clipboard.data != NULL`. **Syscalls**: `sys_clipboard_set(fmt, ptr, size)` -- copies user-mode ptr into kernel space then calls `clipboard_set()`; `sys_clipboard_get(fmt, ptr, max)` -- calls `clipboard_get()` then copies to user space.

- [ ] `typedef enum { CLIP_NONE=0, CLIP_TEXT=1, CLIP_IMAGE=2, CLIP_FILES=3, CLIP_HTML=4 } clip_format_t;` in `clipboard.h`
- [ ] `typedef struct { clip_format_t format; uint8_t *data; uint32_t size; uint32_t source_pid; int64_t timestamp; uint8_t from_pmm; } clipboard_t;` in `clipboard.h`
- [ ] `void clipboard_set(clip_format_t fmt, const void *data, uint32_t size)` -- free old; alloc new; copy; update metadata
- [ ] `int clipboard_get(clip_format_t fmt, void *buf, uint32_t max)` -- format check; memcpy; return bytes or -1
- [ ] `int clipboard_has(clip_format_t fmt)` -- quick format check
- [ ] `void clipboard_clear(void)` -- free data; reset to `CLIP_NONE`
- [ ] `void clipboard_set_bitmap(const uint32_t *pixels, uint32_t w, uint32_t h)` -- pack w+h header + pixel data; `clipboard_set(CLIP_IMAGE, ...)`; called from screenshot (TODO-07 §5)
- [ ] `#define SYS_CLIPBOARD_SET 56`, `#define SYS_CLIPBOARD_GET 57` in `syscall.h`; add dispatch handlers; wire into syscall table
- [ ] `spinlock_t g_clipboard_lock` -- protect `g_clipboard` in `clipboard_set/get/clear`
- [ ] Commit: `"clipboard: kernel buffer -- clip_format_t, set/get/has/clear, PMM/kmalloc alloc, SYS_CLIPBOARD_SET/GET=56/57"`

## 2. Keyboard Shortcut Wiring `[Opus]`

Ctrl+C/X/V in global WM key handler → dispatch to focused window's focused control. `CTRL_TEXTBOX` handles: Ctrl+C reads selection → `clipboard_set(CLIP_TEXT,...)`; Ctrl+X = copy + delete selection; Ctrl+V = `clipboard_get` + insert at cursor. Ctrl+C in terminal with no text selection → SIGINT passthrough (do not intercept).

**Files:** `src/desktop/clipboard_shortcuts.c` (new), `src/desktop/controls.c` (extend), `src/desktop/wm.c` (extend)

> [!NOTE]
> This is `[Opus]` -- the keyboard shortcut wiring requires novel dispatch logic with a subtle SIGINT safety gate. The challenge: Ctrl+C must simultaneously (1) copy selected text in any focused text control, (2) pass through as SIGINT to terminal foreground processes when the terminal has focus **and** no text is selected, and (3) do nothing (pass to app via WM_KEYDOWN) when neither applies. This is a three-way decision tree that has never been implemented in Impossible OS. **Decision tree for Ctrl+C in `wm_handle_key()`**: check focused window type; if `is_terminal(win)`: check `terminal_has_text_selection(win)`; if yes → copy selection + consume key; if no → deliver `WM_KEYDOWN Ctrl+C` (terminal handles SIGINT); if not terminal: check if focused control is `CTRL_TEXTBOX` with a selection → `ctrl_copy_selection(ctrl)` → `clipboard_set(CLIP_TEXT, ...)`; else → deliver `WM_KEYDOWN Ctrl+C` to app (let app handle it). **Ctrl+V**: get `CLIP_TEXT` → insert at `ctrl_textbox_cursor_pos`; call `ctrl_insert_text(ctrl, text, len)`. **Ctrl+X**: `ctrl_copy_selection(ctrl)` + `ctrl_delete_selection(ctrl)`. `ctrl_copy_selection(ctrl)`: extract `ctrl->sel_start..sel_end` from `CTRL_TEXTBOX` text buffer. `ctrl_insert_text(ctrl, text, len)`: replace selection or insert at cursor.

- [ ] `void clipboard_handle_copy(void)` -- get focused window + control; if `CTRL_TEXTBOX` with selection: `ctrl_copy_selection()` → `clipboard_set(CLIP_TEXT,...)`; if terminal + selection: same; else: no-op
- [ ] `void clipboard_handle_cut(void)` -- copy + `ctrl_delete_selection(ctrl)`
- [ ] `void clipboard_handle_paste(void)` -- `clipboard_get(CLIP_TEXT, text, MAX_TEXT)`; if non-empty: `ctrl_insert_text(focused_ctrl, text, len)`
- [ ] `int terminal_has_text_selection(int wh)` -- returns 1 if terminal window `wh` has an active text selection (checks terminal selection state); stub returns 0 until terminal TODO is live
- [ ] `ctrl_copy_selection(control_t *ctrl)` -- read `ctrl->text_buf[sel_start..sel_end]`; return pointer + length
- [ ] `ctrl_insert_text(control_t *ctrl, const char *text, uint32_t len)` -- if selection: delete; insert at cursor; `wm_mark_dirty()`
- [ ] `ctrl_delete_selection(control_t *ctrl)` -- remove `text_buf[sel_start..sel_end]`; update cursor
- [ ] WM key handler: in `wm_handle_key()`: if `KEY_C` + Ctrl: `clipboard_handle_copy()`; consume if handled; if `KEY_V` + Ctrl: `clipboard_handle_paste()`; if `KEY_X` + Ctrl: `clipboard_handle_cut()`; **SIGINT gate**: if focused window `is_terminal` and `!terminal_has_text_selection()` → do NOT intercept; deliver WM_KEYDOWN normally
- [ ] Log on each clipboard op: `[clipboard] set TEXT %u bytes from pid %u`, `[clipboard] paste %u bytes → ctrl %p`
- [ ] Commit: `"clipboard: Ctrl+C/X/V wiring -- textbox selection copy/cut/paste, terminal SIGINT passthrough gate"`

## 3. Win32 Clipboard API Stubs `[Sonnet]`

`OpenClipboard(hWnd)` / `CloseClipboard()` no-ops. `SetClipboardData(CF_TEXT, hMem)` → `clipboard_set(CLIP_TEXT,...)`. `GetClipboardData(CF_TEXT)` → `clipboard_get`. `EmptyClipboard()` → `clipboard_clear`. `IsClipboardFormatAvailable(CF)` → `clipboard_has`. `CF_TEXT=1`, `CF_BITMAP=2`, `CF_HDROP=15`.

**Files:** `src/desktop/win32/user32_clip.c` (new), `include/desktop/win32/user32.h` (extend)

> [!NOTE]
> Win32 `HGLOBAL` memory: clipboard data is passed as `HGLOBAL` (a heap pointer). `SetClipboardData(CF_TEXT, hMem)`: `hMem` is a `char *` in Win32 when `CF_TEXT`; dereference as `const char *text = (const char *)hMem`; call `clipboard_set(CLIP_TEXT, text, kstrlen(text)+1)`. `GetClipboardData(CF_TEXT)`: `clipboard_get(CLIP_TEXT, g_clip_out_buf, MAX_CLIP)`; return `g_clip_out_buf` as `HGLOBAL` (static buffer per process; safe for short-lived Win32 use). CF format map: `CF_TEXT=1` → `CLIP_TEXT`; `CF_BITMAP=2` → `CLIP_IMAGE`; `CF_HDROP=15` → `CLIP_FILES`; `CF_UNICODETEXT=13` → `CLIP_TEXT` (UTF-16 to UTF-8 conversion stub -- for now treat as UTF-8 passthrough). `OpenClipboard/CloseClipboard`: track `g_clip_owner_hwnd`; `OpenClipboard(hWnd)` sets owner; `CloseClipboard()` clears; actual data access does not require locking (single-threaded WM).

- [ ] `#define CF_TEXT 1`, `CF_BITMAP 2`, `CF_METAFILEPICT 3`, `CF_UNICODETEXT 13`, `CF_HDROP 15` in `user32.h`
- [ ] `static HWND g_clip_owner_hwnd = 0;` in `user32_clip.c`
- [ ] `int OpenClipboard(HWND hWnd)` -- `g_clip_owner_hwnd = hWnd`; return 1
- [ ] `int CloseClipboard(void)` -- `g_clip_owner_hwnd = 0`; return 1
- [ ] `int EmptyClipboard(void)` → `clipboard_clear()`; return 1
- [ ] `void *SetClipboardData(uint32_t fmt, void *hMem)` -- map CF_* to CLIP_*; call `clipboard_set()`; return `hMem`
- [ ] `void *GetClipboardData(uint32_t fmt)` -- map CF_* to CLIP_*; call `clipboard_get()` into static `g_clip_out_buf[65536]`; return ptr or NULL
- [ ] `int IsClipboardFormatAvailable(uint32_t fmt)` -- map CF_* to CLIP_*; return `clipboard_has()`
- [ ] `int CountClipboardFormats(void)` → 1 if `g_clipboard.format != CLIP_NONE`; 0 otherwise
- [ ] CF_UNICODETEXT stub: if `SetClipboardData(CF_UNICODETEXT, ...)`: interpret as UTF-8 passthrough (log warning); forward to `CLIP_TEXT`
- [ ] Commit: `"win32: clipboard stubs -- OpenClipboard/SetClipboardData/GetClipboardData/IsClipboardFormatAvailable wired to CLIP_*"`

## 4. Clipboard History `[Sonnet]`

`src/desktop/clip_history.c`: ring buffer of last 25 entries (deep copy + format + timestamp + source app name). Maintained on every `clipboard_set()`. Win+V → popup listing recent entries (truncated text or image thumbnail). Click → restore to active clipboard + send Ctrl+V to focused window. "Clear all". Registry: `HKLM\SYSTEM\Clipboard\HistoryEnabled` (default 1), `MaxItems` (default 25).

**Files:** `src/desktop/clip_history.c` (new), `include/desktop/clip_history.h` (new)

> [!NOTE]
> Ring buffer: `clip_history_entry_t g_history[CLIP_HISTORY_MAX]` + `g_history_head`, `g_history_count`. On `clipboard_set()`: if `HistoryEnabled`: deep-copy data into new `clip_history_entry_t.data` (kmalloc/PMM same rules as §1); record `format`, `timestamp = time_now()`, `source_app_name = current_task->name`. Oldest entry freed when ring wraps. **Win+V popup**: `wm_create_window(NULL, fb_w/2 - 160, fb_h/2 - 200, 320, 400, WM_FLAG_VISIBLE)` at z_order=35000; Acrylic bg; list entries newest-first; text entries: truncated at 48 chars + trailing `…`; image entries: scaled 40×40 px thumbnail. **Click to restore**: `clipboard_set(entry->format, entry->data, entry->size)`; `clipboard_handle_paste()` to insert into focused control; close popup. **"Clear all" button**: `clip_history_clear()` -- free all entries; rebuild empty popup. Win+V hotkey: in global hotkey dispatch table (TODO-06 `hotkeys.c`): `MOD_WIN + KEY_V` → `clip_history_popup_toggle()`.

- [ ] `typedef struct { clip_format_t format; uint8_t *data; uint32_t size; uint8_t from_pmm; int64_t timestamp; char app_name[32]; } clip_history_entry_t;` in `clip_history.h`
- [ ] `#define CLIP_HISTORY_MAX 25`
- [ ] `void clip_history_init(void)`: read `HKLM\SYSTEM\Clipboard\HistoryEnabled` + `MaxItems`; set `g_history_enabled`, `g_history_max` (clamp to 25)
- [ ] `void clip_history_push(const clipboard_t *cb)`: if `!g_history_enabled` return; alloc new entry; deep copy; advance ring head; free overwritten entry's data
- [ ] Hook into `clipboard_set()`: after updating `g_clipboard`: `clip_history_push(&g_clipboard)`
- [ ] `void clip_history_popup_open(void)`: create Acrylic window at z_order=35000; render list of entries (newest first); draw truncated text or 40×40 thumbnail
- [ ] `void clip_history_popup_close(void)`: `wm_destroy_window(popup_wh)`
- [ ] Entry click: `clipboard_set(entry->format, entry->data, entry->size)`; `clipboard_handle_paste()`; `clip_history_popup_close()`
- [ ] "Clear all" button: `clip_history_clear()` → free all entries; zero ring; re-render popup to empty state
- [ ] `clip_history_format_timestamp(entry->timestamp, buf, 16)` -- "X min ago" / "HH:MM" using `time_to_datetime()` (TODO-10 §1)
- [ ] Commit: `"clipboard: history -- 25-entry ring, Win+V Acrylic popup, restore+paste, clear all"`

## 5. Multi-Format Clipboard `[Sonnet]`

`clipboard_set_multi(entries[], count)` stores up to 4 simultaneous format slots. On single `clipboard_set()`, also derive and store a plain-text rendering. Rich text stub for future use. `clipboard_get_best(preferred[], n_preferred)` returns highest-priority available format.

**Files:** `src/desktop/clipboard.c` (extend), `include/desktop/clipboard.h` (extend)

> [!NOTE]
> Extend `g_clipboard` to hold `#define CLIP_MAX_FORMATS 4` slots: `clip_slot_t g_slots[CLIP_MAX_FORMATS]` where `clip_slot_t = { clip_format_t format; uint8_t *data; uint32_t size; uint8_t from_pmm; }`. `clipboard_set(fmt, data, size)` (single-format): clear all slots; set `g_slots[0]`; if `fmt == CLIP_HTML`: also derive and store plain-text in `g_slots[1]` (strip tags with a simple `<[^>]+>` loop -- no regex, hand-coded). `clipboard_set_multi(entries[], count)`: clear all slots; copy each entry up to `CLIP_MAX_FORMATS`. `clipboard_get(fmt, buf, max)`: search `g_slots[]` for matching format; return first match. `clipboard_get_best(formats[], n)`: iterate `formats[]` in priority order; return first format found in `g_slots[]`. `clipboard_has(fmt)`: scan all slots.

- [ ] `typedef struct { clip_format_t format; uint8_t *data; uint32_t size; uint8_t from_pmm; } clip_slot_t;`
- [ ] Replace single `clipboard_t.data` with `clip_slot_t g_slots[CLIP_MAX_FORMATS]` in `clipboard.c` (refactor §1 to use slots; backward-compatible: single-format callers unchanged)
- [ ] `void clipboard_set_multi(const clip_slot_t *entries, int count)` -- clear all slots; copy up to `CLIP_MAX_FORMATS` entries (deep-copy each)
- [ ] `int clipboard_get_best(const clip_format_t *preferred, int n, void *buf, uint32_t max)` -- return first preferred format found; return actual format via return value; -1 if none
- [ ] HTML → plain text derivation in `clipboard_set(CLIP_HTML, ...)`: `clip_strip_html_tags(html, plain, max)` → store `CLIP_TEXT` as second slot
- [ ] `clip_slot_t stub { CLIP_HTML+1, NULL, 0 }` reserved for future "rich text" format slot
- [ ] Update `clipboard_has(fmt)` to scan all `g_slots[]`; update `clipboard_clear()` to free all slots
- [ ] Commit: `"clipboard: multi-format -- 4-slot CLIP_MAX_FORMATS, clipboard_set_multi(), clipboard_get_best(), HTML→text derive"`

---

## OS Comparison


| ⭐  | Feature                 | 🪟 Win11                                                 | 🐧 Linux                                                   | 🚀 Impossible OS                                                               |
| --- | ----------------------- | -------------------------------------------------------- | ---------------------------------------------------------- | ------------------------------------------------------------------------------ |
| 💎  | Kernel clipboard buffer | ✅ Global clipboard via Win32 `SetClipboardData`;        | ✅ X11 selection atoms (PRIMARY/CLIPBOARD); Wayland        | ⬜ §1 single global `g_clipboard`, spinlock-protected; `SYS_CLIPBOARD_SET/GET` |
| ⭐  | Ctrl+C/X/V wiring       | ✅ Win32 apps handle WM_COPY/PASTE; no                   | ✅ X11 selection; terminal emulators handle                | ⬜ §2 -- `⭐` kernel decision tree: terminal+no-selection                      |
| 💎  | Win32 clipboard stubs   | ✅ Full Win32 clipboard API (comdlg32/user32);           | ✅ Wine clipboard emulation; GTK/Qt clipboard              | ⬜ §3 -- CF_TEXT/BITMAP/HDROP/UNICODETEXT → `CLIP_*` forwarding table          |
| ⭐  | Clipboard history       | ✅ Win+V clipboard history (since Win10                  | ✅ `CopyQ`, `Parcellite`, `Clipman` (third-party daemons); | ⬜ §4 -- `⭐` built into kernel shell                                          |
| 💎  | Multi-format            | ✅ Multiple clipboard formats registered simultaneously; | ✅ X11 `TARGETS` atom; multiple MIME                       | ⬜ §5 -- `CLIP_MAX_FORMATS=4`; `clipboard_get_best()` priority order; HTML     |

> **After §1–§5:** Impossible OS has a complete clipboard stack built directly into the kernel shell, with no clipboard daemon. The dual `⭐` differentiators are (1) the SIGINT passthrough gate -- the kernel correctly distinguishes "Ctrl+C to copy" from "Ctrl+C to signal" without a terminal ever needing to intercept or re-implement copy shortcuts; and (2) clipboard history with app attribution (`current_task->name`) sourced at the kernel level -- no IPC or daemon required, just a ring buffer hooked into `clipboard_set()`.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `clipboard_set(CLIP_TEXT, "Hello", 6)`; `clipboard_get(CLIP_TEXT, buf, 64)` → `buf == "Hello"`, returns 6
- [ ] `clipboard_has(CLIP_TEXT)` → 1; `clipboard_has(CLIP_IMAGE)` → 0; `clipboard_clear()`; `clipboard_has(CLIP_TEXT)` → 0
- [ ] `SYS_CLIPBOARD_SET` from user-mode hello.exe: set text; `SYS_CLIPBOARD_GET` from another process: retrieves same text
- [ ] Focus a textbox; select some text; Ctrl+C → `clipboard_get(CLIP_TEXT, ...)` returns selected text; click another textbox; Ctrl+V → text inserted at cursor
- [ ] Focus terminal with no text selection; Ctrl+C → SIGINT delivered to foreground process (not clipboard op); serial log confirms passthrough
- [ ] Focus terminal with text selected; Ctrl+C → text copied to clipboard (not SIGINT); serial log: `[clipboard] set TEXT N bytes from pid X`
- [ ] `Win32 SetClipboardData(CF_TEXT, "Win32 text")` → `clipboard_get(CLIP_TEXT, ...)` returns "Win32 text"; `GetClipboardData(CF_TEXT)` returns pointer to the text
- [ ] Win+V → history popup opens; shows last 3 clipboard entries newest-first; click entry 2 → restores and pastes into focused textbox
- [ ] `clipboard_set(CLIP_HTML, "<b>Bold</b>", 11)` → `clipboard_has(CLIP_TEXT)` → 1 (HTML→text derived); `clipboard_get(CLIP_TEXT, ...)` → "Bold" (tags stripped)
- [ ] Commit: `"clipboard: full clipboard stack -- buffer, shortcuts, Win32 stubs, history, multi-format"`
