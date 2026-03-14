# P0011 — ZIP Compression

> **Goal:** ZIP archive creation, extraction, and shell/UI integration via miniz.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. miniz Integration

- [ ] Port **miniz** (MIT, single file, ~4000 lines) into `src/libs/miniz/`
- [ ] Redirect memory: `mz_malloc → kmalloc`, `mz_free → kfree`
- [ ] Commit: `"libs: miniz compression library"`

---

## 2. ZIP API

**Prompt:** The ZIP API wraps miniz into a simpler kernel API. `zip_create(path, files, count)` creates a new ZIP archive. `zip_extract(path, dest)` extracts all files to a destination. `zip_list(path, names, max)` lists contents. `zip_add_file(path, file)` appends a file. After completing all items, create `docs/architecture/zip.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: ZIP archive support"`.


- [ ] Create `include/zip.h` and `src/kernel/zip.c` (wrapper around miniz)
- [ ] Implement `zip_create(zip_path, files[], count)` — create ZIP from files
- [ ] Implement `zip_extract(zip_path, dest_dir)` — extract all files
- [ ] Implement `zip_list(zip_path, names, max)` — list archive contents
- [ ] Implement `zip_add_file(zip_path, file_path)` — add file to existing ZIP
- [ ] Commit: `"kernel: ZIP archive support"`

---

## 3. Shell & UI Integration

- [ ] Shell commands: `zip archive.zip file1 file2`, `unzip archive.zip`
- [ ] *(Stretch)* Right-click → "Compress to ZIP"
- [ ] *(Stretch)* Right-click `.zip` → "Extract here" / "Extract to..."
- [ ] Commit: `"shell: zip/unzip commands"`
