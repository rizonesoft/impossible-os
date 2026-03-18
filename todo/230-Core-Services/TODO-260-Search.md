# P0008 — Search & File Indexing

> **Goal:** Fast filename and content lookup via an in-memory search index,
> intelligent query API, and integration with Start Menu, File Manager, and shell.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Search Index

**Prompt:** The search index provides fast filename lookup without scanning the entire filesystem on each query. Walk the VFS tree recursively at boot, recording each file/folder name and full path in a sorted flat array. Each entry has: name (basename), full_path, type (FILE/FOLDER/APP), size, modified timestamp. Store the index in `C:\Impossible\System\Cache\search.idx`. Rebuild in a background kernel thread at boot and every 30 minutes via the task scheduler (TODO-290). The index is kept in memory as a PMM-allocated array (not kmalloc — can be large). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: file search indexer"`. Add notes directly in this TODO section covering the index structure, background thread, and cache file format.

> **Beats:** Windows Search uses a Jet database (heavy). Linux `locate` uses a flat file. Impossible OS: simple flat sorted array — fast binary search, minimal overhead.

- [ ] Define `struct search_entry` (name[64], full_path[256], type [FILE/FOLDER/APP], size, modified)
- [ ] Create `src/kernel/search.c` and `include/kernel/search.h`
- [ ] `search_index_rebuild()` — walk VFS tree, populate index array
- [ ] Index storage: PMM-allocated array (`pmm_alloc_contiguous()`) — NOT kmalloc
- [ ] Cache to disk: `C:\Impossible\System\Cache\search.idx` (flat binary: entry count + entry array)
- [ ] Background thread: rebuild at boot + every 30 minutes via TODO-290 `sched_task_add()`
- [ ] `search_index_count()` — number of indexed entries
- [ ] Boot log: `[OK] Search index: N entries indexed`
- [ ] Commit: `"kernel: file search indexer"`

---

## 2. Search Query API

**Prompt:** `search_query(query, results, max)` performs a case-insensitive substring match on the index, returning sorted results (apps first, then folders, then files). Results are ranked: exact name match ranks highest, prefix match second, substring match third. The query also scans app names from `C:\Impossible\Bin\` and `C:\Programs\` for freshly installed apps not yet indexed. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: search query API"`. Add notes directly in this TODO section.

> **Beats:** Windows Search requires indexing service. Linux `locate` is case-sensitive by default. Impossible OS: case-insensitive by default, ranked results, live app scan.

- [ ] Implement `search_query(query, results[], max)` — case-insensitive substring match
- [ ] Result ranking: exact match → prefix match → substring, apps before files
- [ ] Search sources:
  - [ ] Indexed files + folders (from §1 index)
  - [ ] App names live scan: `C:\Impossible\Bin\` + `C:\Programs\` (for freshly installed apps)
  - [ ] *(Stretch)* File contents: grep text files (slow path, opt-in)
  - [ ] *(Stretch)* Control Panel applet names (search settings entries)
- [ ] Add `SYS_SEARCH` syscall (number to be assigned)
- [ ] Commit: `"kernel: search query API"`

---

## 3. Shell Integration

**Prompt:** Add a `find` command to the shell that searches the index and prints matches. `find <query>` searches all indexed entries. `find <query> --type file` or `--type folder` filters. Results show full paths, one per line. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: find command"`.

- [ ] Shell command: `find <query>` — search index, print matching paths
- [ ] `find <query> --type file` — files only
- [ ] `find <query> --type folder` — folders only
- [ ] `find <query> --type app` — apps only
- [ ] Commit: `"shell: find command"`

---

## 4. UI Integration

**Prompt:** Wire the search index into the Start Menu search bar and File Manager search bar. Start Menu: as user types in search bar, call `search_query()` and filter the pinned/all-programs list live. File Manager: search bar in toolbar filters the current directory's file list (client-side filter first, then full index for non-current dirs). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: search UI integration"`.

> **Beats:** Linux search in file managers requires external indexers (Tracker, Baloo). Windows Search is a heavy service. Impossible OS: in-kernel index, zero external process.

- [ ] Start Menu search bar → `search_query()` → filter app list live (see TODO-190 §7.3)
- [ ] File Manager search bar → filter current directory contents (see TODO-320 §1.5)
- [ ] Search results show: icon + name + full path
- [ ] Click result → open file (via `file_assoc_open()`) or navigate folder
- [ ] Commit: `"desktop: search UI integration"`

---

## Priority Order

| Priority | Section              | Reason                                              |
|----------|----------------------|-----------------------------------------------------|
| 🔴 P0    | §1 Search Index      | Foundation — must exist before any query            |
| 🟠 P1    | §2 Search Query API  | API for Start Menu and File Manager                 |
| 🟠 P1    | §3 Shell Integration | `find` command — immediate dev utility              |
| 🟢 P3    | §4 UI Integration    | Wire into Start Menu + File Manager (depends on §2) |

---

## Key Files

| File                                          | Purpose                         |
|-----------------------------------------------|---------------------------------|
| `src/kernel/search.c`                         | [NEW] Search index + query      |
| `include/kernel/search.h`                     | [NEW] Search API header         |
| `C:\Impossible\System\Cache\search.idx`       | [RUNTIME] Persisted index file  |

---

## OS Comparison

| Feature                       | 🪟 Windows 11 (Windows Search)     | 🐧 Linux (locate / Tracker / Baloo) | 🚀 Impossible OS                              |
| ----------------------------- | --------------------------------- | ---------------------------------- | -------------------------------------------- |
| Filename index                | ✅ Jet database                    | ✅ `mlocate` flat file              | ⬜ §1 P0 — PMM flat array                     |
| Background rebuild            | ✅ SearchIndexer.exe service       | ✅ `updatedb` cron / Tracker daemon | ⬜ §1 P0 — in-kernel background thread        |
| Case-insensitive query        | ✅ Always                          | ⚠️ `locate -i` (optional)          | ⬜ §2 P1 — **always case-insensitive**        |
| Result ranking (apps first)   | ✅ Windows Search ranking          | ❌ locate returns raw sorted paths  | ⬜ §2 P1 — **ranked: apps > folders > files** |
| Shell `find` command          | ✅ Windows Search via Explorer API | ✅ `locate` / `find` / `fd`         | ⬜ §3 P1                                      |
| Start Menu search integration | ✅ Bing + Local Search             | ✅ GNOME search provider            | ⬜ §4 P3                                      |
| File content search           | ✅ Full-text indexing              | ✅ Tracker full-text                | ⬜ §2 (stretch)                               |
| **No external daemon**        | ❌ SearchIndexer.exe required      | ❌ Tracker/Baloo daemon             | ✅ **In-kernel thread — zero external**       |
| **Simple flat index**         | ❌ Complex Jet DB                  | ✅ locate flat file                 | ✅ **§1 — flat sorted array, binary search**  |
