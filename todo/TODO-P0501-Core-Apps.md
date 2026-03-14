# P0501 — Core Applications

> **Goal:** Deliver a suite of essential GUI applications that make Impossible OS
> a usable daily environment: file manager, terminal emulator, settings panel,
> text editor, calculator, paint program, and a collection of utility apps —
> all built on a shared UI widget library.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


---

## 1. UI Widget Library (Shared)

> **Moved to [TODO-P0202-GUI.md §2](TODO-P0202-GUI.md)** — Extended Widget Toolkit
> (checkbox, radio, dropdown, slider, progress bar, tabs, list view, tree view,
> toolbar, menu bar, status bar, groupbox, tooltips, dialog system).
> Existing basic controls (Button, Label, TextBox, ScrollBar) are in `controls.c`.

---

## 2. File Manager

### 2.1 File Manager Core

> **Moved to [TODO-P0302-Resources.md](TODO-P0302-Resources.md) §5** — File Manager icon integration, `icon_for_extension()` usage, `file_assoc_open()` wiring.

### 2.2 Sidebar

**Prompt:** The sidebar provides quick navigation to common locations. Show two sections: "Quick Access" (Desktop, Documents, Downloads, Pictures — paths under `C:\Users\Default\`) and "Drives" (list all mounted drives from VFS — C:\, D:\, etc.). Clicking a sidebar item updates the file area to show that path. The sidebar uses a vertical list rendered with icons and labels. After completing all items, update `docs/architecture/file-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: file manager sidebar"`.


- [ ] Quick Access section: Desktop, Documents, Downloads, Pictures
- [ ] Drive section: list all mounted drives (C:\, D:\)
- [ ] Click sidebar item → navigate to that path
- [ ] Commit: `"apps: file manager sidebar"`

### 2.3 View Modes

**Prompt:** The file area supports two view modes: Icon view (large icons in a grid with filenames below each) and Detail view (table with columns: Name, Size, Type, Date Modified). Clicking a column header in Detail view sorts by that column (toggle ascending/descending). The toolbar has view toggle buttons (icon/detail). Remember the user's preference in Registry `HKCU\Software\Impossible\FileManager\ViewMode`. After completing all items, update `docs/architecture/file-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: file manager view modes"`.


- [ ] Icon view — large icons with filename below (default)
- [ ] Detail view — table: Name, Size, Type, Date Modified
- [ ] Sort by: name, date, size, type (click column header to toggle)
- [ ] View toggle buttons in toolbar
- [ ] Commit: `"apps: file manager view modes"`

### 2.4 File Operations

**Prompt:** File operations wire into the clipboard (Phase 03 §3) and recycle bin (Phase 03 §9). Ctrl+C copies the selected file's path to the clipboard (CLIP_FILES format), Ctrl+V pastes (copy file), Ctrl+X cuts (move on paste). Delete key moves the file to the recycle bin via `trash_delete()`. F2 triggers inline rename — the filename label becomes an editable textbox. Ctrl+Shift+N creates a new folder. After completing all items, update `docs/architecture/file-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: file manager operations (create/delete/rename/copy)"`.


- [ ] Create folder: right-click → New → Folder (or Ctrl+Shift+N)
- [ ] Delete: select file → Delete key or right-click → Delete → moves to recycle bin
- [ ] Rename: select file → F2 or right-click → Rename → inline text edit
- [ ] Copy/Paste: Ctrl+C → Ctrl+V (clipboard file paths)
- [ ] Cut/Paste: Ctrl+X → Ctrl+V (move file)
- [ ] Commit: `"apps: file manager operations (create/delete/rename/copy)"`

### 2.5 Advanced Features

**Prompt:** These stretch features make the File Manager a power-user tool: search within the current folder (filter the file list as the user types), drag-and-drop files to desktop or between File Manager windows (Phase 04 §8), file/folder properties dialog (show size, path, dates, permissions), preview pane (render text/image files in a right panel), and tabs (multiple directory views in one window using the Tab Bar widget from §1.4). After completing all items, update `docs/architecture/file-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: file manager advanced features"`.


- [ ] *(Stretch)* Search within current folder (search bar in toolbar)
- [ ] *(Stretch)* Drag and drop files between file manager and desktop
- [ ] *(Stretch)* File/folder properties dialog (size, path, dates)
- [ ] *(Stretch)* Preview pane for images and text files
- [ ] *(Stretch)* Tabs — multiple directories in one window
- [ ] Commit: `"apps: file manager advanced features"`

---

## 3. Terminal Emulator

### 3.1 Terminal Core

**Prompt:** The terminal emulator bridges the shell to a graphical window. The core data structure is a 2D grid of `terminal_cell` structs, each holding a Unicode codepoint, foreground/background colors, and attribute flags (bold, underline, inverse). The grid has `TERM_COLS × SCROLLBACK` cells. Cell dimensions are calculated from Cascadia Code monospace font metrics (Phase 02 §2). The shell process writes to the terminal via `terminal_put_char()`. Keyboard input from the WM is piped to the shell's stdin. After completing all items, create `docs/architecture/terminal.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: terminal emulator core"`.


- [ ] Create `src/apps/terminal/terminal.c` and `include/terminal.h`
- [ ] Define `struct terminal_cell` (codepoint, fg_color, bg_color, attrs: bold/underline/inverse)
- [ ] Define `struct terminal` (grid, cursor, scroll state, ANSI parser, font, selection)
- [ ] Grid: `TERM_COLS × SCROLLBACK` cells (100 cols × 500 scrollback rows)
- [ ] Calculate `cell_w` / `cell_h` from Cascadia Code monospace font
- [ ] Spawn `shell.exe` as child process (or in-process initially)
- [ ] Pipe shell stdout → `terminal_put_char()` → grid
- [ ] Pipe keyboard input → shell stdin
- [ ] Commit: `"apps: terminal emulator core"`

### 3.2 Character Rendering

**Prompt:** `terminal_render` iterates visible rows, drawing each cell: fill background rect if non-default color, then draw the character glyph via `font_draw_char()` using Cascadia Code from the font manager (Phase 02 §2). The cursor (block/underline/bar style) blinks on a 500ms toggle driven by PIT ticks. Handle control characters: `\n` (newline + scroll if at bottom), `\r` (carriage return), `\b` (backspace), `\t` (tab to next 8-column stop). After completing all items, update `docs/architecture/terminal.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: terminal text rendering"`.


- [ ] `terminal_render(t, surface)` — draw all visible cells
- [ ] Draw cell background if non-default color
- [ ] Draw character glyph via `font_draw_char()` (Cascadia Code)
- [ ] Render cursor: block (default), underline, or bar style
- [ ] Cursor blinking animation (toggle every 500ms via PIT ticks)
- [ ] Handle: newline (`\n`), carriage return (`\r`), backspace (`\b`), tab (`\t`)
- [ ] Commit: `"apps: terminal text rendering"`

### 3.3 ANSI Escape Code Parser

**Prompt:** The ANSI parser is a state machine: Normal state processes printable characters, ESC (`\e`) enters escape state, `[` after ESC enters CSI (Control Sequence Introducer) state where numeric parameters are collected until a final command letter. Implement the essential SGR (Select Graphic Rendition) codes: reset (0), bold (1), underline (4), inverse (7), foreground colors 30-37 and 90-97, background colors 40-47. Cursor movement codes (H, A/B/C/D) and screen clearing (2J, K) are needed for programs like vim-lite or top. Define a 16-color palette matching standard ANSI colors. After completing all items, update `docs/architecture/terminal.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: terminal ANSI escape codes"`.


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

### 3.4 Scrollback & Selection

**Prompt:** Scrollback lets users review past output. The grid stores 500 history rows above the visible viewport. Mouse wheel scrolls the viewport up/down through this history. A scrollbar on the right side shows the viewport position. Text selection: click + drag highlights cells (tracked as start_row/col to end_row/col). The selection highlight inverts foreground/background colors. Ctrl+Shift+C copies the selected text to the clipboard (Phase 03 §3.1). Ctrl+Shift+V pastes clipboard text into the shell's stdin. Note: use Ctrl+Shift variants to avoid conflicting with Ctrl+C (SIGINT) in the shell. After completing all items, update `docs/architecture/terminal.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: terminal scrollback and copy/paste"`.


- [ ] Scrollback buffer: 500 lines of history above the visible viewport
- [ ] Mouse wheel → scroll up/down through history
- [ ] Scroll bar on right side
- [ ] Mouse click + drag → text selection
- [ ] Ctrl+Shift+C → copy selection to clipboard
- [ ] Ctrl+Shift+V → paste from clipboard to shell stdin
- [ ] Commit: `"apps: terminal scrollback and copy/paste"`

### 3.5 Registry Settings

**Prompt:** Terminal appearance is configurable via Registry: `HKCU\Software\Impossible\Terminal\FontName` (default "Cascadia Code"), `HKCU\Software\Impossible\Terminal\FontSize` (14), `HKCU\Software\Impossible\Terminal\CursorStyle` ("block"/"underline"/"bar"), `HKCU\Software\Impossible\Terminal\CursorBlink` (REG_DWORD), `HKCU\Software\Impossible\Terminal\Opacity` (0-100 for Acrylic transparency), `HKCU\Software\Impossible\Terminal\ScrollbackLines` (500). Read these values on terminal creation. Recalculate grid columns/rows when the window is resized (new cols = window_width / cell_w). After completing all items, update `docs/architecture/terminal.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: terminal settings"`.


- [ ] Font name: `Apps\Terminal\FontName` (default "Cascadia Code")
- [ ] Font size: `Apps\Terminal\FontSize` (default 14)
- [ ] Cursor style: `Apps\Terminal\CursorStyle` (block/underline/bar)
- [ ] Cursor blink: `Apps\Terminal\CursorBlink` (1/0)
- [ ] Opacity: `Apps\Terminal\Opacity` (0–100, for Acrylic transparency)
- [ ] Scrollback lines: `Apps\Terminal\ScrollbackLines` (default 500)
- [ ] Recalculate cols/rows on window resize
- [ ] Commit: `"apps: terminal settings"`

### 3.6 Advanced Features (Future)

**Prompt:** Stretch goals for a premium terminal: multiple tabs (tab bar at top, each tab is a separate terminal session), Acrylic transparency background (composite the desktop behind the terminal surface at reduced alpha via `gfx_acrylic()`), split panes (divide the terminal window horizontally or vertically into independent sessions), and saved color scheme profiles stored in Registry. After completing all items, update `docs/architecture/terminal.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: terminal advanced features"`.


- [ ] *(Stretch)* Multiple tabs — tabbed terminal sessions
- [ ] *(Stretch)* Acrylic transparency background via `gfx_acrylic()`
- [ ] *(Stretch)* Split panes — vertical/horizontal
- [ ] *(Stretch)* Saved profiles/themes (color schemes)

---

## 4. Settings Panel

### 4.1 SPL Framework

**Prompt:** SPL (Settings Panel Library) defines a standard interface for settings applets. Each applet is a single C function that responds to messages: SPL_INIT (allocate resources), SPL_GETINFO (return name/icon/category), SPL_OPEN (draw UI to provided surface), SPL_CLOSE (cleanup), SPL_SAVE (write changes to Registry). The `spl_panel_t` provides the drawing surface, mouse state, and a Registry root path. This architecture lets settings be modular — new applets can be added without changing the host app. After completing all items, create `docs/architecture/settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: SPL applet interface"`.


- [ ] Create `include/spl.h` — SPL interface
- [ ] Define messages: `SPL_INIT`, `SPL_GETINFO`, `SPL_OPEN`, `SPL_CLOSE`, `SPL_SAVE`
- [ ] Define `spl_info_t` (name, description, icon_path, category, version)
- [ ] Define `spl_panel_t` (surface, width, height, mouse state, registry_root)
- [ ] Define `spl_applet_fn` function pointer type
- [ ] Commit: `"apps: SPL applet interface"`

### 4.2 Settings Host App

**Prompt:** The Settings app is a two-panel window: category sidebar on the left, applet content area on the right. At startup, scan `C:\Impossible\System\Settings\` for .spl files (in practice, these are compiled-in applet functions registered at init). The sidebar groups applets by category (System, Personalization, Apps, Privacy). Clicking an applet name in the sidebar calls SPL_OPEN and renders that applet's UI in the content area. A Back button returns to the applet list. After completing all items, update `docs/architecture/settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Settings Panel host"`.


- [ ] Create `src/apps/settings/settings.c`
- [ ] UI layout: category sidebar (left) + panel area (right)
- [ ] Scan `C:\Impossible\System\Settings\` for `.spl` files at startup
- [ ] For each `.spl`: load, call `SPL_INIT` + `SPL_GETINFO`, add to category list
- [ ] Category sidebar: System, Personalization, Apps, Privacy, Update
- [ ] Click an applet name → call `SPL_OPEN` with panel surface
- [ ] Back button → return to applet list
- [ ] On close → call `SPL_CLOSE` for active applet
- [ ] Commit: `"apps: Settings Panel host"`

### 4.3 Core Applets

**Prompt:** Ship 9 essential applets: `about.spl` (simplest — display OS version, CPU, RAM from CPUID/PMM), `display.spl` (resolution selector, DPI scale dropdown, brightness slider), `theme.spl` (accent color picker, dark/light toggle, corner radius slider), `wallpaper.spl` (browse images in Wallpapers folder, set wallpaper, fit mode), `network.spl` (IP, DHCP toggle, DNS, hostname from network stack), `sound.spl` (volume slider, mute toggle), `datetime.spl` (timezone, 12h/24h, date format, NTP sync button), `power.spl` (screen timeout, shutdown/restart buttons), `taskbar.spl` (height, position, auto-hide). Each applet reads/writes Registry keys and calls the corresponding system functions. After completing all items, update `docs/architecture/settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Settings Panel core applets"`.


- [ ] `about.spl.c` — OS version, CPU, RAM, hardware summary (simplest applet)
- [ ] `display.spl.c` — resolution selector, DPI scale dropdown, brightness slider
- [ ] `theme.spl.c` — accent color picker, dark/light mode toggle, corner radius
- [ ] `wallpaper.spl.c` — wallpaper selector (browse images), fit mode
- [ ] `network.spl.c` — IP address, DHCP toggle, DNS, hostname
- [ ] `sound.spl.c` — volume slider, mute toggle
- [ ] `datetime.spl.c` — timezone selector, 12h/24h toggle, date format, NTP sync button
- [ ] `power.spl.c` — screen timeout, sleep settings, shutdown/restart
- [ ] `taskbar.spl.c` — taskbar height, position, auto-hide toggle
- [ ] Commit: `"apps: Settings Panel core applets"`

### 4.4 Additional Applets

**Prompt:** Additional settings applets for less common configurations: `cursors.spl` (cursor theme, cursor size using DPI scaling from Phase 02 §8), `fonts.spl` (installed fonts list, default system font selector), `accounts.spl` (user management — future, ties into Phase 03 §11.1), `apps.spl` (installed programs list, uninstall), `storage.spl` (disk usage overview, drive info from blkdev layer). After completing all items, update `docs/architecture/settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Settings Panel additional applets"`.


- [ ] `cursors.spl.c` — cursor theme, cursor size
- [ ] `fonts.spl.c` — installed fonts, default system font
- [ ] `accounts.spl.c` — user management (future)
- [ ] `apps.spl.c` — installed programs, uninstall
- [ ] `storage.spl.c` — disk usage, drive information
- [ ] Commit: `"apps: Settings Panel additional applets"`

### 4.5 Win32 .cpl Mapping

**Prompt:** Windows Control Panel applets (.cpl files) are DLLs that export a `CPlApplet` entry point. Map known .cpl names to SPL equivalents: `desk.cpl` → display.spl, `mmsys.cpl` → sound.spl, `sysdm.cpl` → about.spl. When a Win32 program calls `ShellExecute("desk.cpl")`, redirect to the Settings app's corresponding applet. This is a stretch goal for Win32 compatibility (Phase 10). After completing all items, update `docs/architecture/settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: CPL to SPL mapping"`.


- [ ] *(Stretch)* CPL → SPL message translation
- [ ] *(Stretch)* `desk.cpl` → `display.spl`, `mmsys.cpl` → `sound.spl`, etc.
- [ ] *(Stretch)* Add to builtin stub table

---

## 5. Notepad

### 5.1 Text Buffer (Gap Buffer)

**Prompt:** The gap buffer is the most efficient data structure for text editing — insertions and deletions at the cursor are O(1). The buffer has a "gap" (unused region) that sits at the cursor position. `text_insert` drops a character into the gap (gap shrinks). `text_delete` expands the gap to "eat" the character before cursor. `text_move_cursor` shifts the gap to the new position by copying characters. Pre-allocate a reasonable buffer size (64K) and grow by doubling when the gap shrinks to zero. `text_get_line(n)` scans for the nth newline, returning a pointer and length. After completing all items, create `docs/architecture/notepad.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Notepad gap buffer"`.


- [ ] Create `src/apps/notepad/notepad.c`
- [ ] Implement gap buffer: `struct text_buffer` (buf, buf_size, gap_start, gap_end)
- [ ] `text_insert(buf, char)` — insert at cursor (gap start)
- [ ] `text_delete(buf)` — delete char before cursor
- [ ] `text_move_cursor(buf, direction)` — move gap
- [ ] `text_get_line(buf, line_num)` — return line contents
- [ ] `text_line_count(buf)` — count newlines
- [ ] Commit: `"apps: Notepad gap buffer"`

### 5.2 Text Rendering & Cursor

**Prompt:** Render visible lines from the gap buffer using `font_draw_string()` (Selawik or user-configured font from Registry). Track cursor_line and cursor_col. The I-beam cursor blinks at the insertion point (500ms toggle). Arrow keys move the cursor, adjusting line/col and scrolling the viewport if necessary. Home/End jump to line start/end. Ctrl+Home/End jump to file start/end. The scroll position (`scroll_y`) determines which line is at the top of the visible area. After completing all items, update `docs/architecture/notepad.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Notepad text rendering"`.


- [ ] Define `struct notepad` state (text, cursor_line/col, scroll_y, filepath, modified)
- [ ] Render visible lines from gap buffer using `font_draw_string()`
- [ ] Draw blinking I-beam cursor at insertion point
- [ ] Arrow keys: move cursor left/right/up/down
- [ ] Home/End: jump to line start/end
- [ ] Ctrl+Home/End: jump to file start/end
- [ ] Commit: `"apps: Notepad text rendering"`

### 5.3 File Menu

**Prompt:** The File menu uses the menu bar widget from §1.2. File → New clears the buffer and resets the filepath. File → Open invokes `ui_dialog_open()` from §1.3, loads the file via VFS `read()` into the gap buffer. File → Save writes the buffer to the current filepath via VFS `write()`. File → Save As invokes `ui_dialog_save()`. Track a `modified` flag — set on any edit, cleared on save. If modified, show "Do you want to save changes?" dialog (from §1.3) before New/Open/Close. The window title shows "filename.txt — Notepad" (with asterisk if modified). After completing all items, update `docs/architecture/notepad.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Notepad file operations"`.


- [ ] Menu bar: File, Edit, View, Help
- [ ] File → New: clear buffer, reset filepath
- [ ] File → Open: `ui_dialog_open()` → load file via VFS
- [ ] File → Save: write buffer to current filepath via VFS
- [ ] File → Save As: `ui_dialog_save()` → choose path + save
- [ ] "Unsaved changes" warning on close or New if `modified` flag set
- [ ] Window title: "filename.txt — Notepad" (asterisk if modified)
- [ ] Commit: `"apps: Notepad file operations"`

### 5.4 Editing Features

**Prompt:** Mouse click sets the cursor position by calculating which line/col the click coordinates map to. Text selection: click + drag or Shift+Arrow marks a start/end range, rendered with inverted colors. Ctrl+A selects all. Cut/Copy/Paste uses the system clipboard (Phase 03 §3). Vertical scrollbar for long files (from §1.1). Word wrap toggle in the View menu — when enabled, lines wrap at the window width without inserting newlines. Status bar shows line:col, encoding (UTF-8), and line ending type (CRLF/LF). Stretch goals: Find/Replace (Ctrl+F/H) with a search bar, Undo/Redo (Ctrl+Z/Y) with an action stack, line numbers in a left gutter, syntax highlighting for .c/.h files. After completing all items, update `docs/architecture/notepad.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Notepad editing features"`.


- [ ] Mouse click → set cursor position
- [ ] Select text: click + drag, or Shift+Arrow keys
- [ ] Ctrl+A → select all
- [ ] Cut/Copy/Paste via clipboard (Ctrl+X/C/V)
- [ ] Vertical scroll bar for long files
- [ ] Word wrap toggle (View menu)
- [ ] Status bar: line/col, encoding (UTF-8), line ending
- [ ] *(Stretch)* Find/Replace: Ctrl+F, Ctrl+H
- [ ] *(Stretch)* Undo/Redo: Ctrl+Z/Y (action stack)
- [ ] *(Stretch)* Line numbers in left gutter
- [ ] *(Stretch)* Syntax highlighting for .c/.h/.asm
- [ ] Commit: `"apps: Notepad editing features"`

---

## 6. Calculator

### 6.1 Calculator Core

**Prompt:** The Calculator app is a compact fixed-size window with a display area and a 5×4 button grid. Buttons rendered with `gfx_fill_rounded_rect()` with hover/press state colors from the theme. The display shows the current number (large font, right-aligned) and an operation preview (smaller, showing the pending expression like "42 +"). Use floating-point arithmetic via the kernel math shims (Phase 02 `kmath.h`). After completing all items, create `docs/user/calculator.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Calculator layout"`.


- [ ] Create `src/apps/calculator/calculator.c`
- [ ] Define `struct calculator` (display_value, operand, memory, op, flags)
- [ ] Button grid: 5 rows × 4 columns (numbers, operators, functions)
- [ ] Buttons rendered with `gfx_fill_rounded_rect()` + hover/press state colors
- [ ] Display area: current number (large font) + operation preview (small)
- [ ] Commit: `"apps: Calculator layout"`

### 6.2 Arithmetic Engine

**Prompt:** The arithmetic engine handles operator precedence simply by using a two-operand model: the user enters operand1, presses an operator, enters operand2, presses = to compute. Store the pending operation and operand. C (clear) resets everything, CE (clear entry) clears only the current display without losing the pending operation. Handle division by zero gracefully (display "Error"). Accept keyboard input: numpad digits, +, -, *, /, Enter for =. After completing all items, update `docs/user/calculator.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Calculator arithmetic"`.


- [ ] Basic operations: +, −, ×, ÷
- [ ] = key: compute result, display
- [ ] C: clear all, CE: clear entry only
- [ ] ⌫: backspace one digit
- [ ] Decimal point input
- [ ] Percentage (%)
- [ ] Sign toggle (±)
- [ ] 1/x, x², √x
- [ ] Division by zero → display "Error"
- [ ] Keyboard input: numpad and regular number keys
- [ ] Commit: `"apps: Calculator arithmetic"`

### 6.3 Memory & History

**Prompt:** Memory buttons (M+, M−, MR, MC, MS) use a separate stored value that persists across calculations. Stretch goals: Scientific mode adds trig functions (sin/cos/tan — using `kmath.h` shims), log, sqrt, power, π. Programmer mode shows hex/binary/octal representations with bitwise operators (AND, OR, XOR, NOT, shifts). History lists previous calculations. After completing all items, update `docs/user/calculator.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Calculator memory and advanced modes"`.


- [ ] Memory buttons: M+, M−, MR, MC, MS
- [ ] *(Stretch)* Scientific mode: sin, cos, tan, log, sqrt, pow, π
- [ ] *(Stretch)* Programmer mode: hex, binary, octal, bitwise ops
- [ ] *(Stretch)* History: list of previous calculations
- [ ] Commit: `"apps: Calculator memory and advanced modes"`

---

## 7. Paint

### 7.1 Canvas & Viewport

**Prompt:** Paint uses a `gfx_surface_t` as its canvas (default 800×600 white). The canvas may be larger than the window — scroll bars allow panning the viewport. The window layout: tool panel on the left (vertical strip of tool icons), canvas area in the center, color palette at the bottom, and status bar showing canvas dimensions, current tool, and brush size. The undo stack stores canvas snapshots before each stroke (deep-copy the surface, max 32 levels). After completing all items, create `docs/user/paint.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Paint canvas and viewport"`.


- [ ] Create `src/apps/paint/paint.c`
- [ ] Define `struct paint` state (canvas surface, undo stack, current tool, colors, brush size)
- [ ] Canvas: `gfx_surface_t` — default 800×600 white
- [ ] Scroll viewport: horizontal + vertical scroll bars
- [ ] Canvas positioned inside window with toolbar (left) + color palette (bottom) + status bar
- [ ] Commit: `"apps: Paint canvas and viewport"`

### 7.2 Drawing Tools

**Prompt:** All drawing tools operate on the canvas surface. Pencil draws Bresenham lines between consecutive mouse events for smooth freehand drawing. Brush stamps filled circles at each mouse position with configurable radius. Eraser draws with the background color. Line: preview a rubber-band line during drag, render on release. Rectangle/Ellipse: outline or filled, hold Shift for square/circle. Fill bucket uses BFS flood fill from the click point, replacing the target color with the selected color. Text tool: click to place, opens a text input, renders using `font_draw_string()`. Tool selection via toolbar buttons on the left panel. After completing all items, update `docs/user/paint.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Paint drawing tools"`.


- [ ] **Pencil** — freehand drawing (Bresenham line between mouse events)
- [ ] **Brush** — variable-size soft brush (filled circle stamps)
- [ ] **Eraser** — draw with background color
- [ ] **Line** — click + drag → preview rubber-band line → draw on release
- [ ] **Rectangle** — outline or filled rectangle (Shift for square)
- [ ] **Ellipse** — outline or filled ellipse (Shift for circle)
- [ ] **Fill Bucket** — flood fill (BFS/stack-based) with selected color
- [ ] **Text** — click to place, type text, set font size
- [ ] Tool selection via toolbar buttons (left panel)
- [ ] Commit: `"apps: Paint drawing tools"`

### 7.3 Color System

**Prompt:** The color palette bar at the bottom shows 20 preset colors in small squares. Two overlapping squares show the current foreground (on top) and background (behind) colors. Left-click a palette color to set foreground; right-click to set background. Click the swap icon to exchange foreground/background. Stretch goal: a color picker dialog (from §1.3) for custom colors with HSV sliders and hex input. After completing all items, update `docs/user/paint.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Paint color palette"`.


- [ ] Color palette bar at bottom: 20 preset colors
- [ ] Foreground + background color indicators (click to swap)
- [ ] Click palette color → set foreground; right-click → set background
- [ ] *(Stretch)* Color picker dialog for custom colors (Hue/Saturation/Value)
- [ ] Commit: `"apps: Paint color palette"`

### 7.4 Undo & File Operations

**Prompt:** Before each drawing stroke, push a copy of the affected canvas region onto the undo stack (max 32 levels). Ctrl+Z pops the stack and restores. Ctrl+Y re-applies (redo stack). File → Open loads an image via `image_load()` from Phase 02 §3 (JPEG, PNG, BMP). File → Save writes as BMP via `image_save_bmp()`. File → Save As lets the user choose format (BMP or PNG if PNG save is implemented). Stretch goals: rectangle selection tool for move/copy regions, mouse wheel zoom with percentage display. After completing all items, update `docs/user/paint.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Paint undo and file ops"`.


- [ ] Undo stack: save canvas snapshot before each stroke (max 32 levels)
- [ ] Ctrl+Z → undo, Ctrl+Y → redo
- [ ] File → New: create blank canvas (prompt for dimensions)
- [ ] File → Open: load image via `image_load()` (JPG, PNG, BMP)
- [ ] File → Save: save as BMP via `image_save_bmp()`
- [ ] File → Save As: choose format (BMP, PNG)
- [ ] *(Stretch)* Select tool: rectangle selection, move/copy region
- [ ] *(Stretch)* Zoom: mouse wheel zoom, percentage display
- [ ] *(Stretch)* Resize canvas: Image → Resize
- [ ] Status bar: canvas dimensions, current tool, brush size
- [ ] Commit: `"apps: Paint undo and file ops"`

---

## 8. Task Manager

### 8.1 Task Manager App

**Prompt:** The Task Manager opens via Ctrl+Shift+Esc (registered as a system-wide hotkey in Phase 04 §15.1). It has two tabs: Processes (table listing all threads from the scheduler with Name, CPU%, RAM, PID, Status columns — read from the scheduler's task list from Phase 01 §1) and Performance (CPU usage as a rolling 60-second line chart, RAM usage bar from PMM stats). The "End Task" button kills the selected process. Auto-update the display every 1 second. Status bar shows total process count, overall CPU%, and RAM used/total. After completing all items, create `docs/user/task-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Task Manager"`.


- [ ] Create `src/apps/taskmgr/taskmgr.c`
- [ ] Open via Ctrl+Shift+Esc system-wide shortcut
- [ ] **Processes tab**: table listing all processes
  - [ ] Columns: Name, CPU%, RAM, PID, Status
  - [ ] Read from scheduler task list
  - [ ] Select process → [End Task] button → kill process
  - [ ] Auto-update every 1 second
- [ ] **Performance tab**:
  - [ ] CPU usage: rolling line chart (last 60 seconds)
  - [ ] RAM usage: bar showing used / total MB (from PMM stats)
- [ ] Status bar: total processes, overall CPU%, RAM used/total
- [ ] Commit: `"apps: Task Manager"`

---

## 9. Device Manager

### 9.1 Device Manager App

**Prompt:** The Device Manager displays all hardware in a tree view (from §1.4 Tree View widget): categories as parent nodes (Display adapters, Network adapters, Storage controllers, Input devices, System devices), individual devices as children. Device info comes from PCI enumeration results (vendor/device IDs, BARs, IRQs) and registered driver names. Status indicators: green check for working devices, yellow warning for no-driver, red cross for errors. Click a device to see properties (PCI address, vendor ID, device ID, IRQ, driver name). After completing all items, create `docs/user/device-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Device Manager"`.


- [ ] Create `src/apps/devmgr/devmgr.c`
- [ ] Define `struct device_info` (name, driver, category, vendor/device IDs, bus/slot/func, IRQ, status)
- [ ] Tree view: categories → individual devices
  - [ ] Display adapters → "VGA Compatible (Multiboot2 FB)"
  - [ ] Network adapters → "Realtek RTL8139 (PCI)"
  - [ ] Storage controllers → "VirtIO Block Device"
  - [ ] Input devices → "PS/2 Keyboard (IRQ 1)", "VirtIO Tablet (PCI)"
  - [ ] System devices → "PIT Timer", "PCI Bus", "ACPI"
- [ ] Data source: PCI enumeration + registered driver list
- [ ] Device status indicators: OK (✓), no driver (⚠), error (✗)
- [ ] *(Stretch)* Click device → properties: vendor ID, device ID, IRQ, driver name
- [ ] Commit: `"apps: Device Manager"`

---

## 10. Shell Commands (Expanded)

### 10.1 File Operation Commands

**Prompt:** Expand the shell with essential file commands. `cd` changes the current working directory (stored in shell state, passed to VFS operations). `pwd` prints it. `mkdir`/`rmdir` create/remove directories via VFS. `cp` copies a file (read source, write destination). `mv` moves/renames (if same filesystem: rename, otherwise: copy + delete). `rm` sends to recycle bin via `trash_delete()` (Phase 03 §9). `touch` creates an empty file. All commands should print error messages for invalid paths or permissions. After completing all items, add to `docs/user/shell-commands.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: file operation commands"`.


- [ ] `cd <dir>` — change working directory
- [ ] `pwd` — print working directory
- [ ] `mkdir <name>` — create directory
- [ ] `rmdir <name>` — remove empty directory
- [ ] `cp <src> <dst>` — copy file
- [ ] `mv <src> <dst>` — move/rename file
- [ ] `rm <file>` — delete file (to recycle bin)
- [ ] `touch <file>` — create empty file
- [ ] Commit: `"shell: file operation commands"`

### 10.2 System Commands

**Prompt:** `whoami` reads the current username from the user account system (Phase 03 §11.1). `date` displays the current date and time using `time_now_local()` + `time_format()` from Phase 03 §4. `free` shows RAM usage from PMM stats (used/total MB). Output redirection: parse `>` to redirect stdout to a file (create/truncate) and `>>` to append. Stretch goals: tab completion (scan current directory + PATH for matching names), pipe `|` (connect stdout of one command to stdin of the next), and `&&` chaining (run next command only if previous succeeded). After completing all items, add to `docs/user/shell-commands.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: system commands + redirection"`.


- [ ] `whoami` — current user name
- [ ] `date` — current date and time
- [ ] `free` — RAM usage (used/total from PMM)
- [ ] Output redirection: `echo hello > file.txt`
- [ ] *(Stretch)* Tab completion for file/command names
- [ ] *(Stretch)* Pipe: `cat file | grep text`
- [ ] *(Stretch)* `&&` chaining: `mkdir foo && cd foo`
- [ ] Commit: `"shell: system commands + redirection"`

### 10.3 Network Commands

**Prompt:** Stretch goals requiring the HTTP client from Phase 07 §4: `wget <url>` downloads a file (HTTP GET, save to current directory or specified path), `nslookup <host>` resolves a hostname to an IP via the DNS resolver (Phase 07 §2). These commands depend on TCP (Phase 07 §1) and DNS being implemented. After completing all items, add to `docs/user/shell-commands.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: network commands"`.


- [ ] *(Stretch)* `wget <url>` — download file (requires HTTP client)
- [ ] *(Stretch)* `nslookup <host>` — DNS resolve

---

## 11. Image Viewer

### 11.1 Image Viewer App

**Prompt:** The Image Viewer opens images via file association (.jpg, .png, .bmp → imgview.exe from Phase 03 §5.2). Load the image using `image_load()` from Phase 02 §3. Fit the image to the window while preserving aspect ratio. Zoom via mouse wheel (scale up/down using `image_scale()` from Phase 02 §3.2). Pan via click+drag when zoomed past window bounds. Navigate to previous/next images in the same folder using left/right arrow keys (scan directory for image files, maintain an index). Toolbar: Previous, Next, zoom percentage, zoom in/out, fit-to-window buttons. Status bar: filename, dimensions, file size. After completing all items, create `docs/user/image-viewer.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Image Viewer"`.


- [ ] Create `src/apps/imgview/imgview.c`
- [ ] Load image via `image_load()` (JPEG, PNG, BMP)
- [ ] Fit image to window on load (preserve aspect ratio)
- [ ] Zoom: mouse wheel, fit-to-window button, actual size (100%) button
- [ ] Pan: click+drag when zoomed past window bounds
- [ ] Navigate: ← → arrows for previous/next image in same folder
- [ ] Toolbar: Previous, Next, zoom percentage, zoom in/out buttons
- [ ] Status bar: filename, dimensions, file size, image index ("1 of 12")
- [ ] *(Stretch)* Slideshow mode: auto-advance every 3-5 seconds
- [ ] *(Stretch)* Right-click → "Set as wallpaper"
- [ ] Commit: `"apps: Image Viewer"`

---

## 12. Screenshot Tool

### 12.1 Screenshot Capture

**Prompt:** The screenshot tool captures the framebuffer content. `screenshot_capture()` copies the compositor's back buffer surface. `screenshot_window(win)` captures just the focused window's surface. Save as PNG (using `image_save_png()` if available, otherwise BMP via `image_save_bmp()`) to `C:\Users\{name}\Pictures\Screenshots\` with a timestamp filename. Copy to clipboard simultaneously. Show a notification toast (Phase 04 §3.2): "Screenshot saved". Register keyboard shortcuts in the hotkey manager (Phase 04 §15.1): PrtSc for full screen, Alt+PrtSc for active window, Win+Shift+S for region select. After completing all items, create `docs/user/screenshot.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Screenshot Tool"`.


- [ ] Create `src/apps/screenshot/screenshot.c`
- [ ] `screenshot_capture()` — capture full screen from compositor backbuffer
- [ ] `screenshot_window(win)` — capture single window
- [ ] `screenshot_region(x, y, w, h)` — capture rectangular region
- [ ] Save as PNG to `C:\Users\{name}\Pictures\Screenshots\`
- [ ] Copy to clipboard
- [ ] Show notification toast: "Screenshot saved"
- [ ] Keyboard shortcuts:
  - [ ] PrtSc → full screen capture + auto-save
  - [ ] Alt+PrtSc → active window only
  - [ ] Win+Shift+S → region select mode
- [ ] Commit: `"apps: Screenshot Tool"`

### 12.2 Region Select Overlay

**Prompt:** Win+Shift+S enters region select mode: dim the entire screen with a semi-transparent black overlay (alpha ~50%), then let the user click + drag a rubber-band selection rectangle. The selected region is rendered clear (undimmed) so the user can see what they're capturing. On mouse release, capture just the selected region. Press Escape to cancel. This overlay is rendered as a full-screen WM layer above all windows. After completing all items, update `docs/user/screenshot.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Screenshot region select"`.


- [ ] Dim entire screen with semi-transparent overlay
- [ ] Click + drag → rubber-band selection rectangle (clear region)
- [ ] Release → capture the selected region
- [ ] Escape → cancel
- [ ] Commit: `"apps: Screenshot region select"`

---

## 13. Archive Manager

### 13.1 Archive Manager App

**Prompt:** The Archive Manager opens .zip files (registered via file association from Phase 03 §5). It uses the ZIP API from Phase 03 §10.2 to read archive contents. Display a file list with columns: Name, Size, Modified Date, Status. Icons from the icon store map each archived file's extension to its icon. Buttons: [Extract All] invokes `zip_extract()` after a folder selection dialog, [Add Files] invokes `zip_add_file()` after an Open File dialog, [New] creates an empty ZIP. Status bar shows item count and compressed size. After completing all items, create `docs/user/archive-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Archive Manager"`.


- [ ] Create `src/apps/archiver/archiver.c`
- [ ] Open `.zip` files (via file association → double-click)
- [ ] Browse archive contents: file list with name, size, modified date
- [ ] Icons from icon store for each archived file
- [ ] [Extract All] button → choose destination, extract via `zip_extract()`
- [ ] [Add Files] button → add files to existing archive
- [ ] [New] → create new empty archive
- [ ] Status bar: item count, compressed size
- [ ] *(Stretch)* Extract individual files (select + extract)
- [ ] *(Stretch)* Drag files out of archive
- [ ] Commit: `"apps: Archive Manager"`

---

## 14. Calendar App

### 14.1 Calendar View

**Prompt:** The Calendar app shows a month grid (7 columns Mon–Sun × 5-6 week rows). The current day is highlighted with the accent color. Navigation buttons switch months. Clicking a day shows events for that day in a panel below the grid. "+ Add Event" opens a dialog with time picker, title, and color selector. Events are stored in Registry: `HKU\{name}\Software\Impossible\Calendar\Events\YYYY-MM-DD\{id}` with Title, Time, Color values. Also accessible by clicking the taskbar clock (Phase 03 §4.3). Uses the time API from Phase 03 §4.1 for date calculations. After completing all items, create `docs/user/calendar.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Calendar"`.


- [ ] Create `src/apps/calendar/calendar.c`
- [ ] Month view: grid of days (Mon–Sun × weeks)
- [ ] Current day highlighted with accent color
- [ ] Navigation: ◀ / ▶ buttons to switch months
- [ ] Events for selected day shown below calendar grid
- [ ] [+ Add Event] → dialog: time, title, color
- [ ] Events stored in Registry: `HKU\{name}\Software\Impossible\Calendar\Events\{date}\*`
- [ ] Also accessible: click taskbar clock → opens calendar
- [ ] Commit: `"apps: Calendar"`

---

## 15. System Information

### 15.1 System Info App

**Prompt:** System Information is a read-only display app showing hardware and OS details. Data sources: CPUID (vendor string, brand string), `g_boot_info` (framebuffer resolution, multiboot info), PMM (total/available memory), PCI enumeration (NIC names, storage controllers), Registry (OS version, hostname, IP), and uptime from PIT ticks. This code should be reusable by the `about.spl` settings applet from §4.3. Display in a two-column table: label on left, value on right. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: System Information"`.


- [ ] Create `src/apps/sysinfo/sysinfo.c`
- [ ] Display:
  - [ ] OS name, version, build date (from VERSION file)
  - [ ] CPU: vendor, model (from CPUID)
  - [ ] RAM: total MB, available MB (from PMM)
  - [ ] Uptime: hours, minutes
  - [ ] Display: resolution, color depth
  - [ ] Network: NIC name, IP address
  - [ ] Storage: drive name, total size
  - [ ] Boot: Multiboot2 via GRUB2
- [ ] Data from: CPUID, `g_boot_info`, PMM, PCI, Registry
- [ ] Reusable by `about.spl` in Settings Panel
- [ ] Commit: `"apps: System Information"`

---

## 16. On-Screen Keyboard

### 16.1 Virtual Keyboard

**Prompt:** The on-screen keyboard renders a full QWERTY layout as a grid of buttons. Each button click injects a keypress event into the WM's keyboard input queue (just as if a physical key were pressed). Handle modifier keys: Shift toggles between lowercase/uppercase and numbers/symbols, Caps Lock is sticky, Ctrl and Alt are momentary. Auto-show when a text input gains focus (if configured for touch mode). The keyboard window should be non-focusable (clicking it shouldn't steal focus from the target text field). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: On-Screen Keyboard"`.


- [ ] Create `src/apps/osk/osk.c`
- [ ] Full QWERTY layout rendered as button grid
- [ ] Click/tap key → inject keypress event into WM input queue
- [ ] Modifier keys: Shift, Caps Lock, Ctrl, Alt (toggle state)
- [ ] Shifted layout: uppercase letters + symbols
- [ ] Auto-show when text input is focused (touch mode)
- [ ] Auto-hide when physical keyboard detected
- [ ] Movable/resizable window
- [ ] Commit: `"apps: On-Screen Keyboard"`

---

## 17. Utility Apps

### 17.1 Font Manager

> **Moved to [TODO-P0302-Resources.md](TODO-P0302-Resources.md) §6** — Font Manager app, `.ttf` listing, font preview, install/remove, set default system font.

### 17.2 Color Picker

**Prompt:** The system-wide Color Picker activates via Win+Shift+C. The cursor changes to a crosshair/eyedropper icon (from the cursor pack, Phase 02 §5). Draw a magnified circle around the cursor showing individual pixels. On click, read the pixel color from the compositor's back buffer at the cursor position. Show a popup with the color values in RGB, HSL, and HEX (#FF5733) formats. Auto-copy the hex value to the clipboard. Maintain a history of the last 10 picked colors in Registry. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Color Picker"`.


- [ ] Create `src/apps/colorpicker/colorpicker.c`
- [ ] Activate with Win+Shift+C
- [ ] Cursor changes to crosshair/eyedropper
- [ ] Magnified circle around cursor showing pixel grid
- [ ] Click to capture → read pixel from compositor backbuffer
- [ ] Show popup: RGB, HSL, HEX (#FF5733) values
- [ ] Copy hex value to clipboard on capture
- [ ] Color history: remember last 10 picked colors
- [ ] Commit: `"apps: Color Picker"`

### 17.3 Sticky Notes

**Prompt:** Sticky Notes are always-on-top floating windows with editable text on colored backgrounds. Each note is a small window with a simple textarea widget. A "+" button creates a new note. Color options: yellow, pink, blue, green, purple (each is a background color preset). Auto-save to Registry: `HKU\{name}\Software\Impossible\StickyNotes\{id}\Text`, `Color`, `X`, `Y`, `W`, `H`. Load all notes on app startup (which runs at boot via autostart from Phase 03 §11.4). Delete: click X on the note title bar, confirm with a dialog. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Sticky Notes"`.


- [ ] Create `src/apps/stickynotes/stickynotes.c`
- [ ] Floating desktop notes (always-on-top windows)
- [ ] Click "+" to create new note
- [ ] Resizable colored rectangles with editable text
- [ ] Color options: yellow, pink, blue, green, purple
- [ ] Auto-save to Registry: `HKU\{name}\Software\Impossible\StickyNotes\{id}\Text`, `Color`, `X`, `Y`, `W`, `H`
- [ ] Persist across reboots (load at startup)
- [ ] Delete: click X on note → confirm
- [ ] Commit: `"apps: Sticky Notes"`

---

## 18. Agent-Recommended Additions

> Items not in the research files but important for a complete app ecosystem.

### 18.1 Common App Event Loop

**Prompt:** Define a standard event loop pattern that every app follows for consistency: `while (wm_get_event(&evt)) { switch (evt.type) { case KEY_DOWN: ... case MOUSE_CLICK: ... case PAINT: ... case CLOSE: ... } }`. Document this pattern so future apps are easy to implement. Standard event types: KEY_DOWN, KEY_UP, MOUSE_MOVE, MOUSE_CLICK, PAINT (window needs redraw), CLOSE (user clicked X). After completing all items, create `docs/architecture/app-pattern.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: standard app event loop pattern"`.


- [ ] Define standard app message loop pattern: `while (wm_get_event(&evt)) { ... }`
- [ ] Standard event types: KEY_DOWN, KEY_UP, MOUSE_MOVE, MOUSE_CLICK, PAINT, CLOSE
- [ ] Template `app_main()` function that all apps follow
- [ ] Document the pattern so future apps are consistent
- [ ] Commit: `"apps: standard app event loop pattern"`

### 18.2 App Installer / Uninstaller

**Prompt:** Stretch goal: a simple package format `.ipkg` (a ZIP file with a `manifest.json` describing name, version, files, and shortcuts). `install <pkg>` extracts to `C:\Programs\{name}\`, creates Start Menu shortcuts from the manifest, and registers in Registry `HKLM\SOFTWARE\Installed\{name}`. `uninstall <name>` removes the files, shortcuts, and Registry entries. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: package installer"`.


- [ ] *(Stretch)* `.ipkg` format (ZIP with manifest.json): name, version, files, shortcuts
- [ ] *(Stretch)* `install <pkg>` → extract to `C:\Programs\{name}\`, create shortcuts
- [ ] *(Stretch)* `uninstall <name>` → remove files, shortcuts, Registry entries

### 18.3 Help / About Dialog (Shared)

**Prompt:** A generic "About {AppName}" dialog reusable by all apps via `ui_dialog_about(name, version, icon, copyright)`. Shows the app icon, name, version, and copyright text in a compact window. Every app's Help menu should have an "About" item that calls this function. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: shared About dialog"`.


- [ ] Generic "About {AppName}" dialog (reusable by all apps)
- [ ] Show app icon, name, version, copyright
- [ ] "Help → About" menu item in every app
- [ ] Commit: `"apps: shared About dialog"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1.1–1.2 UI Widget Library | Foundation for all apps |
| 🔴 P0 | 3.1–3.2 Terminal (core + rendering) | Primary development tool |
| 🔴 P0 | 2.1–2.2 File Manager (core + sidebar) | Essential file browsing |
| 🟠 P1 | 5. Notepad | Text editing — most basic app |
| 🟠 P1 | 6. Calculator | Quick utility — lightweight showcase |
| 🟠 P1 | 10. Shell Commands | Essential CLI productivity |
| 🟠 P1 | 1.3 Dialogs (Open/Save) | Required by Notepad/Paint/File Manager |
| 🟡 P2 | 8. Task Manager | Process management |
| 🟡 P2 | 4. Settings Panel | System configuration |
| 🟡 P2 | 3.3–3.4 Terminal (ANSI + scrollback) | Terminal polish |
| 🟡 P2 | 11. Image Viewer | Media viewing |
| 🟡 P2 | 12. Screenshot Tool | Utility |
| 🟢 P3 | 7. Paint | Creative app |
| 🟢 P3 | 9. Device Manager | Hardware info |
| 🟢 P3 | 15. System Information | Diagnostics |
| 🟢 P3 | 13. Archive Manager | ZIP handling |
| 🔵 P4 | 14. Calendar | Personal productivity |
| 🔵 P4 | 16. On-Screen Keyboard | Accessibility |
| 🔵 P4 | 17.1 Font Manager | System utility |
| 🔵 P4 | 17.2 Color Picker | Developer utility |
| 🔵 P4 | 17.3 Sticky Notes | Convenience |
| 🔵 P4 | 18. Agent Recommendations | Polish + ecosystem |
