---
schema_version: 1
id: file-search
domain: 09-desktop-shell
status: active
title: "TODO-05 -- File Search & Indexing"
---

# TODO-05 -- File Search & Indexing

> **Goal:** Build an instant, VFS-backed search system: a PMM-allocated flat-array index rebuilt in a background kernel thread, a ranked query API wired to a `SYS_SEARCH` syscall, `find` shell command, Start Menu and File Manager integration, and incremental index update hooks on `vfs_create/delete/rename`.

> [!IMPORTANT]
> **Already exists**: `vfs_readdir(dir_node, index)` + `vfs_finddir(dir_node, name)` + `vfs_stat(path, stat)` + `vfs_create/rename/unlink` in `vfs.h`. `pmm_alloc_contiguous(count)` + `pmm_free_contiguous(addr, count)` in `pmm.h`. `time_now()` (TODO-10 §1) for `modified_ts`. `sched_task_add("index_rebuild", search_index_rebuild, 1800, 1)` already registered in TODO-04 §1 -- §2 here provides the real implementation that replaces the stub. `klog()` in `klog.h`. `task_create()` for background thread. `kmalloc/kfree` for small structs. **Highest defined syscall**: `SYS_CLIPBOARD_GET=57` → assign `SYS_SEARCH=58`. **Missing**: all `search_*` functions, VFS tree walker, index cache file, query ranking, VFS mutation hooks. Complete sections in order: index core → query API → `find` command → Start Menu integration → File Manager integration → change notifications.

## Inputs

- `include/kernel/fs/vfs.h` -- `vfs_readdir(node, idx)`, `vfs_finddir(node, name)`, `vfs_stat(path, stat)`, `vfs_create`, `vfs_rename`, `vfs_unlink`, `vfs_open`, `vfs_read`, `vfs_write` -- used by §1 tree walk, §1 cache I/O, and §6 hooks
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous(count)` -- used by §1 index buffer
- `include/kernel/mm/heap.h` -- `kmalloc/kfree` -- used for small per-query result buffers
- `include/kernel/sched/task.h` -- `task_create()` -- used by §1 background index thread
- `include/kernel/sched/syscall.h` -- syscall table -- `SYS_SEARCH=58` added in §2
- `include/kernel/time.h` (TODO-10 §1) -- `time_now()` -- used by §1 `modified_ts` field
- `include/kernel/klog.h` -- `klog()` -- used throughout
- `include/kernel/scheduler_tasks.h` (TODO-04 §8) -- `sched_task_add()` -- §2 replaces the stub `search_index_rebuild()` registered there
- `include/desktop/wm.h` -- `wm_create_window()`, window event callbacks -- used by §4 Start Menu and §5 File Manager search bars (XREFs)
- → XREF: `09-desktop-shell/TODO-04-recycle-zip-scheduler.md §1` -- `sched_task_add("index_rebuild", search_index_rebuild, 1800, 1)` is registered there; §2 here provides the real body that stub forwards to
- → XREF: `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §4` -- Start Menu search bar calls `search_query()`; §5 is the integration layer that wires §2 into the existing search-bar input handler
- Future dependency (no XREF target yet): `10-apps/TODO-file-manager.md` -- §5 File Manager integration is a forward hook; no File Manager TODO exists yet

## Outcome

- `search_index_rebuild()` walks the full VFS tree recursively; fills a PMM-allocated `search_entry_t[]`; caches to `C:\Impossible\System\Cache\search.idx`.
- `search_query(query, results, max)` returns up to `max` ranked results; apps first.
- `SYS_SEARCH=58` exposes query API to user-mode programs.
- `find <query>` shell command with `--type` and `--path` filters.
- Start Menu search bar live-filters via `search_query()` with 150 ms debounce; matched text highlighted in accent color.
- File Manager search bar scopes to current directory subtree.
- `vfs_create/rename/unlink` hooks increment a dirty flag; scheduler triggers incremental rebuild within 60 s.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                            | Depends On                                                                       | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------- | :----: |
| ⭐  |   1   | §1 Search index -- VFS tree walk, PMM flat array, disk cache, background thread + scheduler hook        | `vfs_readdir`, `pmm_alloc_contiguous`, `task_create`, `time_now()` (planned)   |  [ ]   |
| 💎  |   2   | §2 Query API -- case-insensitive substring match, ranking (score 3/2/1), apps first, `SYS_SEARCH=58`   | §1 index must exist; `SYS_CLIPBOARD_GET=57` highest existing syscall            |  [ ]   |
| 💎  |   3   | §3 `find` shell command -- `find <query>`, `--type`, `--path`, relevance-sorted output                 | §2 query API                                                                     |  [ ]   |
| 💎  |   4   | §4 Start Menu integration -- live filter with 150 ms debounce, accent highlight, keyboard nav           | §2 query API; TODO-09 §4 Start Menu search bar scaffold must exist              |  [ ]   |
| 💎  |   5   | §5 File Manager integration -- toolbar search scoped to current directory subtree                       | §2 query API; File Manager window must have toolbar (future TODO)                |  [ ]   |
| ⭐  |   6   | §6 Index change notifications -- `vfs_create/delete/rename` dirty hooks, incremental add/remove, 60 s  | §1 index; §2 query; `vfs_create/rename/unlink` (exist)                          |  [ ]   |

---

## 1. Search Index Core `[Opus]`

`struct search_entry` (name[64], full_path[256], type: FILE/FOLDER/APP, size, modified_ts). `search_index_rebuild()` walks VFS tree via `vfs_readdir` recursively; fills PMM-allocated flat array; caches to `C:\Impossible\System\Cache\search.idx`. Background kernel thread at boot + every 30 min via scheduler.

**Files:** `src/kernel/search.c` (new), `include/kernel/search.h` (new)

> [!NOTE]
> `[Opus]` due to: novel PMM flat-array design (no prior Impossible OS precedent for PMM-backed index), concurrent background rebuild (lock-based swap of live index pointer without stalling queries), VFS tree recursion with depth limit (guard against cycles/deep nesting), and binary cache format design. **PMM buffer**: `g_search_buf = pmm_alloc_contiguous(SEARCH_MAX_PAGES)` where `SEARCH_MAX_PAGES = (SEARCH_MAX_ENTRIES * sizeof(search_entry_t)) / 4096 + 1`. `SEARCH_MAX_ENTRIES = 65536`. Two buffers (`g_search_buf_a/b`) -- background writes to inactive buffer; on completion: `spinlock_acquire` → swap `g_active_buf` pointer → `spinlock_release` → `pmm_free_contiguous(old_buf)`. **VFS tree walk**: `search_walk_node(path_prefix, vfs_node, depth)` (max depth = 16); `vfs_readdir(node, i)` in loop until NULL; for each dirent: get `vfs_finddir(node, name)` → `vfs_stat()`; if FILE: determine APP if path starts with `C:\Impossible\Bin\` or `C:\Program Files\`; append entry; if DIR: recurse. **Cache format**: `C:\Impossible\System\Cache\search.idx` -- `u32 magic=0x53524348` + `u32 version=1` + `u32 count` + `u64 built_at` + `search_entry_t[count]`. Write with `vfs_create()` + `vfs_write()`. Load on boot if cache exists and is < 2 h old (compare `built_at` to `time_now()`). **Background thread**: `search_rebuild_thread()` -- calls `search_index_rebuild()` then exits; `task_create("search_indexd", search_rebuild_thread, TASK_PRIORITY_LOW)` called from `kernel_main` after VFS is live.

- [ ] `typedef struct { char name[64]; char full_path[256]; uint8_t type; uint64_t size; int64_t modified_ts; } search_entry_t;` -- `#define SEARCH_TYPE_FILE 0`, `SEARCH_TYPE_FOLDER 1`, `SEARCH_TYPE_APP 2`
- [ ] `#define SEARCH_MAX_ENTRIES 65536` + `#define SEARCH_MAX_DEPTH 16` in `search.h`
- [ ] `static search_entry_t *g_search_buf_a, *g_search_buf_b, *g_active_buf` + `static uint32_t g_entry_count` + `static spinlock_t g_search_lock`
- [ ] `static void search_walk_node(const char *prefix, struct vfs_node *node, int depth, search_entry_t *buf, uint32_t *count)` -- recursive walk; depth guard; detect APP by path prefix
- [ ] `void search_index_rebuild(void)` -- alloc inactive buffer; walk from `C:\`; swap live pointer under lock; write cache to `search.idx`; `klog(LOG_INFO, "search", "index: %u entries", count)`
- [ ] `search_index_load_cache(void)` -- read `search.idx`; verify magic/version; check `built_at` age; if fresh: load into `g_active_buf`; skip rebuild
- [ ] `void search_init(void)` -- `vfs_mkdir("C:\\Impossible\\System\\Cache\\")` if absent; try `search_index_load_cache()`; if no cache: spawn `search_rebuild_thread()`
- [ ] `int search_index_count(void)` -- return `g_entry_count`
- [ ] Boot log: `klog(LOG_OK, "search", "index: %u entries (cached)", count)` or `"(rebuilding in background)"`
- [ ] Commit: `"search: index core -- PMM flat array, VFS tree walk, binary cache, background thread, lock-swap"`

## 2. Search Query API `[Sonnet]`

`search_query(query, results[], max)` -- case-insensitive substring match; scoring: exact name = 3, prefix = 2, substring = 1; apps first, then folders, then files. Live scan app dirs for fresh installs. `SYS_SEARCH=58` syscall.

**Files:** `src/kernel/search.c` (extend), `include/kernel/search.h` (extend), `include/kernel/sched/syscall.h` (extend)

> [!NOTE]
> Score calculation per entry: `if kstrcasecmp(entry->name, query) == 0 → score = 3`; `else if kstrncasecmp(entry->name, query, len) == 0 → score = 2`; `else if kstrcasestr(entry->name, query) != NULL → score = 1`; `else score = 0` (skip). Sort result list: primary sort key = type (APP < FOLDER < FILE), secondary = score descending. **Live app scan**: after scanning index, check `C:\Impossible\Bin\` and `C:\Program Files\` via `vfs_readdir` for entries not already in results (freshly installed apps); append with score = 1 if name contains query. **Content search** (opt-in `SEARCH_INCLUDE_CONTENT` flag): if file size < 100 KB and type == FILE: `vfs_read()` file; `kstrcasestr(buf, query)` -- add to results with score = 0 (lowest). `SYS_SEARCH=58`: `sys_search(query_ptr, result_buf, max)` -- copy query from user; call `search_query()`; copy result array to user buf. `#define SYS_SEARCH 58` in `syscall.h`.

- [ ] `typedef struct { search_entry_t entry; int score; } search_result_t;`
- [ ] `int search_query(const char *query, search_result_t *results, int max)` -- scan `g_active_buf`; compute score; filter score > 0; collect; sort by (type, score_desc); return count
- [ ] `static int search_result_cmp(const void *a, const void *b)` -- `qsort` comparator: APP before FOLDER before FILE; higher score first within type
- [ ] `int search_query_scoped(const char *query, const char *root_path, search_result_t *results, int max)` -- same but filter `entry.full_path` starting with `root_path` (for File Manager §5)
- [ ] Live app scan: after index scan, `vfs_readdir()` on `C:\Impossible\Bin\` + `C:\Program Files\`; add missing entries as SEARCH_TYPE_APP, score 1
- [ ] `int sys_search(const char *query, search_result_t *out, int max)` syscall handler; `#define SYS_SEARCH 58` in `syscall.h`
- [ ] Commit: `"search: query API -- scoring 3/2/1, apps-first sort, live app scan, SYS_SEARCH=58"`

## 3. `find` Shell Command `[Sonnet]`

`find <query>` searches index, prints full paths sorted by relevance. `--type file/folder/app` filter. `--path <dir>` limits to subtree. One full path per line.

**Files:** `src/shell/cmds.c` (extend)

> [!NOTE]
> Argument parsing: iterate `argv`; `--type file/folder/app` sets type filter; `--path <dir>` sets `root_path` for `search_query_scoped()`; remaining arg is query string. If no query: print usage. Output: `for each result: kprintf("%s\n", result.entry.full_path)` sorted by relevance (already sorted from `search_query()`). If 0 results: `kprintf("No results found for '%s'\n", query)`. Type filter: post-query filter on `result.entry.type`. Limit output to 100 results to avoid flooding the terminal.

- [ ] `void cmd_find(int argc, char **argv)` -- parse `--type`, `--path`, query; dispatch to `search_query` or `search_query_scoped`; print results
- [ ] Type filter mapping: `"file"→SEARCH_TYPE_FILE`, `"folder"→SEARCH_TYPE_FOLDER`, `"app"→SEARCH_TYPE_APP`
- [ ] Print: `full_path` one per line; prefix `[APP]`/`[DIR]`/`[FILE]` tag
- [ ] 0-result state: `"No results found for '<query>'"`
- [ ] Register `find` in command table: `{ "find", cmd_find, "find <query> [--type file|folder|app] [--path <dir>]" }`
- [ ] Commit: `"shell: find command -- search index query, --type/--path filters, relevance-sorted output"`

## 4. Start Menu Integration `[Sonnet]`

Start Menu search bar calls `search_query()` on each keystroke with 150 ms debounce. Live-filters pinned + all-programs list. Matched portion highlighted in accent color. Keyboard nav: down arrow from search → first result; Enter launches.

**Files:** `src/desktop/startmenu.c` (extend), `include/desktop/startmenu.h` (extend)

> [!NOTE]
> → XREF: `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §4` -- the search bar scaffold (CTRL_TEXTBOX input, "No results found" state) already exists or is planned there; §5 here wires `search_query()` into that scaffold. **Debounce**: maintain `g_search_debounce_tick` (PIT ticks); on keystroke: `g_search_debounce_tick = system_get_ticks() + DEBOUNCE_TICKS` (150 ms → `DEBOUNCE_TICKS = PIT_TARGET_FREQ * 150 / 1000`); in `startmenu_tick()`: if `system_get_ticks() >= g_search_debounce_tick && g_search_dirty`: call `search_query()` + re-render results. **Accent highlight**: for each result name rendered via `ttf_draw_string()`: find match offset; draw prefix in `theme_get(THEME_TEXT)`, match span in `theme_get(THEME_ACCENT)`, suffix in `theme_get(THEME_TEXT)`. **Keyboard nav**: `WM_KEYDOWN(VK_DOWN)` from search textbox → set focus to first result row; Enter → `file_assoc_open(entry.full_path)` + close Start Menu. **"No results"**: render centered gray text if `search_query()` returns 0.

- [ ] `g_search_debounce_tick` + `g_search_dirty` flag in `startmenu.c`
- [ ] Keystroke handler: set `g_search_dirty=1`; update `g_search_debounce_tick`
- [ ] `startmenu_do_search(const char *query)` -- `search_query(query, g_search_results, 12)`; trigger re-render
- [ ] `startmenu_tick()` -- debounce check; call `startmenu_do_search()` when debounce elapsed
- [ ] Render: accent-highlight matched substring per result name using split `ttf_draw_string()` calls
- [ ] Keyboard nav: Down from textbox → first result row focus; Enter → launch + close
- [ ] "No results found" state: gray centered text in results area
- [ ] Commit: `"startmenu: search integration -- 150ms debounce, accent highlight, keyboard nav, no-results state"`

## 5. File Manager Integration `[Sonnet]`

Search bar in File Manager toolbar calls `search_query_scoped()` constrained to current directory subtree. Results replace file area: icon + name + full path. Click result → navigate or `file_assoc_open()`.

**Files:** `src/desktop/file_manager.c` (extend -- if exists; else stub in `src/desktop/startmenu.c`)

> [!NOTE]
> File Manager may not exist yet as a standalone app. If `src/desktop/file_manager.c` does not exist, stub this section as a forward integration point -- add `search_query_scoped()` declaration and note that the File Manager TODO (future, under `10-apps/`) must call it. If File Manager exists: add a `CTRL_TEXTBOX` to the existing toolbar; on `WM_KEYDOWN(VK_RETURN)` in the search box: call `search_query_scoped(query, current_dir, results, 128)`; replace file area `CTRL_LISTVIEW` content with results (icon from `file_assoc_get_icon()`, name, full path); click result: if `SEARCH_TYPE_FOLDER` → navigate to folder; else → `file_assoc_open(full_path)`. Press Escape or clear box → restore normal directory listing.

- [ ] Confirm whether `src/desktop/file_manager.c` exists; if not: add comment stub + `search_query_scoped()` API declaration only
- [ ] If File Manager exists: add toolbar search `CTRL_TEXTBOX`; wire `WM_KEYDOWN(VK_RETURN)` to `search_query_scoped()`
- [ ] Results in file area: icon from `file_assoc_get_icon(ext)`, name, full path columns
- [ ] Click result: folder → navigate; file → `file_assoc_open()`; App → `task_create_user(full_path)`
- [ ] Escape / clear → restore directory listing
- [ ] Commit: `"file_manager: search integration -- scoped search_query_scoped(), results replace file area, Esc restore"`

## 6. Index Change Notifications `[Sonnet]`

Hook `vfs_create`/`vfs_unlink`/`vfs_rename` to mark index dirty. `search_index_add_entry(path)` and `search_index_remove_entry(path)` for incremental updates. Dirty flag triggers rebuild within 60 s via scheduler.

**Files:** `src/kernel/search.c` (extend), `src/kernel/fs/vfs.c` (extend hooks)

> [!NOTE]
> Hook pattern: add `search_on_vfs_create/delete/rename(path)` weak-coupling callbacks at the tail of `vfs_create()`, `vfs_unlink()`, `vfs_rename()` in `vfs.c` -- function pointer table (`g_vfs_mutation_hooks`) to avoid a circular include between `vfs.c` and `search.c`. `vfs_register_mutation_hook(fn)` registered from `search_init()`. **Incremental update**: `search_index_add_entry(path)` -- `vfs_stat(path)` → append/replace entry in `g_active_buf` under spinlock (bump count). `search_index_remove_entry(path)` -- linear scan `g_active_buf` for `full_path` match → zero-copy remove (swap with last entry + decrement count) under spinlock. **Dirty rebuild**: `static volatile int g_index_dirty` + `static int64_t g_dirty_since_ts`. In `sched_task_tick()` equivalent (or the existing `index_rebuild` scheduler task): if `g_index_dirty && (time_now() - g_dirty_since_ts) >= 60` → `search_index_rebuild()` → `g_index_dirty = 0`. Avoids full rebuild on every file operation.

- [ ] `typedef void (*vfs_mutation_hook_t)(const char *path, int event)` in `vfs.h`; `#define VFS_EVENT_CREATE 0`, `VFS_EVENT_DELETE 1`, `VFS_EVENT_RENAME 2`
- [ ] `void vfs_register_mutation_hook(vfs_mutation_hook_t fn)` -- single-slot hook (expandable); called at tail of `vfs_create/unlink/rename`
- [ ] `search_init()` calls `vfs_register_mutation_hook(search_on_vfs_mutation)`
- [ ] `search_on_vfs_mutation(path, event)` -- CREATE/RENAME: `search_index_add_entry(path)`; DELETE: `search_index_remove_entry(path)`; set `g_index_dirty=1`; record `g_dirty_since_ts`
- [ ] `search_index_add_entry(path)` -- `vfs_stat()` → append entry to `g_active_buf[g_entry_count++]` under `g_search_lock`
- [ ] `search_index_remove_entry(path)` -- scan; swap-with-last + decrement under `g_search_lock`
- [ ] Dirty rebuild in scheduler: modify `index_rebuild` task body to check `g_index_dirty` + elapsed ≥ 60 s before doing full rebuild
- [ ] Commit: `"search: VFS change hooks -- incremental add/remove, dirty flag, 60s rebuild trigger"`

---

## OS Comparison


| ⭐  | Feature              | 🪟 Win11                                  | 🐧 Linux                                                   | 🚀 Impossible OS                                                        |
| --- | -------------------- | ----------------------------------------- | ---------------------------------------------------------- | ----------------------------------------------------------------------- |
| ⭐  | Search index         | ✅ Windows Search (ETW-based, NTFS change | ✅ `locate`/`updatedb`; `mlocate.db` binary; `inotify` for | ⬜ §1 -- `⭐` no background daemon --                                   |
| 💎  | Ranked query API     | ✅ Windows Search API; relevance ranking; | ✅ `locate -i`; `grep -r` for                              | ⬜ §2 -- scoring 3/2/1; type-primary sort; live                         |
| 💎  | `find` shell command | ✅ `where`, `dir /s /b`, `Get-ChildItem   | ✅ `find`, `locate`, `fd`, `fzf` (external)                | ⬜ §3 -- integrated with ranked index; `--type`                         |
| 💎  | Start Menu search    | ✅ Start Menu search (integrated, very    | ✅ GNOME Activities (live search, app+file                 | ⬜ §4 -- `⭐` no cloud results mixed                                    |
| 💎  | File Manager search  | ✅ File Explorer search bar (very         | ✅ Nautilus/Dolphin search bars; `tracker` for             | ⬜ §5 -- `search_query_scoped(root_path)` constrains results to subtree |
| ⭐  | VFS change hooks     | ✅ NTFS change journal + USN              | ✅ `inotify` / `fanotify` kernel events;                   | ⬜ §6 -- `⭐` zero-daemon: VFS mutation hooks                           |

> **After §1–§6:** Impossible OS has an always-on search index with no background daemon, no D-Bus, and no third-party indexer. The `⭐` differentiators: double-buffer PMM swap keeps queries live during index rebuild (no stall), VFS mutation hooks update the index inline in the kernel (no `inotify` userspace round-trip), and Start Menu search is guaranteed ad-free with local-only results.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot serial log: `[search] index: N entries (rebuilt)` or `(cached)` -- N > 0
- [ ] `search_query("Inter", results, 10)` → returns entries matching `Inter.ttf` and any matching dirs; score sorted
- [ ] `find inter` → prints `C:\Impossible\Fonts\Inter-Regular.ttf` and any other matching paths
- [ ] `find note --type app` → returns only APP-type entries; `find System32 --path C:\Impossible` → constrained to subtree
- [ ] Start Menu: type "note" → live filter shows Notepad (APP type, score 3 if exact); accent-highlighted match portion; Enter launches app
- [ ] Start Menu: type "xyzzy_nonexistent" → "No results found" state displayed
- [ ] `vfs_create("C:\\Users\\Default\\Documents\\newfile.txt", ...)` → within 60 s: `find newfile` → returns new path
- [ ] `vfs_unlink("C:\\Users\\Default\\Documents\\newfile.txt")` → within 60 s: `find newfile` → "No results found"
- [ ] `search_index_count()` returns correct count matching serial log
- [ ] Commit: `"search: file search and indexing -- all sections complete"`
