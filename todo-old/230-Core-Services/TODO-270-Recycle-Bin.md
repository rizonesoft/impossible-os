# P0010 — Recycle Bin

> **Goal:** Recoverable file deletion with metadata tracking, size limits, and desktop integration.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Recycle Bin Core

**Prompt:** The recycle bin makes file deletion recoverable. `trash_delete(path)` moves the file to `C:\Recycle\{uuid}\` (with a UUID suffix to avoid name collisions) and saves metadata. `trash_restore(trash_name)` reads the metadata and moves it back to the original path. `trash_empty()` permanently deletes everything in `C:\Recycle\`. User-initiated deletions always go to the recycle bin; only explicit "Delete permanently" uses VFS permanent delete. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: recycle bin"`. Add notes directly in this TODO section covering the UUID naming scheme, conflict resolution, and cross-drive trash handling.

> **Beats:** Windows Recycle Bin is per-drive ($Recycle.Bin). Linux Trash is per-user (`~/.local/share/Trash/`). Impossible OS: single unified `C:\Recycle\` — simpler, no per-drive config.

- [ ] Create `src/kernel/trash.c` and `include/kernel/trash.h`
- [ ] `trash_delete(path)` — move file to `C:\Recycle\{uuid}\`, create metadata file
- [ ] UUID suffix: simple counter-based (e.g., `trash_0001`, `trash_0002`) — no real UUID needed
- [ ] `trash_restore(trash_name)` — read OriginalPath from metadata → move back
- [ ] `trash_restore_all()` — restore everything (used by "Undo Delete" in File Manager)
- [ ] `trash_empty()` — permanently delete all files in `C:\Recycle\`
- [ ] `trash_count()` — number of items in recycle bin
- [ ] `trash_size()` — total bytes used by all trashed files
- [ ] Commit: `"kernel: recycle bin"`

---

## 2. Metadata Files

**Prompt:** Each trashed item has a corresponding `.meta` INI file in `C:\Recycle\_meta\`. The metadata stores: `OriginalPath`, `DeletedAt` (Unix timestamp), `Size` (bytes), `OriginalName` (display name). This metadata is read by the Recycle Bin app window to display the list of deleted items. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: recycle bin metadata"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Create `C:\Recycle\_meta\` directory on first boot
- [ ] Each deleted item: `C:\Recycle\_meta\{trash_name}.meta` (INI format)
- [ ] Meta fields: `OriginalPath=`, `OriginalName=`, `DeletedAt=` (Unix timestamp), `Size=` (bytes)
- [ ] `trash_delete()` writes the meta file
- [ ] `trash_restore()` reads the meta file, then deletes it on success
- [ ] `trash_empty()` also deletes all meta files
- [ ] Commit: `"kernel: recycle bin metadata"`

---

## 3. Recycle Bin Window App

**Prompt:** The Recycle Bin desktop icon opens a File Manager-like window showing trashed items as a table: Name, Original Location, Date Deleted, Size. Right-click item → "Restore" or "Delete permanently". Toolbar buttons: "Empty Recycle Bin" (confirm dialog), "Restore all items". After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"apps: recycle bin window"`.

- [ ] Create `src/apps/recycle/recycle.c`
- [ ] List all items in `C:\Recycle\_meta\` and display as a detail table
- [ ] Columns: Name, Original Location, Date Deleted, Size
- [ ] Right-click row → "Restore" (calls `trash_restore()`) or "Delete Permanently" (calls VFS delete)
- [ ] Toolbar: "Empty Recycle Bin" (confirm dialog → `trash_empty()`)
- [ ] Toolbar: "Restore all items" (`trash_restore_all()`)
- [ ] Status bar: item count + total size
- [ ] Commit: `"apps: recycle bin window"`

---

## 4. Size Limits & Auto-Purge

**Prompt:** When trash grows beyond the configured size limit, auto-purge the oldest items. Auto-purge checks on every `trash_delete()` call. Registry `HKLM\SYSTEM\Recycle\MaxSize` (default 1 GiB = 0x40000000 bytes). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: recycle bin auto-purge"`.

- [ ] Read `HKLM\SYSTEM\Recycle\MaxSize` (default 1 GiB)
- [ ] After each `trash_delete()`: if `trash_size() > MaxSize` → delete oldest items until under limit
- [ ] Oldest-first: sort by `DeletedAt` from meta files
- [ ] Settings UI: Recycle Bin properties dialog → configure max size
- [ ] Commit: `"kernel: recycle bin auto-purge"`

---

## 5. Desktop Integration

> **Moved to [TODO-240-Resources.md](TODO-240-Resources.md) §3** — Recycle Bin desktop icon states (`ICON_TRASH_EMPTY`/`ICON_TRASH_FULL`), context menu, dynamic icon.

---

## Priority Order

| Priority | Section                     | Reason                                             |
|----------|-----------------------------|----------------------------------------------------|
| 🔴 P0    | §1 Recycle Bin Core         | Foundation — all delete operations need this       |
| 🔴 P0    | §2 Metadata Files           | Without metadata, restore is impossible            |
| 🟠 P1    | §3 Recycle Bin Window App   | User-facing delete management                      |
| 🟡 P2    | §4 Size Limits / Auto-Purge | Prevent disk fill from deleted files               |
| 🟡 P2    | §5 Desktop Integration      | See TODO-240 §3 (icon state + context menu)        |

---

## Key Files

| File                              | Purpose                                   |
|-----------------------------------|-------------------------------------------|
| `src/kernel/trash.c`              | [NEW] Recycle bin core                    |
| `include/kernel/trash.h`          | [NEW] Trash API header                    |
| `src/apps/recycle/recycle.c`      | [NEW] Recycle Bin window app              |
| `C:\Recycle\_meta\`              | [RUNTIME] Per-item metadata directory     |

---

## OS Comparison

| Feature                         | 🪟 Windows 11 ($Recycle.Bin) | 🐧 Linux (FreeDesktop Trash)      | 🚀 Impossible OS                                   |
| ------------------------------- | --------------------------- | -------------------------------- | ------------------------------------------------- |
| Recoverable deletion            | ✅ $Recycle.Bin per drive    | ✅ `~/.local/share/Trash/`        | ⬜ §1 P0 — `C:\Recycle\`                           |
| Metadata (original path/date)   | ✅ $I file (binary format)   | ✅ `Trash/info/*.trashinfo` (INI) | ⬜ §2 P0 — **INI .meta (readable)**                |
| Restore file to original path   | ✅ Explorer → Restore        | ✅ `trash-restore` / Nautilus     | ⬜ §3 P1 — Recycle Bin window                      |
| Empty recycle bin action        | ✅ Right-click → Empty       | ✅ `trash-empty` / Nautilus       | ⬜ §3 P1                                           |
| Size limit / auto-purge         | ✅ Configurable per drive, % | ❌ No built-in size limit         | ⬜ §4 P2 — Registry MaxSize                        |
| Desktop icon (empty vs full)    | ✅ Dynamic icon              | ✅ GNOME / KDE dynamic            | ⬜ TODO-240 §3 P1                                  |
| Recycle Bin window app          | ✅ Explorer shell namespace  | ✅ Nautilus trash:///             | ⬜ §3 P1                                           |
| **Single unified trash folder** | ❌ Per-drive $Recycle.Bin    | ❌ Per-user per-mount Trash       | ⬜ **§1 — single `C:\Recycle\` — simpler**         |
| **INI metadata (readable)**     | ❌ Binary $I file format     | ✅ .trashinfo INI format          | ⬜ **§2 — matches Linux readability, beats Win32** |
