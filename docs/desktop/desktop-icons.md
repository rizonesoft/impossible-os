<!-- docs: covers=todo/06-desktop-foundation/TODO-06-desktop-icons.md sources=src/desktop/desktop.c,src/kernel/icon_store.c,include/icon_store.h,resources/icons/src reviewed=2026-09-29 order=6 -->
# Desktop Icons

## What is it?

Desktop icons are the shortcuts on the wallpaper: This PC, the Recycle Bin, your user folder, and whatever files and shortcuts you save to the Desktop folder. This roadmap plans the whole system: an icon data model on a grid, special folder icons that are always present, icons read from the Desktop directory, selecting, dragging and renaming, `.lnk` shortcuts, icons chosen by file type, and a per-icon context menu. Today the desktop draws three fixed pictures that do nothing when clicked. Four of the seven sections are marked in progress only because they point at shared owners; no section has shipped.

## How does it work?

**Drawing today.** [`desktop.c`](../../src/desktop/desktop.c) holds a constant array of three items, Computer, Recycle Bin and Control Panel, and paints them every frame in a single column on the right edge of the screen: a 48 pixel colour icon with a 20 pixel margin and an 84 pixel cell, and a 14 pixel label with a one-pixel shadow. The Recycle Bin always shows its empty picture. There is no hit testing, selection or launch.

**Icon store.** The pictures come from the kernel icon store, [`icon_store.c`](../../src/kernel/icon_store.c). Colour icons are loaded from the `icons.ires` resource pack in nine sizes from 16 to 256 pixels, and a request picks the closest size. Ten colour icons are wired up, including `ICON_DESKTOP_COMPUTER`, `ICON_RECYCLE_BIN_EMPTY`, `ICON_RECYCLE_BIN_FULL`, `ICON_CONTROL_PANEL` and the folder and file icons ([`icon_store.h`](../../include/icon_store.h)). The source folder [`resources/icons/src`](../../resources/icons/src) already has 26 SVG designs, including the user folder, network, image file and archive icons the desktop will need, but those have no icon ID yet.

**File types.** `icon_for_extension()` maps eight extensions: `.exe`, `.dll` and `.sys` to program and library icons, and `.txt`, `.md`, `.log`, `.cfg` and `.ini` to the text file icon. Anything else, including an extension without its leading dot or in capitals, gets the generic file icon.

**Planned design.**

1. **Model and grid.** A data-driven icon list on a grid of 76 by 86 pixel cells, filled column by column from the top left with an 8 pixel margin, using the `THEME_SIZE_DESKTOP_*` tokens.
2. **Special icons.** This PC, Recycle Bin (switching between empty and full), the user's folder, Network and Control Panel, in a fixed order.
3. **Desktop folder.** Files and folders in `C:\Users\Default\Desktop` become icons, rescanned on Refresh and F5.
4. **Interaction.** Click to select, Ctrl+click and rubber-band selection, drag to move, double-click to open, F2 to rename inline, Delete to recycle.
5. **Shortcuts.** A simple plain-text `.lnk` format (target path, then an optional icon name), not the Windows binary Shell Link format, shown with the shortcut arrow overlay.
6. **File type icons.** Image and archive types added to the extension map, drawing only from the closed icon set.
7. **Context menu.** Open, Open With, Rename, Delete and Properties (no Rename or Delete on special icons), drawn by the shared context menu engine.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `desktop_draw_icons()` | Shipped: draws the three fixed icons |
| `icon_get_colored()`, `icon_get()`, `icon_draw()` | Shipped: icon store lookups |
| `icon_for_extension()` | Shipped: eight extensions |
| Desktop icon model, `.lnk` parser, per-icon menu | Planned |

## How do I use it?

It cannot be used yet beyond seeing the three icons on the desktop.

## What is not implemented yet?

- [Icon Data Model and Grid Layout](../../todo/06-desktop-foundation/TODO-06-desktop-icons.md#1-icon-data-model-and-grid-layout), shared with [Desktop Icons](../../todo/08-graphics-ui/TODO-08-window-manager.md#4-desktop-icons-sonnet) in the window manager roadmap.
- [Special Folder Icons](../../todo/06-desktop-foundation/TODO-06-desktop-icons.md#2-special-folder-icons-always-present) and [Dynamic Icons from Desktop Directory](../../todo/06-desktop-foundation/TODO-06-desktop-icons.md#3-dynamic-icons-from-desktop-directory).
- [Icon Interaction](../../todo/06-desktop-foundation/TODO-06-desktop-icons.md#4-icon-interaction-select-drag-rename) and [Shortcut (.lnk) File Support](../../todo/06-desktop-foundation/TODO-06-desktop-icons.md#5-shortcut-lnk-file-support).
- [File Type Icon Association](../../todo/06-desktop-foundation/TODO-06-desktop-icons.md#6-file-type-icon-association), which also owns the missing leading-dot and case handling.
- [Icon Right-Click Context Menu](../../todo/06-desktop-foundation/TODO-06-desktop-icons.md#7-icon-right-click-context-menu), on the engine in [Context Menu Engine](../../todo/08-graphics-ui/TODO-09-desktop-shell-features.md#1-context-menu-engine-opus).

## How does it compare with Windows 11 and Linux?

Windows 11 shows system icons chosen in Settings plus the contents of the Desktop folder, with shortcuts, file-type icons from the Registry and a full context menu. GNOME shows `~/Desktop` through an extension, uses `.desktop` launcher files as shortcuts and chooses icons by MIME type. Impossible OS draws three fixed icons today. The plan follows the Windows 11 layout, but its `.lnk` files are a simpler plain-text format, so Windows shortcut files will not open as shortcuts.

## See also

- [Desktop Icon System roadmap](../../todo/06-desktop-foundation/TODO-06-desktop-icons.md)
- [Desktop Shell Today](desktop-shell.md)
- [Shell design: desktop](../design/shell.md#desktop)
- [System Icons design](../design/icons.md#the-set)
- [2D Graphics and Visual Assets](../graphics/graphics-assets.md)
