# Built-in Applications — Research

## Overview

Core GUI applications that ship with Impossible OS. These establish patterns for all future apps.

| App | Windows Equivalent | Purpose |
|-----|-------------------|---------|
| **Notepad** | Notepad.exe | Text file editor |
| **Calculator** | Calc.exe | Arithmetic and scientific calculator |
| **Paint** | MSPaint.exe | Simple image editor |

> [!NOTE]
> **Terminal** has its own dedicated research doc: [terminal.md](file:///home/derickpayne/impossible-os/todo/research/terminal.md)

All apps share a common foundation: they run as windowed processes using the WM, draw via the `gfx_*` compositing API, render text with `font_*`, and load icons from the icon store.

---

## Common App Framework

Each app is an ELF executable that:
1. Opens a WM window
2. Receives input events (keyboard, mouse)
3. Draws to its window surface via `gfx_*` API
4. Reads/writes files via VFS
5. Stores preferences in Codex

### Shared UI widgets needed by all apps

| Widget | Used by |
|--------|---------|
| Menu bar (File, Edit, View, Help) | Notepad, Paint, Calculator |
| Scroll bar (vertical/horizontal) | Notepad, Paint |
| Toolbar with icon buttons | Paint, Calculator |
| Text input field | Notepad (entire surface), Calculator display |
| Button (clickable, hover state) | Calculator, dialogs |
| Dialog box (Open, Save, About) | All apps |
| Status bar (bottom info line) | Notepad, Paint |

> [!TIP]
> Build a **UI widget library** (`ui.h` / `ui.c`) first — all four apps reuse the same components. This is like Windows Common Controls (`comctl32.dll`).

---

## 1. Notepad

### Features

| Feature | Priority | Description |
|---------|----------|-------------|
| Open/Save/New | P0 | File → Open, Save, Save As, New |
| Text editing | P0 | Type, delete, backspace, cursor movement |
| Cursor (I-beam) | P0 | Blinking text cursor at insertion point |
| Line wrapping | P1 | Word wrap toggle |
| Scroll | P1 | Vertical scroll for long files |
| Find/Replace | P1 | Ctrl+F / Ctrl+H |
| Select + Copy/Paste | P1 | Mouse selection + Ctrl+C/V |
| Undo/Redo | P2 | Ctrl+Z / Ctrl+Y |
| Line numbers | P2 | Optional left gutter |
| Syntax highlighting | P3 | For .c, .h, .asm files |
| Tabs | P3 | Multiple files in tabs |

### Architecture

```
┌───────────────────────────────────────┐
│  File  Edit  View  Help          ─ □ ×│
├───────────────────────────────────────┤
│                                       │
│  Hello, world!                        │
│  This is Impossible OS Notepad.█      │
│                                       │
│                                       │
│                                       │
│                                       │
├───────────────────────────────────────┤
│  Ln 2, Col 36  |  UTF-8  |  CRLF     │
└───────────────────────────────────────┘
```

### Data structure

```c
/* Text buffer — gap buffer for efficient insert/delete */
struct text_buffer {
    char    *buf;          /* raw character buffer */
    uint32_t buf_size;     /* allocated size */
    uint32_t gap_start;    /* gap position (cursor is here) */
    uint32_t gap_end;      /* end of gap */
    uint32_t total_chars;  /* characters in buffer (excluding gap) */
};

/* Notepad state */
struct notepad {
    struct text_buffer text;
    uint32_t cursor_line;
    uint32_t cursor_col;
    uint32_t scroll_y;       /* first visible line */
    uint32_t selection_start;
    uint32_t selection_end;
    char     filepath[256];  /* current file path */
    uint8_t  modified;       /* unsaved changes flag */
    uint8_t  word_wrap;
};
```

### File: `src/apps/notepad/notepad.c` (~600-800 lines)

---

## 2. Calculator

### Features

| Feature | Priority | Description |
|---------|----------|-------------|
| Basic arithmetic | P0 | +, −, ×, ÷, =, C, CE |
| Display | P0 | Current number + operation |
| Decimal point | P0 | Floating point numbers |
| Percentage | P1 | % button |
| Memory | P1 | M+, M−, MR, MC |
| Keyboard input | P1 | Type numbers + operators |
| Scientific mode | P2 | sin, cos, tan, log, sqrt, pow, π |
| History | P2 | Previous calculations list |
| Programmer mode | P3 | Hex, binary, octal, bitwise ops |

### Architecture

```
┌─────────────────────────┐
│  Calculator        ─ □ ×│
├─────────────────────────┤
│              123.456    │  ← display
│         42 ×            │  ← operation preview
├─────────────────────────┤
│  MC  MR  M+  M-  MS    │
│                         │
│  %   CE   C   ⌫        │
│  1/x  x²  √x   ÷      │
│   7    8    9   ×       │
│   4    5    6   −       │
│   1    2    3   +       │
│   ±    0    .   =       │
└─────────────────────────┘
```

### Data structure

```c
struct calculator {
    double   display_value;    /* current displayed number */
    double   operand;          /* stored first operand */
    double   memory;           /* memory register */
    char     display_str[32];  /* formatted display string */
    uint8_t  op;               /* pending operator (+, -, *, /) */
    uint8_t  new_number;       /* next digit starts new number */
    uint8_t  has_decimal;      /* decimal point entered */
    uint8_t  error;            /* division by zero, etc. */
};
```

### Rendering

Each button is a `gfx_fill_rounded_rect` with hover and press states:

```c
struct calc_button {
    int      x, y, w, h;
    char     label[4];       /* "7", "+", "sin" */
    uint32_t color;          /* normal state */
    uint32_t hover_color;
    uint32_t press_color;
    uint8_t  is_hovered;
    uint8_t  is_pressed;
};
```

### File: `src/apps/calculator/calculator.c` (~400-500 lines)

---

## 3. Paint

### Features

| Feature | Priority | Description |
|---------|----------|-------------|
| Canvas | P0 | White drawing area with scrollbars |
| Pencil tool | P0 | Freehand drawing |
| Brush tool | P0 | Variable-size brush |
| Eraser | P0 | Erase to background color |
| Color picker | P0 | Palette bar at bottom |
| Line tool | P1 | Draw straight lines |
| Rectangle tool | P1 | Outline or filled rectangles |
| Ellipse tool | P1 | Outline or filled ellipses |
| Fill bucket | P1 | Flood fill an area |
| Text tool | P1 | Add text to canvas |
| Undo/Redo | P1 | Ctrl+Z / Ctrl+Y |
| Selection tool | P2 | Select, move, copy region |
| Open/Save | P2 | BMP/PNG load and save |
| Resize canvas | P2 | Change image dimensions |
| Zoom | P3 | Zoom in/out on canvas |

### Architecture

```
┌─────────────────────────────────────────────┐
│  File  Edit  View  Image  Help         ─ □ ×│
├────┬────────────────────────────────────────┤
│ ✏️ │                                        │
│ 🖌 │           Canvas                       │
│ 🧹 │        (drawing area)                  │
│ ── │                                        │
│ □  │                                        │
│ ○  │                                        │
│ 🪣 │                                        │
│ A  │                                        │
├────┴────────────────────────────────────────┤
│  Foreground ■ ■ ■ ■ ■ ■ ■ ■  Background    │
│  [██]       ■ ■ ■ ■ ■ ■ ■ ■  [██]          │
├─────────────────────────────────────────────┤
│  800 × 600 px  |  Pencil  |  2px            │
└─────────────────────────────────────────────┘
```

### Data structure

```c
#define MAX_UNDO 32

struct paint {
    gfx_surface_t  canvas;       /* the image being edited */
    gfx_surface_t  undo_stack[MAX_UNDO]; /* snapshot copies */
    uint32_t       undo_pos;
    uint32_t       canvas_w, canvas_h;

    /* Current tool */
    enum {
        TOOL_PENCIL, TOOL_BRUSH, TOOL_ERASER,
        TOOL_LINE, TOOL_RECT, TOOL_ELLIPSE,
        TOOL_FILL, TOOL_TEXT, TOOL_SELECT
    } current_tool;

    /* Drawing state */
    gfx_color_t    fg_color;      /* foreground color */
    gfx_color_t    bg_color;      /* background color */
    uint8_t        brush_size;    /* 1-20 px */
    int32_t        drag_start_x;  /* for line/rect/ellipse */
    int32_t        drag_start_y;
    uint8_t        is_drawing;

    /* View */
    int32_t        scroll_x, scroll_y;
    uint8_t        zoom_level;    /* 1 = 100%, 2 = 200%, etc. */
};
```

### Key algorithms

| Algorithm | Used for | Complexity |
|-----------|----------|-----------|
| **Bresenham's line** | Line tool, pencil strokes | ~20 lines |
| **Midpoint circle** | Ellipse tool | ~25 lines |
| **Flood fill (BFS)** | Fill bucket | ~30 lines |
| **Nearest-neighbor scale** | Zoom view | ~10 lines |

### File: `src/apps/paint/paint.c` (~800-1000 lines)

---

## Shared UI Widget Library

Build once, used by all apps:

```c
/* ui.h — Common UI widget library */

/* Menu bar */
struct ui_menu_bar;
void ui_menu_bar_draw(gfx_surface_t *s, struct ui_menu_bar *menu);
int  ui_menu_bar_click(struct ui_menu_bar *menu, int mx, int my);

/* Button */
struct ui_button {
    int x, y, w, h;
    char label[32];
    uint8_t hovered, pressed, disabled;
};
void ui_button_draw(gfx_surface_t *s, struct ui_button *btn);

/* Text input */
struct ui_textbox {
    int x, y, w, h;
    char text[256];
    uint32_t cursor_pos;
    uint8_t focused;
};
void ui_textbox_draw(gfx_surface_t *s, struct ui_textbox *tb);
void ui_textbox_key(struct ui_textbox *tb, uint8_t key);

/* Scroll bar */
struct ui_scrollbar {
    int x, y, w, h;
    uint32_t value, max_value, page_size;
    uint8_t vertical;
};
void ui_scrollbar_draw(gfx_surface_t *s, struct ui_scrollbar *sb);

/* Color picker (for Paint) */
struct ui_color_picker;
gfx_color_t ui_color_picker_draw(gfx_surface_t *s, struct ui_color_picker *cp);

/* Dialog: Open File / Save As */
int ui_dialog_open(char *path_out, uint32_t max_len, const char *filter);
int ui_dialog_save(char *path_out, uint32_t max_len, const char *filter);
int ui_dialog_message(const char *title, const char *msg, uint8_t type);
```

### File: `src/apps/ui/ui.c` (~500 lines)

---

## Files Summary

| App | File | Lines (est.) |
|-----|------|-------------|
| **UI Widget Library** | `src/apps/ui/ui.c` | ~500 |
| **Notepad** | `src/apps/notepad/notepad.c` | ~700 |
| **Calculator** | `src/apps/calculator/calculator.c` | ~450 |
| **Paint** | `src/apps/paint/paint.c` | ~900 |
| **Total** | | **~2,550** |

All installed to `C:\Impossible\Bin\` (`notepad.exe`, `calc.exe`, `paint.exe`).  
Terminal: see [terminal.md](file:///home/derickpayne/impossible-os/todo/research/terminal.md).

---

## Dependencies

| Dependency | Required by | Status |
|-----------|------------|--------|
| WM window creation | All apps | ✅ Exists |
| `gfx_*` compositing | All apps | 🔜 Planned |
| `font_*` TrueType | All apps | 🔜 Planned |
| Keyboard input events | All apps | ✅ Exists |
| Mouse input events | All apps | ✅ Exists |
| VFS file read/write | Notepad, Paint | ✅ Exists |
| Icon store | All apps (title bar) | 🔜 Planned |
| Codex (preferences) | All apps | 🔜 Planned |
| UI widget library | All apps | 🔜 New |

---

## Implementation Order

### Phase 1: UI Widget Library (1 week)
- [ ] Button with hover/press states
- [ ] Menu bar with dropdowns
- [ ] Scroll bar (vertical)
- [ ] Text input field
- [ ] Open/Save dialog (basic file picker)
- [ ] Message dialog (OK / Yes/No)

### Phase 2: Calculator (3-5 days)
- [ ] Button grid layout
- [ ] Basic arithmetic engine (+, −, ×, ÷)
- [ ] Display rendering
- [ ] Keyboard shortcuts (numpad)
- [ ] Memory functions (M+, MR)

### Phase 4: Notepad (1-2 weeks)
- [ ] Gap buffer for text storage
- [ ] Text rendering with cursor
- [ ] Keyboard input (typing, arrow keys, delete)
- [ ] File → Open, Save, Save As
- [ ] Vertical scrolling
- [ ] Find/Replace (Ctrl+F, Ctrl+H)

### Phase 5: Paint (2-3 weeks)
- [ ] Canvas surface + scroll viewport
- [ ] Pencil tool (Bresenham line between mouse events)
- [ ] Color palette bar
- [ ] Brush with variable size
- [ ] Rectangle, ellipse, line tools
- [ ] Flood fill
- [ ] Undo stack (snapshot-based)
- [ ] Save as BMP
