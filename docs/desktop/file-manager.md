<!-- docs: covers=todo/09-desktop-shell/TODO-09-file-manager.md sources=src/desktop/desktop.c,include/icon_store.h,include/kernel/fs/vfs.h,include/desktop/controls.h reviewed=2026-09-29 order=15 -->
# File Manager

## What is it?

The file manager is the File Explorer window for browsing drives and folders, opening files and copying, moving, renaming and deleting them. This roadmap plans a Windows 11-style File Explorer as the first program under `src/apps/`: tabs in the title bar, an address row, a command bar, a navigation pane, icon and details views, file operations with progress, context menus, search, drag and drop and a preview pane. Nothing is implemented yet: there is no file manager program and no `src/apps/` folder.

## How does it work?

**Today.** No window lists files. The pieces it will use exist:

- **VFS.** `vfs_readdir()`, `vfs_finddir()`, `vfs_stat()`, `vfs_create()`, `vfs_rename()` (within one drive), `vfs_unlink()` and `vfs_is_mounted()` cover listing and the basic operations ([`vfs.h`](../../include/kernel/fs/vfs.h)). There is no public `vfs_mkdir()`: a folder is created with `vfs_create()` and the directory type.
- **Icons.** Folder and file-type icons are in the icon store, and `icon_for_extension()` picks a file icon for eight extensions ([`icon_store.h`](../../include/icon_store.h)). The File Explorer artwork exists in `resources/icons/src/file_explorer.svg` but has no icon ID yet.
- **Controls.** The control library has buttons, labels, text boxes and scroll bars only; list views, tab strips and tree views do not exist yet ([`controls.h`](../../include/desktop/controls.h)).
- **Shell entry points.** The Start menu lists Computer, Documents, Pictures, Music, Downloads and Control Panel in its right column, and the desktop shows a Computer icon, but clicking any of them does nothing ([`desktop.c`](../../src/desktop/desktop.c)).

**Planned design.** The layout follows the [File Explorer design](../design/shell.md#file-explorer).

1. **Core layout.** A window with five bands (tabs, address row, command bar, content, status bar), a 16-entry back and forward history, and the folder listing from `vfs_readdir()`.
2. **Navigation pane.** A 220 pixel pane with Home, the user folders and each mounted drive.
3. **Views.** Large icons (48 pixel) and details with sortable name, date, type and size columns, and multi-select.
4. **File operations.** Copy, cut and paste in chunks with a progress dialog, delete to the Recycle Bin, and inline rename.
5. **Context menus.** Open, Open With, Cut, Copy, Delete, Rename and a Properties dialog showing owner and permissions.
6. **Search.** A search box scoped to the current folder, using the file index.
7. **Drag and drop.** A ghost image while dragging, with folders and drives as drop targets.
8. **Advanced.** A preview pane, multiple tabs, a clickable breadcrumb address and a placeholder for network locations.

```mermaid
flowchart TB
    T[Tabs in title bar] --- A[Address row: back, forward, up, breadcrumb, search]
    A --- C[Command bar: New, Cut, Copy, Paste, Rename, Delete, View]
    C --- M[Navigation pane | Content view]
    M --- S[Status bar: item count, selection]
```

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `vfs_readdir()`, `vfs_stat()`, `vfs_rename()`, `vfs_unlink()`, `vfs_create()` | Shipped VFS calls |
| `icon_for_extension()`, `ICON_FOLDER_CLOSED` and the other icons | Shipped |
| `filemgr_*`, `filemgr_copy_file()`, Properties dialog | Planned |
| `CTRL_LISTVIEW`, `CTRL_TABSTRIP`, `context_menu_show()` | Planned by the widget and shell features roadmaps |

## How do I use it?

It cannot be used yet. Use `dir` and `type` in the terminal to look at files.

## What is not implemented yet?

- [Core Layout](../../todo/09-desktop-shell/TODO-09-file-manager.md#1-core-layout-sonnet) and [Sidebar](../../todo/09-desktop-shell/TODO-09-file-manager.md#2-sidebar-sonnet)
- [View Modes](../../todo/09-desktop-shell/TODO-09-file-manager.md#3-view-modes-sonnet), needing the list view in [Complex Controls and Dialogs](../graphics/complex-controls-dialogs.md)
- [File Operations](../../todo/09-desktop-shell/TODO-09-file-manager.md#4-file-operations-sonnet), with delete going to the [Recycle Bin](recycle-bin-zip-scheduler.md)
- [Context Menus](../../todo/09-desktop-shell/TODO-09-file-manager.md#5-context-menus-sonnet), on the engine in [Desktop Shell Features](../graphics/desktop-shell-features.md)
- [File Search](../../todo/09-desktop-shell/TODO-09-file-manager.md#6-file-search-sonnet), on [File Search and Indexing](file-search.md)
- [Drag and Drop](../../todo/09-desktop-shell/TODO-09-file-manager.md#7-drag-and-drop-sonnet) and [Advanced Features](../../todo/09-desktop-shell/TODO-09-file-manager.md#8-advanced-features-sonnet)

## How does it compare with Windows 11 and Linux?

Windows 11 File Explorer has tabs, a simplified command bar, a navigation pane, several icon sizes plus list, details and tiles views, a full copy engine, context menus with shell extensions, indexed search, drag and drop with thumbnails, and preview and details panes. GNOME Files (Nautilus) and Dolphin offer toolbars, sidebars with bookmarks, icon and list views, trash and progress dialogs, indexed search through Tracker or Baloo, and preview panes and tabs. Impossible OS has no file manager yet. The plan follows the Windows 11 layout and shows the IXFS owner and permission bits in Properties.

## See also

- [File Manager roadmap](../../todo/09-desktop-shell/TODO-09-file-manager.md)
- [Shell design: File Explorer](../design/shell.md#file-explorer)
- [Explorer Shell Host](explorer-shell-host.md)
- [Desktop Icons](desktop-icons.md)
- [FAT32 and the VFS](../storage/fat32-vfs.md)
