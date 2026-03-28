# TODO-04 — FAT32 Hardening & VFS Win32 Semantics

> **Goal:** Harden FAT32 from working R/W to spec-correct: BPB strict validation, FSInfo mount-time fallback, dual-FAT compare-and-repair, LFN write, proper FILETIME timestamps, and an in-kernel fsck. Then layer the VFS Win32 semantics that all user-mode code depends on: case-insensitive path resolution, share-mode enforcement, delete-on-close, byte-range locks, and feature-spoofing stubs.

> [!IMPORTANT]
> FAT32 basic R/W, LFN *read*, FSInfo flush, and dual-FAT *write* all work. The gaps are: no strict BPB validation on mount, no FSInfo fallback for `FreeCount=0xFFFFFFFF`, no dual-FAT compare/repair, no LFN *write* (`fat32_create_file_vol()` writes 8.3 SFN only), no FILETIME-encoded timestamps, and no fsck. The VFS semantics (§7–11) are kernel-wide and benefit NTFS and IXFS equally — implement them in `vfs.c`, not inside FAT32. §7 (case-insensitive) must be done before §8–11 since the handle-lookup path it creates is the one share-mode checks run on.

## Inputs

- `src/kernel/fs/fat32/fat32_core.c` — `fat32_set_fat_entry()` loops `num_fats` (dual-FAT *write* works); `fat32_fsinfo_flush()` exists; BPB fields parsed but not validated; §1–3 extend it
- `src/kernel/fs/fat32/fat32_write.c` — `fat32_create_file_vol()` writes 8.3 SFN only (no LFN slots); `fat32_set_times()` exists but needs FILETIME encoding; §4–5 extend it
- `src/kernel/fs/fat32/fat32_dir.c` — `lfn_extract_chars()` for LFN read exists; §4 adds LFN write
- `src/kernel/fs/vfs.c` + `include/kernel/fs/vfs.h` — no share-mode, delete-on-close, byte-range lock, or case-fold in open path; §7–11 extend it
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1` — `vfs_probe()` calls `fat32_init()`; BPB validation (§1) must fire inside `fat32_init()` before `vfs_probe()` returns success
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §5` — `umount` shell calls the VFS unmount path; §5 (fsck) must be callable as `chkdsk D: /fat32`
- → XREF: `11-user-platform-sdk` — Win32 `CreateFile` flags `FILE_SHARE_*` and `FILE_FLAG_DELETE_ON_CLOSE` map directly to §8 and §9; coordinate with the Win32 file-handle TODO

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

## Implementation Order

| ⭐  | Order | Deliverable                                                                           | Depends On                                               | Status |
| --- | :---: | ------------------------------------------------------------------------------------- | -------------------------------------------------------- | :----: |
| 💎  |   1   | §1 FAT32 BPB strict validation — `jmpBoot`, `BytsPerSec`, `SecPerClus`, dirty marker | `fat32_init()` in fat32_core.c                           |  [ ]   |
| 💎  |   2   | §2 FSInfo mount-time fallback — signature check, `0xFFFFFFFF` full-scan fallback     | §1 (init fails fast on bad BPB before FSInfo read)       |  [ ]   |
| 💎  |   3   | §3 Dual-FAT compare and repair — mount compare, log mismatch, FAT2 repair            | §1, §2 (valid volume before FAT compare)                 |  [ ]   |
| 💎  |   4   | §4 LFN write — slot chain before 8.3 SFN, UTF-16LE, checksum, LFN delete            | §3 (FAT write path stable)                               |  [ ]   |
| 💎  |   5   | §5 FAT32 timestamps — FILETIME encode, `LastAccessDate` on read, `WrtTime` on write  | §4 (create path being reworked)                          |  [ ]   |
| 💎  |   6   | §6 FAT32 fsck — BPB check, FAT1/2 compare, cross-linked chains, lost clusters       | §3 + §4 + §5 (stable FAT and directory state)            |  [ ]   |
| ⭐  |   7   | §7 VFS case-insensitive path resolution — uppercase fold in `vfs_open()`             | None (pure VFS layer change)                             |  [ ]   |
| 💎  |   8   | §8 VFS share-mode enforcement — per-handle `share_mode`, `STATUS_SHARING_VIOLATION` | §7 (handle lookup uses case-folded path)                 |  [ ]   |
| 💎  |   9   | §9 Mark-for-delete-on-close — deferred deletion, `STATUS_DELETE_PENDING`            | §8 (open-handle table exists)                            |  [ ]   |
| 💎  |  10   | §10 Byte-range locks — `LockFile`/`UnlockFile`, per-file lock list, conflict detect  | §8 (per-handle file identity)                            |  [ ]   |
| 💎  |  11   | §11 VFS feature-spoofing stubs — ADS, ACL, volume flags, reparse-point queries      | §7–10 (handle plumbing in place)                         |  [ ]   |

> §7 (case-insensitive VFS) is `⭐` exclusive in architecture: Windows case-folds inside `NTFS.sys` / `FAT.sys` per volume type; Linux is case-sensitive by default with per-mount options. Impossible OS applies a unified case-fold in the VFS layer above all filesystem drivers — one correct implementation that benefits NTFS, FAT32, IXFS, and any future driver equally.

---

## 1. FAT32 BPB Strict Validation `[Sonnet]`

Validate critical BPB fields in `fat32_init()` before any further mount operations. Detect and set the dirty volume marker on mount; auto-run fsck (§6) if dirty.

**Files:** `src/kernel/fs/fat32/fat32_core.c` (extend)

> [!NOTE]
> FAT32 BPB spec validation rules (Microsoft FAT spec §3): `BPB_jmpBoot[0]` must be `0xEB` or `0xE9`; `BPB_BytsPerSec` ∈ {512, 1024, 2048, 4096}; `BPB_SecPerClus` must be a power of two ≥ 1 and ≤ 128; `BPB_NumFATs` ∈ {1, 2}; `BPB_RootEntCnt` must be 0 for FAT32; `BPB_RootClus` must be ≥ 2. Dirty marker: FAT32 uses FAT[1] high bit (bit 27 = `0x08000000`) to flag a dirty volume; if this bit is clear at mount time, the volume was not cleanly unmounted.

- [ ] `fat32_validate_bpb(struct fat32_volume *vol)`: check each field; on failure log `[FAT32] BPB validation failed: <reason>` and return `-1`; `fat32_init()` must call this before FSInfo read
- [ ] Fields to check: `jmpBoot[0]`, `BytsPerSec`, `SecPerClus` (power-of-two: `(x & (x-1)) == 0`), `NumFATs`, `RootEntCnt == 0`, `RootClus >= 2`, `TotSec32 > 0`
- [ ] Dirty marker: read FAT[1] (cluster 1 entry); if bit 27 clear: `vol->volume_dirty = 1`; log `[FAT32] Dirty volume — scheduling fsck`; schedule `fat32_fsck()` call after mount completes
- [ ] On clean unmount: set FAT[1] bit 27 (volume clean) before writing final FAT sector
- [ ] Commit: `"fs/fat32: BPB strict validation — jmpBoot, BytsPerSec, SecPerClus, RootClus, dirty marker"`

## 2. FSInfo Mount-Time Fallback `[Sonnet]`

Validate FSInfo signatures on mount. Fall back to a full FAT scan when `FreeCount == 0xFFFFFFFF`. Write updated counts after every alloc/free.

**Files:** `src/kernel/fs/fat32/fat32_core.c` (extend)

> [!NOTE]
> FSInfo sector layout (offset 0): `LeadSig = 0x41615252`; offset 484: `StrucSig = 0x61417272`; offset 488: `FreeCount` (0xFFFFFFFF = unknown); offset 492: `NxtFree` (0xFFFFFFFF = unknown); offset 508: `TrailSig = 0xAA550000`. Backup FSInfo at sector `BPB_BkBootSec + 1` (typically sector 7). If either signature is wrong: `vol->fsinfo_valid = 0`; proceed without FSInfo.

- [ ] At mount: read `vol->fsinfo_sector`; verify `LeadSig`, `StrucSig`, `TrailSig`; if any mismatch: `vol->fsinfo_valid = 0`; log `[FAT32] FSInfo invalid — will scan FAT`
- [ ] If `vol->fsinfo_valid && FreeCount == 0xFFFFFFFF`: full FAT scan (`fat32_count_free_clusters(vol)`); store result in `vol->fsinfo_free_count`; set `vol->fsinfo_dirty = 1`
- [ ] `fat32_alloc_cluster()`: after alloc, decrement `vol->fsinfo_free_count`; update `NxtFree`; set `vol->fsinfo_dirty = 1`
- [ ] `fat32_free_chain()`: after free, increment `vol->fsinfo_free_count`; set `vol->fsinfo_dirty = 1`
- [ ] Verify `fat32_fsinfo_flush()` already writes backup at sector+6; confirm it runs on unmount
- [ ] Commit: `"fs/fat32: FSInfo — signature validation, 0xFFFFFFFF full-scan fallback, alloc/free sync"`

## 3. Dual-FAT Compare and Repair `[Sonnet]`

On mount, compare FAT1 and FAT2 sector by sector. Log any mismatch. Use FAT1 as authoritative; repair FAT2 if they differ.

**Files:** `src/kernel/fs/fat32/fat32_core.c` (extend)

> [!NOTE]
> `fat32_set_fat_entry()` already loops `fi < vol->bpb.num_fats` — both FATs are written on alloc/free. The missing piece is a *mount-time compare*: read corresponding sectors of FAT1 and FAT2 and compare byte-for-byte. FAT size in sectors: `vol->bpb.fat_size_sectors`. If `NumFATs < 2`, skip the compare. Repair: copy FAT1 sectors to FAT2 sectors wherever they differ.

- [ ] `fat32_compare_repair_fats(struct fat32_volume *vol)`: for each sector `s` in `0..fat_size_sectors-1`: read `FAT1[s]` and `FAT2[s]`; if different: log `[FAT32] FAT mismatch at sector %u — repairing FAT2`; write FAT1 sector to FAT2 offset; increment mismatch counter
- [ ] Call at end of `fat32_init()` only when `vol->bpb.num_fats == 2`; log total mismatch count at end
- [ ] Non-fatal: mismatch repairs do not abort mount; log summary `[FAT32] FAT compare: %u mismatched sectors repaired`
- [ ] Commit: `"fs/fat32: dual-FAT compare-on-mount — sector-by-sector compare, FAT2 repair"`

## 4. LFN Write `[Opus]`

Write LFN directory entry chains before the 8.3 SFN entry. Encode names as UTF-16LE. Compute SFN checksum. Handle LFN delete (zero out slot chain).

**Files:** `src/kernel/fs/fat32/fat32_write.c` (extend), `src/kernel/fs/fat32/fat32_dir.c` (extend)

> [!NOTE]
> LFN entry layout (32 bytes): `Ord (1)` = sequence number (last slot OR'd with `0x40`); `Name1 (10)` = chars 1–5 as UTF-16LE; `Attr (1)` = `0x0F`; `Type (1)` = 0; `Chksum (1)` = SFN checksum; `Name2 (12)` = chars 6–11; `FstClusLO (2)` = 0; `Name3 (4)` = chars 12–13. SFN checksum: `for i in 0..10: sum = ((sum & 1) << 7) + (sum >> 1) + sfn[i]`. Slot count: `ceil(utf16len / 13)`. Slots written in *reverse* order (highest sequence first), then the SFN entry. Max LFN length: 255 UTF-16LE codepoints = 20 slots.

- [ ] `fat32_lfn_slot_count(const char *utf8_name)` → count UTF-8 codepoints; return `(count + 12) / 13`
- [ ] `fat32_lfn_write_slots(vol, dir_cluster, sfn[11], utf8_name)`: allocate `slot_count + 1` contiguous free directory entries; write slots from last to first with correct `Ord` + `0x40` on final; pack UTF-16LE chars (stop-fill with `0xFFFF` on last partial slot, `0x0000` terminators); write SFN entry last
- [ ] `fat32_create_file_vol()` and `fat32_create_dir_vol()`: check if name requires LFN (non-8.3 or non-ASCII); if yes call `fat32_lfn_write_slots()`; if pure 8.3 ASCII keep existing SFN-only path
- [ ] LFN delete: `fat32_delete_file_vol()` must scan backwards from SFN entry to find and zero all preceding LFN slots (attr=`0x0F`, matching checksum) — mark each with `Ord[0] = 0xE5`
- [ ] `fat32_lfn_checksum(const uint8_t sfn[11])` — standalone helper
- [ ] Commit: `"fs/fat32: LFN write — slot chain, UTF-16LE encode, SFN checksum, LFN delete"`

## 5. FAT32 Timestamps `[Sonnet]`

Encode kernel FILETIME (100 ns ticks since 1601 UTC) to FAT32 date/time format. Set `CrtTime`/`CrtDate` on create, `WrtTime`/`WrtDate` on write, `LstAccDate` on read. Apply timezone bias.

**Files:** `src/kernel/fs/fat32/fat32_write.c` (extend), `src/kernel/fs/fat32/fat32_ops.c` (extend)

> [!NOTE]
> FAT32 time format (16 bits): bits 15–11 = hours (0–23), 10–5 = minutes (0–59), 4–0 = 2-second counts (0–29). FAT32 date format (16 bits): bits 15–9 = year since 1980 (0–127, so max year 2107), 8–5 = month (1–12), 4–0 = day (1–31). Timezone: FAT32 stores local time; apply `HKLM\SYSTEM\TimeZone\BiasMinutes` offset (FILETIME is UTC). Year 2107 boundary: if year > 2107 (i.e. after 2107-12-31), clamp to 2107/12/31 and log a warning.

- [ ] `fat32_filetime_to_fattime(uint64_t filetime, uint16_t *date, uint16_t *time_val, uint8_t *time_tenth)`: convert FILETIME (UTC) → local time using Registry bias → FAT date/time fields; clamp year > 2107
- [ ] `fat32_create_file_vol()` + `fat32_create_dir_vol()`: call `fat32_filetime_to_fattime(kernel_time_now(), ...)` and set `CrtTime`, `CrtDate`, `WrtTime`, `WrtDate`, `LstAccDate` in the SFN entry
- [ ] `fat32_ops.c` write callback: after successful data write, update `WrtTime`/`WrtDate` in SFN entry
- [ ] `fat32_ops.c` read callback: update `LstAccDate` in SFN entry (date only, no time field for access)
- [ ] `fat32_set_times()` already exists — verify it uses the new `fat32_filetime_to_fattime()` encoder
- [ ] Commit: `"fs/fat32: timestamps — FILETIME→FAT encode, CrtTime on create, WrtTime on write, LstAccDate on read"`

## 6. FAT32 fsck `[Opus]`

Implement `fat32_fsck(vol, fix)` to validate BPB, compare FAT1/FAT2, detect cross-linked chains and lost clusters. Expose as `chkdsk D: /fat32` shell command.

**Files:** `src/kernel/fs/fat32/fat32_fsck.c` (new), `include/kernel/fs/fat32.h` (extend), `src/shell/cmd_chkdsk.c` (extend)

> [!NOTE]
> Cross-linked chain: two directory entries whose cluster chains share a common cluster node. Detect by building a `visited[]` bitset (1 bit per cluster, allocated from `pmm_alloc_contiguous`); if a cluster is already marked, the chain is cross-linked. Lost clusters: clusters marked allocated in FAT1 but not reachable from any directory entry chain. Count them; if `fix`: chain them into `\FOUND.000\FILE0000.CHK` in the root directory (like Windows `chkdsk /f`).

- [ ] `fat32_fsck(struct fat32_volume *vol, int fix)`: validate BPB via `fat32_validate_bpb()`; compare FAT1 vs FAT2 (reuse §3 logic); walk all directory entries recursively from root cluster; build `visited[]` bitset
- [ ] Cross-link detection: on each `fat32_follow_chain()` step, check `visited[cluster]`; if already set: log `[FSCK] Cross-linked chain at cluster %u (files: %s, %s)`; if fix: truncate the second chain at the cross-link point
- [ ] Lost cluster detection: after walk, scan all FAT entries; if FAT[cluster] != 0 and not in `visited[]`: `lost_count++`; if fix: chain lost runs into root `FOUND.000` directory
- [ ] Report: log summary `[FSCK] %u errors, %u cross-links, %u lost clusters` + `Fixed` or `Errors remain`; return 0 (clean) or -1 (errors found or unfixable)
- [ ] `chkdsk D: /fat32` shell: calls `fat32_fsck(vol, fix=0)` for read-only scan; `/fix` flag calls with `fix=1`
- [ ] Commit: `"fs/fat32: fsck — BPB check, FAT1/2 compare, cross-link, lost cluster, chkdsk shell command"`

## 7. VFS Case-Insensitive Path Resolution `[Opus]`

Apply a unified uppercase fold in `vfs_open()` before path component lookup, so `C:\WINDOWS\System32` resolves identically to `c:\windows\system32` on all filesystems.

**Files:** `src/kernel/fs/vfs.c` (extend), `include/kernel/fs/vfs.h` (extend)

> [!NOTE]
> Case-fold strategy: uppercase ASCII A–Z only in the VFS path splitter (no Unicode case-fold required for FAT32 8.3; NTFS uses `$UpCase` table internally per volume). The path splitter in `vfs_open()` should normalise each component to uppercase before passing to `finddir()`. Each filesystem's `finddir()` then does its own case-insensitive compare (FAT32 already has `fat32_strcasecmp()`; NTFS has `ntfs_filename_match()`). This ensures case-folding is consistent regardless of filesystem type.

- [ ] `vfs_path_fold(const char *in, char *out, uint32_t max)`: copy path normalising ASCII letters to uppercase; preserve path separators `\` and drive letter colon
- [ ] Call `vfs_path_fold()` at the top of `vfs_open()` before path-component splitting; use folded path for all `finddir()` calls
- [ ] Verify `fat32_strcasecmp()` and `ntfs_filename_match()` both handle the folded component correctly; add integration test: open `c:\impossible\system32\cmd.exe` and `C:\Impossible\SYSTEM32\CMD.EXE` → both must return the same node
- [ ] `vfs_path_fold()` must handle drive letter: `c:\foo` → `C:\foo` (drive letter uppercased too)
- [ ] Commit: `"fs: vfs_path_fold() — unified ASCII uppercase fold in vfs_open() before finddir()"`

## 8. VFS Share-Mode Enforcement `[Sonnet]`

Add `share_mode` and `access_mode` to the per-open-handle struct. On `vfs_open()`, check existing handles for conflicts. Return `STATUS_SHARING_VIOLATION` on conflict.

**Files:** `src/kernel/fs/vfs.c` (extend), `include/kernel/fs/vfs.h` (extend)

> [!NOTE]
> Windows share-mode semantics: when opening a file with `DesiredAccess=READ` and `ShareMode=FILE_SHARE_READ`, any existing handle with `DesiredAccess=WRITE` that did not grant `FILE_SHARE_WRITE` causes `STATUS_SHARING_VIOLATION`. Implement as: on `vfs_open()`, iterate all open handles for the same inode; for each existing handle `H`: if `(new_access & ~H.share_mode) != 0` OR `(H.access & ~new_share_mode) != 0`, return `STATUS_SHARING_VIOLATION`.

- [ ] Add to `vfs_handle_t` (or equivalent open-handle struct): `uint32_t access_mode`, `uint32_t share_mode`
- [ ] Per-inode open-handle list: maintain `vfs_node_t.open_handles[]` (or a global inode→handle map keyed by device+inode); populated by `vfs_open()`, cleared by `vfs_close()`
- [ ] `vfs_open()`: after resolving the node, call `vfs_check_sharing(node, desired_access, share_mode)`; on conflict return `STATUS_SHARING_VIOLATION`
- [ ] `vfs_check_sharing(node, access, share)`: iterate node's open handles; apply conflict rule above; return 0 (OK) or `STATUS_SHARING_VIOLATION`
- [ ] `vfs_close()`: remove handle from node's open-handle list; if node marked delete-on-close and list is now empty: trigger delete (§9)
- [ ] Commit: `"fs: VFS share-mode — per-handle access/share fields, conflict check, STATUS_SHARING_VIOLATION"`

## 9. Mark-for-Delete-on-Close `[Sonnet]`

Implement `FILE_FLAG_DELETE_ON_CLOSE`: mark the inode for deletion when the handle is opened with this flag; perform the actual deletion when the last handle closes.

**Files:** `src/kernel/fs/vfs.c` (extend)

> [!NOTE]
> Windows semantics: `NtCreateFile(DELETE_ON_CLOSE)` marks the file. Any subsequent `NtOpenFile` on the same path returns `STATUS_DELETE_PENDING`. The actual delete happens in the `NtClose()` path when the last handle referencing the file is closed. The inode must remain reachable by existing handles until last-close.

- [ ] Add `uint8_t delete_on_close` flag to `vfs_node_t` (or the open-handle struct if per-handle)
- [ ] `vfs_open()`: if `flags & VFS_DELETE_ON_CLOSE`: set `node->delete_on_close = 1`; if `node->delete_on_close` already set on a *different* open: return `STATUS_DELETE_PENDING` to new openers
- [ ] `vfs_close()` last-handle path: if `node->delete_on_close` and `open_handle_count == 0`: call `vfs_delete(node)` (which calls the filesystem driver's `unlink()`)
- [ ] `vfs_open()` on `delete_on_close` node: existing openers may still read/write; new openers get `STATUS_DELETE_PENDING`
- [ ] Commit: `"fs: delete-on-close — VFS_DELETE_ON_CLOSE flag, STATUS_DELETE_PENDING, last-close unlink"`

## 10. Byte-Range Locks `[Sonnet]`

Implement `LockFile`/`UnlockFile`: per-file lock list with offset, length, and exclusive/shared flag. Conflict detection returns `STATUS_FILE_LOCK_CONFLICT`.

**Files:** `src/kernel/fs/vfs.c` (extend), `include/kernel/fs/vfs.h` (extend)

> [!NOTE]
> Lock semantics: shared locks are compatible with other shared locks on the same range; exclusive locks conflict with any lock on any overlapping range. Overlap test: `!(new_end <= existing_start || new_start >= existing_end)`. Maximum locks per file: 64 (static array per node). `UnlockFile` must match exact offset+length of a lock owned by the same handle.

- [ ] `vfs_lock_t { uint64_t offset; uint64_t length; vfs_handle_t *owner; uint8_t exclusive; }` — static array `vfs_node_t.locks[64]`, `lock_count`
- [ ] `vfs_lock_file(handle, offset, length, exclusive)`: check for overlapping conflicting locks; if none: add entry; return 0; if conflict: return `STATUS_FILE_LOCK_CONFLICT`
- [ ] `vfs_unlock_file(handle, offset, length)`: find lock entry matching handle + offset + length; remove; return 0 or `STATUS_NOT_LOCKED`
- [ ] `vfs_write()` does NOT check byte-range locks (Windows does not enforce locks in-kernel on write, only via the Win32 API layer — document this caveat)
- [ ] Win32 `LockFile(hFile, offset_low, offset_high, len_low, len_high)` → `vfs_lock_file(handle, offset, length, exclusive=TRUE)`; `UnlockFile` → `vfs_unlock_file()`
- [ ] Commit: `"fs: byte-range locks — vfs_lock_file/unlock_file, overlap conflict, STATUS_FILE_LOCK_CONFLICT"`

## 11. VFS Feature-Spoofing Stubs `[Sonnet]`

Return correct stub responses for Win32 queries that user-mode programs issue on every file system: ADS, ACL, volume flags, and reparse-point queries.

**Files:** `src/kernel/fs/vfs.c` (extend), `src/kernel/win32/file_info.c` (new or extend)

> [!NOTE]
> These stubs allow unmodified Win32 programs that probe filesystem capabilities to run without crashing. They must return the *correct* stub value (not random garbage), because programs make decisions based on the flags. Key stubs: ADS — `FindFirstStreamW` returns `ERROR_HANDLE_EOF` (no streams); ACL query — `GetFileSecurity` returns a minimal SD (Everyone:Full-Control, same as §4 of TODO-02); `GetVolumeInformation` flags — `FILE_UNICODE_ON_DISK | FILE_CASE_PRESERVED_NAMES | FILE_PERSISTENT_ACLS` for NTFS/IXFS, `FILE_UNICODE_ON_DISK | FILE_CASE_PRESERVED_NAMES` for FAT32; reparse point — `DeviceIoControl(FSCTL_GET_REPARSE_POINT)` returns `ERROR_NOT_A_REPARSE_POINT`.

- [ ] ADS: `NtQueryInformationFile(FileStreamInformation)` → single-entry response with just the default `::$DATA` stream, length = file size; `FindFirstStreamW` → returns `::$DATA` then `ERROR_HANDLE_EOF` on next call
- [ ] ACL: `NtQuerySecurityObject(DACL_SECURITY_INFORMATION)` → return the default Everyone:Full-Control SD (from `ntfs_secure_default_sd()` or a VFS-level copy)
- [ ] Volume flags: `NtQueryVolumeInformationFile(FileFsAttributeInformation)` → set `FileSystemAttributes` per filesystem type (see NOTE); `MaximumComponentNameLength = 255`; filesystem name string (`"NTFS"`, `"FAT32"`, `"IXFS"`)
- [ ] Reparse point: `FSCTL_GET_REPARSE_POINT` ioctl via `vfs_ioctl()` → return `STATUS_NOT_A_REPARSE_POINT` for all nodes on non-reparse-supporting filesystems
- [ ] Commit: `"fs: Win32 feature stubs — ADS, ACL, volume flags, reparse-point STATUS_NOT_A_REPARSE_POINT"`

---

## OS Comparison


| ⭐ | Feature                                           | Win11                                                                       | Linux                                                                  | Impossible OS                                                                  |
|----|---------------------------------------------------|-----------------------------------------------------------------------------|------------------------------------------------------------------------|--------------------------------------------------------------------------------|
| 💎 | FAT32 BPB strict validation + dirty-volume marker | ✅ `fatfs.sys`; strict BPB check at                                         | ✅ `fat/inode.c`; `fat_fill_super()` validates BPB; `FAT_STATE_DIRTY`  | ⬜ §1 — `fat32_validate_bpb()`, dirty FAT[1] bit 27,                           |
| 💎 | FSInfo `FreeCount`                                | ✅ `fatfs.sys`; FSInfo maintained; full scan                                | ✅ `fat/fatent.c`; `fat_count_free_clusters()` fallback                | ⚠️ §2 — Partial — flush exists; adds                                           |
| 💎 | Dual-FAT mirror                                   | ✅ `fatfs.sys`; both FATs written; mismatch                                 | ✅ `fat/fatent.c`; `fat_ent_access()`; FAT2 written; no                | ⚠️ §3 — Partial — both written on                                              |
| 💎 | LFN write                                         | ✅ `fatfs.sys`; full LFN read/write                                         | ✅ `fat/dir.c`; `fat_add_entries()` with LFN; full                     | ⚠️ §4 — Partial — LFN read works                                               |
| 💎 | FAT32 timestamps                                  | ✅ `fatfs.sys`; FILETIME encode; UTC→local; all                             | ✅ `fat/inode.c`; `fat_time_fat2unix()` / `fat_time_unix2fat()`        | ⚠️ §5 — Partial — `fat32_set_times()` exists; adds                             |
| 💎 | FAT32 fsck                                        | ✅ `chkdsk.exe`; full FAT check +                                           | ✅ `fsck.fat` (`dosfstools`); cross-link and lost                      | ⬜ §6 — `fat32_fsck.c`, `visited[]` bitset, lost-cluster chaining,             |
| ⭐ | Unified case-insensitive VFS                      | ⚠️ Per-driver case fold (`NTFS.sys`, `fatfs.sys`                            | ⚠️ Per-mount `nocase` option; no unified                               | ⬜ §7 — `vfs_path_fold()` applies once in VFS,                                 |
| 💎 | Share-mode enforcement                            | ✅ `NTFS.sys` / `fatfs.sys`; per-FCB share                                  | ✅ `fs/locks.c`; POSIX open-file description locks;                    | ⬜ §8 — per-handle access/share fields, conflict check                         |
| 💎 | Delete-on-close                                   | ✅ `FILE_FLAG_DELETE_ON_CLOSE`; `STATUS_DELETE_PENDING`; last-handle unlink | ✅ `unlink()`-then-keep-open idiom; fd holds inode                     | ⬜ §9 — `VFS_DELETE_ON_CLOSE` flag, `STATUS_DELETE_PENDING`, last-close unlink |
| 💎 | Byte-range locks                                  | ✅ `LockFile`/`UnlockFile` Win32; per-FCB lock list;                        | ✅ `fcntl(F_SETLK)`; POSIX mandatory/advisory locks; `EACCES`/`EAGAIN` | ⬜ §10 — `vfs_lock_t[]` per node, overlap conflict,                            |
| 💎 | Win32 feature stubs                               | ✅ Full ADS, ACL, volume info,                                              | ❌ Not applicable (POSIX `xattr`, `getfacl`;                           | ⬜ §11 — correct stub returns for `FindFirstStreamW`,                          |

> **After §1–11:** Impossible OS FAT32 matches the spec correctness of `fatfs.sys` and exceeds `dosfstools` in the in-kernel fsck depth. The `⭐` architectural win is §7: a single `vfs_path_fold()` call in the VFS layer above all drivers means no filesystem driver ever needs its own case-fold path — future filesystems (exFAT, ext4) automatically inherit correct case-insensitive behaviour without any extra work.

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
- [ ] Commit: `"fs: FAT32 hardening + VFS Win32 semantics — BPB, LFN, fsck, case-fold, share-mode, locks, stubs"`
