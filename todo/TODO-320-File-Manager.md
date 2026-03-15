# P0502 — File Manager

> **Goal:** Full-featured file browser with sidebar, view modes, file operations,
> drag-and-drop, search, and integration with the clipboard, recycle bin, and file associations.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

> [!NOTE]
> **File Manager Core** (`icon_for_extension()`, `file_assoc_open()` wiring) is defined in
> [TODO-240-Resources.md §5](TODO-240-Resources.md). This file covers the File Manager app
> itself — layout, view modes, file operations, and advanced features.

---

## 1. File Manager Layout & Navigation

### 1.1 Core Layout

**Prompt:** The File Manager window has four zones: toolbar (navigation buttons + address bar), sidebar (quick access links), file area (icon grid or detail table), and status bar (item count + total size). Read directory contents via VFS `readdir()`. Map each file's extension to an icon via `icon_for_extension()` from the icon store (TODO-240 §5.1). Double-click a file calls `file_assoc_open()`. Double-click a folder navigates into it. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apps: file manager core"`. Add notes directly in this TODO section covering the VFS readdir flow and icon lookup.

- [ ] Create `src/apps/filemgr/filemgr.c` and `include/filemgr.h`
- [ ] Window layout: toolbar + sidebar (left panel) + file area + status bar
- [ ] Toolbar: Back (←), Forward (→), Up (↑), address bar (editable path)
- [ ] Address bar: click to type a path, press Enter to navigate
- [ ] File area: read directory via VFS `readdir()`, display files/folders
- [ ] Icons from icon store: `icon_for_extension()` for each file extension
- [ ] Double-click file → `file_assoc_open()` (open with associated app)
- [ ] Double-click folder → navigate into it (update address bar)
- [ ] Navigation: Back/Forward maintain a history stack (last 16 entries)
- [ ] Status bar: `N items` / `N files, N folders, total size`
- [ ] Commit: `"apps: file manager core"`

### 1.2 Sidebar

**Prompt:** The sidebar provides quick navigation to common locations. Show two sections: "Quick Access" (Desktop, Documents, Downloads, Pictures — paths under `C:\Users\Default\`) and "Drives" (list all mounted drives from VFS: C:\, D:\, etc.). Clicking a sidebar item updates the file area to show that path. The sidebar uses a vertical list rendered with icons and labels. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apps: file manager sidebar"`.

- [ ] "Quick Access" section: Desktop, Documents, Downloads, Pictures
- [ ] "Drives" section: list all VFS-mounted drives (C:\, D:\)
- [ ] Click sidebar item → navigate to that path
- [ ] Highlight active sidebar item to match current path
- [ ] Commit: `"apps: file manager sidebar"`

---

## 2. View Modes

**Prompt:** The file area supports two view modes: Icon view (large 48×48 icons in a grid with filenames below each) and Detail view (table with columns: Name, Size, Type, Date Modified). Clicking a column header in Detail view sorts by that column (toggle ascending/descending). The toolbar has view toggle buttons (icon/detail). Remember the user's preference in Registry `HKCU\Software\Impossible\FileManager\ViewMode`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apps: file manager view modes"`.

> **Beats:** Windows Explorer has 8 view modes. Linux Nautilus has 2. Impossible OS: clean 2-mode system — icon + detail. Detail view with sortable columns is the core power-user view.

- [ ] Icon view — 48×48 icons with filename below (default)
- [ ] Detail view — table: Name, Size, Type, Date Modified
- [ ] Sortable columns: click header to sort, click again to reverse
- [ ] Selected file: highlight with accent color
- [ ] Multi-select: Ctrl+click or Shift+click multiple files
- [ ] View toggle buttons in toolbar (icon/detail)
- [ ] Remember view preference: Registry `HKCU\Software\Impossible\FileManager\ViewMode`
- [ ] Commit: `"apps: file manager view modes"`

---

## 3. File Operations

**Prompt:** File operations wire into the clipboard (TODO-230 §1) and recycle bin (TODO-270 §1). Ctrl+C copies the selected file's path to clipboard (`CLIP_FILES` format). Ctrl+V pastes (copies the file to current directory). Ctrl+X cuts (moves file on paste). Delete key moves the file to the recycle bin via `trash_delete()`. F2 triggers inline rename — the filename label becomes an editable textbox, press Enter to confirm, Escape to cancel. Ctrl+Shift+N creates a new folder. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apps: file manager operations"`.

- [ ] Create folder: right-click → New → Folder (or Ctrl+Shift+N)
- [ ] Delete: select → Delete key or right-click → Delete → `trash_delete()`
- [ ] Rename: select → F2 or right-click → Rename → inline textbox edit
- [ ] Copy: Ctrl+C → `clipboard_set(CLIP_FILES, path)` → Ctrl+V → copy to current dir
- [ ] Cut: Ctrl+X → clipboard marks as cut → Ctrl+V → move file
- [ ] Progress dialog: show copy/move progress for large files (> 1 MB)
- [ ] Undo last delete: Ctrl+Z → `trash_restore_all()` (TODO-270 §1)
- [ ] Commit: `"apps: file manager operations"`

---

## 4. Context Menus

**Prompt:** Right-click a file → context menu: Open, Open With..., Cut, Copy, Delete, Rename, Properties. Right-click empty area → New (Folder, Text File), View (Icon/Detail), Sort By, Paste (if clipboard has files). Right-click folder → Open, Open in New Window (stretch), Cut, Copy, Delete, Rename. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: file manager context menus"`.

- [ ] Right-click file → Open, Open With..., Cut, Copy, Delete, Rename, Properties
- [ ] Right-click empty area → New (Folder, Text File, Shortcut), View, Sort By, Paste
- [ ] Right-click folder → same as file + "Open in New Window" (stretch)
- [ ] "Properties" dialog: name, path, size, type, dates, permissions (TODO-300 §3)
- [ ] Commit: `"apps: file manager context menus"`

---

## 5. Advanced Features *(Stretch)*

**Prompt:** These stretch features make the File Manager a power-user tool: search within the current folder (filter the file list as the user types using TODO-260 §4 query API), drag-and-drop files to desktop or between File Manager windows (TODO-170 §5), file/folder properties dialog (show size, path, dates, permissions), preview pane (render text/image files in a right panel), and tabs (multiple directory views in one window using the TabStrip widget). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: file manager advanced features"`.

- [ ] *(Stretch)* Search bar in toolbar → filter current directory via `search_query()`
- [ ] *(Stretch)* Drag and drop: files to/from desktop + between File Manager windows
- [ ] *(Stretch)* File properties dialog: size, full path, created/modified dates, permissions
- [ ] *(Stretch)* Preview pane: render text files (font renderer) + images (stb_image) in right panel
- [ ] *(Stretch)* Tabs: multiple directories in one window (TabStrip from TODO-130 §1.5)
- [ ] *(Stretch)* Breadcrumb address bar: clickable path segments (C:\ › Users › Default)
- [ ] Commit: `"apps: file manager advanced features"`

---

## Priority Order

| Priority | Section                        | Reason                                           |
|----------|--------------------------------|--------------------------------------------------|
| 🔴 P0    | §1.1 Core Layout               | Foundation — no file manager without this        |
| 🟠 P1    | §1.2 Sidebar                   | Quick navigation to common folders               |
| 🟠 P1    | §2 View Modes                  | Icon + Detail view — core browsing               |
| 🟠 P1    | §3 File Operations             | Create/delete/rename/copy/move                   |
| 🟡 P2    | §4 Context Menus               | Right-click interactions                         |
| 🔵 P4    | §5 Advanced Features           | Search, DnD, preview pane, tabs (stretch)        |

---

## Key Files

| File                            | Purpose                                   |
|---------------------------------|-------------------------------------------|
| `src/apps/filemgr/filemgr.c`   | [NEW] File Manager core (layout, nav)     |
| `include/filemgr.h`             | [NEW] File Manager API header             |
| `src/apps/filemgr/filemgr_ops.c`| [NEW] File operations (copy/move/delete) |
| `src/apps/filemgr/filemgr_ctx.c`| [NEW] Context menu handling              |

---

## OS Comparison

| Feature                          | Windows 11 (File Explorer)          | Linux (Nautilus / Dolphin)            | Impossible OS                          |
|----------------------------------|-------------------------------------|---------------------------------------|----------------------------------------|
| Icon view + Detail view          | ✅ 8 view modes (thumbnails, tiles)  | ✅ 2-3 view modes                     | ⬜ §2 P1 — Icon + Detail              |
| Sidebar (quick access + drives)  | ✅ Quick Access + Drive list         | ✅ GNOME Places / KDE sidebar          | ⬜ §1.2 P1                            |
| Sortable columns (Detail view)   | ✅ Click column header               | ✅ Nautilus/Dolphin sortable           | ⬜ §2 P1                              |
| Multi-select (Ctrl/Shift)        | ✅ Full multi-select                 | ✅ Full multi-select                   | ⬜ §2 P1                              |
| File ops (copy/cut/paste/delete) | ✅ Full operations                   | ✅ Full operations                     | ⬜ §3 P1                              |
| Progress dialog for large copies | ✅ Copy dialog with speed/ETA        | ✅ Nautilus/Dolphin progress           | ⬜ §3 P1 (> 1 MB)                    |
| Recycle bin integration          | ✅ Delete → Recycle Bin              | ✅ Trash integration                    | ⬜ §3 P1 — `trash_delete()`          |
| Inline rename (F2)               | ✅ F2 rename                         | ✅ F2 rename                           | ⬜ §3 P1                              |
| Right-click context menus        | ✅ Shell extension menus             | ✅ GNOME/KDE context menus             | ⬜ §4 P2                              |
| Search within folder             | ✅ Windows Search in Explorer        | ✅ Nautilus/Dolphin search bar         | ⬜ §5 P4 (stretch)                    |
| Drag and drop                    | ✅ OLE DnD                            | ✅ GDK/XDnD                            | ⬜ §5 P4 (stretch — TODO-170 §5)     |
| Preview pane                     | ✅ Preview pane in Explorer          | ✅ Dolphin preview                     | ⬜ §5 P4 (stretch)                    |
| Tabs                             | ✅ Windows 11 tabbed Explorer        | ✅ Dolphin / Nautilus (4.2+)           | ⬜ §5 P4 (stretch)                    |
| **Breadcrumb address bar**       | ✅ Clickable path segments           | ✅ Nautilus breadcrumb                 | ⬜ §5 P4 (stretch)                    |
| **No file manager daemon**       | ✅ In-process                        | ✅ GNOME: separate nautilus process    | ✅ **In-kernel app — no separate process** |
