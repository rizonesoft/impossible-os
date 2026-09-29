<!-- docs: covers=todo/09-desktop-shell/TODO-10-notepad.md sources=include/font_mgr.h,include/kernel/fs/vfs.h,include/kernel/mm/pmm.h,include/desktop/controls.h,src/desktop/desktop.c reviewed=2026-09-29 order=16 -->
# Notepad

## What is it?

Notepad is the plain-text editor: open a text file, edit it, save it. This roadmap plans it as a desktop program under `src/apps/notepad/`, built on a gap buffer, with TrueType rendering and a caret, a 200-step undo history, a File menu with open and save dialogs, mouse selection and clipboard, find and replace, go to line, and optional line numbers, syntax colouring and zoom. Nothing is implemented yet: there is no editor program and no Start menu entry for one.

## How does it work?

**Today.** No text editor exists; `type` in the shell is the only way to read a text file, and there is no way to edit one. The building blocks it will use have shipped:

- **Text.** `ttf_draw_string()`, `ttf_measure_width()` and `ttf_get()` draw and measure TrueType text, and `FONT_MONO` selects the monospace face ([`font_mgr.h`](../../include/font_mgr.h)).
- **Files.** `vfs_open()`, `vfs_read()`, `vfs_write()`, `vfs_stat()` and `vfs_create()` ([`vfs.h`](../../include/kernel/fs/vfs.h)).
- **Memory.** `pmm_alloc_contiguous()` for buffers larger than 4 KB ([`pmm.h`](../../include/kernel/mm/pmm.h)).

What it needs and does not have yet: a menu bar, a status bar, file open and save dialogs, context menus and the clipboard. The control library has only buttons, labels, text boxes and scroll bars ([`controls.h`](../../include/desktop/controls.h)), and the Start menu has no Notepad entry ([`desktop.c`](../../src/desktop/desktop.c)).

**Planned design.**

1. **Gap buffer.** `text_buffer_t`: the text with a movable gap at the caret, so typing is a constant-time insert. It grows by reallocating, and loads and saves whole files.
2. **Rendering and caret.** Line-by-line TrueType drawing, an I-beam caret, and arrow, Home, End, Page Up and Page Down navigation.
3. **Undo and redo.** A stack of 200 insert and delete actions on Ctrl+Z and Ctrl+Y, with a whole paste recorded as one action.
4. **File menu.** New, Open, Save and Save As through the menu bar and the file dialogs, with an asterisk in the title while there are unsaved changes and a prompt before discarding them.
5. **Editing.** Mouse and Shift selection, Ctrl+A, cut, copy and paste, a scroll bar, and a status bar with the line and column.
6. **Find and replace.** A find bar that slides in without blocking the text, replace and replace all, and Go to Line.
7. **Extras.** A line-number gutter, colouring for C, assembly and Markdown, and Ctrl+scroll zoom.

The window follows the [window chrome](../design/shell.md#window-chrome), [menu bar](../design/controls.md#menu-bar-and-menus) and [dialog](../design/controls.md#dialog) designs.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `ttf_draw_string()`, `ttf_measure_width()`, `ttf_get(FONT_MONO, ...)` | Shipped |
| `vfs_open()`, `vfs_read()`, `vfs_write()` | Shipped |
| `text_buffer_t`, `notepad.exe` | Planned |
| `CTRL_MENUBAR`, `CTRL_STATUSBAR`, `dialog_file_open()`, `dialog_file_save()` | Planned by [Complex Controls and Dialogs](../graphics/complex-controls-dialogs.md) |

## How do I use it?

It cannot be used yet. Read a text file with `type <file>` in the terminal.

## What is not implemented yet?

- [Gap Buffer](../../todo/09-desktop-shell/TODO-10-notepad.md#1-gap-buffer-opus)
- [Text Rendering and Cursor](../../todo/09-desktop-shell/TODO-10-notepad.md#2-text-rendering--cursor-sonnet)
- [Undo/Redo](../../todo/09-desktop-shell/TODO-10-notepad.md#3-undoredo-sonnet)
- [File Menu](../../todo/09-desktop-shell/TODO-10-notepad.md#4-file-menu-sonnet), which needs the menu bar and file dialogs
- [Editing Features](../../todo/09-desktop-shell/TODO-10-notepad.md#5-editing-features-sonnet), which needs the [Clipboard](clipboard.md)
- [Find and Replace](../../todo/09-desktop-shell/TODO-10-notepad.md#6-find--replace-sonnet)
- [Stretch Features](../../todo/09-desktop-shell/TODO-10-notepad.md#7-stretch-features-sonnet)

A second roadmap, [Notepad in the applications domain](../../todo/11-apps/TODO-08-notepad.md), covers the same program and refers back to this one for the editor core.

## How does it compare with Windows 11 and Linux?

Windows 11 Notepad has unlimited undo, a full File menu, an inline find bar and Ctrl+scroll zoom, but no syntax colouring. gedit and Kate use piece-table buffers, unlimited undo and syntax colouring through GtkSourceView or KSyntaxHighlighting. Impossible OS has no editor yet. The plan keeps the editor small (a gap buffer and a 200-step undo ring) and adds syntax colouring for C, assembly and Markdown, which Windows Notepad lacks.

## See also

- [Notepad Text Editor roadmap](../../todo/09-desktop-shell/TODO-10-notepad.md)
- [Text and Fonts](../graphics/text-fonts.md)
- [Complex Controls and Dialogs](../graphics/complex-controls-dialogs.md)
- [File Associations, Shortcuts and System Resources](file-associations.md)
