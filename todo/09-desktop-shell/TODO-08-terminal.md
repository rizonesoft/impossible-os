---
schema_version: 1
id: terminal
domain: 09-desktop-shell
status: active
title: "TODO-08 -- Terminal Emulator"
---

# TODO-08 -- Terminal Emulator

> **Goal:** Replace the existing 80×20 basic `terminal.c` with a production-quality terminal emulator: PMM-allocated cell grid with 2000-row scrollback, full ANSI/VT100 escape parser, Unicode codepoint per cell, selection + clipboard, resize with SIGWINCH, Registry-configurable appearance, and stretch advanced features (tabs, Acrylic, split panes, hyperlinks) that rival Windows Terminal and GNOME Terminal.

> [!IMPORTANT]
> **Already exists**: `TERM_COLS=80`, `TERM_ROWS=20`, `terminal_open/close/putchar/puts/render/trygetchar` in `terminal.h/terminal.c` -- this TODO replaces the internals while preserving the `terminal_puts()`/`terminal_trygetchar()` entry points so existing `sys_write/sys_read` callers are unaffected. `FONT_MONO=2` (Cascadia Code Regular) + `FONT_MONO_BOLD=3` in `font_mgr.h`; `ttf_get(slot, px)` + `ttf_draw_char(s, f, x, y, ...)` exist. `pipe_create(fds[2])` + `pipe_read/write()` in `pipe.h`. `signal_send(pid, sig)` in `signal.h` -- `SIGWINCH=28` not yet defined; add in §5. `gfx_acrylic(s, x, y, w, h, blur_r)` exists in `gfx.h`. `wm_window.acrylic_cache` already exists in WM for dialog acrylic. `pmm_alloc_contiguous(pages)` for large cell grid. `clipboard_set/get()` planned in TODO-01 §1 -- §4 references as forward dep. **Missing**: ANSI state machine, cell struct with Unicode + attrs, scrollback ring buffer, scrollbar, mouse selection, resize handler. Complete sections in order: cell grid → rendering → ANSI parser → scrollback + selection → resize → Registry settings → advanced features.

## Inputs

- `include/desktop/terminal.h` -- existing `TERM_COLS/ROWS`, `terminal_putchar/puts/render/trygetchar` -- §1 replaces internals; keep entry-point signatures
- `include/font_mgr.h` -- `FONT_MONO`, `FONT_MONO_BOLD`, `ttf_get()`, `ttf_draw_char()` -- used by §2 cell rendering
- `include/gfx.h` -- `gfx_surface_t`, `gfx_fill_rect()`, `gfx_acrylic()` -- §2 cell background fill, §7 Acrylic
- `include/kernel/ipc/pipe.h` -- `pipe_create/read/write()` -- §1 shell stdout→terminal, keyboard→shell stdin
- `include/kernel/ipc/signal.h` -- `signal_send(pid, sig)` + `#define SIGWINCH 28` (add in §5) -- resize notification
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous(pages)` -- §1 cell grid + scrollback allocation
- `include/registry.h` -- `RegGetValue` -- §6 font name, size, cursor style, opacity, scrollback lines
- `include/desktop/wm.h` -- `WM_FLAG_DIALOG`, `wm_window.acrylic_cache` -- §7 Acrylic bg reuse
- `include/kernel/drivers/pit.h` -- `system_get_ticks()` -- §2 cursor blink 500 ms toggle
- `include/kernel/drivers/mouse.h` -- mouse button + wheel events -- §4 scrollback wheel, drag selection
- → XREF: `09-desktop-shell/TODO-01-clipboard.md §1` -- `clipboard_set(CLIP_TEXT, text, len)` used by §4 Ctrl+Shift+C copy; `clipboard_get()` for Ctrl+Shift+V paste; must be complete before §4
- -> XREF: `03-memory-concurrency/TODO-09-win32-ipc-extensions.md` (or `include/kernel/ipc/signal.h`) -- SIGWINCH signal delivered in §5 resize; existing `signal_send()` call path
- → XREF: `08-graphics-ui/TODO-04-animation-engine.md` -- `anim_mgr_add()` used by §7 tab slide animation

## Outcome

- `terminal_cell_t` (Unicode codepoint, fg/bg, attrs); 200-col × 2000-row PMM grid; pipe to shell.
- Cell renderer: background fill + `ttf_draw_char()`; cursor blink; `\n`/`\r`/`\b`/`\t` control chars; scroll on overflow.
- Full ANSI/VT100 state machine: SGR (colors 30-37/40-47/90-97, bold, underline, inverse), cursor move (H/A/B/C/D), erase (J/K), cursor show/hide, save/restore.
- 2000-row scrollback; mouse wheel scroll; proportional scrollbar; click+drag text selection; Ctrl+Shift+C/V clipboard.
- `terminal_resize(w, h)` recalculates visible cols/rows; sends `SIGWINCH=28` to shell process.
- Registry settings: FontName, FontSize, CursorStyle, CursorBlink, Opacity, ScrollbackLines; hot-reload on `WM_THEME_CHANGED`.
- Stretch: multi-tab strip, Acrylic background, split panes, hyperlink detection + Ctrl+click.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                            | Depends On                                                                               | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------- | :----: |
| ⭐  |   1   | §1 Terminal cell grid -- `terminal_cell_t`, 200×2000 PMM grid, shell pipe, Unicode codepoint storage   | `pmm_alloc_contiguous`, `pipe_create/read/write` (exist); replaces existing 80×20 grid |  [ ]   |
| 💎  |   2   | §2 Character rendering -- cell bg fill + `ttf_draw_char`, cursor blink, `\n\r\b\t` control chars       | §1 grid; `FONT_MONO` + `ttf_draw_char` (exist)                                         |  [ ]   |
| 💎  |   3   | §3 ANSI escape parser -- SGR, cursor move (H/A/B/C/D), erase (J/K), cursor show/hide, save/restore     | §1 grid (parser writes to cells); §2 rendering (visual output)                          |  [ ]   |
| 💎  |   4   | §4 Scrollback & selection -- 2000-row history, wheel scroll, scrollbar, click+drag, Ctrl+Shift+C/V     | §1 grid; §2 rendering; TODO-01 `clipboard_set/get()` (forward dep for copy/paste)      |  [ ]   |
| 💎  |   5   | §5 Resize handling -- `terminal_resize(w, h)`, recalc visible cols/rows, `SIGWINCH=28` to shell        | §1 grid; `signal_send()` (exists); `#define SIGWINCH 28` added to `signal.h`           |  [ ]   |
| 💎  |   6   | §6 Registry settings -- FontName, FontSize, CursorStyle, CursorBlink, Opacity, ScrollbackLines         | §1-§5 all complete; Registry (exists); `WM_THEME_CHANGED` (TODO-01 theme)              |  [ ]   |
| 💎  |   7   | §7 Advanced features -- tabs, Acrylic bg, split panes, hyperlink detect + Ctrl+click                   | §1–§6 complete; `gfx_acrylic()` (exists); `anim_mgr_add()` (TODO-02)                  |  [ ]   |

---

## 1. Terminal Cell Grid `[Opus]`

`terminal_cell_t` (Unicode codepoint `uint32_t`, `fg_color`, `bg_color`, attrs byte). `terminal_t` struct (200-col × 2000-row PMM grid, cursor, viewport, ANSI parser state, selection, shell pipe). Spawn `shell.exe` via `task_create_user`; pipe shell stdout → `terminal_put_char()` → grid; pipe WM keyboard events → shell stdin.

**Files:** `src/desktop/terminal.c` (rewrite), `include/desktop/terminal.h` (extend)

> [!NOTE]
> `[Opus]` due to: novel PMM-backed ring-buffer scrollback design (no prior Impossible OS precedent), bidirectional pipe plumbing between kernel shell process and desktop terminal rendering, Unicode codepoint-per-cell replacing byte-per-cell, and cursor/viewport state management under concurrent shell output. **Cell struct**: `typedef struct { uint32_t codepoint; uint32_t fg; uint32_t bg; uint8_t attrs; } terminal_cell_t;` -- `#define TERM_ATTR_BOLD 0x01`, `TERM_ATTR_UNDERLINE 0x02`, `TERM_ATTR_INVERSE 0x04`, `TERM_ATTR_BLINK 0x08`. **Grid size**: 200 cols × 2000 rows × 13 bytes (packed) = ~5.2 MiB → `pmm_alloc_contiguous(1300)` 4K pages. **Layout**: rows 0...(SCROLLBACK-1) where row 0 is oldest; `viewport_top` indexes into this. Visible area = `viewport_top` to `viewport_top + visible_rows`. Active cursor row = `viewport_top + cursor_row`. **Shell pipe**: `pipe_create(stdin_fds)` + `pipe_create(stdout_fds)` → `task_create_user("C:\\Impossible\\System32\\cmd.exe", ...)` with fd[0] wired to shell stdin, fd[1] to shell stdout; `terminal_poll_shell()` called each frame: `pipe_read(stdout_fds[0], buf, 256)` (non-blocking) → `terminal_put_char()` per byte. **Keyboard → shell**: WM `WM_KEYDOWN` handler → `pipe_write(stdin_fds[1], &ch, 1)`.

- [ ] `typedef struct terminal_cell { uint32_t codepoint; uint32_t fg; uint32_t bg; uint8_t attrs; } terminal_cell_t;` in `terminal.h`
- [ ] `#define TERM_COLS_MAX 200`, `#define TERM_SCROLLBACK 2000` replacing `TERM_COLS=80`, `TERM_ROWS=20`
- [ ] `#define TERM_ATTR_BOLD 0x01`, `TERM_ATTR_UNDERLINE 0x02`, `TERM_ATTR_INVERSE 0x04`, `TERM_ATTR_BLINK 0x08`
- [ ] `typedef struct terminal { terminal_cell_t *grid; int cols; int rows; int visible_rows; int cursor_row; int cursor_col; int viewport_top; int shell_pid; int stdin_pipe; int stdout_pipe; ansi_state_t ansi; sel_t selection; ... } terminal_t;`
- [ ] `terminal_t *terminal_create(int x, int y, int w, int h)` -- allocate PMM grid; `pipe_create`×2; `task_create_user("cmd.exe", ...)` with pipes; `#define TERM_PAGES 1300`
- [ ] `void terminal_destroy(terminal_t *t)` -- `pmm_free_contiguous(grid)`, close pipes, `signal_send(pid, SIGKILL)`
- [ ] `void terminal_poll_shell(terminal_t *t)` -- non-blocking `pipe_read(stdout_pipe, buf, 256)`; call `terminal_put_char()` per byte
- [ ] `void terminal_put_char(terminal_t *t, uint32_t cp)` -- write to `grid[cursor_row * cols + cursor_col]`; advance cursor
- [ ] WM keyboard handler: `pipe_write(t->stdin_pipe, &ch, 1)` on printable chars + function keys
- [ ] Keep `terminal_puts(s, len)` entry point wired to `terminal_put_char()` for backward compat
- [ ] Commit: `"terminal: cell grid rewrite -- terminal_cell_t, 200×2000 PMM grid, shell pipe, Unicode codepoints"`

## 2. Character Rendering `[Sonnet]`

`terminal_render(t, surface)`: iterate visible rows, fill cell background if non-default, `ttf_draw_char(FONT_MONO)`; handle `\n`, `\r`, `\b`, `\t`; cursor block/underline/bar with 500 ms blink.

**Files:** `src/desktop/terminal.c` (extend)

> [!NOTE]
> Cell rendering loop: `for row in [viewport_top .. viewport_top+visible_rows]: for col in [0..cols]: cell = grid[row*cols+col]`. Background: if `cell.bg != default_bg`: `gfx_fill_rect(s, cx, cy, cell_w, cell_h, cell.bg)`. Foreground: if `cell.codepoint > 0x20`: `ttf_draw_char(s, mono_font, cx, cy, cell.codepoint, cell.fg)` -- use `FONT_MONO_BOLD` if `TERM_ATTR_BOLD`. **INVERSE**: swap fg/bg for that cell. **UNDERLINE**: draw 1 px horizontal line at `cy + cell_h - 2` in `cell.fg`. **BLINK**: skip render if `(system_get_ticks() / (PIT_TARGET_FREQ/2)) % 2 == 1` (toggle every 500 ms). **Cell dimensions**: `cell_w = ttf_get_advance_width(FONT_MONO, font_px)`, `cell_h = font_px + 2` (2 px line gap). **Control chars in `terminal_put_char()`**: `'\n'` → `cursor_row++`; if at `SCROLLBACK-1`: shift rows up (memmove grid up by cols cells; clear last row); `'\r'` → `cursor_col = 0`; `'\b'` → `cursor_col = max(0, cursor_col-1)`, clear cell; `'\t'` → advance to next 8-column stop (`cursor_col = (cursor_col+8) & ~7`). **Cursor**: block = filled `cell_w × cell_h` rect in accent color; underline = 2 px bar at bottom; bar = 2 px vertical bar at left. Blink via same tick toggle.

- [ ] `terminal_render(terminal_t *t, gfx_surface_t *s, int win_x, int win_y)` -- full cell iteration; bg fill; glyph draw
- [ ] INVERSE: if `TERM_ATTR_INVERSE` set → draw `cell.bg` glyph on `cell.fg` background
- [ ] UNDERLINE: 1 px line at `cy + cell_h - 2` in `cell.fg`
- [ ] Control chars in `terminal_put_char()`: `\n` scroll logic; `\r` CR; `\b` backspace + clear; `\t` 8-col tab
- [ ] `terminal_scroll_up(t)`: `memmove(grid, grid + cols, (SCROLLBACK-1)*cols*sizeof(cell))`; clear last row
- [ ] Cursor draw: `if (t->show_cursor && blink_on)`: block/underline/bar per `cursor_style`; blink toggle via `system_get_ticks()`
- [ ] `cell_w` + `cell_h` cached on font change; `ttf_get(FONT_MONO, font_px)` called in `terminal_create()`
- [ ] Commit: `"terminal: cell renderer -- bg fill, ttf_draw_char, inverse/underline/blink attrs, scroll-up, cursor blink"`

## 3. ANSI Escape Parser `[Sonnet]`

State machine: Normal → ESC → CSI → collect params → command. SGR (m): reset, bold, underline, inverse, 8+8 ANSI colors (30-37/40-47/90-97). Cursor: H, A/B/C/D, J, K. `\e[?25h/l` show/hide cursor. `\e[s`/`\e[u` save/restore. Stretch: `\e[38;5;Nm` 256-color.

**Files:** `src/desktop/terminal.c` (extend), `include/desktop/terminal.h` (extend)

> [!NOTE]
> **State machine** stored in `ansi_state_t` inside `terminal_t`: `state` enum (NORMAL/ESC/CSI/OSC), `params[8]` int array, `param_count`, `priv_flag` (set when `?` follows `[`). Each byte processed by `terminal_put_char()`: in NORMAL: if `'\e'` → state=ESC; else handle control char or write cell. In ESC: if `'['` → state=CSI, reset params; else state=NORMAL. In CSI: if `'0'-'9'` → accumulate digit into `params[param_count]`; if `';'` → param_count++; if `'?'` → priv_flag=1; if final byte (0x40–0x7E): dispatch on command byte → reset state=NORMAL. **SGR dispatch** (command = `'m'`): iterate params; 0=reset all attrs+colors to default; 1=bold; 4=underline; 7=inverse; 30-37=fg ANSI color; 40-47=bg ANSI color; 90-97=fg bright; 100-107=bg bright. **16-color ANSI palette**: `uint32_t ansi_palette[16]` -- dark: black/red/green/yellow/blue/magenta/cyan/white; bright: same at double intensity. **Cursor commands**: `H` → `cursor_row=params[0]-1, cursor_col=params[1]-1` (clamp to grid); `A/B/C/D` → move ±`params[0]` rows/cols; `J 2` → `memset` all cells to default; `J 0` → clear from cursor to end; `K 0` → clear to end of line; `K 2` → clear whole line. `?25h/l`: `t->show_cursor = 1/0`. `'s'`/`'u'`: save/restore `cursor_row/col` to `saved_row/col`.

- [ ] `typedef struct { int state; int params[8]; int param_count; int priv_flag; int saved_row; int saved_col; uint32_t fg; uint32_t bg; uint8_t attrs; } ansi_state_t;`
- [ ] `#define ANSI_STATE_NORMAL 0`, `ANSI_STATE_ESC 1`, `ANSI_STATE_CSI 2`
- [ ] `uint32_t ansi_palette[16]` static array in `terminal.c` -- 8 ANSI + 8 bright ANSI colors
- [ ] `terminal_put_char()` extended: byte dispatch through `ansi_state_t` machine before cell write
- [ ] SGR handler: `terminal_handle_sgr(t, params, count)` -- iterate params; set `ansi.fg`, `ansi.bg`, `ansi.attrs`
- [ ] Cursor command handlers: `H`, `A`, `B`, `C`, `D`, `J`, `K` -- clamp to grid bounds
- [ ] `?25h/l`: `t->show_cursor = priv_flag && (cmd=='h') ? 1 : 0`
- [ ] Save/restore: `'s'` → `ansi.saved_row/col = cursor_row/col`; `'u'` → restore
- [ ] Stretch: `\e[38;5;Nm` 256-color: if `params[0]==38 && params[1]==5` → `ansi.fg = xterm256_palette[params[2]]`; `static uint32_t xterm256_palette[256]` (standard xterm colors)
- [ ] Commit: `"terminal: ANSI parser -- SGR 16-color+bold+ul+inv, cursor H/ABCD, erase J/K, show/hide, save/restore"`

## 4. Scrollback & Text Selection `[Sonnet]`

2000-row scrollback buffer above visible area. Mouse wheel scrolls viewport ±3 rows. Proportional scrollbar on right. Click+drag text selection (inverted colors). Ctrl+Shift+C copy to clipboard; Ctrl+Shift+V paste from clipboard → shell stdin.

**Files:** `src/desktop/terminal.c` (extend)

> [!NOTE]
> **Scrollback**: viewport scrolling changes `t->viewport_top`; clamped to `[0, SCROLLBACK - visible_rows]`. New output always written at `viewport_top + cursor_row`; when shell scrolls, `viewport_top` stays at bottom (auto-scroll to latest unless user has manually scrolled up). `auto_scroll` flag: set to 1 by default; any shell output resets viewport to bottom if `auto_scroll=1`; mouse wheel up clears `auto_scroll`; reaching bottom restores `auto_scroll=1`. **Scrollbar**: right edge of terminal window (8 px wide); thumb height = `(visible_rows * window_h) / SCROLLBACK`; thumb y = `(viewport_top * window_h) / SCROLLBACK`; `gfx_fill_rect()` for track + thumb; click+drag scrollbar thumb → `viewport_top` proportional. **Selection**: `sel_t { int active; int start_row; int start_col; int end_row; int end_col; }`. WM `WM_MOUSE_DOWN` in terminal area → start selection; `WM_MOUSE_MOVE` with button → update end; `WM_MOUSE_UP` → finalize. Render: if cell within selection range: draw with fg/bg swapped. **Ctrl+Shift+C**: collect selected cell codepoints into a temporary buffer (PMM temp); `clipboard_set(CLIP_TEXT, buf, len)` (TODO-01 forward dep). **Ctrl+Shift+V**: `clipboard_get(CLIP_TEXT, buf, buflen)` → `pipe_write(t->stdin_pipe, buf, len)`.

- [ ] `typedef struct { int active; int start_row; int start_col; int end_row; int end_col; } sel_t;` in `terminal_t`
- [ ] `viewport_top` clamped scroll; `auto_scroll` flag; new output resets to bottom when `auto_scroll=1`
- [ ] Mouse wheel handler: `viewport_top -= 3` or `+3`; clamp; clear `auto_scroll` on scroll-up
- [ ] Scrollbar: 8 px right edge; draw track + proportional thumb each `terminal_render()` call
- [ ] Scrollbar click+drag: mouse events on right 8 px → `viewport_top = (mouse_y * SCROLLBACK) / window_h`
- [ ] Selection render: in cell loop: if cell `(row, col)` inside `[start..end]` range → swap fg/bg for that cell
- [ ] Selection range normalize: if `end` < `start` (backward drag) → swap
- [ ] Ctrl+Shift+C: iterate selection cells; build UTF-8 string; `clipboard_set(CLIP_TEXT, buf, len)`
- [ ] Ctrl+Shift+V: `clipboard_get(CLIP_TEXT, buf, 4096)`; `pipe_write(stdin_pipe, buf, len)`
- [ ] Commit: `"terminal: scrollback + selection -- viewport scroll, proportional scrollbar, drag select, Ctrl+Shift+C/V"`

## 5. Resize Handling `[Sonnet]`

On window resize: `terminal_resize(t, new_w, new_h)` recalculates `visible_cols/rows`. Preserves history. Sends `SIGWINCH=28` to shell.

**Files:** `src/desktop/terminal.c` (extend), `include/kernel/ipc/signal.h` (extend), `include/desktop/terminal.h` (extend)

> [!NOTE]
> `#define SIGWINCH 28` added to `signal.h` (matches POSIX/Linux value). `terminal_resize(t, new_w, new_h)`: `visible_cols = (new_w - 8) / cell_w` (subtract scrollbar width); `visible_rows = new_h / cell_h`; clamp to `[1, TERM_COLS_MAX]` and `[1, SCROLLBACK]`; update `t->cols` + `t->visible_rows`; if `cursor_col >= visible_cols`: `cursor_col = visible_cols - 1`; if `cursor_row >= visible_rows`: `cursor_row = visible_rows - 1`. Then `signal_send(t->shell_pid, SIGWINCH)`. Shell (`cmd.c`) receives SIGWINCH → re-reads terminal size via a new `SYS_TERMSIZE` syscall or a shared struct; shells typically call `ioctl(TIOCGWINSZ)` -- stub: store `visible_cols/rows` in `g_terminal_cols/rows` globals accessible by `sys_read` path. WM `WM_RESIZE` callback → `terminal_resize()`.

- [ ] `#define SIGWINCH 28` in `include/kernel/ipc/signal.h`
- [ ] `void terminal_resize(terminal_t *t, int new_w, int new_h)` -- recalc `visible_cols`, `visible_rows`; clamp cursor; `signal_send(pid, SIGWINCH)`
- [ ] `g_terminal_cols`, `g_terminal_rows` globals updated on resize; readable by shell via `SYS_TERMSIZE` or direct access
- [ ] WM `WM_RESIZE` event handler in `terminal.c` → call `terminal_resize()`
- [ ] Commit: `"terminal: resize + SIGWINCH -- terminal_resize(), SIGWINCH=28, visible_cols/rows recalc, WM_RESIZE hook"`

## 6. Registry Settings `[Sonnet]`

`HKCU\Software\Impossible\Terminal\`: FontName (default "Cascadia Code"), FontSize (14), CursorStyle (block/underline/bar), CursorBlink (1), Opacity (100 → Acrylic if < 100), ScrollbackLines (2000). Read on creation; reapply on `WM_THEME_CHANGED`.

**Files:** `src/desktop/terminal.c` (extend)

> [!NOTE]
> `terminal_load_settings(t)`: read each Registry key; if absent: write defaults; apply to `t->font_px`, `t->cursor_style`, `t->cursor_blink`, `t->opacity`, `t->scrollback_lines`. Font: `ttf_get(FONT_MONO, t->font_px)` → update `cell_w/h`. CursorStyle: `#define CURSOR_BLOCK 0`, `CURSOR_UNDERLINE 1`, `CURSOR_BAR 2`. Opacity: if `< 100` → set `WM_FLAG_DIALOG` on terminal window to enable `acrylic_cache` + call `gfx_acrylic()` as background in `terminal_render()`. ScrollbackLines: clamp to `[100, 2000]`. `WM_THEME_CHANGED` in terminal event handler → `terminal_load_settings(t)` + re-render.

- [ ] `void terminal_load_settings(terminal_t *t)` -- read Registry keys; apply font/cursor/opacity/scrollback; write defaults if absent
- [ ] `#define CURSOR_BLOCK 0`, `CURSOR_UNDERLINE 1`, `CURSOR_BAR 2`
- [ ] `t->opacity` < 100 → enable Acrylic: `gfx_acrylic(s, 0, 0, w, h, 20)` as first render step
- [ ] `WM_THEME_CHANGED` handler → `terminal_load_settings()`; `ttf_get()` with new font size; recalc `cell_w/h`
- [ ] `terminal_resize()` called after font size change (cell dimensions changed → visible cols/rows change)
- [ ] Commit: `"terminal: Registry settings -- FontName/Size/CursorStyle/Blink/Opacity/Scrollback, WM_THEME_CHANGED"`

## 7. Advanced Features `[Sonnet]`

Multi-tab strip (each tab = independent `terminal_t` + scrollback). Acrylic transparency background. Split panes (H/V independent sessions). Hyperlink detection (`https?://` → Ctrl+click opens browser).

**Files:** `src/desktop/terminal.c` (extend), `src/desktop/terminal_tabs.c` (new)

> [!NOTE]
> **Tabs**: `terminal_tab_t { terminal_t *term; char title[32]; }` array of up to 8 tabs; tab bar = 24 px strip at top of terminal window; each tab drawn as `CTRL_BUTTON`-style rounded tab with title; click → switch active tab (hide old terminal surface, show new). "+" button at end → `terminal_create()` new session. Close tab: ×button on each tab → `terminal_destroy()`; if last tab: close terminal window. Tab title updated from shell process name or OSC 0 escape (`\e]0;title\a`). **Acrylic bg**: if `t->opacity < 100`: `gfx_acrylic(surface, 0, 0, w, h, 20)` before cell rendering; existing `acrylic_cache` from WM dialog pattern reused. **Split panes**: Ctrl+Shift+E → split active terminal vertically (50/50); Ctrl+Shift+O → split horizontally; each pane = independent `terminal_t`; resize pane by dragging divider; max 4 panes. **Hyperlinks**: in `terminal_render()`: scan cell runs for `https?://` prefix; if found: set `TERM_ATTR_UNDERLINE` for URL span; on `WM_MOUSE_MOVE` over underlined URL span: `cursor_set_shape(CURSOR_HAND)`; Ctrl+click → `file_assoc_open(url)` (opens in default browser via TODO-02 file associations).

- [ ] `typedef struct { terminal_t *term; char title[32]; int active; } terminal_tab_t;` in `terminal_tabs.c`
- [ ] Tab bar render: 24 px strip; draw each tab label; active tab = accent underline; + and × buttons
- [ ] `terminal_new_tab()` -- `terminal_create()` + add to `g_tabs[]`; `terminal_switch_tab(idx)`
- [ ] OSC 0 parser in ANSI state machine: `\e]0;title\a` → update tab title
- [ ] Split pane: `terminal_split(term, SPLIT_H/SPLIT_V)` -- allocate second `terminal_t`; divide window rect; resize both on drag
- [ ] Hyperlink scan in render: `url_scan_row(row_cells, col_start, col_end)` → returns start/end col of URL; set UNDERLINE + `TERM_ATTR_HYPERLINK` flag
- [ ] Ctrl+click handler: detect `TERM_ATTR_HYPERLINK` cell → collect URL string → `file_assoc_open(url)` (TODO-02 forward ref)
- [ ] Acrylic: if `t->opacity < 100` in render: `gfx_acrylic(s, 0, 0, w, h, 20)` as first pass
- [ ] Commit: `"terminal: advanced -- multi-tab, Acrylic bg, split panes H/V, hyperlink detect+Ctrl+click"`

---

## OS Comparison


| ⭐  | Feature           | 🪟 Win11                                             | 🐧 Linux                                                 | 🚀 Impossible OS                                                          |
| --- | ----------------- | ---------------------------------------------------- | -------------------------------------------------------- | ------------------------------------------------------------------------- |
| ⭐  | Cell grid         | ✅ Windows Terminal: full Unicode, unlimited         | ✅ GNOME Terminal/Alacritty: full Unicode, unlimited     | ⬜ §1 -- `⭐` PMM flat-array ring buffer                                  |
| 💎  | ANSI/VT100 parser | ✅ Windows Terminal: VT220+; Sixel; iTerm2           | ✅ libvte/Alacritty: near-complete VT220; full 256-color | ⬜ §3 -- 16-color + SGR + cursor                                          |
| 💎  | Scrollback        | ✅ Windows Terminal: unlimited scrollback (default   | ✅ GNOME Terminal: 10000+ rows; smooth                   | ⬜ §4 -- 2000 rows (configurable via Registry)                            |
| 💎  | Text selection    | ✅ Windows Terminal: block/line/word select; search; | ✅ GNOME Terminal: click+drag; primary selection;        | ⬜ §4 -- inverted-cell selection render; Ctrl+Shift+C/V wired             |
| 💎  | Resize + SIGWINCH | ✅ Windows Terminal: SIGWINCH via ConPTY;            | ✅ PTY SIGWINCH on resize; `TIOCSWINSZ`                  | ⬜ §5 -- `SIGWINCH=28` + `terminal_resize()` recalc +                     |
| 💎  | Registry settings | ✅ Windows Terminal settings.json (font, color       | ✅ GNOME Terminal: per-profile settings in               | ⬜ §6 -- Registry `HKCU\...\Terminal\*`; hot-reload on `WM_THEME_CHANGED` |
| ⭐  | Advanced          | ✅ Windows Terminal: tabs, panes, Acrylic,           | ✅ tmux (split panes, detach); GNOME                     | ⬜ §7 -- `⭐` Acrylic via kernel `gfx_acrylic()`                          |

> **After §1–§7:** Impossible OS has a production-quality terminal emulator. The `⭐` differentiators: the cell grid is a PMM flat-array ring buffer with no heap fragmentation; Acrylic transparency runs directly through the kernel compositor (`gfx_acrylic()`) with no GPU delegation; and hyperlink detection is built into the cell renderer rather than an optional plugin.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Open terminal → shell prompt appears; type `echo hello` → output rendered in terminal grid
- [ ] `printf "\e[1;32mGreen Bold\e[0m Normal"` → "Green Bold" in bright green bold; " Normal" in default color
- [ ] `printf "\e[2J"` → screen cleared; `printf "\e[5;10H"` → cursor jumps to row 5, col 10
- [ ] `printf "\e[?25l"` → cursor hidden; `printf "\e[?25h"` → cursor visible
- [ ] Terminal scroll: run `ls -la` 30+ lines → scroll up with mouse wheel → earlier output visible; scroll down → auto-scroll to latest
- [ ] Click+drag to select text → selected cells appear inverted; Ctrl+Shift+C → text in clipboard
- [ ] Ctrl+Shift+V → clipboard text sent to shell stdin (appears as typed input)
- [ ] Drag terminal window corner to resize → `visible_cols/rows` update; shell prompt reflowed; SIGWINCH delivered
- [ ] Set `HKCU\...\Terminal\CursorStyle=1` → underline cursor; `FontSize=20` → larger font, cell size updated
- [ ] `printf "Visit https://impossible-os.dev for info"` → URL underlined in terminal; Ctrl+click → `file_assoc_open` called
- [ ] New tab (+) → second independent shell session; tab switch preserves state of both sessions
- [ ] Commit: `"terminal: production terminal emulator -- all sections complete"`
