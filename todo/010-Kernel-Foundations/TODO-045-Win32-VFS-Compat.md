# Phase 06b — Win32 VFS Compatibility Layer

> **Goal:** Make the VFS and handle layer enforce Win32 filesystem semantics so that
> Win32 applications work correctly regardless of the underlying filesystem (IXFS,
> FAT32, NTFS, etc.). This is the "compatibility shim" between the Win32 file API
> (TODO-040 §3.6) and application expectations.
>
> **Two-phase strategy:**
> 1. **Phase A (this file):** Implement VFS-level compatibility — stub/spoof missing
>    features for FAT32 and other simple filesystems.
> 2. **Phase B (TODO-040 §5.9.5–5.9.9):** Implement features natively in IXFS so
>    the compat layer routes to real implementations on IXFS volumes.
>
> The end goal is that IXFS is a **superset of NTFS features** — ADS, ACLs, hard
> links, compression, xattrs — while the compat layer remains for FAT32/exFAT.

> [!CAUTION]
> **This is the #1 compatibility risk.** Win32 applications assume Windows-specific
> filesystem behaviors that differ from POSIX. Without this layer, apps will crash,
> corrupt data, or fail silently. Treat FAT32 as the minimum baseline — Windows
> itself runs Win32 apps from FAT32 volumes.

> [!IMPORTANT]
> **Dependencies:**
> - `TODO-040-Filesystem.md §3.6` — Win32 File API (`CreateFile`, `ReadFile`, etc.)
> - `TODO-040-Filesystem.md §3.5` — VFS driver interface (`vfs_ops`)
> - `TODO-028-Process-Model.md` — Per-process handle tables
> - `TODO-040-Filesystem.md §3.6.1` — Handle table and type definitions

---

## 1. Core Semantics (Deal-Breakers)

> These are **mandatory** for the vast majority of Win32 applications. Without
> these, apps will fail with `ERROR_FILE_NOT_FOUND`, corrupt databases, or crash
> during installation.

### 1.1 Case-Insensitive Path Resolution *(agent)*

**Prompt:** Win32 applications are notoriously sloppy with filename capitalization. An installer might write `SystemData.bin` but later call `CreateFileA("systemdata.BIN")`. Add a case-insensitive lookup mode to the VFS path resolution layer. The filesystem stores names as-written (case-preserving), but `vfs_finddir()` performs case-folded comparison. Implement `towupper_ascii()` for A–Z/a–z folding (no ICU needed — Win32 apps use ASCII names). Add a `VFS_LOOKUP_CASE_INSENSITIVE` flag that `CreateFile` always sets. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vfs: case-insensitive path resolution"`. Add notes directly in this TODO section.

- [ ] Implement `vfs_name_compare_ci(a, b)` — case-insensitive ASCII comparison
- [ ] Modify `vfs_finddir()` to accept a `flags` parameter
- [ ] Add `VFS_LOOKUP_CASE_INSENSITIVE` flag (default ON for Win32 API calls)
- [ ] When flag is set: iterate directory entries, compare via `vfs_name_compare_ci()`
- [ ] Return the first match (case-preserving — return the stored name, not the query)
- [ ] `CreateFile` passes `VFS_LOOKUP_CASE_INSENSITIVE` by default
- [ ] Test: create `TestFile.txt`, open via `TESTFILE.TXT` — must succeed
- [ ] Test: create `Hello.dll`, open via `hello.DLL` — must succeed
- [ ] Edge case: two files differing only by case (`readme.txt` vs `README.TXT`) — first-match wins (Windows behavior)
- [ ] Commit: `"vfs: case-insensitive path resolution"`

### 1.2 Mandatory File Locking (Share Modes) *(agent)*

**Prompt:** Windows uses **mandatory** file locking via `CreateFile`'s `dwShareMode` parameter. When a process opens a file with `FILE_SHARE_READ` but NOT `FILE_SHARE_WRITE`, any other process attempting to open the same file for writing must receive `ERROR_SHARING_VIOLATION (32)`. This is NOT advisory — the kernel must enforce it. Many applications (SQLite, Office, installers) rely on sharing violations for concurrency control. Add a per-file lock table that tracks open handles and their share modes. On each `CreateFile` call, check compatibility with existing opens. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vfs: mandatory file locking (share modes)"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> Without mandatory locking, SQLite databases running on Impossible OS will
> silently corrupt. This is critical for any app that uses file-based concurrency.

- [ ] Define share mode flags: `FILE_SHARE_READ (1)`, `FILE_SHARE_WRITE (2)`, `FILE_SHARE_DELETE (4)`
- [ ] Add `struct file_lock_entry` to track per-file open state:
  - [ ] `vfs_node*` — the file
  - [ ] `uint32_t access` — `GENERIC_READ`, `GENERIC_WRITE`
  - [ ] `uint32_t share_mode` — allowed concurrent access
  - [ ] `uint32_t handle_count` — number of open handles with this access/share combo
- [ ] Maintain a global or per-filesystem lock table (hash map by file ID)
- [ ] On `CreateFile`: check all existing opens for compatibility:
  - [ ] If any existing handle forbids this access → return `INVALID_HANDLE_VALUE`, `SetLastError(ERROR_SHARING_VIOLATION)`
  - [ ] If compatible → add entry, proceed
- [ ] On `CloseHandle`: remove entry, allow pending opens if now compatible
- [ ] Share mode compatibility matrix:
  ```
  Existing: SHARE_READ only   → New GENERIC_WRITE → DENIED
  Existing: SHARE_WRITE only  → New GENERIC_READ  → DENIED
  Existing: SHARE_READ|WRITE  → New GENERIC_READ  → OK
  Existing: 0 (exclusive)     → Any new open      → DENIED
  ```
- [ ] Test: open file exclusively, try to open again → `ERROR_SHARING_VIOLATION`
- [ ] Test: open file with `FILE_SHARE_READ`, open again for reading → OK
- [ ] Test: close first handle, re-open for writing → OK
- [ ] Commit: `"vfs: mandatory file locking (share modes)"`

### 1.3 Windows Deletion Semantics *(agent)*

**Prompt:** In traditional Windows, you **cannot delete a file that is currently open.** Even when opened with `FILE_SHARE_DELETE`, the file is only marked as "pending deletion" and is actually removed from the directory when the last handle closes. If your VFS behaves like Unix (immediate unlink of open files), installers that try to overwrite/delete files in use will break. Add a `pending_delete` flag to `vfs_node`, check open handle count before unlinking, and defer the actual directory entry removal to `CloseHandle`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vfs: Windows deletion semantics"`. Add notes directly in this TODO section.

- [ ] Add `pending_delete` flag to `vfs_node` (or handle entry)
- [ ] `DeleteFile()` behavior:
  - [ ] If file has open handles without `FILE_SHARE_DELETE` → `ERROR_SHARING_VIOLATION`
  - [ ] If file has open handles WITH `FILE_SHARE_DELETE` → mark `pending_delete = true`
  - [ ] If file has NO open handles → immediate delete via `ops->unlink()`
- [ ] `CloseHandle()` behavior:
  - [ ] If `pending_delete` is set AND this is the last handle → now call `ops->unlink()`
  - [ ] Reset `pending_delete` flag
- [ ] `CreateFile()` behavior:
  - [ ] If file is pending deletion → `ERROR_ACCESS_DENIED`
- [ ] `FindFirstFile()` / `FindNextFile()`:
  - [ ] Files marked `pending_delete` should still appear in directory listings (Windows behavior)
- [ ] Test: open file, try `DeleteFile` without `FILE_SHARE_DELETE` → `ERROR_SHARING_VIOLATION`
- [ ] Test: open file with `FILE_SHARE_DELETE`, `DeleteFile` → success (pending), close → actually deleted
- [ ] Commit: `"vfs: Windows deletion semantics"`

### 1.4 Unique File Identifiers *(agent)*

**Prompt:** Win32 applications use `GetFileInformationByHandle()` to retrieve `nFileIndexHigh` and `nFileIndexLow` — a 64-bit unique file ID (analogous to a Unix inode number). Programs use this to check if two different file handles (possibly opened via different paths, symlinks, or hard links) point to the same physical file. Your VFS nodes must generate consistent, unique IDs. For IXFS this maps directly to the inode number. For FAT32, synthesize an ID from the directory cluster + entry index (since FAT32 has no inodes). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vfs: unique file identifiers"`. Add notes directly in this TODO section.

- [ ] Add `uint64_t file_id` field to `vfs_node`
- [ ] IXFS: set `file_id = inode_number` (already unique)
- [ ] FAT32: synthesize `file_id = (dir_cluster << 32) | entry_offset_in_dir`
- [ ] NTFS: set `file_id = MFT_record_number`
- [ ] Implement `GetFileInformationByHandle(hFile, lpFileInformation)`:
  - [ ] Define `BY_HANDLE_FILE_INFORMATION` struct
  - [ ] Populate: `nFileIndexHigh`, `nFileIndexLow`, `dwFileAttributes`, `nFileSizeHigh`, `nFileSizeLow`, `ftCreationTime`, `ftLastWriteTime`, `nNumberOfLinks`
- [ ] Test: open same file via two different handles → same `nFileIndex`
- [ ] Test: open two different files → different `nFileIndex`
- [ ] Commit: `"vfs: unique file identifiers"`

### 1.5 Memory-Mapped Executable Loading *(agent)*

**Prompt:** The Windows PE Loader does NOT simply `ReadFile()` an `.exe` into memory. It memory-maps the executable and its DLLs using `CreateFileMapping()` and `MapViewOfFile()`. Page faults trigger demand-paging from disk — only the pages actually executed or accessed are read. Your VFS must integrate tightly with the VMM to handle file-backed page faults, streaming 4 KB pages from disk on demand. This is critical for large executables — without it, loading a 50 MB application requires 50 MB of upfront I/O. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vfs: memory-mapped file I/O"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-023-Virtual-Memory.md` — VMM page fault handler
> → XREF: `TODO-028-Process-Model.md` — PE loader
> → XREF: `TODO-040-Filesystem.md §3.6.3` — ReadFile (used as fallback)

- [ ] Implement `CreateFileMapping(hFile, ..., dwProtection, ...)` → create file mapping object
- [ ] Implement `MapViewOfFile(hMapping, dwDesiredAccess, offset, size)`:
  - [ ] Calculate: file offset → virtual address range
  - [ ] Mark pages as NOT PRESENT in page table (demand-paging)
  - [ ] Store mapping metadata: `{ vfs_node*, file_offset, length, prot }`
- [ ] Page fault handler integration:
  - [ ] On #PF for mapped range → call `ops->read(node, page_offset, 4096, page_buffer)`
  - [ ] Map physical page, mark PRESENT, return from fault
- [ ] Implement `UnmapViewOfFile(lpBaseAddress)` — unmap pages, flush dirty pages
- [ ] Implement `FlushViewOfFile(lpBaseAddress, dwSize)` — write dirty pages back to file
- [ ] Handle shared vs private mappings (`FILE_MAP_COPY` = copy-on-write)
- [ ] Test: map a file, read from mapped address → triggers page fault → reads from disk
- [ ] Test: map executable, execute code from mapped page → demand-loads
- [ ] Commit: `"vfs: memory-mapped file I/O"`

---

## 2. Feature Spoofing (NTFS Compatibility)

> Modern Win32 applications often query NTFS-specific features. Since the underlying
> filesystem may be IXFS or FAT32, the API layer must convincingly return harmless
> fallback values rather than crashing.

### 2.1 Alternate Data Streams (ADS) Handling *(agent)*

**Prompt:** Web browsers use NTFS Alternate Data Streams to append the "Mark of the Web" (`:Zone.Identifier`) to downloaded files. If an app tries to create `file.exe:Zone.Identifier` and the filesystem violently rejects the `:` character, browser downloads will fail. Implement a two-tier strategy: (1) For filesystems without stream support (FAT32, exFAT), silently discard stream data and return success — matching Windows-on-FAT32 behavior. (2) For IXFS, route to native ADS support (TODO-040 §5.9.5) once implemented. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vfs: ADS graceful fallback"`. Add notes directly in this TODO section.

> [!NOTE]
> **Long-term:** IXFS will support native ADS (TODO-040 §5.9.5). This section
> implements the VFS-level router that detects `:stream` syntax and dispatches
> to the right handler. Once §5.9.5 is done, IXFS volumes get real streams;
> FAT32 volumes continue using the discard fallback.

- [ ] Detect ADS path syntax: filename contains `:` followed by stream name (e.g., `file.exe:Zone.Identifier`)
- [ ] In `CreateFile`: check if filesystem supports streams via `vfs_ops.stream_open` callback:
  - [ ] If supported (IXFS, NTFS) → route to `ops->stream_open(node, stream_name)`
  - [ ] If NOT supported (FAT32, exFAT) → return a "null" handle that discards writes
  - [ ] `SetLastError(ERROR_SUCCESS)` — do NOT report an error in either case
- [ ] In `ReadFile` on discard-ADS handle: return 0 bytes read (empty stream)
- [ ] In `WriteFile` on discard-ADS handle: accept data, discard, return bytes written = requested
- [ ] In `DeleteFile` with ADS path: route to `ops->stream_delete()` or silently succeed
- [ ] Implement `FindFirstStreamW` / `FindNextStreamW`:
  - [ ] If FS supports streams → route to `ops->stream_enumerate()`
  - [ ] If not → return only `::$DATA` (the default unnamed stream)
  - [ ] Second call returns `ERROR_HANDLE_EOF`
- [ ] Test: `CreateFile("test.exe:Zone.Identifier", GENERIC_WRITE, ...)` → handle returned (not `INVALID_HANDLE_VALUE`)
- [ ] Test: `WriteFile` to ADS handle → reports success, data discarded
- [ ] Commit: `"vfs: ADS graceful fallback"`

### 2.2 Security Descriptor Routing (ACL Stubs + Native) *(agent)*

**Prompt:** Installers (especially MSI packages) call `SetFileSecurity()` to lock down directories with Access Control Lists. Implement a two-tier strategy: (1) For IXFS, route to native security descriptors (TODO-040 §5.9.6) once implemented — IXFS will persist real ACLs. (2) For FAT32/exFAT (no ACL support), silently return `ERROR_SUCCESS` with a dummy permissive descriptor — matching how Windows handles FAT32 volumes. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vfs: ACL routing (native + stub fallback)"`. Add notes directly in this TODO section.

> [!NOTE]
> **Long-term:** IXFS will support native ACLs (TODO-040 §5.9.6). This section
> implements the VFS-level router. Once §5.9.6 is done, IXFS volumes persist
> real security descriptors; FAT32 volumes use the permissive stub.

- [ ] Implement `SetFileSecurity(lpFileName, SecurityInformation, pSecurityDescriptor)`:
  - [ ] If filesystem supports ACLs (IXFS, NTFS) → `ops->set_security(node, descriptor)`
  - [ ] If filesystem lacks ACLs (FAT32, exFAT) → silently return `TRUE` (discard)
- [ ] Implement `GetFileSecurity(lpFileName, SecurityInformation, pSecurityDescriptor, nLength, lpnLengthNeeded)`:
  - [ ] If filesystem supports ACLs → `ops->get_security(node)` → real descriptor
  - [ ] If filesystem lacks ACLs → return dummy descriptor:
    - [ ] Owner: `BUILTIN\Administrators` SID
    - [ ] DACL: single ACE granting `GENERIC_ALL` to `Everyone` SID
    - [ ] Set `lpnLengthNeeded` to descriptor size
- [ ] Implement `GetSecurityInfo()` / `SetSecurityInfo()` (same routing strategy)
- [ ] Test: `SetFileSecurity` on IXFS → persisted and retrievable
- [ ] Test: `SetFileSecurity` on FAT32 → returns `TRUE`, no error, descriptor discarded
- [ ] Commit: `"vfs: ACL routing (native + stub fallback)"

### 2.3 Volume Information Spoofing *(agent)*

**Prompt:** Some DRM wrappers, anti-cheat engines, and enterprise apps call `GetVolumeInformation()` and check `lpFileSystemNameBuffer`. If they see an unknown filesystem name instead of "NTFS" or "FAT32", they may refuse to run. Report accurate filesystem names for known types (IXFS, FAT32, NTFS, exFAT), but set capability flags honestly — leave out `FILE_PERSISTENT_ACLS` if the filesystem doesn't support them, so well-behaved apps can adapt. Also implement `GetDiskFreeSpace()` and `GetDiskFreeSpaceEx()` for capacity queries. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vfs: GetVolumeInformation + disk space queries"`. Add notes directly in this TODO section.

- [ ] Implement `GetVolumeInformation(lpRootPathName, lpVolumeNameBuffer, nVolumeNameSize, lpVolumeSerialNumber, lpMaxComponentLength, lpFileSystemFlags, lpFileSystemNameBuffer, nFileSystemNameSize)`:
  - [ ] Set `lpFileSystemNameBuffer`:
    - [ ] IXFS volumes → `"IXFS"` (or optionally `"NTFS"` for maximum compat — configurable)
    - [ ] FAT32 volumes → `"FAT32"`
    - [ ] NTFS volumes → `"NTFS"`
    - [ ] exFAT volumes → `"exFAT"`
  - [ ] Set `lpMaxComponentLength` → `255` (all our filesystems support this)
  - [ ] Set `lpFileSystemFlags`:
    - [ ] `FILE_CASE_PRESERVED_NAMES` — always set
    - [ ] `FILE_UNICODE_ON_DISK` — set for IXFS and NTFS
    - [ ] `FILE_PERSISTENT_ACLS` — IXFS (once §5.9.6 done) and NTFS
    - [ ] `FILE_SUPPORTS_SPARSE_FILES` — IXFS only (if implemented)
  - [ ] Set `lpVolumeSerialNumber` from filesystem metadata
- [ ] Implement `GetDiskFreeSpace(lpRootPathName, lpSectorsPerCluster, lpBytesPerSector, lpNumberOfFreeClusters, lpTotalNumberOfClusters)`
- [ ] Implement `GetDiskFreeSpaceEx(lpDirectoryName, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes)`
- [ ] Test: `GetVolumeInformation("C:\\", ...)` → returns `"IXFS"`, correct flags
- [ ] Test: `GetDiskFreeSpaceEx("C:\\", ...)` → returns realistic values
- [ ] Commit: `"vfs: GetVolumeInformation + disk space queries"`

### 2.4 Hard Links & Reparse Point Routing *(agent)*

**Prompt:** Windows heavily relies on hard links for the WinSxS (Side-by-Side) assembly cache, used to load correct MSVC C++ runtimes. Implement a two-tier strategy: (1) For IXFS, route to native hard link and symlink support (TODO-040 §5.9.7). (2) For FAT32 (no hard link support), return `ERROR_INVALID_FUNCTION` so well-written apps fall back to a file copy. Implement `DeviceIoControl(FSCTL_SET_REPARSE_POINT, ...)` routing — native on IXFS (reparse points as symlinks), stub on FAT32. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vfs: hard links + reparse point routing"`. Add notes directly in this TODO section.

> [!NOTE]
> **Long-term:** IXFS supports native hard links and symlinks (TODO-040 §5.9.7).
> This section routes `CreateHardLink` and reparse point operations to the right
> handler. FAT32 returns `ERROR_INVALID_FUNCTION`.

- [ ] Implement `CreateHardLink(lpFileName, lpExistingFileName, lpSecurityAttributes)`:
  - [ ] IXFS: route to `ops->link(dir, name, target_inode)` (native §5.9.7)
  - [ ] FAT32: return `FALSE`, `SetLastError(ERROR_INVALID_FUNCTION)`
  - [ ] NTFS: create new filename attribute in MFT entry, increment link count
- [ ] Implement `CreateSymbolicLink(lpSymlinkName, lpTargetName, dwFlags)`:
  - [ ] IXFS: route to `ops->symlink(dir, name, target_path)` (native §5.9.7)
  - [ ] FAT32: return `FALSE`, `SetLastError(ERROR_INVALID_FUNCTION)`
- [ ] Update `GetFileInformationByHandle` to return `nNumberOfLinks` from inode
- [ ] Implement `DeleteFile` for hard-linked files: decrement `nlink`, only free data when `nlink == 0`
- [ ] Implement `DeviceIoControl(FSCTL_SET_REPARSE_POINT)`:  
  - [ ] IXFS: route to symlink creation
  - [ ] FAT32: return `ERROR_INVALID_FUNCTION`
- [ ] Implement `DeviceIoControl(FSCTL_GET_REPARSE_POINT)`:  
  - [ ] IXFS: read symlink target
  - [ ] FAT32: return `ERROR_NOT_A_REPARSE_POINT`
- [ ] Test: create hard link on IXFS → both paths reference same data
- [ ] Test: delete one link → other still accessible, data intact
- [ ] Test: delete last link → data freed
- [ ] Commit: `"vfs: hard links + reparse point stubs"`

---

## 3. Win32 Error Code Precision

> Win32 applications heavily rely on `GetLastError()` return values. Returning the
> wrong error code causes apps to take incorrect code paths.

### 3.1 Error Code Mapping Table *(agent)*

**Prompt:** Create a comprehensive mapping from internal VFS/filesystem error codes to exact Win32 error codes. Applications check specific error values to decide behavior — returning `ERROR_ACCESS_DENIED (5)` when `ERROR_SHARING_VIOLATION (32)` is the correct error will cause apps to fail in unexpected ways. Define all mappings in a central `win32_errors.h` and ensure every `SetLastError()` call in the file API uses the correct code. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vfs: precise Win32 error code mapping"`. Add notes directly in this TODO section.

- [ ] Create `include/kernel/fs/win32_errors.h` with all error code constants:
  ```c
  #define ERROR_SUCCESS                 0
  #define ERROR_FILE_NOT_FOUND          2
  #define ERROR_PATH_NOT_FOUND          3
  #define ERROR_ACCESS_DENIED           5
  #define ERROR_INVALID_HANDLE          6
  #define ERROR_NOT_ENOUGH_MEMORY      14
  #define ERROR_WRITE_PROTECT          19
  #define ERROR_SHARING_VIOLATION      32
  #define ERROR_LOCK_VIOLATION         33
  #define ERROR_HANDLE_EOF             38
  #define ERROR_NOT_SUPPORTED          50
  #define ERROR_FILE_EXISTS            80
  #define ERROR_INVALID_PARAMETER      87
  #define ERROR_DISK_FULL             112
  #define ERROR_INVALID_NAME          123
  #define ERROR_DIR_NOT_EMPTY         145
  #define ERROR_ALREADY_EXISTS        183
  #define ERROR_NO_MORE_FILES         259  /* FindNextFile end */
  #define ERROR_INVALID_FUNCTION        1
  #define ERROR_NOT_A_REPARSE_POINT  4390
  ```
- [ ] Implement `vfs_error_to_win32(int vfs_err)` — central mapping function
- [ ] Audit all `CreateFile`, `ReadFile`, `WriteFile`, `DeleteFile`, `MoveFile` paths for correct error codes
- [ ] Ensure `ERROR_SHARING_VIOLATION` (not `ERROR_ACCESS_DENIED`) for lock conflicts
- [ ] Ensure `ERROR_FILE_EXISTS` (not `ERROR_ALREADY_EXISTS`) for `CREATE_NEW` on existing file
- [ ] Ensure `ERROR_DIR_NOT_EMPTY` for `RemoveDirectory` on non-empty directory
- [ ] Test: specific error code assertions for each failure mode
- [ ] Commit: `"vfs: precise Win32 error code mapping"`

---

## Priority Order

| Priority | Section | Description | Rationale |
|----------|---------|-------------|-----------|
| 🔴 P0    | 1.1 Case-insensitive lookup | Mandatory for nearly all Win32 apps | Without this, most apps fail with FILE_NOT_FOUND |
| 🔴 P0    | 1.2 Mandatory file locking | Database corruption prevention | SQLite, Office, installers all depend on this |
| 🔴 P0    | 3.1 Error code mapping | Correct app behavior on errors | Wrong error codes → wrong app code paths |
| 🟡 P1    | 1.3 Deletion semantics | Installer compatibility | Installers overwrite/delete files in use |
| 🟡 P1    | 1.4 Unique file IDs | Application identity checks | Used by many apps to detect same-file |
| 🟡 P1    | 2.3 Volume information | App compatibility queries | Some apps refuse to run on unknown FS |
| 🟡 P2    | 1.5 Memory-mapped files | PE loader demand paging | Performance-critical for large executables |
| 🟡 P2    | 2.1 ADS handling | Browser download compat | Zone.Identifier must not crash |
| 🟡 P2    | 2.2 ACL stubs | Installer compat | MSI installers set permissions |
| 🟢 P3    | 2.4 Hard links / reparse | WinSxS / MSVC runtime compat | Needed when running VC++ redistributable apps |

---

## Key Files

| File | Purpose |
|------|---------|
| `src/kernel/fs/vfs.c` | Case-insensitive lookup, file lock table |
| `src/kernel/fs/fileapi.c` | CreateFile share mode enforcement, deletion semantics |
| `include/kernel/fs/win32_errors.h` | [NEW] Win32 error code constants |
| `include/kernel/fs/fileapi.h` | Handle type definitions, error codes |
| `src/kernel/fs/vol_info.c` | [NEW] GetVolumeInformation, GetDiskFreeSpace |
| `src/kernel/fs/security.c` | [NEW] ACL routing (native on IXFS, stub on FAT32) |
| `src/kernel/mm/mmap.c` | Memory-mapped file I/O |

---

## OS Comparison

| Feature                  | 🪟 Windows 11                    | 🐧 Linux                  | 🚀 Impossible OS                       |
| ------------------------ | ------------------------------- | ------------------------ | ------------------------------------- |
| Case-insensitive lookup  | ✅ Native (OBJ_CASE_INSENSITIVE) | ❌ Case-sensitive         | ⬜ §1.1 — VFS flag                     |
| Mandatory file locking   | ✅ dwShareMode enforced          | ❌ Advisory only (flock)  | ⬜ §1.2 — lock table                   |
| Deferred deletion        | ✅ pending_delete                | ❌ Immediate unlink       | ⬜ §1.3 — pending flag                 |
| File IDs (inode-like)    | ✅ nFileIndex                    | ✅ ino_t                  | ⬜ §1.4 — file_id                      |
| Memory-mapped I/O        | ✅ CreateFileMapping             | ✅ mmap                   | ⬜ §1.5 — demand paging                |
| ADS (streams)            | ✅ Native NTFS                   | ❌ No equivalent          | ⬜ §2.1 stub → TODO-040 §5.9.5 native  |
| ACL security descriptors | ✅ Full DACL/SACL                | ✅ POSIX ACLs (different) | ⬜ §2.2 stub → TODO-040 §5.9.6 native  |
| Volume info queries      | ✅ GetVolumeInformation          | ✅ statfs / statvfs       | ⬜ §2.3 — accurate reporting           |
| Hard links / symlinks    | ✅ CreateHardLink                | ✅ link() / symlink()     | ⬜ §2.4 route → TODO-040 §5.9.7 native |
| Extended attributes      | ✅ NtSetEaFile                   | ✅ setxattr               | ⬜ TODO-040 §5.9.8 native              |
| Transparent compression  | ✅ NTFS compression              | ✅ btrfs/zstd             | ⬜ TODO-040 §5.9.9 native              |
| Precise error codes      | ✅ 15,000+ distinct codes        | ✅ errno (limited set)    | ⬜ §3.1 — mapping table                |
