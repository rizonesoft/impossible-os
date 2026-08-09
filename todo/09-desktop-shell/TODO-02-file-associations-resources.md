---
schema_version: 1
id: file-associations-resources
domain: 09-desktop-shell
status: active
title: "TODO-02 -- File Associations, Shortcuts & System Resources"
---

# TODO-02 -- File Associations, Shortcuts & System Resources

> **Goal:** Build the plumbing that makes double-click work -- extension-to-app Registry mapping, first-boot defaults, Open With dialog, INI-format `.lnk` shortcuts with desktop integration, Recycle Bin icon state, system sounds (WAV player), and a font manager app.

> [!IMPORTANT]
> **Already exists**: `icon_for_extension(const char *ext)` in `icon_store.h` -- returns `system_icon_t` for any file extension. `RegOpenKeyEx/RegSetValueEx/RegGetValue` + `HKCR` support in `registry.h`. `vfs_open/read/write/create` in `vfs.h`. `task_exec(data, size)` in `task.h`. `context_menu_show()` (TODO-07 §1) for right-click menus. `dialog_input()` + `CTRL_LISTVIEW` (TODO-05) for Open With dialog. `ttf_get()` + `ttf_draw_string()` in `font_mgr.h` for font preview. `gfx_blit_alpha()` for shortcut arrow overlay. `notify_send()` (TODO-09 §5) for install success toasts. **Missing**: `file_assoc_*`, `shortcut_*`, `trash_*`, WAV player, font manager app, `ttf_mgr_reload()`. **WAV audio**: depends on `04-drivers-hardware/TODO-18-audio-drivers.md` (AC97/HDA driver); §7 implements the WAV parser + player stub that logs to serial if the audio driver is not yet live. Complete sections in order: extension mapping → default associations → shortcut files → desktop shortcut integration → recycle bin → Open With dialog → system sounds → font manager.

## Inputs

- `include/icon_store.h` -- `icon_for_extension(ext)`, `icon_get()`, `ICON_TRASH_EMPTY/FULL` -- used by §1 and §6 trash icon state
- `include/registry.h` -- `RegOpenKeyEx`, `RegSetValueEx`, `RegGetValue`, `HKCR` macros -- used by §1 HKCR reads and §2 first-boot writes
- `include/kernel/fs/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create`, `vfs_readdir` -- used by §4 .lnk parsing, §8 font scan
- `include/kernel/sched/task.h` -- `task_exec(data, size)` -- used by §1 `file_assoc_open()` and §4 `shortcut_execute()`
- `include/desktop/context_menu.h` (TODO-07 §1) -- `context_menu_show()` -- used by §6 recycle bin right-click and §3 Open With
- `include/desktop/controls.h` (TODO-05) -- `CTRL_LISTVIEW`, `dialog_input()` -- used by §3 Open With app list and §8 font list
- `include/font_mgr.h` -- `ttf_get()`, `ttf_draw_string()` -- used by §8 font preview rendering
- `include/gfx.h` -- `gfx_blit_alpha()`, `gfx_fill_rounded_rect()`, `gfx_draw_line()` -- used by §5 shortcut arrow overlay and §7 system sounds UI stub
- `include/desktop/notify.h` (TODO-09 §5) -- `notify_send()` -- §8 font install success toast
- → XREF: `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §1` -- Start Menu reads `.lnk` files from `C:\Users\Default\AppData\StartMenu\`; §5 must create those default shortcuts on first boot
- → XREF: `08-graphics-ui/TODO-08-window-manager.md §3` -- desktop icons double-click calls `file_assoc_open()` from §1
- → XREF: `04-drivers-hardware/TODO-18-audio-drivers.md` (AC97/HDA audio) -- §7 WAV player requires audio output driver; use serial log stub until driver is live
- Related (no stable XREF target): `11-apps/TODO-*` (File Manager) -- File Manager double-click calls `file_assoc_open()` from §1; File Manager copy/delete integrates with §6 Recycle Bin

## Outcome

- `file_assoc_open(filepath)` looks up extension → HKCR → launches associated app with file path arg.
- First-boot HKCR defaults registered for `.txt/log/ini`, `.png/jpg/bmp`, `.lnk`, etc.
- INI-format `.lnk` shortcuts: `shortcut_parse/execute/create`; shortcut arrow overlay on desktop.
- Start Menu auto-populated from `.lnk` files under `C:\Users\Default\AppData\StartMenu\`.
- Recycle Bin icon switches between empty/full based on `trash_count()`; right-click → empty/open.
- Open With dialog lists registered apps + "Always use" option writes new HKCR default.
- System sounds (startup, error, notify, click) via minimal WAV parser → AC97/HDA PCM stub.
- Font manager app: list/preview/install/remove TTF fonts; set default font Registry key.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                       | Depends On                                                                      | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Extension-to-app mapping -- `file_assoc_get_app/icon/set/open` via HKCR                        | `RegGetValue` (exists); `icon_for_extension` (exists); `task_exec` (exists)     |  [ ]   |
| 💎  |   2   | §2 Default associations on first boot -- write HKCR defaults for built-in file types              | §1 API must exist before writing default entries that §1 will later read        |  [ ]   |
| 💎  |   3   | §4 Shortcut files (.lnk) -- INI parse/execute/create; `shortcut_execute()` stub                   | §1 (`.lnk` association → `shortcut_execute()`); `vfs_read` (exists)             |  [ ]   |
| 💎  |   4   | §5 Desktop shortcut integration -- .lnk detection, arrow overlay, Start Menu, first-boot defaults | §3 shortcut parse; TODO-07 desktop icon rendering                               |  [ ]   |
| 💎  |   5   | §6 Recycle Bin icon states -- `trash_count()`, ICON_TRASH_EMPTY/FULL, right-click menu            | §1 `file_assoc_open()` for double-click "Open Recycle Bin"; §4 context menu     |  [ ]   |
| 💎  |   6   | §3 Open With dialog -- app list popup, "Always use" writes HKCR default                           | §2 defaults (app list sourced from HKCR); §5 context menu (`context_menu_show`) |  [ ]   |
| 💎  |   7   | §7 System sounds -- WAV parser, PCM → audio stub, Registry enable, startup chime                  | §6 (first-boot associations must be stable); audio TODO-10 forward ref          |  [ ]   |
| 💎  |   8   | §8 Font manager app -- list/preview/install/remove TTF, set default                               | §7 stable; `ttf_get()` + `vfs_readdir()` (both exist)                           |  [ ]   |

---

## 1. Extension-to-App Mapping `[Sonnet]`

`file_assoc_get_app(ext)` reads `HKCR\.{ext}\(Default)` → prog ID → `HKCR\{progid}\shell\open\command`. `file_assoc_get_icon(ext)` reads `HKCR\.{ext}\DefaultIcon`. `file_assoc_set(ext, app_path)` writes Registry. `file_assoc_open(filepath)` extracts extension, looks up app, calls `task_exec`.

**Files:** `src/desktop/file_assoc.c` (new), `include/desktop/file_assoc.h` (new)

> [!NOTE]
> Follow the Win32 HKCR layout exactly: `HKCR\.txt` = `(Default)` value → "txtfile"; `HKCR\txtfile\shell\open\command` = `(Default)` → `"notepad.exe %1"`. `file_assoc_get_app(ext)`: `RegGetValue(HKCR, ".\{ext}", "(Default)")` → `prog_id`; `RegGetValue(HKCR, "{prog_id}\\shell\\open\\command", "(Default)")` → command template. Command template substitution: replace `%1` with the quoted filepath. App path extraction: take the first token before a space (handles `"notepad.exe %1"` → `notepad.exe`). `file_assoc_open(filepath)`: extract ext from last `.`; `file_assoc_get_app(ext)` → command; build argv; load app ELF/EIF via `vfs_open(app_path)` → `vfs_read()` → `task_exec(data, size)`. `file_assoc_get_icon(ext)`: try `HKCR\.{ext}\DefaultIcon`; fallback to `icon_for_extension(ext)`.

- [ ] `int file_assoc_get_app(const char *ext, char *cmd_buf, uint32_t max)` -- HKCR lookup chain; return 0 on found, -1 on missing
- [ ] `system_icon_t file_assoc_get_icon(const char *ext)` -- HKCR DefaultIcon → parse path; fallback `icon_for_extension(ext)`
- [ ] `int file_assoc_set(const char *ext, const char *prog_id, const char *cmd_template)` -- write `HKCR\.{ext}\(Default)` + `HKCR\{prog_id}\shell\open\command\(Default)`
- [ ] `int file_assoc_open(const char *filepath)` -- extract ext; get cmd; substitute `%1`; load app via VFS; `task_exec()`; log `[assoc] open: %s via %s`; return -1 if no association → `file_assoc_open_with_dialog()`
- [ ] `int file_assoc_has(const char *ext)` -- returns 1 if HKCR entry exists
- [ ] Error path: if no association → `file_assoc_open_with_dialog(filepath)` (§3); if app not found: `notify_send("Cannot Open", "No app for .{ext}", ICON_ERROR, 4000)`
- [ ] Commit: `"file_assoc: extension-to-app mapping -- HKCR prog_id chain, file_assoc_open(), icon fallback"`

## 2. Default Associations on First Boot `[Sonnet]`

Register built-in defaults at first boot: `.txt/.log/.ini/.c/.h/.asm/.md` → `notepad.exe`, `.png/.jpg/.bmp/.gif` → `imgview.exe`, `.zip` → `archiver.exe`, `.lnk` → `shortcut_execute()`, `.ttf` → `fontmgr.exe`. Write to `HKCR` via `file_assoc_set()`. Guard with `HKLM\SYSTEM\FirstBoot\FileAssocsInit` flag.

**Files:** `src/desktop/file_assoc.c` (extend)

> [!NOTE]
> `file_assoc_defaults_init()`: check `HKLM\SYSTEM\FirstBoot\FileAssocsInit` DWORD; if 0 or absent: write all defaults; set flag to 1. This prevents re-writing on every boot. Association table: static array of `{ const char *ext; const char *prog_id; const char *cmd; }` -- call `file_assoc_set()` for each. `.lnk` association: prog_id = "lnkfile"; command = `"shortcut_exec.exe %1"` -- but since shortcuts are handled natively, `file_assoc_open()` special-cases `.lnk` to call `shortcut_execute(path)` directly without loading a separate app.

- [ ] `static const struct { char ext[8]; char prog_id[32]; char cmd[64]; } g_default_assocs[]` -- ~12 entries
- [ ] `void file_assoc_defaults_init(void)`: guard flag check; iterate table; `file_assoc_set()` for each; persist flag
- [ ] Special-case `.lnk` in `file_assoc_open()`: if `kstrcmp(ext, "lnk") == 0` → `shortcut_execute(filepath)` directly (§3)
- [ ] Special-case `.ttf` → launch `fontmgr.exe` with path arg (§8)
- [ ] `file_assoc_defaults_init()` called from `desktop_init()` after Registry is live
- [ ] Commit: `"file_assoc: first-boot defaults -- txt/png/zip/lnk/ttf HKCR entries, FirstBoot guard flag"`

## 3. Open With Dialog `[Sonnet]`

`file_assoc_open_with_dialog(filepath)`: popup listing all registered apps from HKCR + "Browse…" button. Option "Always use this app for .{ext} files" writes new HKCR default. Called when `file_assoc_open()` finds no association.

**Files:** `src/desktop/file_assoc_openwith.c` (new), `include/desktop/file_assoc.h` (extend)

> [!NOTE]
> App list: enumerate `HKCR` subkeys where key has `\shell\open\command` → extract app name + icon. Show in a `CTRL_LISTVIEW` (single-column, icon + app name, 320×280 px modal window). "Browse…" button: `dialog_file_open("*.exe;*.eif", "C:\\Impossible\\System32\\", app_path, MAX_PATH)`. "Always use" checkbox: if checked → `file_assoc_set(ext, prog_id, cmd)` when user clicks OK. "Just once" → launch without saving. Cancel → do nothing. Modal: block using `dialog_input` pattern (inner message loop from TODO-11 §6).

- [ ] `void file_assoc_open_with_dialog(const char *filepath)` -- create 360×320 px modal window; populate `CTRL_LISTVIEW` with HKCR app scan
- [ ] `void file_assoc_scan_all_apps(app_entry_t *out, int *count)` -- enumerate HKCR keys with `shell\open\command`; fill array
- [ ] "Browse…" button → `dialog_file_open()` → add to list
- [ ] "Always use" checkbox state persists across OK click
- [ ] OK + selected app: if "Always use" → `file_assoc_set(ext, ...)`; then `file_assoc_open_impl(filepath, cmd)`
- [ ] Commit: `"file_assoc: Open With dialog -- CTRL_LISTVIEW app scan, Browse, Always use HKCR write"`

## 4. Shortcut Files (.lnk) `[Sonnet]`

INI-format text file: `[Shortcut]`, `Target=`, `Args=`, `Icon=`, `WorkDir=`, `Description=`. `shortcut_parse(path, out)` reads INI fields via VFS. `shortcut_execute(path)` calls `task_exec(target, args)`. `shortcut_create(target, icon, desc, dest_path)` writes INI file.

**Files:** `src/desktop/shortcut.c` (new), `include/desktop/shortcut.h` (new)

> [!NOTE]
> INI parsing: `vfs_open(path)` → `vfs_read(node, 0, size, buf)` → scan lines with a simple `key=value` tokenizer (no libc `strtok` -- hand-coded). Parse `[Shortcut]` section header; for each line: split at first `=`; match key case-insensitively. `shortcut_execute()`: build argv from `target + " " + args`; `vfs_open(target)` → `task_exec(data, size)`. Working directory: store in `task_create` arg (stub if `task_exec` doesn't support cwd yet -- log to serial). `shortcut_create()`: `vfs_create(dest_path, VFS_TYPE_FILE)`; write INI text via `vfs_write()` with `ksnprintf`-formatted content.

- [ ] `typedef struct { char target[256]; char args[128]; char icon_path[256]; char workdir[256]; char description[128]; } shortcut_t;` in `shortcut.h`
- [ ] `int shortcut_parse(const char *path, shortcut_t *out)` -- VFS read; INI key=value tokenizer; return 0 or -errno
- [ ] `int shortcut_execute(const char *path)` -- `shortcut_parse()`; `vfs_open(target)` → `task_exec()`; return 0 or -1
- [ ] `int shortcut_create(const char *target, const char *icon, const char *desc, const char *dest_path)` -- write INI text; `vfs_create()` + `vfs_write()`
- [ ] `shortcut_is_valid(shortcut_t *sc)` -- check `target[0] != '\0'` and target file exists via `vfs_finddir()`
- [ ] Commit: `"shortcut: INI .lnk format -- shortcut_parse/execute/create, VFS read, task_exec launch"`

## 5. Desktop Shortcut Integration `[Sonnet]`

Desktop renderer detects `.lnk` files; uses `Icon=` field + `Description=` as label. Shortcut arrow overlay (small ↗ glyph in lower-left of icon). Double-click → `shortcut_execute()`. Start Menu reads `.lnk` from `C:\Users\Default\AppData\StartMenu\`. First-boot default shortcuts: Terminal, Notepad, Settings, File Manager.

**Files:** `src/desktop/desktop_icons.c` (extend), `src/desktop/shortcut.c` (extend)

> [!NOTE]
> Arrow overlay: after drawing the icon bitmap, draw a 10×10 px sub-glyph using Fluent icon codepoints (U+E0F5 shortcut arrow) at the lower-left corner of the icon cell. Use `ttf_draw_char(s, FONT_UI_ICON, lx, ly, 0xE0F5, 0xFFFFFFFF)` with a 1 px drop shadow for legibility. **Icon selection**: `shortcut_parse()` gives `icon_path`; if non-empty: `image_load(icon_path)` and display; else: `icon_for_extension(target_ext)`. Start Menu integration: `startmenu_scan_apps()` (TODO-09 §1) already scans `C:\Impossible\Bin\`; add scan of `C:\Users\Default\AppData\StartMenu\` for `.lnk` files → `shortcut_parse()` → use `Description` as app name, `Icon` field for icon. **First-boot shortcuts**: `shortcut_defaults_init()` creates 4 `.lnk` files in `C:\Users\Default\AppData\StartMenu\` and 4 on `C:\Users\Default\Desktop\` (Terminal, Notepad, Settings, File Manager); guarded by `HKLM\SYSTEM\FirstBoot\ShortcutsInit` flag.

- [ ] Arrow overlay in `desktop_icon_draw(s, icon)`: after `icon_draw_scaled()`; draw Fluent arrow glyph at `(icon_x, icon_y + icon_h - 10)` if `is_shortcut`
- [ ] `is_shortcut` detection: `kstrcmpi(ext, "lnk") == 0`; load `shortcut_t` to get icon + label
- [ ] Desktop icon double-click: if `.lnk` → `shortcut_execute(filepath)` instead of `file_assoc_open()`
- [ ] `startmenu_scan_apps()`: add `C:\Users\Default\AppData\StartMenu\` scan; parse `.lnk`; use `Description` as name
- [ ] `void shortcut_defaults_init(void)`: create `.lnk` files for Terminal/Notepad/Settings/File Manager in StartMenu + Desktop dirs; guard with `FirstBoot\ShortcutsInit` flag
- [ ] Commit: `"shortcuts: desktop integration -- arrow overlay, .lnk double-click, Start Menu scan, first-boot defaults"`

## 6. Recycle Bin Desktop Icon States `[Sonnet]`

`ICON_TRASH_EMPTY` when `trash_count()==0`, `ICON_TRASH_FULL` otherwise. Refresh on every `trash_delete()` / `trash_empty()`. Right-click → context menu: "Open Recycle Bin", "Empty Recycle Bin" (confirm dialog). `HKLM\SYSTEM\Recycle\MaxSize` (default 1 GiB) auto-purges oldest items when over limit.

**Files:** `src/desktop/trash.c` (new), `include/desktop/trash.h` (new), `src/desktop/desktop_icons.c` (extend)

> [!NOTE]
> Trash directory: `C:\Recycle\` (already defined in OS filesystem layout). `trash_delete(filepath)`: move file to `C:\Recycle\{original_name}_{timestamp}`; store metadata `.{filename}.meta` containing original path + timestamp. `trash_empty()`: iterate `C:\Recycle\`; delete each file + `.meta`; `wm_mark_dirty()`. `trash_count()`: `vfs_readdir("C:\\Recycle\\")` counting non-`.meta` files. `trash_restore(filename)`: read `.meta`; move back to original path. **Auto-purge**: on `trash_delete()`: compute total size; if > `MaxSize` → delete oldest (by timestamp in `.meta`) until under limit. **Desktop icon**: registered as a special desktop icon in `desktop_icons_init()`; `is_trash = 1` flag; `desktop_icon_draw()` uses `ICON_TRASH_FULL` if `trash_count() > 0` else `ICON_TRASH_EMPTY`.

- [ ] `int trash_delete(const char *filepath)` -- move to `C:\Recycle\`; write `.meta` file; return 0 or -errno
- [ ] `int trash_restore(const char *name)` -- read `.meta`; `vfs_rename()` back; delete `.meta`
- [ ] `void trash_empty(void)` -- delete all files in `C:\Recycle\`; `wm_mark_dirty()`
- [ ] `int trash_count(void)` -- count non-`.meta` files in `C:\Recycle\`
- [ ] `uint64_t trash_total_size(void)` -- sum file sizes; used for MaxSize enforcement
- [ ] Desktop trash icon: registered in `desktop_icons_init()` at fixed position (bottom-right); `icon_id` updated on every `trash_delete/empty/restore` call
- [ ] Right-click trash icon → `context_menu_show()`: "Open Recycle Bin" (opens File Manager at `C:\Recycle\`), "Empty Recycle Bin" → `dialog_input`-style confirm → `trash_empty()`
- [ ] `HKLM\SYSTEM\Recycle\MaxSize` DWORD read in `trash_delete()` for auto-purge; default 1 GiB (1073741824)
- [ ] Commit: `"trash: recycle bin -- trash_delete/restore/empty/count, auto-purge, desktop icon states, right-click menu"`

## 7. System Sounds `[Sonnet]`

WAV files (22050 Hz mono 16-bit) in `resources/sounds/`; install to `C:\Impossible\Media\`. Minimal WAV parser: 44-byte header, feed PCM samples to AC97/HDA driver (`04-drivers-hardware/TODO-18-audio-drivers.md`). Play on: boot splash finish, error dialogs, toast notifications, shutdown. Registry `HKLM\SYSTEM\Sound\SystemSounds` enable/disable.

**Files:** `src/kernel/sound/wav.c` (new), `include/kernel/sound/wav.h` (new), `src/desktop/system_sounds.c` (new)

> [!NOTE]
> WAV header layout: `RIFF` (4), file size (4), `WAVE` (4), `fmt ` (4), chunk size (4), audio format (2, must be 1=PCM), num channels (2), sample rate (4), byte rate (4), block align (2), bits per sample (2, must be 16), `data` (4), data size (4) = 44 bytes total. Validate: check `audio_format==1`, `num_channels==1`, `sample_rate==22050`, `bits_per_sample==16`; on mismatch: log warning and skip. PCM output: call `audio_write_pcm(samples, sample_count)` from audio driver; if driver not live: serial log `[sound] play: %s (%u samples) -- audio not ready`. **Sound events**: `system_sound_play(SOUND_STARTUP)` called from `boot_splash_finish()`; `SOUND_ERROR` from `MessageBox(…, MB_ICONERROR)`; `SOUND_NOTIFY` from `notify_send()`; `SOUND_CLICK` from button click handler; `SOUND_RECYCLE` from `trash_delete()`. Sound assets: add `resources/sounds/*.wav` to build system copy to `C:\Impossible\Media\`.

- [ ] `typedef struct { uint16_t audio_format; uint16_t channels; uint32_t sample_rate; uint16_t bits_per_sample; uint32_t data_offset; uint32_t data_size; } wav_header_t;`
- [ ] `int wav_parse_header(const uint8_t *buf, uint32_t size, wav_header_t *out)` -- validate RIFF/WAVE/fmt/data chunks; return 0 or -1
- [ ] `int wav_play(const char *path)` -- `vfs_open()` + `vfs_read()`; `wav_parse_header()`; `audio_write_pcm(pcm_samples, sample_count)` stub; return 0
- [ ] `typedef enum { SOUND_STARTUP, SOUND_CLICK, SOUND_ERROR, SOUND_NOTIFY, SOUND_SHUTDOWN, SOUND_RECYCLE } sound_event_t;`
- [ ] `void system_sound_play(sound_event_t ev)` -- check `HKLM\SYSTEM\Sound\SystemSounds` DWORD; if disabled: return; map event → WAV path; `wav_play()`
- [ ] Wire: `boot_splash_finish()` → `system_sound_play(SOUND_STARTUP)`; `notify_send()` (TODO-09) → `system_sound_play(SOUND_NOTIFY)` if sound enabled; `MessageBox(MB_ICONERROR)` → `SOUND_ERROR`; `trash_delete()` → `SOUND_RECYCLE`
- [ ] Build: add `resources/sounds/` with stub silent WAV files (44-byte header + 1 sample); replace with real assets later
- [ ] Commit: `"system_sounds: WAV parser, PCM output stub, SOUND_* events wired to splash/notify/dialog"`

## 8. Font Manager App `[Sonnet]`

`src/apps/fontmgr/fontmgr.c`: list `.ttf` from `C:\Impossible\Fonts\` via VFS. Preview at 12/16/24/36 px with "The quick brown fox…". Install button: copy file + `ttf_mgr_reload()`. Remove button (block system fonts Selawik/Cascadia). Set Default: write `HKCU\Software\Impossible\Theme\Font`.

**Files:** `src/apps/fontmgr/fontmgr.c` (new), `include/apps/fontmgr.h` (new)

> [!NOTE]
> Launch as a desktop window app (not kernel-mode -- but still runs in kernel mode for now, like `gallery.c`). Window: 640×480 px. Left panel: `CTRL_LISTVIEW` of `.ttf` files in `C:\Impossible\Fonts\`; on selection-change: update right preview panel. Right panel: fixed preview strings at 4 sizes using `ttf_get(FONT_UI, px)` where the font slot is temporarily overridden with the selected TTF. **Install**: `dialog_file_open("*.ttf", "C:\\", src_path, MAX_PATH)` → `vfs_copy(src, "C:\\Impossible\\Fonts\\basename")` → `ttf_mgr_reload()` (re-scans `C:\Impossible\Fonts\` and rebuilds the font slot table) → `notify_send("Font installed", basename, ICON_INFO, 3000)`. **Remove guard**: check if font is Selawik or Cascadia (system fonts); if yes: `notify_send("Cannot remove", "System font", ICON_WARNING, 3000)`. **Set Default**: `RegSetValueEx(HKCU, "Software\\Impossible\\Theme\\Font", font_path)` + `wm_post_message_all(WM_THEME_CHANGED, ...)` (TODO-01 §7).

- [ ] `void fontmgr_open(void)` -- create 640×480 px window; scan `C:\Impossible\Fonts\`; populate `CTRL_LISTVIEW`
- [ ] `void fontmgr_draw_preview(gfx_surface_t *s, const char *ttf_path, int32_t x, int32_t y, int32_t w, int32_t h)` -- render preview string at 4 sizes using `ttf_get()` with the font loaded temporarily
- [ ] `int ttf_mgr_reload(void)` -- re-scan `C:\Impossible\Fonts\`; rebuild font cache; return 0 or -1 (add to `font_mgr.h`)
- [ ] Install button callback: `dialog_file_open()` → `vfs_copy()` → `ttf_mgr_reload()` → `notify_send()`; refresh list
- [ ] Remove button callback: guard system fonts ("Selawik", "Cascadia"); `vfs_delete(font_path)`; `ttf_mgr_reload()`; refresh list
- [ ] "Set Default" button: `RegSetValueEx(HKCU, "...\\Theme\\Font", REG_SZ, ttf_path)`; `wm_post_message_all(WM_THEME_CHANGED, 0, 0)`
- [ ] `.ttf` file association (§1/§2) → launches `fontmgr.exe` with the TTF path as arg → preview the passed font
- [ ] Commit: `"fontmgr: font manager app -- list/preview/install/remove TTF, set default, ttf_mgr_reload()"`

---

## OS Comparison


| ⭐  | Feature                         | 🪟 Win11                                         | 🐧 Linux                                                          | 🚀 Impossible OS                                                        |
| --- | ------------------------------- | ------------------------------------------------ | ----------------------------------------------------------------- | ----------------------------------------------------------------------- |
| 💎  | Extension-to-app mapping        | ✅ Win32 HKCR; Shell `ShellExecuteEx`; prog_id   | ✅ XDG MIME types (`xdg-open`, `mimeapps.list`);                  | ⬜ §1 -- `RegGetValue(HKCR, ...)` prog_id chain; `icon_for_extension()` |
| 💎  | First-boot default associations | ✅ Windows ships with all default                | ✅ Distro ships `mimeapps.list`; `update-mime-database` on        | ⬜ §2 -- `FileAssocsInit` guard flag; 12-entry static                   |
| 💎  | Open With dialog                | ✅ "Open with" dialog; "Always use               | ✅ GNOME/KDE "Open With" dialog; writes                           | ⬜ §3 -- `CTRL_LISTVIEW` HKCR app scan; "Browse…"                       |
| ⭐  | Shortcut files                  | ✅ Binary Shell Link format (.lnk);              | ✅ XDG `.desktop` files (INI-like); `xdg-open`                    | ⬜ §4 -- `⭐` plain-text INI `.lnk` (human-readable                     |
| 💎  | Recycle Bin                     | ✅ Recycle Bin; Restore; auto-purge; right-click | ✅ GNOME/KDE Trash (`~/.local/share/Trash`); `trash-cli`; restore | ⬜ §6 -- `C:\Recycle\`; `.meta` sidecars; MaxSize from                  |
| 💎  | System sounds                   | ✅ Sound schemes; WinMM `PlaySound`; per-event   | ✅ PulseAudio/PipeWire; sound themes; `gsettings` per-event;      | ⬜ §7 -- 44-byte WAV parser; `audio_write_pcm()` stub                   |
| 💎  | Font manager                    | ✅ Settings → Personalization → Fonts;           | ✅ GNOME Font Viewer; KDE Font                                    | ⬜ §8 -- 640×480 px app; 4-size preview                                 |

> **After §1–§8:** Impossible OS has a complete file association and resource stack. The `⭐` differentiator is the human-readable INI `.lnk` shortcut format -- unlike Windows' binary COM Shell Link structure or Linux's XDG `.desktop` (which is similar but XDG-namespaced), Impossible OS `.lnk` files can be created and edited in any text editor, lowering the barrier for system administration and making the format durable across versions.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `file_assoc_open("C:\\Users\\Default\\Documents\\test.txt")` → `notepad.exe` launches with `test.txt` as arg; serial log `[assoc] open: test.txt via notepad.exe`
- [ ] `file_assoc_has(".png")` → 1 after first-boot defaults; `file_assoc_has(".unknown_ext")` → 0 → Open With dialog appears
- [ ] Open With dialog lists notepad.exe + imgview.exe; select imgview; check "Always use"; OK → `file_assoc_has(".unknown_ext")` now 1
- [ ] Create `test.lnk` with `Target=C:\\Impossible\\System32\\notepad.exe`; `shortcut_execute("test.lnk")` → notepad launches
- [ ] Desktop shows `.lnk` files with arrow overlay in lower-left corner of icon
- [ ] `trash_delete("C:\\Users\\Default\\Documents\\test.txt")` → file in `C:\Recycle\`; trash icon switches to ICON_TRASH_FULL; `trash_count()` → 1; right-click → "Empty Recycle Bin" → confirm → `trash_count()` → 0; icon → ICON_TRASH_EMPTY
- [ ] `system_sound_play(SOUND_ERROR)` → serial log `[sound] play: error.wav (N samples) -- audio not ready` (stub path before audio driver live)
- [ ] Font manager: launches via double-click on `.ttf` or from Start Menu; lists fonts in `C:\Impossible\Fonts\`; preview pane shows 4-size text; Install button copies a new TTF and refreshes list
- [ ] Commit: `"file-assoc+shortcuts+resources: extension mapping, .lnk, trash, sounds, fontmgr complete"`
