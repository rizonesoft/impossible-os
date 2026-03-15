# P0011 — ZIP Compression

> **Goal:** ZIP archive creation, extraction, listing, and shell/context menu integration via miniz.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (decompressed file data, buffers). `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. miniz Integration

**Prompt:** Port the **miniz** library (MIT, single-header, ~4000 lines) into `src/libs/miniz/`. Redirect its memory allocation macros to use `kmalloc`/`kfree` for struct allocations (small), and `pmm_alloc_contiguous()` for decompressed data buffers (potentially large). Disable miniz's stdio usage (it should use VFS file I/O instead). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"libs: miniz compression library"`.

> **Source:** https://github.com/richgel999/miniz (single-file, MIT license)

- [ ] Create `src/libs/miniz/miniz.h` and `src/libs/miniz/miniz.c` (or `.h`-only)
- [ ] Redirect: `#define MZ_MALLOC(sz) kmalloc(sz)` / `#define MZ_FREE(p) kfree(p)`
- [ ] For large decompression buffers: wrap miniz allocator to use `pmm_alloc_contiguous()` when sz > 4096
- [ ] Disable stdio: `#define MINIZ_NO_STDIO` and use VFS for file I/O
- [ ] Test: create + extract a minimal 3-file ZIP in a boot test
- [ ] Commit: `"libs: miniz compression library"`

---

## 2. ZIP API

**Prompt:** The ZIP API wraps miniz into a clean kernel-friendly interface. `zip_create(path, files, count)` creates a new ZIP archive. `zip_extract(path, dest_dir)` extracts all files using VFS writes. `zip_list(path, names, max)` lists archive contents without extracting. `zip_add_file(path, file_path)` appends a file. `zip_extract_file(zip_path, filename, dest)` extracts a single named file. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: ZIP archive support"`. Add notes directly in this TODO section covering miniz error codes and VFS write behavior.

- [ ] Create `include/kernel/zip.h` and `src/kernel/zip.c` (wrapper around miniz)
- [ ] `zip_create(zip_path, files[], count)` — create ZIP from file list
- [ ] `zip_extract(zip_path, dest_dir)` — extract all files to destination directory
- [ ] `zip_list(zip_path, names[], max)` — list filenames in archive (no extraction)
- [ ] `zip_add_file(zip_path, file_path)` — append file to existing ZIP
- [ ] `zip_extract_file(zip_path, filename, dest_path)` — extract single named file
- [ ] Handle path separators (`/` in ZIP, `\` in IXFS/FAT32) — normalize on extract
- [ ] Error codes: `ZIP_OK`, `ZIP_ERR_NOT_FOUND`, `ZIP_ERR_CORRUPT`, `ZIP_ERR_NO_SPACE`
- [ ] Commit: `"kernel: ZIP archive support"`

---

## 3. Shell Commands

**Prompt:** Add `zip` and `unzip` commands to the shell. `zip archive.zip file1 file2 ...` creates or appends to an archive. `unzip archive.zip` extracts to current directory. `unzip archive.zip -d dir` extracts to a specified directory. `unzip -l archive.zip` lists contents without extracting. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: zip/unzip commands"`.

- [ ] `zip <archive.zip> <file1> [file2 ...]` — create ZIP archive
- [ ] `zip -a <archive.zip> <file>` — add file to existing archive
- [ ] `unzip <archive.zip>` — extract to current directory
- [ ] `unzip <archive.zip> -d <dest>` — extract to specified directory
- [ ] `unzip -l <archive.zip>` — list contents
- [ ] Commit: `"shell: zip/unzip commands"`

---

## 4. Context Menu Integration *(Stretch)*

**Prompt:** Right-click a file or selection of files → "Compress to ZIP" creates a ZIP in the same directory named `filename.zip`. Right-click a .zip file → "Extract here" extracts in-place, "Extract to..." opens a folder picker dialog, "Peek inside" shows a list of contents in a popup. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: ZIP context menu"`.

> **Beats:** Windows Explorer ZIP integration is built-in. Linux requires right-click plugins (file-roller, ark). Impossible OS: in-kernel ZIP via context menu — no plugin daemon needed.

- [ ] *(Stretch)* Right-click file/selection → "Compress to ZIP" → `zip_create()`
- [ ] *(Stretch)* Right-click .zip → "Extract here" → `zip_extract(path, current_dir)`
- [ ] *(Stretch)* Right-click .zip → "Extract to..." → folder picker dialog → `zip_extract()`
- [ ] *(Stretch)* Right-click .zip → "Peek inside" → popup list from `zip_list()`
- [ ] Commit: `"desktop: ZIP context menu"`

---

## Priority Order

| Priority | Section                         | Reason                                         |
|----------|---------------------------------|------------------------------------------------|
| 🔴 P0    | §1 miniz Integration            | Foundation — all ZIP features depend on this   |
| 🟠 P1    | §2 ZIP API                      | Core kernel API for create/extract/list        |
| 🟠 P1    | §3 Shell Commands               | `zip`/`unzip` — immediate developer utility   |
| 🔵 P4    | §4 Context Menu (Stretch)       | GUI integration — requires context menu system |

---

## Key Files

| File                        | Purpose                            |
|-----------------------------|------------------------------------|
| `src/libs/miniz/miniz.c`    | [NEW] miniz compression library    |
| `src/libs/miniz/miniz.h`    | [NEW] miniz header                 |
| `src/kernel/zip.c`          | [NEW] ZIP API wrapper              |
| `include/kernel/zip.h`      | [NEW] ZIP API header               |

---

## OS Comparison

| Feature                          | Windows 11 (Explorer ZIP)          | Linux (libzip / python-zipfile)       | Impossible OS                           |
|----------------------------------|------------------------------------|---------------------------------------|-----------------------------------------|
| Create ZIP archive               | ✅ Explorer right-click             | ✅ `zip` command / libzip              | ⬜ §2-3 P0-P1 — `zip_create()` + shell |
| Extract ZIP archive              | ✅ Explorer right-click             | ✅ `unzip` / libzip                    | ⬜ §2-3 P0-P1 — `zip_extract()` + shell |
| List ZIP contents                | ✅ Explorer (navigate .zip)        | ✅ `unzip -l`                          | ⬜ §2-3 P1 — `zip_list()` + `unzip -l` |
| Single file extract              | ✅ Drag from Explorer              | ✅ `unzip archive.zip specific.file`   | ⬜ §2 P1 — `zip_extract_file()`        |
| Shell `zip`/`unzip` commands     | ❌ (PowerShell only)               | ✅ Native `zip`/`unzip` tools          | ⬜ §3 P1                               |
| Context menu compress/extract    | ✅ Explorer built-in               | ⚠️ Requires file-roller / ark plugin  | ⬜ §4 P4 (stretch) — **no plugin**     |
| .zip preview in file manager     | ✅ Explorer navigates inside .zip  | ✅ KDE Ark / Nautilus extension        | ⬜ §4 P4 (stretch)                     |
| **No external daemon for ZIP**   | ✅ In-shell                        | ✅ CLI tools                           | ✅ **§1 — in-kernel miniz, no daemon** |
