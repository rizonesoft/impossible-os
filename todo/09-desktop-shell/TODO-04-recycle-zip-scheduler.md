---
schema_version: 1
id: recycle-zip-scheduler
domain: 09-desktop-shell
status: active
title: "TODO-04 -- Recycle Bin, ZIP & Task Scheduler"
---

# TODO-04 -- Recycle Bin, ZIP & Task Scheduler

> **Goal:** Build three independent infrastructure pieces that unlock production-quality core apps: a fully-featured Recycle Bin (counter-keyed trash with meta sidecars + restore window), a miniz-backed ZIP archive API with shell commands, and a 16-slot task scheduler with PIT tick + built-in recurring tasks and an `at` command.

> [!IMPORTANT]
> **Already exists**: `vfs_unlink(path)`, `vfs_rename(old, new)`, `vfs_stat(path, stat)`, `vfs_readdir`, `vfs_create`, `vfs_read/write` in `vfs.h`. `registry_flush()` in `registry.h`. `kmalloc/kfree` in `heap.h`. `klog(level, subsystem, fmt, ...)` in `klog.h`. `system_get_ticks()` + `PIT_TARGET_FREQ=100` for scheduler timing. `time_now()` (TODO-10 §1) for deletion timestamps. `shortcut_execute()` (TODO-02 §5) for `at` command execution. `dialog_input()` + `CTRL_LISTVIEW` (TODO-05) for recycle bin window app. `context_menu_show()` (TODO-07 §1) for right-click items. **Scope overlap**: §1 Recycle Bin core supersedes the lighter stub described in TODO-02 §6 -- implement only once here; TODO-02 §6 becomes a forward reference to this TODO. **Missing**: `src/libs/` directory, miniz, `trash_*`, `zip_*`, `sched_task*`. Complete sections in order: recycle bin core → bin window app → miniz integration → ZIP API → ZIP shell commands → task scheduler → built-in tasks → `at` command.

## Inputs

- `include/kernel/fs/vfs.h` -- `vfs_unlink`, `vfs_rename`, `vfs_stat`, `vfs_readdir`, `vfs_create`, `vfs_read/write` -- used by §1 trash moves and §4 ZIP extract
- `include/registry.h` -- `registry_flush()`, `RegGetValue/SetValueEx` -- used by §1 MaxSize config and §7 scheduler task configs
- `include/kernel/mm/heap.h` -- `kmalloc/kfree` -- used by §3 miniz memory redirectors
- `include/kernel/drivers/pit.h` -- `system_get_ticks()`, `PIT_TARGET_FREQ` -- used by §6 scheduler tick
- `include/kernel/time.h` (TODO-10 §1) -- `time_now()`, `time_to_datetime()` -- used by §1 deletion timestamp and §9 `at` HH:MM parse
- `include/kernel/klog.h` -- `klog()` -- used throughout for operation logs
- `include/desktop/controls.h` (TODO-05) -- `CTRL_LISTVIEW`, `dialog_input()` -- used by §4 recycle bin window table
- `include/desktop/context_menu.h` (TODO-07 §1) -- `context_menu_show()` -- used by §2 right-click item menu
- `include/desktop/shortcut.h` (TODO-02 §5) -- `shortcut_execute()` -- used by §8 `at` command execution
- → XREF: `09-desktop-shell/TODO-02-file-associations-resources.md §6` -- §1 is the full implementation that replaces the stub; TODO-02 §6 trash icon states wire to `trash_count()` from here
- → XREF: `09-desktop-shell/TODO-03-service-manager.md §9` -- `registryd` and `ntpd` built-in daemons (TODO-03) consume `registry_flush()` and `ntp_sync()`; §8 built-in scheduled tasks are a complementary general-purpose scheduling layer for one-shot and timed callbacks
- → XREF: `06-networking/TODO-06-ntp-status-winsock.md` -- `ntp_sync()` called by §7 scheduled NTP task

## Outcome

- `trash_delete(path)` moves file to counter-keyed slot with INI meta sidecar; `trash_restore/empty/count/size`.
- Recycle Bin window: `CTRL_LISTVIEW` table with restore/delete toolbar; right-click per item.
- miniz compiled freestanding with `kmalloc/kfree` redirectors; inflate/deflate round-trip verified.
- `zip_create/add_file/extract/list` API; `zip`/`unzip` shell commands.
- 16-slot `sched_task_t` table; `sched_task_tick()` per PIT second; add/remove/enable/disable.
- Built-in tasks: NTP sync 3600 s, log rotate 86400 s, search index rebuild 1800 s.
- `at HH:MM command` one-shot scheduler; `at list`; `at cancel`.

## Implementation Order

| ⭐  | Order | Deliverable                                                                               | Depends On                                                                 | Status |
| --- | :---: | ----------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Recycle bin core -- `trash_delete/restore/restore_all/empty/count/size`, meta INI      | `vfs_rename/unlink/stat`, `time_now()` (both exist/planned)               |  [ ]   |
| 💎  |   2   | §2 Recycle bin window -- `CTRL_LISTVIEW` table, toolbar, right-click restore/delete       | §1 core must exist; TODO-05 `CTRL_LISTVIEW`; TODO-07 `context_menu_show` |  [ ]   |
| 💎  |   3   | §3 miniz integration -- vendor at `src/libs/miniz/`, kmalloc redirect, freestanding build | `kmalloc/kfree` (exist); no other deps                                    |  [ ]   |
| 💎  |   4   | §4 ZIP kernel API -- `zip_create/add_file/extract/extract_file/list` over miniz            | §3 miniz must be compiled and linkable                                    |  [ ]   |
| 💎  |   5   | §5 ZIP shell commands -- `zip`, `unzip`, `unzip -l`                                        | §4 ZIP API                                                                 |  [ ]   |
| 💎  |   6   | §6 Task scheduler core -- 16-slot `sched_task_t` table, `sched_task_tick()`, add/remove   | `system_get_ticks()`, `PIT_TARGET_FREQ` (exist); workqueue from TODO-03   |  [ ]   |
| 💎  |   7   | §7 Built-in tasks -- NTP/log-rotate/search-index/registry-flush scheduled entries         | §6 scheduler table; `registry_flush()`, `ntp_sync()` (both exist)        |  [ ]   |
| 💎  |   8   | §8 `at` shell command -- `at HH:MM cmd`, `at list`, `at cancel`                           | §6 scheduler (one-shot entries use `interval=0`); `time_now()` (TODO-10) |  [ ]   |

---

## 1. Recycle Bin Core `[Sonnet]`

`trash_delete(path)`: move to `C:\Recycle\trash_{counter}\`, write `C:\Recycle\_meta\trash_{counter}.meta` INI (`OriginalPath`, `OriginalName`, `DeletedAt`, `Size`). `trash_restore(trash_name)`: read meta → `vfs_rename` back → delete meta. `trash_restore_all()`, `trash_empty()`, `trash_count()`, `trash_size()`. Auto-purge oldest when `MaxSize` exceeded.

**Files:** `src/kernel/trash.c` (new), `include/kernel/trash.h` (new)

> [!NOTE]
> Counter: `static uint32_t g_trash_counter` (persistent: read max existing index from `C:\Recycle\_meta\` on `trash_init()`; increment on each delete). Collision avoidance: slot name = `trash_%08u` with zero-padded 8-digit counter. **Meta INI format** (written via `vfs_write`): `[Trash]\nOriginalPath=C:\\...\nOriginalName=file.txt\nDeletedAt=1234567890\nSize=4096\n`. Meta parse: hand-coded `key=value` line scanner (reuse `shortcut_parse` pattern from TODO-02). `trash_delete()` sequence: `vfs_mkdir("C:\\Recycle\\trash_%08u\\")` → `vfs_rename(src, dst)` → write meta. If rename fails (cross-filesystem): `vfs_copy` + `vfs_unlink`. Auto-purge: after each `trash_delete()`: `trash_size()` = sum of all file sizes via `vfs_stat`; if `> MaxSize`: find oldest `DeletedAt` in meta dir; `vfs_unlink(oldest)` + `vfs_unlink(oldest_meta)`; repeat until under limit.

- [ ] `void trash_init(void)`: `vfs_mkdir("C:\\Recycle\\")` + `vfs_mkdir("C:\\Recycle\\_meta\\")` if absent; scan `_meta/` to find max counter → set `g_trash_counter`
- [ ] `int trash_delete(const char *path)` -- increment counter; mkdir slot; rename/copy; write meta; auto-purge check
- [ ] `int trash_restore(const char *trash_name)` -- find and parse `_meta/{name}.meta`; `vfs_rename(slot_path, original_path)`; `vfs_unlink(meta_path)`
- [ ] `void trash_restore_all(void)` -- iterate `_meta/`; `trash_restore()` each entry
- [ ] `void trash_empty(void)` -- iterate `C:\Recycle\`; `vfs_unlink()` all non-`_meta` dirs + their contents; `vfs_unlink()` all meta files; reset `g_trash_counter`
- [ ] `int trash_count(void)` -- count `.meta` files in `_meta/`
- [ ] `uint64_t trash_size(void)` -- sum `Size` field from each `.meta` file
- [ ] `int trash_get_entries(trash_entry_t *out, int max)` -- parse up to `max` meta files; fill `out[]` for UI
- [ ] `typedef struct { char trash_name[32]; char original_path[256]; char original_name[64]; int64_t deleted_at; uint64_t size; } trash_entry_t;`
- [ ] `trash_init()` called from `kernel_main()` after VFS and Registry are live
- [ ] Commit: `"trash: recycle bin core -- counter-keyed slots, INI meta, restore/empty/count/size, auto-purge"`

## 2. Recycle Bin Window App `[Sonnet]`

File-Manager-like window: `CTRL_LISTVIEW` table (Name, Original Location, Date Deleted, Size). Toolbar: "Empty Recycle Bin" (confirm) + "Restore all". Right-click item → Restore / Delete permanently. Double-click → preview. `Ctrl+A` select all.

**Files:** `src/desktop/recycle_bin_app.c` (new), `include/desktop/recycle_bin_app.h` (new)

> [!NOTE]
> Window: 720×480 px. Toolbar (48 px height): "Empty Recycle Bin" button (shows `dialog_input`-style confirm → `trash_empty()`; updates list); "Restore all" button → `trash_restore_all()`; updates list. `CTRL_LISTVIEW` in details mode (4 columns): Name (200 px), Original Location (220 px), Date Deleted (140 px), Size (80 px). Populate: `trash_get_entries(entries, 512)` → `ctrl_listview_add_row()` for each. Sorting: click column header → sort by that field (name/path/date/size). **Right-click item**: `context_menu_show()`: "Restore" → `trash_restore(entry->trash_name)` + remove row; "Delete Permanently" → `vfs_unlink(slot_path)` + `vfs_unlink(meta_path)` + remove row. **Double-click**: if file is image: `file_assoc_open(original_path_preview)` (open from trash slot path). Ctrl+A: select all rows. Opened by: desktop trash icon double-click (`file_assoc_open("C:\\Recycle\\")` routes here) or Start Menu shortcut.

- [ ] `void recycle_bin_open(void)` -- create 720×480 px window; build toolbar + `CTRL_LISTVIEW`; `trash_get_entries()` to populate
- [ ] `void recycle_bin_refresh(void)` -- `trash_get_entries()`; rebuild listview rows
- [ ] Toolbar: "Empty Recycle Bin" → confirm dialog ("Are you sure? This will permanently delete N items.") → `trash_empty()` → `recycle_bin_refresh()`
- [ ] "Restore all" → `trash_restore_all()` → `recycle_bin_refresh()`
- [ ] Right-click handler: `context_menu_show()` with Restore + Delete Permanently options
- [ ] Date formatting: `time_to_datetime(entry->deleted_at, 0, &dt)` + `time_format(&dt, buf, 16, "%d/%m/%Y %H:%M")`
- [ ] Size formatting: `format_bytes(entry->size, buf)` → "4 KB" / "1.2 MB" etc.
- [ ] Column sort: click header → toggle `g_sort_column`; `qsort`-style sort of `trash_entry_t[]`; rebuild rows
- [ ] `Ctrl+A` key handler: `ctrl_listview_select_all(listview_handle)`
- [ ] Commit: `"recycle_bin: window app -- CTRL_LISTVIEW table, toolbar restore/empty, right-click, date+size format"`

## 3. miniz ZIP Library Integration `[Sonnet]`

Vendor miniz (MIT, ~5 K lines, single-file C) at `src/libs/miniz/miniz.c` + `include/libs/miniz.h`. Redirect `MZ_MALLOC/FREE/REALLOC` to `kmalloc/kfree`. Compile with `-ffreestanding -O2 -nostdinc`. Round-trip inflate/deflate test.

**Files:** `src/libs/miniz/miniz.c` (vendor), `include/libs/miniz.h` (vendor), `Makefile` (extend)

> [!NOTE]
> miniz is available at https://github.com/richgel999/miniz (single MIT-licensed `.c` file, ~5500 lines). Add `src/libs/miniz/` to the Makefile with its own compile rule. **Memory redirectors** at the top of `miniz.c` (before the include guard body): `#define MZ_MALLOC(sz) kmalloc(sz)`, `#define MZ_FREE(p) kfree(p)`, `#define MZ_REALLOC(p, ns) krealloc(p, ns)` -- add `krealloc(ptr, new_size)` to `heap.h` if missing (allocate new + memcpy + free old). **Freestanding guards**: miniz includes `<stdlib.h>` + `<string.h>` -- add `-include include/kernel/libc_shim.h` compile option for miniz only, where `libc_shim.h` provides `memcpy/memset/memcmp/memmove` via kernel equivalents. **Test**: add `src/libs/miniz/miniz_test.c` with `miniz_test()`: compress "Hello miniz world" → decompress → assert match; call from `kernel_main` with `#ifdef DEBUG_MINIZ` guard; log result to serial.

- [ ] Download and vendor `miniz.c` + `miniz.h` into `src/libs/miniz/` (do not modify upstream logic)
- [ ] Prepend memory macro redirectors to top of a `miniz_config.h` included before miniz
- [ ] `krealloc(void *ptr, size_t new_size)` stub in `heap.c/h`: `kmalloc(new_size)` + `memcpy` + `kfree(old)` -- if not already present
- [ ] Add `libc_shim.h` providing `size_t`, `memcpy`, `memset`, `memmove`, `memcmp` → kernel equivalents
- [ ] Makefile: `$(LIBS_DIR)/miniz/miniz.o: ... -include include/kernel/libc_shim.h -ffreestanding -O2`
- [ ] `miniz_test()` in `src/libs/miniz/miniz_test.c`: round-trip test; `klog(LOG_INFO, "miniz", "round-trip: %s", ok ? "PASS" : "FAIL")`
- [ ] Commit: `"libs: vendor miniz -- kmalloc/kfree redirect, freestanding build, round-trip test"`

## 4. ZIP Kernel API `[Sonnet]`

`zip_create(path)`, `zip_add_file(archive, filepath, entry_name)`, `zip_extract(archive, dest_dir)`, `zip_extract_file(archive, entry_name, dest)`, `zip_list(archive, entries[], max)` → count + names + sizes.

**Files:** `src/kernel/zip.c` (new), `include/kernel/zip.h` (new)

> [!NOTE]
> Use miniz's `mz_zip_archive` struct as the backing type. `zip_create(path)`: open file via VFS for writing; `mz_zip_writer_init_file()` equivalent -- since we are not stdio-based, implement VFS-backed I/O callbacks for miniz (`mz_zip_writer_init_heap` for in-memory, then write via `vfs_write`). Simpler path: use miniz in-memory mode -- accumulate archive in a `pmm_alloc_contiguous` buffer; on `zip_close()` write the whole buffer to the file. `zip_add_file(archive, filepath, entry_name)`: `vfs_read(filepath)` into buffer; `mz_zip_writer_add_mem(archive, entry_name, data, size, level=6)`. `zip_extract(archive, dest_dir)`: iterate entries with `mz_zip_reader_get_num_files`; for each: `mz_zip_reader_extract_to_heap` → `vfs_create(dest_path)` + `vfs_write(node, data, size)`. Password-protected archives: check `mz_zip_reader_file_stat.m_is_encrypted`; if set: return `ZIP_ERR_ENCRYPTED` and log "Password-protected ZIP not supported".

- [ ] `typedef struct zip_archive zip_archive_t;` in `zip.h` (opaque wrapper over `mz_zip_archive`)
- [ ] `typedef struct { char name[256]; uint64_t size; uint64_t compressed_size; } zip_entry_t;`
- [ ] `zip_archive_t *zip_create(const char *path)` -- allocate; `mz_zip_writer_init_heap()`
- [ ] `int zip_add_file(zip_archive_t *a, const char *filepath, const char *entry_name)` -- VFS read; `mz_zip_writer_add_mem()`
- [ ] `int zip_close(zip_archive_t *a)` -- `mz_zip_writer_finalize_heap_archive()` → `vfs_write()`; free
- [ ] `zip_archive_t *zip_open(const char *path)` -- VFS read whole file into PMM buffer; `mz_zip_reader_init_mem()`
- [ ] `int zip_extract(zip_archive_t *a, const char *dest_dir)` -- iterate entries; extract each via `mz_zip_reader_extract_to_heap`; `vfs_create(dest_path)` + `vfs_write()`
- [ ] `int zip_extract_file(zip_archive_t *a, const char *entry_name, const char *dest)` -- single entry
- [ ] `int zip_list(zip_archive_t *a, zip_entry_t *out, int max)` -- fill `out[]`; return count
- [ ] `void zip_free(zip_archive_t *a)` -- free PMM buffer; free struct
- [ ] `#define ZIP_ERR_ENCRYPTED -2` in `zip.h`
- [ ] Commit: `"zip: kernel API -- zip_create/add_file/close/open/extract/list over miniz in-memory mode"`

## 5. ZIP Shell Commands `[Sonnet]`

`zip <archive.zip> <files…>` creates archive. `unzip <archive.zip> [dest]` extracts to current dir or `dest`. `unzip -l <archive.zip>` lists contents. Error on password-protected archives.

**Files:** `src/shell/cmds.c` (extend)

> [!NOTE]
> `cmd_zip(argc, argv)`: `zip_create(argv[1])` → for each `argv[2..N]`: `zip_add_file(archive, argv[i], basename(argv[i]))` → `zip_close()`; print "Created {archive} ({N} files, {size} bytes)". `cmd_unzip(argc, argv)`: check for `-l` flag; if present: `zip_open()` → `zip_list()` → print table (name, size, compressed); else: `zip_open()` → `zip_extract(archive, dest_dir)` where `dest_dir = argc>=3 ? argv[2] : "."`. On `ZIP_ERR_ENCRYPTED`: print "Error: Password-protected ZIP not supported". Register both in command dispatch table.

- [ ] `void cmd_zip(int argc, char **argv)` -- create archive from file list; print summary
- [ ] `void cmd_unzip(int argc, char **argv)` -- `-l` flag for list; else extract; encrypted error path
- [ ] Progress: `kprintf("[%u/%u] %s\n", i, count, entry_name)` per file during extract
- [ ] Register `zip` + `unzip` in command table with usage strings
- [ ] Commit: `"shell: zip/unzip commands -- create from files, extract with progress, list, encrypted error"`

## 6. Task Scheduler Core `[Sonnet]`

`struct sched_task` (name[32], callback fn ptr, interval_seconds, next_run Unix ts, enabled, last_result). Static 16-entry table. `sched_task_add/remove/enable/tick`. `sched_task_tick()` hooked into PIT once per second.

**Files:** `src/kernel/scheduler_tasks.c` (new), `include/kernel/scheduler_tasks.h` (new)

> [!NOTE]
> Distinct from the process scheduler (`sched.c`) -- this is a simple wall-clock task scheduler for periodic callbacks. `sched_task_tick()` is called from the PIT workqueue (once per second, same pattern as `svc_monitor_tick()` in TODO-03 §5). It compares `time_now()` against each entry's `next_run`; if elapsed: call `task->callback()`; update `task->next_run = time_now() + task->interval_seconds`; store return value in `task->last_result`. One-shot tasks: `interval_seconds == 0` → set `task->enabled = 0` after run (auto-remove). `sched_task_add(name, cb, interval_s, enabled)`: find free slot; Registry persist to `HKLM\SYSTEM\Scheduler\Tasks\{name}\{Interval,Enabled}`. Slot 0 reserved for internal use. Max 16 entries.

- [ ] `typedef struct { char name[32]; int (*callback)(void); uint32_t interval_seconds; int64_t next_run_unix; uint8_t enabled; int last_result; } sched_task_t;`
- [ ] `sched_task_t g_sched_tasks[SCHED_MAX]` + `#define SCHED_MAX 16` + `spinlock_t g_sched_lock`
- [ ] `int sched_task_add(const char *name, int (*cb)(void), uint32_t interval_s, uint8_t enabled)` -- find free slot; fill; persist to Registry
- [ ] `int sched_task_remove(const char *name)` -- find by name; clear slot; delete Registry key
- [ ] `void sched_task_enable(const char *name, int enabled)` -- find; update flag; persist
- [ ] `void sched_task_tick(void)` -- spinlock; iterate; compare `time_now()` vs `next_run`; call callback; update `next_run`; handle one-shot
- [ ] `sched_task_tick()` registered as workqueue item at `PIT_TARGET_FREQ` ticks (1 Hz) in `sched_init()`
- [ ] `void sched_task_list(sched_task_t *out, int *count)` -- copy all enabled+registered tasks
- [ ] Commit: `"sched: task scheduler -- 16-slot table, sched_task_tick() PIT-hooked, one-shot support, Registry persist"`

## 7. Built-In Scheduled Tasks `[Sonnet]`

NTP sync every 3600 s → `ntp_sync()`. Registry flush every 2 s → `registry_flush()`. Search index rebuild every 1800 s → `search_index_rebuild()`. Log rotate every 86400 s → trim `klog.txt` to last 5000 lines. Task configs in `HKLM\SYSTEM\Scheduler\Tasks\{name}\{Interval,Enabled}`.

**Files:** `src/kernel/scheduler_tasks.c` (extend)

> [!NOTE]
> Note relationship to TODO-03 §9 daemons: `registryd` in TODO-03 is a continuous kernel thread looping every 2 s; §8 here provides an alternative scheduled-callback path for the same job. Prefer registering `registryd` as a task here -- replace the TODO-03 `registryd_thread()` with a `sched_task_add("registryd", registry_flush, 2, 1)` call. This removes a kernel thread and simplifies the design. `search_index_rebuild()` stub: just `klog(LOG_INFO, "indexd", "index rebuild placeholder")` until file search is implemented. Log rotate: `klog_rotate(5000)` -- new function in `klog.c` that reads `klog.txt`, keeps last 5000 lines, rewrites.

- [ ] `void sched_builtins_init(void)` -- call `sched_task_add()` for each built-in task; called from `sched_init()`
- [ ] `sched_task_add("ntp_sync", ntp_sync, 3600, 1)` -- `ntp_sync()` returns 0 on success
- [ ] `sched_task_add("reg_flush", registry_flush_sched, 2, 1)` -- wrapper returns int; replaces `registryd_thread`
- [ ] `sched_task_add("index_rebuild", search_index_rebuild, 1800, 1)` -- `search_index_rebuild()` stub returns 0
- [ ] `sched_task_add("log_rotate", klog_rotate_sched, 86400, 1)` -- `klog_rotate(5000)` returns 0
- [ ] `int klog_rotate(uint32_t keep_lines)` -- VFS open `klog.txt`; count lines; if > `keep_lines`: skip first `N - keep_lines` lines; rewrite; return 0
- [ ] Read `HKLM\SYSTEM\Scheduler\Tasks\{name}\Interval` + `Enabled` at `sched_builtins_init()` to allow admin override
- [ ] `int search_index_rebuild(void)` -- stub; `klog(LOG_INFO, "indexd", "search index rebuild")`; return 0
- [ ] Commit: `"sched: built-in tasks -- ntp_sync/reg_flush/index_rebuild/log_rotate registered, klog_rotate()"`

## 8. `at` Shell Command `[Sonnet]`

`at <HH:MM> <command>` schedules one-shot task (interval=0, auto-remove). `at list` shows pending. `at cancel <id>` removes.

**Files:** `src/shell/cmds.c` (extend)

> [!NOTE]
> `at HH:MM command`: parse `HH:MM` into hour + min; compute next occurrence: `time_to_datetime(time_now(), tz, &dt)` → set `dt.hour=H, dt.min=M, dt.second=0`; if time has passed for today → advance day by 1; `datetime_to_time(&dt)` → `next_run_unix`. Register `sched_task_add("at_{id}", at_exec_wrapper, 0, 1)` with `next_run` = that unix timestamp. `at_exec_wrapper`: calls `shortcut_execute()` or `file_assoc_open()` on the stored command string. `at list`: `sched_task_list()` → filter tasks with `interval_seconds == 0`; print `{id} {HH:MM} {command}`. `at cancel <id>`: `sched_task_remove("at_{id}")`.

- [ ] `void cmd_at(int argc, char **argv)` -- dispatch on `argv[1]`: list/cancel/HH:MM
- [ ] `at HH:MM cmd`: parse time; compute `next_run_unix`; create closure storing command string; `sched_task_add()`
- [ ] Command closure: `g_at_cmds[AT_MAX]` static array of `{ id, command_str[256] }`; `at_exec_wrapper()` looks up by task name
- [ ] `at list`: filter `g_sched_tasks[]` for one-shot entries; print table: ID, scheduled time, command
- [ ] `at cancel <id>`: `sched_task_remove()`; clear `g_at_cmds` slot
- [ ] Register `at` in command table: `{ "at", cmd_at, "at HH:MM <cmd> | at list | at cancel <id>" }`
- [ ] Commit: `"shell: at command -- one-shot scheduled tasks, HH:MM parse, list/cancel"`

---

## OS Comparison


| ⭐  | Feature                  | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ------------------------ | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| ⭐  | Recycle Bin              | ✅ `$Recycle.Bin`; `$I`/`$R` file pairs; Recycle | ✅ `~/.local/share/Trash/`; `.trashinfo` INI; `trash-cli`; per-filesystem | ⬜ §1 -- `⭐` human-readable INI meta (matches |
| 💎  | Recycle Bin UI           | ✅ Recycle Bin explorer window; list/icon | ✅ GNOME/KDE Trash window; restore; empty; | ⬜ §2 -- `CTRL_LISTVIEW` with 4 columns; column-sort |
| 💎  | ZIP support              | ✅ Built-in ZIP (Explorer); `Compress-Archive` PowerShell; | ✅ `zip`/`unzip` utils; libz in glibc;   | ⬜ §3 -- -5; miniz vendored + freestanding |
| ⭐  | Task scheduler           | ✅ Task Scheduler (`schtasks`); XML task | ✅ `cron`; `systemd timers`; `at`; D-Bus | ⬜ §6 -- `⭐` zero external daemon --    |
| 💎  | Built-in scheduled tasks | ✅ W32TM, Windows Update, Disk Defrag,   | ✅ systemd timers for chrony, journald   | ⬜ §7 -- same 4 tasks registered via     |
| 💎  | `at` command             | ✅ `at` command (deprecated in Win10+;   | ✅ `at` (POSIX, deprecated in favour     | ⬜ §8 -- one-shot scheduler entries; no daemon |

> **After §1–§8:** Impossible OS has complete Recycle Bin, ZIP, and task scheduling infrastructure. The dual `⭐` differentiators: Recycle Bin uses human-readable INI meta files (more transparent than Windows' binary `$I`/`$R` pairs) and the task scheduler runs as direct PIT-ticked kernel workqueue callbacks -- no `atd` daemon, no XML, no D-Bus, just a 16-slot static array with function pointers.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `trash_delete("C:\\Users\\Default\\test.txt")` → `C:\Recycle\trash_00000001\test.txt` exists; `C:\Recycle\_meta\trash_00000001.meta` has `OriginalPath=...`
- [ ] `trash_count()` → 1; `trash_restore("trash_00000001")` → file back at original path; `trash_count()` → 0
- [ ] Recycle Bin app opens with correct row; "Empty Recycle Bin" → confirm → list cleared; icon state switches
- [ ] `miniz_test()` serial log: `[miniz] round-trip: PASS`
- [ ] `zip test.zip C:\Users\Default\Documents\a.txt C:\Users\Default\Documents\b.txt` → `test.zip` created; `unzip -l test.zip` lists 2 entries with sizes; `unzip test.zip C:\Temp\out` → both files extracted
- [ ] `unzip encrypted.zip` → "Error: Password-protected ZIP not supported"
- [ ] `sched_task_add("test_task", test_cb, 5, 1)` → fires after 5 s; `last_result` updated; fires again 5 s later
- [ ] `at 09:00 notepad.exe` → task registered; `at list` shows entry; after system time reaches 09:00 → notepad launches; task auto-removed
- [ ] `at cancel 1` → task removed from `at list`
- [ ] `klog_rotate(100)` with a log > 100 lines → log file trimmed to last 100 lines
- [ ] Commit: `"recycle+zip+sched: recycle bin, miniz ZIP, task scheduler -- all complete"`
