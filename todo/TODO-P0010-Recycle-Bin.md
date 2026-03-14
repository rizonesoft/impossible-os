# P0010 — Recycle Bin

> **Goal:** Recoverable file deletion with metadata tracking and desktop integration.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Recycle Bin Core

**Prompt:** The recycle bin makes file deletion recoverable. `trash_delete(path)` moves the file to `C:\Recycle\` and saves metadata. `trash_restore(trash_name)` moves it back. `trash_empty()` permanently deletes everything. User-initiated deletions go to the recycle bin; only explicit "Delete permanently" uses VFS permanent delete. After completing all items, create `docs/architecture/recycle-bin.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: recycle bin"`.


- [ ] Create `src/kernel/trash.c`
- [ ] Implement `trash_delete(path)` — move file to `C:\Recycle\`, save metadata
- [ ] Implement `trash_restore(trash_name)` — move back to original path (from metadata)
- [ ] Implement `trash_empty()` — permanently delete all files in recycle
- [ ] Implement `trash_count()` — number of items
- [ ] Implement `trash_size()` — total bytes used
- [ ] Commit: `"kernel: recycle bin"`

---

## 2. Metadata Files

- [ ] Create `.meta` file for each trashed item in `C:\Recycle\_meta\`
- [ ] Store: OriginalPath, DeletedAt (Unix timestamp), Size
- [ ] INI format for easy parsing
- [ ] Commit: `"kernel: recycle bin metadata"`

---

## 3. Desktop Integration

> **Moved to [TODO-P0302-Resources.md](TODO-P0302-Resources.md) §3** — Recycle Bin desktop icon states (`ICON_TRASH_EMPTY`/`ICON_TRASH_FULL`), context menu, auto-purge.
