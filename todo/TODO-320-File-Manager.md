# P0502 — File Manager

> **Goal:** Full-featured file browser with sidebar, view modes, file operations,
> and advanced features.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. File Manager Core

 File Manager Core

> **Moved to [TODO-240-Resources.md](TODO-240-Resources.md) §5** — File Manager icon integration, `icon_for_extension()` usage, `file_assoc_open()` wiring.

### 1.2 Sidebar

**Prompt:** The sidebar provides quick navigation to common locations. Show two sections: "Quick Access" (Desktop, Documents, Downloads, Pictures — paths under `C:\Users\Default\`) and "Drives" (list all mounted drives from VFS — C:\, D:\, etc.). Clicking a sidebar item updates the file area to show that path. The sidebar uses a vertical list rendered with icons and labels. After completing all items, update `docs/architecture/file-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: file manager sidebar"`.


- [ ] Quick Access section: Desktop, Documents, Downloads, Pictures
- [ ] Drive section: list all mounted drives (C:\, D:\)
- [ ] Click sidebar item → navigate to that path
- [ ] Commit: `"apps: file manager sidebar"`

### 1.3 View Modes

**Prompt:** The file area supports two view modes: Icon view (large icons in a grid with filenames below each) and Detail view (table with columns: Name, Size, Type, Date Modified). Clicking a column header in Detail view sorts by that column (toggle ascending/descending). The toolbar has view toggle buttons (icon/detail). Remember the user's preference in Registry `HKCU\Software\Impossible\FileManager\ViewMode`. After completing all items, update `docs/architecture/file-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: file manager view modes"`.


- [ ] Icon view — large icons with filename below (default)
- [ ] Detail view — table: Name, Size, Type, Date Modified
- [ ] Sort by: name, date, size, type (click column header to toggle)
- [ ] View toggle buttons in toolbar
- [ ] Commit: `"apps: file manager view modes"`

### 1.4 File Operations

**Prompt:** File operations wire into the clipboard (Phase 03 §3) and recycle bin (Phase 03 §9). Ctrl+C copies the selected file's path to the clipboard (CLIP_FILES format), Ctrl+V pastes (copy file), Ctrl+X cuts (move on paste). Delete key moves the file to the recycle bin via `trash_delete()`. F2 triggers inline rename — the filename label becomes an editable textbox. Ctrl+Shift+N creates a new folder. After completing all items, update `docs/architecture/file-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: file manager operations (create/delete/rename/copy)"`.


- [ ] Create folder: right-click → New → Folder (or Ctrl+Shift+N)
- [ ] Delete: select file → Delete key or right-click → Delete → moves to recycle bin
- [ ] Rename: select file → F2 or right-click → Rename → inline text edit
- [ ] Copy/Paste: Ctrl+C → Ctrl+V (clipboard file paths)
- [ ] Cut/Paste: Ctrl+X → Ctrl+V (move file)
- [ ] Commit: `"apps: file manager operations (create/delete/rename/copy)"`

### 1.5 Advanced Features

**Prompt:** These stretch features make the File Manager a power-user tool: search within the current folder (filter the file list as the user types), drag-and-drop files to desktop or between File Manager windows (Phase 04 §8), file/folder properties dialog (show size, path, dates, permissions), preview pane (render text/image files in a right panel), and tabs (multiple directory views in one window using the Tab Bar widget from §1.4). After completing all items, update `docs/architecture/file-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: file manager advanced features"`.


- [ ] *(Stretch)* Search within current folder (search bar in toolbar)
- [ ] *(Stretch)* Drag and drop files between file manager and desktop
- [ ] *(Stretch)* File/folder properties dialog (size, path, dates)
- [ ] *(Stretch)* Preview pane for images and text files
- [ ] *(Stretch)* Tabs — multiple directories in one window
- [ ] Commit: `"apps: file manager advanced features"`

## 2. File Manager

### 2.1 File Manager Core

> **Moved to [TODO-240-Resources.md](TODO-240-Resources.md) §5** — File Manager icon integration, `icon_for_extension()` usage, `file_assoc_open()` wiring.

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

