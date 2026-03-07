# Phase 05 — Core Applications

> **Goal:** Deliver a suite of essential GUI applications that make Impossible OS
> a usable daily environment: file manager, terminal emulator, settings panel,
> text editor, calculator, paint program, and a collection of utility apps —
> all built on a shared UI widget library.

---

## 1. UI Widget Library (Shared)
> *Research: [07_builtin_apps.md](research/phase_05_core_apps/07_builtin_apps.md) § Shared UI Widget Library*

### 1.1 Core Widgets

- [ ] Create `include/ui.h` and `src/apps/ui/ui.c`
- [ ] **Button** — `struct ui_button` with label, position, hover/press/disabled states
  - [ ] `ui_button_draw(surface, btn)` — rounded rect with state colors
  - [ ] `ui_button_hit(btn, mx, my)` — hit test
- [ ] **Text Input** — `struct ui_textbox` with text, cursor_pos, focused
  - [ ] `ui_textbox_draw(surface, tb)` — bordered input with blinking cursor
  - [ ] `ui_textbox_key(tb, key)` — handle typing, backspace, delete, arrows
  - [ ] Focus management (click to focus, Tab to next)
- [ ] **Scroll Bar** — `struct ui_scrollbar` with value, max, page_size, vertical/horizontal
  - [ ] `ui_scrollbar_draw(surface, sb)` — track + thumb
  - [ ] Mouse drag support for thumb
  - [ ] Mouse wheel scrolling
- [ ] **Label** — static text rendering with alignment (left/center/right)
- [ ] **Checkbox** — `[ ]` / `[✓]` toggle with label text
- [ ] **Dropdown / ComboBox** — collapsed value + expandable option list
- [ ] Commit: `"apps: UI widget library core"`

### 1.2 Menu Bar

- [ ] Define `struct ui_menu_bar` (list of menu items, each with dropdown items)
- [ ] `ui_menu_bar_draw(surface, menu)` — horizontal bar (File, Edit, View, Help)
- [ ] `ui_menu_bar_click(menu, mx, my)` — open dropdown at correct position
- [ ] Dropdown items: label, shortcut text, separator, callback
- [ ] Keyboard shortcuts: Alt+F for File, Escape to close
- [ ] Commit: `"apps: UI menu bar widget"`

### 1.3 Dialogs

- [ ] **Open File Dialog** — `ui_dialog_open(path_out, max_len, filter)` — VFS browser
  - [ ] File list with icons from icon store
  - [ ] Filter by extension (e.g., "*.txt", "*.png")
  - [ ] Navigate directories, address bar
- [ ] **Save As Dialog** — `ui_dialog_save(path_out, max_len, filter)` — similar with filename input
- [ ] **Message Dialog** — `ui_dialog_message(title, msg, type)` — OK / Yes/No / OK/Cancel
- [ ] **Color Picker Dialog** — gradient square + hue slider + hex input
- [ ] Commit: `"apps: UI dialog widgets (open/save/message)"`

### 1.4 Advanced Widgets

- [ ] **Slider** — horizontal bar with draggable handle (for volume, brightness)
- [ ] **Toggle Switch** — on/off slide switch (for settings)
- [ ] **Progress Bar** — horizontal fill bar (for operations)
- [ ] **Tab Bar** — horizontal tab strip with click-to-switch
- [ ] **List View** — scrollable vertical list with selection
- [ ] **Tree View** — expandable/collapsible hierarchical list (for device manager)
- [ ] **Status Bar** — bottom information bar (line/col, file size, tool name)
- [ ] Commit: `"apps: UI advanced widgets (slider, toggle, tabs, tree)"`

---

## 2. File Manager
> *Research: [01_file_manager.md](research/phase_05_core_apps/01_file_manager.md)*

### 2.1 File Manager Core

- [ ] Create `src/apps/filemgr/filemgr.c`
- [ ] Window layout: toolbar + sidebar + file area + status bar
- [ ] Navigation toolbar: Back (←), Forward (→), Up (↑), address bar
- [ ] Address bar shows current path (`C:\Users\Default\Documents`)
- [ ] File area: read directory via VFS, display files/folders
- [ ] Icons from icon store: `icon_for_extension()` for each file
- [ ] Double-click file → `file_assoc_open()` (open with associated app)
- [ ] Double-click folder → navigate into it
- [ ] Status bar: item count, folder/file breakdown, total size
- [ ] Commit: `"apps: file manager core"`

### 2.2 Sidebar

- [ ] Quick Access section: Desktop, Documents, Downloads, Pictures
- [ ] Drive section: list all mounted drives (C:\, D:\)
- [ ] Click sidebar item → navigate to that path
- [ ] Commit: `"apps: file manager sidebar"`

### 2.3 View Modes

- [ ] Icon view — large icons with filename below (default)
- [ ] Detail view — table: Name, Size, Type, Date Modified
- [ ] Sort by: name, date, size, type (click column header to toggle)
- [ ] View toggle buttons in toolbar
- [ ] Commit: `"apps: file manager view modes"`

### 2.4 File Operations

- [ ] Create folder: right-click → New → Folder (or Ctrl+Shift+N)
- [ ] Delete: select file → Delete key or right-click → Delete → moves to recycle bin
- [ ] Rename: select file → F2 or right-click → Rename → inline text edit
- [ ] Copy/Paste: Ctrl+C → Ctrl+V (clipboard file paths)
- [ ] Cut/Paste: Ctrl+X → Ctrl+V (move file)
- [ ] Commit: `"apps: file manager operations (create/delete/rename/copy)"`

### 2.5 Advanced Features

- [ ] *(Stretch)* Search within current folder (search bar in toolbar)
- [ ] *(Stretch)* Drag and drop files between file manager and desktop
- [ ] *(Stretch)* File/folder properties dialog (size, path, dates)
- [ ] *(Stretch)* Preview pane for images and text files
- [ ] *(Stretch)* Tabs — multiple directories in one window
- [ ] Commit: `"apps: file manager advanced features"`

---

## 3. Terminal Emulator
> *Research: [02_terminal.md](research/phase_05_core_apps/02_terminal.md)*

### 3.1 Terminal Core

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

- [ ] `terminal_render(t, surface)` — draw all visible cells
- [ ] Draw cell background if non-default color
- [ ] Draw character glyph via `font_draw_char()` (Cascadia Code)
- [ ] Render cursor: block (default), underline, or bar style
- [ ] Cursor blinking animation (toggle every 500ms via PIT ticks)
- [ ] Handle: newline (`\n`), carriage return (`\r`), backspace (`\b`), tab (`\t`)
- [ ] Commit: `"apps: terminal text rendering"`

### 3.3 ANSI Escape Code Parser

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

- [ ] Scrollback buffer: 500 lines of history above the visible viewport
- [ ] Mouse wheel → scroll up/down through history
- [ ] Scroll bar on right side
- [ ] Mouse click + drag → text selection (highlighted)
- [ ] Ctrl+Shift+C → copy selection to clipboard
- [ ] Ctrl+Shift+V → paste from clipboard to shell stdin
- [ ] Commit: `"apps: terminal scrollback and copy/paste"`

### 3.5 Codex Settings

- [ ] Font name: `Apps\Terminal\FontName` (default "Cascadia Code")
- [ ] Font size: `Apps\Terminal\FontSize` (default 14)
- [ ] Cursor style: `Apps\Terminal\CursorStyle` (block/underline/bar)
- [ ] Cursor blink: `Apps\Terminal\CursorBlink` (1/0)
- [ ] Opacity: `Apps\Terminal\Opacity` (0–100, for Acrylic transparency)
- [ ] Scrollback lines: `Apps\Terminal\ScrollbackLines` (default 500)
- [ ] Recalculate cols/rows on window resize
- [ ] Commit: `"apps: terminal settings"`

### 3.6 Advanced Features (Future)

- [ ] *(Stretch)* Multiple tabs — tabbed terminal sessions
- [ ] *(Stretch)* Acrylic transparency background via `gfx_acrylic()`
- [ ] *(Stretch)* Split panes — vertical/horizontal
- [ ] *(Stretch)* Saved profiles/themes (color schemes)

---

## 4. Settings Panel
> *Research: [03_settings_panel.md](research/phase_05_core_apps/03_settings_panel.md)*

### 4.1 SPL Framework

- [ ] Create `include/spl.h` — SPL interface
- [ ] Define messages: `SPL_INIT`, `SPL_GETINFO`, `SPL_OPEN`, `SPL_CLOSE`, `SPL_SAVE`
- [ ] Define `spl_info_t` (name, description, icon_path, category, version)
- [ ] Define `spl_panel_t` (surface, width, height, mouse state, codex_root)
- [ ] Define `spl_applet_fn` function pointer type
- [ ] Commit: `"apps: SPL applet interface"`

### 4.2 Settings Host App

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

- [ ] `cursors.spl.c` — cursor theme, cursor size
- [ ] `fonts.spl.c` — installed fonts, default system font
- [ ] `accounts.spl.c` — user management (future)
- [ ] `apps.spl.c` — installed programs, uninstall
- [ ] `storage.spl.c` — disk usage, drive information
- [ ] Commit: `"apps: Settings Panel additional applets"`

### 4.5 Win32 .cpl Mapping

- [ ] *(Stretch)* CPL → SPL message translation
- [ ] *(Stretch)* `desk.cpl` → `display.spl`, `mmsys.cpl` → `sound.spl`, etc.
- [ ] *(Stretch)* Add to builtin stub table

---

## 5. Notepad
> *Research: [07_builtin_apps.md](research/phase_05_core_apps/07_builtin_apps.md) § Notepad*

### 5.1 Text Buffer (Gap Buffer)

- [ ] Create `src/apps/notepad/notepad.c`
- [ ] Implement gap buffer: `struct text_buffer` (buf, buf_size, gap_start, gap_end)
- [ ] `text_insert(buf, char)` — insert at cursor (gap start)
- [ ] `text_delete(buf)` — delete char before cursor
- [ ] `text_move_cursor(buf, direction)` — move gap
- [ ] `text_get_line(buf, line_num)` — return line contents
- [ ] `text_line_count(buf)` — count newlines
- [ ] Commit: `"apps: Notepad gap buffer"`

### 5.2 Text Rendering & Cursor

- [ ] Define `struct notepad` state (text, cursor_line/col, scroll_y, filepath, modified)
- [ ] Render visible lines from gap buffer using `font_draw_string()`
- [ ] Draw blinking I-beam cursor at insertion point
- [ ] Arrow keys: move cursor left/right/up/down
- [ ] Home/End: jump to line start/end
- [ ] Ctrl+Home/End: jump to file start/end
- [ ] Commit: `"apps: Notepad text rendering"`

### 5.3 File Menu

- [ ] Menu bar: File, Edit, View, Help
- [ ] File → New: clear buffer, reset filepath
- [ ] File → Open: `ui_dialog_open()` → load file via VFS
- [ ] File → Save: write buffer to current filepath via VFS
- [ ] File → Save As: `ui_dialog_save()` → choose path + save
- [ ] "Unsaved changes" warning on close or New if `modified` flag set
- [ ] Window title: "filename.txt — Notepad" (asterisk if modified)
- [ ] Commit: `"apps: Notepad file operations"`

### 5.4 Editing Features

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
> *Research: [07_builtin_apps.md](research/phase_05_core_apps/07_builtin_apps.md) § Calculator*

### 6.1 Calculator Core

- [ ] Create `src/apps/calculator/calculator.c`
- [ ] Define `struct calculator` (display_value, operand, memory, op, flags)
- [ ] Button grid: 5 rows × 4 columns (numbers, operators, functions)
- [ ] Buttons rendered with `gfx_fill_rounded_rect()` + hover/press state colors
- [ ] Display area: current number (large font) + operation preview (small)
- [ ] Commit: `"apps: Calculator layout"`

### 6.2 Arithmetic Engine

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

- [ ] Memory buttons: M+, M−, MR, MC, MS
- [ ] *(Stretch)* Scientific mode: sin, cos, tan, log, sqrt, pow, π
- [ ] *(Stretch)* Programmer mode: hex, binary, octal, bitwise ops
- [ ] *(Stretch)* History: list of previous calculations
- [ ] Commit: `"apps: Calculator memory and advanced modes"`

---

## 7. Paint
> *Research: [07_builtin_apps.md](research/phase_05_core_apps/07_builtin_apps.md) § Paint*

### 7.1 Canvas & Viewport

- [ ] Create `src/apps/paint/paint.c`
- [ ] Define `struct paint` state (canvas surface, undo stack, current tool, colors, brush size)
- [ ] Canvas: `gfx_surface_t` — default 800×600 white
- [ ] Scroll viewport: horizontal + vertical scroll bars
- [ ] Canvas positioned inside window with toolbar (left) + color palette (bottom) + status bar
- [ ] Commit: `"apps: Paint canvas and viewport"`

### 7.2 Drawing Tools

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

- [ ] Color palette bar at bottom: 20 preset colors
- [ ] Foreground + background color indicators (click to swap)
- [ ] Click palette color → set foreground; right-click → set background
- [ ] *(Stretch)* Color picker dialog for custom colors (Hue/Saturation/Value)
- [ ] Commit: `"apps: Paint color palette"`

### 7.4 Undo & File Operations

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
> *Research: [04_task_manager.md](research/phase_05_core_apps/04_task_manager.md)*

### 8.1 Task Manager App

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
> *Research: [05_device_manager.md](research/phase_05_core_apps/05_device_manager.md)*

### 9.1 Device Manager App

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
> *Research: [06_shell_commands.md](research/phase_05_core_apps/06_shell_commands.md)*

### 10.1 File Operation Commands

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

- [ ] `whoami` — current user name
- [ ] `date` — current date and time
- [ ] `free` — RAM usage (used/total from PMM)
- [ ] Output redirection: `echo hello > file.txt`
- [ ] *(Stretch)* Tab completion for file/command names
- [ ] *(Stretch)* Pipe: `cat file | grep text`
- [ ] *(Stretch)* `&&` chaining: `mkdir foo && cd foo`
- [ ] Commit: `"shell: system commands + redirection"`

### 10.3 Network Commands

- [ ] *(Stretch)* `wget <url>` — download file (requires HTTP client)
- [ ] *(Stretch)* `nslookup <host>` — DNS resolve

---

## 11. Image Viewer
> *Research: [08_image_viewer.md](research/phase_05_core_apps/08_image_viewer.md)*

### 11.1 Image Viewer App

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
> *Research: [09_screenshot_tool.md](research/phase_05_core_apps/09_screenshot_tool.md)*

### 12.1 Screenshot Capture

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

- [ ] Dim entire screen with semi-transparent overlay
- [ ] Click + drag → rubber-band selection rectangle (clear region)
- [ ] Release → capture the selected region
- [ ] Escape → cancel
- [ ] Commit: `"apps: Screenshot region select"`

---

## 13. Archive Manager
> *Research: [10_archive_manager.md](research/phase_05_core_apps/10_archive_manager.md)*

### 13.1 Archive Manager App

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
> *Research: [11_calendar_app.md](research/phase_05_core_apps/11_calendar_app.md)*

### 14.1 Calendar View

- [ ] Create `src/apps/calendar/calendar.c`
- [ ] Month view: grid of days (Mon–Sun × weeks)
- [ ] Current day highlighted with accent color
- [ ] Navigation: ◀ / ▶ buttons to switch months
- [ ] Events for selected day shown below calendar grid
- [ ] [+ Add Event] → dialog: time, title, color
- [ ] Events stored in Codex: `User\{name}\Calendar\Events\{date}\*`
- [ ] Also accessible: click taskbar clock → opens calendar
- [ ] Commit: `"apps: Calendar"`

---

## 15. System Information
> *Research: [12_system_information.md](research/phase_05_core_apps/12_system_information.md)*

### 15.1 System Info App

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
- [ ] Data from: CPUID, `g_boot_info`, PMM, PCI, Codex
- [ ] Reusable by `about.spl` in Settings Panel
- [ ] Commit: `"apps: System Information"`

---

## 16. On-Screen Keyboard
> *Research: [13_onscreen_keyboard.md](research/phase_05_core_apps/13_onscreen_keyboard.md)*

### 16.1 Virtual Keyboard

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
> *Research: [14_font_manager.md](research/phase_05_core_apps/14_font_manager.md)*

- [ ] Create `src/apps/fontmgr/fontmgr.c`
- [ ] List installed `.ttf` files from `C:\Impossible\Fonts\`
- [ ] Preview each font: "The quick brown fox jumps over the lazy dog"
- [ ] Preview at different sizes (12, 16, 24, 36px)
- [ ] Install new font: copy `.ttf` to fonts directory + register in Codex
- [ ] Remove font (cannot remove system default)
- [ ] Set default system font / monospace font
- [ ] Commit: `"apps: Font Manager"`

### 17.2 Color Picker
> *Research: [15_color_picker.md](research/phase_05_core_apps/15_color_picker.md)*

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
> *Research: [16_sticky_notes.md](research/phase_05_core_apps/16_sticky_notes.md)*

- [ ] Create `src/apps/stickynotes/stickynotes.c`
- [ ] Floating desktop notes (always-on-top windows)
- [ ] Click "+" to create new note
- [ ] Resizable colored rectangles with editable text
- [ ] Color options: yellow, pink, blue, green, purple
- [ ] Auto-save to Codex: `User\{name}\StickyNotes\{id}\Text`, `Color`, `X`, `Y`, `W`, `H`
- [ ] Persist across reboots (load at startup)
- [ ] Delete: click X on note → confirm
- [ ] Commit: `"apps: Sticky Notes"`

---

## 18. Agent-Recommended Additions

> Items not in the research files but important for a complete app ecosystem.

### 18.1 Common App Event Loop

- [ ] Define standard app message loop pattern: `while (wm_get_event(&evt)) { ... }`
- [ ] Standard event types: KEY_DOWN, KEY_UP, MOUSE_MOVE, MOUSE_CLICK, PAINT, CLOSE
- [ ] Template `app_main()` function that all apps follow
- [ ] Document the pattern so future apps are consistent
- [ ] Commit: `"apps: standard app event loop pattern"`

### 18.2 App Installer / Uninstaller

- [ ] *(Stretch)* `.ipkg` format (ZIP with manifest.json): name, version, files, shortcuts
- [ ] *(Stretch)* `install <pkg>` → extract to `C:\Programs\{name}\`, create shortcuts
- [ ] *(Stretch)* `uninstall <name>` → remove files, shortcuts, Codex entries

### 18.3 Help / About Dialog (Shared)

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
