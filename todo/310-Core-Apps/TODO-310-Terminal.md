# P0503 — Terminal Emulator

> **Goal:** Full-featured terminal with ANSI escape codes, scrollback,
> text selection, copy/paste, and configurable appearance.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Terminal Emulator

### 1.1 Terminal Core

**Prompt:** The terminal emulator bridges the shell to a graphical window. The core data structure is a 2D grid of `terminal_cell` structs, each holding a Unicode codepoint, foreground/background colors, and attribute flags (bold, underline, inverse). The grid has `TERM_COLS × SCROLLBACK` cells. Cell dimensions are calculated from Cascadia Code monospace font metrics (Phase 02 §2). The shell process writes to the terminal via `terminal_put_char()`. Keyboard input from the WM is piped to the shell's stdin. After completing all items,sh clean`, and commit as `"apps: terminal emulator core"`.


- [ ] Create `src/apps/terminal/terminal.c` and `include/terminal.h`
- [ ] Define `struct terminal_cell` (codepoint, fg_color, bg_color, attrs: bold/underline/inverse)
- [ ] Define `struct terminal` (grid, cursor, scroll state, ANSI parser, font, selection)
- [ ] Grid: `TERM_COLS × SCROLLBACK` cells (100 cols × 500 scrollback rows)
- [ ] Calculate `cell_w` / `cell_h` from Cascadia Code monospace font
- [ ] Spawn `shell.exe` as child process (or in-process initially)
- [ ] Pipe shell stdout → `terminal_put_char()` → grid
- [ ] Pipe keyboard input → shell stdin
- [ ] Commit: `"apps: terminal emulator core"`

### 1.2 Character Rendering

**Prompt:** `terminal_render` iterates visible rows, drawing each cell: fill background rect if non-default color, then draw the character glyph via `font_draw_char()` using Cascadia Code from the font manager (Phase 02 §2). The cursor (block/underline/bar style) blinks on a 500ms toggle driven by PIT ticks. Handle control characters: `\n` (newline + scroll if at bottom), `\r` (carriage return), `\b` (backspace), `\t` (tab to next 8-column stop). After completing all items,sh clean`, and commit as `"apps: terminal text rendering"`.


- [ ] `terminal_render(t, surface)` — draw all visible cells
- [ ] Draw cell background if non-default color
- [ ] Draw character glyph via `font_draw_char()` (Cascadia Code)
- [ ] Render cursor: block (default), underline, or bar style
- [ ] Cursor blinking animation (toggle every 500ms via PIT ticks)
- [ ] Handle: newline (`\n`), carriage return (`\r`), backspace (`\b`), tab (`\t`)
- [ ] Commit: `"apps: terminal text rendering"`

### 1.3 ANSI Escape Code Parser

**Prompt:** The ANSI parser is a state machine: Normal state processes printable characters, ESC (`\e`) enters escape state, `[` after ESC enters CSI (Control Sequence Introducer) state where numeric parameters are collected until a final command letter. Implement the essential SGR (Select Graphic Rendition) codes: reset (0), bold (1), underline (4), inverse (7), foreground colors 30-37 and 90-97, background colors 40-47. Cursor movement codes (H, A/B/C/D) and screen clearing (2J, K) are needed for programs like vim-lite or top. Define a 16-color palette matching standard ANSI colors. After completing all items,sh clean`, and commit as `"apps: terminal ANSI escape codes"`.


- [ ] Create `src/apps/terminal/ansi.c`
- [ ] State machine: Normal → ESC (`\e`) → CSI (`[`) → parameters → command
- [ ] `\e[0m` — reset attributes
- [ ] `\e[1m` — bold, `\e[4m` — underline, `\e[7m` — inverse
- [ ] `\e[30-37m` / `\e[40-47m` — 8 foreground/background colors
- [ ] `\e[90-97m` — bright foreground colors
- [ ] `\e[H` — cursor position (row, col)
- [ ] `\e[2J` — clear screen, `\e[K` — clear to end of line
- [ ] `\e[A/B/C/D` — cursor up/down/right/left
- [ ] Define 16-color ANSI palette (black through bright white)
- [ ] *(Stretch)* `\e[38;5;Nm` — 256-color extended palette
- [ ] Commit: `"apps: terminal ANSI escape codes"`

### 1.4 Scrollback & Selection

**Prompt:** Scrollback lets users review past output. The grid stores 500 history rows above the visible viewport. Mouse wheel scrolls the viewport up/down through this history. A scrollbar on the right side shows the viewport position. Text selection: click + drag highlights cells (tracked as start_row/col to end_row/col). The selection highlight inverts foreground/background colors. Ctrl+Shift+C copies the selected text to the clipboard (Phase 03 §3.1). Ctrl+Shift+V pastes clipboard text into the shell's stdin. Note: use Ctrl+Shift variants to avoid conflicting with Ctrl+C (SIGINT) in the shell. After completing all items,sh clean`, and commit as `"apps: terminal scrollback and copy/paste"`.


- [ ] Scrollback buffer: 500 lines of history above the visible viewport
- [ ] Mouse wheel → scroll up/down through history
- [ ] Scroll bar on right side
- [ ] Mouse click + drag → text selection
- [ ] Ctrl+Shift+C → copy selection to clipboard
- [ ] Ctrl+Shift+V → paste from clipboard to shell stdin
- [ ] Commit: `"apps: terminal scrollback and copy/paste"`

### 1.5 Registry Settings

**Prompt:** Terminal appearance is configurable via Registry: `HKCU\Software\Impossible\Terminal\FontName` (default "Cascadia Code"), `HKCU\Software\Impossible\Terminal\FontSize` (14), `HKCU\Software\Impossible\Terminal\CursorStyle` ("block"/"underline"/"bar"), `HKCU\Software\Impossible\Terminal\CursorBlink` (REG_DWORD), `HKCU\Software\Impossible\Terminal\Opacity` (0-100 for Acrylic transparency), `HKCU\Software\Impossible\Terminal\ScrollbackLines` (500). Read these values on terminal creation. Recalculate grid columns/rows when the window is resized (new cols = window_width / cell_w). After completing all items,sh clean`, and commit as `"apps: terminal settings"`.


- [ ] Font name: `Apps\Terminal\FontName` (default "Cascadia Code")
- [ ] Font size: `Apps\Terminal\FontSize` (default 14)
- [ ] Cursor style: `Apps\Terminal\CursorStyle` (block/underline/bar)
- [ ] Cursor blink: `Apps\Terminal\CursorBlink` (1/0)
- [ ] Opacity: `Apps\Terminal\Opacity` (0–100, for Acrylic transparency)
- [ ] Scrollback lines: `Apps\Terminal\ScrollbackLines` (default 500)
- [ ] Recalculate cols/rows on window resize
- [ ] Commit: `"apps: terminal settings"`

### 1.6 Advanced Features (Future)

**Prompt:** Stretch goals for a premium terminal: multiple tabs (tab bar at top, each tab is a separate terminal session), Acrylic transparency background (composite the desktop behind the terminal surface at reduced alpha via `gfx_acrylic()`), split panes (divide the terminal window horizontally or vertically into independent sessions), and saved color scheme profiles stored in Registry. After completing all items,sh clean`, and commit as `"apps: terminal advanced features"`.


- [ ] *(Stretch)* Multiple tabs — tabbed terminal sessions
- [ ] *(Stretch)* Acrylic transparency background via `gfx_acrylic()`
- [ ] *(Stretch)* Split panes — vertical/horizontal
- [ ] *(Stretch)* Saved profiles/themes (color schemes)

---

## Priority Order

| Priority | Section                         | Reason                                          |
|----------|---------------------------------|-------------------------------------------------|
| 🔴 P0    | §1.1 Terminal Core              | Foundation — grid, cell struct, shell pipe      |
| 🔴 P0    | §1.2 Character Rendering        | No terminal without text on screen              |
| 🔴 P0    | §1.3 ANSI Escape Codes          | Required by shell prompt + most CLI apps        |
| 🟠 P1    | §1.4 Scrollback + Selection     | Copy/paste from terminal — essential dev UX     |
| 🟠 P1    | §1.5 Registry Settings          | Font size, cursor style, opacity                |
| 🔵 P4    | §1.6 Advanced Features          | Tabs, split panes, profiles (stretch)           |

---

## Key Files

| File                               | Purpose                                   |
|------------------------------------|-------------------------------------------|
| `src/apps/terminal/terminal.c`     | [NEW] Terminal core (grid, rendering)     |
| `include/terminal.h`               | [NEW] Terminal API header                 |
| `src/apps/terminal/ansi.c`         | [NEW] ANSI escape code state machine      |

---

## OS Comparison

| Feature                         | Windows 11 (Windows Terminal)       | Linux (GNOME Terminal / Alacritty)    | Impossible OS                          |
|---------------------------------|-------------------------------------|---------------------------------------|----------------------------------------|
| Terminal cell grid (2D buffer)  | ✅ VT/conPTY                        | ✅ PTY / VTE                           | ⬜ §1.1 P0 — `terminal_cell` grid     |
| TrueType font rendering         | ✅ DirectWrite                       | ✅ Pango/Cairo or alacritty's GPU      | ⬜ §1.2 P0 — stb_truetype (Cascadia Code) |
| ANSI SGR attributes (bold etc.) | ✅ Full VT100/VT220/xterm            | ✅ Full VTE/xterm                      | ⬜ §1.3 P0 — SGR 0/1/4/7/30-37/90-97 |
| 256-color extended palette      | ✅ xterm-256color                    | ✅ xterm-256color                      | ⬜ §1.3 (stretch)                     |
| Scrollback buffer               | ✅ Configurable                      | ✅ Configurable                        | ⬜ §1.4 P1 — 500 lines PMM-allocated  |
| Mouse text selection + copy     | ✅ Win Terminal selection            | ✅ Terminal selection                  | ⬜ §1.4 P1 — click+drag selection     |
| Ctrl+Shift+C/V (copy/paste)     | ✅ Windows Terminal                  | ✅ Standard terminal shortcut          | ⬜ §1.4 P1                            |
| Cursor blink (block/underline)  | ✅ Configurable                      | ✅ Configurable                        | ⬜ §1.2 P0 — 3 styles, 500ms blink   |
| Acrylic/blur background         | ✅ Windows Terminal acrylic          | ✅ alacritty transparent bg             | ⬜ §1.6 (stretch) — `gfx_acrylic()`  |
| Multiple tabs                   | ✅ Windows Terminal tabs              | ✅ GNOME Terminal tabs                  | ⬜ §1.6 (stretch)                     |
| Split panes                     | ✅ Windows Terminal split             | ✅ Tilix / tmux                         | ⬜ §1.6 (stretch)                     |
| **No GPU required for rendering** | ❌ DirectWrite (GPU)               | ❌ GPU preferred (Cairo/Vulkan)        | ✅ **stb_truetype CPU rasterizer — works in QEMU** |
| **No PTY/conPTY layer**         | ❌ conPTY abstraction layer          | ❌ PTY kernel layer required            | ✅ **Direct shell stdout pipe — simpler** |
