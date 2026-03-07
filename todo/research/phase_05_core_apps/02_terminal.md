# Terminal — Research

## Overview

A **graphical terminal emulator** that runs the existing shell (`shell.exe`) inside a WM window with proper TrueType font rendering, ANSI color codes, scrollback buffer, and keyboard/mouse input.

Currently the shell runs in text mode via QEMU serial. The Terminal app wraps it in a desktop-class GUI window — like Windows Terminal wraps cmd.exe.

---

## Features

| Feature | Priority | Description |
|---------|----------|-------------|
| Shell execution | P0 | Spawn `shell.exe`, pipe stdin/stdout |
| Monospace rendering | P0 | Cascadia Code font via `font_*` API |
| Keyboard input | P0 | Type commands, enter to execute |
| Cursor | P0 | Blinking block/line cursor at prompt |
| Scrollback | P1 | 500+ lines of history, scroll with mouse wheel |
| ANSI colors | P1 | `\e[31m` red, `\e[1m` bold, etc. |
| Copy/Paste | P1 | Mouse select + Ctrl+Shift+C/V |
| Resize | P1 | Recalculate cols/rows on window resize |
| Tab completion | P2 | Tab completes filenames/commands |
| Multiple tabs | P3 | Multiple shell sessions in tabs |
| Transparency | P3 | Acrylic/blur background via `gfx_acrylic()` |
| Split panes | P3 | Vertical/horizontal split views |
| Profiles | P3 | Saved color schemes + fonts in Codex |

---

## Architecture

```
┌─────────────────────────────────────────┐
│  Terminal                          ─ □ ×│
├─────────────────────────────────────────┤
│  Impossible OS Shell v0.1.0             │
│  Type 'help' for commands.              │
│                                         │
│  impossible> ls                         │
│  hello.exe    shell.exe    bg.raw       │
│  impossible> ping 8.8.8.8              │
│  Pinging 8.8.8.8... OK (23ms)          │
│  impossible> █                          │
│                                         │
│                                         │
│                                         │
└─────────────────────────────────────────┘
```

### Process model

```
┌───────────────┐      pipe/pty      ┌──────────────┐
│  terminal.exe │ ◄══════════════►   │  shell.exe   │
│  (GUI app)    │   stdin/stdout     │  (user shell) │
│  - renders    │                    │  - commands   │
│  - input      │                    │  - output     │
└───────────────┘                    └──────────────┘
```

Terminal spawns `shell.exe` as a child process. Shell's stdout is piped to Terminal's grid, and Terminal sends keystrokes to shell's stdin.

> [!NOTE]
> Until we have proper pipes/pty, the Terminal can run the shell in-process by calling shell functions directly, then migrate to process isolation later.

---

## Data Structures

### Terminal cell grid

```c
#define TERM_COLS     100
#define TERM_ROWS     30
#define SCROLLBACK    500  /* lines of scrollback history */

struct terminal_cell {
    uint32_t  codepoint;   /* Unicode character */
    uint32_t  fg_color;    /* foreground (ARGB) */
    uint32_t  bg_color;    /* background (ARGB) */
    uint8_t   attrs;       /* ATTR_BOLD | ATTR_UNDERLINE | ATTR_INVERSE */
};

#define ATTR_BOLD      0x01
#define ATTR_UNDERLINE 0x02
#define ATTR_INVERSE   0x04
#define ATTR_BLINK     0x08
```

### Terminal state

```c
struct terminal {
    struct terminal_cell grid[SCROLLBACK][TERM_COLS];

    /* Cursor */
    uint32_t cursor_row;     /* current row in grid */
    uint32_t cursor_col;     /* current column */
    uint8_t  cursor_visible;
    uint8_t  cursor_style;   /* 0=block, 1=underline, 2=bar */
    uint32_t cursor_blink_ticks;

    /* Scrolling */
    uint32_t scroll_top;     /* first visible row in grid */
    uint32_t total_rows;     /* total rows used */
    uint32_t visible_rows;   /* rows visible in window */

    /* Current text attributes */
    uint32_t current_fg;
    uint32_t current_bg;
    uint8_t  current_attrs;

    /* ANSI escape parser */
    uint8_t  ansi_state;     /* 0=normal, 1=ESC, 2=CSI */
    char     ansi_buf[32];   /* parameter accumulator */
    uint8_t  ansi_len;

    /* Shell connection */
    int      shell_pid;

    /* Font */
    font_t  *mono_font;
    int      cell_w;         /* pixel width of one character */
    int      cell_h;         /* pixel height of one character */

    /* Selection */
    uint32_t sel_start_row, sel_start_col;
    uint32_t sel_end_row, sel_end_col;
    uint8_t  selecting;
};
```

---

## ANSI Escape Code Parser

Parse VT100/xterm escape sequences for colors and cursor control:

```c
void terminal_put_char(struct terminal *t, char c) {
    switch (t->ansi_state) {
    case 0: /* Normal */
        if (c == '\e') {
            t->ansi_state = 1;
            t->ansi_len = 0;
        } else if (c == '\n') {
            terminal_newline(t);
        } else if (c == '\r') {
            t->cursor_col = 0;
        } else if (c == '\b') {
            if (t->cursor_col > 0) t->cursor_col--;
        } else if (c == '\t') {
            t->cursor_col = (t->cursor_col + 8) & ~7;
        } else {
            terminal_write_cell(t, c);
        }
        break;

    case 1: /* After ESC */
        if (c == '[') {
            t->ansi_state = 2; /* CSI sequence */
        } else {
            t->ansi_state = 0; /* unknown, ignore */
        }
        break;

    case 2: /* CSI — reading parameters */
        if (c >= '0' && c <= '9' || c == ';') {
            t->ansi_buf[t->ansi_len++] = c;
        } else {
            t->ansi_buf[t->ansi_len] = '\0';
            terminal_handle_csi(t, c); /* 'm', 'H', 'J', 'K', etc. */
            t->ansi_state = 0;
        }
        break;
    }
}
```

### Supported ANSI sequences

| Sequence | Meaning | Implementation |
|----------|---------|----------------|
| `\e[0m` | Reset all attributes | Reset fg/bg/attrs |
| `\e[1m` | Bold | Set ATTR_BOLD |
| `\e[4m` | Underline | Set ATTR_UNDERLINE |
| `\e[7m` | Inverse | Set ATTR_INVERSE |
| `\e[30-37m` | Set foreground color | `current_fg = ansi_colors[n-30]` |
| `\e[40-47m` | Set background color | `current_bg = ansi_colors[n-40]` |
| `\e[90-97m` | Bright foreground | `current_fg = ansi_colors[n-82]` |
| `\e[38;5;Nm` | 256-color foreground | Extended palette |
| `\e[H` | Move cursor to (row, col) | Set cursor position |
| `\e[2J` | Clear screen | Fill grid with spaces |
| `\e[K` | Clear to end of line | Fill rest of row |
| `\e[A/B/C/D` | Cursor up/down/right/left | Move cursor |

### Standard 16-color palette

```c
static const uint32_t ansi_colors[16] = {
    0xFF000000, /* 0:  Black */
    0xFFCC0000, /* 1:  Red */
    0xFF00CC00, /* 2:  Green */
    0xFFCCCC00, /* 3:  Yellow */
    0xFF0000CC, /* 4:  Blue */
    0xFFCC00CC, /* 5:  Magenta */
    0xFF00CCCC, /* 6:  Cyan */
    0xFFCCCCCC, /* 7:  White */
    0xFF666666, /* 8:  Bright Black */
    0xFFFF3333, /* 9:  Bright Red */
    0xFF33FF33, /* 10: Bright Green */
    0xFFFFFF33, /* 11: Bright Yellow */
    0xFF3333FF, /* 12: Bright Blue */
    0xFFFF33FF, /* 13: Bright Magenta */
    0xFF33FFFF, /* 14: Bright Cyan */
    0xFFFFFFFF, /* 15: Bright White */
};
```

---

## Rendering

```c
void terminal_render(struct terminal *t, gfx_surface_t *surface) {
    /* Clear background */
    gfx_fill_rect(surface, 0, 0, surface->width, surface->height,
                  t->theme.bg_color);

    /* Draw each visible cell */
    for (uint32_t row = 0; row < t->visible_rows; row++) {
        uint32_t grid_row = t->scroll_top + row;
        for (uint32_t col = 0; col < TERM_COLS; col++) {
            struct terminal_cell *cell = &t->grid[grid_row][col];
            int px = col * t->cell_w;
            int py = row * t->cell_h;

            /* Draw cell background if non-default */
            if (cell->bg_color != t->theme.bg_color) {
                gfx_fill_rect(surface, px, py, t->cell_w, t->cell_h,
                              cell->bg_color);
            }

            /* Draw character */
            if (cell->codepoint > ' ') {
                font_draw_char(surface, t->mono_font, px, py,
                               cell->codepoint, cell->fg_color);
            }
        }
    }

    /* Draw cursor */
    if (t->cursor_visible) {
        int cx = t->cursor_col * t->cell_w;
        int cy = (t->cursor_row - t->scroll_top) * t->cell_h;
        gfx_fill_rect_alpha(surface, cx, cy, t->cell_w, t->cell_h,
                            GFX_RGBA(255, 255, 255, 180));
    }
}
```

---

## Codex Settings

```ini
; C:\Impossible\System\Config\Codex\apps.codex
[Terminal]
FontName = "Cascadia Code"
FontSize = 14
ColorScheme = "default"
CursorStyle = "block"       ; "block", "underline", "bar"
CursorBlink = 1
Opacity = 100               ; 0-100, for transparency
ScrollbackLines = 500
```

---

## Files

| Action | File | Lines (est.) | Purpose |
|--------|------|-------------|---------|
| **[NEW]** | `src/apps/terminal/terminal.c` | ~600 | Terminal emulator main |
| **[NEW]** | `src/apps/terminal/ansi.c` | ~200 | ANSI escape code parser |
| **[NEW]** | `include/terminal.h` | ~60 | Terminal structs and API |

---

## Implementation Order

### Phase 1: Basic terminal (4-5 days)
- [ ] Terminal window with monospace font grid
- [ ] Static text rendering (display stored output)
- [ ] Keyboard input → shell stdin
- [ ] Shell stdout → terminal grid
- [ ] Newline, carriage return, backspace, tab

### Phase 2: ANSI + scrolling (3-4 days)
- [ ] ANSI escape parser (CSI sequences)
- [ ] 16-color support
- [ ] Cursor positioning (`\e[H`)
- [ ] Clear screen/line (`\e[2J`, `\e[K`)
- [ ] Scrollback buffer + mouse wheel scrolling

### Phase 3: Selection + polish (2-3 days)
- [ ] Mouse text selection (click + drag)
- [ ] Copy to clipboard (Ctrl+Shift+C)
- [ ] Paste from clipboard (Ctrl+Shift+V)
- [ ] Cursor blinking animation
- [ ] Codex settings (font size, colors, cursor style)

### Phase 4: Advanced features (future)
- [ ] Multiple tabs
- [ ] Acrylic transparency background
- [ ] Split panes
- [ ] 256-color and truecolor support
- [ ] Saved profiles/themes
