# P0007 — Clipboard

> **Goal:** System clipboard with copy/paste support, Win32 API mapping,
> keyboard shortcuts (Ctrl+C/X/V), and clipboard history (Win+V).

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

---

## 1. System Clipboard

**Prompt:** The clipboard is a single kernel-resident buffer shared between all processes. `clipboard_set(fmt, data, size)` copies data into the buffer with a format tag (TEXT, IMAGE, FILES). `clipboard_get(fmt, buf, max)` retrieves it. Data must be deep-copied on set. Syscalls SYS_CLIPBOARD_SET/GET let user-mode apps access it. Keep it simple: one clipboard entry at a time. After completing all items,sh clean`, and commit as `"kernel: system clipboard"`.


- [ ] Define `clip_format_t` enum: TEXT, IMAGE, FILES
- [ ] Define `clipboard_t` struct (format, data pointer, size)
- [ ] Create `include/clipboard.h` and `src/kernel/clipboard.c`
- [ ] Implement `clipboard_set(fmt, data, size)` — copy data to clipboard buffer
- [ ] Implement `clipboard_get(fmt, buf, max_size)` — read clipboard data
- [ ] Implement `clipboard_has(fmt)` — check if format available
- [ ] Implement `clipboard_clear()` — clear all clipboard data
- [ ] Add `SYS_CLIPBOARD_SET` and `SYS_CLIPBOARD_GET` syscalls
- [ ] Commit: `"kernel: system clipboard"`

---

## 2. Keyboard Shortcuts

**Prompt:** Ctrl+C/X/V must be wired through the keyboard event pipeline to reach the focused control. The flow: keyboard driver → WM key event → check for global shortcuts → dispatch to focused window → focused control handles Ctrl+C by reading its selection and calling `clipboard_set`. Ctrl+X does copy + delete selection. Ctrl+V calls `clipboard_get` and inserts at cursor. Must not conflict with SIGINT in the terminal — SIGINT should only fire when no text is selected. After completing all items,sh clean`, and commit as `"desktop: clipboard keyboard shortcuts"`.


- [ ] Ctrl+C in focused control → copy selection to clipboard
- [ ] Ctrl+X in focused control → cut selection to clipboard
- [ ] Ctrl+V in focused control → paste from clipboard
- [ ] Wire shortcuts through WM → focused window → focused control
- [ ] Commit: `"desktop: clipboard keyboard shortcuts"`

---

## 3. Win32 Clipboard Mapping

- [ ] `OpenClipboard()` / `CloseClipboard()` → no-op
- [ ] `SetClipboardData(CF_TEXT, data)` → `clipboard_set(CLIP_TEXT, ...)`
- [ ] `GetClipboardData(CF_TEXT)` → `clipboard_get(CLIP_TEXT, ...)`
- [ ] `EmptyClipboard()` → `clipboard_clear()`
- [ ] Add to `user32.dll` builtin stub table
- [ ] Commit: `"win32: clipboard API stubs"`

---

## 4. Clipboard History

**Prompt:** Clipboard history keeps the last 25 entries in a ring buffer — each entry is a deep copy plus metadata (format, timestamp, source app name). Win+V opens a popup listing recent entries. Clicking an entry sets it as current and pastes. Configurable via Registry: `HKLM\SYSTEM\Clipboard\HistoryEnabled`, `HKLM\SYSTEM\Clipboard\MaxItems`. After completing all items,sh clean`, and commit as `"desktop: clipboard history (Win+V)"`.


- [ ] Create `src/desktop/clip_history.c`
- [ ] Maintain ring buffer of last 25 clipboard entries
- [ ] Each entry: format, data copy, timestamp, source app name
- [ ] Win+V keyboard shortcut → show clipboard history popup
- [ ] Click an entry → paste it (set as current clipboard, send paste to focused control)
- [ ] "Clear all" button → empty history
- [ ] Registry: `HKLM\SYSTEM\Clipboard\HistoryEnabled`, `HKLM\SYSTEM\Clipboard\MaxItems`
- [ ] Commit: `"desktop: clipboard history (Win+V)"`

---

## Priority Order

| Priority | Section                    | Reason                         |
|----------|----------------------------|--------------------------------|
| 🔴 P0     | §1 System Clipboard        | Core copy/paste infrastructure |
| 🟠 P1     | §2 Keyboard Shortcuts      | Essential UX (Ctrl+C/X/V)      |
| 🟡 P2     | §3 Win32 Clipboard Mapping | Windows app compatibility      |
| 🟢 P3     | §4 Clipboard History       | Win+V power-user feature       |

---

## Key Files

| File                             | Purpose                       |
|----------------------------------|-------------------------------|
| `src/kernel/clipboard.c`         | [NEW] System clipboard buffer |
| `include/clipboard.h`            | [NEW] Clipboard API header    |
| `src/desktop/clip_history.c`     | [NEW] Clipboard history UI    |

---

## OS Comparison

| Feature                         | Windows 11 (Clipboard API)             | Linux (X11 SELECT / Wayland wl_data)    | Impossible OS                             |
|---------------------------------|----------------------------------------|-----------------------------------------|-------------------------------------------|
| System clipboard (text)         | ✅ `SetClipboardData` / `GetClipboardData` | ✅ X11 PRIMARY + CLIPBOARD selections | ⬜ §1 P0 — `clipboard_set/get`           |
| IMAGE format                    | ✅ `CF_DIB` / `CF_BITMAP`              | ✅ `image/png` MIME type                | ⬜ §1 P0 — `CLIP_IMAGE`                  |
| FILES format                    | ✅ `CF_HDROP`                          | ✅ `text/uri-list`                      | ⬜ §1 P0 — `CLIP_FILES`                  |
| Ctrl+C/X/V shortcuts            | ✅ Win32 WM_COPY/WM_CUT/WM_PASTE      | ✅ Toolkit-specific                     | ⬜ §2 P1 — WM route → focused control    |
| Win32 clipboard API             | ✅ Native                              | ✅ xclip / wl-clipboard user-space      | ⬜ §3 P2 — `OpenClipboard/SetClipboardData` stubs |
| Clipboard history (Win+V)       | ✅ Win+V (Windows 10+)                 | ❌ No system clipboard history          | ⬜ §4 P3 — 25-entry ring buffer           |
| Syscall (user-mode access)      | ✅ user32.dll/ntdll                    | ✅ X11 IPC / Wayland protocol           | ⬜ §1 — `SYS_CLIPBOARD_SET/GET`          |
| Multi-format on one write       | ✅ Multiple formats simultaneously     | ✅ MIME target list                     | 🔵 Future (one format at a time for now) |
| **No clipboard manager daemon** | ❌ clipboardhistory.exe service        | ❌ X11 requires clipboard manager       | ✅ **§1 — in-kernel, no daemon**          |
| **Clipboard history**           | ✅ Windows 10+                         | ❌ no system clipboard history          | ⬜ **§4 — beats Linux, matches Win10+**  |
