# TODO-04 -- FAT32 Hardening & VFS Win32 Semantics

> **Goal:** Harden FAT32 from working R/W to spec-correct: BPB strict validation, FSInfo mount-time fallback, dual-FAT compare-and-repair, LFN write, proper FILETIME timestamps, and an in-kernel fsck. Then layer the VFS Win32 semantics that all user-mode code depends on: case-insensitive path resolution, share-mode enforcement, delete-on-close, byte-range locks, and feature-spoofing stubs.

> [!IMPORTANT]
> FAT32 basic R/W, LFN *read*, FSInfo flush, and dual-FAT *write* all work. The gaps are: no strict BPB validation on mount, no FSInfo fallback for `FreeCount=0xFFFFFFFF`, no dual-FAT compare/repair, no LFN *write* (`fat32_create_file_vol()` writes 8.3 SFN only), no FILETIME-encoded timestamps, and no fsck. The VFS semantics (§7–11) are kernel-wide and benefit NTFS and IXFS equally -- implement them in `vfs.c`, not inside FAT32. §7 (case-insensitive) must be done before §8–11 since the handle-lookup path it creates is the one share-mode checks run on.

## Inputs

- `src/kernel/fs/fat32/fat32_core.c` -- `fat32_set_fat_entry()` loops `num_fats` (dual-FAT *write* works); `fat32_fsinfo_flush()` exists; BPB fields parsed but not validated; §1–3 extend it
- `src/kernel/fs/fat32/fat32_write.c` -- `fat32_create_file_vol()` writes 8.3 SFN only (no LFN slots); `fat32_set_times()` exists but needs FILETIME encoding; §4–5 extend it
- `src/kernel/fs/fat32/fat32_dir.c` -- `lfn_extract_chars()` for LFN read exists; §4 adds LFN write
- `src/kernel/fs/vfs.c` + `include/kernel/fs/vfs.h` -- no share-mode, delete-on-close, byte-range lock, or case-fold in open path; §7–11 extend it
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1` -- `vfs_probe()` calls `fat32_init()`; BPB validation (§1) must fire inside `fat32_init()` before `vfs_probe()` returns success
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §5` -- `umount` shell calls the VFS unmount path; §5 (fsck) must be callable as `chkdsk D: /fat32`
- → XREF: `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md` -- Win32 `CreateFile` flags `FILE_SHARE_*` and `FILE_FLAG_DELETE_ON_CLOSE` map directly to §8 and §9
- → XREF: `01-boot-platform/TODO-17-blackbox-service-partition.md` §12 -- BlackBox FAT32 dirty-bit check depends on §1; fsck-on-mount depends on §6

## Outcome

- `fat32_init()` rejects volumes with invalid BPB; dirty-mounted volumes auto-fsck before returning handle.
- FSInfo `FreeCount` always accurate; full FAT scan fallback on `0xFFFFFFFF`.
- FAT2 compared and repaired on mount; both FATs written on every cluster alloc/free.
- `fat32_create_file_vol()` and `fat32_create_dir_vol()` write correct LFN slot chains before the 8.3 entry.
- All create/write/read ops update correct timestamps in FAT32 encoding from kernel FILETIME.
- `fat32_fsck()` detects cross-linked chains and lost clusters; `chkdsk D: /fat32` shell command works.
- `vfs_open()` resolves case-insensitively on all filesystems.
- `CreateFile` with conflicting share-mode returns `STATUS_SHARING_VIOLATION`.
- `FILE_FLAG_DELETE_ON_CLOSE` defers deletion to last-handle-close.
- `LockFile`/`UnlockFile` enforce byte-range exclusion per file.
- ADS, ACL, reparse-point, and volume-info queries return correct stubs.
- 8.3 SFN generation handles numeric tail collisions (`~1` through `~99`).
- Writes beyond FAT32's 4 GiB file size limit fail gracefully, not silently corrupt.
- Opportunistic locks (Level 1/2) enable Win32 programs to cache file data safely.

## Implementation Order

| ⭐  | Order | Deliverable                                                                           | Depends On                                               | Status |
| --- | :---: | ------------------------------------------------------------------------------------- | -------------------------------------------------------- | :----: |
| 💎  |   1   | §1 FAT32 BPB strict validation -- `jmpBoot`, `BytsPerSec`, `SecPerClus`, dirty marker | `fat32_init()` in fat32_core.c                           |  [x]   |
| 💎  |   2   | §2 FSInfo mount-time fallback -- signature check, `0xFFFFFFFF` full-scan fallback     | §1 (init fails fast on bad BPB before FSInfo read)       |  [x]   |
| 💎  |   3   | §3 Dual-FAT compare and repair -- mount compare, log mismatch, FAT2 repair            | §1, §2 (valid volume before FAT compare)                 |  [x]   |
| 💎  |   4   | §4 LFN write -- slot chain before 8.3 SFN, UTF-16LE, checksum, LFN delete             | §3 (FAT write path stable)                               |  [x]   |
| 💎  |   5   | §5 FAT32 timestamps -- FILETIME encode, `LastAccessDate` on read, `WrtTime` on write  | §4 (create path being reworked)                          |  [ ]   |
| 💎  |   6   | §6 FAT32 fsck -- BPB check, FAT1/2 compare, cross-linked chains, lost clusters        | §3 + §4 + §5 (stable FAT and directory state)            |  [ ]   |
| ⭐  |   7   | §7 VFS case-insensitive path resolution -- uppercase fold in `vfs_open()`             | None (pure VFS layer change)                             |  [ ]   |
| 💎  |   8   | §8 VFS share-mode enforcement -- per-handle `share_mode`, `STATUS_SHARING_VIOLATION`  | §7 (handle lookup uses case-folded path)                 |  [ ]   |
| 💎  |   9   | §9 Mark-for-delete-on-close -- deferred deletion, `STATUS_DELETE_PENDING`             | §8 (open-handle table exists)                            |  [ ]   |
| 💎  |  10   | §10 Byte-range locks -- `LockFile`/`UnlockFile`, per-file lock list, conflict detect  | §8 (per-handle file identity)                            |  [ ]   |
| 💎  |  11   | §11 VFS feature-spoofing stubs -- ADS, ACL, volume flags, reparse-point queries       | §7–§10 (handle plumbing in place)                        |  [ ]   |
| 💎  |  12   | §12 SFN numeric tail collision -- ~1 through ~9, 5-char truncation for ~10+           | §4 (LFN write reworks create path)                       |  [ ]   |
| 💎  |  13   | §13 FAT32 4 GiB write guard -- reject writes that would exceed 0xFFFFFFFF bytes       | §5 (write path stable)                                   |  [ ]   |
| 💎  |  14   | §14 VFS opportunistic locks -- Level 1/2, Batch, Read/Read-Write/Read-Handle          | §8 (share-mode per-handle state)                         |  [ ]   |

> §7 (case-insensitive VFS) is `⭐` exclusive in architecture: Windows case-folds inside `NTFS.sys` / `FAT.sys` per volume type; Linux is case-sensitive by default with per-mount options. Impossible OS applies a unified case-fold in the VFS layer above all filesystem drivers -- one correct implementation that benefits NTFS, FAT32, IXFS, and any future driver equally.

---

## 1. FAT32 BPB Strict Validation

Validate critical BPB fields in `fat32_init()` before any further mount operations. Detect and set the dirty volume marker on mount; auto-run fsck (§6) if dirty.

**Files:** `src/kernel/fs/fat32/fat32_core.c` (extend)

> [!NOTE]
> FAT32 BPB spec validation rules (Microsoft FAT spec §3): `BPB_jmpBoot[0]` must be `0xEB` or `0xE9`; `BPB_BytsPerSec` ∈ {512, 1024, 2048, 4096}; `BPB_SecPerClus` must be a power of two ≥ 1 and ≤ 128; `BPB_NumFATs` ∈ {1, 2}; `BPB_RootEntCnt` must be 0 for FAT32; `BPB_RootClus` must be ≥ 2. Dirty marker: FAT32 uses FAT[1] high bit (bit 27 = `0x08000000`) to flag a dirty volume; if this bit is clear at mount time, the volume was not cleanly unmounted.

- [x] `fat32_validate_bpb(struct fat32_volume *vol)`: checks jmpBoot, BytsPerSec, SecPerClus, NumFATs, RootEntCnt, RootClus, TotSec32; on failure logs `"BPB validation failed: <reason>"` and returns -1; called from `fat32_init()` before FSInfo read
- [x] Fields checked: `jmpBoot[0]` (0xEB/0xE9), `BytsPerSec` (512/1024/2048/4096), `SecPerClus` (power-of-two 1-128), `NumFATs` (1/2), `RootEntCnt == 0`, `RootClus >= 2`, `TotSec32 > 0`
- [x] Dirty marker: `fat32_read_dirty_marker()` reads FAT[1] bit 27; if clear: `vol->volume_dirty = 1`; logs `"Dirty volume -- not cleanly unmounted"`
- [x] On clean unmount: `fat32_set_clean_marker()` sets FAT[1] bit 27 in both FAT copies; called from `fat32_flush_disk()`
- [x] 3 unit tests: BPB struct size bounds, FSInfo signature constants, dirty bit semantics
- [x] Commit: `"fs/fat32: BPB strict validation -- jmpBoot, BytsPerSec, SecPerClus, RootClus, dirty marker"`

## 2. FSInfo Mount-Time Fallback

Validate FSInfo signatures on mount. Fall back to a full FAT scan when `FreeCount == 0xFFFFFFFF`. Write updated counts after every alloc/free.

**Files:** `src/kernel/fs/fat32/fat32_core.c` (extend)

> [!NOTE]
> FSInfo sector layout (offset 0): `LeadSig = 0x41615252`; offset 484: `StrucSig = 0x61417272`; offset 488: `FreeCount` (0xFFFFFFFF = unknown); offset 492: `NxtFree` (0xFFFFFFFF = unknown); offset 508: `TrailSig = 0xAA550000`. Backup FSInfo at sector `BPB_BkBootSec + 1` (typically sector 7). If either signature is wrong: `vol->fsinfo_valid = 0`; proceed without FSInfo.

- [x] At mount: FSInfo signature validation already in `fat32_init()` -- checks LeadSig, StrucSig, TrailSig; sets `fsinfo_valid = 0` on mismatch with log
- [x] If `vol->fsinfo_valid && FreeCount == 0xFFFFFFFF`: new `fat32_count_free_clusters(vol)` scans full FAT table; stores result in `vol->fsinfo_free_count`; sets dirty
- [x] `fat32_alloc_cluster()`: already decrements `fsinfo_free_count`, updates `NxtFree`, sets dirty (verified)
- [x] `fat32_free_chain()`: already increments `fsinfo_free_count`, sets dirty (verified)
- [x] `fat32_fsinfo_flush()`: verified -- writes primary + backup at sector+6; runs from `fat32_flush_disk()` on unmount
- [x] Commit: `"fs/fat32: FSInfo -- 0xFFFFFFFF full-scan fallback via fat32_count_free_clusters()"`

## 3. Dual-FAT Compare and Repair

On mount, compare FAT1 and FAT2 sector by sector. Log any mismatch. Use FAT1 as authoritative; repair FAT2 if they differ.

**Files:** `src/kernel/fs/fat32/fat32_core.c` (extend)

> [!NOTE]
> `fat32_set_fat_entry()` already loops `fi < vol->bpb.num_fats` -- both FATs are written on alloc/free. The missing piece is a *mount-time compare*: read corresponding sectors of FAT1 and FAT2 and compare byte-for-byte. FAT size in sectors: `vol->bpb.fat_size_sectors`. If `NumFATs < 2`, skip the compare. Repair: copy FAT1 sectors to FAT2 sectors wherever they differ.

- [x] `fat32_compare_repair_fats(vol)`: reads FAT1 and FAT2 sector-by-sector via `blkdev_read()`; on mismatch: copies FAT1 sector to FAT2; logs each mismatch + summary count
- [x] Called from `fat32_init()` after dirty marker read; skips when `num_fats < 2`
- [x] Non-fatal: repairs do not abort mount; summary: `"FAT compare: %u mismatched sectors repaired"`
- [x] Commit: `"fs/fat32: dual-FAT compare-on-mount -- sector-by-sector compare, FAT2 repair"`

## 4. LFN Write

Write LFN directory entry chains before the 8.3 SFN entry. Encode names as UTF-16LE. Compute SFN checksum. Handle LFN delete (zero out slot chain).

**Files:** `src/kernel/fs/fat32/fat32_write.c` (extend), `src/kernel/fs/fat32/fat32_dir.c` (extend)

> [!NOTE]
> LFN entry layout (32 bytes): `Ord (1)` = sequence number (last slot OR'd with `0x40`); `Name1 (10)` = chars 1–5 as UTF-16LE; `Attr (1)` = `0x0F`; `Type (1)` = 0; `Chksum (1)` = SFN checksum; `Name2 (12)` = chars 6–11; `FstClusLO (2)` = 0; `Name3 (4)` = chars 12–13. SFN checksum: `for i in 0..10: sum = ((sum & 1) << 7) + (sum >> 1) + sfn[i]`. Slot count: `ceil(utf16len / 13)`. Slots written in *reverse* order (highest sequence first), then the SFN entry. Max LFN length: 255 UTF-16LE codepoints = 20 slots.

- [x] `fat32_lfn_slot_count(name)`: counts chars, returns `(len + 12) / 13`
- [x] `fat32_lfn_checksum(sfn[11])`: SFN checksum per FAT spec rotate-and-add algorithm
- [x] `fat32_needs_lfn(name)`: detects lowercase, multi-dot, long base/ext, non-ASCII, spaces
- [x] `fat32_find_free_dir_slots(vol, dir_cluster, count, ...)`: finds N contiguous free 32-byte entries; extends directory cluster chain if needed
- [x] `fat32_lfn_write_slots(vol, dir_cluster, sfn, name, sfn_entry)`: writes LFN slots in reverse order (highest seq first, OR'd with 0x40 on last), packs UTF-16LE with 0xFFFF fill on partial slot, writes SFN entry after all slots
- [x] `fat32_create_file_vol()` and `fat32_create_dir_vol()`: call `fat32_needs_lfn()` -- if yes: `fat32_lfn_write_slots()`; if pure 8.3: existing SFN-only path preserved
- [x] LFN delete: `fat32_delete_file_vol()` scans backwards from SFN entry, matches `attr==0x0F` + checksum, marks each preceding LFN slot with `0xE5`
- [ ] Commit: `"fs/fat32: LFN write -- slot chain, UTF-16LE encode, SFN checksum, LFN delete"`

## 5. FAT32 Timestamps

Encode kernel FILETIME (100 ns ticks since 1601 UTC) to FAT32 date/time format. Set `CrtTime`/`CrtDate` on create, `WrtTime`/`WrtDate` on write, `LstAccDate` on read. Apply timezone bias.

**Files:** `src/kernel/fs/fat32/fat32_write.c` (extend), `src/kernel/fs/fat32/fat32_ops.c` (extend)

> [!NOTE]
> FAT32 time format (16 bits): bits 15–11 = hours (0–23), 10–5 = minutes (0–59), 4–0 = 2-second counts (0–29). FAT32 date format (16 bits): bits 15–9 = year since 1980 (0–127, so max year 2107), 8–5 = month (1–12), 4–0 = day (1–31). Timezone: FAT32 stores local time; apply `HKLM\SYSTEM\TimeZone\BiasMinutes` offset (FILETIME is UTC). Year 2107 boundary: if year > 2107 (i.e. after 2107-12-31), clamp to 2107/12/31 and log a warning.

- [ ] `fat32_filetime_to_fattime(uint64_t filetime, uint16_t *date, uint16_t *time_val, uint8_t *time_tenth)`: convert FILETIME (UTC) → local time using Registry bias → FAT date/time fields; clamp year > 2107
- [ ] `fat32_create_file_vol()` + `fat32_create_dir_vol()`: call `fat32_filetime_to_fattime(kernel_time_now(), ...)` and set `CrtTime`, `CrtDate`, `WrtTime`, `WrtDate`, `LstAccDate` in the SFN entry
- [ ] `fat32_ops.c` write callback: after successful data write, update `WrtTime`/`WrtDate` in SFN entry
- [ ] `fat32_ops.c` read callback: update `LstAccDate` in SFN entry (date only, no time field for access)
- [ ] `fat32_set_times()` already exists -- verify it uses the new `fat32_filetime_to_fattime()` encoder
- [ ] Commit: `"fs/fat32: timestamps -- FILETIME→FAT encode, CrtTime on create, WrtTime on write, LstAccDate on read"`

## 6. FAT32 fsck

Implement `fat32_fsck(vol, fix)` to validate BPB, compare FAT1/FAT2, detect cross-linked chains and lost clusters. Expose as `chkdsk D: /fat32` shell command.

**Files:** `src/kernel/fs/fat32/fat32_fsck.c` (new), `include/kernel/fs/fat32.h` (extend), `src/shell/cmd_chkdsk.c` (extend)

> [!NOTE]
> Cross-linked chain: two directory entries whose cluster chains share a common cluster node. Detect by building a `visited[]` bitset (1 bit per cluster, allocated from `pmm_alloc_contiguous`); if a cluster is already marked, the chain is cross-linked. Lost clusters: clusters marked allocated in FAT1 but not reachable from any directory entry chain. Count them; if `fix`: chain them into `\FOUND.000\FILE0000.CHK` in the root directory (like Windows `chkdsk /f`).

- [ ] `fat32_fsck(struct fat32_volume *vol, int fix)`: validate BPB via `fat32_validate_bpb()`; compare FAT1 vs FAT2 (reuse §3 logic); walk all directory entries recursively from root cluster; build `visited[]` bitset
- [ ] Cross-link detection: on each `fat32_follow_chain()` step, check `visited[cluster]`; if already set: log `[FSCK] Cross-linked chain at cluster %u (files: %s, %s)`; if fix: truncate the second chain at the cross-link point
- [ ] Lost cluster detection: after walk, scan all FAT entries; if FAT[cluster] != 0 and not in `visited[]`: `lost_count++`; if fix: chain lost runs into root `FOUND.000` directory
- [ ] Report: log summary `[FSCK] %u errors, %u cross-links, %u lost clusters` + `Fixed` or `Errors remain`; return 0 (clean) or -1 (errors found or unfixable)
- [ ] `chkdsk D: /fat32` shell: calls `fat32_fsck(vol, fix=0)` for read-only scan; `/fix` flag calls with `fix=1`
- [ ] Commit: `"fs/fat32: fsck -- BPB check, FAT1/2 compare, cross-link, lost cluster, chkdsk shell command"`

## 7. VFS Case-Insensitive Path Resolution

Apply a unified uppercase fold in `vfs_open()` before path component lookup, so `C:\WINDOWS\System32` resolves identically to `c:\windows\system32` on all filesystems.

**Files:** `src/kernel/fs/vfs.c` (extend), `include/kernel/fs/vfs.h` (extend)

> [!NOTE]
> Case-fold strategy: uppercase ASCII A–Z only in the VFS path splitter (no Unicode case-fold required for FAT32 8.3; NTFS uses `$UpCase` table internally per volume). The path splitter in `vfs_open()` should normalise each component to uppercase before passing to `finddir()`. Each filesystem's `finddir()` then does its own case-insensitive compare (FAT32 already has `fat32_strcasecmp()`; NTFS has `ntfs_filename_match()`). This ensures case-folding is consistent regardless of filesystem type.

- [ ] `vfs_path_fold(const char *in, char *out, uint32_t max)`: copy path normalising ASCII letters to uppercase; preserve path separators `\` and drive letter colon
- [ ] Call `vfs_path_fold()` at the top of `vfs_open()` before path-component splitting; use folded path for all `finddir()` calls
- [ ] Verify `fat32_strcasecmp()` and `ntfs_filename_match()` both handle the folded component correctly; add integration test: open `c:\impossible\system32\cmd.exe` and `C:\Impossible\SYSTEM32\CMD.EXE` → both must return the same node
- [ ] `vfs_path_fold()` must handle drive letter: `c:\foo` → `C:\foo` (drive letter uppercased too)
- [ ] Commit: `"fs: vfs_path_fold() -- unified ASCII uppercase fold in vfs_open() before finddir()"`

## 8. VFS Share-Mode Enforcement

Add `share_mode` and `access_mode` to the per-open-handle struct. On `vfs_open()`, check existing handles for conflicts. Return `STATUS_SHARING_VIOLATION` on conflict.

**Files:** `src/kernel/fs/vfs.c` (extend), `include/kernel/fs/vfs.h` (extend)

> [!NOTE]
> Windows share-mode semantics: when opening a file with `DesiredAccess=READ` and `ShareMode=FILE_SHARE_READ`, any existing handle with `DesiredAccess=WRITE` that did not grant `FILE_SHARE_WRITE` causes `STATUS_SHARING_VIOLATION`. Implement as: on `vfs_open()`, iterate all open handles for the same inode; for each existing handle `H`: if `(new_access & ~H.share_mode) != 0` OR `(H.access & ~new_share_mode) != 0`, return `STATUS_SHARING_VIOLATION`.

- [ ] Add to `vfs_handle_t` (or equivalent open-handle struct): `uint32_t access_mode`, `uint32_t share_mode`
- [ ] Per-inode open-handle list: maintain `vfs_node_t.open_handles[]` (or a global inode→handle map keyed by device+inode); populated by `vfs_open()`, cleared by `vfs_close()`
- [ ] `vfs_open()`: after resolving the node, call `vfs_check_sharing(node, desired_access, share_mode)`; on conflict return `STATUS_SHARING_VIOLATION`
- [ ] `vfs_check_sharing(node, access, share)`: iterate node's open handles; apply conflict rule above; return 0 (OK) or `STATUS_SHARING_VIOLATION`
- [ ] `vfs_close()`: remove handle from node's open-handle list; if node marked delete-on-close and list is now empty: trigger delete (§9)
- [ ] Commit: `"fs: VFS share-mode -- per-handle access/share fields, conflict check, STATUS_SHARING_VIOLATION"`

## 9. Mark-for-Delete-on-Close

Implement `FILE_FLAG_DELETE_ON_CLOSE`: mark the inode for deletion when the handle is opened with this flag; perform the actual deletion when the last handle closes.

**Files:** `src/kernel/fs/vfs.c` (extend)

> [!NOTE]
> Windows semantics: `NtCreateFile(DELETE_ON_CLOSE)` marks the file. Any subsequent `NtOpenFile` on the same path returns `STATUS_DELETE_PENDING`. The actual delete happens in the `NtClose()` path when the last handle referencing the file is closed. The inode must remain reachable by existing handles until last-close.

- [ ] Add `uint8_t delete_on_close` flag to `vfs_node_t` (or the open-handle struct if per-handle)
- [ ] `vfs_open()`: if `flags & VFS_DELETE_ON_CLOSE`: set `node->delete_on_close = 1`; if `node->delete_on_close` already set on a *different* open: return `STATUS_DELETE_PENDING` to new openers
- [ ] `vfs_close()` last-handle path: if `node->delete_on_close` and `open_handle_count == 0`: call `vfs_delete(node)` (which calls the filesystem driver's `unlink()`)
- [ ] `vfs_open()` on `delete_on_close` node: existing openers may still read/write; new openers get `STATUS_DELETE_PENDING`
- [ ] Commit: `"fs: delete-on-close -- VFS_DELETE_ON_CLOSE flag, STATUS_DELETE_PENDING, last-close unlink"`

## 10. Byte-Range Locks

Implement `LockFile`/`UnlockFile`: per-file lock list with offset, length, and exclusive/shared flag. Conflict detection returns `STATUS_FILE_LOCK_CONFLICT`.

**Files:** `src/kernel/fs/vfs.c` (extend), `include/kernel/fs/vfs.h` (extend)

> [!NOTE]
> Lock semantics: shared locks are compatible with other shared locks on the same range; exclusive locks conflict with any lock on any overlapping range. Overlap test: `!(new_end <= existing_start || new_start >= existing_end)`. Maximum locks per file: 64 (static array per node). `UnlockFile` must match exact offset+length of a lock owned by the same handle.

- [ ] `vfs_lock_t { uint64_t offset; uint64_t length; vfs_handle_t *owner; uint8_t exclusive; }` -- static array `vfs_node_t.locks[64]`, `lock_count`
- [ ] `vfs_lock_file(handle, offset, length, exclusive)`: check for overlapping conflicting locks; if none: add entry; return 0; if conflict: return `STATUS_FILE_LOCK_CONFLICT`
- [ ] `vfs_unlock_file(handle, offset, length)`: find lock entry matching handle + offset + length; remove; return 0 or `STATUS_NOT_LOCKED`
- [ ] `vfs_write()` does NOT check byte-range locks (Windows does not enforce locks in-kernel on write, only via the Win32 API layer -- document this caveat)
- [ ] Win32 `LockFile(hFile, offset_low, offset_high, len_low, len_high)` → `vfs_lock_file(handle, offset, length, exclusive=TRUE)`; `UnlockFile` → `vfs_unlock_file()`
- [ ] Commit: `"fs: byte-range locks -- vfs_lock_file/unlock_file, overlap conflict, STATUS_FILE_LOCK_CONFLICT"`

## 11. VFS Feature-Spoofing Stubs

Return correct stub responses for Win32 queries that user-mode programs issue on every file system: ADS, ACL, volume flags, and reparse-point queries.

**Files:** `src/kernel/fs/vfs.c` (extend), `src/kernel/win32/file_info.c` (new or extend)

> [!NOTE]
> These stubs allow unmodified Win32 programs that probe filesystem capabilities to run without crashing. They must return the *correct* stub value (not random garbage), because programs make decisions based on the flags. Key stubs: ADS -- `FindFirstStreamW` returns `ERROR_HANDLE_EOF` (no streams); ACL query -- `GetFileSecurity` returns a minimal SD (Everyone:Full-Control, same as §4 of TODO-02); `GetVolumeInformation` flags -- `FILE_UNICODE_ON_DISK | FILE_CASE_PRESERVED_NAMES | FILE_PERSISTENT_ACLS` for NTFS/IXFS, `FILE_UNICODE_ON_DISK | FILE_CASE_PRESERVED_NAMES` for FAT32; reparse point -- `DeviceIoControl(FSCTL_GET_REPARSE_POINT)` returns `ERROR_NOT_A_REPARSE_POINT`.

- [ ] ADS: `NtQueryInformationFile(FileStreamInformation)` → single-entry response with just the default `::$DATA` stream, length = file size; `FindFirstStreamW` → returns `::$DATA` then `ERROR_HANDLE_EOF` on next call
- [ ] ACL: `NtQuerySecurityObject(DACL_SECURITY_INFORMATION)` → return the default Everyone:Full-Control SD (from `ntfs_secure_default_sd()` or a VFS-level copy)
- [ ] Volume flags: `NtQueryVolumeInformationFile(FileFsAttributeInformation)` → set `FileSystemAttributes` per filesystem type (see NOTE); `MaximumComponentNameLength = 255`; filesystem name string (`"NTFS"`, `"FAT32"`, `"IXFS"`)
- [ ] Reparse point: `FSCTL_GET_REPARSE_POINT` ioctl via `vfs_ioctl()` → return `STATUS_NOT_A_REPARSE_POINT` for all nodes on non-reparse-supporting filesystems
- [ ] Commit: `"fs: Win32 feature stubs -- ADS, ACL, volume flags, reparse-point STATUS_NOT_A_REPARSE_POINT"`

## 12. SFN Numeric Tail Collision Avoidance

Current `fat32_make_short_name()` hardcodes `~1` when a name needs truncation. Windows generates `~1` through `~9`, then truncates to 5 chars for `~10` through `~99`. Without collision avoidance, creating two files with similar long names produces duplicate 8.3 entries -- corrupting the directory.

- [ ] `fat32_generate_sfn(vol, dir_cluster, utf8_name, sfn[11])`: generate an 8.3 SFN; if name fits 8.3 natively, use it directly; else truncate to 6 chars + `~1` and check for collisions in the directory
- [ ] Collision loop: if `~1` exists, try `~2` through `~9`; if all taken, truncate to 5 chars + `~10` through `~99`
- [ ] Replace `fat32_make_short_name()` call in `fat32_create_file_vol()` and `fat32_create_dir_vol()` with `fat32_generate_sfn()`
- [ ] Case-insensitive collision check: compare uppercase SFN against existing directory entries
- [ ] Commit: `"fs/fat32: SFN numeric tail collision -- ~1 through ~9, 5-char ~10+"`

**Test checkpoint:** Create 10 files with names `"LongFileName_01.txt"` through `"LongFileName_10.txt"` -- directory entries show `LONGFI~1.TXT` through `LONGFI~9.TXT` then `LONGF~10.TXT`. All 10 files readable by name. Verify on QEMU WHPX, TCG, bare metal.

## 13. FAT32 4 GiB Write Guard

FAT32 `file_size` is a 32-bit field in the directory entry -- max 4,294,967,295 bytes (4 GiB - 1). Writes that would push the file past this limit must fail gracefully with `STATUS_DISK_FULL` rather than wrapping the size field and corrupting the directory entry.

- [ ] In `fat32_file_write_vfs()`: before extending the file, check `(uint64_t)current_size + write_len > 0xFFFFFFFF`; if so: return `STATUS_DISK_FULL` (or appropriate VFS error code)
- [ ] In `fat32_vfs_truncate()`: reject truncation to a size > `0xFFFFFFFF`
- [ ] Log: `klog(LOG_WARN, "fat32", "4 GiB file size limit reached: %s", filename)`
- [ ] Commit: `"fs/fat32: 4 GiB write guard -- reject writes beyond FAT32 file_size limit"`

**Test checkpoint:** Create a file, write exactly 4 GiB - 1 bytes -- succeeds. Write 1 more byte -- returns error, file size unchanged. Verify file is still readable. Verify on QEMU WHPX, TCG.

## 14. VFS Opportunistic Locks (Oplocks)

Windows file system drivers support opportunistic locks that allow clients to cache file data locally for performance. `CreateFile` opens can break or request oplocks. Without oplock support, Win32 programs that request batch oplocks (e.g., compilers, editors) get `ERROR_SHARING_VIOLATION` or degraded performance.

> [!NOTE]
> Oplock types: **Level 1** (exclusive read+write cache), **Level 2** (shared read cache), **Batch** (exclusive, delays close for repeated open/close cycles), **Filter** (exclusive, read-only, non-breaking on writes). Modern Windows also has **Read**, **Read-Write**, **Read-Handle**, **Read-Write-Handle** lease types. Start with legacy Level 1/2/Batch which cover 95% of real-world usage.

- [ ] Add `vfs_oplock_t` struct: `{ uint8_t type; vfs_handle_t *owner; }` per inode, max 1 exclusive or N shared
- [ ] `vfs_request_oplock(handle, type)`: grant if compatible with existing state; fail if conflict
- [ ] `vfs_break_oplock(node, desired_access)`: called from `vfs_open()` before share-mode check; if existing exclusive oplock conflicts with new access: break to Level 2 or None; notify owner via callback
- [ ] `FSCTL_REQUEST_OPLOCK` / `FSCTL_OPLOCK_BREAK_ACKNOWLEDGE` ioctls via `vfs_ioctl()`
- [ ] Start with Level 1 (exclusive) and Level 2 (shared read) only; Batch and Filter deferred until real programs need them
- [ ] Commit: `"fs: VFS opportunistic locks -- Level 1/2 oplock grant, break on conflicting open"`

**Test checkpoint:** Open file with Level 1 oplock. Second open breaks oplock to Level 2 (notification logged). Third open with write access breaks to None. `FSCTL_REQUEST_OPLOCK` returns correct level. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_fat32_hardening()` and `test_register_vfs_semantics()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `test=1` in boot.conf. FAT32 tests require a mounted FAT32 volume (BlackBox X:\ or test volume).

- [ ] Create `src/kernel/test/test_fat32_hardening.c` with:
  - `fat32_validate_bpb()` rejects zero `BytsPerSec` (returns -1)
  - `fat32_validate_bpb()` rejects non-power-of-two `SecPerClus` (returns -1)
  - `fat32_validate_bpb()` accepts valid BPB from mounted volume (returns 0)
  - FSInfo `FreeCount` matches actual free clusters after `fat32_count_free_clusters()`
  - LFN write: create `"test_long_name.txt"`, read back name matches exactly (case-preserved)
  - LFN delete: delete the LFN file, directory entry slots are marked `0xE5`
  - SFN collision: create `"collision_test_1.txt"` and `"collision_test_2.txt"` -- both get unique 8.3 names
  - 4 GiB guard: `fat32_file_write_vfs()` rejects write that would exceed `0xFFFFFFFF` bytes
  - Timestamp encode: `fat32_filetime_to_fattime()` for 2026-04-04 12:30:00 produces correct FAT date/time fields
- [ ] Create `src/kernel/test/test_vfs_semantics.c` with:
  - `vfs_path_fold("c:\\foo\\Bar")` produces `"C:\\FOO\\BAR"`
  - `vfs_path_fold("X:\\Logs\\kernel.log")` preserves drive letter and backslashes
  - Share-mode: open with WRITE+no-share, second open returns `STATUS_SHARING_VIOLATION`
  - Share-mode: close first handle, second open succeeds
  - Delete-on-close: open with `VFS_DELETE_ON_CLOSE`, second open returns `STATUS_DELETE_PENDING`
  - Delete-on-close: close handle, file no longer exists
  - Byte-range lock: lock 0-1023 exclusive, overlapping lock 512-1535 returns `STATUS_FILE_LOCK_CONFLICT`
  - Byte-range lock: unlock, second lock succeeds
  - Oplock: request Level 1 on file, returns success; second open breaks to Level 2
- [ ] Register in `test_runner_init()`: `test_register_fat32_hardening()`, `test_register_vfs_semantics()`
- [ ] Commit: `"test: add FAT32 hardening + VFS Win32 semantics test suites"`

---

## OS Comparison


| ⭐ | Feature                | 🪟 Win11                  | 🐧 Linux                  | 🚀 Impossible OS              |
|----|------------------------|---------------------------|----------------------------|-------------------------------|
| 💎 | BPB validation + dirty | ✅ fastfat strict check   | ✅ fat_fill_super + DIRTY  | ✅ §1 -- validate_bpb + dirty |
| 💎 | FSInfo FreeCount       | ✅ maintained + scan      | ✅ count_free fallback     | ✅ §2 -- scan + sync          |
| 💎 | Dual-FAT mirror        | ✅ both written + repair  | ✅ both written, no repair | ✅ §3 -- write + compare      |
| 💎 | LFN write              | ✅ full LFN R/W           | ✅ fat_add_entries          | ✅ §4 -- full LFN R/W         |
| 💎 | FAT32 timestamps       | ✅ FILETIME encode        | ✅ fat_time_unix2fat       | ⚠️ §5 -- set_times partial    |
| 💎 | FAT32 fsck             | ✅ chkdsk full check      | ✅ fsck.fat cross-link     | ⬜ §6 -- fat32_fsck           |
| ⭐ | Unified case-fold VFS  | ⚠️ per-driver fold        | ⚠️ per-mount nocase        | ⬜ §7 -- vfs_path_fold        |
| 💎 | Share-mode enforcement | ✅ per-FCB share check    | ✅ POSIX locks             | ⬜ §8 -- per-handle check     |
| 💎 | Delete-on-close        | ✅ DELETE_ON_CLOSE flag    | ✅ unlink-then-keep-open   | ⬜ §9 -- last-close unlink    |
| 💎 | Byte-range locks       | ✅ LockFile/UnlockFile    | ✅ fcntl F_SETLK           | ⬜ §10 -- vfs_lock_file       |
| 💎 | Win32 feature stubs    | ✅ full ADS/ACL/vol       | ❌ POSIX only              | ⬜ §11 -- stub returns        |
| 💎 | SFN tail collision     | ✅ ~1-~9, 5-char ~10+     | ✅ fat_gen_ks_short hash   | ⬜ §12 -- collision loop      |
| 💎 | 4 GiB write guard      | ✅ rejects at limit       | ✅ fat_cont_expand check   | ⬜ §13 -- write + truncate    |
| 💎 | Opportunistic locks    | ✅ L1/L2/Batch/R/RW/RH   | ⚠️ POSIX leases only       | ⬜ §14 -- Level 1/2           |

> After §1-§14, FAT32 matches fastfat.sys spec correctness and exceeds dosfstools with in-kernel fsck.
> §7 (unified VFS case-fold) is the architectural win -- one implementation benefits all filesystem drivers.
> §12-§14 close remaining parity: SFN collision, file size guard, oplocks.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] BPB invalid volume (bad `jmpBoot`): `fat32_init()` returns error; serial shows `[FAT32] BPB validation failed`
- [ ] FSInfo `FreeCount=0xFFFFFFFF`: mount triggers full FAT scan; `FreeCount` correct after scan; FSInfo written on unmount
- [ ] Dual-FAT: corrupt FAT2 manually; mount → serial shows `[FAT32] FAT mismatch ... repairing FAT2`; FAT2 repaired byte-for-byte to match FAT1
- [ ] LFN write: create file `"Long File Name Test.txt"` → directory entry has correct LFN slots; read back name matches; delete → LFN slots zeroed (`0xE5`)
- [ ] Timestamps: create file, check `CrtTime`; write to file, check `WrtTime` updated; read file, check `LstAccDate` updated; timestamps parse to reasonable UTC values
- [ ] fsck: `chkdsk D: /fat32` on clean volume → `0 errors`; inject cross-link → reports cross-link; `/fix` → resolves
- [ ] Case-insensitive: `vfs_open("c:\\impossible\\system32\\cmd.exe")` returns same node as `vfs_open("C:\\Impossible\\System32\\cmd.exe")`
- [ ] Share-mode: open file with `WRITE, no-share`; second open with `READ` → `STATUS_SHARING_VIOLATION`; after close, second open succeeds
- [ ] Delete-on-close: open with `DELETE_ON_CLOSE`; second open returns `STATUS_DELETE_PENDING`; close first handle → file deleted; verify directory no longer contains entry
- [ ] Byte-range lock: lock bytes 0–1023 exclusive; second lock on bytes 512–1535 → `STATUS_FILE_LOCK_CONFLICT`; unlock → second lock succeeds
- [ ] Feature stubs: `FindFirstStreamW` returns only `::$DATA`; `GetFileSecurity` returns Everyone:Full-Control SD; `FSCTL_GET_REPARSE_POINT` → `STATUS_NOT_A_REPARSE_POINT`
- [ ] Commit: `"fs: FAT32 hardening + VFS Win32 semantics -- BPB, LFN, fsck, case-fold, share-mode, locks, stubs"`
