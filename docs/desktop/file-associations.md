<!-- docs: covers=todo/09-desktop-shell/TODO-02-file-associations-resources.md sources=src/kernel/icon_store.c,include/icon_store.h,src/desktop/desktop.c,include/font_mgr.h,src/kernel/gfx/gfx_text.c,include/registry.h,src/kernel/registry.c reviewed=2026-09-29 order=8 -->
# File Associations, Shortcuts and System Resources

## What is it?

File associations decide which program opens a file, shortcuts are small files that point at another file or program, and system resources are the shared sounds, fonts and icons the shell uses. This roadmap plans all of them: an extension-to-program map in the Registry, default associations written on first boot, an Open With dialog, `.lnk` shortcuts on the desktop and in the Start menu, Recycle Bin icon states, system sounds, a font manager and icon extraction from programs. None of its nine sections has shipped. Only the pieces it builds on exist: a file-type icon lookup, the Registry and the font loader.

## How does it work?

**Today.**

- **File-type icons.** `icon_for_extension()` in [`icon_store.c`](../../src/kernel/icon_store.c) compares the extension against eight entries: `.exe` to the program icon, `.dll` and `.sys` to the library icon, and `.txt`, `.md`, `.log`, `.cfg` and `.ini` to the text file icon. Everything else, including an extension written without its dot or in capitals, gets the generic file icon. It chooses a picture only; it does not say which program opens the file.
- **Registry.** `HKEY_CLASSES_ROOT` is an alias for `HKLM\SOFTWARE\Classes`, which [`registry.c`](../../src/kernel/registry.c) creates empty at boot. No association is written into it.
- **Desktop.** The desktop draws three fixed icons, and the Recycle Bin always shows its empty picture ([`desktop.c`](../../src/desktop/desktop.c)); see [Desktop Icons](desktop-icons.md).
- **Fonts.** The font manager loads five fixed font slots from fixed file names under `C:\Impossible\Fonts\`, with Inter as a fallback ([`gfx_text.c`](../../src/kernel/gfx/gfx_text.c)). There is no directory scan and no way to reload fonts ([`font_mgr.h`](../../include/font_mgr.h)).
- **Sounds.** There is no audio playback path and no sound files.

**Planned design.**

1. **Mapping.** `file_assoc_get_app()`, `file_assoc_get_icon()`, `file_assoc_set()` and `file_assoc_open()`, reading `HKCR\.ext` to a program ID and the program ID's `shell\open\command`, as Windows does.
2. **Defaults.** About a dozen built-in associations written once, guarded by `HKLM\SYSTEM\FirstBoot\FileAssocsInit`.
3. **Open With.** A dialog listing the registered programs, with "Always use this app" writing the default.
4. **Shortcuts.** A plain-text `.lnk` format (target, arguments, working folder, icon), readable in any editor. It is not the Windows binary Shell Link format, so Windows shortcut files will not work as shortcuts.
5. **Desktop and Start menu.** Shortcut arrow overlays and Start menu entries built from shortcut folders.
6. **Recycle Bin states.** The empty and full icons driven by the Recycle Bin's item count.
7. **System sounds.** A WAV parser and named system sounds, logged to serial until an audio driver exists.
8. **Font manager.** A window to list, preview, install and remove TrueType fonts.
9. **Icon extraction.** Icons read from EIF and PE program resources at any size.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `icon_for_extension()` | Shipped: eight extensions, icon only |
| `RegGetValue()`, `RegSetValueEx()`, `HKEY_CLASSES_ROOT` | Shipped: the Registry API the plan uses ([`registry.h`](../../include/registry.h)) |
| `ttf_get()`, `ttf_draw_string()` | Shipped: fixed font slots |
| `file_assoc_*`, `shortcut_parse()`, `shortcut_execute()`, `shortcut_create()` | Planned |
| `ttf_mgr_reload()`, WAV playback, icon extraction | Planned |

## How do I use it?

It cannot be used yet. Nothing opens a file by type today: the shell runs only its built-in commands, so the one way to look at a file is `type <file>` (or `cat`) for text.

## What is not implemented yet?

- [Extension-to-App Mapping](../../todo/09-desktop-shell/TODO-02-file-associations-resources.md#1-extension-to-app-mapping-sonnet) and [Default Associations on First Boot](../../todo/09-desktop-shell/TODO-02-file-associations-resources.md#2-default-associations-on-first-boot-sonnet)
- [Open With Dialog](../../todo/09-desktop-shell/TODO-02-file-associations-resources.md#3-open-with-dialog-sonnet)
- [Shortcut Files (.lnk)](../../todo/09-desktop-shell/TODO-02-file-associations-resources.md#4-shortcut-files-lnk-sonnet) and [Desktop Shortcut Integration](../../todo/09-desktop-shell/TODO-02-file-associations-resources.md#5-desktop-shortcut-integration-sonnet)
- [Recycle Bin Desktop Icon States](../../todo/09-desktop-shell/TODO-02-file-associations-resources.md#6-recycle-bin-desktop-icon-states-sonnet), with the Recycle Bin itself owned by [Recycle Bin, ZIP and Task Scheduler](recycle-bin-zip-scheduler.md)
- [System Sounds](../../todo/09-desktop-shell/TODO-02-file-associations-resources.md#7-system-sounds-sonnet), [Font Manager App](../../todo/09-desktop-shell/TODO-02-file-associations-resources.md#8-font-manager-app-sonnet) and [Icon Extraction](../../todo/09-desktop-shell/TODO-02-file-associations-resources.md#9-icon-extraction-eif-svg-and-pe-ico-resources)

The desktop icon roadmap also plans a `.lnk` format and file-type icons ([Desktop Icons](desktop-icons.md)); both plans describe the same plain-text shortcut, and the missing leading-dot and case handling in `icon_for_extension()` is owned there.

## How does it compare with Windows 11 and Linux?

Windows 11 resolves a file through `HKCR` and `ShellExecuteEx`, ships default associations, offers Open With and "Always use", uses binary Shell Link `.lnk` files, and has sound schemes and a Fonts page in Settings. Linux desktops use the XDG MIME database (`xdg-open`, `mimeapps.list`), `.desktop` launcher files, freedesktop sound themes and GNOME or KDE font viewers. Impossible OS has only file-type icons today. The plan copies the Windows Registry layout so Windows tools find what they expect, and chooses a human-editable shortcut format instead of the binary one.

## See also

- [File Associations, Shortcuts and System Resources roadmap](../../todo/09-desktop-shell/TODO-02-file-associations-resources.md)
- [Registry](../kernel/registry.md)
- [Desktop Icons](desktop-icons.md)
- [Text and Fonts](../graphics/text-fonts.md)
- [2D Graphics and Visual Assets](../graphics/graphics-assets.md)
- [System icons design](../design/icons.md)
