# P0008 — Search & File Indexing

> **Goal:** Fast filename lookup via an in-memory search index, query API,
> and integration with Start Menu, File Manager, and shell.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Search Index

**Prompt:** The search index provides fast filename lookup without scanning the entire filesystem on each query. Walk the VFS tree recursively, recording each file/folder name and full path in a sorted index. Rebuild in a background thread at boot and periodically (every 30 minutes). Keep the index in memory, flush to disk for persistence. After completing all items, create `docs/architecture/search.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: file search indexer"`.


- [ ] Define `struct search_result` (path, match, type [FILE/FOLDER/APP/SETTING], modified)
- [ ] Create `src/kernel/search.c`
- [ ] Implement `search_index_rebuild()` — walk VFS tree, index file/folder names
- [ ] Store index in `C:\Impossible\System\Cache\search.idx` (flat file: path + name)
- [ ] Run index rebuild in background thread on boot + periodically (30 min)
- [ ] Commit: `"kernel: file search indexer"`

---

## 2. Search Query

- [ ] Implement `search_query(query, results, max)` — substring match on index
- [ ] Search sources:
  - [ ] File names (from index)
  - [ ] App names (scan `C:\Impossible\Bin\` + `C:\Programs\`)
  - [ ] *(Stretch)* File contents (scan text files — slow path)
  - [ ] *(Stretch)* Settings panel names
- [ ] Add `SYS_SEARCH` syscall
- [ ] Commit: `"kernel: search query API"`

---

## 3. Integration

- [ ] *(Stretch)* Start menu search bar → type to search apps + files
- [ ] *(Stretch)* File manager search bar → filter current directory
- [ ] *(Stretch)* Shell `find <query>` command
- [ ] Commit: `"desktop: search integration"`

---

## Priority Order

| Priority | Section         | Reason                               |
|----------|-----------------|--------------------------------------|
| 🔴 P0     | §1 Search Index | Foundation — index must exist first  |
| 🟠 P1     | §2 Search Query | API for apps to search               |
| 🟢 P3     | §3 Integration  | UI wiring (Start Menu, File Manager) |
