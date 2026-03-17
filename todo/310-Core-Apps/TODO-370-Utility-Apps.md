# P0509 — Utility Apps & Shell Commands

> **Goal:** Shell commands, Image Viewer, Screenshot Tool, Archive Manager,
> Calendar, System Info, On-Screen Keyboard, and other small utilities.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Shell Commands

### 1.1 File Operation Commands

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

### 1.2 System Commands

**Prompt:** `whoami` reads the current username from the user account system (Phase 03 §11.1). `date` displays the current date and time using `time_now_local()` + `time_format()` from Phase 03 §4. `free` shows RAM usage from PMM stats (used/total MB). Output redirection: parse `>` to redirect stdout to a file (create/truncate) and `>>` to append. Stretch goals: tab completion (scan current directory + PATH for matching names), pipe `|` (connect stdout of one command to stdin of the next), and `&&` chaining (run next command only if previous succeeded). After completing all items, add to `docs/user/shell-commands.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: system commands + redirection"`.


- [ ] `whoami` — current user name
- [ ] `date` — current date and time
- [ ] `free` — RAM usage (used/total from PMM)
- [ ] Output redirection: `echo hello > file.txt`
- [ ] *(Stretch)* Tab completion for file/command names
- [ ] *(Stretch)* Pipe: `cat file | grep text`
- [ ] *(Stretch)* `&&` chaining: `mkdir foo && cd foo`
- [ ] Commit: `"shell: system commands + redirection"`

### 1.3 Network Commands

**Prompt:** Stretch goals requiring the HTTP client from Phase 07 §4: `wget <url>` downloads a file (HTTP GET, save to current directory or specified path), `nslookup <host>` resolves a hostname to an IP via the DNS resolver (Phase 07 §2). These commands depend on TCP (Phase 07 §1) and DNS being implemented. After completing all items, add to `docs/user/shell-commands.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: network commands"`.


- [ ] *(Stretch)* `wget <url>` — download file (requires HTTP client)
- [ ] *(Stretch)* `nslookup <host>` — DNS resolve

---

## 2. Image Viewer

### 2.1 Image Viewer App

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

## 3. Screenshot Tool

### 3.1 Screenshot Capture

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

### 3.2 Region Select Overlay

**Prompt:** Win+Shift+S enters region select mode: dim the entire screen with a semi-transparent black overlay (alpha ~50%), then let the user click + drag a rubber-band selection rectangle. The selected region is rendered clear (undimmed) so the user can see what they're capturing. On mouse release, capture just the selected region. Press Escape to cancel. This overlay is rendered as a full-screen WM layer above all windows. After completing all items, update `docs/user/screenshot.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Screenshot region select"`.


- [ ] Dim entire screen with semi-transparent overlay
- [ ] Click + drag → rubber-band selection rectangle (clear region)
- [ ] Release → capture the selected region
- [ ] Escape → cancel
- [ ] Commit: `"apps: Screenshot region select"`

---

## 4. Archive Manager

### 4.1 Archive Manager App

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

## 5. Calendar App

### 5.1 Calendar View

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

## 6. System Information

### 6.1 System Info App

**Prompt:** System Information is a read-only display app showing hardware and OS details. Data sources: CPUID (vendor string, brand string), `g_boot_info` (framebuffer resolution, multiboot info), PMM (total/available memory), PCI enumeration (NIC names, storage controllers), Registry (OS version, hostname, IP), and uptime from PIT ticks. This code should be reusable by the `sysdm.cpl` Control Panel applet from §4.3. Display in a two-column table: label on left, value on right. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: System Information"`.


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
- [ ] Reusable by `sysdm.cpl` in Control Panel
- [ ] Commit: `"apps: System Information"`

---

## 7. On-Screen Keyboard

### 7.1 Virtual Keyboard

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

## 8. Utility Apps

### 8.1 Font Manager

> **Moved to [TODO-240-Resources.md](../230-Core-Services/TODO-240-Resources.md) §6** — Font Manager app, `.ttf` listing, font preview, install/remove, set default system font.

### 8.2 Color Picker

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

### 8.3 Sticky Notes

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

## 9. Agent-Recommended Additions

> Items not in the research files but important for a complete app ecosystem.

### 9.1 Common App Event Loop

**Prompt:** Define a standard event loop pattern that every app follows for consistency: `while (wm_get_event(&evt)) { switch (evt.type) { case KEY_DOWN: ... case MOUSE_CLICK: ... case PAINT: ... case CLOSE: ... } }`. Document this pattern so future apps are easy to implement. Standard event types: KEY_DOWN, KEY_UP, MOUSE_MOVE, MOUSE_CLICK, PAINT (window needs redraw), CLOSE (user clicked X). After completing all items, create `docs/architecture/app-pattern.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: standard app event loop pattern"`.


- [ ] Define standard app message loop pattern: `while (wm_get_event(&evt)) { ... }`
- [ ] Standard event types: KEY_DOWN, KEY_UP, MOUSE_MOVE, MOUSE_CLICK, PAINT, CLOSE
- [ ] Template `app_main()` function that all apps follow
- [ ] Document the pattern so future apps are consistent
- [ ] Commit: `"apps: standard app event loop pattern"`

### 9.2 App Installer / Uninstaller

**Prompt:** Stretch goal: a simple package format `.ipkg` (a ZIP file with a `manifest.json` describing name, version, files, and shortcuts). `install <pkg>` extracts to `C:\Programs\{name}\`, creates Start Menu shortcuts from the manifest, and registers in Registry `HKLM\SOFTWARE\Installed\{name}`. `uninstall <name>` removes the files, shortcuts, and Registry entries. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: package installer"`.


- [ ] *(Stretch)* `.ipkg` format (ZIP with manifest.json): name, version, files, shortcuts
- [ ] *(Stretch)* `install <pkg>` → extract to `C:\Programs\{name}\`, create shortcuts
- [ ] *(Stretch)* `uninstall <name>` → remove files, shortcuts, Registry entries

### 9.3 Help / About Dialog (Shared)

**Prompt:** A generic "About {AppName}" dialog reusable by all apps via `ui_dialog_about(name, version, icon, copyright)`. Shows the app icon, name, version, and copyright text in a compact window. Every app's Help menu should have an "About" item that calls this function. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: shared About dialog"`.


- [ ] Generic "About {AppName}" dialog (reusable by all apps)
- [ ] Show app icon, name, version, copyright
- [ ] "Help → About" menu item in every app
- [ ] Commit: `"apps: shared About dialog"`

