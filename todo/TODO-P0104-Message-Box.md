# P0104 — Message Box System

> **Goal:** Implement a Win32-compatible MessageBox API with embedded BGRA icons
> (error, warning, info, question) compiled into the kernel. The API uses
> Win32-compatible constants (`MB_OK`, `MB_ICONERROR`, `IDOK`, etc.) so the
> The native Win32 API (P0105) exports this directly, and native PE programs
> call the same ABI. The message box renders as a modal dialog using the
> existing window manager, compositor, TrueType fonts, and gfx primitives.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

> [!TIP]
> **Design Principle:** By matching the Win32 MessageBox ABI at the native level,
> Win32 apps call `MessageBoxA/W` → shim passes `uType` directly to `MessageBox()`.
> Native PE apps call `MessageBox()` directly. One implementation serves all.

**Cross-references:**
- **P0105** §7.1: `MessageBoxA/W` → native pass-through to this API
- **P0105** Native: `MessageBox()` available via `SYS_MSGBOX` syscall
---

## 1. Embedded Message Box Icons

> Icons are compiled directly into the kernel binary as BGRA `uint32_t` arrays,
> exactly like `boot_splash_icon.h`. No filesystem I/O required — icons are
> available even during early boot errors or disk failures.

### 1.1 Generate Icon Assets

**Prompt:** Generate four 48×48 Windows 11-style message box icons using the `generate_image` tool: (1) **Error** — red circle with white ✕, (2) **Warning** — yellow triangle with black ❗, (3) **Info** — blue circle with white ℹ, (4) **Question** — blue circle with white ❔. Each icon should have a transparent background, be modern and flat (not skeuomorphic), with anti-aliased edges. Save the generated PNGs to `assets/icons/msgbox/`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"assets: message box icon PNGs"`.


- [ ] Generate error icon (red circle, white ✕) — `assets/icons/msgbox/error.png`
- [ ] Generate warning icon (yellow triangle, black ❗) — `assets/icons/msgbox/warning.png`
- [ ] Generate info icon (blue circle, white ℹ) — `assets/icons/msgbox/info.png`
- [ ] Generate question icon (blue circle, white ❔) — `assets/icons/msgbox/question.png`
- [ ] Commit: `"assets: message box icon PNGs"`

### 1.2 Convert Icons to C Arrays

**Prompt:** Create `tools/png2header.py` (or extend `tools/convert_icon.py`) to convert each 48×48 PNG into a C header with a `static const uint32_t` array in BGRA pixel format (matching framebuffer byte order), identical to how `src/kernel/boot_splash_icon.h` is structured. Generate four headers: `src/desktop/msgbox_icon_error.h`, `msgbox_icon_warning.h`, `msgbox_icon_info.h`, `msgbox_icon_question.h`. Each header defines `MSGBOX_ICON_{TYPE}_W`, `MSGBOX_ICON_{TYPE}_H`, and `msgbox_icon_{type}_pixels[]`. Add a Makefile rule to auto-regenerate these headers from the PNGs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"tools: PNG to C header converter for msgbox icons"`.


- [ ] Create/extend `tools/convert_icon.py` to handle arbitrary PNG → BGRA C array
- [ ] Generate `src/desktop/msgbox_icon_error.h` (48×48, BGRA `uint32_t[2304]`)
- [ ] Generate `src/desktop/msgbox_icon_warning.h`
- [ ] Generate `src/desktop/msgbox_icon_info.h`
- [ ] Generate `src/desktop/msgbox_icon_question.h`
- [ ] Add Makefile rule: `make msgbox-icons` regenerates all four headers from PNGs
- [ ] Verify: each header matches `boot_splash_icon.h` format (`#pragma once`, dimensions, pixel array)
- [ ] Commit: `"tools: PNG to C header converter for msgbox icons"`

---

## 2. Message Box API (Win32-Compatible)

> Uses Win32-compatible constants so the native API and Win32 shim
> share the same underlying implementation. Win32 programs call
> `MessageBoxA/W` which maps directly to our `MessageBox()` — zero translation.

### 2.1 API Header and Types

**Prompt:** Create `include/desktop/msgbox.h` with a Win32-compatible MessageBox API. Use the standard Win32 constants:

**Button flags** (`uType` low nibble): `MB_OK` (0x00), `MB_OKCANCEL` (0x01), `MB_ABORTRETRYIGNORE` (0x02), `MB_YESNOCANCEL` (0x03), `MB_YESNO` (0x04), `MB_RETRYCANCEL` (0x05).

**Icon flags** (`uType` byte 1): `MB_ICONERROR` / `MB_ICONHAND` (0x10), `MB_ICONQUESTION` (0x20), `MB_ICONWARNING` / `MB_ICONEXCLAMATION` (0x30), `MB_ICONINFORMATION` / `MB_ICONASTERISK` (0x40).

**Default button** (`uType` byte 2): `MB_DEFBUTTON1` (0x00), `MB_DEFBUTTON2` (0x100), `MB_DEFBUTTON3` (0x200).

**Modal flags**: `MB_APPLMODAL` (0x00), `MB_SYSTEMMODAL` (0x1000), `MB_TASKMODAL` (0x2000).

**Return values**: `IDOK` (1), `IDCANCEL` (2), `IDABORT` (3), `IDRETRY` (4), `IDIGNORE` (5), `IDYES` (6), `IDNO` (7).

Declare: `int MessageBox(void *hWnd, const char *lpText, const char *lpCaption, unsigned int uType)`. This signature matches Win32 exactly. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: Win32-compatible MessageBox API header"`.


- [ ] Create `include/desktop/msgbox.h`
- [ ] Define button flags: `MB_OK`, `MB_OKCANCEL`, `MB_YESNOCANCEL`, `MB_YESNO`, `MB_RETRYCANCEL`, `MB_ABORTRETRYIGNORE`
- [ ] Define icon flags: `MB_ICONERROR`, `MB_ICONQUESTION`, `MB_ICONWARNING`, `MB_ICONINFORMATION`
- [ ] Define default button flags: `MB_DEFBUTTON1`, `MB_DEFBUTTON2`, `MB_DEFBUTTON3`
- [ ] Define modal flags: `MB_APPLMODAL`, `MB_SYSTEMMODAL`, `MB_TASKMODAL`
- [ ] Define return values: `IDOK`, `IDCANCEL`, `IDABORT`, `IDRETRY`, `IDIGNORE`, `IDYES`, `IDNO`
- [ ] Declare `int MessageBox(void *hWnd, const char *lpText, const char *lpCaption, unsigned int uType)`
- [ ] Commit: `"desktop: Win32-compatible MessageBox API header"`

### 2.2 Message Box Renderer

**Prompt:** Implement `src/desktop/msgbox.c`. The `MessageBox()` function extracts button type from `uType & 0x0F`, icon from `uType & 0xF0`, and default button from `uType & 0xF00`. Creates a modal window (approximately 420×200px, centered on screen) using `wm_create()`. Layout: 16px padding, icon (48×48) on the left at (16, 40), caption text (bold, 14px Selawik/UI font) at (80, 20), body text (regular, 13px) at (80, 48) with word wrapping, buttons right-aligned at the bottom (32px tall, 80px wide, 8px spacing). The icon is selected from the embedded BGRA arrays based on `MB_ICON*` flags. Buttons are styled with rounded corners, hover highlight, and accent color for the default button. The dialog paints a semi-transparent overlay (50% black) over all other windows for `MB_SYSTEMMODAL`. The function blocks (polling mouse/keyboard events) until a button is clicked, then destroys the window and returns `IDOK`/`IDCANCEL`/`IDYES`/`IDNO`/`IDRETRY`/`IDABORT`/`IDIGNORE`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: MessageBox renderer"`.


- [ ] Create `src/desktop/msgbox.c`
- [ ] `MessageBox()` parses `uType` bitmask: buttons, icon, default, modal
- [ ] Create centered modal window via `wm_create()`
- [ ] Draw semi-transparent overlay (50% black) for `MB_SYSTEMMODAL`
- [ ] Blit embedded icon (48×48 BGRA) based on `MB_ICON*` flags
- [ ] Render caption text (bold, 14px) and body text (regular, 13px, word-wrapped)
- [ ] Render buttons (right-aligned, 80×32px, rounded corners)
  - [ ] Button set selected by `uType & 0x0F` (OK, OK/Cancel, Yes/No, etc.)
  - [ ] Default button set by `MB_DEFBUTTON*` flags, gets accent color
  - [ ] Hover highlight effect
- [ ] Block and poll input events until button clicked
- [ ] Return `IDOK`, `IDCANCEL`, `IDYES`, `IDNO`, `IDRETRY`, `IDABORT`, `IDIGNORE`
- [ ] Destroy window and remove overlay on close
- [ ] Commit: `"desktop: MessageBox renderer"`

### 2.3 Keyboard Support

**Prompt:** Add keyboard support to the message box: Enter activates the default (focused) button, Escape activates Cancel (or closes if no Cancel button), Tab cycles focus between buttons, and the first letter of a button label (&underline convention) activates it directly. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: msgbox keyboard support"`.


- [ ] Enter → activate focused/default button
- [ ] Escape → activate Cancel (or close if no Cancel)
- [ ] Tab / Shift+Tab → cycle button focus
- [ ] Underline/accelerator key support (e.g., Alt+Y for Yes)
- [ ] Commit: `"desktop: msgbox keyboard support"`

---

## 3. Integration and Testing

### 3.1 Kernel Integration

**Prompt:** Wire the MessageBox into the kernel. Add `#include "desktop/msgbox.h"` to `main.c`. Add a test call in the boot sequence after desktop init: `MessageBox(NULL, "Impossible OS loaded successfully.", "Welcome", MB_OK | MB_ICONINFORMATION)`. Also use `MessageBox()` for critical error paths: AHCI init failure, filesystem mount failure, heap exhaustion (with `MB_ICONERROR`). Add a `SYS_MSGBOX` syscall that accepts the same Win32-compatible `uType` flags so native user-mode PE programs and Win32 programs (via PE loader) can invoke message boxes. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: MessageBox integration"`.


- [ ] Add test `MessageBox()` call in `main.c` after desktop init
- [ ] Use `MessageBox()` for critical error paths (AHCI fail, FS mount fail, heap exhaustion)
- [ ] Add `SYS_MSGBOX` syscall — passes `uType` directly (Win32-compatible flags)
- [ ] Shell command: `msgbox <title> <message> [error|warning|info|question]`
- [ ] Commit: `"kernel: MessageBox integration"`

### 3.2 Build and Test

**Prompt:** Build the full OS with `bash scripts/build.sh clean` and test with `bash scripts/build.sh run`. Verify: (1) Boot log shows no errors. (2) The test message box appears after desktop loads with correct icon, text, and button. (3) Clicking OK dismisses the dialog. (4) The overlay darkens the background correctly. (5) Keyboard shortcuts (Enter, Escape) work. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"desktop: msgbox tested and verified"`.


- [ ] `bash scripts/build.sh clean` → BUILD OK
- [ ] `bash scripts/build.sh run` → QEMU shows message box after desktop loads
- [ ] Verify icon renders correctly (48×48, no artifacts)
- [ ] Verify text renders with TrueType fonts
- [ ] Verify button clicks return correct result
- [ ] Verify keyboard: Enter=OK, Escape=Cancel, Tab=cycle
- [ ] Verify overlay darkens background
- [ ] Commit: `"desktop: msgbox tested and verified"`

---

## 4. Documentation

**Prompt:** Create or update `docs/architecture/msgbox.md` documenting the message box API, icon embedding process, modal rendering, and button types. Update `README.md` if it references dialogs or message boxes. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"docs: message box system"`.


- [ ] Create `docs/architecture/msgbox.md`
- [ ] Document API: `MessageBox()` signature, `MB_*` flags, `ID*` return values
- [ ] Document Win32 compatibility: flag values match Win32 exactly
- [ ] Document icon embedding: PNG → C array → compiled into kernel
- [ ] Document modal rendering and input blocking
- [ ] Document shim strategy: Win32 pass-through (PE is the native format)
- [ ] Update `README.md` if needed
- [ ] Commit: `"docs: message box system"`

---

## Priority Order

1. **§1.1** Generate icon PNGs (need assets first)
2. **§1.2** Convert to C arrays (embedded headers)
3. **§2.1** API header and types (Win32-compatible constants)
4. **§2.2** Message box renderer (main implementation)
5. **§2.3** Keyboard support
6. **§3.1** Kernel integration
7. **§3.2** Build and test
8. **§4** Documentation

---

## Key Files

| File | Purpose |
|------|---------|
| `include/desktop/msgbox.h` | [NEW] Win32-compatible API: `MB_*` flags, `ID*` returns, `MessageBox()` |
| `src/desktop/msgbox.c` | [NEW] Renderer: window, icon, text, buttons |
| `src/desktop/msgbox_icon_error.h` | [NEW] Embedded 48×48 BGRA error icon |
| `src/desktop/msgbox_icon_warning.h` | [NEW] Embedded 48×48 BGRA warning icon |
| `src/desktop/msgbox_icon_info.h` | [NEW] Embedded 48×48 BGRA info icon |
| `src/desktop/msgbox_icon_question.h` | [NEW] Embedded 48×48 BGRA question icon |
| `assets/icons/msgbox/*.png` | [NEW] Source PNG icons |
| `tools/convert_icon.py` | [MODIFY] PNG → C header converter |
| `src/kernel/main.c` | [MODIFY] Test call, error paths |
| `docs/architecture/msgbox.md` | [NEW] Documentation |
