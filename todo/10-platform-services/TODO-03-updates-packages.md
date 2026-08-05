---
schema_version: 1
id: updates-packages
domain: 10-platform-services
status: active
title: "TODO-03 -- System Updates & IPKG Package Manager"
---

# TODO-03 -- System Updates & IPKG Package Manager

> **Goal:** Make Impossible OS self-maintaining -- deliver the update check/download/verify/apply pipeline and the IPKG package format with installer, uninstaller, and `appwiz.cpl` list. After this TODO the OS can update itself and users can install third-party apps without manual file copying.

> [!IMPORTANT]
> **Already exists**: `zip_open/extract/list/add_file()` from TODO-04-recycle-zip-scheduler (forward dep). `cng_sha256(data, len, out)` from TODO-07-cng-crypto (forward dep). `http_get(url, buf, size)` + HTTPS from `06-networking/TODO-03` (forward dep). `registry_set/get/delete()` + `HKLM\SOFTWARE\*`, `HKLM\SYSTEM\Version`. `shortcut_create()` from TODO-02-file-associations (forward dep). `file_assoc_set()` from TODO-02 (forward dep). `notify_send()` from TODO-09. `privilege_request()` UAC from TODO-06-security-accounts (install requires admin elevation). `CTRL_LISTVIEW`, `CTRL_PROGRESSBAR`, `dialog_confirm()` from TODO-05 controls. **Missing**: entire update pipeline, IPKG format, installer/uninstaller wizards. **No new syscalls needed** -- all I/O goes through existing VFS + network + ZIP + crypto APIs.

## Inputs

- `include/kernel/net/http.h` (TODO-06-net §3) -- `http_get(url, buf, max, &len)`, `http_get_to_file(url, path, progress_cb)` -- §1 version check, §2 download
- `include/cng.h` (TODO-07-cng §1) -- `cng_sha256(data, len, out32)` -- §2 integrity verification
- `include/kernel/zip.h` (TODO-04 §6) -- `zip_open/extract/list/close()` -- §3 update apply, §8 app install
- `include/registry.h` -- `HKLM\SYSTEM\Version`, `HKLM\SOFTWARE\{name}\*`, `HKLM\SYSTEM\Update\*` -- §1 version compare, §6 install manifest, §8 app list
- `include/kernel/fs/vfs.h` -- `vfs_mkdir/rename/unlink/stat()` -- §3 file replace, §6 install, §7 uninstall
- `include/desktop/shortcut.h` (TODO-02 §4) -- `shortcut_create(path, target, icon)` -- §6 Start Menu + Desktop shortcuts
- `include/desktop/file_assoc.h` (TODO-02 §1) -- `file_assoc_set(ext, prog_id, app_path)` -- §6 `.ipkg` association + app assoc
- `include/desktop/controls.h` (TODO-05) -- `CTRL_LISTVIEW`, `CTRL_PROGRESSBAR`, `CTRL_WIZARDPAGE`, `dialog_confirm()` -- §3 wuapp, §6 wizard, §8 appwiz
- `include/cpl.h` (TODO-11) -- `CPlApplet_t`, `NEWCPLINFO` -- §8 `wuapp.cpl`, §4 `appwiz.cpl`
- `include/kernel/sched/task.h` -- `sched_task_add()` -- §4 boot auto-check background task
- `include/desktop/notification.h` (TODO-09) -- `notify_send()` -- §5 "Update available" toast
- → XREF: `06-networking/TODO-03` -- HTTPS client; §1 + §9 depend on HTTP GET being available
- → XREF: `09-desktop-shell/TODO-07` -- `cng_sha256()`; §2 verify depends on crypto module
- → XREF: `09-desktop-shell/TODO-04 §6` -- ZIP/IPKG extract; §3 and §8 depend on `zip_extract()`
- → XREF: `10-platform-services/TODO-04 §2` -- system restore point; §3 update-apply and §8 app install call `restore_create()` before making changes
- → XREF: `09-desktop-shell/TODO-11 §5` -- `appwiz.cpl` stub registered in Control Panel; §4 implements it

## Outcome

- `update_check/download/verify/apply()` kernel API; SHA-256 integrity gate; restore point before apply.
- `wuapp.cpl` in Control Panel: check now, auto-check toggle + channel, update history.
- IPKG format: `.ipkg` = ZIP with `manifest.ini` + `install.ini` + `files/`.
- App installer wizard: welcome/path/progress/finish; file extract, registry, shortcuts, file assoc.
- App uninstaller: reverse install with confirmation; clean empty dirs.
- `appwiz.cpl`: installed app list, version/size/date, search, [Uninstall] per app.
- `ipkg_create` host tool: pack directory → `.ipkg`; validates manifest; `gcc`-compiled.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                         | Depends On                                                                              | Status |
| --- | :---: | --------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- | :----: |
| ⭐  |   1   | §5 IPKG format spec -- `manifest.ini` + `install.ini` + `files/`; parser `ipkg_parse()`             | `zip_open/list()` (TODO-04); `registry_set()` (exists)                                |  [ ]   |
| ⭐  |   2   | §9 IPKG build tool -- `tools/ipkg_create.c`; `gcc`-compiled; pack dir → `.ipkg`; manifest validate | §5 format spec; host `gcc`; `libzip` or miniz host build                               |  [ ]   |
| 💎  |   3   | §1 Update check API -- `struct update_info`, `update_check()`, HTTP GET, Registry version compare   | `http_get()` (TODO-06-net §3); `HKLM\SYSTEM\Version`                                  |  [ ]   |
| 💎  |   4   | §2 Update download & verify -- `update_download()` with progress; `update_verify()` SHA-256 gate   | §1; `cng_sha256()` (TODO-07); `http_get_to_file()`                                    |  [ ]   |
| 💎  |   5   | §3 Update apply -- restore point, ZIP extract, file replace, version bump, restart prompt           | §2; `zip_extract()` (TODO-04); `restore_create()` (TODO-04 §2)                        |  [ ]   |
| 💎  |   6   | §4 `wuapp.cpl` -- check/download/apply UI, auto-check boot task, update history, toast             | §1-3; `CTRL_PROGRESSBAR` (TODO-05); `notify_send()` (TODO-09); `sched_task_add()`    |  [ ]   |
| 💎  |   7   | §6 App installer wizard -- IPKG parse, file extract, registry, shortcuts, file assoc, UAC           | §5; `zip_extract()` (TODO-04); `shortcut_create()` (TODO-02); `privilege_request()`  |  [ ]   |
| 💎  |   8   | §7 App uninstaller -- reverse install, confirmation dialog, empty dir cleanup                       | §6 install metadata in registry; `dialog_confirm()`                                    |  [ ]   |
| 💎  |   9   | §8 `appwiz.cpl` -- `CTRL_LISTVIEW` installed app list, search/filter, [Uninstall] button           | §7; `HKLM\SOFTWARE\*` keys; `CTRL_LISTVIEW` (TODO-05); `include/cpl.h` (TODO-11)     |  [ ]   |

---

## 1. Update Check API `[Sonnet]`

`struct update_info` (version[32], url[256], sha256_hash[65], size, type HOTFIX/MINOR/MAJOR, description[256]). `update_check(info)` HTTP GET to `https://impossible-os.dev/api/version`, parse INI/JSON response, compare with `HKLM\SYSTEM\Version`. Returns `UPDATE_AVAILABLE / UPDATE_CURRENT / UPDATE_ERROR`.

**Files:** `src/kernel/update.c` (new), `include/kernel/update.h` (new)

> [!NOTE]
> `HKLM\SYSTEM\Version` stores current version as string `"1.0.0"` (set at build time in kernel init). `update_check(struct update_info *out)`: `http_get("https://impossible-os.dev/api/version", buf, 1024, &len)` → parse response: expected INI format `[Version]\nLatest=1.0.1\nURL=https://...\nSHA256=abc...\nSize=102400\nType=HOTFIX\nDesc=...`. Compare: `strcmp(latest_ver, current_ver)` -- if equal: `UPDATE_CURRENT`; else: fill `out` → `UPDATE_AVAILABLE`. On HTTP error or parse fail: `klog_warn("update_check: %s", err)` → `UPDATE_ERROR`. Version string comparison: split on `.`; compare major/minor/patch as integers. Timeout: `http_get_timeout = 10 s` to avoid boot hang if network is down. Cache last result in `HKLM\SYSTEM\Update\LastCheck` (timestamp) + `LastResult` (0/1/2).

- [ ] `include/kernel/update.h`: `struct update_info`, `UPDATE_AVAILABLE/CURRENT/ERROR` enum, `update_check/download/verify/apply()` prototypes
- [ ] `src/kernel/update.c`: `update_check(struct update_info *out)` -- `http_get()` + INI parse + version compare
- [ ] Version string comparison: split `"1.0.0"` → `{1, 0, 0}`; compare element-wise
- [ ] Cache result: `registry_set("HKLM\\SYSTEM\\Update\\LastCheck", ...)` timestamp; `LastResult` 0/1/2
- [ ] `update_check()` returns `UPDATE_ERROR` on timeout (10 s), network failure, or parse error; `klog_warn()`
- [ ] Commit: `"kernel: update_check -- http_get version endpoint, INI parse, registry version compare"`

## 2. Update Download & Verification `[Sonnet]`

`update_download(info, dst_path)`: HTTP GET to `info->url` → `C:\Temp\update.ipkg`; progress callback `(bytes, total)`. `update_verify(path, expected_sha256)`: compute `cng_sha256()` over full file; compare; return `0` on match, `-1` on mismatch.

**Files:** extend `src/kernel/update.c`

> [!NOTE]
> `update_download(const struct update_info *info, const char *dst_path, void (*progress_cb)(uint64_t, uint64_t))`: call `http_get_to_file(info->url, dst_path, progress_cb)` -- streams HTTP response directly to VFS file in 64 KiB chunks via PMM buffer; calls `progress_cb(bytes_so_far, info->size)` on each chunk. `update_verify(const char *path, const char *expected_hex)`: VFS read full file (if > 4 KiB → `pmm_alloc_contiguous(ceil(size/4096))`); `cng_sha256(data, size, hash32)`; convert `hash32[32]` to 64-char hex; `strcmp(hex, expected_hex)` (constant-time compare via `crypto_verify32()` from monocypher). On mismatch: `klog_err("update_verify: hash mismatch: expected=%s got=%s", ...)` → `vfs_unlink(dst_path)` (delete corrupt file). PMM free after verify.

- [ ] `update_download(info, dst_path, progress_cb)` -- `http_get_to_file()` streaming; PMM 64 KiB chunk buffer
- [ ] `progress_cb(uint64_t bytes, uint64_t total)` -- called every chunk; `wuapp.cpl` passes a `CTRL_PROGRESSBAR` update fn
- [ ] `update_verify(path, expected_hex)` -- VFS read; `cng_sha256()`; hex encode; `crypto_verify32()` compare
- [ ] On verify mismatch: `klog_err()` + `vfs_unlink(dst_path)` + return `UPDATE_ERR_HASH`
- [ ] PMM alloc/free for large files in verify; `kmalloc` for < 4 KiB (unlikely for update packages)
- [ ] Commit: `"kernel: update_download/verify -- http_get_to_file, cng_sha256, hash compare, corrupt delete"`

## 3. Update Application `[Sonnet]`

`update_apply(path)`: create restore point (`restore_create()`), extract `.ipkg` via `zip_extract()` to staging dir, replace files in `C:\Impossible\System\`, update `HKLM\SYSTEM\Version` + `LastUpdate`. Prompt restart for MAJOR/kernel updates.

**Files:** extend `src/kernel/update.c`

> [!NOTE]
> **Restore point first**: `restore_create("Pre-update %s", info.version)` from TODO-04 §2 -- snapshot `C:\Impossible\System\` file list + registry hive before overwriting anything. **Extract**: `zip_extract(path, "C:\\Temp\\update_stage\\")` → `files/` directory appears. **Replace files**: iterate `install.ini` `[Files]` section (`SrcFile=DestPath`); for each: `vfs_rename(dest, dest_bak)` (backup old); `vfs_rename(src, dest)`. On any VFS error: abort + log + preserve backups. **HOTFIX** (< 100 KB single file): no restart needed; `klog_info()` "Update applied: %s". **MINOR** (1–5 MB, driver/service): ask "Restart to complete update? [Restart] [Later]". **MAJOR** (kernel `kernel.exe`): mandatory restart -- write `HKLM\SYSTEM\Update\PendingRestart = 1`; dialog "A restart is required. [Restart Now]" (cannot defer). **Version bump**: `registry_set("HKLM\\SYSTEM\\Version", info.version)` + `LastUpdate` timestamp. **Cleanup**: `vfs_unlink(path)` (delete `.ipkg`); `rmdir("C:\\Temp\\update_stage\\")`.

- [ ] `update_apply(const char *ipkg_path, const struct update_info *info)` -- orchestrate: restore → extract → replace → bump → restart prompt
- [ ] `restore_create("Pre-update %s", info->version)` -- call TODO-04 §2 restore point API
- [ ] `zip_extract(ipkg_path, "C:\\Temp\\update_stage\\")` → staging
- [ ] Iterate `install.ini [Files]` → `vfs_rename(old, old.bak)` + `vfs_rename(new, dest)` for each file
- [ ] On any error: `klog_err()` + partial-replace rollback (restore `.bak` → orig); return `UPDATE_ERR_APPLY`
- [ ] HOTFIX: no restart; MINOR: `dialog_confirm()` restart prompt; MAJOR: mandatory restart dialog
- [ ] `registry_set("HKLM\\SYSTEM\\Version", info->version)` + `registry_set("HKLM\\SYSTEM\\Update\\LastUpdate", timestamp)`
- [ ] Cleanup: `vfs_unlink(ipkg_path)` + clean staging dir
- [ ] **A/B-aware (TODO-21):** once A/B slots exist, a MAJOR/kernel update installs to the INACTIVE slot via the §1 metadata API, never in-place -> XREF: [`01-boot-platform/TODO-21`](../01-boot-platform/TODO-21-ab-boot-rollback.md)
- [ ] Commit: `"kernel: update_apply -- restore point, ipkg extract, file replace, version bump, restart prompt"`

## 4. `wuapp.cpl` -- Windows Update Applet `[Sonnet]`

Control Panel Windows Update applet: "Check for updates" button, auto-check toggle + channel (daily/weekly/never), last check timestamp, update history list. Boot background task → toast if update available.

**Files:** `src/apps/control/applets/wuapp.c` (new)

> [!NOTE]
> `CPlApplet()` inner surface (380×320 px). **Status area** (top): "Your OS is up to date." or "Update available: v{version} ({type})" with accent-color type badge. **"Check for updates" button**: calls `update_check()` in background via `task_create("wuapp_check", ...)` → refresh status on completion. **While checking**: spinner animation (rotating arc via `gfx_draw_arc()` or CSS-style). **Download/Apply flow if AVAILABLE**: "Download and install" button → `update_download()` with inline `CTRL_PROGRESSBAR`; then [Install]; on MAJOR: mandatory restart button. **Auto-check settings**: `CTRL_DROPDOWN` (Daily/Weekly/Never) → `registry_set("HKLM\\SYSTEM\\Update\\AutoCheck", ...)`. **Channel**: `CTRL_DROPDOWN` (Stable/Beta) → `registry_set("HKLM\\SYSTEM\\Update\\Channel", ...)`. **Update history**: `CTRL_LISTVIEW` (Version, Date, Type, Status) from `HKLM\SYSTEM\Update\History\*` subkeys. **Boot auto-check**: `sched_task_add("update_autocheck", update_autocheck_task, CHECK_INTERVAL_S, 1)` in kernel init (reads `AutoCheck` Registry); `update_autocheck_task()`: `update_check()` → if `UPDATE_AVAILABLE`: `notify_send("System Update", "v{version} is available", ICON_UPDATE, ...)`.

- [ ] `src/apps/control/applets/wuapp.c` implementing `CPlApplet()` `CPL_INIT/INQUIRE/DBLCLK/STOP`
- [ ] Status area: "Up to date" or "Update available: v{} ({type})"
- [ ] "Check for updates" → background `task_create()` → refresh on result
- [ ] In-progress spinner; `CTRL_PROGRESSBAR` during download
- [ ] "Download and install" flow: §2 download + §3 apply in sequence; restart prompt
- [ ] Auto-check `CTRL_DROPDOWN` (Daily/Weekly/Never) + Channel `CTRL_DROPDOWN` → Registry
- [ ] `CTRL_LISTVIEW` update history from `HKLM\SYSTEM\Update\History\*`
- [ ] Boot auto-check: `sched_task_add()` at kernel init reading `AutoCheck` interval; toast on available
- [ ] Write history entry to `HKLM\SYSTEM\Update\History\{timestamp}\{Version,Type,Status}` on each check/apply
- [ ] Commit: `"wuapp.cpl: update UI -- check, download/apply progress, auto-check, history list, boot toast"`

## 5. IPKG Package Format `[Sonnet]`

`.ipkg` = ZIP archive containing `manifest.ini` (app metadata), `install.ini` (file destinations, registry, shortcuts, associations), `files/` tree. `ipkg_parse(path, manifest, install)` extracts and parses both INI files.

**Files:** `src/kernel/ipkg.c` (new), `include/kernel/ipkg.h` (new)

> [!NOTE]
> **`manifest.ini`** (required):
> ```
> [Package]
> Name=MyApp
> Version=1.0.0
> Author=Developer
> Icon=files/myapp.ico
> Description=A great application
> InstallPath=C:\Program Files\MyApp
> StartMenu=1
> Desktop=0
> ```
> **`install.ini`** (required):
> ```
> [Files]
> files/myapp.exe=C:\Program Files\MyApp\myapp.exe
> files/readme.txt=C:\Program Files\MyApp\readme.txt
>
> [Registry]
> HKLM\SOFTWARE\MyApp\Version=1.0.0
> HKLM\SOFTWARE\MyApp\InstallPath=C:\Program Files\MyApp
>
> [Shortcuts]
> Start=myapp.exe
> Desktop=myapp.exe
>
> [Associations]
> .mydata=MyApp.Document=C:\Program Files\MyApp\myapp.exe
> ```
> `ipkg_parse(path, manifest, install)`: `zip_open()` → `zip_extract_file("manifest.ini")` + `zip_extract_file("install.ini")` to tmp buffers → parse via `ini_parse()` (existing or simple key=value scanner). Validate required fields; `klog_err()` if missing.

- [ ] `include/kernel/ipkg.h`: `struct ipkg_manifest { name[64], version[32], author[64], icon_path[128], description[256], install_path[128]; int start_menu, desktop; }`, `struct ipkg_install { file_map[64][2][128], reg_entries[32][2][256], shortcuts[4][64], assoc[16][3][128]; int file_count, reg_count, shortcut_count, assoc_count; }`
- [ ] `int ipkg_parse(const char *ipkg_path, struct ipkg_manifest *m, struct ipkg_install *i)` -- `zip_open()` → extract + parse both INI files
- [ ] Simple INI parser: `ini_parse(buf, section_cb, kv_cb)` for flat key=value sections; or reuse Registry INI loader if available
- [ ] Validate required fields: Name, Version, InstallPath; return error if missing
- [ ] `ipkg_get_file_list(ipkg_path, entries, max)` → list `files/` entries for pre-install display
- [ ] File association entry format: `ext=progid=app_path` parsed into 3 components
- [ ] Commit: `"ipkg: package format -- manifest.ini + install.ini parser, ipkg_parse(), field validation"`

## 6. App Installer Wizard `[Sonnet]`

`src/apps/installer/installer.c`: wizard UI (welcome → path → progress → finish). IPKG parse → files extract → registry entries → shortcuts → file associations. Restore point before install. Requires admin (`privilege_request()`).

**Files:** `src/apps/installer/installer.c` (new), `include/apps/installer.h` (new)

> [!NOTE]
> Window: 520×400 px fixed. **Page 1 -- Welcome**: app icon (48×48, from `manifest.icon_path` extracted to temp); name (24 px bold); version; description; author; [Next] / [Cancel]. **Page 2 -- Install Path**: `CTRL_TEXTBOX` pre-filled with `manifest.install_path`; [Browse] → `dialog_file_open(dir_mode)`. **Page 3 -- Progress**: `CTRL_PROGRESSBAR(0, total_files)`; file-by-file extract: `zip_extract_file(src, dst)` per `install.ini [Files]` entry; increment progress. Then: registry entries from `[Registry]`; shortcuts via `shortcut_create()`; file assoc via `file_assoc_set()`. **Page 4 -- Finish**: "Installation complete!" or error summary; [Finish] / [Launch]. **Pre-install**: `privilege_request()` → UAC consent; `restore_create("Pre-install %s %s", name, version)`. **Registry on install**: `HKLM\SOFTWARE\{name}\Version`, `InstallPath`, `UninstallCmd = "installer.exe /uninstall {name}"`, `InstallDate` (timestamp), `EstimatedSize` (sum of file sizes). **File association**: `.ipkg` → `installer.exe` registered at app launch.

- [ ] `void installer_open(const char *ipkg_path)` -- `wm_create_window()`; parse IPKG; show Page 1
- [ ] Page 1: icon blit + name/version/desc labels; [Next]/[Cancel]
- [ ] Page 2: `CTRL_TEXTBOX` install path; [Browse] → `dialog_file_open(dir)` → fill textbox
- [ ] Page 3: `CTRL_PROGRESSBAR`; per-file extract loop; registry write; shortcut create; file assoc register
- [ ] Page 4: "Complete" status + optional [Launch] → `file_assoc_open(main_exe_path)`
- [ ] `privilege_request()` before install; abort if denied
- [ ] `restore_create()` before modifying anything
- [ ] Registry: write `HKLM\SOFTWARE\{name}\{Version,InstallPath,UninstallCmd,InstallDate,EstimatedSize}`
- [ ] Register `.ipkg` → `installer.exe` in file assoc (add to TODO-02 §1 default assoc list)
- [ ] Commit: `"installer: IPKG app installer wizard -- welcome/path/progress/finish, registry, shortcuts, UAC"`

## 7. App Uninstaller `[Sonnet]`

Read `HKLM\SOFTWARE\{name}\InstallPath`, reverse install: delete files, remove registry entries, delete shortcuts, clean empty dirs. Confirmation dialog "Uninstall {Name}? This will remove the application."

**Files:** extend `src/apps/installer/installer.c`

> [!NOTE]
> `void uninstaller_run(const char *app_name)`: read `HKLM\SOFTWARE\{name}\InstallPath` + `UninstallCmd`. `dialog_confirm("Uninstall {name}?", "This will remove the application and all its files.", YES/NO)` → if NO: return. Steps: (1) delete all files in `install_path` (re-parse `install.ini` from remaining install dir if available, else scan `InstallPath` dir with `vfs_readdir()`); (2) `registry_delete("HKLM\\SOFTWARE\\{name}")` subtree; (3) delete Start Menu shortcut + Desktop shortcut if present; (4) `vfs_rmdir_empty(install_path)` -- remove dir only if empty (do not delete dirs with user files). (5) clean file associations from `HKCR\{ext}` if registered by this app. Notify: `notify_send("{name} has been uninstalled.", ...)`. If `appwiz.cpl` is open: refresh its list.

- [ ] `void uninstaller_run(const char *app_name)` -- confirmation dialog → delete files → reg subtree delete → shortcut delete → empty dir cleanup → file assoc cleanup
- [ ] Re-parse `install.ini` from `install_path\install.ini` (if preserved during install) for precise file list
- [ ] Fallback: `vfs_readdir(install_path)` scan if `install.ini` missing
- [ ] `registry_delete_tree("HKLM\\SOFTWARE\\{name}")` -- delete all subkeys recursively
- [ ] Shortcut delete: `vfs_unlink(start_menu_path)` + `vfs_unlink(desktop_path)` if exist
- [ ] `vfs_rmdir_empty(dir)` -- only if empty; preserve dirs with user-created files
- [ ] File assoc cleanup: if `HKCR\{ext}` points to this app → delete assoc
- [ ] `notify_send("{name} has been uninstalled.", ...)` on success
- [ ] Commit: `"installer: app uninstaller -- file delete, registry cleanup, shortcut remove, empty dir prune"`

## 8. `appwiz.cpl` -- Programs & Features Applet `[Sonnet]`

List all installed apps from `HKLM\SOFTWARE\*`: Name, Version, Size, Install Date. Search/filter. [Uninstall] per row → `uninstaller_run()`. Part of Control Panel. Stub registered in TODO-11 §5.

**Files:** `src/apps/control/applets/appwiz.c` (implement the stub from TODO-11 §9)

> [!NOTE]
> `CTRL_LISTVIEW` (4 columns: Name 180 / Version 80 / Size 70 / Install Date 100 px); sortable. Populate: enumerate `HKLM\SOFTWARE\*` subkeys; for each: read `Version`, `EstimatedSize`, `InstallDate`; filter out system keys (Impossible, Classes, etc.) by checking for `InstallPath` presence. **Search bar**: `CTRL_TEXTBOX` at top; on type: filter `CTRL_LISTVIEW` rows to name substring match. **[Uninstall] button**: enabled when row selected; click → `uninstaller_run(selected_name)` → refresh list. **App icon**: read 16×16 icon for each row from `HKLM\SOFTWARE\{name}\Icon` path (if set); fall back to default app icon. **Double-click row**: show app properties dialog (name, version, publisher, install path, install date, size, description).

- [ ] `appwiz.c` implementing `CPlApplet()` `CPL_INIT/INQUIRE/DBLCLK/STOP`
- [ ] `CTRL_LISTVIEW` 4-column list; populated from `HKLM\SOFTWARE\*` subkeys with `InstallPath` filter
- [ ] Sort by column click (Name/Version/Size/Date)
- [ ] Search `CTRL_TEXTBOX` at top; substring filter on Name column
- [ ] [Uninstall] button: calls `uninstaller_run(name)` → `appwiz_refresh()` on return
- [ ] Double-click → properties dialog: labels for all Registry fields
- [ ] 16×16 icon per row from `Icon` Registry value or default
- [ ] Commit: `"appwiz.cpl: programs & features -- installed app list, search, uninstall button"`

## 9. IPKG Build Tool (Host) `[Sonnet]`

`tools/ipkg_create.c`: host-side `gcc`-compiled tool. Packs a directory + manifest into `.ipkg` (ZIP). Validates required fields. `--name --version --dir --output` args. Runs on build host (Linux/Windows).

**Files:** `tools/ipkg_create.c` (new)

> [!NOTE]
> Compiled with `gcc tools/ipkg_create.c -o tools/ipkg_create -lz` (or vendor miniz as single-file). Usage: `./ipkg_create --name "MyApp" --version "1.0.0" --author "Dev" --desc "My app" --dir ./dist/ --install-path "C:\Program Files\MyApp" --output myapp.ipkg`. **Steps**: (1) validate `--dir` contains at least one file; (2) auto-generate `manifest.ini` from args + auto-detect main `.exe` for icon; (3) auto-generate `install.ini [Files]` by scanning `--dir` recursively; (4) `zip_create(output)`; `zip_add_str("manifest.ini", manifest_buf)`; `zip_add_str("install.ini", install_buf)`; `zip_add_dir("files/", dir_path)` (recurse); `zip_close()`. **Validate**: error if `Name` is empty; `Version` matches `[0-9]+\.[0-9]+\.[0-9]+`; `InstallPath` starts with a drive letter. **`--registry`** option: `--registry "HKLM\SOFTWARE\MyApp\Key=Value"` appends to `[Registry]` section. **`--assoc`** option: `--assoc ".mydata=MyApp.Document"`. Print manifest summary on success; exit 0.

- [ ] `tools/ipkg_create.c`: parse CLI args (`--name`, `--version`, `--author`, `--desc`, `--dir`, `--install-path`, `--output`, `--registry`, `--assoc`)
- [ ] Validate required fields; print error + exit 1 on failure
- [ ] Auto-generate `manifest.ini` string; auto-generate `install.ini` from `--dir` scan
- [ ] Vendor miniz as single-file (`tools/miniz.h`) or link `-lz`; create ZIP; add `manifest.ini`, `install.ini`, `files/` tree
- [ ] Add to `Makefile` `tools` target: `gcc tools/ipkg_create.c tools/miniz.c -o tools/ipkg_create`
- [ ] Test: `./ipkg_create --name "hello" --version "1.0.0" --dir ./user/hello/ --output hello.ipkg` → valid ZIP; open in archiver shows `manifest.ini` + `install.ini` + `files/hello.exe`
- [ ] Commit: `"tools: ipkg_create -- host-side IPKG builder, manifest/install auto-gen, zip pack"`

---

## OS Comparison


| ⭐  | Feature                                             | 🪟 Win11                                         | 🐧 Linux                                             | 🚀 Impossible OS                                                      |
| --- | --------------------------------------------------- | ------------------------------------------------ | ---------------------------------------------------- | --------------------------------------------------------------------- |
| 💎  | Update check                                        | ✅ Windows Update: HTTPS WSUS/WU endpoint;       | ✅ `apt check`; `dnf check-update`; `pacman          | ⬜ §1 -- `http_get()` INI endpoint; Registry version                  |
| 💎  | Update download + SHA-256 verification before apply | ✅ Windows Update: SHA-256 + code-signed         | ✅ `apt/dnf`: GPG-signed package files +             | ⬜ §2 -- `cng_sha256()` + `crypto_verify32()` constant-time compare   |
| ⭐  | Update apply                                        | ✅ Windows Update: cabinet/WIM staging; System   | ✅ `apt/dpkg`: pre-inst/post-inst scripts; no atomic | ⬜ §3 -- `⭐` explicit restore point before                           |
| ⭐  | IPKG package format                                 | ✅ MSI: COM-based installer database; complex    | ✅ `.deb`/`.rpm`: binary control data +              | ⬜ §5 -- `⭐` INI text manifest +                                     |
| 💎  | App installer wizard                                | ✅ NSIS/Inno/WiX/MSI installers: full wizard UI; | ✅ GUI: Discover/GNOME Software/Pamac; CLI: `apt     | ⬜ §6 -- 4-page wizard; `privilege_request()` UAC; `restore_create()` |
| 💎  | App uninstaller                                     | ✅ Programs & Features / Settings                | ✅ `apt remove`/`dnf remove`; `purge` for            | ⬜ §7 -- re-parse `install.ini` for precise file                      |
| 💎  | Programs & Features (`appwiz.cpl`)                  | ✅ Settings → Apps: list +                       | ✅ GNOME Software; Pamac; Muon; `dpkg                | ⬜ §8 -- `CTRL_LISTVIEW` from `HKLM\SOFTWARE\*`; search filter        |
| ⭐  | `ipkg_create` host build tool                       | ✅ WiX Toolset / NSIS /                          | ✅ `dpkg-deb`, `rpmbuild`, `makepkg`: complex spec   | ⬜ §9 -- `⭐` single `gcc`-compiled tool; 6                           |

> **After §1–§9:** Impossible OS has a complete self-update + app ecosystem. The `⭐` advantages: IPKG uses human-readable INI manifests inside a plain ZIP (trivially inspectable with any archive tool, unlike MSI's COM database or `.deb`'s binary control); the update pipeline requires an explicit restore point before every file replacement (rollback is always possible); and `ipkg_create` is a single-source host tool vs the hundreds-of-lines spec files required by `rpmbuild` or WiX.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `update_check()` with mock HTTP server → parses response → returns `UPDATE_AVAILABLE` with correct version/hash/url
- [ ] `update_verify("test.ipkg", correct_sha256)` → 0 (match); wrong hash → returns error + file deleted
- [ ] Create `.ipkg` with `ipkg_create --name "test" --version "1.0.0" --dir ./test_app/ --output test.ipkg` → valid ZIP; `manifest.ini` + `install.ini` present
- [ ] Open installer wizard → Page 1 shows name/version/desc; Page 2 path editable; Page 3 progress bar fills; Page 4 "Complete"
- [ ] After install: `HKLM\SOFTWARE\test\Version` = "1.0.0"; Start Menu shortcut created; `.ipkg` opens installer
- [ ] `appwiz.cpl` → list shows "test" entry with correct version/size/date; [Uninstall] → confirm → app removed; list refreshed
- [ ] Control Panel → Windows Update → "Check for updates" → spinner → status updated; auto-check dropdown change persists across reboot
- [ ] Commit: `"updates: update pipeline, IPKG format, installer wizard, appwiz.cpl, ipkg_create tool -- complete"`
