---
schema_version: 1
id: exfat-readwrite
domain: 05-storage-filesystems
status: active
title: "TODO-08 -- exFAT Read/Write Driver"
---

# TODO-08 -- exFAT Read/Write Driver

> **Goal:** Implement a complete exFAT read/write driver -- VBR parser + boot checksum, Allocation Bitmap, Up-Case Table, directory entry sets (File/Stream/Filename), directory read, file read (FAT-chain and NoFatChain fast path), file write, create/delete/rename, timestamp encoding, VFS registration with `vfs_probe()`, fsck, and 64-bit large file support. Without this, the OS cannot use the majority of USB drives and SD cards sold today.

> [!IMPORTANT]
> exFAT is the SD Association–mandated format for SDXC cards (> 32 GB) and the default Windows format for USB drives > 32 GB. Microsoft open-sourced the exFAT specification in 2019, making a clean-room implementation legal and straightforward. No exFAT code exists in the repo -- this is a blank-slate driver. §1–§3 (VBR, Bitmap, Up-Case) are the mandatory foundation; §4–§6 (entry sets, directory read, file read) form the read path; §7–§8 (write, create/delete/rename) complete the write path. §10 (VFS registration) must be done last -- it calls `vfs_probe()` which requires §1–§9 all working. Wire exFAT into `vfs_probe()` at priority step 4 (→ XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1`).

## Inputs

- `src/kernel/fs/fat32/` -- reference implementation for cluster chain traversal pattern and directory scan loop; do not share code, but follow the same structural patterns
- `src/kernel/fs/vfs.c` + `include/kernel/fs/vfs.h` -- `vfs_mount()`, `vfs_fs_driver`, `vfs_node_t` interface all drivers implement
- `src/kernel/fs/partition.c` -- `partition_mount_filesystems()` will be replaced by `vfs_probe()` (TODO-03 §1), which calls `exfat_probe()`; the exFAT probe must detect `"EXFAT   "` OEM ID at sector offset 3
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1` -- `vfs_probe()` calls `exfat_probe()` at step 4 in the probe priority chain; §7 of this TODO must return the correct `fs_identify_result_t` with label, total bytes, free bytes
- → XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md §1` -- VFS passes names verbatim; exFAT must do case-insensitive compare in its own `finddir()` via Up-Case Table
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §3` -- `CreateFile` with an exFAT drive letter calls `NtCreateFile` → `vfs_open` → `exfat_ops.finddir`; ensure `exfat_ops` exposes the full 14-entry `vfs_fs_driver` vtable

## Outcome

- VBR parsed and boot checksum verified; backup VBR read on primary failure.
- Allocation Bitmap loaded and maintained; `exfat_alloc_cluster()`/`exfat_free_cluster()` accurate.
- Up-Case Table verified and cached; `exfat_upcase()` used in all name comparisons and hash computation.
- Directory entry sets (File/Stream/Filename) fully parsed; UTF-16LE → UTF-8 name decode.
- File read works for both FAT-chain and `NoFatChain` (contiguous) files.
- File write: within-cluster update, cluster append, truncation; timestamps + SetChecksum updated.
- Create/delete/rename fully operational with entry-set atomicity and bitmap/FAT consistency.
- `exfat_probe()` integrated into `vfs_probe()` chain; dirty-volume auto-fsck.
- `exfat_fsck()` detects cross-linked chains and orphaned clusters; `chkdsk D: /exfat` works.
- Files > 4 GiB created, written, and read correctly (64-bit `DataLength`).
- Full UTF-16 round-trip including emoji and CJK; filename validation rejects illegal characters.

## Implementation Order

| ⭐  | Order | Deliverable                                                                          | Depends On                                                   | Status |
| --- | :---: | ------------------------------------------------------------------------------------ | ------------------------------------------------------------ | :----: |
| 💎  |   1   | §1 VBR parser + boot checksum -- OEM ID, geometry fields, backup VBR fallback        | Block device I/O working                                     |  [ ]   |
| 💎  |   2   | §2 Allocation Bitmap -- bitmap cache, `exfat_alloc_cluster`, `exfat_free_cluster`    | §1 (VBR fields: `ClusterHeapOffset`, `ClusterCount`)         |  [ ]   |
| 💎  |   3   | §3 Up-Case Table -- verify checksum, cache, `exfat_upcase()`, name hash              | §1 (root directory cluster to find UpCase entry)             |  [ ]   |
| 💎  |   4   | §4 Directory entry sets -- File/Stream/Filename parse, UTF-16 decode, SetChecksum    | §3 (Up-Case needed for name hash validation)                 |  [ ]   |
| 💎  |   5   | §5 Directory read -- cluster chain walk, entry-set assembly, `vfs_dirent` output     | §4 (entry set parser), §2 (FAT chain for multi-cluster dirs) |  [ ]   |
| 💎  |   6   | §6 File read -- FAT-chain path + `NoFatChain` fast path                              | §5 (inode has cluster + DataLength from entry set)           |  [ ]   |
| 💎  |   7   | §7 File write -- within-cluster, cluster append, truncation, timestamps, SetChecksum | §6 (read path proven; write mirrors it), §2 (alloc needed)   |  [ ]   |
| 💎  |   8   | §8 File create / delete / rename -- entry-set write, bitmap+FAT cleanup              | §7 (write path stable), §5 (directory cluster chain write)   |  [ ]   |
| 💎  |   9   | §9 Timestamp encoding -- `FILETIME`↔exFAT binary, 10 ms precision, UTC offset        | §8 (create/write paths call the encoder)                     |  [ ]   |
| 💎  |  10   | §10 VFS registration + probe + dirty-volume fsck + `chkdsk` shell command            | §1–§9 all complete                                           |  [ ]   |
| 💎  |  11   | §11 Unicode edge cases -- 255-char names, emoji/CJK round-trip, illegal char reject  | §4 (name parsing complete)                                   |  [ ]   |
| 💎  |  12   | §12 Large file support (> 4 GiB) -- 64-bit `DataLength`, `ValidDataLength` tracking  | §7 (write path complete)                                     |  [ ]   |

> All sections are `💎` parity -- exFAT is a mandatory interoperability feature with no OS-differentiation claim. The goal is spec-correct conformance with the published Microsoft exFAT specification (2019) to achieve full interoperability with Windows-formatted drives.

---

## 1. VBR Parser + Boot Checksum `[Sonnet]`

Parse the exFAT Volume Boot Record. Verify the 11-sector boot checksum. Fall back to the backup VBR at sector offset 12 on primary failure.

**Files:** `src/kernel/fs/exfat/exfat_core.c` (new), `include/kernel/fs/exfat.h` (new), `include/kernel/fs/exfat_internal.h` (new)

> [!NOTE]
> VBR layout (sectors 0–10): sector 0 = Main Boot Sector (`JumpBoot[3]`, `OEMName[8] = "EXFAT   "`, `PartitionOffset:8`, `VolumeLength:8`, `FATOffset:4`, `FATLength:4`, `ClusterHeapOffset:4`, `ClusterCount:4`, `FirstClusterOfRootDirectory:4`, `VolumeSerialNumber:4`, `FileSystemRevision:2 = 0x0100`, `VolumeFlags:2`, `BytesPerSectorShift:1 (9–12, i.e. 512–4096 B)`, `SectorsPerClusterShift:1 (0–25)`, `NumberOfFATs:1 (1 or 2)`, `PercentInUse:1`); sectors 1–8 = Extended Boot Sectors; sector 9 = OEM Parameters; sector 10 = Reserved; sector 11 = Boot Checksum (each of the 4-byte checksum values is the CRC32 of sectors 0–10 combined, excluding `VolumeFlags.ActiveFAT` and `VolumeFlags.VolumeDirty` bits). Backup VBR at sectors 12–23 (same layout, same checksum).

- [ ] `exfat_volume_t` struct: all geometry fields extracted from VBR; computed fields: `bytes_per_sector`, `bytes_per_cluster`, `fat_lba`, `heap_lba`, `root_cluster`
- [ ] `exfat_vbr_parse(dev, &vol)`: read sector 0; verify OEM ID `"EXFAT   "` at offset 3; verify boot checksum (sum all 4-byte words of sectors 0–10 with exclusions); if fail → read backup VBR (sector 12); if backup valid → restore primary; if both fail → return -1
- [ ] `exfat_cluster_to_lba(vol, cluster)` → `heap_lba + (cluster - 2) * sectors_per_cluster`
- [ ] `exfat_fat_entry(vol, cluster)` → read FAT sector, return 32-bit cluster entry
- [ ] `exfat_fat_set(vol, cluster, value)` → write FAT sector entry; write mirror FAT if `NumberOfFATs == 2`
- [ ] `vol->dirty`: if `VolumeFlags & VOLUME_IS_DIRTY (bit 1)`: set `vol->dirty = 1`; schedule fsck after mount
- [ ] Log: `[exFAT] Mounted %c: vol=%s cluster_size=%u sectors=%llu`
- [ ] Commit: `"fs/exfat: VBR parser -- geometry, boot checksum, backup VBR fallback, dirty flag"`

## 2. Allocation Bitmap `[Sonnet]`

Load the Allocation Bitmap cluster into memory. Implement `exfat_alloc_cluster(n)` (first-fit contiguous) and `exfat_free_cluster(start, n)`. Flush on `fsync` and unmount.

**Files:** `src/kernel/fs/exfat/exfat_alloc.c` (new)

> [!NOTE]
> The Allocation Bitmap is found via a directory entry in the root directory (Type `0x81`, `DataLength` = `ceil(ClusterCount / 8)` bytes). One bit per data cluster; bit 0 of byte 0 = cluster 2 (first data cluster). Bit set = allocated. `exfat_alloc_cluster(n)`: scan bitmap for n *contiguous* free bits; set them; update `PercentInUse` in VBR. `exfat_alloc_cluster(1)` for single-cluster alloc. Cached in-memory bitmap (`vol->bitmap`); dirty flag; `exfat_bitmap_flush()` writes changed sectors back.

- [ ] `exfat_bitmap_load(vol)`: find Allocation Bitmap directory entry in root; read its cluster chain into `vol->bitmap` (heap-allocated); store `vol->bitmap_size_bytes`
- [ ] `exfat_alloc_cluster(vol, count, &first)`: first-fit scan of `vol->bitmap`; set bits; update `vol->free_clusters`; set `vol->bitmap_dirty = 1`; update `PercentInUse` in VBR; return 0 or -ENOSPC
- [ ] `exfat_alloc_cluster_near(vol, hint, count, &first)`: start scan from cluster `hint` for locality; fall back to full scan if not found
- [ ] `exfat_free_cluster(vol, start, count)`: clear bitmap bits; update `vol->free_clusters`; `vol->bitmap_dirty = 1`
- [ ] `exfat_bitmap_flush(vol)`: write all dirty bitmap sectors to disk via `exfat_write_sector()`
- [ ] `exfat_fat_chain_len(vol, first_cluster)` → count FAT entries until end-of-chain (`>= 0xFFFFFFF8`)
- [ ] Commit: `"fs/exfat: Allocation Bitmap -- load, alloc/free contiguous, dirty flush, free_clusters tracking"`

## 3. Up-Case Table `[Sonnet]`

Load and verify the Up-Case Table. Cache in memory. Implement `exfat_upcase(codepoint)` for all name comparisons. Implement `exfat_name_hash()`.

**Files:** `src/kernel/fs/exfat/exfat_upcase.c` (new)

> [!NOTE]
> The Up-Case Table is found via a directory entry in the root directory (Type `0x82`, `DataLength = 128 * 1024` = 128 KiB for the full table, 2 bytes per Unicode codepoint × 65 536 codepoints). The table may be compressed -- the spec allows a 5 740-entry table for the ASCII range if the upper range follows identity mapping. Table checksum: CRC32 of the table data (excluding sync bits). `exfat_name_hash(name_utf16, len)`: compute hash as per spec: `hash = ((hash << 15) | (hash >> 1)) + upcase(codepoint)` for each UTF-16LE codepoint, initial value 0. Case-fold in `exfat_dir_lookup` uses `exfat_upcase()`.

- [ ] `exfat_upcase_load(vol)`: find UpCase entry in root dir; read cluster chain; verify CRC32; `vol->upcase = kmalloc(128*1024)`; if compressed: expand to full 65 536-entry table
- [ ] `exfat_upcase(vol, codepoint)` → `vol->upcase[codepoint]`; if `codepoint >= 0xFFFF` return `codepoint` (identity)
- [ ] `exfat_name_hash(vol, name_utf16, name_len)` → per-spec hash using `exfat_upcase()`; must match `NameHash` field in Stream Extension
- [ ] `exfat_name_cmp(vol, a_utf16, a_len, b_utf16, b_len)` → upcase both; compare; return 0 if equal (case-insensitive)
- [ ] Commit: `"fs/exfat: Up-Case Table -- load+verify CRC32, exfat_upcase(), exfat_name_hash(), case-insensitive cmp"`

## 4. Directory Entry Sets `[Opus]`

Parse File Entry (type `0x85`), Stream Extension (type `0xC0`), and Filename Extension (type `0xC1`) entry sets. Decode UTF-16LE names. Verify SetChecksum. Assemble into `exfat_inode_t`.

**Files:** `src/kernel/fs/exfat/exfat_dir.c` (new), `include/kernel/fs/exfat_internal.h` (extend)

> [!NOTE]
> Entry set rules: a valid file entry set consists of exactly one File Entry (`0x85`) followed by one Stream Extension (`0xC0`) and 1–17 Filename Extension entries (`0xC1`). All entries in the set must be in consecutive directory slots (no gaps). `SecondaryCount` in the File Entry gives the total number of secondary entries (Stream + Filename extensions). `SetChecksum`: sum of all bytes in all entries, excluding bytes 2–3 of the File Entry (the checksum field itself); algorithm: `checksum = ((checksum << 15) | (checksum >> 1)) + byte` for each byte. The entry set is valid only if `SetChecksum` in the File Entry matches the recomputed value.

- [ ] `struct exfat_file_entry_raw` + `struct exfat_stream_ext_raw` + `struct exfat_fname_ext_raw` -- exact on-disk layout (32 bytes each)
- [ ] `exfat_inode_t { uint32_t first_cluster; uint64_t data_length; uint64_t valid_data_length; uint16_t attributes; uint32_t file_entry_sector; uint32_t file_entry_offset; exfat_timestamp_t create_time, modified_time, access_time; char name_utf8[768]; uint16_t name_utf16[256]; uint8_t name_len; }`
- [ ] `exfat_parse_entry_set(vol, buf, offset, &inode)`: read File Entry at `offset`; validate type `0x85`; read `SecondaryCount` secondary entries; assemble Stream + Filename entries; decode UTF-16LE name (concat all `0xC1` entries); verify `SetChecksum`; populate `exfat_inode_t`
- [ ] `exfat_compute_set_checksum(entry_set_buf, total_entries)` → recompute and compare
- [ ] Deleted entry detection: if `entry_type & 0x80 == 0` → skip (deleted); continue scan
- [ ] UTF-16LE → UTF-8 decode: `exfat_utf16_to_utf8(name_utf16, name_len, name_utf8, 768)`; handle surrogate pairs for emoji (U+1F000+)
- [ ] Commit: `"fs/exfat: directory entry sets -- File/Stream/Filename parse, UTF-16 decode, SetChecksum verify"`

## 5. Directory Read `[Sonnet]`

Walk directory cluster chains. Assemble entry sets. Emit `vfs_dirent` per valid file. Handle multi-cluster directories. Skip deleted entries.

**Files:** `src/kernel/fs/exfat/exfat_dir.c` (extend)

> [!NOTE]
> Directory cluster chain: starting cluster from parent inode's `first_cluster`; follow FAT chain until end-of-chain. Each cluster holds `bytes_per_cluster / 32` directory entries (32 bytes each). Entry scan: for each 32-byte slot in order: read type byte; if `0x00` (EndOfDirectory): stop; if `type & 0x80 == 0`: deleted, skip; if `type == 0x85`: start of new entry set; call `exfat_parse_entry_set()`.

- [ ] `exfat_readdir(vol, dir_inode, callback, ctx)`: walk FAT chain from `dir_inode->first_cluster`; for each cluster: read all sectors; for each 32-byte slot: check type; assemble entry sets; call `callback(ctx, &inode)` per valid entry
- [ ] End-of-directory: entry type `0x00` means no more entries after this point; break loop
- [ ] `exfat_dir_lookup(vol, dir_inode, name_utf8, &result)`: `exfat_readdir()` with a name-compare callback using `exfat_name_cmp()`; return the first matching `exfat_inode_t`
- [ ] VFS `finddir` callback: `exfat_vfs_finddir(vfs_node, name)` → `exfat_dir_lookup()` → return `vfs_node_t*`
- [ ] VFS `readdir` callback: `exfat_vfs_readdir(vfs_node, index)` → walk to `index`-th entry; return `vfs_dirent`
- [ ] Commit: `"fs/exfat: directory read -- cluster chain walk, entry set scan, finddir, readdir VFS callbacks"`

## 6. File Read `[Sonnet]`

Read file data via FAT cluster chain. Add `NoFatChain` fast path (contiguous allocation, no FAT traversal). Handle offsets spanning multiple clusters.

**Files:** `src/kernel/fs/exfat/exfat_io.c` (new)

> [!NOTE]
> `NoFatChain` flag: bit 1 of `GeneralSecondaryFlags` in the Stream Extension. When set, the file's data clusters are contiguous -- to reach cluster N, compute `first_cluster + N` directly, skipping FAT traversal entirely. This is set by default for newly allocated contiguous files. FAT-chain path: walk `exfat_fat_entry(cluster)` until end-of-chain. Both paths must handle reads spanning cluster boundaries correctly.

- [ ] `exfat_file_read(vol, inode, offset, buf, len)`:
  - Compute starting cluster index: `cluster_idx = offset / bytes_per_cluster`
  - If `GeneralSecondaryFlags & EXFAT_FLAG_NOFATCHAIN`: `cluster = inode->first_cluster + cluster_idx`
  - Else: walk FAT chain `cluster_idx` steps from `first_cluster`
  - Read sectors within cluster; advance to next cluster; repeat until `len` bytes read or `offset + n >= valid_data_length`
- [ ] Handle `ValidDataLength < DataLength`: bytes in `[ValidDataLength, DataLength)` return zero without disk I/O
- [ ] VFS `read` callback: `exfat_vfs_read(vfs_node, buf, offset, len)` → `exfat_file_read()`
- [ ] Commit: `"fs/exfat: file read -- FAT-chain + NoFatChain fast path, ValidDataLength zero-fill, VFS read callback"`

## 7. File Write `[Opus]`

Within-cluster update, cluster append (alloc + extend FAT chain), truncation (free tail clusters). Update `DataLength`, `ValidDataLength`, `LastModifiedTimestamp`, `SetChecksum` on every write.

**Files:** `src/kernel/fs/exfat/exfat_io.c` (extend), `src/kernel/fs/exfat/exfat_dir.c` (extend)

> [!NOTE]
> Cluster append: when write extends beyond current `DataLength`, call `exfat_alloc_cluster_near(vol, last_cluster, n, &new_cluster)`, link into FAT chain (or extend contiguous range for NoFatChain files -- if not contiguous any more, clear NoFatChain flag and build FAT chain from scratch). Update `DataLength` and `ValidDataLength` in the Stream Extension; recompute `SetChecksum` for the entire entry set; write the updated entry set back to its directory sector. Truncation: free tail clusters from FAT, clear bitmap bits, update `DataLength`.

- [ ] `exfat_file_write(vol, inode, offset, buf, len)`: within-cluster: read-modify-write; beyond-end: allocate new clusters; update `DataLength` and `ValidDataLength`; recompute entry-set `SetChecksum`; write updated directory entry
- [ ] `exfat_file_truncate(vol, inode, new_length)`: if shorter → free tail cluster chain from FAT + bitmap; update `DataLength`
- [ ] `exfat_update_dir_entry(vol, inode)`: write updated File Entry + Stream Extension + Filename extensions back to their original `file_entry_sector` / `file_entry_offset`; recompute `SetChecksum`
- [ ] FAT chain extension: `exfat_fat_chain_append(vol, inode, new_cluster)`: set `FAT[old_last] = new_cluster`; `FAT[new_cluster] = 0xFFFFFFFF`
- [ ] NoFatChain promotion: if new cluster is not contiguous with the previous last cluster: clear `EXFAT_FLAG_NOFATCHAIN`; rebuild FAT chain for the entire extent list; update Stream Extension flags
- [ ] VFS `write` callback: `exfat_vfs_write(vfs_node, buf, offset, len)` → `exfat_file_write()`
- [ ] Commit: `"fs/exfat: file write -- within-cluster, cluster append, truncate, DataLength/SetChecksum update"`

## 8. File Create / Delete / Rename `[Sonnet]`

Write a complete File/Stream/Filename entry set in the parent directory. Free cluster chains and bitmap bits on delete. Atomic move within the same volume.

**Files:** `src/kernel/fs/exfat/exfat_dir.c` (extend)

> [!NOTE]
> Create: find 1 + SecondaryCount free consecutive directory slots in the parent cluster chain (if none: extend the directory chain by allocating a new cluster); write File Entry, Stream Extension, then N Filename Extension entries. For a new empty file: `FirstCluster = 0`, `DataLength = 0`. `SecondaryCount = 1 + ceil(name_len / 15)`. `SetChecksum` must be computed and written in the File Entry. Delete: mark File Entry type as `type & ~0x80` (e.g., `0x85 → 0x05`); do the same for each secondary entry; free the cluster chain via `exfat_free_cluster()`. Rename: delete old entry set; write new entry set in target directory (may be different directory).

- [ ] `exfat_find_free_slots(vol, dir_inode, count, &sector, &offset)`: walk dir cluster chain; find `count` consecutive free or deleted slots
- [ ] `exfat_dir_create_entry(vol, dir_inode, name_utf8, attrs, &new_inode)`: build entry set in memory; compute `SetChecksum`; write to free slots; return inode
- [ ] `exfat_dir_delete_entry(vol, dir_inode, inode)`: mark all entries in set as deleted; `exfat_free_cluster(vol, inode->first_cluster, chain_len)`; `exfat_bitmap_flush(vol)`
- [ ] `exfat_dir_rename(vol, old_dir, old_inode, new_dir, new_name)`: delete old entry set; write new entry set in `new_dir` with `new_name`; copy `first_cluster` and size fields from old inode
- [ ] VFS callbacks: `exfat_vfs_create`, `exfat_vfs_unlink`, `exfat_vfs_rename` → call the above
- [ ] Commit: `"fs/exfat: create/delete/rename -- entry set write, SetChecksum, bitmap cleanup, VFS callbacks"`

## 9. Timestamp Encoding `[Sonnet]`

Convert between `FILETIME` (100 ns ticks since 1601 UTC) and exFAT binary timestamps (2 s granularity + 10 ms offset field + UTC offset byte).

**Files:** `src/kernel/fs/exfat/exfat_time.c` (new)

> [!NOTE]
> exFAT timestamp (4 bytes): bits 31–25 = year since 1980 (0–127 → max 2107); bits 24–21 = month (1–12); bits 20–16 = day (1–31); bits 15–11 = hours (0–23); bits 10–5 = minutes (0–59); bits 4–0 = 2-second counts (0–29). `UtcOffset` byte (1 byte): if bit 7 set, the offset field (bits 6–0) is the UTC offset in 15-minute increments (signed, two's complement). `10msIncrement` byte: additional 10 ms precision (0–199); added to the 2-second base. Total precision: 10 ms. File Entry has 3 timestamp fields: `CreateTime`, `LastModifiedTime`, `LastAccessedTime`; each has a corresponding `10msIncrement` and `UtcOffset`.

- [ ] `exfat_encode_timestamp(filetime_utc, &ts4, &ms10, &utc_off)`: convert `FILETIME` → exFAT 4-byte field + 10 ms increment + UTC offset; read timezone bias from `HKLM\SYSTEM\TimeZone\BiasMinutes`; clamp year > 2107
- [ ] `exfat_decode_timestamp(ts4, ms10, utc_off)` → `FILETIME` (UTC); handle `UtcOffset` bit 7 presence/absence; convert to 100 ns FILETIME ticks
- [ ] Wire into §7 (`exfat_update_dir_entry`): update `LastModifiedTime` on write; `LastAccessedTime` on read (date only, not time); `CreateTime` on create
- [ ] Commit: `"fs/exfat: timestamp encoding -- FILETIME↔exFAT binary, 10ms precision, UTC offset, year-2107 clamp"`

## 10. VFS Registration + Probe + fsck `[Sonnet]`

Register exFAT with `vfs_probe()`. Wire dirty-volume auto-fsck. Implement `exfat_fsck()`. Add `chkdsk <drive> /exfat` shell command.

**Files:** `src/kernel/fs/exfat/exfat_vfs.c` (new), `src/shell/cmd_chkdsk.c` (extend)

> [!NOTE]
> `exfat_probe(blkdev)`: read sector 0; check bytes 3–10 for `"EXFAT   "`; return 1 (match) or 0. `exfat_mount()`: parse VBR; load bitmap; load Up-Case Table; scan root directory for Allocation Bitmap and UpCase entries; mount VFS; if `VolumeFlags & VOLUME_IS_DIRTY`: run `exfat_fsck(vol, fix=1)` before returning handle. VFS driver vtable: implement all 14 `vfs_fs_driver` callbacks. `exfat_fsck()` algorithm: walk all directory cluster chains; for each file entry: follow cluster chain or validate contiguous range; check against bitmap (each reached cluster must be set); detect cross-linked chains via `visited[]` bitset; detect orphaned clusters (set in bitmap but unreachable from any directory); count and report.

- [ ] `exfat_probe(blkdev_t *dev)` → read sector 0; return 1 if `buf[3..10] == "EXFAT   "`, 0 otherwise
- [ ] `exfat_init(blkdev_t *dev)` → `exfat_vbr_parse()` + `exfat_bitmap_load()` + `exfat_upcase_load()` + root dir scan for special entries; return `exfat_volume_t*`
- [ ] `exfat_get_driver()` / `exfat_get_root()` → for use by `vfs_probe()` (same pattern as `fat32_get_driver()`/`fat32_get_root()`)
- [ ] VFS driver vtable: `open`, `close`, `read`, `write`, `finddir`, `readdir`, `create`, `unlink`, `rename`, `mkdir`, `rmdir`, `stat`, `flush`, `setattr` -- all delegating to `exfat_*` functions
- [ ] `exfat_fsck(vol, fix)`: `visited[]` bitset; walk all directories; collect cluster chains; compare against bitmap; cross-link detection; orphan detection; if `fix`: clear orphaned bits in bitmap; report `[exFAT] fsck: %u errors, %u orphaned clusters`
- [ ] `chkdsk D: /exfat` → call `exfat_fsck(vol, fix=0)`; `/fix` flag calls with `fix=1`
- [ ] Clean unmount: `exfat_unmount()` → `exfat_bitmap_flush()` → clear `VolumeFlags.VolumeDirty` bit in VBR + backup VBR
- [ ] Add to `vfs_probe()` probe chain at step 4 (after FAT32, before ext4)
- [ ] Log: `[exFAT] Mounted %c: "%s", %llu clusters, cluster_size=%u`
- [ ] Commit: `"fs/exfat: VFS registration -- probe, mount, dirty-volume fsck, chkdsk /exfat, clean unmount"`

## 11. Unicode Edge Cases `[Sonnet]`

Filenames up to 255 UTF-16 characters. Emoji and CJK round-trip correctly. `NameHash` validated. Illegal characters rejected with `STATUS_OBJECT_NAME_INVALID`.

**Files:** `src/kernel/fs/exfat/exfat_dir.c` (extend), `src/kernel/fs/exfat/exfat_upcase.c` (extend)

> [!NOTE]
> Illegal filename characters (per exFAT spec §7.7): `0x00–0x1F` (control chars), `"`, `*`, `/`, `:`, `<`, `>`, `?`, `\`, `|`. Also `.` and `..` are not valid file names (they are implicit). Surrogate pairs for emoji: UTF-16LE uses surrogate pairs for codepoints > U+FFFF (e.g., emoji U+1F600 = `0xD83D 0xDE00`); decode to a single Unicode codepoint for UTF-8 output. CJK: codepoints in the range U+4E00–U+9FFF, U+3040–U+30FF; `exfat_upcase()` returns identity for these (no case folding). Maximum filename length: 255 UTF-16 codepoints = up to 17 Filename Extension entries (15 chars each, rounded up).

- [ ] `exfat_name_validate(name_utf8)`: scan for illegal characters; return `STATUS_OBJECT_NAME_INVALID` on violation; reject empty names
- [ ] `exfat_utf8_to_utf16(name_utf8, buf_utf16, max_codepoints)`: handle surrogate pairs for U+10000+; return codepoint count or -EILSEQ on invalid UTF-8
- [ ] Verify `exfat_name_hash()` produces correct result for names with CJK and emoji characters; compare against Windows-computed hash for the same name
- [ ] 255-char name test: create file with 255 Japanese characters; read back; verify round-trip
- [ ] Commit: `"fs/exfat: Unicode -- 255-char names, emoji/CJK round-trip, surrogate pairs, illegal char reject"`

## 12. Large File Support (> 4 GiB) `[Sonnet]`

Verify 64-bit `DataLength` and `ValidDataLength` in the Stream Extension are handled correctly throughout the read, write, and truncate paths. Test with a 5 GiB sparse file.

**Files:** `src/kernel/fs/exfat/exfat_io.c` (audit), `src/kernel/fs/exfat/exfat_dir.c` (audit)

> [!NOTE]
> exFAT's primary advantage over FAT32: `DataLength` in the Stream Extension is 64-bit, enabling files > 4 GiB. FAT32 uses a 32-bit `FileSize` field, capping at 4 GiB − 1 byte. Cluster index arithmetic must use 64-bit types throughout (`uint64_t cluster_idx = offset / bytes_per_cluster`). `ValidDataLength` tracks how far the file has been written; bytes in `[ValidDataLength, DataLength)` are zero without disk I/O. Test strategy: use a sparse file (extend `DataLength` to 5 GiB without allocating most clusters) to avoid needing 5 GiB of actual disk space.

- [ ] Audit all offset and length computations: ensure `uint64_t` used for `offset`, `length`, `DataLength`, `ValidDataLength`, cluster index; no 32-bit truncation
- [ ] `exfat_file_extend(vol, inode, new_length)`: extend `DataLength` without allocating clusters (sparse); set `ValidDataLength` to current write position; write zeros for unallocated ranges on read
- [ ] Test: `exfat_test_large_file()`: create file; extend to 5 GiB (`DataLength = 5 * 1024^3`); write 4 KiB at offset 0, 4 KiB at offset 4 GiB; read back both; verify zeros in unwritten ranges; `DataLength == 5 GiB` in directory entry
- [ ] Ensure `GetFileSizeEx()` on exFAT file > 4 GiB returns correct 64-bit size
- [ ] Commit: `"fs/exfat: large file support -- 64-bit DataLength/ValidDataLength, 5 GiB sparse test"`

---

## OS Comparison


| ⭐  | Feature                                              | 🪟 Win11                                                | 🐧 Linux                                                                 | 🚀 Impossible OS                                                            |
| --- | ---------------------------------------------------- | ------------------------------------------------------- | ------------------------------------------------------------------------ | --------------------------------------------------------------------------- |
| 💎  | VBR parse + boot checksum + backup VBR fallback      | ✅ `exfatfs.sys`; full VBR + 11-sector                  | ✅ `exfat.ko` (Linux 5.7+); VBR parse                                    | ⬜ §1 -- CRC32 11-sector checksum, backup VBR                               |
| 💎  | Allocation Bitmap                                    | ✅ `exfatfs.sys`; bitmap maintained in sync             | ✅ `exfat.ko`; bitmap alloc + free                                       | ⬜ §2 -- `exfat_alloc_cluster()`, first-fit scan, `vol->bitmap_dirty` flush |
| 💎  | Up-Case Table                                        | ✅ `exfatfs.sys`; Up-Case table verified +              | ✅ `exfat.ko`; Up-Case table loaded; `exfat_utf16_cmp()`                 | ⬜ §3 -- CRC32 verify, 65 536-entry cache,                                  |
| 💎  | Directory entry sets                                 | ✅ `exfatfs.sys`; full entry set atomicity;             | ✅ `exfat.ko`; entry set parse; `exfat_build_inode()`                    | ⬜ §4 -- 3-type entry set, SetChecksum compute/verify,                      |
| 💎  | Directory read                                       | ✅ Full `FindFirstFileW` support over exFAT             | ✅ `exfat.ko`; `exfat_iterate()` readdir                                 | ⬜ §5 -- FAT-chain dir walk, entry-set assembly,                            |
| 💎  | File read                                            | ✅ `exfatfs.sys`; `NoFatChain` respected for sequential | ✅ `exfat.ko`; `EXFAT_FLAG_CONTIGUOUS`; fast path for                    | ⬜ §6 -- dual path: FAT-chain walk vs.                                      |
| 💎  | File write                                           | ✅ `exfatfs.sys`; full write path; `ValidDataLength`    | ✅ `exfat.ko`; `exfat_write_begin/end()`; cluster chain extend           | ⬜ §7 -- cluster append + FAT extend,                                       |
| 💎  | File create/delete/rename                            | ✅ `exfatfs.sys`; full CRUD; transactional-ish (no      | ✅ `exfat.ko`; `exfat_create()`, `exfat_unlink()`, `exfat_rename()`      | ⬜ §8 -- consecutive free-slot search, entry-set write,                     |
| 💎  | Timestamps                                           | ✅ `exfatfs.sys`; 10 ms + UTC                           | ✅ `exfat.ko`; `exfat_set_time()` / `exfat_get_inode_time()`; UTC        | ⬜ §9 -- `exfat_encode/decode_timestamp()`, UTC offset, 10 ms               |
| 💎  | VFS registration + dirty auto-fsck + `chkdsk /exfat` | ✅ `exfatfs.sys`; dirty flag auto-repair at             | ✅ `exfat.ko`; `EXFAT_SB_DIRTY` check on mount;                          | ⬜ §10 -- `exfat_probe()` in `vfs_probe()`, dirty auto-fsck,                |
| 💎  | Unicode                                              | ✅ Full Unicode; all cases handled                      | ✅ `exfat.ko`; full UTF-8/UTF-16 conversion; `exfat_validate_filename()` | ⬜ §11 -- surrogate-pair decode, CJK identity upcase,                       |
| 💎  | Large files > 4 GiB                                  | ✅ `exfatfs.sys`; 64-bit sizes; primary advantage       | ✅ `exfat.ko`; `i_size` is 64-bit; `ValidDataLength`                     | ⬜ §12 -- all offsets `uint64_t`, 5 GiB                                     |

> **After §1–12:** Impossible OS achieves full interoperability with every Windows-formatted exFAT USB drive and SDXC card. There is no architectural differentiation claim here -- the goal is spec-correct conformance with the published Microsoft exFAT specification (open-sourced 2019). The clean-room implementation using only the public spec is legally clean and functionally equivalent to the proprietary `exfatfs.sys` / `exfat.ko` implementations for all standard use cases.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] VBR: corrupt primary VBR checksum → `[exFAT] primary VBR invalid, using backup`; corrupt both → mount fails cleanly
- [ ] Allocation Bitmap: `exfat_alloc_cluster(1)` → bit set in bitmap; `exfat_free_cluster()` → bit cleared; `free_clusters` accurate after create+delete cycle
- [ ] Up-Case Table: `exfat_name_hash(vol, "Hello")` == `exfat_name_hash(vol, "hello")` (case-insensitive); hash matches value stored in Windows-formatted drive's entry
- [ ] Directory read: mount Windows-formatted exFAT USB drive in QEMU; `FindFirstFileW("E:\\*.*")` enumerates all files; filenames with spaces, dots, and Unicode chars correct
- [ ] File read (FAT-chain): read a fragmented file; content matches bytes written by Windows; `NoFatChain` path: read a large contiguous file; verify no FAT traversal (log shows contiguous path)
- [ ] File write: `WriteFile` to exFAT; unmount; remount; `ReadFile` returns identical bytes; `DataLength` correct in directory entry; `SetChecksum` valid
- [ ] Create/delete/rename: create `"test.txt"` → appears in `FindFirstFileW`; delete → no longer visible; rename `"a.txt"` → `"b.txt"` → `FindFirstFileW` returns `"b.txt"` only
- [ ] Timestamps: create file; `GetFileTime` → creation time within 10 ms of system time; `SetFileTime`; remount; `GetFileTime` returns set value
- [ ] Dirty volume: set `VOLUME_IS_DIRTY` flag; mount → auto-fsck runs; log shows `[exFAT] fsck: 0 errors`; flag cleared after clean fsck
- [ ] `chkdsk E: /exfat` on clean volume → `0 errors`; inject cross-linked chain → reports error; `/fix` resolves
- [ ] Unicode: create file named `"🎮テスト.txt"`; `FindFirstFileW` returns correct name; read/write data; no corruption
- [ ] Large file: `exfat_file_extend(vol, inode, 5GB)`; write at offset 0 and offset 4 GiB; read back; zeros between; `GetFileSizeEx` → 5 GiB
- [ ] Commit: `"fs/exfat: complete exFAT R/W driver -- VBR, bitmap, upcase, entry sets, read, write, CRUD, Unicode, large files"`
