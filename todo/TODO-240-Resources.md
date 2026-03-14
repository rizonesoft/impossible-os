# P0301 — Resources & Assets

> **Goal:** Consolidate all resource, icon, font, and sound asset management
> into a single reference. This covers file type icon mapping, shortcut icons,
> desktop icon states, system sounds, and font management — everything that
> loads, stores, or renders visual/audio assets from disk.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


---

## 1. File Type Icon Mapping
> *Moved from Phase 03 §5 (File Associations)*

### 1.1 Extension-to-App Mapping

**Prompt:** File associations map extensions to applications — when a user double-clicks `readme.txt`, the OS looks up `.txt` in the Registry to find the associated app (`notepad.exe`) and launches it with the file path as argv[1]. `file_assoc_get_app(".txt")` reads `HKCR\.txt\(Default)` to get the prog ID, then looks up the command. `file_assoc_get_icon(".txt")` reads the icon name. `file_assoc_open(filepath)` extracts the extension, finds the app, and calls `task_exec(app, filepath)`. This is used by the File Manager, desktop icon double-click, and shortcut execution. After completing all items, create `docs/architecture/file-associations.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: file associations"`.


- [ ] Create `include/file_assoc.h` and `src/kernel/file_assoc.c`
- [ ] Implement `file_assoc_get_app(ext)` — look up Registry `HKCR\.{ext}\(Default)` → prog ID → command
- [ ] Implement `file_assoc_get_icon(ext)` — look up icon name for extension
- [ ] Implement `file_assoc_set(ext, app_path)` — set/change default app
- [ ] Implement `file_assoc_open(filepath)` — extract extension, find app, exec with filepath as argument
- [ ] Commit: `"kernel: file associations"`

### 1.2 Default Associations

**Prompt:** Register sensible defaults in the Registry on first boot so common file types open correctly out of the box. Map text file extensions (.txt, .md, .log) to Notepad, source code (.c, .h, .py, .js) to Notepad (or a code editor if available), images (.jpg, .png, .bmp) to Image Viewer, and archives (.zip) to the archive handler. Also map icon names for each extension type so the icon store (Phase 02 §4.3) displays the correct icon. After completing all items, update `docs/architecture/file-associations.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: default file associations"`.


- [ ] Register defaults in Registry (HKCR) on first boot:
  - [ ] `.txt`, `.md`, `.log` → `notepad.exe`, icon `file_text`
  - [ ] `.c`, `.h`, `.py`, `.js` → `notepad.exe`, icon `file_code`
  - [ ] `.jpg`, `.png`, `.bmp` → `imgview.exe`, icon `file_image`
  - [ ] `.exe` → (self), icon `file_exe`
  - [ ] `.zip` → (archive handler), icon `file_archive`
- [ ] Commit: `"kernel: default file associations"`

### 1.3 "Open With..." Dialog (Future)

**Prompt:** This stretch goal adds a dialog that appears when right-clicking a file and choosing "Open With..." — it lists all installed applications, lets the user choose one, and optionally sets it as the default via a checkbox that updates the Registry association. The dialog needs to enumerate all executables in `C:\Impossible\Bin\` and `C:\Programs\`. After completing all items, update `docs/architecture/file-associations.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: open with dialog"`.


- [ ] *(Stretch)* Show list of installed apps for any file type
- [ ] *(Stretch)* "Always use this app" checkbox → updates Registry
- [ ] *(Stretch)* Right-click context menu entry

---

## 2. Shortcut Icons
> *Moved from Phase 03 §7 (Shortcuts)*

### 2.1 Shortcut Format & API

**Prompt:** Shortcut files (.lnk) are small files that point to a target executable with arguments, a working directory, a custom icon, and a description. Use an INI-style format for simplicity and human readability: `[Shortcut]` section with `Target=`, `Arguments=`, `Icon=`, `WorkingDir=`, `Description=` keys. `shortcut_create` writes this file, `shortcut_read` parses it, `shortcut_execute` reads the target and calls the process exec function. The File Manager should render .lnk files with an overlay arrow on their icon and show the description as the label. After completing all items, create `docs/architecture/shortcuts.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: shortcut file (.lnk) support"`.


- [ ] Define `struct shortcut` (target, arguments, icon_path, working_dir, description)
- [ ] Create `include/shortcut.h` and `src/kernel/shortcut.c`
- [ ] Define `.lnk` file format (INI-style: [Shortcut] section with Target, Arguments, Icon, WorkingDir, Description)
- [ ] Implement `shortcut_create(lnk_path, shortcut)` — write .lnk file
- [ ] Implement `shortcut_read(lnk_path, shortcut)` — parse .lnk file
- [ ] Implement `shortcut_execute(lnk_path)` — read target + exec with arguments
- [ ] Commit: `"kernel: shortcut file (.lnk) support"`

### 2.2 Desktop & Start Menu Integration

**Prompt:** Desktop icons are rendered from .lnk files in `C:\Users\{name}\Desktop\`. The Start menu reads .lnk files from `C:\Users\{name}\AppData\StartMenu\`. On first boot, create default shortcuts for Terminal, Notepad, and Settings. Double-clicking a .lnk file on the desktop calls `shortcut_execute()`. The desktop renderer must detect .lnk files and use their custom icon (from the Icon field) + description (as the label text) instead of the default file icon. After completing all items, update `docs/architecture/shortcuts.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: shortcut integration"`.


- [ ] Desktop renders `.lnk` files with their custom icon + description as label
- [ ] Double-click `.lnk` on desktop → `shortcut_execute()`
- [ ] Start Menu reads `.lnk` files from `C:\Users\{name}\AppData\StartMenu\`
- [ ] Create default shortcuts on first boot (Terminal, Notepad, Settings)
- [ ] Commit: `"desktop: shortcut integration"`

---

## 3. Desktop Icon States
> *Moved from Phase 03 §9.3 (Recycle Bin Desktop Integration)*

### 3.1 Recycle Bin Desktop Integration

**Prompt:** The Recycle Bin desktop icon dynamically shows empty or full state based on `trash_count()`. Right-clicking it shows a context menu (Phase 02 §9.2) with "Open Recycle Bin" and "Empty Recycle Bin". The File Manager's Delete action calls `trash_delete()` instead of permanent delete. Add a Registry setting `HKLM\SYSTEM\Recycle\MaxSize` (default 1 GB) — when the total trash size exceeds this limit, auto-purge the oldest items until under the limit. After completing all items, update `docs/architecture/recycle-bin.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: recycle bin integration"`.


- [ ] Desktop icon: `ICON_TRASH_EMPTY` when bin is empty, `ICON_TRASH_FULL` when items present
- [ ] Right-click trash icon → "Open Recycle Bin", "Empty Recycle Bin"
- [ ] File manager "Delete" action → `trash_delete()` instead of permanent delete
- [ ] Registry: `HKLM\SYSTEM\Recycle\MaxSize` — auto-purge oldest when limit reached (default 1 GB)
- [ ] Commit: `"desktop: recycle bin integration"`

---

## 4. System Sounds
> *Moved from Phase 08 §1.4 (System Sounds)*

### 4.1 Sound Assets

**Prompt:** Create WAV system sounds (22050 Hz, mono, 16-bit — small file sizes): startup chime (played after boot splash), button click (tactile feedback), error alert (for error dialogs), notification toast sound, shutdown sound, and recycle bin empty sound. Store in `resources/sounds/` in the source tree, install to `C:\Impossible\Sounds\` on IXFS. Play startup chime after boot splash finishes. Play error sound with error dialogs from Phase 05 §1.3 Message Dialog. Play notification sound with toast notifications from Phase 04 §6.3. Control via Registry `HKLM\SYSTEM\Sound\SystemSounds` (enable/disable). After completing all items, update `docs/architecture/audio.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: system sounds"`.


- [ ] Create `resources/sounds/` directory
- [ ] Generate or source system sounds (WAV format, 22050 Hz, mono):
  - [ ] `startup.wav` — OS boot chime
  - [ ] `click.wav` — button click feedback
  - [ ] `error.wav` — error alert
  - [ ] `notify.wav` — notification toast
  - [ ] `shutdown.wav` — shutdown sound
  - [ ] `recycle.wav` — empty recycle bin
- [ ] Install to `C:\Impossible\Sounds\` on IXFS
- [ ] Play startup chime after boot splash finishes
- [ ] Play error sound with error dialogs
- [ ] Play notification sound with toast notifications
- [ ] Registry: `HKLM\SYSTEM\Sound\SystemSounds = 1` (enable/disable)
- [ ] Commit: `"kernel: system sounds"`

---

## 5. File Manager Icons
> *Moved from Phase 05 §2.1 (File Manager Core)*

### 5.1 File Manager Icon Integration

**Prompt:** The File Manager is the primary file browsing app. The window layout has four zones: toolbar (navigation buttons + address bar), sidebar (quick access links), file area (icon grid or detail table), and status bar (item count + total size). Read directory contents via VFS `readdir()`, map each file's extension to an icon via `icon_for_extension()` from Phase 02 §4.3. Double-click a file calls `file_assoc_open()` from Phase 03 §5.1. Double-click a folder navigates into it (updating the address bar). The Back/Forward buttons maintain a navigation history stack. After completing all items, create `docs/architecture/file-manager.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: file manager core"`.


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

---

## 6. Font Management
> *Moved from Phase 05 §17.1 (Font Manager)*

### 6.1 Font Manager App

**Prompt:** The Font Manager lists all .ttf files installed in `C:\Impossible\Fonts\` using the font manager API from Phase 02 §2. For each font, render a preview line ("The quick brown fox...") at multiple sizes (12, 16, 24, 36px). An "Install" button copies a .ttf file to the fonts directory and registers it in Registry. A "Remove" button deletes the font file (but prevents removing system-required fonts like Selawik and Cascadia Code). A "Set Default" button updates `HKCU\Software\Impossible\Theme\Font` in Registry. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Font Manager"`.


- [ ] Create `src/apps/fontmgr/fontmgr.c`
- [ ] List installed `.ttf` files from `C:\Impossible\Fonts\`
- [ ] Preview each font: "The quick brown fox jumps over the lazy dog"
- [ ] Preview at different sizes (12, 16, 24, 36px)
- [ ] Install new font: copy `.ttf` to fonts directory + register in Registry
- [ ] Remove font (cannot remove system default)
- [ ] Set default system font / monospace font
- [ ] Commit: `"apps: Font Manager"`

---

## Priority Order

| Priority | Section                  | Reason                                |
|----------|--------------------------|---------------------------------------|
| 🔴 P0     | 1.1 File Associations    | Core OS functionality — opening files |
| 🔴 P0     | 1.2 Default Associations | Out-of-box file type support          |
| 🔴 P0     | 2.1 Shortcut Format      | Desktop/Start Menu depends on this    |
| 🟠 P1     | 2.2 Shortcut Integration | Desktop icon rendering                |
| 🟠 P1     | 3.1 Recycle Bin Icons    | Dynamic desktop icon states           |
| 🟠 P1     | 4.1 System Sounds        | UX — startup chime, notifications     |
| 🟠 P1     | 5.1 File Manager Icons   | Icon-per-file in file browser         |
| 🟡 P2     | 6.1 Font Manager         | User font customization               |
| 🟢 P3     | 1.3 Open With Dialog     | Stretch — power user feature          |
