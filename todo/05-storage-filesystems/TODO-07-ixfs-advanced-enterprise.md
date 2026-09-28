---
schema_version: 1
id: ixfs-advanced-enterprise
domain: 05-storage-filesystems
status: active
title: "TODO-07 -- IXFS Advanced Storage, Reliability & Enterprise"
---

# TODO-07 -- IXFS Advanced Storage, Reliability & Enterprise

> **Goal:** Deliver the features that make IXFS genuinely superior to NTFS and competitive with ZFS/Btrfs: transparent LZ4/Zstd compression, inline block deduplication, reflink instant copy, online defragmentation, sparse files, real-time TRIM, online volume resize, self-healing metadata, automatic scheduled snapshots, per-file AES-256-XTS encryption, USN change journal, disk quotas, filesystem-native storage tiering, a volume health dashboard, and a comprehensive test suite.

> [!IMPORTANT]
> All sections depend on the v3 inode from `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §2` being on disk (`IXFS_VERSION=3`, 256-byte inodes). §10 (encryption) uses Monocypher and §1 (compression) uses LZ4/Zstd from `02-kernel-core/TODO-03-kernel-libraries.md` (kernel libraries -- LZ4/Zstd, Monocypher). §6 (TRIM) builds on `blkdev_discard()` which already exists in `src/kernel/drivers/blkdev.c:125`. Do **not** begin §7 (online resize), §8 (self-healing), or §13 (storage tiering) before §3 (reflink) and §4 (defrag) are stable -- they all manipulate the extent tree and block allocator. Delete `todo-old/010-Kernel-Foundations/TODO-040-Filesystem/TODO-040.11-IXFS.md` after this TODO file is created.

## Inputs

- `src/kernel/fs/ixfs/ixfs_alloc.c` -- block allocator; §4 (defrag), §6 (TRIM), §7 (resize), §13 (tiering) all extend it
- `src/kernel/fs/ixfs/ixfs_cow.c` -- CoW refcount table; §2 (dedup), §3 (reflink), §5 (sparse CoW), §9 (auto-snapshot) reuse it
- `src/kernel/fs/ixfs/ixfs_extent.c` -- extent engine; §1 (compression per-4KiB), §3 (reflink), §4 (defrag), §5 (hole extents), §7 (resize) extend it
- `src/kernel/fs/ixfs/ixfs_journal.c` -- WAL journal; all destructive operations must be journaled
- `src/kernel/drivers/blkdev.c` -- `blkdev_discard(dev, lba, count)` already exists; §6 wires IXFS into it
- → XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md` -- v3 inode with `i_compress_type`, `i_encrypt_key_id`, `i_flags` (sparse/immutable), `i_access_count` (tiering) must be in place
- -> XREF: `02-kernel-core/TODO-03-kernel-libraries.md` §3 (LZ4) + §9 (Zstd) -- the kernel-libraries codecs §1 compression depends on; LZ4 source is vendored, Zstd §9 is filed and source-pending. Monocypher (D02 T03 §5) for AES-256-XTS and PBKDF2; xxHash64 for dedup hashing (vendored with LZ4). Until §3/§9 ship, stub §1 and §10 behind compile-time feature flags
- → XREF: `09-desktop-shell` domain -- Disk Manager UI panels (§14 health dashboard, §4 defrag button, §9 Previous Versions, §12 quota panel, §13 tier config) are desktop components; coordinate on the IPC/message interface used to query IXFS stats
- → XREF: `04-drivers-hardware/TODO-13-storage-controller-device-drivers.md §7` -- AHCI SMART data feeds the health dashboard (§14) disk temperature + error count fields

## Outcome

- LZ4/Zstd per-4KiB compression with `FSCTL_SET_COMPRESSION`; write-time cost gate (≥10% ratio).
- xxHash64 inline dedup reusing CoW refcounts; near-zero overhead; dedup ratio logged on unmount.
- `CopyFileW` same-volume IXFS → reflink (O(1) regardless of file size); CoW on first write.
- Background defrag thread; `FSCTL_DEFRAGMENT_FILE`; `defrag` CLI; Disk Manager button.
- Sparse file hole extents; `FSCTL_SET_SPARSE`/`FSCTL_SET_ZERO_DATA`/`FSCTL_QUERY_ALLOCATED_RANGES`.
- Real-time TRIM: journal-batched, coalesced; on by default for SSDs, off for HDDs.
- `ixfs_grow()` online volume resize; Disk Manager "Extend Volume".
- Self-healing: backup superblock at last block; automatic restore on primary CRC failure.
- Auto snapshots with configurable interval + retention policy; Explorer "Previous Versions" tab.
- Per-file AES-256-XTS encryption; PBKDF2 master key; `FILE_ATTRIBUTE_ENCRYPTED`.
- USN change journal persisted across reboots; `FSCTL_QUERY_USN_JOURNAL`/`FSCTL_READ_USN_JOURNAL`.
- Volume quotas; `STATUS_DISK_FULL` on limit; Disk Manager quota panel.
- Multi-device storage tiering; background promote/demote; Disk Manager tier config.
- Volume health dashboard: superblock, journal, checksum, fragmentation, snapshot, dedup, encryption stats.
- All features covered by `ixfs_test.c` comprehensive suite; clean `bash scripts/build.sh clean run`.

## Implementation Order

| ⭐  | Order | Deliverable                                                                              | Depends On                                                        | Status |
| --- | :---: | ---------------------------------------------------------------------------------------- | ----------------------------------------------------------------- | :----: |
| 💎  |   1   | §5 Sparse files -- hole extents, `FSCTL_SET_SPARSE/SET_ZERO_DATA/QUERY_ALLOCATED_RANGES`  | v3 inode `i_flags` (TODO-06 §2)                                  |  [ ]   |
| ⭐  |   2   | §6 Real-time TRIM -- journal-batched discard, SSD auto-detect, `blkdev_discard()`         | `blkdev_discard()` exists; §1 (hole extents inform TRIM ranges)  |  [ ]   |
| ⭐  |   3   | §1 Transparent compression -- LZ4/Zstd 4KiB units, write gate ≥10%, `FSCTL_SET_COMPRESSION` | extent engine; kernel-libraries TODO for codecs              |  [ ]   |
| ⭐  |   4   | §2 Inline block dedup -- xxHash64, `dedupe_ht`, CoW refcount reuse, per-volume opt-in     | §3 (compression changes write path; dedup runs after compress)   |  [ ]   |
| ⭐  |   5   | §3 Reflink instant copy -- `ixfs_reflink()`, O(1) `CopyFileW` same-volume, CoW on write  | §4 (dedup + CoW refcounts must be stable)                        |  [ ]   |
| ⭐  |   6   | §4 Online defrag -- `ixfs_defrag_file()`, background idle thread, `FSCTL_DEFRAGMENT_FILE`  | §5 (reflink stable before extent rewrite path used by defrag)    |  [ ]   |
| ⭐  |   7   | §7 Online volume resize -- `ixfs_grow()`, bitmap extend, superblock update                | §6 (defrag stable before extending block ranges)                 |  [ ]   |
| ⭐  |   8   | §8 Self-healing metadata -- backup superblock, auto-restore on CRC fail                   | §7 (final volume layout stable before backup SB location fixed)  |  [ ]   |
| 💎  |   9   | §9 Auto snapshots + Explorer Previous Versions -- schedule, retention, prune              | §8 (self-healing ensures snapshot metadata survives)             |  [ ]   |
| ⭐  |  10   | §10 Per-file AES-256-XTS encryption -- key table, PBKDF2, per-block encrypt/decrypt       | §8 (backup SB for key table resilience); Monocypher              |  [ ]   |
| ⭐  |  11   | §11 USN change journal -- circular buffer in reserved region, 5 reason codes, `FSCTL_*`   | §7 (resize stable; USN region locked at format time)             |  [ ]   |
| 💎  |  12   | §12 Volume quotas -- quota table, `ixfs_alloc_block` check, `FSCTL_GET/SET_VOLUME_QUOTA`  | §7 (resize changes block counts; quota check needs stable alloc) |  [ ]   |
| ⭐  |  13   | §13 Storage tiering -- multi-device superblock, promote/demote thread, Disk Manager panel | §6 (defrag migration is the same block-copy mechanism)           |  [ ]   |
| ⭐  |  14   | §14 Volume health dashboard -- Disk Manager panel, stats, one-click actions               | §1–13 all complete (all stats sources must exist)                |  [ ]   |
| ⭐  |  15   | §15 Comprehensive test suite -- all features, journal recovery, QEMU clean run            | §1–14 all complete                                               |  [ ]   |
| 🔥  |  16   | §16 Inode-table + on-disk-layout hardening: mount validation, restore/unlink atomicity     | --                                                                 |  [ ]   |

> Sections §1–§13 are all `⭐` exclusive or represent clear superior positioning: no single general-purpose filesystem ships transparent per-file compression + inline dedup + reflinks + AES-256-XTS encryption + filesystem-native tiering + USN journal + self-healing all in one volume. NTFS has compression and EFS but not dedup/reflinks/tiering. Btrfs has compression/dedup/snapshots but not per-file encryption or tiering. ZFS has most features but requires a pool layer and 300 MB RAM overhead. IXFS delivers all of them at the block driver abstraction level with CoW-reuse for near-zero dedup RAM cost.

---

## 1. Sparse File Support `[Opus]`

Implement hole extents (a special extent type marking unallocated data ranges), `FSCTL_SET_SPARSE`, `FSCTL_SET_ZERO_DATA`, and `FSCTL_QUERY_ALLOCATED_RANGES`.

**Files:** `src/kernel/fs/ixfs/ixfs_extent.c` (extend), `src/kernel/fs/ixfs/ixfs_sparse.c` (new), `include/kernel/fs/ixfs.h` (extend)

> [!NOTE]
> Hole extent marker: `e_start = 0 (reserved), e_flags |= IXFS_EXTENT_HOLE, e_count = N` where N is the number of 4 KiB blocks the hole spans. Read path: on encountering a hole extent, `memset(buf, 0, N*4096)` with no disk I/O. Write into hole: allocate blocks normally, replace hole extent. `FSCTL_SET_ZERO_DATA(FileHandle, {FileOffset, BeyondFinalZero})`: free blocks in that byte range; insert hole extents. `FSCTL_QUERY_ALLOCATED_RANGES(FileHandle, range, buf)`: return list of `FILE_ALLOCATED_RANGE_BUFFER` pairs covering non-hole extents. `FILE_ATTRIBUTE_SPARSE_FILE` set in `i_win32_attrs`. `ixfs_stat.actual_blocks` excludes hole ranges.

- [ ] `IXFS_EXTENT_HOLE (1 << 7)` flag in `e_flags`; update `ixfs_block_map()` to skip hole extents (return NULL block address → caller zeros buffer)
- [ ] `FSCTL_SET_SPARSE(hFile)` → set `FILE_ATTRIBUTE_SPARSE_FILE` in `i_win32_attrs`; journal the attribute change
- [ ] `ixfs_punch_hole(vol, inode, byte_offset, byte_len)`: walk extents overlapping range; for each block: if refcount == 1 free block; if refcount > 1 (CoW-shared): decrement refcount only; replace extent range with hole extent; journal
- [ ] `FSCTL_SET_ZERO_DATA(hFile, offset, length)` → `ixfs_punch_hole()` if `FILE_ATTRIBUTE_SPARSE_FILE` set; else `memset` write (retain blocks)
- [ ] `FSCTL_QUERY_ALLOCATED_RANGES(hFile, query_range, out_buf, buf_len)`: iterate extent list; emit `FILE_ALLOCATED_RANGE_BUFFER {FileOffset, Length}` for every non-hole extent within the query range
- [ ] `ixfs_stat()` update: `actual_blocks` = total extents minus hole extent counts
- [ ] Add to `ixfs_test.c`: write 10 MB; punch 5 MB hole in middle; read back (zeros in hole, data outside); `QUERY_ALLOCATED_RANGES` shows two ranges; sparse + CoW: share block between file and snapshot; punch hole in file → snapshot block intact
- [ ] Commit: `"ixfs: sparse files -- hole extents, FSCTL_SET_SPARSE/SET_ZERO_DATA/QUERY_ALLOCATED_RANGES, CoW-safe punch"`

## 2. Real-Time TRIM `[Sonnet]`

Batch freed blocks within journal transactions. Coalesce adjacent ranges. Issue `blkdev_discard()` at `ixfs_txn_commit`. Auto-detect SSD via ATA IDENTIFY. Add `IXFS_MOUNT_DISCARD` option.

**Files:** `src/kernel/fs/ixfs/ixfs_alloc.c` (extend), `src/kernel/fs/ixfs/ixfs_journal.c` (extend)

> [!NOTE]
> TRIM strategy: during a journal transaction, collect freed block ranges in a `trim_pending[]` list (max 64 entries) in `ixfs_txn_t`. On `ixfs_txn_commit()`, after writing the commit record, coalesce adjacent entries (`if (a.end == b.start)` merge), then iterate and call `blkdev_discard(dev, lba, count)` for each range. SSD detection: at mount, call `blkdev_identify(dev)` → check ATA IDENTIFY word 217 (`TRIM support`); set `vol->mount_flags |= IXFS_MOUNT_DISCARD` automatically. HDD or unknown: default off. Mount option `ixfs_mount_flags` can override.

- [ ] Add `trim_ranges[64]` and `trim_count` to `ixfs_txn_t`
- [ ] `ixfs_free_block(vol, block)`: after bitmap clear + journal record, if `txn->trim_count < 64`: add `{block, 1}` to `trim_ranges[]`; else: flush trim list immediately
- [ ] `ixfs_txn_commit()`: after commit record written, sort `trim_ranges[]` by start; merge adjacent; for each range: `blkdev_discard(vol->dev, lba, count)`; clear list
- [ ] SSD auto-detect: in `ixfs_init()`: call `blkdev_identify(dev)` if available; check TRIM support bit; set `IXFS_MOUNT_DISCARD` accordingly; log `[IXFS] TRIM: %s`, enabled/disabled
- [ ] Registry override: `HKLM\SYSTEM\Storage\IXFS\DisableDiscard (REG_DWORD)` = 1 forces off
- [ ] Commit: `"ixfs: real-time TRIM -- journal-batched discard, SSD auto-detect, 64-range coalesce"`

## 3. Transparent LZ4/Zstd Compression `[Opus]`

Per-4KiB compression unit on the write path. Skip compression if ratio < 10%. Decompress transparently on read. Wire `FSCTL_SET_COMPRESSION`/`FSCTL_GET_COMPRESSION`. Set `FILE_ATTRIBUTE_COMPRESSED`.

**Files:** `src/kernel/fs/ixfs/ixfs_compress.c` (new), `include/kernel/fs/ixfs.h` (extend)

> [!NOTE]
> Compression unit header (8 bytes prepended to each compressed block): `uint16_t compressed_size`; `uint8_t algo` (1=LZ4, 2=Zstd); `uint8_t flags`; `uint32_t original_size`. If `compressed_size >= original_size * 0.90` (less than 10% savings): store uncompressed with `flags |= IXFS_COMP_STORED`. The compressed data + header replaces the 4 KiB extent block; if compressed ≤ 4080 bytes, it fits in one block; else store uncompressed. Requires LZ4 and Zstd from kernel-libraries TODO (-> XREF: `02-kernel-core/TODO-03-kernel-libraries.md` §3 LZ4 + §9 Zstd). Until available: stub behind `#ifdef IXFS_COMPRESSION_ENABLED`; compile-time disabled by default.

- [ ] `ixfs_compress_block(algo, in_buf, in_len, out_buf, out_size)` → compress with LZ4 or Zstd; return compressed length or -1 if would expand
- [ ] `ixfs_decompress_block(algo, in_buf, in_len, out_buf, out_max)` → decompress; return original size
- [ ] Write path in `ixfs_write_block()`: if `i_compress_type != 0`: try `ixfs_compress_block()`; if ratio ≥ 10%: write compressed (flag block as compressed in extent `e_flags`); else write uncompressed
- [ ] Read path in `ixfs_read_block()`: if extent has `IXFS_EXTENT_COMPRESSED` flag: read raw block; detect header; `ixfs_decompress_block()`; return decompressed data
- [ ] `FSCTL_SET_COMPRESSION(hFile, COMPRESSION_FORMAT_DEFAULT)` → set `i_compress_type = 1` (LZ4); set `FILE_ATTRIBUTE_COMPRESSED`; journal; re-compress existing data on next write (lazy)
- [ ] `FSCTL_SET_COMPRESSION(hFile, COMPRESSION_FORMAT_NONE)` → clear `i_compress_type`; decompress existing blocks on next write
- [ ] `FSCTL_GET_COMPRESSION(hFile, &fmt)` → return current `i_compress_type`
- [ ] Log on unmount: `[IXFS] Compression: %llu blocks compressed, avg ratio %.1f%%`
- [ ] Commit: `"ixfs: LZ4/Zstd transparent compression -- 4KiB units, 10% gate, FSCTL_SET_COMPRESSION"`

## 4. Inline Block Deduplication `[Opus]`

xxHash64 per-4KiB block at write time. In-memory `dedupe_ht` rebuilt on mount. Collision confirmed with byte-compare. Deduplicated blocks reuse CoW refcounts.

**Files:** `src/kernel/fs/ixfs/ixfs_dedup.c` (new), `include/kernel/fs/ixfs.h` (extend)

> [!NOTE]
> Dedup hash table: `dedupe_ht[DEDUP_HT_SIZE]` where each bucket chains `{ uint64_t xxhash; uint32_t block_num; }` entries. `DEDUP_HT_SIZE = 65536` (256 KiB for the table). Collision handling: on hash match, perform `memcmp(new_data, existing_block_data, 4096)` before sharing. On confirmed dedup: discard the just-allocated new block, point the extent entry at the existing block, increment the existing block's CoW refcount. Dedup ratio logged on unmount. **Not enabled by default for volumes < 1 GiB** (insufficient data for meaningful dedup). Configurable via `HKLM\SYSTEM\Storage\IXFS\EnableDedup`.

- [ ] `ixfs_dedup_ht_init(vol)`: `kmalloc(DEDUP_HT_SIZE * sizeof(dedup_entry_t*))`; insert existing allocated blocks on mount (`ixfs_dedup_rebuild()`)
- [ ] `ixfs_dedup_lookup(vol, xxhash, buf)` → probe `dedupe_ht[xxhash % HT_SIZE]`; for each chain entry with matching hash: `memcmp` confirm; return block_num or 0
- [ ] `ixfs_dedup_insert(vol, xxhash, block_num)` → insert into hash table
- [ ] Write path in `ixfs_alloc_block()` (after compression, if enabled): compute `xxhash64(buf, 4096)`; call `ixfs_dedup_lookup()`; on match: `ixfs_refcount_inc(existing_block)`; skip disk write; update extent to point at existing block; free the newly allocated block
- [ ] `ixfs_dedup_remove(vol, block_num)` → called on block free; remove from hash table
- [ ] Unmount log: `[IXFS] Dedup: %llu blocks deduped, %llu blocks saved, ratio %.1f:1`
- [ ] Enabled gate: check `vol->sb.s_total_blocks * IXFS_BLOCK_SIZE >= 1 GiB`; else skip dedup silently
- [ ] Commit: `"ixfs: inline block dedup -- xxHash64, byte-compare confirm, CoW refcount reuse, dedup_ht"`

## 5. Reflink Instant Copy `[Opus]`

`ixfs_reflink()` creates a new inode sharing the source's extent list. Increment refcount of every shared block. Wire into `CopyFileW` for same-volume IXFS. CoW fires automatically on first write.

**Files:** `src/kernel/fs/ixfs/ixfs_cow.c` (extend), `src/kernel/win32/fileops.c` (extend CopyFileW)

> [!NOTE]
> Reflink = copy-on-write clone at inode granularity. The new inode gets an identical copy of the source's extent list; the physical blocks are shared. Every shared block's refcount is incremented (using the existing CoW refcount table). A write to either file triggers `ixfs_cow_block()` for the specific block being written, creating an independent copy. Reflink time is O(N extents), not O(file size) -- for a 1 GiB file with 2 extents, reflink takes ~microseconds. `CopyFileW` on same-volume IXFS source and destination: detect via drive letter and call `ixfs_reflink()` instead of the byte-copy loop.

- [ ] `ixfs_reflink(vol, src_inode_num, dst_dir_inode, dst_name)`: create new inode; copy `i_extents[]` and `i_extent_count` from source; for each extent block: `ixfs_refcount_inc(block)`; journal both the new inode and refcount increments; set `dst->i_size = src->i_size`; add dir entry; return dst inode number
- [ ] `CopyFileW` same-volume detection: in `fileops.c`, if `src_letter == dst_letter` and `vfs_probe_type(src_letter) == FS_IXFS`: call `ixfs_reflink()` → return TRUE; else fall through to existing byte-copy loop
- [ ] Verify `ixfs_cow_block()` fires correctly when either the original or clone is written (CoW already implemented in `ixfs_cow.c`)
- [ ] `ixfs_stat()` must not double-count shared blocks -- `actual_blocks` should count only blocks where refcount == 1 (exclusively owned)
- [ ] Test: reflink 1 GiB file → instant (< 10 ms); write 1 byte to clone → only that block CoW'd; original unchanged; delete clone → refcounts decremented; delete original → last-refcount blocks freed
- [ ] Commit: `"ixfs: reflink instant copy -- O(1) extent clone, CoW on write, CopyFileW same-volume hook"`

## 6. Online Defragmentation `[Opus]`

`ixfs_defrag_file()`: skip if ≤2 extents, allocate contiguous range, copy data, update extent list, free old blocks. Background SCHED_IDLE thread. `FSCTL_DEFRAGMENT_FILE`. `defrag` CLI.

**Files:** `src/kernel/fs/ixfs/ixfs_defrag.c` (new), `src/kernel/sched/` (background thread)

> [!NOTE]
> Defrag algorithm: (1) skip if `i_extent_count ≤ 2`; (2) compute total block count `N`; (3) `ixfs_alloc_block_near(vol, first_extent.e_start, N)` -- allocate N contiguous blocks near the file's current location; (4) copy all data blocks in extent order to new contiguous range; (5) in a single journal transaction: update `i_extents[]` to a single entry covering the new range; free each old block (respecting CoW refcounts -- do not free blocks with refcount > 1); (6) commit. CoW-safety: shared blocks (from reflink or snapshot) are copied to new blocks during defrag -- this increases their refcounts on the old copy, not the new one.

- [ ] `ixfs_defrag_file(vol, inode_num)`: check extent count; allocate contiguous range; copy; update extents; free old; journal; log `[IXFS] Defrag: inode %u %u→1 extents`
- [ ] `ixfs_alloc_block_near(vol, hint_block, count)`: attempt to allocate `count` contiguous blocks starting at or near `hint_block`; fall back to any contiguous range if unavailable
- [ ] Background thread: SCHED_IDLE priority; sleep 30 s; wake; walk inode table; collect inodes with `i_extent_count > 4`; defrag each in sequence; re-sleep; respect `HKLM\SYSTEM\Storage\IXFS\AutoDefrag` (default 1)
- [ ] `FSCTL_DEFRAGMENT_FILE(hFile)` → `ixfs_defrag_file(vol, inode)` synchronously on the calling thread
- [ ] `defrag C:` shell command: `ixfs_defrag_file` on all fragmented inodes; print progress `Defragmenting... %u files, %u extents eliminated`
- [ ] Disk Manager "Defragment" button → same as `defrag C:` + show progress in panel
- [ ] Commit: `"ixfs: online defrag -- ixfs_defrag_file, contiguous alloc, background IDLE thread, FSCTL_DEFRAGMENT_FILE"`

## 7. Online Volume Resize `[Opus]`

`ixfs_grow(vol, new_total_blocks)`: extend block bitmap, update superblock fields, recalculate block groups. Flush via journal. Wire to Disk Manager "Extend Volume".

**Files:** `src/kernel/fs/ixfs/ixfs_format.c` (extend), `src/kernel/fs/ixfs/ixfs_alloc.c` (extend)

> [!NOTE]
> Growth-only (shrink requires data relocation -- deferred). Steps: (1) validate `new_total_blocks > vol->sb.s_total_blocks`; (2) extend the block bitmap by writing new bitmap blocks at their calculated location (beyond current volume end); (3) update `s_total_blocks`, `s_free_blocks += delta`, `s_bitmap_blocks` in the superblock in a journal transaction; (4) recalculate block group descriptors (`ixfs_init_groups(vol)`); (5) flush superblock + new bitmap blocks + checksum of new blocks; (6) log and return. The underlying block device must already present the larger size (verified via `blkdev_capacity(dev)`).

- [ ] `ixfs_grow(vol, new_total_blocks)`: validate new size > current; compute new bitmap size; allocate and zero new bitmap blocks via `ixfs_write_block()`; journal superblock update; call `ixfs_init_groups(vol)` to update group descriptors; flush; log `[IXFS] Volume grown: %llu → %llu blocks`
- [ ] Verify `blkdev_capacity(dev) >= new_total_blocks * IXFS_BLOCK_SIZE / 512` before starting
- [ ] Disk Manager "Extend Volume" right-click: calls `ixfs_grow(vol, new_size_in_blocks)`; shows progress and result
- [ ] `diskpart extend fs=ixfs size=<MB>` shell: calls `ixfs_grow()`
- [ ] Test: format 100 MiB volume; grow to 200 MiB; verify `s_total_blocks` correct; write file to new region; read back; `bash scripts/build.sh clean run` passes
- [ ] Commit: `"ixfs: online volume resize -- ixfs_grow(), bitmap extension, superblock update, group recalculation"`

## 8. Self-Healing Metadata `[Sonnet]`

Write backup superblock at last volume block on every flush. On mount, if primary superblock CRC32C fails: read backup, restore if valid. Wire to Disk Manager volume health panel.

**Files:** `src/kernel/fs/ixfs/ixfs_core.c` (extend), `src/kernel/fs/ixfs/ixfs_format.c` (extend)

> [!NOTE]
> Backup superblock location: last block of the volume (`block = s_total_blocks - 1`). Written in a journal transaction on every `ixfs_flush()` call (same CRC32C as primary). Mount sequence: read block 0 (primary superblock); verify CRC32C; if invalid: read last block (backup); if backup CRC32C valid: restore to block 0 (write primary); log `[IXFS] SELF-HEAL: superblock restored from backup`; proceed with mount. If both invalid: log `[IXFS] FATAL: both superblocks corrupt; cannot mount`; return error.

- [ ] `ixfs_flush_superblock(vol)`: write primary (block 0); compute CRC32C; write backup (last block); journal both writes in one transaction
- [ ] Mount sequence extension in `ixfs_init()`: on primary CRC fail → read backup; if backup valid → write back to block 0 + log self-heal event; if both fail → return -EIO with log
- [ ] Self-heal event counter: `vol->self_heal_count++`; persisted in superblock reserved field; shown in health dashboard (§14)
- [ ] Extend `ixfs_format()` (v3): write backup superblock at last block on initial format
- [ ] Disk Manager: health panel shows `Self-heals: %u (last: %s)` from event log
- [ ] Commit: `"ixfs: self-healing metadata -- backup superblock, CRC32C restore on mount, heal event counter"`

## 9. Automatic Scheduled Snapshots `[Sonnet]`

Configurable interval + retention policy. Prune oldest automatically. Wire to Explorer "Previous Versions" tab via shell protocol.

**Files:** `src/kernel/fs/ixfs/ixfs_snapshot_sched.c` (new), `src/desktop/prevversions.c` (new)

> [!NOTE]
> Snapshot naming: `auto_YYYYMMDD_HHMMSS` (formatted from kernel `timer_get_wallclock()`). Retention: keep last 24 hourly + 7 daily + 4 weekly. Prune algorithm: on each snapshot create, count existing `auto_*` snapshots; categorize by age bucket; if any bucket exceeds limit, `ixfs_snapshot_delete()` on oldest in that bucket. Registry: `HKLM\SYSTEM\Storage\IXFS\SnapshotInterval` (seconds, default 3600); `HKLM\SYSTEM\Storage\IXFS\SnapshotRetainHourly/Daily/Weekly`. Windows VSS comparison: VSS is a separate service (`vssvc.exe`), unreliable, documented as a "best effort"; IXFS auto-snapshots are filesystem-native and protected by the WAL journal.

- [ ] `ixfs_snapshot_sched_init(vol)`: read interval from Registry; start kernel timer; register `ixfs_snapshot_auto_tick()` callback
- [ ] `ixfs_snapshot_auto_tick()`: generate `auto_YYYYMMDD_HHMMSS` name; call `ixfs_snapshot_create(name)`; call `ixfs_snapshot_prune(vol)` to enforce retention
- [ ] `ixfs_snapshot_prune(vol)`: call `ixfs_snapshot_list()` to get all snapshots; separate `auto_*` from manual; bucket by age; delete oldest exceeding each bucket limit
- [ ] Explorer "Previous Versions" tab: on file Properties → Previous Versions tab: query `ixfs_snapshot_list()`; for each snapshot containing the file: list entry with date/time; "Restore" button → `ixfs_snapshot_restore(name)` + VFS re-read; "Open" → mount snapshot as read-only temporary drive letter
- [ ] Log: `[IXFS] Auto-snapshot created: %s`; `[IXFS] Snapshot pruned: %s (retention policy)`
- [ ] Commit: `"ixfs: auto snapshots -- scheduled create, retention 24h/7d/4w, prune, Explorer Previous Versions"`

## 10. Per-File AES-256-XTS Encryption `[Opus]`

Key table in reserved blocks. Master key via PBKDF2 (100 K iterations). Per-file random key wrapped by master key stored in `i_encrypt_key_id`. AES-256-XTS with block-number tweak. `FILE_ATTRIBUTE_ENCRYPTED`.

**Files:** `src/kernel/fs/ixfs/ixfs_encrypt.c` (new), `include/kernel/fs/ixfs.h` (extend)

> [!NOTE]
> Key slot (48 bytes): `uint8_t key_id[4]`; `uint8_t wrapped_key[32]` (per-file key AES-256-wrapped by master key); `uint8_t salt[16]` (for per-file key derivation); `uint32_t flags`. Key table in reserved inode 9 (`s_key_table_inode`). Master key: derived once at mount from user password via PBKDF2-SHA256 (100 K iterations, 32-byte salt stored in superblock); stored in `vol->master_key[32]` in RAM only -- never written to disk. AES-256-XTS tweak: block number (64-bit) in little-endian. Requires Monocypher from TODO-20-kernel-libraries. Until available: stub behind `#ifdef IXFS_ENCRYPTION_ENABLED`.

- [ ] Master key derivation: `ixfs_master_key_derive(password, salt, master_key)` → PBKDF2-SHA256; prompt for password at mount if any encrypted files exist
- [ ] Per-file key: `ixfs_file_key_create(vol, &key_id)` → random 32-byte key via `hwrng_read()`; AES-256-wrap with master key; store in key table inode; return `key_id` stored in `i_encrypt_key_id`
- [ ] `ixfs_file_key_unwrap(vol, key_id, out_key)` → read slot from key table; AES-256-unwrap with master key; store in `out_key`
- [ ] Write path: if `i_encrypt_key_id != 0`: `ixfs_file_key_unwrap()`; `aes256_xts_encrypt(buf, 4096, block_num, key)` → write ciphertext
- [ ] Read path: if `i_encrypt_key_id != 0`: read ciphertext; `aes256_xts_decrypt(buf, 4096, block_num, key)` → plaintext
- [ ] `FSCTL_SET_ENCRYPTION(hFile, 1)` → generate per-file key; set `FILE_ATTRIBUTE_ENCRYPTED`; encrypt existing blocks; journal all changes
- [ ] Encrypt directory → all new files within automatically get per-file keys
- [ ] Lock enforcement: if `vol->master_key` not set (wrong/no password): return `STATUS_ACCESS_DENIED` for any encrypted file open
- [ ] Commit: `"ixfs: per-file AES-256-XTS encryption -- PBKDF2 master key, key table, block-number tweak, FSCTL_SET_ENCRYPTION"`

## 11. USN Change Journal `[Sonnet]`

Circular 32-byte USN record buffer in the reserved journal region. 5 reason codes. `FSCTL_QUERY_USN_JOURNAL`/`FSCTL_READ_USN_JOURNAL`. Persists across reboots.

**Files:** `src/kernel/fs/ixfs/ixfs_usn.c` (new), `include/kernel/fs/ixfs.h` (extend)

> [!NOTE]
> USN record (32 bytes): `uint64_t usn` (monotonic counter); `uint32_t timestamp` (seconds since epoch); `uint32_t inode`; `uint32_t name_hash` (FNV1a of filename); `uint32_t reason` (bitmask: `USN_REASON_FILE_CREATE (0x100)`, `_DELETE (0x200)`, `_DATA_OVERWRITE (0x01)`, `_RENAME_NEW (0x2000)`, `_SECURITY_CHANGE (0x800)`); `uint32_t parent_inode`; `uint32_t reserved`. Journal region: `s_usn_start` block + `s_usn_blocks` blocks in superblock (default 256 blocks = 1 MiB, holds ~32 K records). `s_usn_current` = next USN to write. Wraps when region full (oldest overwritten).

- [ ] `ixfs_usn_record(vol, inode, reason, parent)`: compute record; write to `s_usn_start + (s_usn_current % capacity)`; increment `s_usn_current`; update superblock (batched, not per-record journal)
- [ ] Hook into VFS callbacks: `ixfs_create()` → `USN_REASON_FILE_CREATE`; `ixfs_unlink()` → `_DELETE`; `ixfs_write()` → `_DATA_OVERWRITE`; `ixfs_rename()` → `_RENAME_NEW`; `ixfs_set_security()` → `_SECURITY_CHANGE`
- [ ] `FSCTL_QUERY_USN_JOURNAL` → return `{ UsnJournalID, FirstUsn, NextUsn, MinSupportedMajorVersion=2, MaxSupportedMajorVersion=2 }`
- [ ] `FSCTL_READ_USN_JOURNAL(hVol, data, out_buf, buf_len)` → read records starting from `data.StartUsn`; fill `FILE_NOTIFY_INFORMATION`-compatible structures; return bytes read
- [ ] `FSCTL_DELETE_USN_JOURNAL` → zero the region; reset `s_usn_current = 0`
- [ ] Commit: `"ixfs: USN change journal -- circular 32B records, 5 reason codes, FSCTL_QUERY/READ_USN_JOURNAL, persists on reboot"`

## 12. Volume Quotas `[Sonnet]`

Quota table in reserved blocks. `ixfs_alloc_block` checks limit. `STATUS_DISK_FULL` on quota exceeded. `FSCTL_GET/SET_VOLUME_QUOTA`. Disk Manager quota panel.

**Files:** `src/kernel/fs/ixfs/ixfs_quota.c` (new), `src/kernel/fs/ixfs/ixfs_alloc.c` (extend)

> [!NOTE]
> Quota entry (16 bytes): `uint16_t uid`; `uint16_t gid`; `uint32_t limit_blocks` (0 = unlimited); `uint32_t used_blocks`; `uint32_t flags`. Table stored in reserved inode 11 (`s_quota_table_inode`); up to 256 entries (4 KiB block). Default: all-zero entry for uid=0 → unlimited. `ixfs_alloc_block()`: after bitmap alloc, look up `current_uid` in quota table; if `used_blocks + 1 > limit_blocks && limit != 0`: undo alloc, return `STATUS_DISK_FULL`. `ixfs_free_block()`: decrement `used_blocks`.

- [ ] `ixfs_quota_init(vol)`: load quota table from `s_quota_table_inode` into in-memory array `vol->quota[256]`
- [ ] `ixfs_quota_check(vol, uid, blocks_requested)` → 0 (allow) or `STATUS_DISK_FULL`; called from `ixfs_alloc_block()` with current task's uid
- [ ] `ixfs_quota_charge(vol, uid, blocks)` / `ixfs_quota_refund(vol, uid, blocks)` → update `used_blocks`; persist quota table block on unmount
- [ ] `FSCTL_GET_VOLUME_QUOTA(hVol, uid, &entry)` → read entry from `vol->quota[]`
- [ ] `FSCTL_SET_VOLUME_QUOTA(hVol, uid, limit_bytes)` → convert to blocks; write entry; persist; return STATUS_SUCCESS
- [ ] Disk Manager quota panel: list all users with quotas; "Add/Edit" button; "Remove" button; shows used vs. limit bar chart per user
- [ ] Commit: `"ixfs: volume quotas -- quota table, ixfs_alloc check, STATUS_DISK_FULL, FSCTL_GET/SET_VOLUME_QUOTA"`

## 13. Filesystem-Native Storage Tiering `[Opus]`

Multi-device IXFS volume with SSD fast tier + HDD capacity tier. Background promote/demote thread. `i_access_count` + `i_tier_id` in v3 inode. Disk Manager tier config and migration progress.

**Files:** `src/kernel/fs/ixfs/ixfs_tier.c` (new), `include/kernel/fs/ixfs.h` (extend superblock)

> [!NOTE]
> Tier descriptor in superblock: `s_tier_count (uint8_t)`; `s_tier_devices[4]` array of `{ blkdev_id: uint8_t, tier_type: uint8_t (0=SSD,1=HDD), start_block: uint64_t, block_count: uint64_t }`. The IXFS volume spans multiple physical devices; block allocator chooses tier based on file's `i_tier_id`. Promotion policy: `i_access_count > promote_threshold` (default 100 reads/writes) → migrate all blocks to fast tier. Demotion: `i_access_count == 0` for `demote_days` (default 7) → migrate to capacity tier. `i_access_count` decayed by 50% per midnight timer. Migration = CoW-safe block copy (identical mechanism to defrag in §4).

- [ ] Superblock: add `s_tier_count` + `s_tier_devices[4]`; update `ixfs_format_v3()` to accept tier configuration
- [ ] `ixfs_alloc_block_tiered(vol, inode)`: if `inode->i_tier_id == TIER_SSD`: allocate from SSD range; else allocate from HDD range; fall through to HDD if SSD full
- [ ] Background tier thread (SCHED_IDLE): scan inode table; check `i_access_count` and `i_tier_id`; promote hot files (`count > threshold`), demote cold files (`count == 0` for N days); use `ixfs_defrag_file()` block-copy mechanism to migrate; journal; update `i_tier_id`
- [ ] `i_access_count` increment: in `ixfs_read_block()` and `ixfs_write_block()`: `inode->i_access_count = min(65535, inode->i_access_count + 1)` (update periodically, not per-I/O)
- [ ] `i_access_count` decay: midnight kernel timer: for each mounted IXFS volume, walk inodes; `i_access_count >>= 1`
- [ ] Registry thresholds: `HKLM\SYSTEM\Storage\IXFS\TierPromoteThreshold` (default 100); `TierDemoteDays` (default 7)
- [ ] Disk Manager tier config: list tiers (SSD/HDD, size, used, hot/cold file counts); "Add Tier" (attach new blkdev); migration progress bar during background migration
- [ ] Commit: `"ixfs: storage tiering -- SSD/HDD multi-device, promote/demote background thread, Disk Manager panel"`

## 14. Volume Health Dashboard `[Sonnet]`

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

Disk Manager panel with superblock health, journal state, checksum failures, fragmentation, snapshot count, dedup ratio, encryption count, and one-click actions.

**Files:** `src/desktop/disk_manager.c` (extend), `src/kernel/fs/ixfs/ixfs_health.c` (new)

> [!NOTE]
> `ixfs_health_report_t`: `uint8_t superblock_ok`; `uint8_t backup_superblock_ok`; `uint8_t journal_clean`; `uint32_t checksum_failures`; `uint32_t self_heal_count`; `float fragmentation_pct` (avg extents per file / 2.0, capped at 100%); `uint32_t snapshot_count`; `uint64_t oldest_snapshot_age_s`; `float dedup_ratio`; `uint32_t encrypted_file_count`; `uint8_t tier_count`; `uint8_t health_score` (0=Critical, 1=NeedsAttention, 2=Healthy). Health score: Critical if `!superblock_ok`; NeedsAttention if `checksum_failures > 0 || fragmentation_pct > 50`; else Healthy.

- [ ] `ixfs_health_query(vol, report)`: populate `ixfs_health_report_t` from live vol fields + cached stats
- [ ] Disk Manager panel: top status line `● Healthy / ⚠ Needs Attention / ✖ Critical` with colour; stats grid; one-click actions: "Scrub" → `ixfs_scrub()`; "Defragment" → `ixfs_defrag_file()` all; "Create Snapshot" → `ixfs_snapshot_create(manual_name)`; "Repair" → attempt superblock restore + journal replay
- [ ] Auto-refresh: panel polls `ixfs_health_query()` every 10 s; update displayed values
- [ ] AHCI SMART integration: if `blkdev_smart_query(dev)` available (TODO-01 §5): show temperature + reallocated sector count in the panel
- [ ] Commit: `"desktop: IXFS volume health dashboard -- stats, health score, one-click scrub/defrag/snapshot/repair"`

## 15. Comprehensive Test Suite `[Sonnet]`

Extend `ixfs_test.c` to cover every feature from TODO-06 and TODO-07. Journal recovery simulation. Full `bash scripts/build.sh clean run` green.

**Files:** `src/kernel/fs/ixfs/ixfs_test.c` (extend)

> [!NOTE]
> Test coverage checklist: basic CRUD, large files > 4 GiB (64-bit `i_size`), hard links (cascade-free), symlinks (loop detection), ADS (enumerate/cascade-delete), security descriptors (DACL inherit), case-insensitive paths, sparse files (`actual_blocks < logical_size / 4096`), compression round-trips (LZ4 + Zstd), dedup (same data → same block, separate files independent after CoW), reflink (instant, independent after write), defrag (10 extents → ≤2), journal recovery (kill mid-write → remount → no corruption), checksum scrub (corrupt block → detect), USN records (reason codes correct), quotas (`STATUS_DISK_FULL` at limit), TRIM (`blkdev_discard` called on free), online resize (100 MB → 200 MB), encryption (read back decrypted, `STATUS_ACCESS_DENIED` without master key), tiering (promote/demote cycle), object ID persistence across rename, auto-snapshot create/prune, self-heal (corrupt primary SB → restore from backup).

- [ ] Add one test function per feature; log `[IXFS-TEST] <feature>: PASS/FAIL`
- [ ] Journal recovery: write partial data, simulate crash by skipping `ixfs_txn_commit()`; remount; verify no corruption; verify partial write not visible
- [ ] `ixfs_test_run_all()`: call all test functions; count failures; log summary `[IXFS-TEST] %u/%u tests passed`
- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU boot: serial log shows `[IXFS-TEST] 35/35 tests passed` (or however many tests total)
- [ ] Commit: `"test: IXFS comprehensive test suite -- all features covered, journal recovery, QEMU verified"`

---

## 16. Inode-Table + On-Disk-Layout Hardening

> **Spawned-by:** §32 (review) -- Codex adversarial round 3 of `02-kernel-core/TODO-10-kernel-security-hardening.md` §32's `ixfs_finddir` use-after-free fix; findings 5-6 (vnode-cache locking, snapshot create) added by the kernel-quality-auditor and consistency legs of that same review
> **User impact:** a crafted or corrupted IXFS volume (a malicious USB drive, a bit-flipped image, a partially-written snapshot) can make the kernel read or overwrite blocks outside the inode table, and a failed inode write during unlink can leave freed blocks still referenced by a live on-disk inode -- both are silent corruption paths reachable without any code bug beyond mounting the volume.

Six findings verified at file:line while fixing the unrelated `ixfs_finddir` UAF (`TODO-10` §32; the fifth and sixth found by the mandatory kernel-quality-auditor and consistency review passes over that fix's own diff, same root cause); none is caused by that fix and none is fixable within its scope, because each touches a different file or a pre-existing failure path that fix never introduced.

- [ ] **Mount-time superblock structural validation.** `ixfs_mount` trusts every layout field as-read
  - It checks magic/version and warns on a checksum mismatch but validates NO layout field: `s_inode_start`, `s_inode_blocks`, `s_total_inodes`, `s_data_start` and friends are trusted as-read.
  - `ixfs_ino_in_table()` (`src/kernel/fs/ixfs/ixfs_inode.c`, added by `TODO-10` §32) does 64-bit-safe arithmetic on `s_inode_blocks`/`s_total_inodes`, but it can only be as honest as those fields -- nothing rejects an inode-table extent that overlaps the journal, refcount, snapshot or data regions, or that runs past the device.
  - Reject at mount: overflow in any derived offset/size; an inode-table extent overlapping any other named region; `s_inode_start`/`s_data_start`/etc. extending past `s_total_blocks`; `s_total_inodes` exceeding what `s_inode_blocks` physically holds (the 64-bit-safe clamp in `ixfs_ino_in_table` is a runtime backstop, not a substitute for rejecting the volume up front)
- [ ] **Snapshot restore bypasses inode validation and ignores I/O failure.** `ixfs_snapshot_restore` (`src/kernel/fs/ixfs/ixfs_cow.c:377-395`)
  - Copies `se_inode_blocks` raw blocks directly from the snapshot to `s_inode_start`, ignoring both the read and write result, then reloads every cached vnode via `ixfs_read_inode` while ALSO ignoring that call's result -- and logs success regardless.
  - A corrupt snapshot's block count bypasses `ixfs_ino_in_table` entirely (it operates on raw blocks, not inode numbers), and a partial restore leaves some vnodes describing the pre-restore table against post-restore disk state.
  - Validate the full source and destination table ranges before copying a single block; check every `ixfs_read_block`/`ixfs_write_block` result; stage all vnode reloads before publishing any of them, or invalidate the whole vnode cache on any failure; return failure rather than log success over a partial copy
  - The unchecked `ixfs_read_inode` reload (`ixfs_cow.c:390`) has a sharper failure mode than "stale" as of `TODO-10` §32: that section's `ixfs_ino_in_table` now REJECTS a retired vnode-cache slot's `ino == 0` (`ixfs_unlink` sets `.ino = 0` without clearing the rest of the slot -- see `TODO-10` §32's item), so `ixfs_read_inode` returns -1 immediately and `inode.i_size` is read on the next line UNTOUCHED -- silently wrong with no `klog`, where before §32 the same call at least read back inode 0's real (harmless) on-disk record. Restoring this loop must check the return and either skip publishing that vnode's `node.size` or invalidate the slot, not merely read whatever was there before
- [ ] **`ixfs_snapshot_create` writes an unbounded, unreserved inode-table range.** `ixfs_snapshot_create` (`src/kernel/fs/ixfs/ixfs_cow.c:270-289`)
  - Allocates only ONE destination block, then copies `s_inode_blocks` blocks (a normal volume has 8) from `s_inode_start + i` to `saved_block + i` with no range validation and no write-result check.
  - Blocks `saved_block+1 .. saved_block+7` are never reserved -- they may already hold live data, or stay marked free and get allocated to something else, silently overwriting the snapshot; `ixfs_snapshot_delete` then frees this same unreserved range, compounding the corruption.
  - Reserve the COMPLETE destination range (or use explicit extents) before copying a single block; validate source and destination ranges in 64-bit; check every block I/O result; roll back the reservation on failure
- [ ] **The vnode cache (`ixfs_get_vnode` and callers) mutates shared state with no lock.** `src/kernel/fs/ixfs/ixfs_inode.c`
  - `ixfs_get_vnode` reads then writes `vol->vnode_count` and indexes `vol->vnodes[vol->vnode_count]` unsynchronized. The only concurrency comment in the tree (`ixfs_format.c:16-19`) scopes "boot-serialized, no lock" to volume-TABLE allocation at mount, not to steady-state file operations.
  - `ixfs_get_vnode`/`ixfs_finddir`/`ixfs_read_inode`/`ixfs_write_inode` ARE reachable from ordinary post-boot syscalls (`sys_read`/`sys_write`/`sys_readfile` -> VFS) on any CPU. Two threads opening different new files concurrently can collide on the same `vol->vnodes[]` slot or lose an increment of `vol->vnode_count`.
  - Serialize the vnode-cache read-modify-write (a per-volume spinlock is the natural fit, matching the pattern `src/kernel/mm/heap.c` §11 already established for `kmalloc`/`kfree`); audit whether `ixfs_unlink`'s slot-retire (`.ino = 0`) needs the same lock against a concurrent `ixfs_get_vnode` scan
  - This is IXFS's first documented SMP gap in its live (post-mount) path; the existing "boot-serialized" comment describes only the table-allocation step and should be corrected to say so explicitly once this item ships, so a future reader does not read it as covering the whole file
- [ ] **`ixfs_unlink` ignores `ixfs_write_inode`'s return.** `src/kernel/fs/ixfs/ixfs_ops.c:720`
  - Frees the inode's extents, zeroes it in memory, then calls `ixfs_write_inode(vol, target_ino, &target)` without checking the result before incrementing `s_free_inodes` and clearing the directory entry.
  - If that write fails (allocation failure, a read-block failure, or now also `ixfs_ino_in_table` rejecting `target_ino`), the on-disk inode still references the just-freed blocks while the filesystem believes them free and reusable -- a cross-link corruption path once another file claims the same block.
  - The same unchecked-return pattern recurs at every other `ixfs_write_inode` call site in `ixfs_ops.c` (`:43, 91, 125, 193, 270, 558, 623, 801, 908, 937, 960`); this item's fix should establish the pattern the others then follow, not merely patch `ixfs_unlink` alone.
  - Make unlink failure-atomic: do not publish the free-inode count or the directory-entry clear unless the zeroed inode durably wrote; roll back the extent frees on failure, ideally inside the existing journal transaction mechanism rather than as a separate patch
- [ ] **A version 1 volume's read-only flag is only partly enforced.** `ixfs_init` sets `vol->read_only` for v1 (`src/kernel/fs/ixfs/ixfs_format.c:297-300`), but only write and create check it (`ixfs_ops.c:105`, `:473`)
  - `ixfs_unlink`, truncate, mkdir, rmdir, rename and set-attribute paths mutate the volume without the check, and `ixfs_journal_recover` writes during mount regardless. Found 2026-09-28 by the documentation-site storage pages review (`docs/storage/ixfs-core.md`).
  - Fix: one `read_only` guard at the VFS-ops entry for every mutating op, and skip journal replay (or refuse the mount) on a read-only volume; test that every mutating op on a v1 volume returns an error and leaves the image byte-identical.
- [ ] Add hostile-volume, failure-injection and concurrency tests for the findings above
  - Mount rejects an inode-table extent overlapping the journal/refcount/data regions, and rejects an inflated `s_total_inodes`.
  - Snapshot restore with an injected block-write failure leaves the vnode cache and disk state consistent (not merely non-crashing); snapshot create on a fragmented volume does not corrupt live data in the unreserved range.
  - Unlink with an injected `ixfs_write_inode` failure leaves no block both marked free and still referenced; concurrent opens of two different new files do not collide on `vol->vnode_count`.
- [ ] Commit: `"ixfs: mount-time layout validation + failure-atomic snapshot restore and unlink"`

**Test checkpoint:** the six hostile-volume/failure-injection/concurrency cases above all pass; existing `ixfs` and `vfs` suites stay green; `bash scripts/test-smoke.sh` boots to `C:\>` on an ordinary, non-crafted volume (a validation gate that rejects a legitimate volume is a regression, not hardening). Scope: this section owns validation, failure-atomicity, and vnode-cache locking for the paths named above; it does not re-open `TODO-10` §32's `ixfs_finddir` fix or build new snapshot/journal features (`TODO-07` §6-§9 own those). Platforms: QEMU KVM + TCG.

## OS Comparison


| ⭐  | Feature                                                  | 🪟 Win11                                                       | 🐧 Linux                                                                    | 🚀 Impossible OS                                                             |
| --- | -------------------------------------------------------- | -------------------------------------------------------------- | --------------------------------------------------------------------------- | ---------------------------------------------------------------------------- |
| 💎  | Sparse files                                             | ✅ NTFS sparse; `FSCTL_SET_SPARSE`; `DeviceIoControl`          | ✅ All major FS; `lseek(SEEK_HOLE/DATA)`; `fallocate(FALLOC_FL_PUNCH_HOLE)` | ⬜ §1 -- hole extent type in extent                                          |
| ⭐  | Real-time journal-batched TRIM + SSD auto-detect         | ⚠️ NTFS TRIM: scheduled once/week by                           | ⚠️ `ext4` `discard` mount flag real-time,                                   | ⬜ §2 -- batched in txn commit, coalesced,                                   |
| ⭐  | Transparent per-file LZ4/Zstd compression, 10% cost gate | ⚠️ NTFS uses LZ77 (1993) per-cluster;                          | ✅ Btrfs (LZ4/Zstd/LZO); `ext4` no compression;                             | ⬜ §3 -- per-4KiB unit, LZ4+Zstd selectable per-file,                        |
| ⭐  | Inline dedup reusing CoW refcounts                       | ❌ No inline dedup in NTFS;                                    | ✅ Btrfs inline dedup (experimental, high                                   | ⬜ §4 -- xxHash64, byte-compare confirm, reuses CoW                          |
| ⭐  | Reflink instant copy via `CopyFileW` same-volume hook    | ⚠️ NTFS block cloning (`FSCTL_DUPLICATE_EXTENTS_TO_FILE`) only | ✅ Btrfs/XFS reflink via `FICLONE` ioctl;                                   | ⬜ §5 -- O(1) `ixfs_reflink()` hooked transparently into                     |
| ⭐  | Online in-filesystem defrag                              | ✅ NTFS `Defragment` via `defrag.exe` +                        | ✅ `e4defrag`; `btrfs balance`; not in-FS                                   | ⬜ §6 -- `ixfs_defrag_file()`, IDLE background thread, CoW-refcount-aware    |
| ⭐  | Online volume grow                                       | ✅ NTFS online grow via Disk                                   | ✅ `resize2fs -f` (online for ext4);                                        | ⬜ §7 -- `ixfs_grow()`, bitmap extension, journal-protected superblock       |
| ⭐  | Self-healing                                             | ❌ NTFS no self-heal; requires `chkdsk                         | ⚠️ `ext4` has journal replay but                                            | ⬜ §8 -- backup SB at last block,                                            |
| ⭐  | Auto snapshots                                           | ⚠️ VSS: separate `vssvc.exe` service, unreliable,              | ⚠️ Btrfs: `snapper` daemon (user-space script);                             | ⬜ §9 -- in-kernel schedule, 24h/7d/4w retention, Explorer                   |
| ⭐  | Per-file AES-256-XTS encryption with PBKDF2              | ⚠️ NTFS EFS: certificate-based, breaks on                      | ⚠️ `ext4` fscrypt: requires CLI key                                         | ⬜ §10 -- `ixfs_encrypt.c`, PBKDF2 master key, per-block                     |
| ⭐  | USN journal persisted across reboots                     | ✅ NTFS `$UsnJrnl` in `$Extend`; full                          | ❌ `inotify`/`fanotify` are in-memory only; lost                            | ⬜ §11 -- reserved-region circular buffer, 32B records,                      |
| 💎  | Volume quotas                                            | ✅ NTFS quotas via `fsutil quota`;                             | ✅ `ext4`/`xfs` quotas; `quota`/`repquota` tools; per-user+group            | ⬜ §12 -- `ixfs_quota.c`, in-alloc check, `FSCTL_GET/SET_VOLUME_QUOTA`, Disk |
| ⭐  | Filesystem-native storage tiering (SSD/HDD)              | ❌ Windows Storage Spaces (separate volume                     | ❌ bcache/dm-cache (block-level, not filesystem-aware); no                  | ⬜ §13 -- multi-device superblock, access-count promote/demote, CoW-safe     |
| ⭐  | Volume health dashboard                                  | ⚠️ Disk Management: basic; `fsutil volume`                     | ⚠️ `smartmontools`; `btrfs scrub status`; no                                | ⬜ §14 -- Disk Manager panel, health score,                                  |

> **After §1–15:** IXFS is the only general-purpose filesystem with all of: transparent per-file compression (LZ4+Zstd), inline dedup reusing CoW refs, reflinks exposed as standard `CopyFileW`, per-file AES-256-XTS without certificate infrastructure, filesystem-native storage tiering, a persistent USN journal, and self-healing metadata -- all in one volume, all journaled, all CoW-safe, all accessible via the standard Win32 API surface. The nearest competitor for feature breadth is ZFS, which requires 300+ MiB RAM overhead and a pool layer; IXFS achieves comparable features at a fraction of the RAM cost by reusing the CoW refcount table for both snapshots, dedup, and reflinks rather than maintaining three separate data structures.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Sparse: write 10 MB file; punch 5 MB hole; `QUERY_ALLOCATED_RANGES` → 2 ranges; `actual_blocks * 4096 < i_size`; read hole → all zeros; snapshot of file → original block refcounts > 1 after punch
- [ ] TRIM: `blkdev_discard()` call count > 0 after file delete on SSD-detected volume; HDD volume: no `blkdev_discard()` calls
- [ ] Compression: write incompressible data → stored uncompressed (IXFS_COMP_STORED); write text data → stored compressed; read back identical; `FSCTL_GET_COMPRESSION` returns set format
- [ ] Dedup: write same 4 KiB block to two files; verify both extents point to same block; verify refcount == 2; modify one → CoW → independent
- [ ] Reflink: `CopyFileW("C:\\big.bin", "C:\\big_copy.bin")` on same IXFS volume → completes in < 10 ms for 1 GiB file; write 1 byte to copy → only that block diverges; delete copy → original intact
- [ ] Defrag: create 10 MB file with 10+ extents (alternating allocations); `FSCTL_DEFRAGMENT_FILE` → extent count ≤ 2; data unchanged
- [ ] Online resize: `ixfs_grow(100MB → 200MB)`; write file to 150 MB offset → succeeds; `s_total_blocks` correct
- [ ] Self-healing: corrupt first 512 bytes of block 0 (primary SB); mount → `[IXFS] SELF-HEAL: superblock restored from backup`; volume mounts and is writable
- [ ] Auto-snapshot: set interval to 5 s; wait 10 s → 2 snapshots created; prune test: create 25 hourly → only 24 kept; Explorer Previous Versions tab lists snapshots
- [ ] Encryption: `FSCTL_SET_ENCRYPTION` on file; read back decrypts correctly; remount with wrong password → `STATUS_ACCESS_DENIED` on encrypted file open
- [ ] USN journal: create/write/delete file → 3 records with correct reason codes; `FSCTL_READ_USN_JOURNAL` returns them; remount → records persist
- [ ] Quotas: set limit 10 MB for uid 1; write 11 MB → `STATUS_DISK_FULL` (Win32 `ERROR_DISK_FULL`); free 2 MB → write 1 MB succeeds
- [ ] Tiering: promote test: set `i_access_count = 200` (above threshold); tier thread fires → block migrated to SSD tier; `i_tier_id` updated
- [ ] Health dashboard: shows all stats; "Scrub" button runs `ixfs_scrub()`; "Create Snapshot" button works; health score changes to "Needs Attention" after injected checksum failure
- [ ] `ixfs_test.c` suite: `[IXFS-TEST] N/N tests passed` in QEMU serial log; N ≥ 35
- [ ] Commit: `"ixfs: advanced enterprise features -- compression, dedup, reflinks, encrypt, tiering, USN, quotas, health"`
